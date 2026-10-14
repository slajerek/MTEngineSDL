#!/usr/bin/env python3
"""The two stamp rules of platform/<P>/build-jpeg.sh, as a regression test.

build-jpeg.sh (plan 2026-10-07-libjpeg-turbo) builds libjpeg-turbo into a STORE
(archive, stamp, staged headers) and into WORK prefixes that build-image_codecs.sh
reads. Two rules keep its stamp honest, both because the store and the work
prefix live and die separately:

  1. The SIMD decision and the final-build refusal run BEFORE the stamp check.
     Otherwise a development build's scalar x86_64 stamp satisfies a final
     build (MT_PROD_BUILD=1), and the refusal is bypassed by a cache hit.
  2. A stamp hit needs every artefact a CONSUMER reads -- the work prefixes too.
     A surviving store beside a cleaned work root must rebuild, never exit 0
     with prefixes the codec script refuses.

The script runs for real, in a temporary MTENGINE_BUILD_ROOT, with fake
cmake/clang/libtool/lipo/nasm (macOS) or cmake/cc/ar/nasm (Linux) first on PATH.
The fakes create the outputs the real tools would and append their argv to a
log, so the test can count configure runs. curl and the SHA-256 tool are faked
too: the "download" is a tiny tarball with the two files the script reads from
the source tree, and the hash tool answers the pinned value. Nothing touches
the network or the user's cache.

Skipped on Windows hosts: the .ps1 twin is checked by hand (plan Task 7).

Run:  python3 -B tools/mtcaps/tests/test_build_jpeg_stamp.py
"""

import os
import platform
import re
import shutil
import stat
import subprocess
import sys
sys.dont_write_bytecode = True  # THE RULE: nothing lands in the engine checkout
import tarfile
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))

IS_MAC = platform.system() == "Darwin"
SCRIPT = os.path.join(ENGINE, "platform", "MacOS" if IS_MAC else "Linux", "build-jpeg.sh")


def _pinned(name):
    with open(SCRIPT, encoding="utf-8") as f:
        m = re.search(r'^%s="([^"]+)"' % name, f.read(), re.M)
    if not m:
        raise AssertionError("%s not found in %s" % (name, SCRIPT))
    return m.group(1)


# Shared by every fake: log argv, then act.
FAKE_HEAD = """#!/usr/bin/env bash
set -euo pipefail
echo "$(basename "$0") $*" >> "$FAKE_LOG"
"""

FAKE_CMAKE = FAKE_HEAD + r"""
if [[ "${1:-}" == "--build" ]]; then exit 0; fi
if [[ "${1:-}" == "--install" ]]; then
  build="$2"; prefix="$(cat "$build/.fake-prefix")"; simd="$(cat "$build/.fake-simd")"
  mkdir -p "$prefix/lib" "$prefix/include"
  : > "$prefix/lib/libjpeg.a"
  for h in jpeglib.h jmorecfg.h jerror.h; do echo "/* $h */" > "$prefix/include/$h"; done
  { echo "#define JPEG_LIB_VERSION 62"
    if [[ "$simd" == "1" ]]; then echo "#define WITH_SIMD 1"; else echo "/* #undef WITH_SIMD */"; fi
  } > "$prefix/include/jconfig.h"
  exit 0
fi
build=""; prefix=""; simd=1
while [[ $# -gt 0 ]]; do
  case "$1" in
    -B) build="$2"; shift ;;
    -DCMAKE_INSTALL_PREFIX=*) prefix="${1#*=}" ;;
    -DWITH_SIMD=*) simd="${1#*=}" ;;
  esac
  shift
done
mkdir -p "$build"
echo "$prefix" > "$build/.fake-prefix"
# What turbo really prints. WITH_SIMD=0 asked for: "SIMD extensions: None".
# SIMD asked for but unavailable (FAKE_TURBO_SIMD_FAIL=1 -- no usable NASM, an
# unsupported CPU): simd_fail prints only a WARNING and turns WITH_SIMD off,
# so the installed jconfig.h is the one honest witness.
if [[ "$simd" == "0" ]]; then
  echo "-- SIMD extensions: None (WITH_SIMD = 0)"
elif [[ "${FAKE_TURBO_SIMD_FAIL:-0}" == "1" ]]; then
  echo "CMake Warning at simd/CMakeLists.txt:3 (message):"
  echo "  SIMD extensions disabled: could not find NASM compiler"
  simd=0
else
  echo "-- SIMD extensions: FAKE (WITH_SIMD = 1)"
fi
echo "$simd" > "$build/.fake-simd"
"""

