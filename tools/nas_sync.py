#!/usr/bin/env python3
"""Copy derived products to a NAS, and delete local ones the NAS has.

    python3 nas_sync.py ~/ionograms --dest /mnt/nas/ionozond
    python3 nas_sync.py ~/ionograms --dest samsung@nas:/volume1/ionozond
    python3 nas_sync.py ~/ionograms --dest ... --keep-h5-days 14 --apply

Why one tool and not two
-----------------------
Because they are one decision. A local .h5 may be deleted exactly when the
NAS has a byte-identical copy of it, so an uploader that does not know what
was deleted and a pruner that does not know what was uploaded can only ever
agree by accident. Here the prune set is derived from the verification, and
nothing is deleted that was not just proven present.

This is not a port of chirpsounder2's scripts, which do different jobs:
`sync_iono_data.py` HTTP-POSTs dashboard PNGs to a web server, and
`iono_housekeeping.py` deletes digital_rf ringbuffer scratch off a RAM disk.
Neither moves ionograms anywhere.

What travels, and what does not
-------------------------------
  .h5   always. The archive is the point: 1.25 MB, reprocessable, and the
        format ionograms-handler reads.
  .lfp  always. 70 kB, and it is what any console pointed at the NAS copy
        would need in order to display anything.
  .lfs  only with --include-lfs. 80 MB each; on most links this saturates
        the upload for longer than the repetition period, which is a good
        way to find out that the sounder and the sync are sharing a disk.

Only .h5 is ever deleted locally. The .lfp is 20 MB a day, the console reads
it, and nothing is gained by making the display depend on the network.

Credentials
-----------
None are handled here, and none should be. For an SSH destination use a key
(`ssh-keygen`, then `ssh-copy-id`); for a CIFS mount put the credentials in a
root-owned file at mode 600 and reference it from /etc/fstab. A password on
this command line would be visible in `ps` to every user on the host.
"""

import argparse
import os
import shutil
import subprocess
import sys
import time

# Products, in the order they are worth having if a transfer is cut short.
# The archive first: it is the one that cannot be rebuilt from anything else
# still on the NAS.
PRODUCTS = ("*.h5", "*.lfp")
RAW = "*.lfs"


def read_console_config(path):
    """Top-level literal assignments from the console's chirp_config.py.

    A cut-down copy of rx_dechirp.load_config, and deliberately a copy: that
    lives in a 2000-line script which this tool has no other reason to import,
    and importing it under systemd to read two strings would be the tail
    wagging the dog. Parsed, never executed -- the file is generated, but it
    is still a file on disk that this runs against.
    """
    import ast

    cfg = {}
    try:
        with open(path, "r", encoding="utf-8") as fh:
            tree = ast.parse(fh.read(), filename=path)
    except (IOError, OSError, SyntaxError):
        return cfg
    for node in tree.body:
        if not isinstance(node, ast.Assign) or len(node.targets) != 1:
            continue
        target = node.targets[0]
        if not isinstance(target, ast.Name):
            continue
        try:
            cfg[target.id] = ast.literal_eval(node.value)
        except (ValueError, SyntaxError):
            pass
    return cfg


def is_remote(dest):
    """Does this destination go through SSH?

    rsync's own rule: a colon before any slash. `/mnt/nas/x` is local,
    `nas:/vol/x` is not, and `C:/x` would be wrong on Windows and is not a
    case that arises on the station.
    """
    head = dest.split("/", 1)[0]
    return ":" in head


def mount_entry_for(path):
    """(mount point, fstype) of the filesystem holding `path`, from
    /proc/self/mountinfo. (None, None) where that does not exist.

    The longest mount point that prefixes the path wins, which is how the
    kernel resolves it too. Fields before the " - " separator are variable in
    number, so the separator is found rather than counted.
    """
    try:
        with open("/proc/self/mountinfo", "r") as fh:
            lines = fh.readlines()
    except (IOError, OSError):
        return None, None

    try:
        target = os.path.realpath(path)
    except OSError:
        target = path

    best = (None, None)
    best_len = -1
    for line in lines:
        try:
            head, tail = line.split(" - ", 1)
            point = head.split()[4].replace("\\040", " ")
            fstype = tail.split()[0]
        except (IndexError, ValueError):
            continue
        if target == point or target.startswith(point.rstrip("/") + "/"):
            if len(point) > best_len:
                best, best_len = (point, fstype), len(point)
    return best


