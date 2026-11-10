# HiByOS patcher

HiByOS cannot redistribute HiBy's firmware. The patcher is a small program that users run
on their own computer to turn **their own copy** of the official HiBy R3 II (2025) firmware
v1.3 (`r3ii_2025.upt`, md5 `0d16d09c1458dd2f2cdaf38d0047917a`) into the HiByOS firmware.
It ships only HiByOS files (the `overlay/` folder) and the list of changes.

It needs no external tools, no WSL and no administrator/root rights: it is pure Python
(plus the pure-Python `pycdlib` library), packaged as a single `HiByOS-Patcher.exe` for
Windows. The same code runs with `python3` on macOS and Linux.

## Using it

- Windows: drag `r3ii_2025.upt` onto `HiByOS-Patcher.exe`, or double-click the .exe and
  drop the file into the window, or run `HiByOS-Patcher.exe C:\path\to\r3ii_2025.upt`.
- macOS/Linux: unzip `HiByOS-Patcher-<version>-python.zip` and run
  `python3 hibyos_patcher.py /path/to/r3ii_2025.upt` (pycdlib is bundled in `vendor/`).

The result is `HiByOS-<version>/r3ii_2025.upt`, next to the input file. The input is never
modified. Any other input file is refused (md5 check) and nothing is written.
Options: `--out-dir DIR` (another output folder), `--no-pause` (Windows: do not wait for
Enter at the end).

## How it works

`tools/hibyos_patcher.py`, using `tools/build.py` (the `.upt` code and the patch lists are
shared, not copied), `tools/squashfs.py` and `tools/lzo1x.py`:

1. Checks the md5 of the whole input file.
2. Opens the `.upt` (an ISO 9660 image) and rebuilds the kernel (`xImage`) and root file
   system (`rootfs.squashfs`) images from their 512 KiB chunks, checking every chunk's md5
   chain and the `img_md5`/`img_size` values of `ota_update.in` (same code as `build.py`).
3. Reads the SquashFS 4.0 (LZO) root file system in memory: superblock, id table,
   fragment table, inode and directory tables. Hard links are kept as shared inodes.
4. Applies exactly what `build.py` does:
   - the `overlay/` files (CRLF converted to LF for scripts, `.sh`, `.json`, `.ini`,
     `.view`); new files are 0755 if they start with `#!` or are ELF, else 0644; a
     replaced file keeps its original mode and owner; new folders are 0755 root:root;
   - `/etc/hibymod_build` with a random build id;
   - `TEXT_PATCHES` and `BIN_PATCHES` from `build.py`, with the same checks (text found
     exactly once, md5 and original bytes of binary files);
   - ownership as `build.py` ends up producing it (it unpacks in a user namespace and
     restores owners with a mksquashfs pseudo file): symlinks and `/` are root:root,
     symlinks are 0777, every other original entry keeps its owner and mode.
5. Writes a new SquashFS image (`tools/squashfs.py`), mksquashfs layout (data, inode table,
   directory table, fragment table, NFS export table, id table; no xattrs; 128 KiB blocks):
   - the compressed data blocks and fragment blocks of untouched files are **copied byte
     for byte** from HiBy's image (only relocated); HiBy's data is never recompressed;
   - new and patched files are compressed with a pure-Python LZO1X-1 compressor
     (`tools/lzo1x.py`); a block is stored uncompressed when compression does not help,
     as mksquashfs does. Identical new files share their data. Files smaller than a block
     go into new fragment blocks;
   - metadata blocks (8 KiB) are LZO compressed too.
6. Checks the size against the rootfs partition limit used by `build.py`
   (95 % of 0x2D00000 bytes).
7. Verifies the new image by reading it back: every entry's type, mode, owner, link target
   and hard links, and the full content of every file (all of HiByOS and HiBy's files are
   decompressed and compared by SHA-256 with the intended content).
8. Writes the `.upt` exactly like `build.py` (same chunking, `ota_update.in` changes only
   in the rootfs `img_md5`/`img_size`), to a temporary file, re-reads it like the recovery
   does (md5 chains, sizes, kernel unchanged, rootfs identical to the one built) and only
   then renames it to `r3ii_2025.upt`.

Taking about 10 seconds, it is faster than the Linux build because nothing of HiBy's
data is decompressed and recompressed except for the files being patched (and the
verification pass).

## Differences from the `build.py` image

The file tree is identical (same files, types, modes, owners, symlinks, hard links and
content; only the random build id differs). The image bytes differ: `build.py` recompresses
everything with mksquashfs (LZO1X-999), the patcher keeps HiBy's original compressed
blocks and uses LZO1X-1 for the new data. The patcher image is about 140 KB bigger
(for 0.6.1: 44,130,304 bytes, 98.4 % of the limit, vs 43,986,944 bytes, 98.1 %).
Data of files replaced by the overlay that lived in a shared fragment block stays in that
block (the block is still used by other files).

## Verification done for 0.6.1

- Reference image built with `tools/build_hibyos.sh` (WSL) and patcher image both
  extracted with `unsquashfs` (liblzo2): zero errors, same 4,830 entries, same type,
  mode, uid/gid, size and symlink target for each, same SHA-256 for all 4,095 files
  (except `/etc/hibymod_build`), same 208 hard link groups. `unsquashfs -s` reports the
  usual LZO, 128 KiB, exportable, no xattrs image.
- Both images mounted read-only with the Linux kernel squashfs driver (WSL2, Linux 6.6):
  identical `find` listing (type, mode, owner, link count, size, link target), identical
  `diff -r` and SHA-256 of every file read through the kernel; no kernel errors.
- `.upt` container: chunk md5 chains, `img_md5`/`img_size`, kernel image identical to
  HiBy's, `ota_config.in`/`ota_v0.ok` identical to the official ones.
- `tools/lzo1x.py` cross-checked against the real liblzo2 on ~3,000 inputs: liblzo2's
  `lzo1x_decompress_safe` decodes all of our streams, and we decode all of liblzo2's
  `lzo1x_1_compress` streams. The decompressor also unpacks all 84 MB of HiBy's rootfs.
- The Windows .exe, Windows Python 3.14 and Linux Python 3.12 (from the python zip)
  produce byte-identical `.upt` files for the same build id (`--build-id`, hidden test
  option) and `SOURCE_DATE_EPOCH` (fixes the ISO 9660 dates).

To repeat the comparison after changing the overlay or the patches, build both images and
compare their `unsquashfs -lln` listings and extracted files as above.

## Building the patcher

```
python tools/build_patcher.py
```

Creates (in `out/patcher/`, not tracked by git) a private venv with PyInstaller and
pycdlib if they are missing, then:

- `HiByOS-Patcher.exe` (Windows only) and `HiByOS-Patcher-<version>-windows.zip`;
- `HiByOS-Patcher-<version>-python.zip`: the patcher modules, `overlay/`, pycdlib in
  `vendor/` (LGPL-2.1, license included) and a README.

`<version>` comes from `overlay/usr/bin/hibymod` (`HiByOS 0.6.1` -> `0.6.1`).
