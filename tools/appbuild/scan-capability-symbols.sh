#!/usr/bin/env bash
set -euo pipefail

# scan-capability-symbols.sh <binary> <MT_OUT>
#
# THE GATE THE VOCABULARY HAS BEEN DESCRIBING SINCE THE PROGRAMME STARTED.
#
# Every capability carries symbols.forbidden_when_off and
# symbols.required_when_on. vocab.py has only ever checked that the two strings
# EXIST, and the one scanner in the tree (scan-forbidden-symbols.sh) is specific
# to FFmpeg's decoder list. So "the library really is gone when the capability
# is off" has been checked by hand, capability by capability, or not at all --
# and a stub archive that silently kept the real objects would look identical to
# a correct build from the outside.
#
# It reads the RESOLVED set rather than a manifest, because a capability can be
# off through four different routes -- an explicit 0, an OS suffix, the engine
# default, or a commercial-tier effect -- and a binary has to prove the same
# thing in all four.
#
# TWO DIRECTIONS, ONE OF WHICH IS A GATE AND ONE OF WHICH IS A REPORT, and the
# asymmetry is not a compromise -- it is what the two claims actually mean:
#
#   forbidden_when_off  A HARD GATE. If the capability is off and its symbols
#                       are in the binary, something linked a real archive
#                       where a stub was promised. There is no innocent reason
#                       for that, so it fails the build.
#
#   required_when_on    A REPORT. Its absence does NOT mean the capability is
#                       broken: these binaries link STATIC archives, and a
#                       linker pulls a member only for a symbol that is still
#                       undefined. An app that links FTXUI and never calls it
#                       has no ftxui symbol, correctly. Measured on the
#                       DummyApp the first time this ran: FTXUI, WEBSOCKETS,
#                       GAMEPADS and COLOR_MANAGEMENT all reported absent, and
#                       all four were right. So a miss prints `unused` and the
#                       reader decides whether that app should have been
#                       calling it.
#
# Exit 0: no forbidden symbol is present.
# Exit 1: at least one is, named on stdout.
# Exit 2: it could not check (bad arguments, no fragment, stripped binary).

BINARY="${1:-}"
MT_OUT="${2:-}"

if [[ -z "$BINARY" || -z "$MT_OUT" ]]; then
  echo "usage: scan-capability-symbols.sh <binary> <MT_OUT>" >&2
  exit 2
fi
if [[ ! -f "$BINARY" ]]; then
  echo "scan-capability-symbols: no such binary: $BINARY" >&2
  exit 2
fi

FRAGMENT="$MT_OUT/MTEngineCapabilities.cmake"
if [[ ! -f "$FRAGMENT" ]]; then
  echo "scan-capability-symbols: no capability fragment at $FRAGMENT" >&2
  echo "  (pass the MT_CAPS_OUT that 'mtcaps resolve' printed for this build)" >&2
  exit 2
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VOCAB="$ROOT_DIR/tools/mtcaps/vocabulary.json"
if [[ ! -f "$VOCAB" ]]; then
  echo "scan-capability-symbols: no vocabulary at $VOCAB" >&2
  exit 2
fi

# MANGLED names, deliberately not demangled: the vocabulary's patterns are
# written against the mangled spellings (_ZN2cv3Mat, ImGuiTestEngine_, llama_)
# so that one pattern can serve a C++ namespace and a C prefix alike.
SYMBOLS="$(nm -g "$BINARY" 2>/dev/null || nm "$BINARY" 2>/dev/null || true)"
if [[ -z "$SYMBOLS" ]]; then
  echo "scan-capability-symbols: nm produced nothing for $BINARY -- fully stripped?" >&2
  exit 2
fi

FAILED=0

# TAB, not '|', and this is a correctness fix rather than a style one: nearly
# every pattern in the vocabulary is an ALTERNATION -- `basisu|ktx2`,
# `avcodec_|avformat_|vpx_`, `_ZN2cv|@cv@@` -- and with IFS='|' the shell split
# each of those into extra fields. `forbidden` then held only the first
# alternative and `required` held the rest of the pattern instead of its own
# value, so a capability could be reported ok on the strength of half a check.
# A tab cannot appear in these patterns.
while IFS=$'\t' read -r key value forbidden required; do
  [[ -z "$key" ]] && continue

  if [[ "$value" == "0" && -n "$forbidden" ]]; then
    if grep -qE "$forbidden" <<< "$SYMBOLS"; then
      echo "FAIL  $key=0 but the binary defines symbols matching /$forbidden/"
      grep -oE "$forbidden" <<< "$SYMBOLS" | sort -u | head -3 | sed 's/^/          /'
      FAILED=1
    else
      echo "ok    $key=0, nothing matches /$forbidden/"
    fi
  elif [[ "$value" == "1" && -n "$required" ]]; then
    if grep -qE "$required" <<< "$SYMBOLS"; then
      echo "ok    $key=1, /$required/ present"
    else
      # NOT a failure: see the two-directions note at the top.
      echo "unused $key=1 but nothing matches /$required/ -- linked, never called"
    fi
  fi
done < <(python3 - "$VOCAB" "$FRAGMENT" <<'PY'
import json, re, sys

vocab = json.load(open(sys.argv[1], encoding="utf-8"))["capabilities"]

# The fragment emits one `set(MT_CAP_X 0|1)` line per capability. Parsed rather
# than sourced because this script must not run CMake to ask a question this
# cheap.
values = {}
for line in open(sys.argv[2], encoding="utf-8"):
    m = re.match(r"\s*set\((MT_CAP_[A-Z0-9_]+)\s+([01])\)", line)
    if m:
        values[m.group(1)] = m.group(2)

for key in sorted(vocab):
    symbols = vocab[key].get("symbols", {})
    print("%s\t%s\t%s\t%s" % (key,
                               values.get(key, ""),
                               symbols.get("forbidden_when_off", ""),
                               symbols.get("required_when_on", "")))
PY
)

if [[ "$FAILED" != "0" ]]; then
  echo "scan-capability-symbols: FAILED for $BINARY" >&2
  echo "  A capability resolved to 0 but its library is in the binary: the" >&2
  echo "  acquisition script wrote a real archive where a stub was promised," >&2
  echo "  or a build system linked something the capability set excluded." >&2
fi
exit "$FAILED"
