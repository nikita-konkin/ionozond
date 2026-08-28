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

## UHD on Ubuntu 24.04

```bash
sudo apt-get install -y uhd-host
sudo uhd_images_downloader                     # needs internet — use the WiFi
```

The N210's FPGA image must match the installed UHD version. `uhd_usrp_probe`
will say so if it does not:

```bash
uhd_usrp_probe --args="addr=192.168.10.2"
sudo uhd_image_loader --args="type=usrp2,addr=192.168.10.2"   # if it complains
```

Reflashing an N210 over Ethernet is safe to interrupt — it recovers — but let
it finish. This touches the radio, not the host's networking, so it cannot
affect your session.

## Performance, once it talks

Only after basic communication works. These are throughput fixes, not
connectivity fixes.

```bash
# jumbo frames on the USRP link (the N210 needs them for full rate)
sudo nmcli connection modify usrp-link 802-3-ethernet.mtu 8000
sudo nmcli connection up usrp-link

# socket buffers
sudo sysctl -w net.core.rmem_max=50000000
sudo sysctl -w net.core.wmem_max=50000000
```

Make the sysctls permanent in `/etc/sysctl.d/` only once they have proven
useful. Note some USB Ethernet adapters do not support an MTU above 1500; the
onboard NIC usually does.

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