def local_dest_is_safe(dest):
    """Is a local destination actually a mounted share? (ok, why)

    An unmounted mount point is indistinguishable from an empty directory by
    every means that does not consult the mount table, so this is the one
    check that matters for a NAS path: rsync writing a day of archives onto
    the station's own root filesystem, under /mnt, is silent until the disk
    is full and the sounder stops.

    Two tests, because either alone has a false negative. st_dev differing
    from the root filesystem's proves something is mounted there. But a dead
    CIFS mount also passes that -- the mount entry outlives the SMB session,
    which is exactly how `mountpoint -q` reports a corpse as healthy -- so
    the directory is also listed, under a timeout, since I/O on a stale hard
    mount blocks rather than failing.
    """
    path = dest.rstrip("/") or "/"
    probe = path if os.path.isdir(path) else (os.path.dirname(path) or "/")
    if not os.path.isdir(probe):
        return False, "%s does not exist" % probe

    point, fstype = mount_entry_for(probe)
    if fstype == "autofs":
        # An automount trigger that has not fired, or whose mount failed.
        # It answers stat() and can list empty without error, so neither the
        # st_dev test nor the listing below would catch it -- met on the
        # station, where four CIFS shares were autofs triggers standing in
        # front of mounts that had never once succeeded.
        return False, ("%s is an autofs trigger with nothing mounted behind "
                       "it. The share failed to mount: check `journalctl -u "
                       "$(systemd-escape -p --suffix=mount %s)` and "
                       "`dmesg | grep -i cifs`." % (point, point))
    if fstype is None:
        # Not Linux, or no mountinfo. Fall back to the device comparison.
        try:
            if os.stat(probe).st_dev == os.stat("/").st_dev:
                return False, ("%s is on the root filesystem. If it is a "
                               "mount point, the share is not mounted." % probe)
        except OSError as exc:
            return False, "cannot stat %s: %s" % (probe, exc)
    elif point == "/":
        return False, ("%s is on the root filesystem. If it is a mount point, "
                       "the share is not mounted." % probe)

    # Listing it proves the session behind the mount is alive. A stale CIFS
    # mount answers stat() from cache and fails or hangs on this.
    try:
        with _time_limit(10):
            os.listdir(probe)
    except Exception as exc:
        return False, ("%s is mounted but not readable (%s). A stale CIFS "
                       "mount looks fine to stat and fails on use: "
                       "sudo umount -lf %s, then remount." % (probe, exc, probe))
    return True, ""


class _time_limit(object):
    """SIGALRM around a block, so a hung mount cannot hang the sync.

    A no-op where SIGALRM does not exist (Windows), which is fine: the check
    it guards only ever runs against a local path on the station.
    """

    def __init__(self, seconds):
        self.seconds = seconds
        self.previous = None

    def __enter__(self):
        try:
            import signal
            self.signal = signal
            self.previous = signal.signal(signal.SIGALRM, self._fire)
            signal.alarm(self.seconds)
        except (ImportError, AttributeError, ValueError):
            self.previous = None
        return self

    def _fire(self, signum, frame):
        raise TimeoutError("timed out after %d s" % self.seconds)

    def __exit__(self, *exc):
        if self.previous is not None:
            self.signal.alarm(0)
            self.signal.signal(self.signal.SIGALRM, self.previous)
        return False


def day_dirs(root):
    """The archive's YYYY.MM.DD directories, oldest first."""
    out = []
    for name in sorted(os.listdir(root)):
        path = os.path.join(root, name)
        if os.path.isdir(path) and len(name) == 10 and name.count(".") == 2:
            out.append((name, path))
    return out


