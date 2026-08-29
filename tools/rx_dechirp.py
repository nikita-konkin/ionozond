#!/usr/bin/env python3
"""Dechirp a sounding and write it as .lfs -- the core of chirpsounder1.

    python3 rx_dechirp.py --self-test              # no radio needed
    python3 rx_dechirp.py --from-raw cap.c64 --t0 ...
    sudo python3 rx_dechirp.py --config /tmp/out/chirp_config.py

This is a port of gr-juha's juha::chirp_downconvert onto plain Python 3,
numpy and python3-uhd -- no GNU Radio, no out-of-tree C++ module. The
original's implementation body was not in the recovered backup, so the
algorithm here is reconstructed from the block's declarations, from
chirp.py's use of it, and from the geometry the console independently
verifies.

The algorithm
-------------
The receiver tunes to `cf` and samples at `sr`, so baseband spans
cf +- sr/2. chirp.py calls set_chirp_par(-sr/2, rate): the replica starts at
the bottom of that span and sweeps upward at `rate` Hz/s. Over `dur` seconds
it covers dur*rate Hz, which for the standard schedule equals sr exactly --
250 s x 100 kHz/s = 25 MHz -- so the sweep is 7.5 to 32.5 MHz for cf = 20 MHz.

Dechirping multiplies by the conjugate replica:

    y[n] = x[n] * conj(exp(2j*pi*(f0*t + rate*t^2/2))),   t = n/sr

An echo delayed by tau then sits at a constant beat frequency of -rate*tau.
Negative, because a late arrival is below the replica -- which is why the
console reverses the spectrum so its rows ascend with delay.

Decimation is a boxcar mean of `dec` samples, not a filter: the original
block's add_and_advance_phasor(a, mean) accumulates and averages. Cheap, and
cheapness is the point -- this has to keep up with 25 MS/s on a laptop.

Phase is generated from a lookup table, as the original did with its sintab,
because a complex exponential over 25e6 points per second is far too slow.
"""

import argparse
import calendar
import math
import os
import shutil
import struct
import sys
import time

try:
    import numpy as np
except ImportError:
    sys.stderr.write("numpy is missing:  sudo apt-get install -y python3-numpy\n")
    sys.exit(2)


# ---------------------------------------------------------------- .lfs header

# Byte-exact, from src/lfs_header.h, itself verified field by field against
# real captures. The C++ writer in gr-juha is the only producer that ever
# reached disk, so 1.0 / 498 is the only variant in the wild -- the Python
# lfs_header.py in the backup declares 1.1 / 512 but never packs anything.
LFS_STRUCT = "<4sf4sH64sff64sff7HIIIHIIiiIIHII292s"
LFS_FORMAT_VER = 1.0
LFS_HEADER_SIZE = 498         # what the field says; the header occupies 512
LFS_TOTAL = 512

DAYS_FROM_MONTH_START = [0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334]

# The reserved field is not blank. Every capture in the archive carries eight
# ASCII '0' characters followed by NULs -- checked across captures from several
# days. Reproduced so files from this writer are byte-identical to the ones the
# original produced.
RESERVED = b"0" * 8 + b"\0" * 284


def day_number(year, month, day):
    """Day of the year.

    The original adds a leap day whenever the year is a leap year, including
    for January and February, which is wrong for those two months. Nothing
    reads the field -- the console parses it and never uses it -- so this
    computes it correctly rather than reproducing the error.
    """
    leap = (year % 4 == 0 and year % 100 != 0) or (year % 400 == 0)
    n = DAYS_FROM_MONTH_START[month - 1] + day
    if leap and month > 2:
        n += 1
    return n


def pack_lfs_header(tx_name, tx_lat, tx_lon, rx_name, rx_lat, rx_lon,
                    start_epoch, chirpt, cf, dur, rate, rep, rmin, rmax,
                    dec, sample_rate, whiten, whiten_len, whiten_n):
    t = time.gmtime(start_epoch)
    raw = struct.pack(
        LFS_STRUCT,
        b"LFSG", LFS_FORMAT_VER, b"fmt ", LFS_HEADER_SIZE,
        tx_name.encode("latin-1")[:64], float(tx_lat), float(tx_lon),
        rx_name.encode("latin-1")[:64], float(rx_lat), float(rx_lon),
        t.tm_year, day_number(t.tm_year, t.tm_mon, t.tm_mday),
        t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec,
        int(start_epoch), int(chirpt), int(cf), int(dur), int(rate), int(rep),
        int(rmin), int(rmax), int(dec), int(sample_rate),
        1 if whiten else 0, int(whiten_len), int(whiten_n), RESERVED)
    assert len(raw) == LFS_TOTAL, len(raw)
    return raw


# ---------------------------------------------------------------- the dechirp

