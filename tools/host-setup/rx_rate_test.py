#!/usr/bin/env python3
"""Measure whether a USRP can actually deliver the sounder's data rate.

UHD ships a C++ ``benchmark_rate`` example that does this, but Ubuntu's
uhd-host package does not install it. python3-uhd is available, so do the
measurement directly.

    rx_rate_test.py --args addr=192.168.10.3 --sensors
    rx_rate_test.py --args addr=192.168.10.3 --rates 5e6,12.5e6,25e6

Exit status: 0 clean, 1 samples were lost, 2 could not run at all.

Receiving into Python is a fair test here. The sounder's UHD source hands out
complex float and the dechirp runs on the host, so the conversion and the
memory traffic this measures are work the real application also does. What it
adds beyond that is one Python call per buffer, which is why the buffer is a
million samples rather than one packet: at 25 MS/s a packet-sized buffer would
mean about 69000 calls a second and would measure the interpreter, not the
radio.
"""

import argparse
import sys
import time

try:
    import numpy as np
except ImportError:
    sys.stderr.write("numpy is missing:  sudo apt-get install -y python3-numpy\n")
    sys.exit(2)

try:
    import uhd
except ImportError:
    sys.stderr.write("python3-uhd is missing:  sudo apt-get install -y python3-uhd\n")
    sys.exit(2)


def fmt_sensor(sensor):
    """Render a sensor_value across binding versions.

    UHD 4.6 as packaged for Ubuntu 24.04 does not expose to_pp_string on the
    Python object, though the C++ class has it. Fall back to the attributes,
    which are bound.
    """
    pp = getattr(sensor, "to_pp_string", None)
    if callable(pp):
        try:
            return pp()
        except Exception:
            pass
    value = getattr(sensor, "value", None)
    if value is not None:
        unit = getattr(sensor, "unit", None) or ""
        return ("%s %s" % (value, unit)).strip()
    return repr(sensor)


def show_sensors(usrp):
    try:
        names = list(usrp.get_mboard_sensor_names(0))
    except Exception as exc:
        print("  could not list sensors: %s" % exc)
        return {}

    print("  sensors present: %s" % ", ".join(names))
    print()
    found = {}
    for name in ("ref_locked", "gps_locked", "gps_servo", "gps_time", "gps_gpgga"):
        if name not in names:
            continue
        try:
            text = fmt_sensor(usrp.get_mboard_sensor(name, 0))
            print("    %-11s = %s" % (name, text))
            found[name] = text
        except Exception as exc:
            print("    %-11s : %s" % (name, exc))

    print()
    ref = found.get("ref_locked", "").lower()
    gps = found.get("gps_locked", "").lower()
    if "true" in ref:
        print("  ref_locked true  - the N210 is locked to the GPSDO's 10 MHz.")
    elif ref:
        print("  *** ref_locked FALSE - the mainboard is not locked to the")
        print("  *** GPSDO reference. Sample timing is running off the stock")
        print("  *** clock, which is parts in 10^6.")
    if "true" in gps:
        print("  gps_locked true  - the FireFly has satellites, so time-of-day")
        print("                     is disciplined and the reference is good to")
        print("                     parts in 10^11.")
    elif gps:
        print("  *** gps_locked FALSE - no satellite lock. Usually no GPS")
        print("  *** antenna, or no sky view. The OCXO still beats the stock")
        print("  *** clock, but time-of-day is holdover and drifts. Since a")
        print("  *** millisecond is about 300 km of apparent range, fix this")
        print("  *** before trusting the delay axis of any ionogram.")
    return found


