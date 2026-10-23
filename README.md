# dsperate-lr

A [libretro](https://www.libretro.com/) core around
[DSperate](https://github.com/beebono/DSperate), the Nintendo DS emulator that
reimplements DraStic's techniques. DSperate is a submodule pinned to a release;
this repository adds only the libretro wrapper and its build.

Made for [plorpOS](https://github.com/thwonp/plorpOS) on the Anbernic RG Rotate,
where it runs 2.4-3.2x faster than melonDS DS on the same frames.

## What the core does

- Drives DSperate's core library like DSperate's own headless frontend: BIOS
  (`bios9.bin`, `bios7.bin`, `firmware.bin` in the system directory, else
  DSperate's FreeBIOS and a generated firmware), direct boot, JIT.
- Video: both screens stacked, 256x384 XRGB8888; the frontend lays them out.
- Audio at the SPU's real rate (32728.5 Hz).
- Input: joypad, and the libretro pointer on the bottom screen.
- Battery saves: the cart's save chip as `RETRO_MEMORY_SAVE_RAM`, a raw chip
  image (the same `.srm` other DS cores use).
- Save states (fixed size, length-prefixed), core option `dsperate_jit`.
- DSperate's thread pinning and real-time scheduling stay off: pacing and
  thread placement are the frontend's.

Not yet: microphone, DSi mode, cheats, local wireless.

## Building

    docker build -t dsperate-lr-cross .
    docker run --rm -v "$PWD:/w" -w /w dsperate-lr-cross ./build.sh /w/build

`build.sh` builds DSperate's core with DSperate's own CMake so the PGO profile
DSperate commits with each release applies (it was made with GCC 13.3, hence
the Ubuntu 24.04 image), then links the wrapper. The build prints how much of
the profile applied; expect 0 functions without theirs.

Fixes that are not in the pinned DSperate yet live in `patches/`; `build.sh`
applies them to the submodule first (see `patches/README.md`).

## Licence

GPL-3.0-or-later, as DSperate. `include/libretro.h` is the libretro API
header under its own MIT licence.
