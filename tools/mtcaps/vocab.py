"""Vocabulary loading and validation for mtcaps.

Python 3, stdlib only (decision 9). There is no JSON Schema validator in the
stdlib, so `vocabulary.schema.json` is the *specification* and this module is the
*implementation* of it. That split is deliberate and is stated here rather than
left for a reader to discover: the schema file is what a human or an editor reads,
this code is what actually runs, and the two must be changed together. A rule that
exists only in the schema file is a rule nothing enforces.

Validation is a startup step, not a CI-only check: mtcaps validates before doing
anything else and exits 2 naming the offending key and the rule it broke.
"""

import json
import os
import re

from errors import VocabError

CAP_KEY_RE = re.compile(r"^MT_CAP_[A-Z][A-Z0-9_]*$")
SHORT_RE = re.compile(r"^[A-Z][A-Z0-9_]*$")
FLAG_RE = re.compile(r"^MT_(ENABLE|CAMERA)_[A-Z0-9_]*$")
PLATFORMS = ("linux", "macos", "windows")
COMMERCIAL_EFFECTS = ("none", "capability-off", "variant")

# The licence switch. It is NOT a capability and never appears in `capabilities`:
# it is an orthogonal per-build-mode input that can veto or narrow one.
COMMERCIAL_KEY = "MT_COMMERCIAL_BUILD"

# The DISTRIBUTION switch, and the second half of a pair COMMERCIAL_KEY alone
# could not express. Not a capability either, for the same reason.
#
# WHY A SECOND KEY RATHER THAN A THIRD VALUE ON THE FIRST. There are three
# distribution tiers, not two, and the middle one is the one that bites:
#
#   private        built and kept; never handed to anyone. May include
#                  anything that builds.
#   public/free    distributed publicly at no cost (c64d's GitHub Actions
#                  release). Copyright licensing is easy here -- an open
#                  source project satisfies LGPL relinking trivially -- but
#                  PATENT terms (HEVC's pools above all) can attach to
#                  DISTRIBUTION whether or not money changes hands, so this
#                  tier is NOT automatically as permissive as private.
#   commercial     sold / app-store. Strictest.
#
# MT_COMMERCIAL_BUILD=0 conflates the first two, and they are genuinely
# different: one commercial app's non-commercial build is private (its author's own
# dev/test build, not shipped), while c64d's non-commercial build is
# published to the world. Same flag value, opposite eligibility.
#
# So: MT_PRIVATE_BUILD=1 means "this artifact is never distributed", and only
# that tier may pull in a dependency whose distribution terms are unsettled.
# Default 0 -- the SAFE default, because an app that forgets to say is far
# more likely to be shipping something than not.
PRIVATE_KEY = "MT_PRIVATE_BUILD"

# Debug symbols in Release builds (unification plan, decision 0.5). A
# build-settings knob, NOT a capability and NOT a C define -- it must never be
# guardable in code, so emit.py writes its VALUE into the fragments and no
# define anywhere. MT_COMMERCIAL_BUILD=1 forces it to 0 unless the invocation
# explicitly `--set`s it back (the UAT case); see resolve.resolve(). It joins
# the canonical form (a UAT and a store artifact must never share one $MT_OUT)
# but NOT the deps hash (an archive is byte-identical either way); see
# resolve.deps_form().

REQUIRED_CAP_FIELDS = (
    "short", "description", "default", "kind", "implies", "enables",
    "app_visible", "dependencies", "commercial", "symbols",
)
SYMBOLS_KEY = "MT_RELEASE_SYMBOLS"

REQUIRED_DEP_FIELDS = ("name", "licence", "version", "provenance")


class Vocabulary:
    def __init__(self, data, path):
        self.path = path
        self.version = data["version"]
        self.core = data["core"]
        self.capabilities = data["capabilities"]
        # Byte-value sort (C locale) once, here, so every consumer sees the same
        # order. #4.2 pins the canonical form's ordering and two implementers
        # would otherwise produce two different strings for "the same" set.
        self.keys = sorted(self.capabilities.keys())

    def get(self, key):
        return self.capabilities[key]

    def short(self, key):
        return self.capabilities[key]["short"]

    def default(self, key):
        return self.capabilities[key]["default"]

    def implies(self, key):
        return self.capabilities[key]["implies"]

    def enables(self, key):
        return self.capabilities[key]["enables"]

    def app_visible(self, key):
        return self.capabilities[key]["app_visible"]

    def acquisition(self, key):
        """The per-platform scripts that BUILD this capability's dependency into
        the keyed deps bucket, or None when it owns no such archive (vendored,
        header-only, or compiled straight into the engine).

        Presence is what makes a capability part of the deps key (L16,
        resolve.deps_key_capabilities): a capability with an acquisition script
        owns an archive whose content its value can change."""
        return self.capabilities[key].get("acquisition")

    def all_dependencies(self):
        """Core dependencies first, then per-capability, as (owner, dep) pairs."""
        for dep in self.core["dependencies"]:
            yield ("core", dep)
        for key in self.keys:
            for dep in self.capabilities[key]["dependencies"]:
                yield (key, dep)


