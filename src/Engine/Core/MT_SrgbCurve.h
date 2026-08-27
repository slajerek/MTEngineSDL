#ifndef _MT_SrgbCurve_h_
#define _MT_SrgbCurve_h_

#include <cmath>
// <math.h> as well as <cmath>: powf is called UNQUALIFIED below, and <cmath>
// formally guarantees only std::powf. These call sites used to sit under
// CImageData.h's global `using namespace std;`, which this extraction
// deliberately removed -- and the whole point of a leaf header is that it be
// safe in a foreign translation unit (A1 review round 2).
#include <math.h>

// ---------------------------------------------------------------------------
// THE sRGB transfer curve -- one copy, and a LEAF header
// ---------------------------------------------------------------------------
//
// The IEC 61966-2-1 sRGB transfer curve, SIGN-SYMMETRIC and CONTINUED above
// 1.0 rather than clamped there. That continuation is not our invention: it is
// how CoreAnimation defines kCGColorSpaceExtendedSRGB (and extended Display
// P3, which shares the curve), so a value of 2.0 written into an extended
// surface means "the curve evaluated at 2.0", not "white".
//
// Sign symmetry matters as much as the above-1.0 half: a wide-gamut colour
// converted into sRGB primaries can land slightly NEGATIVE, and folding those
// to zero would quietly clip the gamut the float pipeline exists to preserve.
//
// WHY THIS IS ITS OWN FILE (S-6 A1 review round 1). It lived in CImageData.h,
// which is fine for image code and wrong for a RENDER BACKEND: CImageData.h
// pulls SYS_Main.h, CExifReader.h, png.h and -- the one that bites --
// `using namespace std;` at global scope. The Direct3D 11 backend's
// translation unit also includes <windows.h>/<d3d11.h>, and
// `using namespace std` next to rpcndr.h's global `byte` and C++17's
// `std::byte` is the classic `error C2872: 'byte': ambiguous symbol`. Nobody
// on the authoring machine can compile that TU to find out, so this header is
// <cmath> and nothing else: MT_SurfaceEncoding.h includes THIS. CImageData.h
// includes it too, so there is still exactly ONE copy of the curve and every
// existing caller is untouched.
//
// WHAT THIS DOES *NOT* DO (whole-phase review). The split covers
// MT_SurfaceEncoding.h, not the backend. CRenderBackendD3D11.cpp includes
// CImageData.h directly anyway -- it needs HalfToFloat() and
// getResultDataForUpload() -- so png.h and the global `using` DO reach that TU
// beside <windows.h>. What holds the `byte` ambiguity off there is
// `_HAS_STD_BYTE=0`, present in all four engine configurations and all four
// app configurations, and predating this stage. Do not read this paragraph as
// a promise that the hazard is gone; read it as a promise that
// MT_SurfaceEncoding.h will never be the thing that causes it.
// ---------------------------------------------------------------------------

inline float SrgbExtendedEncode(float v)
{
	const float a = (v < 0.0f) ? -v : v;
	const float e = (a <= 0.0031308f) ? (a * 12.92f)
									  : (1.055f * powf(a, 1.0f / 2.4f) - 0.055f);
	return (v < 0.0f) ? -e : e;
}

inline float SrgbExtendedDecode(float v)
{
	const float a = (v < 0.0f) ? -v : v;
	const float d = (a <= 0.04045f) ? (a / 12.92f)
									: powf((a + 0.055f) / 1.055f, 2.4f);
	return (v < 0.0f) ? -d : d;
}

#endif
