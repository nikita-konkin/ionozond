#!/bin/bash
#
# Launch the console in a browser, on Linux. Builds the dev image on first use.
#
#   ./ionozond-gui.sh                       # the reconstruction, no archive
#   ./ionozond-gui.sh --data ~/captures
#   ./ionozond-gui.sh viewer --data ~/captures --capture cyprus1_...lfs
#   ./ionozond-gui.sh both --original ../dsChirp --data ~/captures
#
# Modes:
#   app        the reconstructed console            (default)
#   original   the shipped binary, for comparison   (needs --original)
#   both       both side by side                    (needs --original)
#   viewer     single-capture viewer                (needs --capture)
#
# Nothing is installed on the host but Docker. The application runs on a
# virtual display inside the container and is published over noVNC, so you
# drive it in an ordinary browser.
#
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE=ionozond-dev

MODE=app
DATA=""
CAPTURE=""
ORIGINAL=""
PORT=6080
GEOMETRY=1600x1000
MEMORY=6g
REBUILD=0

case "${1:-}" in
    app|original|both|viewer) MODE="$1"; shift ;;
esac

while [ $# -gt 0 ]; do
    case "$1" in
        --data)     DATA="${2:-}"; shift 2 ;;
        --capture)  CAPTURE="${2:-}"; shift 2 ;;
        --original) ORIGINAL="${2:-}"; shift 2 ;;
        --port)     PORT="${2:-}"; shift 2 ;;
        --geometry) GEOMETRY="${2:-}"; shift 2 ;;
        --memory)   MEMORY="${2:-}"; shift 2 ;;
        --rebuild)  REBUILD=1; shift ;;
        -h|--help)  awk 'NR>1 && /^#/{sub(/^# ?/,""); print; next}
                         NR>1{exit}' "$0"; exit 0 ;;
        *)          echo "unknown option: $1"; exit 2 ;;
    esac
done

# ---- checks that are better made here than by Docker ----------------------
# Argument checks come before touching Docker, so a typo is reported as a typo
# rather than behind a daemon error or a five-minute image build.
# A bind mount of a path that does not exist makes Docker silently create it,
# root-owned and empty, which looks like an archive with no captures in it.
if [ -n "$DATA" ]; then
    if [ ! -d "$DATA" ]; then
        echo "archive directory not found: $DATA"
        exit 1
    fi
    DATA="$(cd "$DATA" && pwd)"
fi

case "$MODE" in
    original|both)
        if [ -z "$ORIGINAL" ]; then
            echo "mode '$MODE' runs the shipped dsChirp binary, so it needs"
            echo "--original <dir>, a directory containing bin/dsChirp."
            echo "That binary is not part of this repository."
            exit 2
        fi
        if [ ! -x "$ORIGINAL/bin/dsChirp" ] && [ ! -f "$ORIGINAL/bin/dsChirp" ]; then
            echo "no bin/dsChirp under: $ORIGINAL"
            exit 1
        fi
        ORIGINAL="$(cd "$ORIGINAL" && pwd)"
        ;;
    viewer)
        if [ -z "$CAPTURE" ]; then
            echo "mode 'viewer' needs --capture <file.lfs>, named relative to"
            echo "the archive given with --data."
            exit 2
        fi
        if [ -z "$DATA" ]; then
            echo "mode 'viewer' needs --data <archive dir> as well."
            exit 2
        fi
        if [ ! -f "$DATA/$CAPTURE" ]; then
            echo "capture not found: $DATA/$CAPTURE"
            exit 1
        fi
        ;;
esac

# ---- docker, with or without sudo ----------------------------------------
DOCKER="docker"
if ! docker info >/dev/null 2>&1; then
    if sudo docker info >/dev/null 2>&1; then
        DOCKER="sudo docker"
        echo "note: using 'sudo docker'. To drop the sudo, add yourself to the"
        echo "      docker group:  sudo usermod -aG docker \$USER  (then re-login)"
    else
        echo "cannot talk to the Docker daemon. Is it running?"
        echo "    sudo systemctl status docker"
        exit 1
    fi
fi

# ---- is the port already taken? ------------------------------------------
# Almost always an earlier run of this same GUI still holding the port. Docker's
# own message names an endpoint id, which is no help in finding it.
BUSY=$($DOCKER ps --filter "publish=$PORT" --format '{{.ID}}  {{.Image}}  {{.Names}}' 2>/dev/null)
if [ -n "$BUSY" ]; then
    echo "port $PORT is already published by a running container:"
    printf '%s\n' "$BUSY" | sed 's/^/    /'
    echo
    echo "stop it:"
    echo "    $DOCKER stop $(printf '%s\n' "$BUSY" | awk '{print $1}' | tr '\n' ' ')"
    echo "or run this one somewhere else:"
    echo "    $0 $MODE --port $((PORT + 1))"
    exit 1
fi
if command -v ss >/dev/null 2>&1 && ss -ltn 2>/dev/null | grep -q ":$PORT "; then
    echo "port $PORT is in use, but not by a container. What is listening:"
    ss -ltnp 2>/dev/null | grep ":$PORT " | sed 's/^/    /'
    echo "    (run under sudo to see the process name)"
    echo "Pick another port:  $0 $MODE --port $((PORT + 1))"
    exit 1
fi

# ---- image ----------------------------------------------------------------
if [ "$REBUILD" = "1" ] || [ -z "$($DOCKER images -q "$IMAGE" 2>/dev/null)" ]; then
    echo "building the $IMAGE image (first run takes a few minutes) ..."
    if ! $DOCKER build -t "$IMAGE" -f "$REPO/docker/Dockerfile" "$REPO/docker"; then
        echo
        echo "build failed. It downloads Ubuntu packages, so it needs working"
        echo "internet. On a host where that only works through a proxy:"
        echo "    $DOCKER build --build-arg http_proxy=\"\$http_proxy\" \\"
        echo "        --build-arg https_proxy=\"\$https_proxy\" \\"
        echo "        -t $IMAGE -f $REPO/docker/Dockerfile $REPO/docker"
        exit 1
    fi
fi

# ---- run ------------------------------------------------------------------
MOUNTS=(-v "$REPO:/work/ionozond")
[ -n "$DATA" ]     && MOUNTS+=(-v "$DATA:/data:ro")
[ -n "$ORIGINAL" ] && MOUNTS+=(-v "$ORIGINAL:/work/dsChirp:ro")

ARGS=("$MODE")
[ "$MODE" = "viewer" ] && ARGS+=("/data/$CAPTURE")

echo
echo "  mode      $MODE"
echo "  archive   ${DATA:-<none mounted; the app will start with an empty archive>}"
echo "  browser   http://localhost:$PORT/vnc.html"
echo
echo "  Ready once the banner appears below. Ctrl-C here shuts it down."
echo

exec $DOCKER run --rm -it \
    -m "$MEMORY" \
    -p "${PORT}:6080" \
    "${MOUNTS[@]}" \
    -e "GEOMETRY=$GEOMETRY" \
    "$IMAGE" bash /work/ionozond/gui/run-gui.sh "${ARGS[@]}"
