# Patches onto the pinned DSperate

`build.sh` applies these to the `DSperate` submodule before it builds, so the
core carries them until upstream has the fix. Each one says what it fixes and
how it was checked. Drop a patch once the submodule moves past its fix.

- `0001-pgo-fingerprint-without-tune.patch` - keeps `DSPERATE_TUNE` out of
  the PGO fingerprint, which is written as the committed profile was made (an
  empty `tune=`), so a device's `-mcpu` build still uses the profile. `-mcpu`
  changes code generation, not the control flow GCC matches profiles on;
  `build.sh`'s "functions without their profile" count shows any that lost it.
