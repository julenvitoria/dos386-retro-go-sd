# FatFs — VENDORED (was a submodule until 2026-09-01)

Provenance: https://github.com/sylverb/FatFs.git, branch `main`, commit
`61014fc4ed1a2cfc3f3a6b24f510253c06ef63af`
("ffconf: drop LFN and exFAT — this instance only mounts FAT12 floppies").

## Why this is vendored and not a submodule

`git worktree add` does NOT initialize submodules. Every agent worktree got a
disconnected file-tree copy with no `.git`, so a change to `ffconf.h` made in a
worktree lived in no repository at all: it could not be committed, did not show
in `git status`, and vanished at merge without a conflict. Our build
configuration lives in `ffconf.h`, so that failure mode was reachable by design.
Vendoring makes this directory ordinary tracked source that a worktree can edit
and merge like any other file.

## What is ours, not upstream's

`ffconf.h` is TUNED FOR THIS INSTANCE and is not upstream's default:
`FF_USE_LFN 0`, `FF_FS_EXFAT 0`, `FF_FS_TINY 1`, `FF_USE_FASTSEEK 0`.

This instance mounts the FAT12 filesystem INSIDE a guest `.dsk`. The firmware
carries a SECOND, separate FatFs that reads the `.dsk` off the SD card
underneath it; `Makefile.core` renames every public entry point of ours to
`dsk_f_*` so the two do not collide in one binary. Their configs differ on
purpose — with LFN off, our `FILINFO` has NO `altname` field, so code walking
one must use `DOS_SHORT_NAME()`.

## Editing ffconf.h

Build flags are not dependencies, and `FF_FS_EXFAT` alone changes the width of
`LBA_t`. Any edit here requires `rm -f lib/FatFs/*.o build/core/*.o` and a
clean rebuild, or you link objects built against two different configs — which
presents as a hang and a mounted-but-empty directory, neither of which is real.

Upstreaming a fix: send it to the fork above, then re-vendor here.
