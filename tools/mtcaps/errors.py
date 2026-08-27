"""Exit codes and the exceptions that carry them.

The codes are part of the contract, because the build steps distinguish them:
`2` names the offending key, `1` names the stale fragment.

    0  agree / success
    1  disagree -- a stale fragment, a real capability mismatch
    2  manifest or vocabulary error -- conflict, unknown key, missing input
    3  usage error
"""

EXIT_OK = 0
EXIT_DISAGREE = 1
EXIT_INPUT = 2
EXIT_USAGE = 3


class MtCapsError(Exception):
    exit_code = EXIT_INPUT


class VocabError(MtCapsError):
    """The vocabulary file is malformed. Exit 2."""
    exit_code = EXIT_INPUT


class ManifestError(MtCapsError):
    """The manifest is malformed, names an unknown key, or is incoherent. Exit 2."""
    exit_code = EXIT_INPUT


class MissingInputError(MtCapsError):
    """A capability was asked for and its inputs are absent. Exit 2.

    There is no downgrade tier (decision 13). The message must carry the exact
    command that fixes it: a build that stops with `Run: bash
    platform/Linux/build-image_codecs.sh` is strictly better than one that
    succeeds and lies about what it contains.
    """
    exit_code = EXIT_INPUT


class DisagreeError(MtCapsError):
    """The agreement check failed. Exit 1."""
    exit_code = EXIT_DISAGREE


class UsageError(MtCapsError):
    exit_code = EXIT_USAGE
