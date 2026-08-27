#!/usr/bin/env python3
"""Golden tests for mtcaps (#4.4.5).

`tools/mtcaps` is the single point of failure for the whole capability system, so
it gets tests before it gets consumers.

Run:  python3 tools/mtcaps/tests/test_mtcaps.py

stdlib unittest, no pip -- the same constraint as the tool itself.
"""

import glob
import os
import shutil
import subprocess
import sys
sys.dont_write_bytecode = True  # THE RULE: nothing lands in the engine checkout
import tempfile
import unittest
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.dirname(HERE)
ENGINE = os.path.dirname(os.path.dirname(TOOL))
sys.path.insert(0, TOOL)

import resolve as R          # noqa: E402
import vocab as V            # noqa: E402
from errors import ManifestError, VocabError  # noqa: E402

MTCAPS = os.path.join(TOOL, "mtcaps.py")


def run(*args, **kw):
    return subprocess.run([sys.executable, "-B", MTCAPS] + list(args),
                          capture_output=True, text=True, **kw)


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="mtcaps-test-")
        self.out_root = os.path.join(self.tmp, "out")
        self.vocab = V.load()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def manifest(self, text, name="mtengine.caps"):
        p = os.path.join(self.tmp, name)
        with open(p, "w", encoding="utf-8", newline="") as f:
            f.write(text)
        return p

    def resolve(self, manifest, *extra, platform="macos", arch="arm64",
                config="Debug", app="testapp"):
        return run("resolve", "--manifest", manifest, "--app", app,
                   "--platform", platform, "--arch", arch, "--config", config,
                   "--engine-dir", ENGINE, "--out-dir", self.out_root, *extra)

    def resolved_of(self, proc):
        for line in proc.stdout.splitlines():
            if line.startswith("resolved="):
                return line[len("resolved="):]
        self.fail("no `resolved=` line in:\n%s\n%s" % (proc.stdout, proc.stderr))

    def out_of(self, proc):
        for line in proc.stdout.splitlines():
            if line.startswith("out_dir="):
                return line[len("out_dir="):]
        self.fail("no `out_dir=` line")

    def deps_of(self, proc):
        for line in proc.stdout.splitlines():
            if line.startswith("deps_dir="):
                return line[len("deps_dir="):]
        self.fail("no `deps_dir=` line in:\n%s\n%s" % (proc.stdout, proc.stderr))


class TestVocabulary(Base):
    def test_ships_valid(self):
        """The shipped vocabulary validates. If this fails nothing else matters."""
        self.assertGreaterEqual(len(self.vocab.keys), 20)

    def test_short_names_unique_and_distinct_from_tokens(self):
        shorts = [self.vocab.short(k) for k in self.vocab.keys]
        self.assertEqual(len(shorts), len(set(shorts)))
        for key in self.vocab.keys:
            self.assertNotEqual(self.vocab.short(key), key)

    def test_commercial_is_not_a_capability(self):
        """It is an orthogonal licence-mode input, not a capability. A key here
        would put it in the enum and in MT_Capabilities.h."""
        self.assertNotIn("MT_COMMERCIAL_BUILD", self.vocab.capabilities)

    def test_implications_terminate_and_target_real_keys(self):
        for key in self.vocab.keys:
            for target in self.vocab.implies(key):
                self.assertIn(target, self.vocab.capabilities)

    def test_every_dependency_states_a_licence(self):
        """LICENSES.txt is generated from these. A missing one ships a legally
        incomplete SBOM that looks complete."""
        for owner, dep in self.vocab.all_dependencies():
            self.assertTrue(dep["licence"].strip(), "%s: %s" % (owner, dep["name"]))

    def test_core_section_covers_the_always_on_libraries(self):
        names = " ".join(d["name"] for d in self.vocab.core["dependencies"]).lower()
        for expected in ("sdl3", "zlib", "alsa", "gtk3", "freetype", "libuv"):
            self.assertIn(expected, names,
                          "%s ships outside every capability key and must be in `core`"
                          % expected)

    def test_malformed_vocabulary_names_the_key_and_the_rule(self):
        bad = os.path.join(self.tmp, "bad.json")
        with open(bad, "w") as f:
            f.write('{"version":1,"core":{"dependencies":[]},'
                    '"capabilities":{"NOT_A_CAP":{}}}')
        with self.assertRaises(VocabError) as cm:
            V.load(bad)
        self.assertIn("NOT_A_CAP", str(cm.exception))


