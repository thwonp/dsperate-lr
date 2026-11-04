# dsperate-lr

A [libretro](https://www.libretro.com/) core around
[DSperate](https://github.com/beebono/DSperate), the Nintendo DS emulator that
reimplements DraStic's techniques. DSperate is a submodule pinned to a release (v3.3.0);
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
- Save states (fixed size, length-prefixed).
- Slot 2 (DSperate v3.2.0): a GBA game for the dual-slot DS games (Pokemon, Kirby
  Canvas Curse, ...), the Rumble Pak, and the Guitar Hero Guitar Grip.
- Analog (CPAD) patches (DSperate v3.1.0): the built-in catalogue's patch for the
  loaded game, fed both sticks and ZL/ZR by the frontend.
- DSperate's thread pinning and real-time scheduling stay off: pacing and
  thread placement are the frontend's.

## Core options

| option | values | what |
|---|---|---|
| `dsperate_jit` | enabled, disabled | JIT recompiler |
| `dsperate_frameskip` | auto, disabled | frame skip when the frontend asks (below) |
| `dsperate_slot2` | auto, rumble pak, empty | slot 2 when no GBA game is chosen: `auto` is a Rumble Pak if the frontend has a rumble motor |
| `dsperate_gba_game` | none, then the GBA games the DS game accepts | the list is rebuilt when a game loads, from `$DSPERATE_GBA_DIR` or the `Game Boy Advance` folder beside the DS one, filtered by DSperate's own dual-slot table. A GBA game's save is `<save dir>/<name>.sav` |
| `dsperate_guitar_grip` | auto, off | the Grip goes in by itself for the Guitar Hero games; its frets are L, L2, R2, R |
| `dsperate_solar` | 0-10 | Boktai's light level |
| `dsperate_analog` | off, auto | feed the sticks to the game's CPAD patch. Off by default: on a handheld whose stick is also the pen cursor both would move |

A frontend only accepts a stored value the core offers, so `dsperate_gba_game`
is declared again after the DS game is known and before its value is read.

## Frame skip - read this before tuning audio

DSperate has a frame skip of its own: a skipped frame leaves out the 3D raster
and line output **only**. The game, both CPUs and the sound chip run exactly as
on a drawn frame, so a skip never changes the audio, only what is on screen
(the previous frame is shown again). That is the lever for a frontend that
runs late on heavy 3D scenes. The three pieces:

1. **The frontend decides** when it will be late and says so through
   libretro's audio buffer status callback (`underrun_likely`). plorpOS's
   Diatom does it on the Brick and H700 when a frame is more than 8 ms behind
   (`DIATOM_FRAMESKIP_MS`); on its other ports it never does, so nothing skips.
2. **This wrapper** (`dsperate_frameskip = auto`) turns the hint into
   `gpu.set_frame_skip()` for whole display periods, at most two in a row, then
   one frame is drawn, and returns `video_cb(nullptr)` for skipped frames.
3. **DSperate** decides one frame ahead, so a skip asked for now lands on the
   next frame.

The design is [slotPlus](https://github.com/nelynes/slotPlus)'s
(`cores/dsperate/wrapper-0001-frameskip.patch`, suggested by crobledo); plorpOS
carried an earlier simpler version as a TortOS patch before folding this in.
If frames still run late with `auto`, the cause is the frontend's lateness
estimate, not the audio: look at the frontend's skip rule first.

Not yet: DSi mode, cheats, local wireless. The microphone is the frontend's (libretro microphone interface), opened the first time a game listens.

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