def ssh_transport(key):
    """rsync's -e argument for an SSH destination, or None for a local path.

    Borrowed from chirpsounder2's backup scripts, which pass
    `-e "ssh -i ~/.ssh/id_rsa"` -- naming the key matters under systemd, where
    there is no agent and no login shell to have loaded one.

    BatchMode=yes is ours, and is the part that makes this survivable
    unattended. Without it ssh *prompts*: for a passphrase, or to accept an
    unknown host key. A prompt with no terminal attached does not fail, it
    blocks -- and a timer whose last run never exited is a sync that silently
    stops for ever. With BatchMode it exits non-zero at once and the journal
    says why; the fix is then to ssh in by hand once and accept the host key.
    """
    parts = ["ssh", "-o", "BatchMode=yes"]
    if key:
        parts += ["-i", os.path.expanduser(key)]
    return " ".join(parts)


def run(cmd, log, dry=False):
    if dry:
        log("    would run: %s" % " ".join(cmd))
        return 0, ""
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT)
    except FileNotFoundError as exc:
        return 127, str(exc)
    return proc.returncode, proc.stdout.decode("utf-8", "replace")


def upload(root, dest, day, include_lfs, bwlimit, ssh, log):
    """Push one day's products. Returns rsync's exit code.

    -a without -H/-A/-X: the NAS may be a CIFS share that cannot hold owners,
    ACLs or hard links, and rsync fails the whole transfer rather than the
    attribute. Times and the recursive walk are all this needs.
    """
    src = os.path.join(root, day) + "/"
    # --timeout matters unattended: without it an SSH session that stops
    # answering blocks every later run of the timer, and the only symptom is
    # a sync that quietly never happens again.
    cmd = ["rsync", "-rt", "--partial", "--stats", "--human-readable",
           "--timeout=120"]
    if ssh:
        cmd += ["-e", ssh]
    if bwlimit:
        cmd += ["--bwlimit=%d" % bwlimit]
    for pattern in PRODUCTS:
        cmd += ["--include", pattern]
    if include_lfs:
        cmd += ["--include", RAW]
    # Everything not named above is excluded, so a stray .partial, a core
    # dump or a half-written file never reaches the NAS.
    cmd += ["--exclude", "*"]
    cmd += [src, "%s/%s/" % (dest.rstrip("/"), day)]

    rc, out = run(cmd, log)
    if rc != 0:
        log("  *** rsync failed for %s (exit %d)" % (day, rc))
        for line in out.strip().splitlines()[-6:]:
            log("      %s" % line)
    return rc, out


def parse_itemized(out):
    """Basenames rsync said it would transfer, from `-i` output.

    Separate from confirmed_remote so it can be tested without rsync. Every
    itemised line is an 11-character flag field, a space, then the path; the
    second character is the entry type, so `f` selects regular files and
    drops the `cd+++++++++ ./` directory lines and the `sent 1,234 bytes`
    summary alike.

    Getting this backwards would delete files the NAS does not have, so it is
    written to fail safe: anything unparseable is simply not treated as
    confirmed.
    """
    names = set()
    for line in out.splitlines():
        parts = line.split(None, 1)
        if len(parts) == 2 and len(parts[0]) == 11 and parts[0][1] == "f":
            names.add(os.path.basename(parts[1].strip()))
    return names