# clang / cc / libtool: create whatever follows -o.
FAKE_OUTPUT_O = FAKE_HEAD + r"""
out=""
while [[ $# -gt 0 ]]; do
  if [[ "$1" == "-o" ]]; then out="$2"; shift; fi
  shift
done
[[ -n "$out" ]] && { mkdir -p "$(dirname "$out")"; : > "$out"; }
exit 0
"""

FAKE_LIPO = FAKE_HEAD + r"""
if [[ "${1:-}" == "-info" ]]; then echo "Architectures in the fat file: $2 are: x86_64 arm64"; exit 0; fi
out=""
while [[ $# -gt 0 ]]; do
  if [[ "$1" == "-output" ]]; then out="$2"; shift; fi
  shift
done
[[ -n "$out" ]] && : > "$out"
exit 0
"""

# GNU ar: `rcs out obj...` and `-M` (MRI script on stdin, CREATE <out>).
FAKE_AR = FAKE_HEAD + r"""
if [[ "${1:-}" == "-M" ]]; then
  out="$(sed -n 's/^CREATE //p')"
  : > "$out"; exit 0
fi
: > "$2"
exit 0
"""

FAKE_CURL = FAKE_HEAD + r"""
out=""
while [[ $# -gt 0 ]]; do
  if [[ "$1" == "-o" ]]; then out="$2"; shift; fi
  shift
done
cp "$FAKE_TARBALL" "$out"
"""

# shasum -a 256 <f> / sha256sum <f>: answer the pinned hash for every file.
FAKE_SHA = """#!/usr/bin/env bash
echo "$FAKE_SHA256  ${@: -1}"
"""

FAKE_TRUE = FAKE_HEAD + "exit 0\n"


