// Build cpp-httplib implementation as a single TU.
//
// We include the engine wrapper header first so the same feature macros are
// applied consistently.

// Pull in the engine's core typedefs (uint8/uint16/uint32) and the corresponding
// guard macros used by some macOS system headers.
#include "../../Core/SYS_Defs.h"

#include "httplib.h"

// NOTE: This file is vendored inside the llama.cpp submodule.
#include "../../../../other/lib/llama.cpp/vendor/cpp-httplib/httplib.cpp"