def confirmed_remote(paths, dest, day, ssh, log):
    """Which of these local files the NAS already holds, byte for byte.

    `rsync -n -c` compares by checksum rather than by size and mtime. That
    distinction is the whole reason this function exists: size and mtime
    agree for a file that was truncated at exactly a block boundary, or
    written by a different run, and this is the check standing between a
    verified copy and deleting the only one.

    rsync itemises only what it *would* transfer, so a file absent from the
    output is one the NAS already matches.
    """
    if not paths:
        return set()
    cmd = ["rsync", "-nci", "--checksum", "--timeout=120"]
    if ssh:
        cmd += ["-e", ssh]
    cmd += list(paths) + ["%s/%s/" % (dest.rstrip("/"), day)]
    rc, out = run(cmd, log)
    if rc != 0:
        log("  *** could not verify %s against the NAS (exit %d); nothing "
            "will be deleted for that day" % (day, rc))
        return set()

    would_transfer = parse_itemized(out)
    return set(p for p in paths
               if os.path.basename(p) not in would_transfer)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("archive", help="the ionograms directory")
    ap.add_argument("--dest", default="",
                    help="rsync destination: a mounted path, or user@host:/path. "
                         "Normally left unset -- it comes from nas_dest in the "
                         "console's parameters dialog. $NAS_DEST is the "
                         "fallback for a station with no console.")
    ap.add_argument("--config",
                    default=os.environ.get("IONOZOND_CONFIG",
                                           os.path.expanduser("~/chirp_config.py")),
                    help="the console's chirp_config.py, which carries "
                         "nas_dest and nas_keep_h5_days")
    ap.add_argument("--ssh-key", default=os.environ.get("NAS_SSH_KEY", ""),
                    help="private key for an SSH destination. Defaults to "
                         "$NAS_SSH_KEY, and to ssh's own search when unset. "
                         "Name it for unattended runs: systemd has no agent.")
    ap.add_argument("--include-lfs", action="store_true",
                    help="also upload the 80 MB raw captures")
    ap.add_argument("--keep-h5-days", type=float, default=None,
                    help="delete local .h5 older than this once the NAS has "
                         "them, verified by checksum. Negative uploads and "
                         "deletes nothing. Unset takes nas_keep_h5_days from "
                         "the dialog, then $NAS_KEEP_H5_DAYS, then -1.")
    ap.add_argument("--bwlimit", type=int, default=0,
                    help="KB/s ceiling for the transfer. Worth setting: the "
                         "sounder and rsync share one disk and one NIC.")
    ap.add_argument("--days", type=int, default=0,
                    help="only sync the most recent N day directories "
                         "(default: all of them)")
    ap.add_argument("--allow-local-disk", action="store_true",
                    help="write to a local destination that is not a mounted "
                         "share. Only for testing: on the station this means "
                         "filling the disk the sounder is recording to.")
    ap.add_argument("--apply", action="store_true",
                    help="actually delete. Uploading always happens; only "
                         "the irreversible half needs this.")
    opts = ap.parse_args()

    def log(msg):
        sys.stdout.write(msg + "\n")
        sys.stdout.flush()

    root = os.path.expanduser(opts.archive)
    if not os.path.isdir(root):
        log("%s is not a directory" % root)
        return 2

    # Where the settings come from, most specific first. The dialog is the
    # normal place: it is where the operator already sets everything else
    # about this station, and it needs no root. The environment is the
    # fallback for a host with no console, and the flags override both for a
    # run by hand.
    console = read_console_config(os.path.expanduser(opts.config))
    if not opts.dest:
        opts.dest = (console.get("nas_dest")
                     or os.environ.get("NAS_DEST", "") or "")
    opts.dest = os.path.expanduser(str(opts.dest).strip())
    if opts.keep_h5_days is None:
        fallback = console.get("nas_keep_h5_days",
                               os.environ.get("NAS_KEEP_H5_DAYS", -1.0))
        try:
            opts.keep_h5_days = float(fallback)
        except (TypeError, ValueError):
            opts.keep_h5_days = -1.0

    if not opts.dest:
        # Not an error. A station with no NAS is a perfectly good station,
        # and an hourly timer that fails on one is how an operator learns to
        # ignore its failures.
        log("No NAS destination configured -- nothing to do.")
        log("Set it in the console's parameters dialog (Сохранение данных),")
        log("or pass --dest.")
        return 0
    if shutil.which("rsync") is None:
        log("rsync is missing:  sudo apt-get install -y rsync")
        return 2

    days = day_dirs(root)
    if opts.days > 0:
        days = days[-opts.days:]

    log("=" * 68)
    log(" syncing %s -> %s" % (root, opts.dest))
    log("=" * 68)
    ssh = ssh_transport(opts.ssh_key) if is_remote(opts.dest) else None
    log("  transport  %s" % (ssh if ssh else "local path"))
    if ssh and opts.ssh_key and not os.path.exists(os.path.expanduser(opts.ssh_key)):
        log("  *** %s does not exist" % opts.ssh_key)
        return 2
    log("  products   %s" % ", ".join(PRODUCTS +
                                      ((RAW,) if opts.include_lfs else ())))
    log("  days       %d" % len(days))
    if opts.bwlimit:
        log("  bandwidth  %d KB/s" % opts.bwlimit)
    if opts.keep_h5_days >= 0:
        log("  local .h5  deleted past %.1f days, once verified on the NAS"
            % opts.keep_h5_days)
    log("")

    # Checked before the first byte moves: writing a day of archives onto the
    # station's own root disk, under an unmounted /mnt/..., is silent until
    # the disk is full and the sounder stops for want of space.
    if not is_remote(opts.dest):
        ok, why = local_dest_is_safe(opts.dest)
        if not ok:
            log("  *** %s" % why)
            if not opts.allow_local_disk:
                log("  *** Nothing was uploaded. Pass --allow-local-disk if "
                    "the destination really is meant to be on this machine.")
                return 2
            log("  *** --allow-local-disk given; continuing anyway.")

    failed = 0
    uploaded = 0
    for name, _path in days:
        rc, out = upload(root, opts.dest, name, opts.include_lfs,
                         opts.bwlimit, ssh, log)
        if rc != 0:
            failed += 1
            continue
        uploaded += 1
        for line in out.splitlines():
            if line.startswith("Number of regular files transferred"):
                n = line.split(":")[-1].strip()
                if n not in ("0", ""):
                    log("  %s  %s file(s)" % (name, n))

    if failed:
        log("")
        log("  *** %d of %d day(s) failed to upload. Nothing will be deleted."
            % (failed, len(days)))
        return 1

    if opts.keep_h5_days < 0:
        log("")
        log("  uploaded %d day(s). No local deletion was asked for." % uploaded)
        return 0

    # ---- delete what the NAS has ------------------------------------------
    cutoff = time.time() - opts.keep_h5_days * 86400.0
    removed = 0
    freed = 0
    unverified = 0
    log("")
    for name, path in days:
        candidates = []
        for fname in sorted(os.listdir(path)):
            if not fname.endswith(".h5"):
                continue
            full = os.path.join(path, fname)
            try:
                if os.path.getmtime(full) > cutoff:
                    continue
            except OSError:
                continue
            candidates.append(full)
        if not candidates:
            continue

        ok = confirmed_remote(candidates, opts.dest, name, ssh, log)
        unverified += len(candidates) - len(ok)
        day_removed = 0
        day_freed = 0
        for full in sorted(ok):
            size = os.path.getsize(full)      # asked for before, not after
            if opts.apply:
                try:
                    os.remove(full)
                except OSError as exc:
                    log("  *** %s: %s" % (os.path.basename(full), exc))
                    continue
            day_removed += 1
            day_freed += size
        removed += day_removed
        freed += day_freed
        if day_removed:
            log("  %s  %d archive(s) %s, %.1f MB"
                % (name, day_removed,
                   "deleted" if opts.apply else "would delete",
                   day_freed / 1e6))

    log("")
    log("  %s %d archive(s), %.2f GB"
        % ("deleted" if opts.apply else "would delete", removed, freed / 1e9))
    if unverified:
        log("  %d were past the window but not verified on the NAS, and stay."
            % unverified)
    if not opts.apply and removed:
        log("")
        log("  Nothing was deleted. Re-run with --apply.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
