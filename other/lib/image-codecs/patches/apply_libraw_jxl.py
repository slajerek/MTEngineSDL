#!/usr/bin/env python3
"""Add a native JPEG XL DNG decoder (Compression 52546) to a LibRaw checkout.

WHY THIS EXISTS
LibRaw ships jxl_dng_load_raw_placeholder(), whose body is
`throw LIBRAW_EXCEPTION_UNSUPPORTED_FORMAT;` with the comment "real decoding
implemented in DNG SDK" -- and dngsdk_glue.cpp routes Compression 52546 to the
Adobe DNG SDK "regardless of flags or use_dngsdk value". We do not ship the
Adobe DNG SDK (a bespoke non-OSI agreement that also drags in the XMP Toolkit),
so a DNG produced by Adobe DNG Converter's "Lossy" option under DNG 1.7+
compatibility could not be opened at all. See
PhotoCruise/specs/superpowers/specs/2026-08-19-jpegxl-dng-*.md.

WHY A SCRIPT AND NOT A .patch
A context diff against a specific LibRaw tarball rots silently: a version bump
either fails with reject hunks or, worse, applies somewhere plausible. Every
edit here asserts its anchor matches EXACTLY ONCE and aborts with a readable
message otherwise, so a LibRaw upgrade fails loudly at build time and points at
the anchor that moved. Idempotent: re-running on a patched tree is a no-op.

LICENSING
LibRaw is distributed by PhotoCruise under its CDDL 1.0 option. CDDL 3.2 makes
this modification itself CDDL; 3.3 requires each modified file to carry a
notice identifying the contributor (the inserted code does); 3.1 requires the
modified source to be available, which this script and the built tree satisfy.
"""

import io
import os
import sys

MARKER = "MTEngineSDL JPEG XL DNG decoder"