def default_vocabulary_path():
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "vocabulary.json")


def load(path=None):
    path = path or default_vocabulary_path()
    if not os.path.isfile(path):
        raise VocabError("vocabulary not found at %s" % path)
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except json.JSONDecodeError as e:
        raise VocabError("%s is not valid JSON: %s" % (path, e))

    _validate(data, path)
    return Vocabulary(data, path)


def _err(path, where, rule):
    raise VocabError("%s: %s: %s" % (path, where, rule))


def _validate_dependency(path, where, dep, owner_enables=None):
    if not isinstance(dep, dict):
        _err(path, where, "a dependency must be an object")
    for field in REQUIRED_DEP_FIELDS:
        if field not in dep or not isinstance(dep[field], str) or not dep[field].strip():
            _err(path, "%s.%s" % (where, field),
                 "required, and a non-empty string. LICENSES.txt is generated from "
                 "these fields; a missing licence ships a legally incomplete SBOM "
                 "that looks complete")
    # The commercial deny-list input (unification plan, decision 0.2). An
    # EXPLICIT boolean, never parsed out of the licence prose: prose is for
    # humans, and a guard that greps prose is a guard that rots.
    if "commercial_safe" not in dep or not isinstance(dep["commercial_safe"], bool):
        _err(path, "%s.commercial_safe" % where,
             "required, and a boolean. resolve refuses a MT_COMMERCIAL_BUILD=1 "
             "build that would enable a dependency with commercial_safe false; "
             "a row without the field would silently sit outside that guard")
    # Optional: the single MT_ENABLE_* flag that gates this dependency, for
    # capabilities that enable several flags (the image-codec bundle). Absent
    # means "gated by the capability itself".
    if "flag" in dep:
        flag = dep["flag"]
        if not isinstance(flag, str) or not FLAG_RE.match(flag):
            _err(path, "%s.flag" % where,
                 "must be an MT_(ENABLE|CAMERA)_* flag name")
        if owner_enables is not None and flag not in owner_enables:
            _err(path, "%s.flag" % where,
                 "%r is not in the owning capability's `enables` list -- the "
                 "deny-list evaluates dependencies against the final flag set, "
                 "and a flag the capability does not emit is never in it" % flag)


