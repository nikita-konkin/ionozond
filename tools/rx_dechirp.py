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
        """Per-chunk offsets, cached: they only depend on the chunk length."""
        if length != self._cache_len:
            m = np.arange(length, dtype=np.float64)
            self._dt = m / self.sr
            self._dt2 = 0.5 * self.rate * self._dt * self._dt
            self._cache_len = length
        return self._dt, self._dt2

    def phase_turns(self, length):
        """Replica phase in turns, for `length` samples from the current index.

        Split as a block constant plus local terms so the arithmetic stays in
        the thousands rather than the billions. Over a 250 s sounding the
        absolute phase reaches ~3e9 turns; evaluating that directly in float64
        would still work, but this keeps six more digits of margin and is the
        same recurrence the C++ block used.
        """
        t0 = self.n / self.sr
        c0 = self.f0 * t0 + 0.5 * self.rate * t0 * t0
        c1 = self.f0 + self.rate * t0          # instantaneous frequency here
        dt, dt2 = self._local(length)
        return (c0 - math.floor(c0)) + c1 * dt + dt2

    def feed(self, chunk):
        """Dechirp and decimate one chunk. Returns whatever completed."""
        length = len(chunk)
        if length == 0:
            return np.empty(0, dtype=np.complex64)

        turns = self.phase_turns(length)
        idx = (turns * self.TABLE_SIZE).astype(np.int64)
        idx &= self.TABLE_SIZE - 1
        mixed = chunk * self.table[idx]
        self.n += length

        if len(self.carry):
            mixed = np.concatenate((self.carry, mixed))
        whole = (len(mixed) // self.dec) * self.dec
        self.carry = mixed[whole:].copy()
        if whole == 0:
            return np.empty(0, dtype=np.complex64)
        block = mixed[:whole].reshape(-1, self.dec)
        return block.mean(axis=1).astype(np.complex64)


# ---------------------------------------------------------------- config file

def load_config(path):
    """Read the console's generated chirp_config.py without executing it."""
    import ast
    with open(path, "r", encoding="utf-8") as fh:
        tree = ast.parse(fh.read(), filename=path)
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

        locked = None
        try:
            locked = str(getattr(usrp.get_mboard_sensor("gps_locked", 0),
                                 "value", "")).lower()
        except Exception:
            pass
        if locked is not None and "true" not in locked:
            print("  *** GPS is not locked. Chirp timing will come from the")
            print("  *** system clock, which is far less certain -- and a")
            print("  *** millisecond is about 300 km of apparent range.")
        else:
            # Put the radio's clock on GPS seconds so a timed stream command
            # can be given in the same epoch the schedule uses.
            try:
                gps_time = int(float(getattr(
                    usrp.get_mboard_sensor("gps_time", 0), "value")))
                usrp.set_time_next_pps(uhd.types.TimeSpec(gps_time + 1))
                time.sleep(1.2)
                print("  radio clock set from GPS: %s UTC"
                      % time.strftime("%Y-%m-%d %H:%M:%S",
                                      time.gmtime(usrp.get_time_now().get_real_secs())))
            except Exception as exc:
                print("  could not set radio time from GPS: %s" % exc)

    usrp.set_rx_rate(rate, 0)
    usrp.set_rx_freq(uhd.types.TuneRequest(freq), 0)
    return usrp, uhd


def run_live(opts, cfg, sounder):
    sr = float(cfg["sample_rate"])
    dec = int(cfg["dec"])
    cf = float(sounder["cf"])
    rate = float(sounder["rate"])
    dur = float(opts.duration if opts.duration else sounder["dur"])
    f0 = -sr / 2.0

    print("sounder    %s" % sounder["name"])
    print("  centre   %.3f MHz, sweeping %.3f -> %.3f MHz at %g kHz/s"
          % (cf / 1e6, (cf - sr / 2) / 1e6,
             (cf - sr / 2 + sounder["dur"] * rate) / 1e6, rate / 1e3))
    print("  sampling %.3f MS/s, dec %d -> %.0f Hz out"
          % (sr / 1e6, dec, sr / dec))
    print("  duration %g s%s" % (dur, "" if not opts.duration else " (SHORTENED for a test)"))
    print()

    usrp, uhd = open_radio(opts.args, opts.subdev, sr, cf,
                           use_gpsdo=not opts.no_gpsdo)
    sr = usrp.get_rx_rate()

    t0 = determine_next(sounder["rep"], sounder["chirpt"])
    print("  next chirp at %s UTC (%.1f s away)"
          % (time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(t0)), t0 - time.time()))

    rx = cfg.get("rx_station", {"name": "", "lat": 0.0, "lon": 0.0})
    day = time.strftime("%Y.%m.%d", time.gmtime(t0))
    tstamp = time.gmtime(t0)
    name = "%s_%04d%02d%02d_%02d%02d%02d.lfs" % (
        sounder["name"], tstamp.tm_year, tstamp.tm_mon, tstamp.tm_mday,
        tstamp.tm_hour, tstamp.tm_min, tstamp.tm_sec)
    outdir = os.path.join(opts.outdir or cfg.get("data_dir", "."), day)
    os.makedirs(outdir, exist_ok=True)
    path = os.path.join(outdir, name)

    header = pack_lfs_header(
        sounder["name"], sounder["lat"], sounder["lon"],
        rx.get("name", ""), rx.get("lat", 0.0), rx.get("lon", 0.0),
        int(t0), sounder["chirpt"], cf, sounder["dur"], rate, sounder["rep"],
        sounder.get("rmin", 0), sounder.get("rmax", 5000), dec, sr,
        cfg.get("whiten", False), cfg.get("whiten_len", 8192),
        cfg.get("whiten_n", 20000))

    stream_args = uhd.usrp.StreamArgs("fc32", "sc16")
    stream_args.channels = [0]
    streamer = usrp.get_rx_stream(stream_args)
    spb = opts.spb
    buf = np.empty((1, spb), dtype=np.complex64)
    metadata = uhd.types.RXMetadata()

    cmd = uhd.types.StreamCMD(uhd.types.StreamMode.start_cont)
    try:
        cmd.stream_now = False
        cmd.time_spec = uhd.types.TimeSpec(float(t0))
        streamer.issue_stream_cmd(cmd)
        timed = True
    except Exception as exc:
        print("  timed start unavailable (%s); starting now and accepting" % exc)
        print("  whatever offset that costs")
        cmd.stream_now = True
        streamer.issue_stream_cmd(cmd)
        timed = False

    wanted = int(sr * dur)
    dech = Dechirper(sr, f0, rate, dec)
    overflows = 0
    got_total = 0
    written = 0

    print("  writing %s" % path)
    began = None
    with open(path, "wb") as fh:
        fh.write(header)
        while got_total < wanted:
            timeout = max(5.0, t0 - time.time() + 5.0) if timed and began is None else 2.0
            got = streamer.recv(buf, metadata, timeout)
            code = str(metadata.error_code).lower()
            if "overflow" in code:
                overflows += 1
            elif "none" not in code and got == 0:
                print("  stream error: %s" % metadata.error_code)
                break
            if got <= 0:
                continue
            if began is None:
                began = time.time()
            take = min(got, wanted - got_total)
            out = dech.feed(buf[0, :take])
            if len(out):
                out.tofile(fh)
                written += len(out)
            got_total += take

    streamer.issue_stream_cmd(uhd.types.StreamCMD(uhd.types.StreamMode.stop_cont))
    elapsed = (time.time() - began) if began else 0.0

    print()
    print("  received  %d samples (%.1f s of signal)" % (got_total, got_total / sr))
    print("  wrote     %d samples at %.0f Hz -> %d bytes"
          % (written, sr / dec, 512 + written * 8))
    print("  overflows %d" % overflows)
    if elapsed > 0:
        margin = (got_total / sr) / elapsed
        print("  kept up   %.2fx real time" % margin)
        if margin < 1.0:
            print("  *** SLOWER THAN REAL TIME. The dechirp cannot sustain this")
            print("  *** rate on this machine, which is what the overflow count")
            print("  *** above is really telling you.")
    if overflows:
        print("  *** samples were lost, so the ionogram will have gaps")
    print()
    print("  read it with:   python3 python/lfs_info.py %s" % path)
    return 0 if overflows == 0 else 1


