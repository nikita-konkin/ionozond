# host-setup

Bringing up a sounder host — network, USRP, drivers — on a machine you can only
reach remotely.

Written for **Ubuntu 24.04 with NetworkManager**. Every script here is either
read-only or arms its own rollback before it changes anything.

## The rule

The remote session rides on one interface. Almost every lockout is the same
failure: a new interface takes the **default route**, traffic reroutes through
a network that cannot reach the outside, and the session dies.

Neither wired interface needs a default route:

| interface | purpose | needs a default route? |
|---|---|---|
| WiFi | internet, remote session | **yes — leave it alone** |
| onboard Ethernet | direct link to the USRP | no, it is point-to-point |
| USB Ethernet | corporate LAN, storage | no, only a route to that subnet |

So: **WiFi stays the only default route.** Both wired profiles are created with
`ipv4.never-default yes`, which makes it impossible for them to install one
regardless of what the other end advertises.

The scripts also never touch the existing WiFi profile. They only *add*
profiles, so rolling back is deleting what was added.

## Order

### 1. Inventory — changes nothing

```bash
bash 00-inventory.sh
```

Run this first and read it before anything else. It reports the interfaces,
the default route, whether `192.168.10.0/24` collides with a corporate subnet,
the USB adapter's driver, and whether UHD is installed.

### 1a. Clear any stale profile for that interface

A host may already carry a leftover profile. Check the inventory output for one
whose `ipv4.method` is `auto` with `never-default:no` — that combination can
add a default route if anything on the link answers DHCP.

If it is not activated, deleting it changes nothing live:

```bash
nmcli connection delete USRP        # only if it shows as inactive
```

Leaving it in place means two profiles competing for the same interface.

### 2. USRP link — arms a rollback

```bash
sudo bash 10-usrp-link.sh enp3s0
sudo bash 10-usrp-link.sh enp3s0 192.168.20.1/24   # if .10 collides
```

Gives the USRP-facing interface a static address with no gateway, then pings
the radio and runs `uhd_find_devices`. It **refuses to run** against the
interface holding the default route, and refuses if the subnet is already
routed elsewhere.

A rollback timer is armed *before* the change. If you lose contact the profile
deletes itself and the machine returns to its current state. If all is well,
run the cancel command it prints.

An N210 ships on `192.168.10.2`. The host takes `.1` on the same subnet.

**Give yourself a longer window while debugging.** The default rollback is 7
minutes, which is right for a single confirmed change but short if you are
about to investigate:

```bash
sudo ROLLBACK_SECONDS=1800 bash 10-usrp-link.sh eno1
```

### 3. If the USRP does not answer

```bash
sudo bash 11-usrp-diagnose.sh eno1
```

Read-only. The question it exists to answer is what is actually on the far end
of the cable: a **silent** link is consistent with a powered, idle USRP, while
one carrying DHCP, mDNS or STP is a switch — meaning the cable is in the wrong
socket. It also arpings the radio, checks the neighbour table, and runs UHD
broadcast discovery if UHD is installed.

### 4. Host tuning — before any real capture

```bash
sudo DRY_RUN=1 bash 12-host-tuning.sh eno1     # see what it would do
sudo bash 12-host-tuning.sh eno1
```

Socket buffers, real-time priority for the receive thread, and optionally the
link MTU. Touches no routing, so it cannot affect a remote session. See
*The rate the sounder actually needs* below for why the defaults are not enough.

Note what Ubuntu's `uhd-host` leaves half-done: it installs
`/etc/security/limits.d/uhd.conf` granting `@usrp` an rtprio limit, but never
creates the `usrp` group, so the limit applies to nobody. The script creates
the group and adds you to it. **Both only take effect on a new login** — PAM
applies limits at session start, so `newgrp` is not enough. Until then, run
tests under `sudo`, where root is exempt.

### 5. Prove the receive path

```bash
sudo bash 13-usrp-rx-test.sh 192.168.10.3
```