def measure(usrp, rate, freq, duration, spb, subdev_note=""):
    """Stream for `duration` seconds and count what went wrong."""
    result = {"rate": rate, "ok": False, "samples": 0, "achieved": 0.0,
              "overflow": 0, "timeout": 0, "late": 0, "other": 0, "error": None}

    try:
        usrp.set_rx_rate(rate, 0)
        actual = usrp.get_rx_rate()
        usrp.set_rx_freq(uhd.types.TuneRequest(freq), 0)

        stream_args = uhd.usrp.StreamArgs("fc32", "sc16")
        stream_args.channels = [0]
        streamer = usrp.get_rx_stream(stream_args)

        buf = np.empty((1, spb), dtype=np.complex64)
        metadata = uhd.types.RXMetadata()

        start = uhd.types.StreamCMD(uhd.types.StreamMode.start_cont)
        start.stream_now = True
        streamer.issue_stream_cmd(start)
    except Exception as exc:
        result["error"] = str(exc)
        return result

    # One throwaway buffer: the first one after start_cont carries the cost of
    # the stream spinning up and would otherwise be charged to the radio.
    try:
        streamer.recv(buf, metadata, 3.0)
    except Exception:
        pass

    total = 0
    began = time.time()
    try:
        while time.time() - began < duration:
            got = streamer.recv(buf, metadata, 1.0)
            code = str(metadata.error_code).lower()
            if "none" in code:
                total += got
            elif "overflow" in code:
                result["overflow"] += 1
                total += got
            elif "timeout" in code:
                result["timeout"] += 1
            elif "late" in code:
                result["late"] += 1
            else:
                result["other"] += 1
    except Exception as exc:
        result["error"] = str(exc)
    elapsed = time.time() - began

    try:
        streamer.issue_stream_cmd(
            uhd.types.StreamCMD(uhd.types.StreamMode.stop_cont))
        drain_until = time.time() + 0.5
        while time.time() < drain_until:
            if streamer.recv(buf, metadata, 0.1) == 0:
                break
    except Exception:
        pass

    result["samples"] = total
    result["achieved"] = total / elapsed if elapsed > 0 else 0.0
    result["configured"] = actual
    lost = result["overflow"] + result["timeout"] + result["late"] + result["other"]

    # Overflows are the authoritative signal: UHD says explicitly when it threw
    # data away. The achieved rate is only a backstop for the case where recv
    # returns short without flagging anything, so judge it loosely - a couple of
    # percent is the measurement, not the radio. Anything between the two
    # thresholds is worth printing but is not a failure.
    ratio = result["achieved"] / actual if actual > 0 else 0.0
    result["ratio"] = ratio
    result["shortfall"] = ratio < 0.90
    result["slightly_short"] = 0.90 <= ratio < 0.98
    result["ok"] = (lost == 0 and not result["shortfall"]
                    and result["error"] is None)
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--args", default="addr=192.168.10.3")
    ap.add_argument("--sensors", action="store_true",
                    help="read clock and GPS sensors, then stop")
    ap.add_argument("--rates", default="5e6,12.5e6,25e6")
    ap.add_argument("--duration", type=float, default=10.0)
    ap.add_argument("--freq", type=float, default=20e6,
                    help="centre frequency; 20 MHz matches the archive's cf")
    ap.add_argument("--subdev", default="A:A",
                    help="LFRX frontend A. Do not leave this to the default: "
                         "AB pairs RXA as I with RXB as Q")
    ap.add_argument("--spb", type=int, default=1 << 20)
    opts = ap.parse_args()

    try:
        usrp = uhd.usrp.MultiUSRP(opts.args)
    except Exception as exc:
        print("  could not open the device: %s" % exc)
        return 2

    if opts.subdev:
        try:
            usrp.set_rx_subdev_spec(uhd.usrp.SubdevSpec(opts.subdev))
        except Exception as exc:
            print("  could not set subdev %s: %s" % (opts.subdev, exc))

    if opts.sensors:
        show_sensors(usrp)
        return 0

    print("  subdev %s, centre %.3f MHz, %gs per step, %d samples per buffer"
          % (opts.subdev, opts.freq / 1e6, opts.duration, opts.spb))
    print()

    worst = 0
    for token in opts.rates.split(","):
        token = token.strip()
        if not token:
            continue
        rate = float(token)
        sys.stdout.write("  %-9s " % token)
        sys.stdout.flush()

        res = measure(usrp, rate, opts.freq, opts.duration, opts.spb)

        if res["error"] and res["samples"] == 0:
            print("FAILED: %s" % res["error"])
            worst = 2
            continue

        summary = ("%.3f MS/s achieved, %d samples"
                   % (res["achieved"] / 1e6, res["samples"]))
        if res["ok"]:
            note = ""
            if res.get("slightly_short"):
                note = ", %.1f%% of the configured rate" % (res["ratio"] * 100)
            print("clean   (%s%s)" % (summary, note))
        else:
            parts = []
            for key in ("overflow", "timeout", "late", "other"):
                if res[key]:
                    parts.append("%d %s" % (res[key], key))
            if res.get("shortfall"):
                parts.append("only %.1f%% of the %.3f MS/s configured"
                             % (res["ratio"] * 100, res["configured"] / 1e6))
            print("LOSSES  (%s; %s)" % (summary, ", ".join(parts) or "unknown"))
            worst = max(worst, 1)

    return worst


if __name__ == "__main__":
    sys.exit(main())
