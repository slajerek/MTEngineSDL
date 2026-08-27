#ifndef _CVideoTransferFunctions_h_
#define _CVideoTransferFunctions_h_

#include <cmath>
#include <vector>
#include "SYS_Defs.h"   // u16

// ---------------------------------------------------------------------------
// HDR video transfer functions -- THE ONE COPY (S-5 Phase 5, Task 1)
// ---------------------------------------------------------------------------
//
// PQ (SMPTE ST 2084) and HLG (ARIB STD-B67) decode, the HLG display OOTF, and
// the BT.2020 -> sRGB primaries matrix. These lived as file-static helpers
// inside CVideoFrameExtractor.cpp, where only the POSTER lane could reach
// them. Phase 5 teaches the live playback shader the same maths, and two
// copies of a transfer function is precisely how a poster and its own playing
// frames drift apart -- which is the bug that phase exists to remove. So the
// maths moved here and the extractor's local copies were deleted, not left
// beside it.
//
// THE SHADER COPIES ARE GENERATED FROM THESE VALUES AND MUST BE UPDATED
// TOGETHER. There are THREE of them:
//   * platform/MacOS/src.MacOS/Render/CVideoYUVShaderMetal.mm         (MSL)
//   * src/Engine/Video/CVideoYUVShader.cpp                            (GLSL)
//   * platform/Windows/src.Windows/Render/Shaders/VideoYUV.hlsl       (HLSL)
// and none of them can include this header, because shader source is a string
// compiled by the driver (MSL, GLSL) or bytecode compiled by fxc/dxc (HLSL),
// never a translation unit. The HLSL copy arrived with S-6; a note that named
// only two of three would have read as complete.
// That is not a licence to let them drift: the ImGui test `hdr_shader_agrees_with_transfer_header` evaluates a
// ramp of code values through the REAL compiled shader on the GPU, reads it
// back, and compares against these functions on the CPU. A drifted
// shader constant fails THERE, with a named value, rather than showing up as
// an unexplained tolerance miss in a whole-frame comparison.
//
// WHY 203: SDR reference white is 203 nits (ITU-R BT.2408), and it is the
// anchor for the whole float pipeline -- the same white the surface's HDR10
// metadata describes. Output of the functions below is "1.0 == SDR reference
// white", so a value of 2.0 genuinely means twice as bright as paper white.
// One reference across the pipeline, or the parts would be describing
// different whites.

namespace VideoTransfer
{
	// SDR reference white in nits (BT.2408). The divisor that turns absolute
	// luminance into the pipeline's "1.0 == reference white" convention.
	static const float kSdrReferenceWhiteNits = 203.0f;

	// PQ is an ABSOLUTE encoding: its 1.0 means 10 000 nits, always.
	static const float kPqPeakNits = 10000.0f;

	// HLG is RELATIVE, so a nominal peak has to be chosen. 1000 nits is the
	// BT.2100 reference monitor and what the poster lane has always assumed.
	// Scaling the OOTF by the DISPLAY's real peak instead is a follow-up, and
	// it must move BOTH lanes in one change or they disagree again.
	static const float kHlgNominalPeakNits = 1000.0f;

	static const float kPqScale  = kPqPeakNits / kSdrReferenceWhiteNits;         // 49.261
	static const float kHlgScale = kHlgNominalPeakNits / kSdrReferenceWhiteNits; //  4.926

	// AVCOL_TRC values, so callers do not spell the magic numbers themselves.
	enum
	{
		TRC_PQ  = 16,   // AVCOL_TRC_SMPTE2084
		TRC_HLG = 18,   // AVCOL_TRC_ARIB_STD_B67
	};

	inline bool IsHdrTrc(int colorTrc)
	{
		return colorTrc == TRC_PQ || colorTrc == TRC_HLG;
	}

	// PQ (SMPTE ST 2084) EOTF: encoded 0..1 -> absolute luminance 0..1 where
	// 1.0 means 10 000 nits. Multiply by kPqScale for the pipeline convention.
	inline float PqEotf(float e)
	{
		const float m1 = 0.1593017578125f, m2 = 78.84375f;
		const float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
		const float p = powf(e < 0.0f ? 0.0f : e, 1.0f / m2);
		const float num = p - c1;
		const float den = c2 - c3 * p;
		if (num <= 0.0f || den <= 0.0f)
			return 0.0f;
		return powf(num / den, 1.0f / m1);
	}

