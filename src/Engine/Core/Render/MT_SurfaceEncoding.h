#ifndef _MT_SurfaceEncoding_h_
#define _MT_SurfaceEncoding_h_

// ---------------------------------------------------------------------------
// Surface encoding conversions -- extended sRGB <-> scRGB <-> PQ (S-6 Task A1)
// ---------------------------------------------------------------------------
//
// WHAT THIS IS FOR. Windows has no equivalent of macOS's
// kCGColorSpaceExtendedSRGB. DXGI's float HDR colour space is scRGB
// (DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) and the "G10" in that name means
// gamma 1.0 -- LINEAR. Everything this engine produces is sRGB-ENCODED, so the
// D3D11 backend renders into an offscreen RGBA16F target exactly as it always
// has and runs ONE present-time resolve pass that decodes the extended-sRGB
// curve and rescales into scRGB. The arithmetic of that pass lives here.
//
// WHY IT LIVES IN A HEADER OF ITS OWN, on a machine that cannot compile a line
// of Direct3D: because it is the ONLY part of S-6 that can be proven before
// anybody reaches a Windows box. CTestSurfaceEncoding pins the curve to an
// ABSOLUTE value (0.5 decodes to 0.21404, computed from the published IEC
// formula, not observed from a run), pins the white-level scale's DIRECTION,
// pins the SDR arm as a bit-exact identity, and proves PqInverseEotf is a true
// inverse by round-tripping PqEotf. Everything else in Subphase A is
// unverified text until Subphase B; this is not.
//
// PLATFORM-NEUTRAL ON PURPOSE. No <d3d11.h>, no DXGI enums, no SDL. It is
// registered in all three build systems and is TESTED ON macOS, where
// CTestSurfaceEncoding runs as part of the CTest suite. The Linux run is OWED
// -- do not read this comment as a claim that it has happened (A1 review round
// 2 caught the first version asserting both).
//
// THE HLSL COPY IS platform/Windows/src.Windows/Render/Shaders/Resolve.hlsl
// (Task A4). It transcribes EncodedSrgbToScRgb / ResolveToSurface because a
// shader cannot include a C++ header, and the two MUST be updated together --
// the same rule, and for the same reason, as CVideoTransferFunctions.h's THREE
// shader siblings (MSL, GLSL, HLSL).
//
// ---------------------------------------------------------------------------
// THE ONE THING THIS FILE EXISTS TO GET RIGHT: the white-level scale is a
// MULTIPLY.
// ---------------------------------------------------------------------------
//
// SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT is documented as "the value of SDR
// white in the SDL_COLORSPACE_SRGB_LINEAR colorspace. On Windows this
// corresponds to the SDR white level in scRGB colorspace" (SDL_video.h). Read
// that precisely: it is the scRGB value SDR white BECOMES, not a divisor. With
// Windows 11's common 200-nit SDR white the property reads 2.5 (200/80), every
// SDR window on the desktop is composited with its white at 2.5, and for our UI
// to sit beside them at the same brightness OUR 1.0 must be emitted as 2.5.
//
// Dividing instead -- the first draft of the S-6 plan -- would put the whole
// application at 0.4 scRGB = 32 nits next to 200-nit SDR windows, AND would
// make the app get DARKER as the user turned the Windows SDR-brightness slider
// UP. CTestSurfaceEncoding asserts the direction explicitly, and asserts
// monotonicity in the slider, because that inversion is invisible to anything
// that only checks endpoints.
//
// 203 NITS DOES NOT APPEAR IN THE scRGB PATH, and that is deliberate. Our
// pipeline's 1.0 is "SDR reference white"; VideoTransfer::kSdrReferenceWhiteNits
// (203, BT.2408) is only the anchor that maps PQ/HLG ABSOLUTE nits into that
// convention. How many nits the display then shows for our white is the OS's
// decision -- macOS makes it with display brightness (SDL reports 1.0 there),
// Windows with the SDR slider -- and SDL hands the answer over already in the
// units the swapchain wants. 203 enters EncodedSrgbToPq alone, where absolute
// nits are the entire point.
// ---------------------------------------------------------------------------

#include <cmath>
// SrgbExtendedEncode / SrgbExtendedDecode -- THE copy of the IEC 61966-2-1
// curve, sign-symmetric and continued past 1.0. Re-deriving it here rather
// than including it is exactly the mistake S-5 Phase 5 spent three review
// rounds preventing.
//
// MT_SrgbCurve.h, NOT CImageData.h. The curve used to live in CImageData.h,
// which drags SYS_Main.h, CExifReader.h, png.h and a global
// `using namespace std` -- and this header is included by a translation unit
// that also includes <windows.h> and <d3d11.h>, where `using namespace std`
// beside rpcndr.h's `byte` and C++17's `std::byte` is a well-known ambiguity
// error. Nobody here can compile that TU to find out, so A1 review round 1
// split the curve into a leaf header instead of leaving the hazard for
// Subphase B to discover. That protects THIS header only -- the backend
// includes CImageData.h on its own account, where `_HAS_STD_BYTE=0` is what
// does the work. See MT_SrgbCurve.h.
#include "MT_SrgbCurve.h"

namespace MTSurfaceEncoding
{
	// scRGB's own reference white, per IEC 61966-2-2: scRGB 1.0 is 80 nits,
	// always. This is the constant that turns SDL's float into a nit count and
	// back, and it is the reason the property reads 2.5 for a 200-nit SDR white.
	// `inline constexpr`, not `static const`: a namespace-scope `static const`
	// in a header has INTERNAL linkage, so each translation unit's copy of the
	// external-linkage inline functions below would name a different entity --
	// a technical ODR violation no compiler diagnoses (A1 review round 1).
	inline constexpr float kScRgbReferenceWhiteNits = 80.0f;