def _validate(data, path):
    for field in ("version", "core", "capabilities"):
        if field not in data:
            _err(path, field, "required at the top level")

    if not isinstance(data["version"], int) or data["version"] < 1:
        _err(path, "version", "must be an integer >= 1")

    core = data["core"]
    if not isinstance(core, dict) or "dependencies" not in core:
        _err(path, "core", "required, with a `dependencies` array. Without it "
                           "LICENSES.txt omits SDL3, zlib, ALSA, GTK3, freetype, "
                           "libuv and the committed prebuilts")
    for i, dep in enumerate(core["dependencies"]):
        _validate_dependency(path, "core.dependencies[%d]" % i, dep)

    caps = data["capabilities"]
    if not isinstance(caps, dict) or not caps:
        _err(path, "capabilities", "required and non-empty")

    shorts = {}
    for key in sorted(caps):
        if not CAP_KEY_RE.match(key):
            _err(path, key,
                 "a capability key must match ^MT_CAP_[A-Z][A-Z0-9_]*$. bash "
                 "`source` is the binding constraint and is stricter than the "
                 "other two parsers: anything else is not an assignment, the "
                 "shell runs the line as a command, and every build-*.sh runs "
                 "`set -e`")

        if key == COMMERCIAL_KEY:
            _err(path, key,
                 "MT_COMMERCIAL_BUILD is not a capability. It is an orthogonal "
                 "licence-mode input; see the `commercial` field on each capability")

        if key == PRIVATE_KEY:
            _err(path, key,
                 "MT_PRIVATE_BUILD is not a capability. It is an orthogonal "
                 "distribution-tier input; see `private_only` on a dependency")

        if key == SYMBOLS_KEY:
            _err(path, key,
                 "MT_RELEASE_SYMBOLS is not a capability. It is a build-settings "
                 "mode key (unification plan, decision 0.5)")

        cap = caps[key]
        if not isinstance(cap, dict):
            _err(path, key, "must be an object")

        for field in REQUIRED_CAP_FIELDS:
            if field not in cap:
                _err(path, "%s.%s" % (key, field), "required")

        if not SHORT_RE.match(cap["short"]):
            _err(path, "%s.short" % key, "must match ^[A-Z][A-Z0-9_]*$")
        if cap["short"] in shorts:
            _err(path, "%s.short" % key,
                 "duplicate enumerator %r, also used by %s -- the generated enum "
                 "would not compile" % (cap["short"], shorts[cap["short"]]))
        shorts[cap["short"]] = key

        if cap["short"] == key:
            _err(path, "%s.short" % key,
                 "the enumerator must be DISTINCT from the MT_CAP_ token. The "
                 "token is an object-like macro in the same generated header, so "
                 "`enum class MTCapability { MT_CAP_LLM }` expands to `{ 0 }` and "
                 "does not compile")

        if cap["default"] not in (0, 1):
            _err(path, "%s.default" % key, "must be 0 or 1, never ON/OFF/true/false")

        if cap["kind"] not in ("engine-compiled", "dependency-only"):
            _err(path, "%s.kind" % key, "must be engine-compiled or dependency-only")

        if not isinstance(cap["app_visible"], bool):
            _err(path, "%s.app_visible" % key, "must be a boolean")

        for flag in cap["enables"]:
            if not FLAG_RE.match(flag):
                _err(path, "%s.enables" % key,
                     "%r must match ^MT_(ENABLE|CAMERA)_[A-Z0-9_]*$" % flag)

        for i, dep in enumerate(cap["dependencies"]):
            _validate_dependency(path, "%s.dependencies[%d]" % (key, i), dep,
                                 owner_enables=cap["enables"])

        for group in ("acquisition", "artefacts"):
            table = cap.get(group, {})
            if not isinstance(table, dict):
                _err(path, "%s.%s" % (key, group), "must be an object keyed by platform")
            for plat in table:
                if plat not in PLATFORMS:
                    _err(path, "%s.%s.%s" % (key, group, plat),
                         "unknown platform; expected one of %s" % (PLATFORMS,))

        comm = cap["commercial"]
        if not isinstance(comm, dict) or comm.get("effect") not in COMMERCIAL_EFFECTS:
            _err(path, "%s.commercial.effect" % key,
                 "must be one of %s. Three values, not a boolean: "
                 "MT_COMMERCIAL_BUILD keeps video playback fully on and refuses "
                 "specific decoders, which a veto could not express" % (COMMERCIAL_EFFECTS,))

        syms = cap["symbols"]
        if not isinstance(syms, dict):
            _err(path, "%s.symbols" % key, "must be an object")
        for field in ("forbidden_when_off", "required_when_on"):
            if field not in syms or not isinstance(syms[field], str):
                _err(path, "%s.symbols.%s" % (key, field),
                     "required (may be empty for a capability with no library)")

    # Implication targets must themselves be capabilities. An implication onto a
    # core concept -- audio, i18n, ICC -- is an unknown key, and this is where it
    # is caught rather than at emit time.
    for key in sorted(caps):
        for target in caps[key]["implies"]:
            if target not in caps:
                _err(path, "%s.implies" % key,
                     "%r is not a capability. If it is a core concept (audio, "
                     "i18n, ICC) it has no key by design, and the coupling belongs "
                     "in this entry's prose, not in the implication graph" % target)
            if target == key:
                _err(path, "%s.implies" % key, "a capability cannot imply itself")

    _check_implication_cycles(caps, path)


def _check_implication_cycles(caps, path):
    """Implication runs downward only. A cycle would make closure non-terminating
    and, worse, would make `X=1 implies Y=1 implies X=1` look like a conflict."""
    WHITE, GREY, BLACK = 0, 1, 2
    colour = dict.fromkeys(caps, WHITE)
    stack = []

    def visit(node):
        colour[node] = GREY
        stack.append(node)
        for target in caps[node]["implies"]:
            if colour[target] == GREY:
                cycle = stack[stack.index(target):] + [target]
                _err(path, "implies", "cycle: %s" % " -> ".join(cycle))
            if colour[target] == WHITE:
                visit(target)
        stack.pop()
        colour[node] = BLACK

    for node in sorted(caps):
        if colour[node] == WHITE:
            visit(node)
