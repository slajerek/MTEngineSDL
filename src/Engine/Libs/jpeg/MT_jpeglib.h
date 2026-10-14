#pragma once
// The ONE way to include libjpeg (plan 2026-10-07-libjpeg-turbo). The process
// links exactly one jpeg_* library -- libjpeg-turbo, built by build-jpeg with
// the jpeg-6b API -- and a translation unit compiled against any other
// jpeglib.h (the old jpeg-9a tree, Homebrew's turbo with the jpeg-8 API, a
// distro package) would disagree with it about every struct layout.
#include <stddef.h>
#include <stdio.h>    // jpeglib.h needs FILE and size_t first; C headers, because callers
                      // include this inside extern "C" { } (JPEGWriter.h:8-11, JPEG.h:4-6, ...)
#include "jpeglib.h"
// The version floor: 3.x is what build-jpeg pins (3.2.0) and what transupp's
// and jpeg_skip_scanlines' behaviour here was verified against; an older turbo
// built with the same jpeg-6b API would otherwise pass unnoticed.
#if !defined(LIBJPEG_TURBO_VERSION_NUMBER) || JPEG_LIB_VERSION != 62 || LIBJPEG_TURBO_VERSION_NUMBER < 3000000
#error "jpeglib.h is not the engine's libjpeg-turbo 3.x (jpeg-6b API) -- check that $(MT_CAPS_LIBS_DIR)/jpeg/include comes first and build-jpeg has run"
#endif
