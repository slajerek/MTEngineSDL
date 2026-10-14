#!/usr/bin/env python3
"""Make libultrahdr's raw (planar YCbCr) JPEG paths safe on IJG libjpeg 9.

Since 2026-10-07 the bundle builds libultrahdr against libjpeg-turbo
(JPEG_LIB_VERSION 62), where the decoder flag changes nothing and the encoder
edit is compiled out (#if >= 70); kept so a jpeg-9 build stays safe.

WHY THIS EXISTS
libultrahdr is written against libjpeg-turbo. We built it against the engine's
own jpeg-9a until 2026-10-07 (one jpeg_* ABI per process; see build-image_codecs.sh).
IJG libjpeg 7+ has "DCT scaling": with do_fancy_upsampling left at its default
TRUE, jpeg_start_decompress scales 2x-subsampled chroma UP inside the IDCT
(jdmaster.c, the `ssize` loop: 8 * 1 <= DCTSIZE), so a 4:2:0 component gets a
16x16 IDCT and a downsampled_width equal to the FULL image width. In
raw_data_out mode that full-size chroma is handed straight to the caller.
libjpeg-turbo never does this (its loop is `ssize < DCTSIZE`).

libultrahdr's JpegDecoderHelper::decodeToCSYCbCr sizes its chroma planes and
row-pointer arrays for HALF-size chroma, 8 rows per v_samp per call -- so every
4:2:0 decode wrote 16 rows of double-width chroma through 8 row pointers,
the other 8 uninitialised stack values. On every iPhone gain-map JPEG it
corrupted memory, and it crashed a host app in jpeg_idct_16x16 while browsing
(2026-10-06). The encoder decodes a compressed SDR base the same way, and its
own raw_data_in path has the mirror case (do_fancy_downsampling scales chroma
DOWN inside the DCT and then reads full-size rows the caller never provided).

Turning the "fancy" flag off makes libjpeg 9 use plain DCTSIZE blocks for every
component in raw mode, which is exactly the contract libultrahdr assumes. It
changes nothing on libjpeg-turbo (fancy upsampling never feeds raw output there).

WHY A SCRIPT AND NOT A .patch
Same reason as apply_libraw_jxl.py: each edit asserts its anchor matches
EXACTLY ONCE and aborts with a readable message otherwise, so a libultrahdr
bump fails loudly at build time instead of applying somewhere plausible.
Idempotent: re-running on a patched tree is a no-op.

LICENSING
libultrahdr is Apache-2.0; section 4(b) asks that modified files carry a
prominent notice, which the inserted lines do.
"""

import io
import os
import sys

MARKER = "MTEngineSDL: IJG libjpeg 9 raw-mode DCT scaling off"

EDITS = [
    (
        "lib/src/jpegdecoderhelper.cpp",
        "      cinfo.raw_data_out = TRUE;\n",
        "      cinfo.raw_data_out = TRUE;\n"
        "      // " + MARKER + " (modified by the MTEngineSDL project, 2026-10-06):\n"
        "      // libjpeg 7+ otherwise upsamples 4:2:0 chroma inside a 16x16 IDCT and\n"
        "      // hands FULL-size chroma to the half-size planes below.\n"
        "      cinfo.do_fancy_upsampling = FALSE;\n",
    ),
    (
        "lib/src/jpegencoderhelper.cpp",
        "    if (format != UHDR_IMG_FMT_24bppRGB888) cinfo.raw_data_in = TRUE;\n",
        "    if (format != UHDR_IMG_FMT_24bppRGB888) cinfo.raw_data_in = TRUE;\n"
        "#if JPEG_LIB_VERSION >= 70\n"
        "    // " + MARKER + " (modified by the MTEngineSDL project, 2026-10-06):\n"
        "    // libjpeg 7+ otherwise downsamples chroma inside the DCT and reads\n"
        "    // FULL-size raw chroma rows the caller never provided.\n"
        "    cinfo.do_fancy_downsampling = FALSE;\n"
        "#endif\n",
    ),
]


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: apply_libultrahdr_jpeg9_raw.py <libultrahdr source dir>")
    root = sys.argv[1]
    for rel, anchor, replacement in EDITS:
        path = os.path.join(root, rel)
        with io.open(path, "r", encoding="utf-8", newline="") as f:
            text = f.read()
        if MARKER in text:
            continue
        n = text.count(anchor)
        if n != 1:
            sys.exit("apply_libultrahdr_jpeg9_raw.py: anchor %r found %d times in %s "
                     "(expected exactly 1) -- a libultrahdr upgrade moved it; "
                     "re-check the raw-mode DCT scaling fix by hand" % (anchor.strip(), n, path))
        text = text.replace(anchor, replacement)
        with io.open(path, "w", encoding="utf-8", newline="") as f:
            f.write(text)
        print("patched %s" % rel)
    return 0


if __name__ == "__main__":
    sys.exit(main())
