# Building and deploying dsChirp

Two targets are covered:

- **Modern Ubuntu** (22.04 / 24.04) — Qt 5.15 + Qwt 6.1 from the distribution
- **Legacy Ubuntu 16.04** — the environment the original was built on, for
  installing onto an existing sounder machine without touching it

Both produce the same application. The reconstruction is Qt5 + Qwt6 + FFTW3
and has no other dependencies.

---

## 1. Modern Ubuntu (22.04 / 24.04)

### Dependencies

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    qtbase5-dev qtbase5-dev-tools qttools5-dev-tools \
    libqwt-qt5-dev \
    libfftw3-dev
```

That gives Qt 5.15.x and Qwt 6.1.4 — the versions this tree is developed
against.

### Build

```bash
cd dsChirp-src
mkdir -p build && cd build
qmake ../ionozond.pro
make -j"$(nproc)"
```

The binary lands at `build/dsChirp`. The capture viewer builds separately:

```bash
mkdir -p build-viewer && cd build-viewer
qmake ../viewer.pro
make -j"$(nproc)"
```

### Install

```bash
sudo install -m 755 build/dsChirp        /usr/local/bin/dsChirp
sudo install -m 755 build-viewer/dsChirp_viewer /usr/local/bin/dsChirp_viewer
```

First run creates `~/.config/dsChirp/config.ini` and `schedule.ini` from the
factory defaults compiled into the binary (extracted from the original — see
`res/ini/`).

### Note on the Qwt library name

Debian and Ubuntu ship Qwt as `libqwt-qt5.so`, which is what `ionozond.pro`
links (`-lqwt-qt5`). If you build Qwt from source it will usually install as
plain `libqwt.so`; in that case:

```bash
qmake ../ionozond.pro "LIBS -= -lqwt-qt5" "LIBS += -lqwt" "QWT_INCLUDE=/usr/local/qwt-6.1.4/include"
```

---

## 2. Legacy Ubuntu 16.04

16.04 is out of support, so its archive has moved to `old-releases`. Fix the
sources first or `apt-get update` will fail:

```bash
sudo sed -i -e 's|archive.ubuntu.com|old-releases.ubuntu.com|g' \
            -e 's|security.ubuntu.com|old-releases.ubuntu.com|g' \
            /etc/apt/sources.list
sudo apt-get update
```

### Dependencies

```bash
sudo apt-get install -y \
    build-essential \
    qtbase5-dev qtbase5-dev-tools qttools5-dev-tools \
    libqwt-qt5-dev \
    libfftw3-dev
```

16.04 provides Qt 5.5.1 and Qwt 6.1.2. Both are older than the development
target but the code stays inside their API:

- **C++11** only (`CONFIG += c++11`), which GCC 5.4 supports fully.
- The one construct that does *not* exist before Qt 5.14 is
  `Qt::SkipEmptyParts`, used in `src/qcpuusagewidget.cpp`. On Qt 5.5 use
  `QString::SkipEmptyParts` instead. The build below patches it automatically.

```bash
# Qt < 5.14 spells this differently
if [ "$(qmake -query QT_VERSION | cut -d. -f2)" -lt 14 ]; then
    sed -i 's/Qt::SkipEmptyParts/QString::SkipEmptyParts/g' src/qcpuusagewidget.cpp
fi
```

### Build

Identical to the modern case:

```bash
cd dsChirp-src
mkdir -p build && cd build
qmake ../ionozond.pro
make -j"$(nproc)"
```

### If the distribution Qt is too old

The original was built against a hand-installed Qt 5.9.1 under
`/home/ionouser/Qt5.9.1/5.9.1/gcc_64` — its `RPATH` still records this. To
reproduce that arrangement, install the Qt run-file into a prefix and build
with it:

```bash
export PATH=/opt/Qt5.9.1/5.9.1/gcc_64/bin:$PATH
qmake ../ionozond.pro
make -j"$(nproc)"
```

Qwt then has to be built against the same Qt:

```bash
tar xf qwt-6.1.4.tar.bz2 && cd qwt-6.1.4
/opt/Qt5.9.1/5.9.1/gcc_64/bin/qmake qwt.pro
make -j"$(nproc)" && sudo make install        # -> /usr/local/qwt-6.1.4
```

and the project pointed at it:

```bash
qmake ../ionozond.pro "QWT_INCLUDE=/usr/local/qwt-6.1.4/include" \
      "LIBS -= -lqwt-qt5" "LIBS += -L/usr/local/qwt-6.1.4/lib -lqwt"
```

If Qt is not installed system-wide, either set `LD_LIBRARY_PATH` at run time
or bake the path in:

```bash
qmake ../ionozond.pro "QMAKE_RPATHDIR += /opt/Qt5.9.1/5.9.1/gcc_64/lib"
```

---

## 3. Runtime requirements

The program itself only needs Qt5 Widgets, Qwt and FFTW3. The **sounding**
half additionally needs, on the machine that actually receives:

- GNU Radio with the `gr-juha` out-of-tree module (`reference/` holds the
  copies recovered from the operator's backup)
- `chirpsounder/chirp.py` and a writable path for the `chirp_config.py` that
  dsChirp generates
- a USRP and its UHD driver

None of that is needed to view an existing archive — the app and the viewer
read `.lfs` captures on their own.

### Paths to set on first run

Open **Параметры** (the gear button) and set:

| Field | Meaning |
|---|---|
| Программа зондирования | path to `chirp.py` |
| Конфигурация | path where `chirp_config.py` should be written |
| Каталог для ионограмм | archive root, holding `<yyyy.MM.dd>/` directories |

All three must already exist — the dialog refuses to accept otherwise, and
until it has been accepted once the main window does not know the paths.
(That is original behaviour, reproduced; see NOTES.md.)

The factory `config.ini` shipped inside the original writes `sound_app_dir`,
while every code path reads `sound_app`, so on a genuinely fresh install the
sounder path starts out empty and must be set here. Also original behaviour.

---

## 4. Verifying an install

```bash
# unit and differential tests (needs the captures mounted at /data)
bash tests/run_tests.sh

# render an ionogram from a capture, no GUI needed
./build-viewer/dsChirp_viewer /path/to/cyprus1_20191023_071510.lfs
```

A correct build renders an oblique ionogram with traces around 9 ms spanning
roughly 9–19 MHz for the cyprus1 → yoshkar-ola path.

---

## 5. Building in Docker instead

If you would rather not install anything, the container used during
development builds and runs both programs, and `gui/README.md` explains how to
drive them from a browser:

```bash
docker build -t dschirp-dev -f docker/Dockerfile docker
docker run --rm -v /path/to/project:/work -v /path/to/captures:/data:ro \
    dschirp-dev bash /work/ionozond/build.sh
```
