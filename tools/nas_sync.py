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


def is_remote(dest):
    """Does this destination go through SSH?

    rsync's own rule: a colon before any slash. `/mnt/nas/x` is local,
    `nas:/vol/x` is not, and `C:/x` would be wrong on Windows and is not a
    case that arises on the station.
    """
    head = dest.split("/", 1)[0]
    return ":" in head


def day_dirs(root):
    """The archive's YYYY.MM.DD directories, oldest first."""
    out = []
    for name in sorted(os.listdir(root)):
        path = os.path.join(root, name)
        if os.path.isdir(path) and len(name) == 10 and name.count(".") == 2:
            out.append((name, path))
    return out


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


def upload(root, dest, day, include_lfs, bwlimit, log):
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


def confirmed_remote(paths, dest, day, log):
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
    cmd = ["rsync", "-nci", "--checksum", "--timeout=120"] + list(paths) \
        + ["%s/%s/" % (dest.rstrip("/"), day)]
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
    ap.add_argument("--dest", default=os.environ.get("NAS_DEST", ""),
                    help="rsync destination: a mounted path, or user@host:/path. "
                         "Defaults to $NAS_DEST.")
    ap.add_argument("--include-lfs", action="store_true",
                    help="also upload the 80 MB raw captures")
    ap.add_argument("--keep-h5-days", type=float, default=-1.0,
                    help="delete local .h5 older than this once the NAS has "
                         "them, verified by checksum. Negative (the default) "
                         "uploads and deletes nothing.")
    ap.add_argument("--bwlimit", type=int, default=0,
                    help="KB/s ceiling for the transfer. Worth setting: the "
                         "sounder and rsync share one disk and one NIC.")
    ap.add_argument("--days", type=int, default=0,
                    help="only sync the most recent N day directories "
                         "(default: all of them)")
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
    if not opts.dest:
        log("no destination: pass --dest or set NAS_DEST")
        return 2
    if shutil.which("rsync") is None:
        log("rsync is missing:  sudo apt-get install -y rsync")
        return 2

    days = day_dirs(root)
    if opts.days > 0:
        days = days[-opts.days:]

    log("=" * 68)
    log(" syncing %s -> %s" % (root, opts.dest))
    log("=" * 68)
    log("  transport  %s" % ("ssh" if is_remote(opts.dest) else "local path"))
    log("  products   %s" % ", ".join(PRODUCTS +
                                      ((RAW,) if opts.include_lfs else ())))
    log("  days       %d" % len(days))
    if opts.bwlimit:
        log("  bandwidth  %d KB/s" % opts.bwlimit)
    if opts.keep_h5_days >= 0:
        log("  local .h5  deleted past %.1f days, once verified on the NAS"
            % opts.keep_h5_days)
    log("")

    # A destination that is a mount point but is not mounted looks exactly
    # like an empty directory, and rsync will happily fill the local disk
    # under it. Checked before the first byte moves.
    if not is_remote(opts.dest):
        parent = os.path.dirname(opts.dest.rstrip("/")) or "/"
        if not os.path.isdir(parent):
            log("  *** %s does not exist. If it is a mount point, the share "
                "is not mounted." % parent)
            return 2

    failed = 0
    uploaded = 0
    for name, _path in days:
        rc, out = upload(root, opts.dest, name, opts.include_lfs,
                         opts.bwlimit, log)
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

        ok = confirmed_remote(candidates, opts.dest, name, log)
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