def benchmark(opts):
    """How fast does the dechirp run on this machine, with no radio involved?

    The sounder needs 25 MS/s sustained. If this reports less, no amount of
    network tuning will help -- the CPU is the limit, and that is the whole
    question hanging over running chirpsounder1 on an old laptop.
    """
    sr, rate, dec = opts.rate, opts.chirp_rate, opts.dec
    spb = opts.spb
    print("=" * 66)
    print(" dechirp throughput, %d samples per block" % spb)
    print("=" * 66)

    x = (np.random.randn(spb) + 1j * np.random.randn(spb)).astype(np.complex64)
    d = Dechirper(sr, -sr / 2.0, rate, dec)

    d.feed(x)                                   # warm the caches and the tables
    blocks = max(4, int(math.ceil(2.0 * sr / spb)))
    began = time.time()
    for _ in range(blocks):
        d.feed(x)
    elapsed = time.time() - began

    total = blocks * spb
    throughput = total / elapsed
    print("  processed %d samples in %.2f s" % (total, elapsed))
    print("  throughput  %.2f MS/s" % (throughput / 1e6))
    print("  needed      %.2f MS/s" % (sr / 1e6))
    print()
    margin = throughput / sr
    if margin >= 1.5:
        print("  %.2fx real time -- comfortable." % margin)
    elif margin >= 1.0:
        print("  %.2fx real time -- it keeps up, but with little to spare." % margin)
        print("  Expect occasional overflows when anything else runs.")
    else:
        print("  *** %.2fx real time -- TOO SLOW on this machine." % margin)
        print("  *** Options, roughly in order of effort:")
        print("  ***   - close everything else; use the performance governor")
        print("  ***   - larger blocks (--spb), fewer Python round trips")
        print("  ***   - move the mix-and-decimate inner loop into C")
    return 0 if margin >= 1.0 else 1


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
    ap.add_argument("--raw-out", default="")
    ap.add_argument("--config", default="/tmp/out/chirp_config.py")
    ap.add_argument("--station", default="", help="which sounder; default the first")
    ap.add_argument("--args", default="addr=192.168.10.3")
    ap.add_argument("--subdev", default="A:A")
    ap.add_argument("--outdir", default="")
    ap.add_argument("--duration", type=float, default=0.0,
                    help="override dur, for a short test run")
    ap.add_argument("--spb", type=int, default=1 << 20)
    ap.add_argument("--no-gpsdo", action="store_true")
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
    cfg = load_config(opts.config)
    sounders = all_sounders(cfg)
    if not sounders:
        print("no sounders in %s" % opts.config)
        return 2
    chosen = sounders[0]
    if opts.station:
        matches = [s for s in sounders if s.get("name") == opts.station]
        if not matches:
            print("no sounder named %r; have: %s"
                  % (opts.station, ", ".join(s.get("name", "?") for s in sounders)))
            return 2
        chosen = matches[0]
    return run_live(opts, cfg, chosen)


if __name__ == "__main__":
    sys.exit(main())