	// PQ's absolute peak. The same value as VideoTransfer::kPqPeakNits; the two
	// are pinned equal by CTestSurfaceEncoding rather than merely intended to
	// be, because this header deliberately does NOT include
	// CVideoTransferFunctions.h (that one lives under Engine/Video and this one
	// must stay includable from render code with no video include path).
	inline constexpr float kPqPeakNits = 10000.0f;

	// nits -> the scRGB value that many nits of SDR white lands on. Exists so
	// the nits<->scRGB convention is written down ONCE and tested; the backend
	// itself reads SDL's float directly and never converts through nits.
	//
	// TASK B4 USES THIS FORWARD, and getting the direction wrong there is a
	// 6400x error in the headroom that then feeds tone-mapping. The headroom
	// ratio is ScRgbSdrWhiteFromNits(IDXGIOutput6::GetDesc1().MaxLuminance)
	// divided by SDL's SDR_WHITE_LEVEL float -- i.e. MaxLuminance/80 over the
	// SDR white in scRGB. That is exactly what SDL itself does in
	// SDL_windowsmodes.c; the INVERSE (x 80) is only for turning SDL's float
	// back into a nit count for a log line.
	inline float ScRgbSdrWhiteFromNits(float sdrWhiteNits)
	{
		return sdrWhiteNits / kScRgbReferenceWhiteNits;
	}

	// One channel: our extended-sRGB-ENCODED value -> scRGB LINEAR.
	//
	// `sdrWhiteScRgb` is SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT verbatim, so our
	// 1.0 lands exactly on the display's SDR white, beside every SDR window.
	// MULTIPLY -- see the header comment; this is the line the whole file is
	// about. Sign symmetry and the above-1.0 continuation both come free from
	// SrgbExtendedDecode and are asserted anyway, because a wide-gamut colour
	// converted into sRGB primaries can land NEGATIVE and folding that to zero
	// clips exactly the gamut a float pipeline exists to preserve.
	inline float EncodedSrgbToScRgb(float v, float sdrWhiteScRgb)
	{
		return SrgbExtendedDecode(v) * sdrWhiteScRgb;
	}

	// THE resolve-pass function, one channel, for BOTH swapchain kinds.
	//
	// An SDR swapchain (R8G8B8A8_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)
	// wants the values exactly as the pipeline wrote them -- sRGB-encoded -- so
	// the resolve is the IDENTITY there, bit for bit. Only a linear (G10, FP16)
	// swapchain decodes and rescales.
	//
	// This is NOT "the same maths at scale 1.0". SrgbExtendedDecode(0.5) is
	// 0.214, so decoding on an SDR swapchain would display mid-grey as 0.21 --
	// a ~73-LSB error across the entire UI on every non-HDR Windows machine,
	// which is S-4's washed-out surface bug inverted onto the DEFAULT path.
	// Review round 2 of the S-6 plan caught the first draft specifying exactly
	// that, and CTestSurfaceEncoding pins the identity on values that would
	// expose it.
	//
	// The SAME shader and the SAME pass run in both modes, so there is no
	// HDR-only code path to rot while nobody looks at it.
	inline float ResolveToSurface(float v, bool swapchainIsLinear, float sdrWhiteScRgb)
	{
		return swapchainIsLinear ? EncodedSrgbToScRgb(v, sdrWhiteScRgb) : v;
	}

	// The ST 2084 (PQ) inverse EOTF: absolute luminance 0..1, where 1.0 means
	// kPqPeakNits, -> PQ code 0..1.
	//
	// CVideoTransferFunctions.h has PqEotf and deliberately NO inverse ("THE ONE
	// COPY" -- an inverse there would be a second formula to keep in step with
	// its three shader transcriptions). This one is the inverse of THAT function
	// and nothing else, and CTestSurfaceEncoding proves it by round-tripping
	// through PqEotf rather than against a hardcoded decimal.
	inline float PqInverseEotf(float absLuminance01)
	{
		const float m1 = 0.1593017578125f, m2 = 78.84375f;
		const float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
		float y = absLuminance01;
		if (y < 0.0f) y = 0.0f;
		if (y > 1.0f) y = 1.0f;
		const float p = powf(y, m1);
		return powf((c1 + c2 * p) / (1.0f + c3 * p), m2);
	}

	// One channel: our extended-sRGB-encoded value -> PQ code (0..1), for the
	// HDR10 10-bit swapchain if it is ever chosen over scRGB (S-6 Open Question
	// 2). NOT used by the shipping resolve pass.
	//
	// This is the ONE place 203 nits enters: PQ is an ABSOLUTE encoding, so our
	// "1.0 == SDR reference white" has to be pinned to a nit value before it
	// means anything, and VideoTransfer::kSdrReferenceWhiteNits is that pin.
	// It is a PARAMETER rather than a constant here so this header stays free of
	// the video include path; every caller passes that constant.
	inline float EncodedSrgbToPq(float v, float referenceWhiteNits)
	{
		const float linear = SrgbExtendedDecode(v);           // 1.0 == reference white
		const float nits   = linear * referenceWhiteNits;     // absolute
		return PqInverseEotf(nits / kPqPeakNits);
	}
}

#endif
