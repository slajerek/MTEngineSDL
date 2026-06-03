#pragma once

// Wrapper for vendored cpp-httplib (from llama.cpp).
//
// Important:
// - We enable the mbedTLS backend to support HTTPS downloads without relying on
//   external tools.
// - We also enable platform root certificate loading where supported.

#if defined(_WIN32)
// cpp-httplib requires Windows 10+ when _WIN32_WINNT is explicitly set.
// Some legacy headers in the engine set this lower, so force it up here.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#elif _WIN32_WINNT < 0x0A00
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#endif

#if !defined(MT_ENABLE_MBEDTLS) || (MT_ENABLE_MBEDTLS)
#ifndef CPPHTTPLIB_MBEDTLS_SUPPORT
#define CPPHTTPLIB_MBEDTLS_SUPPORT 1
#endif
#endif

#if defined(__APPLE__) && !defined(CPPHTTPLIB_USE_CERTS_FROM_MACOSX_KEYCHAIN)
#define CPPHTTPLIB_USE_CERTS_FROM_MACOSX_KEYCHAIN 1
#endif

#if defined(_WIN32) && !defined(CPPHTTPLIB_WINDOWS_AUTOMATIC_ROOT_CERTIFICATES_UPDATE)
#define CPPHTTPLIB_WINDOWS_AUTOMATIC_ROOT_CERTIFICATES_UPDATE 1
#endif

#include "../../../../other/lib/llama.cpp/vendor/cpp-httplib/httplib.h"
