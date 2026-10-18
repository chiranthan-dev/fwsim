# disk-sim (vendored snapshot)

A snapshot of the **library** sources of disk-sim, the hard-disk mechanics
simulator that fwsim uses as its media layer: `include/disksim.h` (the public
API) and `src/` (everything except the disk-sim CLI's `main.c`).

fwsim's Makefile uses a sibling checkout (`../disk-sim` or
`../Projects/disk-sim`) when one exists, and falls back to this copy, so that
a fresh clone of fwsim builds without anything else. fwsim uses only the
public API from `disksim.h`; nothing here was modified.