class Dechirper:
    """Streaming dechirp and boxcar decimation.

    Feed it chunks in order; it returns decimated output and carries the
    remainder across chunk boundaries.
    """

    # 8192, as the original's "#define TABL 8192". Phase quantisation of one
    # part in 8192 puts its spurs around -78 dBc, far below any HF noise floor,
    # and a 64 kB table stays in cache where a larger one would not -- which
    # matters more here, since this loop is memory-bound.
    TABLE_SIZE = 8192

    def __init__(self, sr, f0, rate, dec):
        self.sr = float(sr)
        self.f0 = float(f0)
        self.rate = float(rate)
        self.dec = int(dec)
        self.n = 0                       # absolute sample index since t0
        self.carry = np.empty(0, dtype=np.complex64)

        # Conjugate replica, one turn. Looking this up beats evaluating a
        # complex exponential 25 million times a second by a wide margin.
        k = np.arange(self.TABLE_SIZE, dtype=np.float64)
        self.table = np.exp(-2j * np.pi * k / self.TABLE_SIZE).astype(np.complex64)

        self._cache_len = 0
        self._dt = None
        self._dt2 = None

    def _local(self, length):
        """Per-chunk constants and scratch, rebuilt only when the length changes.

        The table scaling is folded into these arrays rather than applied to
        the phase afterwards: a separate multiply over a million float64s is a
        whole pass through memory for nothing.
        """
        if length != self._cache_len:
            m = np.arange(length, dtype=np.float64)
            dt = m / self.sr
            self._dt = dt * self.TABLE_SIZE
            self._dt2 = (0.5 * self.rate * dt * dt) * self.TABLE_SIZE
            self._work = np.empty(length, dtype=np.float64)
            self._idx = np.empty(length, dtype=np.int64)
            self._gain = np.empty(length, dtype=np.complex64)
            self._cache_len = length
        return self._dt, self._dt2

    def phase_turns(self, length):
        """Replica phase for `length` samples from here, in table units.

        Split as a block constant plus local terms so the arithmetic stays in
        the thousands rather than the billions. Over a 250 s sounding the
        absolute phase reaches ~3e9 turns; this keeps six more digits of margin
        and is the same recurrence the C++ block used.
        """
        dt, dt2 = self._local(length)
        t0 = self.n / self.sr
        c0 = self.f0 * t0 + 0.5 * self.rate * t0 * t0
        # dt already carries both the 1/sr and the table scaling, so c1 stays
        # in Hz -- the instantaneous frequency at the start of this block.
        c1 = self.f0 + self.rate * t0
        work = self._work
        np.multiply(dt, c1, out=work)
        np.add(work, dt2, out=work)
        work += (c0 - math.floor(c0)) * self.TABLE_SIZE
        return work

    def feed(self, chunk):
        """Dechirp and decimate one chunk. Returns whatever completed.

        Everything is written into buffers that persist between calls. At
        25 MS/s each temporary is eight megabytes, and allocating three of them
        per block costs more than the arithmetic does.
        """
        length = len(chunk)
        if length == 0:
            return np.empty(0, dtype=np.complex64)

        units = self.phase_turns(length)
        np.copyto(self._idx, units, casting="unsafe")   # truncate, no new array
        self._idx &= self.TABLE_SIZE - 1
        np.take(self.table, self._idx, out=self._gain)
        np.multiply(chunk, self._gain, out=self._gain)
        self.n += length

        mixed = self._gain
        if len(self.carry):
            # Only happens when the chunk length is not a multiple of dec.
            # Choosing spb as a multiple avoids this copy entirely.
            mixed = np.concatenate((self.carry, mixed))
        whole = (len(mixed) // self.dec) * self.dec
        self.carry = mixed[whole:].copy()
        if whole == 0:
            return np.empty(0, dtype=np.complex64)
        return mixed[:whole].reshape(-1, self.dec).mean(axis=1)


# ---------------------------------------------------------------- config file

class ConfigError(Exception):
    pass


def load_config(path):
    """Read the console's generated chirp_config.py without executing it."""
    import ast
    with open(path, "r", encoding="utf-8") as fh:
        raw = fh.read()

    if not raw.strip():
        raise ConfigError(
            "%s is empty (%d bytes)."
            "\nThe console truncates this file when it starts and rewrites it"
            "\nwhen you press START, so an empty one means START never wrote"
            "\nit. Look in the console log for \"Error writing configuration"
            "\nfile\"." % (path, len(raw)))
    try:
        tree = ast.parse(raw, filename=path)
    except SyntaxError as exc:
        raise ConfigError(
            "%s is not valid Python: %s at line %s"
            "\n    %s"
            "\nA bare \"tb =\" does this; see DSCHIRP_FIX_EMPTY_TB."
            % (path, exc.msg, exc.lineno, (exc.text or "").rstrip()))
    cfg = {}
    for node in tree.body:
        if not isinstance(node, ast.Assign) or len(node.targets) != 1:
            continue
        target = node.targets[0]
        if not isinstance(target, ast.Name):
            continue
        try:
            cfg[target.id] = ast.literal_eval(node.value)
        except (ValueError, SyntaxError):
            pass          # if_rate is an expression; we derive it ourselves
    if "sample_rate" in cfg and "dec" in cfg:
        cfg["if_rate"] = cfg["sample_rate"] / cfg["dec"]
    if "sounders" not in cfg:
        raise ConfigError(
            "%s has no 'sounders'. It does define: %s"
            % (path, ", ".join(sorted(cfg)) or "(nothing at all)"))
    return cfg


def all_sounders(cfg):
    out = []
    for thread in cfg.get("sounders", []):
        for sounder in thread:
            out.append(sounder)
    return out


def determine_next(rep, chirpt, now=None):
    """Next chirp start, as unix time. Port of chirp.py's determineNext."""
    if now is None:
        now = time.time()
    tnow = time.gmtime(now)
    midnight = calendar.timegm((tnow.tm_year, tnow.tm_mon, tnow.tm_mday,
                                0, 0, 0, 0, 0, 0))
    t0 = midnight + chirpt
    while t0 - now < 3:
        t0 += rep
    return t0


# ---------------------------------------------------------------- self test

def self_test():
    """Verify the dechirp against a synthetic echo of known delay.

    A signal delayed by tau must land at a beat frequency of exactly
    -rate*tau. That single relation is what turns the output spectrum into a
    delay axis, so if it holds the dechirp is right.
    """
    print("=" * 66)
    print(" self test: synthetic chirp, no radio")
    print("=" * 66)

    sr, rate, dec, dur = 1e6, 100e3, 25, 2.0
    f0 = -sr / 2.0
    ok = True

    for delay_ms in (0.0, 2.0, 8.9, 20.0):
        tau = delay_ms / 1000.0
        n = int(sr * dur)

        # The received signal: the same sweep, tau late.
        t = np.arange(n, dtype=np.float64) / sr
        td = t - tau
        phase = f0 * td + 0.5 * rate * td * td
        x = np.exp(2j * np.pi * (phase - np.floor(phase))).astype(np.complex64)

        d = Dechirper(sr, f0, rate, dec)
        out = []
        pos = 0
        while pos < n:                      # feed in uneven chunks on purpose,
            step = 100003 if pos % 2 == 0 else 65536   # to exercise the carry
            out.append(d.feed(x[pos:pos + step]))
            pos += step
        y = np.concatenate(out)

        if_rate = sr / dec
        spec = np.abs(np.fft.fftshift(np.fft.fft(y * np.hanning(len(y)))))
        freqs = np.fft.fftshift(np.fft.fftfreq(len(y), 1.0 / if_rate))
        measured = freqs[int(np.argmax(spec))]
        expected = -rate * tau

        # One FFT bin is if_rate/len(y); allow two.
        tolerance = 2.0 * if_rate / len(y)
        good = abs(measured - expected) <= tolerance
        ok = ok and good
        print("  delay %5.1f ms -> beat %+9.2f Hz  (expected %+9.2f, tol %.2f)  %s"
              % (delay_ms, measured, expected, tolerance, "ok" if good else "FAIL"))

    # Delay resolution the real schedule gives: sr/dec spans +-if_rate/2 of
    # beat, which is +-if_rate/(2*rate) of delay.
    print()
    print("  With the live schedule (25 MS/s, dec 625, 100 kHz/s):")
    print("    output rate      %.0f Hz" % (25e6 / 625))
    print("    delay span       +-%.3f s  =  +-%.0f km group path"
          % ((25e6 / 625) / 2 / 100e3, 3e5 * ((25e6 / 625) / 2 / 100e3)))
    print("    which matches VIRT_HEIGHT_MIN/MAX = -+60000 km in the console")

    # ---- header packing -------------------------------------------------
    print()
    print("  header packing:")
    raw = pack_lfs_header("cyprus1", 35.0, 34.0, "yoshkar-ola", 56.38, 47.53,
                          1573127410, 10, 20000000, 250, 100000, 300,
                          0, 5000, 625, 25000000, False, 8192, 20000)
    fields = struct.unpack(LFS_STRUCT, raw)
    checks = [
        ("length", len(raw), 512),
        ("magic", fields[0], b"LFSG"),
        ("format_ver", round(fields[1], 3), 1.0),
        ("header_size", fields[3], 498),
        ("start_year", fields[10], 2019),
        ("start_daynumber", fields[11], 311),
        ("start_month", fields[12], 11),
        ("start_day", fields[13], 7),
        ("start_hour", fields[14], 11),
        ("start_minute", fields[15], 50),
        ("start_second", fields[16], 10),
    ]
    for name, got, want in checks:
        good = got == want
        ok = ok and good
        print("    %-16s %-12s %s" % (name, got, "ok" if good else "FAIL, want %s" % (want,)))

    print()
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


# ---------------------------------------------------------------- radio

def open_radio(args, subdev, rate, freq, use_gpsdo=True):
    import uhd
    usrp = uhd.usrp.MultiUSRP(args)
    try:
        usrp.set_rx_subdev_spec(uhd.usrp.SubdevSpec(subdev))
    except Exception as exc:
        print("  could not set subdev %s: %s" % (subdev, exc))

    if use_gpsdo:
        for setter, source in (("set_clock_source", "gpsdo"),
                               ("set_time_source", "gpsdo")):
            try:
                getattr(usrp, setter)(source)
            except Exception as exc:
                print("  %s(gpsdo) failed: %s" % (setter, exc))

    usrp.set_rx_rate(rate, 0)
    usrp.set_rx_freq(uhd.types.TuneRequest(freq), 0)
    return usrp, uhd


class Logger:
    """Append to chirp.log beside the archive, and echo to the terminal.

    The original kept chirp.log and analyzed.log in the data directory. One
    file is enough, but the habit is worth keeping: an unattended sounder that
    leaves no trace of why it stopped is not much use.
    """

    def __init__(self, directory):
        self.path = os.path.join(directory, "chirp.log")
        try:
            os.makedirs(directory, exist_ok=True)
            self.fh = open(self.path, "a", encoding="utf-8")
        except OSError:
            self.fh = None

    def __call__(self, message):
        stamp = time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime())
        # flush: stdout is block-buffered when this runs under a service
        # manager or into a pipe, so without it everything since the last 4 kB
        # boundary is lost when the process is killed -- exactly the output you
        # need in order to find out why it was killed.
        print(message, flush=True)
        if self.fh:
            try:
                self.fh.write("%s %s\n" % (stamp, message))
                self.fh.flush()
            except OSError:
                pass