Reads the clock and GPS sensors, then runs a receive rate ladder (5, 12.5,
25 MS/s) and counts overflows. `uhd_usrp_probe` only proves the radio answers
control packets; this proves it can actually deliver data at the rate the
sounder runs at.

It prefers UHD's C++ `benchmark_rate`, but **Ubuntu's `uhd-host` ships the
utilities without the examples**, so `benchmark_rate` is normally absent. It
then falls back to `rx_rate_test.py`, which measures the same thing through
`python3-uhd`:

```bash
sudo apt-get install -y python3-uhd
```

Overflows are the verdict — UHD states explicitly when it discarded data. The
achieved rate is printed as a cross-check, and only counts as a failure below
90% of the configured rate; a percent or two is the measurement, not the radio.

## UHD on Ubuntu 24.04

```bash
sudo apt-get install -y uhd-host tcpdump iputils-arping
```

Ubuntu 24.04 ships UHD 4.6.0, which still supports the N2xx family — discovery
reports them as `type: usrp2`.

### The images downloader is broken, and it does not matter

A bare `uhd_images_downloader` tries to fetch **every** image set and dies on
the first one missing from the server — an X410 FPGA image, nothing to do with
an N210. Narrowing it to the right target does not help, because the N210
images are gone from that cache too:

```
[ERROR] URL does not exist: https://files.ettus.com/binaries/cache/
        usrp2/fpga-6bea23d/usrp2_n210_fpga_default-g6bea23d.zip
```

Both failures are server-side: the manifest shipped with UHD 4.6 points at
paths Ettus no longer serves. Nothing on the host can fix it.

**It is almost certainly not needed.** UHD compares firmware and FPGA
compatibility numbers when it opens a device and refuses outright on a
mismatch. So a probe that *completes* is proof the images are already right —
you never have to reason about version numbers yourself.

If a future radio does need flashing, get `/usr/share/uhd/images` from a
working host, or from the images archive matching your UHD version, rather than
fighting the downloader.

### Probe before flashing

```bash
uhd_usrp_probe --args="addr=192.168.10.3"     # use the address discovery found
```

If the firmware or FPGA does not match the installed UHD, the probe says so
explicitly and refuses. Only then:

```bash
sudo uhd_image_loader --args="type=usrp2,addr=192.168.10.3"
```

### The radio may not be on the factory address

`uhd_find_devices` uses a UDP broadcast to port 49152, so it finds an N210
whatever address it holds. On the first host tested here the radio answered on
**192.168.10.3**, not the factory `192.168.10.2` — ping and arping to `.2`
found nothing at all while the radio was sitting there perfectly healthy.

There is no need to renumber it. Point the tooling at the address it has;
changing a USRP's IP means writing its EEPROM, which is a bigger risk than
using a different number.

Reflashing an N210 over Ethernet is safe to interrupt — it recovers — but let
it finish. This touches the radio, not the host's networking, so it cannot
affect your session.

## The radio on the first host

What `uhd_usrp_probe` reported, recorded because several of these details
change how the sounder must be configured:

| | |
|---|---|
| model | N210r4, hardware rev 2577 |
| serial | E4R24NCUP |
| address | 192.168.10.3 |
| firmware / FPGA | 12.4 / 11.1 — accepted by UHD 4.6, so no flashing |
| RX daughterboard | **LFRX** (0x000f), serial F61D56 |
| TX daughterboard | **LFTX** (0x000e), serial F5C310 |
| RX DSP tuning range | ±50 MHz (100 MHz master clock) |
| GPSDO | **Jackson-Labs FireFly, rev 0.929** — `ref_locked` and `gps_locked` both true |
| position | 56°37.33′ N, 47°53.10′ E (from its own GPGGA) |
| measured | **25.005 MS/s, zero overflows, at MTU 1500** |

Three things follow from that.