class TestResolution(Base):
    def test_rung4_unlisted_keys_get_the_engine_default(self):
        m = self.manifest("MT_CAP_LLM=0\n")
        values, prov = R.resolve(self.vocab, m, "macos")
        self.assertEqual(values["MT_CAP_LLM"], 0)
        self.assertEqual(prov["MT_CAP_LLM"], "manifest")
        self.assertEqual(values["MT_CAP_MIDI"], self.vocab.default("MT_CAP_MIDI"))
        self.assertEqual(prov["MT_CAP_MIDI"], "default")

    def test_os_suffix_beats_unsuffixed_on_that_platform_only(self):
        m = self.manifest("MT_CAP_PHOTO_CODECS=1\nMT_CAP_PHOTO_CODECS__LINUX=0\n")
        linux, _ = R.resolve(self.vocab, m, "linux")
        macos, _ = R.resolve(self.vocab, m, "macos")
        self.assertEqual(linux["MT_CAP_PHOTO_CODECS"], 0)
        self.assertEqual(macos["MT_CAP_PHOTO_CODECS"], 1)

    def test_implication_runs_downward(self):
        """Every shipped default is 1 today -- the engine has everything on -- so
        an implication can never CHANGE a value in the shipped vocabulary, only
        detect a contradiction. The mechanism still has to work for a capability
        that defaults off, so this exercises it against a synthetic vocabulary
        rather than pretending the shipped one demonstrates it."""
        import json
        data = json.load(open(self.vocab.path, encoding="utf-8"))
        for key in ("MT_CAP_RAW", "MT_CAP_DEVELOP"):
            data["capabilities"][key]["default"] = 0
        path = os.path.join(self.tmp, "vocab-off.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f)
        vocab = V.load(path)

        m = self.manifest("MT_CAP_COLOR_MANAGEMENT=1\n")
        values, prov = R.resolve(vocab, m, "macos")
        self.assertEqual(values["MT_CAP_DEVELOP"], 1)
        self.assertEqual(values["MT_CAP_RAW"], 1)
        self.assertTrue(prov["MT_CAP_RAW"].startswith("implied by"), prov["MT_CAP_RAW"])

        # And with COLOR_MANAGEMENT off, the leaves stay at their 0 default:
        # implication runs downward only, never upward.
        m2 = self.manifest("MT_CAP_COLOR_MANAGEMENT=0\n", "off.caps")
        values2, _ = R.resolve(vocab, m2, "macos")
        self.assertEqual(values2["MT_CAP_RAW"], 0)

    def test_turning_off_a_leaf_alone_is_a_conflict_with_its_on_by_default_parent(self):
        """A consequence of every default being 1, and it is the DESIGNED
        behaviour rather than a wart: an app that wants RAW off must also write
        DEVELOP=0 and COLOR_MANAGEMENT=0, so a reader can see from the manifest
        alone what the app does not want. Silently resolving it either way would
        give the app a capability set nobody chose."""
        m = self.manifest("MT_CAP_RAW=0\n")
        with self.assertRaises(ManifestError) as cm:
            R.resolve(self.vocab, m, "macos")
        self.assertIn("MT_CAP_DEVELOP", str(cm.exception))

    def test_implication_does_not_run_upward(self):
        m = self.manifest("MT_CAP_RAW=1\nMT_CAP_DEVELOP=0\nMT_CAP_COLOR_MANAGEMENT=0\n")
        values, _ = R.resolve(self.vocab, m, "macos")
        self.assertEqual(values["MT_CAP_DEVELOP"], 0)

    def test_conflict_is_an_error_naming_BOTH_keys(self):
        """An explicit =0 never silently overrides an implication: the app has
        asked for something incoherent and only its author can say which half
        was meant."""
        m = self.manifest("MT_CAP_COLOR_MANAGEMENT=1\nMT_CAP_RAW=0\n")
        with self.assertRaises(ManifestError) as cm:
            R.resolve(self.vocab, m, "macos")
        msg = str(cm.exception)
        self.assertIn("MT_CAP_RAW", msg)
        self.assertIn("conflict", msg)

    def test_llm_implies_https_not_merely_http(self):
        """Every model URL is https://; LLM=1 HTTPS=0 would build, link, and be
        unable to fetch a model."""
        m = self.manifest("MT_CAP_LLM=1\n")
        values, _ = R.resolve(self.vocab, m, "macos")
        self.assertEqual(values["MT_CAP_HTTP"], 1)
        self.assertEqual(values["MT_CAP_HTTPS"], 1)

    def test_net_game_implies_transport_and_http(self):
        m = self.manifest("MT_CAP_NET_GAME=1\n")
        values, _ = R.resolve(self.vocab, m, "macos")
        self.assertEqual(values["MT_CAP_NET_TRANSPORT"], 1)
        self.assertEqual(values["MT_CAP_HTTP"], 1)


class TestManifestFormat(Base):
    def test_whole_line_comments_and_blanks_are_legal(self):
        m = self.manifest("# the DummyApp's example manifest\n\n"
                          "# no LLM here\nMT_CAP_LLM=0\n\n")
        values, _ = R.resolve(self.vocab, m, "macos")
        self.assertEqual(values["MT_CAP_LLM"], 0)

    def test_inline_comments_are_rejected(self):
        """ConvertFrom-StringData keeps an inline comment as part of the VALUE, so
        a manifest that reads fine in bash means something else on Windows."""
        m = self.manifest("MT_CAP_LLM=0 # off\n")
        with self.assertRaises(ManifestError):
            R.resolve(self.vocab, m, "macos")

    def test_key_must_be_a_legal_shell_identifier(self):
        """bash `source` is the binding constraint and it is the strictest of the
        three parsers. Measured: a dotted key is a `command not found` and,
        under `set -e`, a hard build abort."""
        m = self.manifest("MT_CAP_HTTPS.linux=0\n")
        with self.assertRaises(ManifestError) as cm:
            R.resolve(self.vocab, m, "macos")
        self.assertIn("shell identifier", str(cm.exception))

    def test_the_manifest_really_does_source_in_bash(self):
        """Not an argument -- a measurement. If bash cannot source it, the format
        claim is false whatever this module thinks."""
        m = self.manifest("# comment\n\nMT_CAP_LLM=0\nMT_CAP_HTTPS__MACOS=1\n")
        proc = subprocess.run(["bash", "-c", "set -e; source %s; echo $MT_CAP_LLM" % m],
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stdout.strip(), "0")

    def test_unknown_key_is_an_error(self):
        m = self.manifest("MT_CAP_TYPO=1\n")
        with self.assertRaises(ManifestError) as cm:
            R.resolve(self.vocab, m, "macos")
        self.assertIn("MT_CAP_TYPO", str(cm.exception))

    def test_non_binary_value_is_an_error(self):
        for bad in ("ON", "true", "2", "yes"):
            m = self.manifest("MT_CAP_LLM=%s\n" % bad)
            with self.assertRaises(ManifestError):
                R.resolve(self.vocab, m, "macos")


class TestCanonicalForm(Base):
    def test_shape_is_pinned(self):
        m = self.manifest("MT_CAP_LLM=0\n")
        values, _ = R.resolve(self.vocab, m, "macos")
        s = R.canonical_form(self.vocab, values)
        self.assertFalse(s.endswith(";"), "no trailing separator")
        self.assertEqual(s, ";".join(sorted(s.split(";"))), "sorted by byte value")
        for entry in s.split(";"):
            key, _, value = entry.partition("=")
            self.assertIn(value, ("0", "1"), "values are 1 or 0 only")
        self.assertIn("MT_COMMERCIAL_BUILD=0", s,
                      "included as 0/1 so the check can tell off from unset")
        self.assertNotIn("MT_ENABLE_", s, "MT_ENABLE_* are excluded")

    def test_materialises_every_known_capability_not_just_listed_ones(self):
        m = self.manifest("MT_CAP_LLM=0\n")
        values, _ = R.resolve(self.vocab, m, "macos")
        s = R.canonical_form(self.vocab, values)
        # +2, not +1: every capability, plus BOTH mode keys
        # (MT_COMMERCIAL_BUILD and MT_PRIVATE_BUILD). Each is carried as 0/1
        # rather than by presence, because the agreement check has to tell
        # "off" from "unset" for a mode flag.
        self.assertEqual(len(s.split(";")), len(self.vocab.keys) + 2)

    def test_os_suffixes_are_resolved_away(self):
        m = self.manifest("MT_CAP_PHOTO_CODECS__LINUX=0\n")
        values, _ = R.resolve(self.vocab, m, "linux")
        self.assertNotIn("__LINUX", R.canonical_form(self.vocab, values))


class TestRoundTrip(Base):
    def test_resolve_then_check_agrees(self):
        m = self.manifest("MT_CAP_LLM=0\n")
        p = self.resolve(m)
        self.assertEqual(p.returncode, 0, p.stderr)
        c = run("check", "--manifest", m, "--app", "testapp", "--platform", "macos",
                "--arch", "arm64", "--config", "Debug", "--engine-dir", ENGINE,
                "--resolved", self.resolved_of(p))
        self.assertEqual(c.returncode, 0, c.stderr)

    def test_hand_edited_fragment_makes_check_disagree(self):
        m = self.manifest("MT_CAP_LLM=0\n")
        p = self.resolve(m)
        tampered = self.resolved_of(p).replace("MT_CAP_LLM=0", "MT_CAP_LLM=1")
        c = run("check", "--manifest", m, "--app", "testapp", "--platform", "macos",
                "--arch", "arm64", "--config", "Debug", "--engine-dir", ENGINE,
                "--resolved", tampered)
        self.assertEqual(c.returncode, 1)
        self.assertIn("MT_CAP_LLM", c.stderr)

    def test_reordered_and_crlf_manifest_still_passes(self):
        """A manifest that is merely reordered or CRLF-terminated must pass: the
        check compares re-resolved SETS, never bytes."""
        a = self.manifest("MT_CAP_LLM=0\nMT_CAP_MIDI=0\n", "a.caps")
        p = self.resolve(a)
        b = self.manifest("MT_CAP_MIDI=0\r\nMT_CAP_LLM=0\r\n", "b.caps")
        c = run("check", "--manifest", b, "--app", "testapp", "--platform", "macos",
                "--arch", "arm64", "--config", "Debug", "--engine-dir", ENGINE,
                "--resolved", self.resolved_of(p))
        self.assertEqual(c.returncode, 0, c.stderr)

    def test_override_round_trips_through_overrides_caps(self):
        """rung 1 and the agreement check must not cancel out: an override is a
        deliberate disagreement with the manifest, and without the persisted
        overlay the Phase 6 matrix could not build a single non-default
        combination."""
        m = self.manifest("MT_CAP_LLM=0\n")
        p = self.resolve(m, "--set", "MT_CAP_MIDI=0")
        out = self.out_of(p)
        overrides = os.path.join(out, "overrides.caps")
        self.assertTrue(os.path.isfile(overrides))
        self.assertIn("MT_CAP_MIDI=0", self.resolved_of(p))
        c = run("check", "--manifest", m, "--app", "testapp", "--platform", "macos",
                "--arch", "arm64", "--config", "Debug", "--engine-dir", ENGINE,
                "--overrides", overrides, "--resolved", self.resolved_of(p))
        self.assertEqual(c.returncode, 0, c.stderr)

    def test_an_ordinary_build_passes_no_overrides_and_reads_none(self):
        """Staleness is impossible rather than cleaned up."""
        m = self.manifest("MT_CAP_LLM=0\n")
        p = self.resolve(m)
        self.assertFalse(os.path.isfile(os.path.join(self.out_of(p), "overrides.caps")))

    def test_two_override_variants_get_two_roots(self):
        """A matrix is many builds of ONE app at once. Two variants must not race
        on one fragment or one override file."""
        m = self.manifest("MT_CAP_LLM=0\n")
        a = self.out_of(self.resolve(m, "--set", "MT_CAP_MIDI=0"))
        b = self.out_of(self.resolve(m, "--set", "MT_CAP_MIDI=1"))
        self.assertNotEqual(a, b)

    def test_stamp_form_of_check(self):
        m = self.manifest("MT_CAP_LLM=0\n")
        p = self.resolve(m)
        stamp = os.path.join(self.out_of(p), "resolved.stamp")
        ok = run("check", "--stamp", stamp, "--resolved", self.resolved_of(p))
        self.assertEqual(ok.returncode, 0, ok.stderr)
        bad = run("check", "--stamp", stamp, "--resolved", "MT_CAP_LLM=1")
        self.assertEqual(bad.returncode, 1)

    def test_idempotence_two_runs_are_byte_identical(self):
        m = self.manifest("MT_CAP_LLM=0\n")
        out = self.out_of(self.resolve(m))
        first = {}
        for root, _, files in os.walk(out):
            for name in files:
                path = os.path.join(root, name)
                first[path] = open(path, "rb").read()
        self.resolve(m)
        for path, data in first.items():
            self.assertEqual(open(path, "rb").read(), data, "%s changed" % path)


class TestMSBuildFragments(Base):
    """The Windows channel has never been executed -- there is no MSBuild on the
    machine it was written on -- so the least it can do is be well-formed.

    XML comments cannot contain `--`, and an em-dash rendered as `--` inside one
    makes the whole file unparseable. That was caught by hand three times while
    writing these files; catching it here costs nothing and does not depend on
    somebody remembering."""

    def _msbuild_files(self):
        root = os.path.dirname(os.path.dirname(os.path.dirname(TOOL)))
        out = []
        for pat in ("*/platform/Windows/Directory.Build.props",
                    "*/platform/Windows/Directory.Build.targets",
                    "*/platform/Windows/*/*.vcxproj"):
            out += glob.glob(os.path.join(root, pat))
        return out

    def test_every_msbuild_fragment_is_well_formed_xml(self):
        files = self._msbuild_files()
        self.assertGreater(len(files), 5, "found almost no MSBuild files to check")
        for p in files:
            try:
                ET.parse(p)
            except ET.ParseError as e:
                self.fail("%s is not well-formed XML: %s" % (p, e))

    def test_the_ide_target_reads_the_flat_defines_file(self):
        """The Visual Studio path turns on one thing: a target that runs before
        ClCompile reading a file MSBuild's property-function whitelist can
        actually read. If someone 'tidies' it into an <Import> it silently stops
        working, because an Import is evaluated before any target runs."""
        root = os.path.dirname(os.path.dirname(os.path.dirname(TOOL)))
        targets = glob.glob(os.path.join(root, "*/platform/Windows/Directory.Build.targets"))
        self.assertGreaterEqual(len(targets), 5, "expected engine + four apps")
        for p in targets:
            text = open(p, encoding="utf-8").read()
            self.assertIn('BeforeTargets="ClCompile"', text, p)
            self.assertIn("System.IO.File]::ReadAllText", text, p)
            self.assertIn("%(ClCompile.PreprocessorDefinitions)", text, p)
            self.assertNotIn("%%(ClCompile", text, "double-escaped metadata in " + p)


class TestOutputRootKey(Base):
    def test_key_separates_every_dimension(self):
        m = self.manifest("MT_CAP_LLM=0\n")
        base = self.out_of(self.resolve(m))
        for extra, label in (
            (("--arch", "x86_64"), "arch"),
            (("--config", "Release"), "config"),
        ):
            args = ["resolve", "--manifest", m, "--app", "testapp",
                    "--platform", "macos", "--arch", "arm64", "--config", "Debug",
                    "--engine-dir", ENGINE, "--out-dir", self.out_root]
            for i in range(0, len(extra), 2):
                idx = args.index(extra[i])
                args[idx + 1] = extra[i + 1]
            other = self.out_of(run(*args))
            self.assertNotEqual(base, other, "%s must key the output root" % label)

    def test_platform_keys_the_root(self):
        m = self.manifest("MT_CAP_LLM=0\n")
        a = self.out_of(self.resolve(m, platform="macos"))
        b = self.out_of(self.resolve(m, platform="linux"))
        self.assertNotEqual(a, b)

    def test_commercial_mode_keys_the_root(self):
        """The same FFmpeg archive NAME holds different decoder sets under
        MT_COMMERCIAL_BUILD, so two modes must not share a root."""
        m = self.manifest("MT_CAP_LLM=0\n")
        a = self.out_of(self.resolve(m))
        b = self.out_of(self.resolve(m, "--set", "MT_COMMERCIAL_BUILD=1"))
        self.assertNotEqual(a, b)
        self.assertIn("/full/", a)
        self.assertIn("/commercial/", b)

    def test_engine_options_key_backend_but_NOT_the_resolved_form(self):
        """They select a different llama.cpp backend: a materially different
        archive under an identical capability set. In the resolved form they would
        make `check` fail permanently, since it has no input to reproduce them."""
        m = self.manifest("MT_CAP_LLM=0\n")
        plain = self.resolve(m)
        cuda = self.resolve(m, "--engine-option", "MT_LLAMA_CUDA=1")
        self.assertNotEqual(self.out_of(plain), self.out_of(cuda))
        self.assertEqual(self.resolved_of(plain), self.resolved_of(cuda))

    def test_default_root_is_outside_every_checkout(self):
        root = R.default_build_root()
        for repo in ("MTEngineSDL", "MTEngineSDLDummyApp", "c64d", "PhotoCruise",
                     "LightHeroes"):
            self.assertNotIn(os.sep + repo + os.sep, root + os.sep)
        self.assertNotIn("mtengine/mtengine", root.replace("\\", "/"),
                         "the default already ends in /mtengine; do not append it twice")


class TestEmittedFragments(Base):
    def setUp(self):
        super().setUp()
        self.m = self.manifest("MT_CAP_LLM=0\nMT_CAP_HTTPS=0\n")
        self.p = self.resolve(self.m)
        self.out = self.out_of(self.p)

    def read(self, *parts):
        with open(os.path.join(self.out, *parts), encoding="utf-8") as f:
            return f.read()

    def read_at(self, out, *parts):
        """read(), but for a DIFFERENT output root than setUp's -- the
        distribution-tier tests compare two resolves of one manifest."""
        with open(os.path.join(out, *parts), encoding="utf-8") as f:
            return f.read()

    def test_all_four_fragments_plus_header_and_stamp_exist(self):
        for name in ("MTEngineCapabilities.cmake", "MTEngineCaps.xcconfig",
                     "MTEngineCaps.props", "resolved.stamp"):
            self.assertTrue(os.path.isfile(os.path.join(self.out, name)), name)
        self.assertTrue(os.path.isfile(os.path.join(self.out, "include",
                                                    "MT_Capabilities.h")))

    def test_nothing_is_written_inside_any_checkout(self):
        """THE rule, asserted rather than asserted-about."""
        self.assertNotIn(ENGINE, self.out)

    def test_header_never_defines_the_commercial_flag(self):
        """MT_Capabilities.h is on the include path of every engine and app TU, and
        every guard on this flag is presence-style, so a `#define ... 0` would take
        all 30 sites down the COMMERCIAL branch -- including the licence-audit
        tests written to catch that."""
        self.assertNotIn("define MT_COMMERCIAL_BUILD", self.read("include",
                                                                "MT_Capabilities.h"))

    def test_commercial_define_is_ABSENT_when_off_and_present_when_on(self):
        # The CMake fragment emits KEYS and a MT_CAPS_COMMERCIAL flag, not a
        # frozen KEY=VALUE list, so the assertion is on the flag: 0 means
        # mt_apply_capabilities appends no MT_COMMERCIAL_BUILD define at all.
        #
        # Not a whole-file grep: MT_CAPS_RESOLVED legitimately contains
        # MT_COMMERCIAL_BUILD=0, because the canonical form carries it as 0/1 so
        # the check can tell "off" from "unset". The exception is about the define
        # the build EMITS, and a whole-file grep would forbid the very distinction
        # it draws.
        cmake = self.read("MTEngineCapabilities.cmake")
        self.assertIn("set(MT_CAPS_COMMERCIAL 0)", cmake)
        # The CMake fragment is a PROGRAM now, not a data list, so no textual
        # assertion can express "the define is absent" -- the line that would emit
        # it exists, inside `if(MT_CAPS_COMMERCIAL)`. Three successive textual
        # assertions here were each too broad, and each was written one line under
        # a comment explaining why a broad grep is wrong.
        #
        # So RUN IT. cmake_defines_for() configures a throwaway project against
        # the real fragment and reads the flags CMake actually emitted.
        defs = self.cmake_defines_for(self.out)
        if defs is not None:
            self.assertNotIn("-DMT_COMMERCIAL_BUILD=0", defs)
            self.assertNotIn("-DMT_COMMERCIAL_BUILD=1", defs)
            self.assertIn("-DMT_ENABLE_MBEDTLS=0", defs,
                          "an off flag must still be emitted, explicitly, as 0")
        props = self.read("MTEngineCaps.props")
        pre = props.split("<PreprocessorDefinitions>", 1)[1].split("</", 1)[0]
        self.assertNotIn("MT_COMMERCIAL_BUILD", pre)
        on = self.out_of(self.resolve(self.m, "--set", "MT_COMMERCIAL_BUILD=1"))
        with open(os.path.join(on, "MTEngineCaps.props"), encoding="utf-8") as f:
            self.assertIn("MT_COMMERCIAL_BUILD=1", f.read())
        on_defs = self.cmake_defines_for(on)
        if on_defs is not None:
            self.assertIn("-DMT_COMMERCIAL_BUILD=1", on_defs)

    def test_private_define_is_ABSENT_when_off_and_present_when_on(self):
        """MT_PRIVATE_BUILD follows MT_COMMERCIAL_BUILD's presence-style rule
        exactly: absent when off, never =0, because a `#ifdef MT_PRIVATE_BUILD`
        would be TRUE for a define of 0 and send a public build down the
        private branch -- the precise failure this flag exists to prevent."""
        props = self.read("MTEngineCaps.props")
        pre = props.split("<PreprocessorDefinitions>", 1)[1].split("</", 1)[0]
        self.assertNotIn("MT_PRIVATE_BUILD", pre)

        on = self.out_of(self.resolve(self.m, "--set", "MT_PRIVATE_BUILD=1"))
        with open(os.path.join(on, "MTEngineCaps.props"), encoding="utf-8") as f:
            self.assertIn("MT_PRIVATE_BUILD=1", f.read())

        cmake = self.read("MTEngineCapabilities.cmake")
        self.assertIn("set(MT_CAPS_PRIVATE 0)", cmake)
        defs = self.cmake_defines_for(self.out)
        if defs is not None:
            self.assertNotIn("-DMT_PRIVATE_BUILD=0", defs)
            self.assertNotIn("-DMT_PRIVATE_BUILD=1", defs)
        on_defs = self.cmake_defines_for(on)
        if on_defs is not None:
            self.assertIn("-DMT_PRIVATE_BUILD=1", on_defs)

    def test_header_never_defines_the_private_flag(self):
        """Same hazard as the commercial flag, same assertion. The canonical
        string literal inside MT_GetCapabilityManifest() legitimately CONTAINS
        the text, so this checks for a #define specifically."""
        self.assertNotIn("define MT_PRIVATE_BUILD", self.read("include",
                                                              "MT_Capabilities.h"))

    def test_private_only_library_needs_the_private_tier(self):
        """libheif is distribution-restricted: its HEVC patent position is
        unsettled for anything that leaves the building, and 'the app is open
        source' does not settle it -- that is a copyright answer to a patent
        question. So MT_CAP_PHOTO_CODECS=1 alone must NOT turn the library on;
        only MT_PRIVATE_BUILD=1 may. The sibling BSD-family codecs under the
        same capability must be unaffected, which is the half a
        capability-level workaround would have broken."""
        pub = self.out_of(self.resolve(self.manifest("MT_CAP_PHOTO_CODECS=1\n")))
        pub_cmake = self.read_at(pub, "MTEngineCapabilities.cmake")
        self.assertIn("set(MT_ENABLE_LIBHEIF 0)", pub_cmake)
        for sibling in ("MT_ENABLE_LIBTIFF", "MT_ENABLE_LIBWEBP", "MT_ENABLE_LIBAVIF"):
            self.assertIn("set(%s 1)" % sibling, pub_cmake,
                          "%s is BSD-family and must not be collateral" % sibling)

        priv = self.out_of(self.resolve(
            self.manifest("MT_CAP_PHOTO_CODECS=1\nMT_PRIVATE_BUILD=1\n")))
        self.assertIn("set(MT_ENABLE_LIBHEIF 1)", self.read_at(priv, "MTEngineCapabilities.cmake"))

    def test_commercial_and_private_together_are_a_configure_error(self):
        """One artifact cannot be both sold and never distributed. Naming both
        keys and refusing to choose is the same contract the implication
        conflict honours -- only the author knows which this build is."""
        proc = self.resolve(
            self.manifest("MT_COMMERCIAL_BUILD=1\nMT_PRIVATE_BUILD=1\n"))
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("MT_COMMERCIAL_BUILD", proc.stderr)
        self.assertIn("MT_PRIVATE_BUILD", proc.stderr)

    def test_private_tier_keys_the_output_root(self):
        """A private and a public build of one manifest link different
        libraries, so they must not share an $MT_OUT or one silently reuses the
        other's archives."""
        m = self.manifest("MT_CAP_PHOTO_CODECS=1\n")
        self.assertNotEqual(self.out_of(self.resolve(m)),
                            self.out_of(self.resolve(m, "--set", "MT_PRIVATE_BUILD=1")))

    def cmake_defines_for(self, out, extra=""):
        """Configure a throwaway project against the real generated fragment and
        return the defines CMake emitted. None when cmake is unavailable.

        Running it is the only way to assert on a fragment that is a program."""
        cmake = shutil.which("cmake")
        if not cmake:
            return None
        d = tempfile.mkdtemp(prefix="mtcaps-cmake-", dir=self.tmp)
        with open(os.path.join(d, "t.c"), "w") as f:
            f.write("int x;\n")
        with open(os.path.join(d, "CMakeLists.txt"), "w") as f:
            f.write("cmake_minimum_required(VERSION 3.14)\n"
                    "project(t C)\n"
                    'include("%s/MTEngineCapabilities.cmake")\n' % out.replace("\\", "/")
                    + extra +
                    "add_library(t STATIC t.c)\n"
                    "mt_apply_capabilities(t)\n")
        proc = subprocess.run([cmake, "."], cwd=d, capture_output=True, text=True)
        if proc.returncode != 0:
            self.fail("cmake failed on the generated fragment:\n" + proc.stderr)
        flags = os.path.join(d, "CMakeFiles", "t.dir", "flags.make")
        if not os.path.isfile(flags):
            return None
        for line in open(flags):
            if line.startswith("C_DEFINES"):
                return line
        return ""

    def test_a_downgrade_after_the_include_reaches_the_compile_line(self):
        """The whole reason the fragment emits KEYS rather than a frozen
        KEY=VALUE list. CMakeLists.txt can still legitimately change a flag after
        including it -- the libheif exception does exactly that -- and with a
        frozen list the engine would compile every TU with the PRE-downgrade
        value while neither the include dirs nor the library were added: a hard
        failure behind a warning claiming the feature was disabled."""
        defs = self.cmake_defines_for(
            self.out, extra='set(MT_ENABLE_LIBHEIF OFF CACHE BOOL "" FORCE)\n')
        if defs is None:
            self.skipTest("no cmake")
        self.assertIn("-DMT_ENABLE_LIBHEIF=0", defs)
        self.assertNotIn("-DMT_ENABLE_LIBHEIF=1", defs)

    def test_generated_xcconfig_folds_nothing_into_GCC_PREPROCESSOR_DEFINITIONS(self):
        """It is READ BY the wrapper, never imported by Xcode: a
        baseConfigurationReference is a tracked path and $MT_OUT ends in a
        caps-hash, so no tracked reference can name it."""
        text = self.read("MTEngineCaps.xcconfig")
        self.assertNotIn("GCC_PREPROCESSOR_DEFINITIONS", text)
        self.assertIn("MT_CAPS_DEFINES = ", text)

    def test_every_known_ENABLE_flag_appears_with_an_explicit_value(self):
        """Emitting only the ON ones leaves the rest UNDEFINED -- and the #ifdef
        family defaults off while the value style defaults on, so "absent" means
        two different things depending on the guard it meets."""
        cmake = self.read("MTEngineCapabilities.cmake")
        # As variables...
        self.assertIn("set(MT_ENABLE_MBEDTLS 0)", cmake)
        self.assertIn("set(MT_ENABLE_LLAMA_CPP 0)", cmake)
        # ...and as keys the apply function turns into defines at CALL time.
        # Deliberately not a baked KEY=VALUE list: CMakeLists.txt can still change
        # a flag after including the fragment -- the libheif downgrade does -- and
        # a frozen list would compile every TU with the pre-downgrade value while
        # neither the include dirs nor the library were added.
        self.assertIn("set(MT_CAPS_FLAG_KEYS", cmake)
        for flag in ("MT_ENABLE_MBEDTLS", "MT_ENABLE_LLAMA_CPP", "MT_ENABLE_LIBHEIF"):
            self.assertIn("    %s\n" % flag, cmake)
        self.assertIn("foreach(_k IN LISTS MT_CAPS_CAPABILITY_KEYS MT_CAPS_FLAG_KEYS)", cmake)

    def test_caps_defines_carries_app_visible_flags_only(self):
        text = self.read("MTEngineCaps.xcconfig")
        line = [l for l in text.splitlines() if l.startswith("MT_CAPS_DEFINES = ")][0]
        self.assertIn("MT_ENABLE_MBEDTLS=0", line, "httplib.h:21 -- the ODR case")
        self.assertNotIn("MT_ENABLE_LLAMA_CPP", line, "no app guards on it")

    def test_generated_header_compiles_AND_LINKS_through_MT_HAS_CAP(self):
        """Compile-only proved nothing three rounds running: `inline bool f();`
        with no body compiles until something calls it."""
        clang = shutil.which("clang++") or shutil.which("g++")
        if not clang:
            self.skipTest("no C++ compiler")
        src = os.path.join(self.tmp, "link.cpp")
        with open(src, "w") as f:
            f.write('#include "MT_Capabilities.h"\n'
                    '#include <cstdio>\n'
                    'MT_REQUIRE_CAP(MT_CAP_HTTP);\n'
                    'int main(){ printf("%d %s\\n", (int)MT_HAS_CAP(MT_CAP_LLM),'
                    ' MT_GetCapabilityManifest()); return 0; }\n')
        exe = os.path.join(self.tmp, "link")
        proc = subprocess.run([clang, "-std=c++20", "-Wall", "-Wextra",
                               "-I", os.path.join(self.out, "include"), src, "-o", exe],
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stderr.strip(), "", "no new warnings")
        got = subprocess.run([exe], capture_output=True, text=True).stdout.strip()
        self.assertTrue(got.startswith("0 "))
        self.assertEqual(got.split(" ", 1)[1], self.resolved_of(self.p),
                         "MT_GetCapabilityManifest() must return EXACTLY the "
                         "canonical form the backstop compares against")

    def test_licenses_covers_core_AND_capabilities(self):
        """A LICENSES.txt derived from the resolved capability set ALONE is
        legally incomplete while LOOKING complete: SDL3, zlib, ALSA, GTK3,
        freetype and libuv all ship outside every capability key. Assert both
        halves, or this passes on a partial SBOM -- which is worse than an
        obviously partial one, because nobody re-checks a document that appears
        finished."""
        text = self.read("LICENSES.txt")
        for core in ("SDL3", "zlib", "ALSA", "GTK3", "freetype", "libuv"):
            self.assertIn(core, text, "%s is core and must be listed" % core)
        self.assertIn("MT_CAP_VIDEO_PLAYBACK", text)
        self.assertIn("FFmpeg", text)
        # a DISABLED capability is named too, so the document says what is NOT
        # in the binary as well as what is
        self.assertIn("MT_CAP_LLM", text)
        self.assertIn(self.resolved_of(self.p), text)

    def test_licenses_states_a_licence_for_every_listed_dependency(self):
        text = self.read("LICENSES.txt")
        checked = 0
        for line in text.splitlines():
            if line.strip().startswith("licence"):
                value = line.split(":", 1)[1].strip()
                self.assertTrue(value and value.lower() != "unknown", line)
                checked += 1
        self.assertGreater(checked, 5, "the SBOM listed almost nothing")

    def test_MT_REQUIRE_CAP_fails_for_an_OFF_capability(self):
        clang = shutil.which("clang++") or shutil.which("g++")
        if not clang:
            self.skipTest("no C++ compiler")
        src = os.path.join(self.tmp, "neg.cpp")
        with open(src, "w") as f:
            f.write('#include "MT_Capabilities.h"\nMT_REQUIRE_CAP(MT_CAP_LLM);\n')
        proc = subprocess.run([clang, "-std=c++20", "-fsyntax-only",
                               "-I", os.path.join(self.out, "include"), src],
                              capture_output=True, text=True)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("is required by this host", proc.stderr)


class TestDepsDir(Base):
    """Where third-party archives go, and what discriminates two of them.

    Keyed by the RESOLVED CAPABILITY SET and the things that change an archive
    without being capabilities -- not by app or engine revision, which would cost
    a full rebuild of SDL3, FFmpeg, libvpx and llama.cpp per commit, per app, to
    produce a byte-identical result.
    """

    CAPS = "MT_CAP_LLM=1\n"

    def test_shape_and_components(self):
        deps = self.deps_of(self.resolve(self.manifest(self.CAPS)))
        parts = deps.split(os.sep)
        self.assertEqual(parts[-1], "libs")
        self.assertEqual(parts[-7], "_deps")
        self.assertEqual(parts[-6], "macos")
        self.assertEqual(parts[-5], "arm64")
        self.assertEqual(parts[-4], "common")   # <config>, not a Windows build
        self.assertEqual(parts[-3], "default")  # <backend>, no --engine-option
        self.assertEqual(len(parts[-2]), 12)    # <caps-hash>

    def test_caps_hash_is_the_documented_sha_prefix(self):
        """Byte-identical to what the retired mt_caps_deps_key produced, or every
        existing cache is orphaned by a formatting change."""
        import hashlib
        proc = self.resolve(self.manifest(self.CAPS))
        expected = hashlib.sha256(
            self.resolved_of(proc).encode("utf-8")).hexdigest()[:12]
        self.assertEqual(self.deps_of(proc).split(os.sep)[-2], expected)

    def test_capability_set_discriminates(self):
        a = self.deps_of(self.resolve(self.manifest("MT_CAP_LLM=1\n")))
        b = self.deps_of(self.resolve(self.manifest("MT_CAP_LLM=0\n", "off.caps")))
        self.assertNotEqual(a, b)

    def test_backend_discriminates(self):
        """MT_LLAMA_CUDA / MT_GGML_NATIVE select a materially different llama.cpp
        under an IDENTICAL capability set, and the archive's stamp carries no
        backend component -- so only the path can tell them apart."""
        m = self.manifest(self.CAPS)
        a = self.deps_of(self.resolve(m))
        b = self.deps_of(self.resolve(m, "--engine-option", "MT_GGML_NATIVE=OFF"))
        self.assertNotEqual(a, b)

    def test_arch_discriminates(self):
        """Only macOS builds universal archives. A Linux x64 and an ARM64 archive
        share a name and nothing else."""
        m = self.manifest(self.CAPS)
        a = self.deps_of(self.resolve(m, platform="linux", arch="x86_64"))
        b = self.deps_of(self.resolve(m, platform="linux", arch="aarch64"))
        self.assertNotEqual(a, b)

    def test_config_separates_on_windows(self):
        """MSVC cannot mix a Debug CRT (/MDd) and a Release CRT (/MD), and every
        Windows acquisition script already stamps $Configuration."""
        m = self.manifest(self.CAPS)
        dbg = self.deps_of(self.resolve(m, platform="windows", arch="x64", config="Debug"))
        rel = self.deps_of(self.resolve(m, platform="windows", arch="x64", config="Release"))
        self.assertNotEqual(dbg, rel)
        self.assertEqual(dbg.split(os.sep)[-4], "Debug")

    def test_config_does_not_separate_elsewhere(self):
        """macOS and Linux build third-party archives once for both configs. A
        <config> component there would double the cache and rebuild SDL3, FFmpeg
        and llama.cpp to produce archives a Debug app links unchanged."""
        m = self.manifest(self.CAPS)
        for platform, arch in (("macos", "arm64"), ("linux", "x86_64")):
            a = self.deps_of(self.resolve(m, platform=platform, arch=arch, config="Debug"))
            b = self.deps_of(self.resolve(m, platform=platform, arch=arch, config="Release"))
            self.assertEqual(a, b, "%s must not split the deps dir by config" % platform)
            self.assertEqual(a.split(os.sep)[-4], "common")

    def test_app_does_not_discriminate(self):
        """The whole point: two apps that resolve alike share one build."""
        m = self.manifest(self.CAPS)
        a = self.deps_of(self.resolve(m, app="AppOne"))
        b = self.deps_of(self.resolve(m, app="AppTwo"))
        self.assertEqual(a, b)

    def test_engine_revision_does_not_appear(self):
        """out_dir carries the engine revision; deps_dir must not. An engine
        commit is worse than irrelevant to a third-party archive."""
        proc = self.resolve(self.manifest(self.CAPS))
        rev = R.engine_rev(ENGINE)
        self.assertIn(rev, self.out_of(proc))
        self.assertNotIn(rev, self.deps_of(proc))

    def test_print_deps_dir_gives_one_bare_line(self):
        proc = self.resolve(self.manifest(self.CAPS), "--print", "deps-dir")
        lines = proc.stdout.strip().splitlines()
        self.assertEqual(len(lines), 1, proc.stdout)
        self.assertTrue(lines[0].endswith("libs"), lines[0])

    def test_existing_print_modes_still_give_one_bare_line(self):
        """The third contract line must not leak into --print."""
        m = self.manifest(self.CAPS)
        for mode in ("resolved", "out-dir"):
            proc = self.resolve(m, "--print", mode)
            self.assertEqual(len(proc.stdout.strip().splitlines()), 1,
                             "%s: %s" % (mode, proc.stdout))


if __name__ == "__main__":
    unittest.main(verbosity=2)
