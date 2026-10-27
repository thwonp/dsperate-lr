#!/bin/sh
# Build dsperate_libretro.so for aarch64 Linux.
#
#   ./build.sh [OUT]          inside an environment with the cross toolchain
#   DSPERATE_TUNE=-mcpu=...   the device's CPU (patches/0002 keeps the profile)
#                              (Dockerfile: Ubuntu 24.04, GCC 13.3)
#
# Two configures. DSperate's own CMake builds its core library with the PGO
# profile it commits (pgo/aarch64, made with GCC 13.3 - the compiler its CI
# uses), in its own build tree, because the profile names each object by its
# path there. Then this project links the wrapper against that library.
#
# The build log says how much of the profile applied: "control flow of
# function ... does not match" is a function that lost its profile, "data file
# not found" an object with none. DSperate's CI prints the same two counts.
set -e
ROOT=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$ROOT/build}
mkdir -p "$OUT"

# Fixes not upstream yet, onto the pinned DSperate (patches/README.md); one
# already applied is left alone, so the step can run again.
for p in "$ROOT"/patches/*.patch; do
  [ -e "$p" ] || continue
  git -C "$ROOT/DSperate" apply --reverse --check "$p" 2>/dev/null && continue
  git -C "$ROOT/DSperate" apply "$p" && echo "patched: $(basename "$p")"
done

cmake -S "$ROOT/DSperate" -B "$OUT/dsperate" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/DSperate/cmake/aarch64-linux-gnu.cmake" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PROJECT_INCLUDE="$ROOT/cmake/dsperate-pic.cmake" \
  -DDSPERATE_PGO=use -DDSPERATE_PGO_STRICT=ON -DDSPERATE_TUNE="${DSPERATE_TUNE:-}" \
  -DDSPERATE_TESTS=OFF -DDSPERATE_HEADLESS=OFF -DDSPERATE_SDL=OFF \
  -DDSPERATE_CHEEVOS=OFF -DDSPERATE_NET=OFF > "$OUT/dsperate-configure.log"
cmake --build "$OUT/dsperate" --target dsperate_core 2>&1 \
  | grep -v "source locations for function" > "$OUT/dsperate-build.log" \
  || { tail -30 "$OUT/dsperate-build.log"; exit 1; }
echo "PGO: $(grep -c 'control flow of function' "$OUT/dsperate-build.log" || true) functions without their profile," \
     "$(grep -c 'data file not found' "$OUT/dsperate-build.log" || true) objects without one"

cmake -S "$ROOT" -B "$OUT/lr" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/DSperate/cmake/aarch64-linux-gnu.cmake" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDSPERATE_BUILD="$OUT/dsperate" > "$OUT/lr-configure.log"
cmake --build "$OUT/lr"
aarch64-linux-gnu-strip --strip-unneeded -o "$OUT/dsperate_libretro.so" "$OUT/lr/dsperate_libretro.so"
echo "$OUT/dsperate_libretro.so"
