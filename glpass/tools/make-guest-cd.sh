#!/bin/sh
# Packs the IRIX-side sources (glpass/guest) into a CD image the emulator can
# insert: a plain tar archive, with everything under glpass/, padded to
# 2048-byte sectors. Inside IRIX, as root:
#   mkdir -p /usr/local/irix-emu && cd /usr/local/irix-emu
#   dd if=/dev/rdsk/dks0d4vol bs=2048 | tar xf -
#
#   glpass/tools/make-guest-cd.sh [output, default build/guest-cd.img]
set -eu
glpass=$(cd "$(dirname "$0")/.." && pwd)
root=$(cd "$glpass/.." && pwd)
out=${1:-$root/build/guest-cd.img}
mkdir -p "$(dirname "$out")"
# COPYFILE_DISABLE: no macOS ._ resource files in the archive. -s renames
# guest/ to glpass/ inside it, the directory IRIX builds in.
(cd "$glpass" && COPYFILE_DISABLE=1 tar --format ustar -s ',^guest,glpass,' -cf "$out" \
    --exclude '*.o' --exclude 'libGL.so' --exclude 'libglpass_x.so' --exclude 'gltri' --exclude 'glthreads' --exclude 'glthreads_pt' --exclude 'glmove' --exclude 'glcheck' --exclude 'glgone' guest)
size=$(wc -c < "$out")
pad=$(( (2048 - size % 2048) % 2048 + 2048 * 16 ))  # plus some empty sectors
dd if=/dev/zero bs=1 count=$pad 2>/dev/null >> "$out"
echo "$out ($(wc -c < "$out") bytes)"
