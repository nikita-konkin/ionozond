#!/usr/bin/env python3
"""Look at the HF band the receiver actually sees.

    sudo python3 rx_spectrum.py --args addr=192.168.10.3

Stage one of bringing up a sounder: before anything is dechirped, establish
that the antenna is connected and that the band looks like HF. A live HF
antenna shows the shortwave broadcast bands stacked up between 5 and 22 MHz
and 30-50 dB of structure. A disconnected input shows a flat, featureless
ADC noise floor with only a few dB of variation.

It draws the spectrum as text so it is readable over a remote session, and
writes a PNG only if asked.

Nothing is transmitted. This only receives.
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


# Shortwave broadcast bands, MHz. Their presence is the clearest sign that a
# real antenna is attached; they are strong, crowded and unmistakable.
BROADCAST_BANDS = [
    (3.90, 4.00, "75 m"), (5.90, 6.20, "49 m"), (7.20, 7.45, "41 m"),
    (9.40, 9.90, "31 m"), (11.60, 12.10, "25 m"), (13.57, 13.87, "22 m"),
    (15.10, 15.83, "19 m"), (17.48, 17.90, "16 m"), (21.45, 21.85, "13 m"),
]


def capture(usrp, rate, freq, seconds, spb=1 << 20):
    """Receive `seconds` of samples and return them as one array."""
    usrp.set_rx_rate(rate, 0)
    actual_rate = usrp.get_rx_rate()
    usrp.set_rx_freq(uhd.types.TuneRequest(freq), 0)
    actual_freq = usrp.get_rx_freq(0)

    stream_args = uhd.usrp.StreamArgs("fc32", "sc16")
    stream_args.channels = [0]
    streamer = usrp.get_rx_stream(stream_args)

    buf = np.empty((1, spb), dtype=np.complex64)
    metadata = uhd.types.RXMetadata()

    cmd = uhd.types.StreamCMD(uhd.types.StreamMode.start_cont)
    cmd.stream_now = True
    streamer.issue_stream_cmd(cmd)

    # Discard the first buffer: it carries the cost of the stream starting.
    try:
        streamer.recv(buf, metadata, 3.0)
    except Exception:
        pass

    wanted = int(actual_rate * seconds)
    chunks = []
    got_total = 0
    overflows = 0
    while got_total < wanted:
        got = streamer.recv(buf, metadata, 1.0)
        code = str(metadata.error_code).lower()
        if "overflow" in code:
            overflows += 1
        if got > 0:
            chunks.append(buf[0, :got].copy())
            got_total += got

    streamer.issue_stream_cmd(uhd.types.StreamCMD(uhd.types.StreamMode.stop_cont))
    drain_until = time.time() + 0.5
    while time.time() < drain_until:
        try:
            if streamer.recv(buf, metadata, 0.1) == 0:
                break
        except Exception:
            break

    return np.concatenate(chunks)[:wanted], actual_rate, actual_freq, overflows


def periodogram(samples, nfft):
    """Averaged power spectrum, fftshifted, in dB."""
    nseg = len(samples) // nfft
    if nseg < 1:
        raise ValueError("not enough samples for one %d-point FFT" % nfft)
    window = np.hanning(nfft).astype(np.float32)
    acc = np.zeros(nfft, dtype=np.float64)
    for i in range(nseg):
        seg = samples[i * nfft:(i + 1) * nfft] * window
        acc += np.abs(np.fft.fft(seg)) ** 2
    acc /= nseg
    acc = np.fft.fftshift(acc)
    acc[acc <= 0] = np.finfo(np.float64).tiny
    return 10.0 * np.log10(acc)


def band_label(lo_mhz, hi_mhz):
    """Label a row if it *overlaps* a broadcast band.

    Testing only the row's midpoint misses any band that straddles two rows --
    49 m at 5.90-6.20 falls between the rows centred on 5.75 and 6.25 and gets
    labelled on neither, which makes the display look like the bands are absent
    when they are merely split.
    """
    names = [name for lo, hi, name in BROADCAST_BANDS
             if lo < hi_mhz and hi > lo_mhz]
    return names[0] if names else ""


def draw(freqs_mhz, power_db, lo_mhz, hi_mhz, rows=56, width=58):
    """Text spectrum, frequency down the page so it reads in a terminal."""
    edges = np.linspace(lo_mhz, hi_mhz, rows + 1)
    floor = np.median(power_db)
    peak = np.percentile(power_db, 99.9)
    span = max(peak - floor, 1.0)

    print("  %-7s %-6s %s" % ("MHz", "band", "power (median .. 99.9th pct)"))
    for i in range(rows):
        sel = (freqs_mhz >= edges[i]) & (freqs_mhz < edges[i + 1])
        if not np.any(sel):
            continue
        value = np.percentile(power_db[sel], 90)
        filled = int(round(width * max(0.0, min(1.0, (value - floor) / span))))
        mid = 0.5 * (edges[i] + edges[i + 1])
        print("  %7.2f %-6s |%s%s" % (mid, band_label(edges[i], edges[i + 1]),
                                      "#" * filled, " " * (width - filled)))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--args", default="addr=192.168.10.3")
    ap.add_argument("--rate", type=float, default=25e6)
    ap.add_argument("--freq", type=float, default=12.5e6,
                    help="tuned centre; 12.5 MHz with a 25 MS/s span covers 0-25 MHz")
    ap.add_argument("--seconds", type=float, default=0.5)
    ap.add_argument("--nfft", type=int, default=8192)
    ap.add_argument("--subdev", default="A:A")
    ap.add_argument("--show-from", type=float, default=2.0, help="MHz")
    ap.add_argument("--show-to", type=float, default=30.0, help="MHz")
    ap.add_argument("--png", default="", help="also write a plot here")
    opts = ap.parse_args()

    try:
        usrp = uhd.usrp.MultiUSRP(opts.args)
    except Exception as exc:
        print("could not open the device: %s" % exc)
        return 2

    try:
        usrp.set_rx_subdev_spec(uhd.usrp.SubdevSpec(opts.subdev))
    except Exception as exc:
        print("could not set subdev %s: %s" % (opts.subdev, exc))

    print("capturing %.2f s at %.3f MS/s, centred on %.3f MHz ..."
          % (opts.seconds, opts.rate / 1e6, opts.freq / 1e6))
    try:
        samples, rate, freq, overflows = capture(usrp, opts.rate, opts.freq,
                                                 opts.seconds)
    except Exception as exc:
        print("capture failed: %s" % exc)
        return 2

    print("  %d samples, %d overflow%s"
          % (len(samples), overflows, "" if overflows == 1 else "s"))
    if overflows:
        print("  (overflows do not invalidate a spectrum -- it is an average)")

    # ---- level and clipping -----------------------------------------------
    # fc32 from UHD is the sc16 sample scaled to +-1.0, so |sample| approaching
    # 1 means the ADC is at full scale. LFRX has no attenuator, so the only
    # remedy is a pad or less antenna.
    peak = float(np.max(np.abs(samples)))
    rms = float(np.sqrt(np.mean(np.abs(samples) ** 2)))
    print()
    print("  peak |sample|  %.4f of full scale" % peak)
    print("  rms  |sample|  %.5f   (%.1f dB below full scale)"
          % (rms, 20 * np.log10(max(rms, 1e-12))))
    if peak > 0.95:
        print("  *** AT OR NEAR FULL SCALE -- the ADC is clipping. LFRX has no")
        print("  *** attenuator, so fit a pad ahead of the radio. Clipped input")
        print("  *** produces spurious lines all over the band.")
    elif peak < 0.002:
        print("  *** almost no signal. Consistent with nothing connected to RXA.")

    # ---- spectrum ----------------------------------------------------------
    power_db = periodogram(samples, opts.nfft)
    bins = np.fft.fftshift(np.fft.fftfreq(opts.nfft, 1.0 / rate))
    freqs_mhz = (freq + bins) / 1e6

    view = (freqs_mhz >= opts.show_from) & (freqs_mhz <= opts.show_to)
    if not np.any(view):
        print("nothing to show in %.1f..%.1f MHz" % (opts.show_from, opts.show_to))
        return 1

    print()
    draw(freqs_mhz[view], power_db[view], opts.show_from, opts.show_to)

    # ---- verdict -----------------------------------------------------------
    hf = power_db[view]
    floor = float(np.median(hf))
    strong = float(np.percentile(hf, 99.9))
    dynamic = strong - floor
    print()
    print("  noise floor (median)   %7.1f dB" % floor)
    print("  strongest (99.9th pct) %7.1f dB" % strong)
    print("  dynamic range          %7.1f dB" % dynamic)
    print()
    if dynamic > 15.0:
        print("  The band has real structure. An antenna is connected and HF")
        print("  signals are reaching the receiver. Check the bars above line up")
        print("  with the broadcast bands named beside them -- if they do, the")
        print("  frequency axis is right too.")
        rc = 0
    else:
        print("  *** Only %.1f dB between the floor and the strongest bin. That is" % dynamic)
        print("  *** a flat noise floor, not the HF band. Likely nothing is")
        print("  *** connected to RXA, or the feed is broken. LFRX has no gain,")
        print("  *** so a weak-but-present antenna still shows the broadcast")
        print("  *** bands clearly; flatness means no signal at all.")
        rc = 1

    if opts.png:
        try:
            import matplotlib
            matplotlib.use("Agg")
            import matplotlib.pyplot as plt
            plt.figure(figsize=(11, 5))
            plt.plot(freqs_mhz[view], hf, lw=0.5)
            for lo, hi, name in BROADCAST_BANDS:
                if hi >= opts.show_from and lo <= opts.show_to:
                    plt.axvspan(lo, hi, color="orange", alpha=0.20)
            plt.xlabel("frequency, MHz")
            plt.ylabel("power, dB")
            plt.title("%.2f s at %.3f MS/s, centre %.3f MHz"
                      % (opts.seconds, rate / 1e6, freq / 1e6))
            plt.grid(alpha=0.3)
            plt.tight_layout()
            plt.savefig(opts.png, dpi=110)
            print("\n  wrote %s" % opts.png)
        except ImportError:
            print("\n  matplotlib not installed, skipping the PNG")

    return rc


if __name__ == "__main__":
    sys.exit(main())
