FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential \
      qtbase5-dev qtbase5-dev-tools qttools5-dev-tools \
      libqwt-qt5-dev \
      libfftw3-dev \
      xvfb x11-utils x11-apps imagemagick xdotool wmctrl \
      fonts-dejavu-core fonts-liberation \
      x11vnc novnc websockify openbox \
      python3-numpy python3-matplotlib \
      file binutils gdb less vim-tiny \
      ca-certificates \
 && rm -rf /var/lib/apt/lists/*

# The original dsChirp binary needs libqwt.so.6; Ubuntu 22.04 ships Qwt 6.1.4 as
# /usr/lib/libqwt-qt5.so.6. This symlink lets the reference binary run in this
# container as a verification oracle (see NOTES.md).
RUN ln -sf /usr/lib/libqwt-qt5.so.6 /usr/lib/libqwt.so.6 && ldconfig

# Qwt headers live in /usr/include/qwt on Debian/Ubuntu
ENV QWT_INCLUDE=/usr/include/qwt

# noVNC serves its client from here; websockify bridges it to x11vnc.
EXPOSE 6080

WORKDIR /work
