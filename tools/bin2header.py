#!/usr/bin/env python3
"""Generate a C header embedding a compiled .metallib AND its MSL source.

ONE SOURCE OF TRUTH: the .metal file. The generated header carries both

  * the compiled .metallib bytes, for the SHIPPING path
    (CRenderShaderMetal::GetEmbeddedLibraryData -> newLibraryWithData), and
  * the MSL text, for the DEVELOPMENT path
    (GetMetalShaderSource -> newLibraryWithSource),

so the two can never drift: editing the .metal file and regenerating updates
both, and there is no second copy of the shader anywhere to forget.

A CONTENT HASH of the source is emitted alongside them. The build compares it
against a freshly computed hash of the .metal file and FAILS when they differ.
It is a hash rather than a timestamp deliberately: timestamps are not preserved
by git, so a fresh clone or a branch switch would either spuriously fail or
spuriously pass depending on checkout order, and "regenerate because the mtime
moved" trains people to regenerate without looking.

Usage:
    python3 tools/bin2header.py <name> <source.metal> <compiled.metallib> <out.h>
"""

import hashlib
import os
import sys


def c_identifier(name):
    return "".join(ch if ch.isalnum() else "_" for ch in name)


def emit_bytes(data, per_line=16):
    lines = []
    for i in range(0, len(data), per_line):
        chunk = data[i:i + per_line]
        lines.append("\t" + " ".join("0x%02x," % b for b in chunk))
    return "\n".join(lines)


def emit_source_literal(text):
    # A raw string literal with a delimiter that cannot appear in MSL. Escaping
    # the source by hand would be one more thing to get subtly wrong.
    delim = "MTMSL"
    assert (')' + delim + '"') not in text, "source contains the raw-string delimiter"
    return 'R"%s(\n%s)%s"' % (delim, text, delim)


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)

    name, src_path, lib_path, out_path = sys.argv[1:5]
    ident = c_identifier(name)

    with open(src_path, "r", encoding="utf-8") as f:
        source = f.read()
    with open(lib_path, "rb") as f:
        blob = f.read()

    source_hash = hashlib.sha256(source.encode("utf-8")).hexdigest()

    header = """// GENERATED FILE -- DO NOT EDIT.
//
// Produced by tools/bin2header.py from %s.
// Regenerate with:
//
//     ./tools/embed-metal-shaders.sh
//
// Committed to the repository on purpose: it lets a checkout build without the
// Metal toolchain having to run, and it puts shader changes in the diff where a
// reviewer can see them. The build FAILS if this file is stale -- see
// k%sSourceSha256 and the check in embed-metal-shaders.sh.

#ifndef _%s_Metallib_h_
#define _%s_Metallib_h_

// sha256 of the .metal source this header was generated from.
static const char *k%sSourceSha256 = "%s";

// The MSL source. DEVELOPMENT path: newLibraryWithSource.
static const char *k%sMetalSource = %s;

// The compiled library. SHIPPING path: newLibraryWithData. %d bytes.
static const unsigned char k%sMetallibData[] = {
%s
};
static const unsigned long k%sMetallibLength = %d;

#endif
""" % (os.path.basename(src_path), ident, ident, ident, ident, source_hash,
       ident, emit_source_literal(source), len(blob), ident, emit_bytes(blob),
       ident, len(blob))

    with open(out_path, "w", encoding="utf-8") as f:
        f.write(header)

    print("%-34s %7d bytes metallib  sha %s" % (os.path.basename(out_path), len(blob), source_hash[:12]))


if __name__ == "__main__":
    main()
