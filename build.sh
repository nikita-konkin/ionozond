#!/bin/bash
# Build every target.
#   docker run --rm -m 6g -v N:\ds_shirp_revers_eng\ionozond:/work/ionozond \
#       ionozond-dev bash /work/ionozond/build.sh
set -u
SRC=/work/ionozond
rc=0

for target in ionozond viewer lfp_build; do
    BUILD="/tmp/build-$target"
    mkdir -p "$BUILD"
    cd "$BUILD"
    if ! qmake "$SRC/$target.pro" >/dev/null 2>&1; then
        echo "  $target: qmake FAILED"; rc=1; continue
    fi
    if ! make -j"$(nproc)" >"/tmp/make-$target.log" 2>&1; then
        echo "  $target: BUILD FAILED"
        grep -E 'error:' "/tmp/make-$target.log" | head -8
        rc=1; continue
    fi
    binary=$(find "$BUILD" -maxdepth 1 -type f -perm -u+x ! -name '*.o' ! -name 'Makefile' | head -1)
    printf "  %-12s ok  %s\n" "$target" "$(ls -la "$binary" | awk '{print $5" bytes"}')"
done

exit $rc