	// HLG (ARIB STD-B67) inverse OETF: encoded 0..1 -> SCENE linear 0..1.
	// The display OOTF is applied separately, because it needs all three
	// channels at once (it is driven by luma) -- see HlgOotfGain.
	inline float HlgInverseOetf(float e)
	{
		const float a = 0.17883277f, b = 0.28466892f, c = 0.55991073f;
		if (e < 0.0f) e = 0.0f;
		return (e <= 0.5f) ? (e * e) / 3.0f
						   : (expf((e - c) / a) + b) / 12.0f;
	}

	// HLG's display OOTF, as a per-pixel GAIN to multiply all three scene-linear
	// channels by: Yd = Ys^(gamma-1) * E, gamma 1.2, Ys the BT.2020 luma.
	// Split out as a gain rather than folded in, so the shader (which has the
	// three channels in a vector) and the CPU (which has them in an array) can
	// share the identical expression.
	inline float HlgOotfGain(float r, float g, float b)
	{
		const float ys = 0.2627f * r + 0.6780f * g + 0.0593f * b;
		return (ys > 0.0f) ? powf(ys, 0.2f) : 0.0f;
	}

	// BT.2020 -> sRGB primaries, linear, D65. Both are D65, so this is a pure
	// primaries change with no chromatic adaptation. Row-major: out[r] is
	// row r dotted with the input.
	//
	// The rows sum to 1.0, which is the property that breaks first if this is
	// ever mistyped: neutral must stay neutral.
	static const float kBt2020ToSrgb[3][3] = {
		{  1.6605f, -0.5876f, -0.0728f },
		{ -0.1246f,  1.1329f, -0.0083f },
		{ -0.0182f, -0.1006f,  1.1187f },
	};

	inline void Bt2020ToSrgb(const float in[3], float out[3])
	{
		for (int r = 0; r < 3; r++)
			out[r] = kBt2020ToSrgb[r][0] * in[0]
				   + kBt2020ToSrgb[r][1] * in[1]
				   + kBt2020ToSrgb[r][2] * in[2];
	}

	// -----------------------------------------------------------------------
	// THE BULK FAST PATH: table-driven EOTF.
	// -----------------------------------------------------------------------
	//
	// The analytic functions above stay EXACT and are what the unit tests pin
	// to 1e-6. These tables exist because whole-image conversion calls them
	// millions of times: a 1024x576 poster is 1.77M channel conversions, and
	// PQ costs TWO powf each -- 3.5M powf, measured at 56 ms per poster in
	// Release (against 6.6 ms for the same-size SDR poster). That is on the
	// browse path, and every HDR clip pays it twice, once for the grid poster
	// and once for the t=0 still.
	//
	// DIRECTLY INDEXED BY THE 16-BIT CODE VALUE, with no interpolation, so
	// the table is EXACT for every input the bulk converter can present --
	// there is no approximation to reason about, and no error term to carry
	// into the tolerance of any downstream comparison. 65536 floats = 256 KB
	// per transfer function, built lazily on first use (a clip that is not HLG
	// never pays for the HLG table) and once per process. Construction is
	// ~131k powf, about 2 ms, amortised over every frame and poster after it.
	//
	// Function-local statics: C++11 guarantees the initialisation is
	// thread-safe and happens exactly once, which matters because decode
	// workers reach these concurrently.

	static const int kEotfTableSize = 65536;

	// PQ, WITH kPqScale ALREADY FOLDED IN -- one multiply saved per channel,
	// and it keeps the table's meaning identical to what PqEotf(e) * kPqScale
	// returns, so the two are directly comparable in a test.
	inline const float *PqEotfTable()
	{
		static const std::vector<float> table = [] {
			std::vector<float> t((size_t)kEotfTableSize);
			for (int i = 0; i < kEotfTableSize; i++)
				t[(size_t)i] = PqEotf((float)i * (1.0f / 65535.0f)) * kPqScale;
			return t;
		}();
		return table.data();
	}

	// HLG's inverse OETF only. The OOTF is NOT folded in: it is driven by the
	// luma of all three channels, so it cannot be a per-channel table.
	inline const float *HlgInverseOetfTable()
	{
		static const std::vector<float> table = [] {
			std::vector<float> t((size_t)kEotfTableSize);
			for (int i = 0; i < kEotfTableSize; i++)
				t[(size_t)i] = HlgInverseOetf((float)i * (1.0f / 65535.0f));
			return t;
		}();
		return table.data();
	}

	// The OOTF gain, which is the ONE powf per pixel that survives the tables
	// above on the HLG path. Its input is BT.2020 luma of scene-linear values,
	// which HlgInverseOetf bounds to [0, 1], so a table over exactly that
	// range is complete. Interpolated rather than direct-indexed because the
	// input is a computed float, not a code value.
	static const int kOotfTableSize = 4096;

