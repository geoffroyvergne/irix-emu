#!/bin/sh
# Builds a profile-guided optimised emulator in build-pgo/ (about 13% faster
# than a plain Release build). It trains on your own machine image: an
# instrumented build boots IRIX unthrottled for a while, and the recorded
# profile then drives the final optimised build.
#
#   tools/build-pgo.sh <prom.bin> <disk image> [training seconds, default 120]
#
# The disk image is not modified: training runs on a copy (an instant clone
# on APFS). Uses clang's llvm-profdata (xcrun on macOS).
set -eu

if [ $# -lt 2 ]; then
    echo "usage: $0 <prom.bin> <disk image> [seconds]" >&2
    exit 2
fi
prom=$1
disk=$2
seconds=${3:-120}
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/indy-pgo.XXXXXX")
trap 'rm -rf "$work"' EXIT

profdata=$(command -v llvm-profdata 2>/dev/null || xcrun -f llvm-profdata 2>/dev/null || true)
if [ -z "$profdata" ]; then
    echo "llvm-profdata not found (install the LLVM / Xcode command line tools)" >&2
    exit 1
fi

echo "== 1/4 instrumented build (build-pgo-gen)"
cmake -S "$root" -B "$root/build-pgo-gen" -DCMAKE_BUILD_TYPE=Release -DINDY_BUILD_TESTS=OFF \
      -DINDY_PGO_GENERATE=ON -DINDY_PGO_PROFILE= > /dev/null
cmake --build "$root/build-pgo-gen" -j

echo "== 2/4 training: IRIX boots for $seconds s"
cp -c "$disk" "$work/disk" 2>/dev/null || cp "$disk" "$work/disk"
nvram="$root/indy_nvram.bin"
[ -f "$nvram" ] && cp "$nvram" "$work/nvram.bin"
LLVM_PROFILE_FILE="$work/indy-%p.profraw" \
    timeout -s INT "$seconds" "$root/build-pgo-gen/bin/indy" "$prom" --disk "$work/disk" \
    --nvram "$work/nvram.bin" --unthrottled < /dev/null > "$work/training.log" 2>&1 || true

echo "== 3/4 merging the profile"
"$profdata" merge -o "$root/indy.profdata" "$work"/*.profraw

echo "== 4/4 optimised build (build-pgo)"
cmake -S "$root" -B "$root/build-pgo" -DCMAKE_BUILD_TYPE=Release -DINDY_PGO_GENERATE=OFF \
      "-DINDY_PGO_PROFILE=$root/indy.profdata" > /dev/null
# A full rebuild: CMake does not track the profile as an input, and objects
# compiled against different profiles cannot be linked together.
cmake --build "$root/build-pgo" -j --clean-first
echo "Done: $root/build-pgo/bin/indy"