DECODER = r'''
// ---------------------------------------------------------------------------
// MTEngineSDL JPEG XL DNG decoder (Compression 52546)
//
// MODIFICATION by the MTEngineSDL / PhotoCruise project, 2026-08-19, added
// under LibRaw's CDDL 1.0 option (CDDL 3.3 contributor notice).
//
// Modelled line for line on lossy_dng_load_raw() below, which is LibRaw's own
// native loader for Compression 34892 -- the same shape in every respect
// except the tile codec: LinearRaw, tiled, 3 components, OpcodeList2 carrying
// MapPolynomial. The opcode-list parsing here is deliberately identical to
// that function's, including the big-endian read and the field skips, so the
// two cannot drift.
//
// DNG JXL tiles are BARE JXL CODESTREAMS -- each begins FF 0A and is a
// complete, self-contained image -- so each tile is handed to libjxl as-is.
//
// THE ONE DELIBERATE DIFFERENCE: the 34892 path is 8-bit and builds a
// 256-entry curve indexed by the JPEG sample. JXL DNG tiles are 16-bit, so the
// polynomial is evaluated over the full 16-bit range here. Reusing a 256-entry
// curve would quantise the image to 256 levels per channel and throw away
// exactly what the 16-bit format was chosen for.
#ifdef USE_JXL

void LibRaw::jxl_dng_load_raw()
{
  if (!image)
    throw LIBRAW_EXCEPTION_IO_CORRUPT;

  unsigned sorder = order, ntags, opcode, deg, i, j, c;
  unsigned trow = 0, tcol = 0, row, col;
  double coeff[9], tot;

  // Per-channel 16-bit transfer curves from OpcodeList2's MapPolynomial
  // opcodes. 64 Ki entries x 4 channels x 2 bytes = 512 KB, heap not stack.
  std::vector<ushort> curve16((size_t)4 * 65536);
  for (c = 0; c < 4; c++)
    for (i = 0; i < 65536; i++)
      curve16[(size_t)c * 65536 + i] = (ushort)i;

  if (meta_offset)
  {
    fseek(ifp, meta_offset, SEEK_SET);
    order = 0x4d4d;   // opcode lists are big-endian regardless of TIFF order
    ntags = get4();
    while (ntags--)
    {
      opcode = get4();
      get4();
      get4();
      if (opcode != 8)
      {
        fseek(ifp, get4(), SEEK_CUR);
        continue;
      }
      fseek(ifp, 20, SEEK_CUR);        // payload size + top/left/bottom/right
      if ((c = get4()) > 3)            // plane
        break;
      fseek(ifp, 12, SEEK_CUR);        // planes, rowPitch, colPitch
      if ((deg = get4()) > 8)          // degree
        break;
      for (i = 0; i <= deg && i < 9; i++)
        coeff[i] = getreal(LIBRAW_EXIFTAG_TYPE_DOUBLE);
      for (i = 0; i < 65536; i++)
      {
        for (tot = j = 0; j <= deg; j++)
          tot += coeff[j] * pow(i / 65535.0, (int)j);
        if (tot < 0.0) tot = 0.0;
        if (tot > 1.0) tot = 1.0;
        curve16[(size_t)c * 65536 + i] = (ushort)(tot * 0xffff);
      }
    }
    order = sorder;
  }

  // Tile offsets: data_offset is the FILE POSITION OF THE OFFSETS TABLE for a
  // tiled IFD (tiff.cpp case 0x0144 stores ftell, not a data pointer), which is
  // why lossy_dng_load_raw walks it with successive get4()s. Read the whole
  // table up front so a tile's SIZE can be taken as the gap to the next one:
  // libjxl needs a bounded buffer, whereas libjpeg self-terminates. Overshoot
  // is harmless -- the decoder stops at the end of the codestream and ignores
  // trailing bytes -- so the last tile simply runs to end of file.
  const unsigned tiles_across = (raw_width + tile_width - 1) / tile_width;
  const unsigned tiles_down = (raw_height + tile_length - 1) / tile_length;
  const size_t tile_count = (size_t)tiles_across * tiles_down;
  if (!tile_count || tile_count > 1000000)
    throw LIBRAW_EXCEPTION_IO_CORRUPT;

  std::vector<INT64> tile_offset(tile_count, 0);
  fseek(ifp, data_offset, SEEK_SET);
  for (size_t t = 0; t < tile_count; t++)
    tile_offset[t] = (INT64)get4();

  INT64 file_size = libraw_internal_data.internal_data.input->size();
  std::vector<uint8_t> tile_data;
  std::vector<uint16_t> pixels;

  JxlDecoder *dec = JxlDecoderCreate(NULL);
  if (!dec)
    throw LIBRAW_EXCEPTION_ALLOC;

  try
  {
    for (size_t t = 0; t < tile_count; t++)
    {
      checkCancel();

      INT64 begin = tile_offset[t];
      INT64 end = (t + 1 < tile_count && tile_offset[t + 1] > begin)
                      ? tile_offset[t + 1] : file_size;
      if (begin <= 0 || begin >= file_size || end <= begin)
        throw LIBRAW_EXCEPTION_IO_CORRUPT;
      size_t avail = (size_t)(end - begin);

      tile_data.resize(avail);
      fseek(ifp, begin, SEEK_SET);
      if (fread(tile_data.data(), 1, avail, ifp) != avail)
        throw LIBRAW_EXCEPTION_IO_EOF;

      JxlDecoderReset(dec);
      if (JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE)
          != JXL_DEC_SUCCESS)
        throw LIBRAW_EXCEPTION_DECODE_RAW;
      if (JxlDecoderSetInput(dec, tile_data.data(), tile_data.size()) != JXL_DEC_SUCCESS)
        throw LIBRAW_EXCEPTION_DECODE_RAW;
      JxlDecoderCloseInput(dec);

      JxlBasicInfo info;
      memset(&info, 0, sizeof(info));
      JxlPixelFormat fmt;
      memset(&fmt, 0, sizeof(fmt));
      bool got_image = false;

      for (;;)
      {
        JxlDecoderStatus st = JxlDecoderProcessInput(dec);
        if (st == JXL_DEC_ERROR || st == JXL_DEC_NEED_MORE_INPUT)
          throw LIBRAW_EXCEPTION_DECODE_RAW;
        if (st == JXL_DEC_BASIC_INFO)
        {
          if (JxlDecoderGetBasicInfo(dec, &info) != JXL_DEC_SUCCESS)
            throw LIBRAW_EXCEPTION_DECODE_RAW;
          if (info.num_color_channels < 1 || info.num_color_channels > 4)
            throw LIBRAW_EXCEPTION_DECODE_RAW;
          fmt.num_channels = info.num_color_channels;
          fmt.data_type = JXL_TYPE_UINT16;
          fmt.endianness = JXL_NATIVE_ENDIAN;
          fmt.align = 0;
          continue;
        }
        if (st == JXL_DEC_NEED_IMAGE_OUT_BUFFER)
        {
          size_t need = 0;
          if (JxlDecoderImageOutBufferSize(dec, &fmt, &need) != JXL_DEC_SUCCESS)
            throw LIBRAW_EXCEPTION_DECODE_RAW;
          pixels.resize(need / sizeof(uint16_t));
          if (JxlDecoderSetImageOutBuffer(dec, &fmt, pixels.data(), need) != JXL_DEC_SUCCESS)
            throw LIBRAW_EXCEPTION_DECODE_RAW;
          continue;
        }
        if (st == JXL_DEC_FULL_IMAGE)
        {
          got_image = true;
          break;
        }
        if (st == JXL_DEC_SUCCESS)
          break;
        // Any other event (colour encoding, boxes, ...) is not needed here.
      }

      if (!got_image || !info.xsize || !info.ysize)
        throw LIBRAW_EXCEPTION_DECODE_RAW;

      const unsigned nch = fmt.num_channels;
      const unsigned use_ch = MIN((unsigned)colors, nch);
      for (unsigned y = 0; y < info.ysize; y++)
      {
        row = trow + y;
        if (row >= (unsigned)height)
          break;
        const uint16_t *src = pixels.data() + (size_t)y * info.xsize * nch;
        for (col = 0; col < info.xsize && tcol + col < (unsigned)width; col++)
          for (c = 0; c < use_ch; c++)
            image[(size_t)row * width + tcol + col][c] =
                curve16[(size_t)c * 65536 + src[(size_t)col * nch + c]];
      }

      if ((tcol += tile_width) >= raw_width)
        trow += tile_length + (tcol = 0);
    }
  }
  catch (...)
  {
    JxlDecoderDestroy(dec);
    order = sorder;
    throw;
  }

  JxlDecoderDestroy(dec);
  maximum = 0xffff;
}

#else  // !USE_JXL

void LibRaw::jxl_dng_load_raw()
{
  // Built without libjxl: behave exactly like the upstream placeholder so the
  // caller reports LIBRAW_FILE_UNSUPPORTED rather than crashing.
  throw LIBRAW_EXCEPTION_UNSUPPORTED_FORMAT;
}

#endif // USE_JXL

'''