	inline float HlgOotfGainFast(float ys)
	{
		static const std::vector<float> table = [] {
			std::vector<float> t((size_t)kOotfTableSize + 1);
			for (int i = 0; i <= kOotfTableSize; i++)
			{
				const float v = (float)i / (float)kOotfTableSize;
				t[(size_t)i] = (v > 0.0f) ? powf(v, 0.2f) : 0.0f;
			}
			return t;
		}();
		if (!(ys > 0.0f))            // also catches NaN
			return 0.0f;
		if (ys >= 1.0f)
			return table[(size_t)kOotfTableSize];
		const float x = ys * (float)kOotfTableSize;
		const int   i = (int)x;
		const float f = x - (float)i;
		return table[(size_t)i] + (table[(size_t)i + 1] - table[(size_t)i]) * f;
	}

	// The bulk equivalent of HdrEncodedToLinearSrgb, taking the RAW 16-BIT
	// CODE VALUES the converter already has rather than normalised floats --
	// so the table index needs no arithmetic at all.
	//
	// Produces exactly what the analytic chain produces for PQ (the table is
	// exact at every index); for HLG the only difference is the interpolated
	// OOTF gain, which the tests bound.
	inline void HdrCodeToLinearSrgb(const u16 code[3], int colorTrc, float outLinear[3])
	{
		float lin[3];
		if (colorTrc == TRC_PQ)
		{
			const float *lut = PqEotfTable();
			lin[0] = lut[code[0]]; lin[1] = lut[code[1]]; lin[2] = lut[code[2]];
		}
		else
		{
			const float *lut = HlgInverseOetfTable();
			lin[0] = lut[code[0]]; lin[1] = lut[code[1]]; lin[2] = lut[code[2]];
			const float ys = 0.2627f * lin[0] + 0.6780f * lin[1] + 0.0593f * lin[2];
			const float gain = HlgOotfGainFast(ys);
			for (int c = 0; c < 3; c++)
				lin[c] = lin[c] * gain * kHlgScale;
		}
		Bt2020ToSrgb(lin, outLinear);
	}

	// The whole chain for one pixel: PQ/HLG-encoded R'G'B' with BT.2020
	// primaries -> LINEAR sRGB, 1.0 == SDR reference white.
	//
	// This is the function the shader's fragment maths mirrors, and the one
	// the GPU-vs-CPU agreement test evaluates. `colorTrc` is TRC_PQ or
	// TRC_HLG; anything else is treated as PQ by the caller's contract (the
	// caller is expected to have gated on IsHdrTrc first).
	inline void HdrEncodedToLinearSrgb(const float rgbEncoded[3], int colorTrc, float outLinear[3])
	{
		const bool isPq = (colorTrc == TRC_PQ);
		float lin[3];
		for (int c = 0; c < 3; c++)
		{
			lin[c] = isPq ? PqEotf(rgbEncoded[c]) * kPqScale
						  : HlgInverseOetf(rgbEncoded[c]);
		}
		if (!isPq)
		{
			const float gain = HlgOotfGain(lin[0], lin[1], lin[2]);
			for (int c = 0; c < 3; c++)
				lin[c] = lin[c] * gain * kHlgScale;
		}
		Bt2020ToSrgb(lin, outLinear);
	}

	// The extended Reinhard tone-map, normalised so that the value equal to
	// `headroom` maps exactly to 1.0:
	//
	//     out = v * (1 + v/h^2) / (1 + v)
	//
	// At headroom 1.0 this is EXACTLY the identity on 0..1 -- v*(1+v)/(1+v) --
	// so the SDR body comes out precisely where it does today.
	//
	// THE SAME CURVE AS CImageData::ConvertRGBA16FToRGBA8, deliberately: that
	// is what the POSTER goes through when the resident format is 8-bit, and
	// the gate-closed playback arm has to land in the same place or the two
	// disagree in exactly the arm Phase 5 Task 6 exists to keep aligned. The
	// CPU path additionally applies an ordered Bayer dither; the shader does
	// not, which is why their comparison is a tolerance rather than byte
	// equality.
	inline float ToneMapReinhard(float v, float headroom)
	{
		if (!(headroom >= 1.0f))    // also catches NaN
			headroom = 1.0f;
		if (v < 0.0f) v = 0.0f;
		const float invH2 = 1.0f / (headroom * headroom);
		const float t = v * (1.0f + v * invH2) / (1.0f + v);
		return (t > 1.0f) ? 1.0f : t;
	}

} // namespace VideoTransfer

#endif
