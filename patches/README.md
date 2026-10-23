# Patches onto the pinned DSperate

`build.sh` applies these to the `DSperate` submodule before it builds, so the
core carries them until upstream has the fix. Each one says what it fixes and
how it was checked. Drop a patch once the submodule moves past its fix.

- `0001-gpu3d-clear-swap_busy_until_-on-state-load.patch` - an in-place
  `load_state` (libretro unserialize: rewind, quick load, the in-game menu)
  kept the geometry engine's post-SWAP busy deadline from the timeline the
  machine ran on after the state was made, so GXSTAT read busy for up to as
  long as it had run ahead. Metroid Prime Hunters' 3D froze and the run
  diverged from frame 4; a fresh machine loading the same state did not.
  Checked with a no-JIT x86 build: load in place after 60 frames vs a fresh
  load, 120 frames on - Metroid Prime Hunters 117 differing frames before,
  0 after; Advance Wars - Dual Strike and Pokemon Pearl 0 both.