def status(*fields):
    """One machine-readable line for whatever launched us.

    The console runs the sounder as a child process and reads its stdout, so
    this is the whole progress channel: space-separated, prefixed STATUS, and
    flushed, because stdout is a pipe and therefore block-buffered. The console
    filters these out of the log pane and drives the session widget with them;
    anything else that reads the output can ignore them.
    """
    print("STATUS " + " ".join(str(f) for f in fields), flush=True)


class Radio:
    """The receiver, opened once and reused for every sounding.

    Opening a USRP and disciplining its clock from GPS takes several seconds.
    With rep=300 and dur=250 there are only fifty seconds between soundings, so
    doing that once rather than per capture is the difference between keeping
    the schedule and missing every other chirp.
    """

    def __init__(self, opts, cfg, first_cf):
        self.opts = opts
        self.sr = float(cfg["sample_rate"])
        self.dec = int(cfg["dec"])

        self.usrp, self.uhd = open_radio(opts.args, opts.subdev, self.sr,
                                         first_cf, use_gpsdo=not opts.no_gpsdo)
        self.sr = self.usrp.get_rx_rate()
        self.tuned = first_cf
        self.gps = None                  # unknown until discipline() runs
        self.clock_set = False
        self.authority = "host"

        stream_args = self.uhd.usrp.StreamArgs("fc32", "sc16")
        stream_args.channels = [0]
        self.streamer = self.usrp.get_rx_stream(stream_args)
        self.metadata = self.uhd.types.RXMetadata()

        # Block size rounded down to a multiple of the decimation: otherwise
        # every block leaves a remainder to carry and concatenate onto the next,
        # an eight-megabyte copy for nothing.
        self.spb = max(self.dec, (opts.spb // self.dec) * self.dec)
        self.nbuf = max(3, opts.buffers)
        self.pool = [np.empty((1, self.spb), dtype=np.complex64)
                     for _ in range(self.nbuf)]

    def tune(self, cf):
        if cf != self.tuned:
            self.usrp.set_rx_freq(self.uhd.types.TuneRequest(cf), 0)
            self.tuned = cf

    def gps_locked(self):
        try:
            value = getattr(self.usrp.get_mboard_sensor("gps_locked", 0),
                            "value", "")
            return "true" in str(value).lower()
        except Exception:
            return False

    @staticmethod
    def host_synced():
        """Is this host's clock under NTP discipline? None if unknown."""
        import subprocess
        try:
            out = subprocess.run(
                ["timedatectl", "show", "-p", "NTPSynchronized", "--value"],
                capture_output=True, text=True, timeout=5)
            value = out.stdout.strip()
            return value == "yes" if value in ("yes", "no") else None
        except Exception:
            return None

    def discipline(self, log):
        """Put the radio's clock on a known epoch, and say which one.

        The radio must be set to *something*: a stream command is given as an
        absolute time in the radio's own base, so a clock nobody has set is a
        clock that makes every timed start either late or fifty years away.
        Leaving it unset when GPS was unlocked is what made a whole run of
        soundings fail with rx_metadata_error_code.late.

        GPS is preferred and is real UTC. Failing that the system clock is used,
        which is only as good as NTP but is at least an epoch we share.
        """
        uhd = self.uhd
        locked = self.gps_locked()
        if locked != self.gps:
            log("  GPS %s" % ("locked" if locked else "LOST LOCK"))
        self.gps = locked

        try:
            if locked:
                gps_time = int(float(getattr(
                    self.usrp.get_mboard_sensor("gps_time", 0), "value")))
                self.usrp.set_time_next_pps(uhd.types.TimeSpec(gps_time + 1))
                time.sleep(1.2)
                source = "GPS"
            else:
                # No PPS worth waiting for; set it directly and accept the
                # tens of milliseconds that costs.
                self.usrp.set_time_now(uhd.types.TimeSpec(time.time()))
                source = "the system clock"
            now = self.usrp.get_time_now().get_real_secs()
            log("  radio clock set from %s: %s UTC (%+.3f s from this host)"
                % (source, time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(now)),
                   now - time.time()))
            self.clock_set = True
        except Exception as exc:
            log("  *** could not set the radio clock: %s" % exc)
            self.clock_set = False

        # Decide once which clock to schedule against, and check the answer
        # rather than assuming it.
        #
        # "GPS is locked, therefore the radio holds UTC" is not safe on this
        # hardware. With the host under NTP at about a millisecond, the radio's
        # clock still read 3.9 s away right after being set from GPS -- the
        # gps_time sensor returns stale values, times out, and sometimes throws.
        # A disciplined host beats a GPS reading nobody checked.
        # Precision matters more than it looks. A start-time error of dt puts
        # the whole echo at an apparent delay dt out, and the delay axis is
        # light travel time: one millisecond is 300 km against a window 1680 km
        # wide. GPS is good to microseconds, NTP over a LAN to about a
        # millisecond -- so when both are available and they agree, the radio's
        # clock is the better one to schedule against. NTP's job here is to
        # corroborate it, not to replace it.
        synced = self.host_synced()
        offset = self.offset()
        self.authority = "host"
        if synced:
            if offset is not None and abs(offset) > 0.1:
                log("  *** the radio's clock is %+.3f s from an NTP-synchronised"
                    % offset)
                log("  *** host. Whatever GPS reported, that is not UTC --")
                log("  *** scheduling against this host instead.")
            elif locked and self.clock_set:
                self.authority = "radio"
                log("  clocks agree to %.0f ms; scheduling against the radio,"
                    % (abs(offset or 0.0) * 1e3))
                log("  whose GPS clock is the more precise of the two")
            else:
                log("  scheduling against this host (NTP)")
        elif locked and self.clock_set:
            self.authority = "radio"
            log("  host clock is not NTP-disciplined; scheduling against the")
            log("  radio's GPS clock, which is the better of the two")
        else:
            log("  *** neither clock is disciplined: no NTP here and no GPS")
            log("  *** lock there. Timing is whatever this host believes.")

        if not locked:
            log("  *** Without GPS the delay axis is only as good as this")
            log("  *** host's clock. A millisecond is about 300 km of range.")

    def offset(self):
        """radio time minus system time, or None if unreadable."""
        try:
            return self.usrp.get_time_now().get_real_secs() - time.time()
        except Exception:
            return None


def capture_one(radio, opts, cfg, sounder, t0, path, log, stop):
    """Record and dechirp one sounding. Returns a dict describing what happened."""
    import queue
    import threading

    uhd = radio.uhd
    sr, dec, spb = radio.sr, radio.dec, radio.spb
    cf = float(sounder["cf"])
    rate = float(sounder["rate"])
    dur = float(opts.duration if opts.duration else sounder["dur"])
    f0 = -sr / 2.0

    result = {"path": path, "sounder": sounder["name"], "t0": t0,
              "overflows": 0, "got": 0, "written": 0, "short_reads": 0,
              "busy": 0.0, "stalled": 0.0, "elapsed": 0.0, "wanted": int(sr * dur),
              "incomplete": True, "error": None, "aborted": False}

    radio.tune(cf)

    rx = cfg.get("rx_station", {"name": "", "lat": 0.0, "lon": 0.0})
    header = pack_lfs_header(
        sounder["name"], sounder["lat"], sounder["lon"],
        rx.get("name", ""), rx.get("lat", 0.0), rx.get("lon", 0.0),
        int(t0), sounder["chirpt"], cf, sounder["dur"], rate, sounder["rep"],
        sounder.get("rmin", 0), sounder.get("rmax", 5000), dec, sr,
        cfg.get("whiten", False), cfg.get("whiten_len", 8192),
        cfg.get("whiten_n", 20000))

    # Stream commands name a time in the radio's own base, so the target has to
    # be expressed in that base -- and which clock is authoritative decides how.
    #
    # GPS-locked, the radio holds real UTC and t0 is already in its base. That
    # is the configuration that produced a correct ionogram, with the host's
    # clock reading 3.9 s different at the time: correcting for that offset
    # would have started the sweep 3.9 s late, which at 100 kHz/s is 390 kHz of
    # beat offset and no trace at all.
    #
    # Without GPS there is nothing better than this host, so the offset is
    # measured and applied. That also covers a radio still counting from boot.
    offset = radio.offset()
    if offset is None:
        log("  *** cannot read the radio's clock; assuming it matches ours")
        offset = 0.0
    target = t0 if radio.authority == "radio" else t0 + offset

    # Wait in short steps -- so a stop signal is acted on within half a second
    # rather than whenever the radio next returns -- and measure the remaining
    # time in the radio's base, not this host's. Waiting until "t0 minus five
    # seconds" by a host clock 3.9 s adrift leaves only 1.1 s of real lead,
    # which is how a correctly scheduled capture still managed to miss its own
    # start.
    LEAD = 5.0
    while target - (time.time() + offset) > LEAD:
        if stop["now"]:
            result["aborted"] = True
            return result
        time.sleep(0.5)

    radio_now = time.time() + offset
    lead = target - radio_now

    if abs(offset) > 1.0:
        log("  radio clock is %+.3f s from this host; scheduling against %s"
            % (offset, "the radio (GPS)" if radio.authority == "radio"
               else "this host"))

    cmd = uhd.types.StreamCMD(uhd.types.StreamMode.start_cont)
    timed = False
    if lead < 0.5:
        log("  *** %.2f s until the start in the radio's clock -- too late to"
            % lead)
        log("  *** schedule. Starting now; the sweep will not be aligned.")
    else:
        try:
            cmd.stream_now = False
            cmd.time_spec = uhd.types.TimeSpec(float(target))
            radio.streamer.issue_stream_cmd(cmd)
            timed = True
        except Exception as exc:
            log("  timed start refused (%s)" % exc)
    if not timed:
        cmd.stream_now = True
        radio.streamer.issue_stream_cmd(cmd)
    result["lead"] = lead

    dech = Dechirper(sr, f0, rate, dec)
    free_q = queue.Queue()
    full_q = queue.Queue()
    for b in radio.pool:
        free_q.put(b)
    buf = free_q.get()
    failure = []
    counters = {"written": 0, "busy": 0.0}

    def worker(fh):
        while True:
            item = full_q.get()
            if item is None:
                return
            block, count = item
            try:
                started = time.perf_counter()
                out = dech.feed(block[0, :count])
                if len(out):
                    out.tofile(fh)
                    counters["written"] += len(out)
                counters["busy"] += time.perf_counter() - started
            except Exception as exc:            # keep the receive loop alive
                failure.append(exc)
            finally:
                free_q.put(block)

    wanted = result["wanted"]
    got_total = 0
    overflows = 0
    short_reads = 0
    stalled = 0.0
    waited = 0.0
    began = None
    last_tick = 0.0

    with open(path, "wb") as fh:
        # Before anything else, and flushed: the worker writes through
        # ndarray.tofile, which goes at the file descriptor rather than through
        # this object's buffer. Leaving 512 bytes in the buffer while another
        # thread writes past it is exactly the corruption this prevents.
        fh.write(header)
        fh.flush()

        thread = None
        if opts.threads:
            thread = threading.Thread(target=worker, args=(fh,), daemon=True)
            thread.start()

        while got_total < wanted:
            if stop["now"]:
                result["aborted"] = True
                break
            timeout = 10.0 if began is None else 2.0
            got = radio.streamer.recv(buf, radio.metadata, timeout)
            code = str(radio.metadata.error_code).lower()
            if "overflow" in code:
                overflows += 1
            elif "timeout" in code and began is None:
                # Nothing has arrived yet, which before the scheduled start is
                # the expected state rather than a fault. Treating it as fatal
                # made a capture give up on its own start. Keep waiting until
                # the start is comfortably past.
                waited += timeout
                if time.time() + offset > target + 15.0:
                    result["error"] = "no samples by %.0f s after the scheduled start" % 15.0
                    break
                continue
            elif "none" not in code and got == 0:
                result["error"] = str(radio.metadata.error_code)
                break
            if got <= 0:
                continue
            if began is None:
                began = time.time()
            if got != spb:
                short_reads += 1
            take = min(got, wanted - got_total)
            got_total += take

            # A tick a second is enough for a progress bar and cheap enough
            # not to matter inside the receive loop.
            now = time.time()
            if now - last_tick >= 1.0:
                last_tick = now
                status(sounder["name"], "capturing",
                       "%.4f" % (got_total / float(wanted)),
                       overflows)

            if thread is not None:
                full_q.put((buf, take))
                waited = time.perf_counter()
                buf = free_q.get()          # blocks if the dechirp fell behind
                stalled += time.perf_counter() - waited
            else:
                started = time.perf_counter()
                out = dech.feed(buf[0, :take])
                if len(out):
                    out.tofile(fh)
                    counters["written"] += len(out)
                counters["busy"] += time.perf_counter() - started

        if thread is not None:
            full_q.put(None)
            thread.join()

    radio.streamer.issue_stream_cmd(
        uhd.types.StreamCMD(uhd.types.StreamMode.stop_cont))

    # Drain before returning. Stopping tells the radio to stop sending; it does
    # not empty what is already in flight or sitting in the socket. Leave that
    # there and the next capture's first recv meets stale packets out of
    # sequence and never recovers -- which is exactly the shape of the failure
    # here: one good sounding, then every one after it timing out with no
    # samples at all.
    drained = 0
    deadline = time.time() + 3.0
    while time.time() < deadline:
        try:
            got = radio.streamer.recv(buf, radio.metadata, 0.1)
        except Exception:
            break
        if got == 0:
            break
        drained += got
    if drained:
        result["drained"] = drained

    if failure:
        result["error"] = "dechirp thread: %s" % failure[0]

    result["got"] = got_total
    result["written"] = counters["written"]
    result["busy"] = counters["busy"]
    result["overflows"] = overflows
    result["short_reads"] = short_reads
    result["stalled"] = stalled
    result["elapsed"] = (time.time() - began) if began else 0.0
    result["incomplete"] = got_total < wanted

    # A capture that stopped early is not a capture. Rename it so the console
    # does not try to load a header promising 250 s in front of no data.
    if result["incomplete"]:
        partial = path + ".partial"
        try:
            os.replace(path, partial)
            result["path"] = partial
        except OSError:
            pass

    try:
        result["bytes"] = os.path.getsize(result["path"])
    except OSError:
        result["bytes"] = 0
    return result


def report(result, opts, sr, dec, log):
    """Say what one capture did. Returns 0 clean, 1 lossy, 2 failed."""
    log("  received  %d samples (%.1f s of signal)"
        % (result["got"], result["got"] / sr))
    if result["short_reads"]:
        log("  short reads %d (recv returned less than a full block)"
            % result["short_reads"])

    # Stat the file rather than reporting what we believe we wrote. Reporting
    # the computed size once hid a missing header through several captures: the
    # number printed was right every time and the file was wrong every time.
    expected = LFS_TOTAL + result["written"] * 8
    log("  wrote     %d samples at %.0f Hz -> %d bytes"
        % (result["written"], sr / dec, result["bytes"]))
    if result["bytes"] != expected:
        log("  *** expected %d bytes (%d header + %d samples); the file is %+d off"
            % (expected, LFS_TOTAL, result["written"] * 8,
               result["bytes"] - expected))
    log("  overflows %d" % result["overflows"])

    # Not "how many times real time": the radio delivers samples at exactly sr,
    # so a live capture can never finish faster than the signal arrives and that
    # ratio is pinned at 1.0 however much headroom there is. What matters is how
    # much of the wall clock the dechirp needed.
    if result["elapsed"] > 1.0 and not result["incomplete"]:
        occupancy = result["busy"] / result["elapsed"]
        log("  dechirp   busy %.0f%% of the capture (%.1f s of %.1f s)"
            % (100 * occupancy, result["busy"], result["elapsed"]))
        if opts.threads:
            log("  receiver  stalled %.2f s waiting for a free buffer"
                % result["stalled"])
        headroom = (1.0 / occupancy) if occupancy > 0 else float("inf")
        if occupancy < 0.7:
            log("  %.1fx headroom -- comfortable." % headroom)
        elif occupancy < 0.95:
            log("  %.1fx headroom -- it fits, but anything else running will"
                " cost samples." % headroom)
        else:
            log("  *** no headroom. The dechirp is saturating a core; expect")
            log("  *** overflows whenever the machine does anything else.")

    if result["aborted"]:
        log("  stopped on request after %.1f s" % (result["got"] / sr))
        return 2
    if result["incomplete"]:
        log("  *** CAPTURE FAILED after %.1f s of the %.0f s expected"
            % (result["got"] / sr, result["wanted"] / sr))
        if result["error"]:
            log("  *** %s" % result["error"])
        log("  *** kept as %s" % os.path.basename(result["path"]))
        if opts.args and "frame_size" in opts.args:
            log("  *** You passed a frame size in --args. A stream that dies")
            log("  *** immediately is the signature of a path that cannot carry")
            log("  *** the frames it agreed to. Retry without it.")
        return 2

    if result["overflows"] == 0:
        log("  clean capture")
        return 0

    # Overflows here are not the dechirp being too slow on average -- occupancy
    # says otherwise -- but it falling behind in bursts, and the receiver having
    # nowhere to put samples while it catches up. The stall time is the
    # giveaway: it tracks the overflow count almost exactly.
    bursty = result["stalled"] > 1.0
    if result["overflows"] <= 5 and not bursty:
        log("  %d overflow%s -- gaps of well under a millisecond each; a chirp"
            % (result["overflows"], "" if result["overflows"] == 1 else "s"))
        log("  sounding integrates across the whole sweep, so this costs a"
            " little SNR rather than the trace")
        return 0

    log("  *** %d overflows, and the receiver spent %.1f s with no free buffer."
        % (result["overflows"], result["stalled"]))
    log("  *** The file is complete and probably still shows a trace; this is")
    log("  *** lost signal-to-noise, not a lost sounding.")
    if bursty:
        log("  *** The dechirp used only %.0f%% of the clock, so it is not too"
            % (100 * result["busy"] / max(result["elapsed"], 1e-9)))
        log("  *** slow -- it stalls in bursts. More buffers ride those out:")
        log("  ***     --buffers 32")
        log("  *** and anything else heavy on this machine competes for the")
        log("  *** same cores. The console reprocessing an 80 MB capture is")
        log("  *** exactly such a thing.")
    return 1


def pick_next(sounders):
    """The station whose next chirp comes soonest.

    One receiver can only follow one sweep at a time. The original ran several
    in parallel threads, one per receiver; with a single radio the useful
    equivalent is simply to take whichever is due first.
    """
    now = time.time()
    best, best_t = None, None
    for s in sounders:
        t = determine_next(s["rep"], s["chirpt"], now)
        if best_t is None or t < best_t:
            best, best_t = s, t
    return best, best_t


def enough_disk(root, need_bytes, floor_bytes, log):
    try:
        free = shutil.disk_usage(root).free
    except OSError as exc:
        log("  cannot check free space on %s: %s" % (root, exc))
        return True                     # do not refuse to work over this
    if free >= need_bytes + floor_bytes:
        return True
    log("  *** NOT ENOUGH DISK: %.1f GB free; this capture needs %.0f MB on"
        % (free / 2 ** 30, need_bytes / 2 ** 20))
    log("  *** top of a %.1f GB floor. Skipping this sounding."
        % (floor_bytes / 2 ** 30))
    log("  *** A station at rep=300 writes about 23 GB a day. Move captures")
    log("  *** off, or lower --min-free-gb if you know what you are doing.")
    return False


def load_sidecar_builder(log):
    """Import python/lfp_products.py, or explain why the sidecar is skipped."""
    here = os.path.dirname(os.path.abspath(__file__))
    lib = os.path.normpath(os.path.join(here, os.pardir, "python"))
    if lib not in sys.path:
        sys.path.insert(0, lib)
    try:
        import lfp_products
        return lfp_products
    except Exception as exc:
        log("  no sidecars: %s" % exc)
        return None


def run_live(opts, cfg, sounders):
    import signal as signal_module

    sr = float(cfg["sample_rate"])
    dec = int(cfg["dec"])
    outroot = opts.outdir or cfg.get("data_dir", ".")
    os.makedirs(outroot, exist_ok=True)
    log = Logger(outroot)

    first = sounders[0]
    log("sounder    %s%s"
        % (", ".join(s["name"] for s in sounders),
           "" if len(sounders) == 1 else "  (whichever is due first)"))
    log("  centre   %.3f MHz, sweeping %.3f -> %.3f MHz at %g kHz/s"
        % (first["cf"] / 1e6, (first["cf"] - sr / 2) / 1e6,
           (first["cf"] - sr / 2 + first["dur"] * first["rate"]) / 1e6,
           first["rate"] / 1e3))
    log("  sampling %.3f MS/s, dec %d -> %.0f Hz out" % (sr / 1e6, dec, sr / dec))
    log("  archive  %s" % outroot)
    if opts.duration:
        log("  duration %g s -- SHORTENED for a test; the header still says %d"
            % (opts.duration, first["dur"]))

    products = load_sidecar_builder(log) if opts.sidecar else None
    if products:
        log("  writing .lfp sidecars beside each capture")

    radio = Radio(opts, cfg, float(first["cf"]))
    if not opts.no_gpsdo:
        radio.discipline(log)
    log("  %d receive buffers of %d samples, dechirp on %s"
        % (radio.nbuf, radio.spb,
           "a worker thread" if opts.threads else "the receive thread"))

    stop = {"now": False}

    def on_signal(signum, _frame):
        if stop["now"]:                 # a second one means "now, really"
            log("second signal -- exiting immediately")
            raise SystemExit(130)
        stop["now"] = True
        log("signal %d received; finishing and stopping" % signum)

    signal_module.signal(signal_module.SIGINT, on_signal)
    signal_module.signal(signal_module.SIGTERM, on_signal)

    limit = opts.count if opts.count else (0 if opts.loop else 1)
    floor_bytes = int(opts.min_free_gb * 2 ** 30)
    done = 0
    clean = 0
    worst = 0
    skips = 0
    failures = 0

    while not stop["now"]:
        sounder, t0 = pick_next(sounders)
        dur = float(opts.duration if opts.duration else sounder["dur"])
        need = LFS_TOTAL + int((sr / dec) * dur) * 8

        stamp = time.gmtime(t0)
        day = time.strftime("%Y.%m.%d", stamp)
        name = "%s_%04d%02d%02d_%02d%02d%02d.lfs" % (
            sounder["name"], stamp.tm_year, stamp.tm_mon, stamp.tm_mday,
            stamp.tm_hour, stamp.tm_min, stamp.tm_sec)
        outdir = os.path.join(outroot, day)

        log("")
        log("%s at %s UTC (%.0f s away) -> %s/%s"
            % (sounder["name"],
               time.strftime("%Y-%m-%d %H:%M:%S", stamp), t0 - time.time(),
               day, name))
        status(sounder["name"], "waiting", int(t0), int(t0 + dur))

        if not enough_disk(outroot, need, floor_bytes, log):
            skips += 1
            if skips >= 3:
                log("  *** three windows skipped for want of disk. Stopping:")
                log("  *** nothing here will free space, and a sounder that")
                log("  *** silently records nothing is worse than one that")
                log("  *** stops and says why.")
                worst = max(worst, 2)
                break
            # Sit out this window rather than spinning: the next one is a whole
            # repetition period away and nothing will have changed before then.
            while time.time() < t0 + dur and not stop["now"]:
                time.sleep(1.0)
            continue
        skips = 0

        try:
            os.makedirs(outdir, exist_ok=True)
        except OSError as exc:
            log("  *** cannot create %s: %s" % (outdir, exc))
            worst = max(worst, 2)
            break

        try:
            result = capture_one(radio, opts, cfg, sounder, t0,
                                 os.path.join(outdir, name), log, stop)
        except Exception as exc:
            # One bad sounding must not end an unattended run -- but it must
            # not be retried instantly either. pick_next returns the same t0
            # until that window has passed, so without waiting this retries
            # thousands of times a second and fails identically every time.
            log("  *** capture raised: %s" % exc)
            if isinstance(exc, PermissionError):
                log("  *** The archive is not writable by this user. Captures")
                log("  *** made earlier under sudo leave root-owned day")
                log("  *** directories behind:")
                log("  ***     sudo chown -R $USER: %s" % outroot)
            status(sounder["name"], "failed", 0, 0)
            worst = max(worst, 2)
            done += 1
            failures += 1
            if failures >= 3:
                log("")
                log("*** three soundings in a row produced no data. Stopping.")
                break
            if limit and done >= limit:
                break
            # Sit out the rest of this window rather than hammering it.
            while time.time() < t0 + dur and not stop["now"]:
                time.sleep(1.0)
            continue

        if result["aborted"] and result["got"] == 0:
            log("  stopped before the sounding began")
            break

        rc = report(result, opts, sr, dec, log)

        # The sidecar is what anything downstream actually reads, and there is
        # a whole repetition period of idle time to build it in. Doing it here
        # means the console never has to touch the 80 MB capture.
        if products and rc <= 1 and not result["incomplete"]:
            try:
                began_side = time.time()
                status(sounder["name"], "writing")
                out, size = products.build_one(result["path"], quiet=True)
                raw = os.path.getsize(result["path"])
                log("  sidecar   %s  %.1f kB  (%.0fx smaller, %.1f s)"
                    % (os.path.basename(out), size / 1024.0,
                       raw / float(size or 1), time.time() - began_side))
            except Exception as exc:
                log("  *** sidecar failed: %s" % exc)

        # After the sidecar, so the panel reads capturing -> products -> result
        # rather than announcing a verdict and then going quiet for a second.
        status(sounder["name"],
               {0: "clean", 1: "degraded"}.get(rc, "failed"),
               result["overflows"], result["got"])

        worst = max(worst, rc)
        done += 1
        if rc == 0:
            clean += 1
        # Only a capture that did not happen counts toward stopping. A complete
        # sounding with overflows is degraded, not failed -- it is a full file
        # that still shows a trace, and halting the sounder over it throws away
        # the good captures either side.
        if rc >= 2:
            failures += 1
            if failures >= 3:
                log("")
                log("*** three soundings in a row produced no data. Stopping.")
                log("*** Whatever is wrong is not going to fix itself, and")
                log("*** every further attempt leaves another .partial file.")
                log("*** If the messages above name a clock, this host and the")
                log("*** radio disagree about the time; if they say timeout")
                log("*** with no samples at all, the radio has most likely")
                log("*** stopped answering and wants a power cycle.")
                worst = max(worst, 2)
                break
        else:
            failures = 0
        if limit and done >= limit:
            break

    if done > 1 or opts.loop or opts.count:
        log("")
        log("%d sounding%s, %d clean" % (done, "" if done == 1 else "s", clean))
    if done == 1 and worst == 0:
        log("")
        log("  point the console at %s" % outroot)
    return worst


def benchmark(opts):
    """How fast does the dechirp run on this machine, with no radio involved?

    The sounder needs 25 MS/s sustained. If this reports less, no amount of
    network tuning will help -- the CPU is the limit, and that is the whole
    question hanging over running chirpsounder1 on an old laptop.
    """
    sr, rate, dec = opts.rate, opts.chirp_rate, opts.dec
    spb = max(dec, (opts.spb // dec) * dec)
    print("=" * 66)
    print(" dechirp throughput, %d samples per block" % spb)
    print("=" * 66)

    # Rotate through a pool rather than reusing one array. Feeding the same
    # buffer repeatedly leaves it in cache and overstates the result -- which is
    # how an earlier version of this reported 2.3x on a host that then managed
    # 0.83x live. Real blocks arrive fresh from the network every time.
    pool = [(np.random.randn(spb) + 1j * np.random.randn(spb)).astype(np.complex64)
            for _ in range(8)]
    print("  input pool %.0f MB, cycled so nothing stays in cache"
          % (8 * spb * 8 / 1e6))

    d = Dechirper(sr, -sr / 2.0, rate, dec)
    d.feed(pool[0])                             # build the tables and scratch
    blocks = max(8, int(math.ceil(2.0 * sr / spb)))
    began = time.time()
    for i in range(blocks):
        d.feed(pool[i % len(pool)])
    elapsed = time.time() - began

    total = blocks * spb
    throughput = total / elapsed
    print()
    print("  processed %d samples in %.2f s" % (total, elapsed))
    print("  dechirp throughput  %.2f MS/s" % (throughput / 1e6))
    print("  needed              %.2f MS/s" % (sr / 1e6))
    print()
    margin = throughput / sr
    print("  %.2fx real time for the dechirp alone." % margin)
    print()
    print("  This is not the whole story. Receiving costs real time too --")
    print("  UHD converts sc16 to complex float at 100 MB/s in, 200 MB/s out --")
    print("  and with --threads 1 that runs in parallel with this. The live")
    print("  figure is whichever of the two is slower, so treat this as an")
    print("  upper bound and trust the number the live run prints.")
    if margin < 1.2:
        print()
        print("  *** Under 1.2x here means the dechirp alone is marginal.")
        print("  *** Options, roughly in order of effort:")
        print("  ***   - close everything else; performance CPU governor")
        print("  ***   - larger --spb, fewer Python round trips per second")
        print("  ***   - move the mix-and-decimate inner loop into C")
    return 0 if margin >= 1.0 else 1


def repair_header(opts, cfg, sounder):
    """Put a header back on a capture that lost one.

    Captures written between the threading change and its fix are correct
    dechirped data with no 512-byte header, so the samples are worth keeping.
    Everything the header needs is recoverable: the schedule from the config,
    the start time from the filename the capture was given.
    """
    import re

    path = opts.repair
    with open(path, "rb") as fh:
        if fh.read(4) == b"LFSG":
            print("%s already has a header; nothing to do" % path)
            return 0

    base = os.path.basename(path)
    m = re.match(r"(?P<name>.+)_(?P<d>\d{8})_(?P<t>\d{6})\.lfs$", base)
    if not m:
        print("cannot read a timestamp from %s" % base)
        print("Expected <station>_<yyyyMMdd>_<hhmmss>.lfs")
        return 2
    d, t = m.group("d"), m.group("t")
    t0 = calendar.timegm((int(d[0:4]), int(d[4:6]), int(d[6:8]),
                          int(t[0:2]), int(t[2:4]), int(t[4:6]), 0, 0, 0))

    size = os.path.getsize(path)
    if size % 8:
        print("%s is %d bytes, not a whole number of complex64 samples."
              % (path, size))
        print("It is not a headerless capture; leaving it alone.")
        return 2

    sr = float(cfg["sample_rate"])
    dec = int(cfg["dec"])
    rx = cfg.get("rx_station", {"name": "", "lat": 0.0, "lon": 0.0})
    samples = size // 8
    expected = int((sr / dec) * sounder["dur"])
    print("  %s" % path)
    print("  %d samples, %.1f s at %.0f Hz (a full sounding is %d)"
          % (samples, samples / (sr / dec), sr / dec, expected))
    print("  start %s UTC, from the filename"
          % time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(t0)))

    header = pack_lfs_header(
        m.group("name"), sounder["lat"], sounder["lon"],
        rx.get("name", ""), rx.get("lat", 0.0), rx.get("lon", 0.0),
        t0, sounder["chirpt"], sounder["cf"], sounder["dur"], sounder["rate"],
        sounder["rep"], sounder.get("rmin", 0), sounder.get("rmax", 5000),
        dec, sr, cfg.get("whiten", False), cfg.get("whiten_len", 8192),
        cfg.get("whiten_n", 20000))

    tmp = path + ".repairing"
    with open(tmp, "wb") as out:
        out.write(header)
        with open(path, "rb") as src:
            shutil.copyfileobj(src, out, 1 << 22)
    os.replace(tmp, path)
    print("  header prepended; now %d bytes" % os.path.getsize(path))
    print("  check it:  python3 python/lfs_info.py %s" % path)
    return 0


def run_from_raw(opts):
    """Dechirp a raw complex64 recording -- for testing without the radio."""
    sr = opts.rate
    rate = opts.chirp_rate
    dec = opts.dec
    f0 = -sr / 2.0
    x = np.fromfile(opts.from_raw, dtype=np.complex64)
    print("  %d samples, %.3f s at %.3f MS/s" % (len(x), len(x) / sr, sr / 1e6))
    dech = Dechirper(sr, f0, rate, dec)
    out = []
    step = 1 << 20
    for pos in range(0, len(x), step):
        out.append(dech.feed(x[pos:pos + step]))
    y = np.concatenate(out)
    y.tofile(opts.raw_out or "dechirped.c64")
    print("  wrote %d samples to %s"
          % (len(y), opts.raw_out or "dechirped.c64"))
    spec = np.abs(np.fft.fftshift(np.fft.fft(y * np.hanning(len(y)))))
    freqs = np.fft.fftshift(np.fft.fftfreq(len(y), dec / sr))
    peak = freqs[int(np.argmax(spec))]
    print("  strongest beat %+.1f Hz -> delay %.3f ms -> %.0f km group path"
          % (peak, -1000.0 * peak / rate, -3e5 * peak / rate))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--benchmark", action="store_true",
                    help="measure dechirp throughput on this machine, no radio")
    ap.add_argument("--from-raw", default="")
    ap.add_argument("--repair", default="",
                    help="prepend a header to a capture that lost one")
    ap.add_argument("--raw-out", default="")
    ap.add_argument("--config", default="/tmp/out/chirp_config.py")
    ap.add_argument("--station", default="",
                    help="restrict to one sounder; default all of them")
    ap.add_argument("--loop", action="store_true",
                    help="keep sounding until stopped, instead of once")
    ap.add_argument("--count", type=int, default=0,
                    help="stop after this many soundings (implies looping)")
    ap.add_argument("--min-free-gb", type=float, default=5.0,
                    help="refuse to start a capture that would leave less free")
    ap.add_argument("--args", default="addr=192.168.10.3")
    ap.add_argument("--subdev", default="A:A")
    ap.add_argument("--outdir", default="")
    ap.add_argument("--duration", type=float, default=0.0,
                    help="override dur, for a short test run")
    ap.add_argument("--spb", type=int, default=625 * 1600,
                    help="samples per block; rounded to a multiple of dec")
    ap.add_argument("--buffers", type=int, default=16,
                    help="receive buffers in flight; each absorbs one block of "
                         "burst, so more of them ride out a busy moment")
    ap.add_argument("--threads", type=int, default=1,
                    help="1 = dechirp on a worker thread (default), 0 = inline")
    ap.add_argument("--no-gpsdo", action="store_true")
    ap.add_argument("--no-sidecar", dest="sidecar", action="store_false",
                    default=True,
                    help="do not write .lfp sidecars after each capture")
    ap.add_argument("--rate", type=float, default=25e6, help="--from-raw only")
    ap.add_argument("--chirp-rate", type=float, default=100e3, help="--from-raw only")
    ap.add_argument("--dec", type=int, default=625, help="--from-raw only")
    opts = ap.parse_args()

    if opts.self_test:
        return self_test()
    if opts.benchmark:
        return benchmark(opts)
    if opts.from_raw:
        return run_from_raw(opts)

    if not os.path.exists(opts.config):
        print("no config at %s" % opts.config)
        print("The console writes it when you press START; its path is the")
        print("\"Konfiguraciya\" field in the parameters dialog.")
        return 2
    try:
        cfg = load_config(opts.config)
    except ConfigError as exc:
        print(exc)
        return 2
    sounders = all_sounders(cfg)
    if not sounders:
        print("%s defines 'sounders' but it is empty: %r"
              % (opts.config, cfg.get("sounders")))
        print("The console only emits a station whose schedule has active=true.")
        return 2
    if opts.station:
        matches = [s for s in sounders if s.get("name") == opts.station]
        if not matches:
            print("no sounder named %r; have: %s"
                  % (opts.station, ", ".join(s.get("name", "?") for s in sounders)))
            return 2
        sounders = matches
    chosen = sounders[0]
    if opts.repair:
        return repair_header(opts, cfg, chosen)
    return run_live(opts, cfg, sounders)


if __name__ == "__main__":
    sys.exit(main())
