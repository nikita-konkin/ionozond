# Running the programs yourself

Everything runs inside the Linux container and is displayed in your browser
over noVNC — nothing to install on Windows beyond Docker Desktop, no X server.

## Quickest path

```powershell
cd N:\ds_shirp_revers_eng\dsChirp-src\gui
.\dschirp-gui.ps1
```

It builds, starts a virtual display, and opens
`http://localhost:6080/vnc.html` in your browser. Press **Connect** if the page
doesn't connect on its own. Ctrl-C in the PowerShell window shuts it down.

## The four variants

| Command | What you get |
|---|---|
| `.\dschirp-gui.ps1` | the reconstruction (default) |
| `.\dschirp-gui.ps1 -Mode original` | the **shipped binary**, for side-by-side comparison |
| `.\dschirp-gui.ps1 -Mode both` | both at once on one desktop — drag the windows apart |
| `.\dschirp-gui.ps1 -Mode viewer -Capture cyprus1_20191023_071510.lfs` | standalone capture viewer |

`-Mode both` is the useful one for checking fidelity: openbox is running, so
you can move and resize either window and put them next to each other.

## Pointing it at your data

```powershell
.\dschirp-gui.ps1 -Data F:\MyData\ND\lfs\ionozond_data2
.\dschirp-gui.ps1 -Data F:\MyData\ND\lfs -Geometry 1920x1080
```

The directory is mounted read-only at `/data`, so nothing can modify your
archive. Captures are found in either layout:

- `<data>/<yyyy.MM.dd>/<station>_<date>_<time>.lfs` — the app's own layout
- `<data>/<station>_<date>_<time>.lfs` — loose files, which the launcher
  symlinks into the dated layout automatically

## The viewer

`-Mode viewer` opens a single window with just the ionogram panel:

- **Открыть...** — pick any `.lfs`; every capture in the same directory is
  loaded into the dropdown
- **<<** / **>>** — step through them
- **Цвета** — switch between the nine recovered colour maps (index 1 is what
  the shipped config uses)

The status bar shows the frequency sweep and delay window read from the
capture header.

## No browser? Use a native X server instead

If you prefer VcXsrv or X410, start it with access control disabled and run:

```powershell
docker run --rm -it -e DISPLAY=host.docker.internal:0 `
    -v N:\ds_shirp_revers_eng:/work -v F:\MyData\ND\lfs:/data:ro `
    dschirp-dev bash -lc "cd /tmp && qmake /work/ionozond/ionozond.pro && make -j8 && ./dsChirp"
```

## Headless, no GUI at all

Render an ionogram straight to a PNG:

```powershell
docker run --rm -m 6g -v N:\ds_shirp_revers_eng:/work -v F:\MyData\ND\lfs:/data:ro dschirp-dev bash /work/ionozond/tools/render.sh
```

Run the whole test suite:

```powershell
docker run --rm -v N:\ds_shirp_revers_eng:/work -v F:\MyData\ND\lfs:/data:ro dschirp-dev bash /work/ionozond/tests/run_tests.sh
```

## Troubleshooting

**The browser page is blank or won't connect.** The build runs first and takes
30–60 s on a cold container; wait for the `Open http://localhost:6080/...`
banner in the PowerShell window, then reload.

**Port 6080 is busy.** `.\dschirp-gui.ps1 -Port 6081`.

**Drive not shared.** Docker Desktop → Settings → Resources → File sharing, add
`N:` and `F:`.

**The window is bigger than the browser view.** The noVNC page is opened with
`resize=scale`; use its left-hand toolbar to change the scaling mode, or start
with a smaller `-Geometry`.