**LFRX is the right board and it has no gain.** Direct sampling, DC–30 MHz,
which covers the HF sounding band exactly. But `Gain Elements: None` — there is
no analog gain and no attenuator. Level is set entirely outside the radio, by
the antenna, any preamp, and any pad in front of it. The ADC's digital gain
scales numbers after conversion; it cannot recover a weak signal or protect
against a strong one.

**Pick the subdev explicitly: `A:A`.** LFRX presents four frontends — `A`, `B`,
and the complex pairings `AB` and `BA`. A single antenna on RXA means frontend
`A`: a real input, which the FPGA downconverter turns into proper complex
baseband. Left to itself UHD may select `AB`, which takes I from RXA and Q from
RXB — with nothing on RXB that halves the amplitude and mirrors the spectrum.
This belongs in the sounder's configuration, not just in test commands.

**The GPSDO is worth more here than in most applications.** The dechirp replica
has to track the transmitter's sweep, and absolute time decides where the delay
axis begins — a millisecond of timing error is about 300 km of apparent range.
A free-running TCXO is parts in 10⁶; a locked GPSDO is parts in 10¹¹. Note the
mainboard EEPROM says `gpsdo: none` while UHD detected the FireFly anyway and
switched the references to it: the EEPROM flag was simply never programmed.
Worth confirming `gps_locked` is true — that needs a GPS antenna with sky view,
and `13-usrp-rx-test.sh` reads it.

## The rate the sounder actually needs

The captures in the archive were taken at `sample_rate = 25000000` with
`dec = 625`, giving the 40 kS/s that lands in the `.lfs` file. The decimation
is **host-side**: the dechirp has to see the whole swept band, so the full rate
crosses the Ethernet link.

```
25e6 samples/s x 4 bytes (sc16 over the wire) = 100 MB/s = 800 Mbit/s
```

That is the N210's documented maximum over gigabit Ethernet — the sounder runs
the radio flat out, with no headroom. At that rate the stock settings do not
hold, which is what `uhd_usrp_probe` was complaining about:

- **receive socket stuck at 212992 bytes.** UHD asked for 50 MB. At 100 MB/s
  the default holds about 2 ms; one scheduler hiccup loses samples.
- **`error in pthread_setschedparam`.** UHD could not raise the receive
  thread's priority, so the kernel is free to preempt it mid-burst.
- **frame size 1472**, i.e. MTU 1500 — roughly 68000 packets per second.

`12-host-tuning.sh` fixes the first two and offers the third; `13-usrp-rx-test.sh`
measures whether it worked. Some USB Ethernet adapters cap the MTU at 1500; the
onboard NIC usually does not. 25 MS/s is reachable without jumbo frames, just
at a higher cost in CPU — which matters on this host, an i7-2760QM from 2011.

## Disk

A 250 s capture is ~80 MB, and at `rep=300` one station writes about **23 GB a
day**. On a 120 GB SSD that is under a week of raw data.

Generate `.lfp` sidecars as captures land — they are ~2000× smaller, and the
console reads them in preference. Keep sidecars indefinitely and rotate the raw
captures off to the storage server on the corporate LAN.

## The corporate proxy — last

Only needed if you want package installs or internet **over the wired
interface**. If WiFi already has internet, you may not need it at all for
bring-up.

When you do configure it:

- **Do not put the password in a file that could reach the repository.**
  `/etc/apt/apt.conf.d/95proxy` should be `chmod 600`, and it should never be
  inside a git working tree.
- Prefer a per-tool proxy over a system-wide one. A system-wide
  `http_proxy` in `/etc/environment` is picked up by things you did not intend
  — including, potentially, the remote-access client, which is exactly what you
  cannot afford to break.
- If the proxy needs your domain credentials, treat that as a reason to keep it
  narrow: `apt` only, set for a single command, rather than exported globally.

```bash
# one command only, nothing persisted
sudo http_proxy="http://USER:PASS@proxy.example:3128" \
     https_proxy="http://USER:PASS@proxy.example:3128" \
     apt-get update
```