def edit(path, old, new, why):
    """Replace `old` with `new`, asserting it occurs exactly once."""
    s = io.open(path, encoding="utf-8", errors="surrogateescape").read()
    if new in s:
        return False            # already patched
    n = s.count(old)
    if n != 1:
        sys.exit("apply_libraw_jxl: anchor for '%s' matched %d times in %s -- "
                 "LibRaw has changed; update this script rather than forcing it.\n"
                 "  anchor: %s" % (why, n, path, old.strip().splitlines()[0]))
    io.open(path, "w", encoding="utf-8", errors="surrogateescape").write(
        s.replace(old, new))
    return True


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = sys.argv[1]

    dng = os.path.join(root, "src/decoders/dng.cpp")
    funcs = os.path.join(root, "internal/libraw_internal_funcs.h")
    identify = os.path.join(root, "src/metadata/identify.cpp")
    for f in (dng, funcs, identify):
        if not os.path.isfile(f):
            sys.exit("apply_libraw_jxl: not a LibRaw tree: missing " + f)

    if MARKER in io.open(dng, encoding="utf-8", errors="surrogateescape").read():
        print("apply_libraw_jxl: already applied")
        return

    changed = []

    # 1. Declare the new loader beside the placeholder it replaces.
    if edit(funcs,
            "\tvoid        jxl_dng_load_raw_placeholder();",
            "\tvoid        jxl_dng_load_raw_placeholder();\n"
            "\tvoid        jxl_dng_load_raw();   // MTEngineSDL: real JPEG XL DNG decode",
            "loader declaration"):
        changed.append(funcs)

    # 2. The decoder itself, inserted immediately before lossy_dng_load_raw's
    #    NO_JPEG guard -- i.e. right next to the function it is modelled on.
    if edit(dng,
            "#ifdef NO_JPEG\nvoid LibRaw::lossy_dng_load_raw() {}",
            DECODER + "#ifdef NO_JPEG\nvoid LibRaw::lossy_dng_load_raw() {}",
            "decoder insertion point"):
        changed.append(dng)

    # 3. Route Compression 52546 at it instead of the throwing placeholder.
    if edit(identify,
            "    case 52546:\n      load_raw = &LibRaw::jxl_dng_load_raw_placeholder;\n      break;",
            "    case 52546:\n"
            "      // MTEngineSDL: real decode via libjxl (see dng.cpp). Falls back to\n"
            "      // throwing UNSUPPORTED_FORMAT when built without USE_JXL.\n"
            "      load_raw = &LibRaw::jxl_dng_load_raw;\n      break;",
            "compression dispatch"):
        changed.append(identify)

    # 4. Includes. <vector> and <cmath> are already pulled in transitively by
    #    LibRaw's headers, but jxl/decode.h is ours to add.
    if edit(dng,
            '#include "../../internal/dcraw_defs.h"',
            '#include "../../internal/dcraw_defs.h"\n'
            '#ifdef USE_JXL\n'
            '#include <jxl/decode.h>\n'
            '#include <vector>\n'
            '#include <cstring>\n'
            '#endif',
            "decoder includes"):
        changed.append(dng)

    print("apply_libraw_jxl: patched %d file(s)" % len(set(changed)))


if __name__ == "__main__":
    main()