@unittest.skipIf(os.name == "nt", "the .ps1 twin is checked by hand (plan Task 7)")
class TestBuildJpegStamp(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="build-jpeg-test-")
        self.root = os.path.join(self.tmp, "root")
        self.view = os.path.join(self.tmp, "view")
        self.bin = os.path.join(self.tmp, "bin")
        self.log = os.path.join(self.tmp, "fake.log")
        os.makedirs(self.bin)
        version = _pinned("LIBJPEG_TURBO_VERSION")
        top = "libjpeg-turbo-%s" % version
        stage = os.path.join(self.tmp, "stage", top, "src")
        os.makedirs(stage)
        for name in ("transupp.h", "transupp.c"):
            with open(os.path.join(stage, name), "w") as f:
                f.write("/* fake %s */\n" % name)
        self.tarball = os.path.join(self.tmp, "turbo.tar.gz")
        with tarfile.open(self.tarball, "w:gz") as t:
            t.add(os.path.join(self.tmp, "stage", top), arcname=top)

        fakes = {"cmake": FAKE_CMAKE, "nasm": FAKE_TRUE, "curl": FAKE_CURL}
        if IS_MAC:
            fakes.update({"clang": FAKE_OUTPUT_O, "libtool": FAKE_OUTPUT_O,
                          "lipo": FAKE_LIPO, "shasum": FAKE_SHA})
        else:
            fakes.update({"cc": FAKE_OUTPUT_O, "ar": FAKE_AR, "ranlib": FAKE_TRUE,
                          "sha256sum": FAKE_SHA})
        for name, body in fakes.items():
            p = os.path.join(self.bin, name)
            with open(p, "w") as f:
                f.write(body)
            os.chmod(p, os.stat(p).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

        self.env = dict(os.environ)
        for k in list(self.env):
            if k.startswith("MT_") or k in ("MACOSX_DEPLOYMENT_TARGET",):
                del self.env[k]
        self.env.update({
            # The script puts MacPorts/system dirs first on PATH (the codec
            # script's order); the fakes go through its test-only override.
            "MT_JPEG_TEST_TOOL_DIR": self.bin,
            "MTENGINE_BUILD_ROOT": self.root,
            "MT_CAPS_LIBS_DIR": self.view,
            "FAKE_LOG": self.log,
            "FAKE_TARBALL": self.tarball,
            "FAKE_SHA256": _pinned("LIBJPEG_TURBO_SHA256"),
        })
        self.arches = ["arm64", "x86_64"] if IS_MAC else [platform.machine()]

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_script(self, **extra):
        env = dict(self.env)
        env.update(extra)
        open(self.log, "w").close()
        return subprocess.run(["bash", SCRIPT], env=env, capture_output=True, text=True)

    def configures(self):
        with open(self.log) as f:
            return [l for l in f if l.startswith("cmake ") and " -S " in l]

    def prefix(self, arch):
        return os.path.join(self.root, "_deps", "work", "jpeg", "install-%s" % arch)

    def assert_built(self, proc):
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertTrue(os.path.isfile(os.path.join(self.view, "libmt_jpeg.a")))
        self.assertTrue(os.path.isfile(os.path.join(self.view, "libmt_jpeg.stamp")))
        self.assertTrue(os.path.isfile(os.path.join(self.view, "jpeg", "include", "transupp.h")))
        for arch in self.arches:
            for rel in ("lib/libjpeg.a", "lib/libjpeg_transupp.a", "include/jpeglib.h",
                        "include/jconfig.h", "include/jmorecfg.h", "include/jerror.h",
                        "include/transupp.h"):
                self.assertTrue(os.path.isfile(os.path.join(self.prefix(arch), rel)),
                                "%s missing from %s" % (rel, self.prefix(arch)))

    def test_unchanged_second_run_hits_the_stamp(self):
        """(c) The stamp still works: nothing changed, nothing is configured."""
        self.assert_built(self.run_script())
        self.assertEqual(len(self.configures()), len(self.arches))
        again = self.run_script()
        self.assertEqual(again.returncode, 0, again.stderr)
        self.assertEqual([], self.configures(), "a second identical run re-ran cmake")

    def test_lost_work_prefix_rebuilds_despite_a_valid_store(self):
        """(a) Rule 2: the store and stamp survive, the work prefixes do not
        (a cleaned work root) -- the script must rebuild them, not exit 0."""
        self.assert_built(self.run_script())
        for arch in self.arches:
            shutil.rmtree(self.prefix(arch))
        proc = self.run_script()
        self.assertGreaterEqual(len(self.configures()), 1,
                                "store hit with missing prefixes did not rebuild")
        self.assert_built(proc)

    def test_scalar_dev_stamp_does_not_satisfy_a_final_build(self):
        """(b) Rule 1: a development scalar build's stamp, then a final build
        with the same inputs -- it must refuse, not hit the cached stamp."""
        if "x86_64" in self.arches:
            scalar = {"MT_JPEG_X86_SIMD": "0"}
        else:
            # aarch64 Linux: the SIMD state comes from turbo's configure.
            scalar = {"FAKE_TURBO_SIMD_FAIL": "1"}
        dev = self.run_script(**scalar)
        self.assert_built(dev)
        self.assertIn("WITHOUT SIMD", dev.stderr)
        with open(os.path.join(self.view, "libmt_jpeg.stamp")) as f:
            self.assertIn("simd=0", f.read())
        prod = self.run_script(MT_PROD_BUILD="1", **scalar)
        self.assertNotEqual(prod.returncode, 0,
                            "a final build accepted the scalar stamp:\n" + prod.stdout + prod.stderr)
        self.assertIn("final build", prod.stderr)

    def test_requested_simd_that_fails_in_configure_is_caught(self):
        """SIMD asked for (nasm present) but turbo's configure turns it off --
        it prints only "WARNING: SIMD extensions disabled", never "SIMD
        extensions: None". The script must still see a scalar build (from the
        installed jconfig.h): a development build warns and stamps simd=0, a
        final build refuses."""
        dev = self.run_script(FAKE_TURBO_SIMD_FAIL="1")
        self.assert_built(dev)
        self.assertIn("WITHOUT SIMD", dev.stderr)
        with open(os.path.join(self.view, "libmt_jpeg.stamp")) as f:
            self.assertIn("simd=0", f.read())
        prod = self.run_script(MT_PROD_BUILD="1", FAKE_TURBO_SIMD_FAIL="1")
        self.assertNotEqual(prod.returncode, 0,
                            "a final build shipped a slice whose SIMD failed in configure:\n"
                            + prod.stdout + prod.stderr)
        self.assertIn("final build", prod.stderr)


if __name__ == "__main__":
    unittest.main()
