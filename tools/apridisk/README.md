# apridisk (Linux port)

Port of APRIDISK.EXE (Apricot floppy disk filing system, DOS build "Oct 7 2004"),
written from a disassembly of that binary. It reads Apricot / MS-DOS floppies
into `.DSK` image files ("ACT Apricot disk image" format) and writes them back.

    make            # builds ./apridisk (plain C, no dependencies)
    make test       # round-trip tests on raw image files, no hardware needed

## Use

    ./apridisk [-d DEVICE] [-n]            interactive menu, same keys as the original
    ./apridisk [-d DEVICE] read  FILE [COMMENT]
    ./apridisk [-d DEVICE] write FILE
    ./apridisk [-d DEVICE] info
    ./apridisk check FILE                  item listing (also written to apridisk.dbg)

* `DEVICE` defaults to `/dev/fd0`. A USB floppy (`/dev/sdX`) or a raw image file also works.
* `-n` (or `/n`) is the original's numbered-disk mode.
* `apridisk.ini` (directory, next disk number) is read from the current directory, as before.
  Default directory is `./disks`.
* Reading a real floppy needs read access to the device; writing needs write access and a
  disk that is **already formatted** (the original did not format either).
* Any key aborts a transfer. Up-arrow in the comment editor recalls the previous comment line.

## What is faithful, what is not

Faithful (traced in the disassembly): image layout and compression, sector addressing,
boot-sector classification, the disk-information printout, copy loops (whole-track read
with sector-by-sector fallback, unreadable sectors skipped and counted), prompts,
messages, filename rules (`APR00001.DSK`, `.DSK` default extension), ini handling.

Changed on purpose ("PORT:" comments in the source):
* INT 13h replaced by pread/pwrite plus the Linux floppy ioctls (geometry is set from the
  boot sector for `/dev/fdN`).
* The original's **E, L and M commands are empty stubs** in the 2004 build. Small working
  versions are provided here.
* `I` reports "unable to copy" when the format is unknown (the original printed garbage).
* Division by zero on a corrupt BPB is guarded; item sizes are sanity-checked.
* Creator item says "APRIDISK Linux port".

## Not verified

Tested only against raw image files and an independent parser of the format. It has not been
run against a physical Apricot floppy, and images were not cross-checked against files made by
the original EXE.
