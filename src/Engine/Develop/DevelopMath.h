#pragma once

// RD-C: the numeric substrate of the develop pipeline (design #2.3, #4.3,
// #11.1). Mechanisms only -- no ACR policy numbers live here (#6). The one
// deliberate inclusion of published DNG-SDK DATA (the Robertson temperature
// table) is ruled engine-eligible by spec #5.1's dng_temperature precedent:
// DNG SDK maths, not an ACR policy number.
//
// All functions are pure and thread-safe; nothing here allocates per pixel.

#include "SYS_Defs.h"
#include <cmath>
#include <functional>
#include <vector>

// ---------------------------------------------------------------------------
// Gamma encode/decode (#2.3): the sRGB piecewise TRC applied to ProPhoto
// PRIMARIES; odd extension below 0; unclamped above 1.
// ---------------------------------------------------------------------------

float PC_DevEncode(float linear);   // linear -> gamma-encoded
float PC_DevDecode(float encoded);  // gamma-encoded -> linear
void  PC_DevEncodeBuffer(float *rgb, size_t count);   // count = float count
void  PC_DevDecodeBuffer(float *rgb, size_t count);

// ---------------------------------------------------------------------------
// Natural cubic spline (#4.3) -- the dng_spline_solver algorithm shape:
// natural boundary conditions, evaluated by interval. Control points x
// ascending. Overshoot is NOT clamped here (clamp the evaluated LUT, #4.3).
// ---------------------------------------------------------------------------

class CDevSpline
{
public:
	// Returns false on <2 points or non-ascending x.
	bool Solve(const float (*xy)[2], int count);
	float Evaluate(float x) const;   // linear extrapolation outside [x0, xn]

private:
	std::vector<float> xs_, ys_, y2_;   // second derivatives per the solve
};

// ---------------------------------------------------------------------------
// 1-D LUT over the pipeline's encoded domain (#2.3): [-0.5, 2.0], with
// values OUTSIDE evaluated by the analytic function, never clamped into the
// table -- an unbounded input with a [0,1] table is the silent clipper the
// float container was chosen to avoid.
// ---------------------------------------------------------------------------

struct SDevLut1D
{
	static const int   kSize = 4096;
	static constexpr float kDomainLo = -0.5f;
	static constexpr float kDomainHi =  2.0f;

	std::vector<float> table;                 // kSize entries
	std::function<float(float)> analytic;     // exact function, used outside

	// Builds table + keeps `fn` for the out-of-domain arm.
	void Build(const std::function<float(float)> &fn);
	bool IsBuilt() const { return !table.empty(); }
	float Sample(float x) const;              // lerp inside, analytic outside
};

// Compose helper for stage 9 (#5.2 consequence 1): the linear-domain curve
// folds into ONE encoded-domain LUT -- encode(curve(decode(x))). `curve` is
// a LINEAR-domain function.
void PC_DevBuildEncodedLutFromLinearCurve(const std::function<float(float)> &linearCurve,
                                          SDevLut1D *out);

// RD-D F7: dng_sdk's RGBTone (dng_reference.cpp:1729) -- how a 1-D tone
// curve reaches a 3-channel pixel. The curve is applied to the MAX and MIN
// channels; the MIDDLE is reconstructed by ratio: mid' = T(min) +
// (T(max)-T(min)) * (mid-min)/(max-min). NOT per-channel (hue-shifted on
// saturated colours), NOT luminance-ratio. `rgb` is LINEAR; `linearCurve`
// is a linear-domain LUT (analytic outside [-0.5, 2.0]).
void PC_DevRgbTone(float rgb[3], const SDevLut1D &linearCurve);

// ---------------------------------------------------------------------------
// 3x3 matrix helpers (row-major, like CRawDecoder's camToXYZ)
// ---------------------------------------------------------------------------

void  PC_Mat3Mul(const float a[3][3], const float b[3][3], float out[3][3]);
bool  PC_Mat3Invert(const float m[3][3], float out[3][3]);   // false if singular
void  PC_Mat3Apply(const float m[3][3], const float in[3], float out[3]);

// Published constant matrices. All row-major.
// Bradford chromatic adaptation D65 -> D50 (the baseline matrix seam folds
// this onto LibRaw's D65-normalised camToXYZ, #5 table row 2).
extern const float PC_kBradfordD65ToD50[3][3];
// ROMM RGB (linear ProPhoto) -> XYZ(D50): the published ISO 22028-2 matrix,
// column sums exactly the ICC PCS D50 (0.9642, 1.0000, 0.8249) -- the SAME
// constants ICC_BuildLinearProPhotoProfileV2 writes as colorants, so the
// pipeline and the profile can never disagree (#7.1).
extern const float PC_kRommToXyzD50[3][3];
extern const float PC_kXyzD50ToRomm[3][3];   // its inverse, published

// Linear ProPhoto(D50) -> linear sRGB(D65): Bradford(D50->D65) then
// XYZ(D65)->sRGB, composed. The #7 item-5 fallback arm (CM-off / declined
// hand-off) uses this followed by PC_DevEncodeSrgb below.
extern const float PC_kRommToLinearSrgb[3][3];

// Linear sRGB (Rec.709 primaries, D65) -> linear Display P3 (P3 primaries,
// D65). Same white point, so no chromatic adaptation is involved -- this is a
// pure primaries change.
//
// Needed because the HDR surface follows the user's gamut preference: with HDR
// on and the preference set to P3, CoreAnimation is told the layer is extended
// DISPLAY P3, not extended sRGB. Float pixels written in sRGB primaries into a
// P3-declared surface come out oversaturated, and no capture test can see it --
// the capture returns what we wrote, and the misinterpretation happens in the
// compositor afterwards.
extern const float PC_kLinearSrgbToLinearP3[3][3];

// The TRUE sRGB piecewise encode (for the fallback arm's final encode --
// same formula as PC_DevEncode; alias kept so call sites say what they mean).
inline float PC_DevEncodeSrgb(float linear) { return PC_DevEncode(linear); }

// ---------------------------------------------------------------------------
// HSV in the CURRENT domain (#4.4): plain hexcone RGB<->HSV. Values may
// exceed [0,1]; V = max, S = (max-min)/max with max guarded, H in [0,6).
// ---------------------------------------------------------------------------

void PC_RgbToHsv(const float rgb[3], float hsv[3]);
void PC_HsvToRgb(const float hsv[3], float rgb[3]);

// ---------------------------------------------------------------------------
// Correlated colour temperature (#11.1 row 1): the Robertson uv-t method
// over the published 31-entry isotemperature table, as implemented by the
// public dng_temperature (reimplemented from the published algorithm --
// dng_sdk is NOT vendored). Tint scale matches dng_sdk's -3000.
// ---------------------------------------------------------------------------

struct SDevTempTint
{
	float temperatureK = 0.f;
	float tint         = 0.f;
};

// CIE 1960 u,v from xy and back.
void PC_XyToUv(const float xy[2], float uv[2]);
void PC_UvToXy(const float uv[2], float xy[2]);

// xy -> (temperature, tint) and back.
bool PC_XyToTempTint(const float xy[2], SDevTempTint *out);
bool PC_TempTintToXy(const SDevTempTint &tt, float xy[2]);

// Kelvin/tint -> camera-space neutral via a camera->XYZ matrix (the baseline
// Resolve's Temp/Tint arm, spec #5.1 rev 8): xy -> XYZ (Y = 1) ->
// camToXYZ^-1 -> normalise G = 1. False when the matrix is singular.
bool PC_TempTintToCameraNeutral(const SDevTempTint &tt,
                                const float camToXYZ[3][3],
                                float outNeutral[3]);
