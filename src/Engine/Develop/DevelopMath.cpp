#include "DevelopMath.h"
#include "DevelopSeams.h"
#include "CRawDecoder.h"

#include <algorithm>
#include <cstring>

// ---------------------------------------------------------------------------
// Gamma encode/decode (#2.3)
// ---------------------------------------------------------------------------
//
// The sRGB piecewise transfer function applied to ProPhoto primaries -- NOT
// ROMM's own 1.8 and NOT a pure 1/2.2 (#2.3's table: those place a curve node
// at 128 on materially different linear values). Odd extension below zero:
// negatives are legal in the scene-referred container and must survive the
// round trip (#2.2, #9.1's rev-8 row). Above 1 the same formula, unclamped.

static inline float DevEncodeCore(float x)
{
	return (x <= 0.0031308f) ? 12.92f * x
	                         : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}

static inline float DevDecodeCore(float y)
{
	return (y <= 0.04045f) ? y / 12.92f
	                       : std::pow((y + 0.055f) / 1.055f, 2.4f);
}

float PC_DevEncode(float linear)
{
	return (linear < 0.f) ? -DevEncodeCore(-linear) : DevEncodeCore(linear);
}

float PC_DevDecode(float encoded)
{
	return (encoded < 0.f) ? -DevDecodeCore(-encoded) : DevDecodeCore(encoded);
}

void PC_DevEncodeBuffer(float *rgb, size_t count)
{
	for (size_t i = 0; i < count; i++)
		rgb[i] = PC_DevEncode(rgb[i]);
}

void PC_DevDecodeBuffer(float *rgb, size_t count)
{
	for (size_t i = 0; i < count; i++)
		rgb[i] = PC_DevDecode(rgb[i]);
}

// ---------------------------------------------------------------------------
// Natural cubic spline (#4.3)
// ---------------------------------------------------------------------------
//
// Standard natural-spline tridiagonal solve -- the same algorithm shape as
// the public dng_spline_solver, which the spec names as the reference for
// ProfileToneCurve. Natural boundaries (y'' = 0 at both ends).

bool CDevSpline::Solve(const float (*xy)[2], int count)
{
	xs_.clear(); ys_.clear(); y2_.clear();
	if (xy == nullptr || count < 2)
		return false;
	for (int i = 1; i < count; i++)
		if (!(xy[i][0] > xy[i - 1][0]))
			return false;

	xs_.resize(count); ys_.resize(count); y2_.assign(count, 0.f);
	for (int i = 0; i < count; i++) { xs_[i] = xy[i][0]; ys_[i] = xy[i][1]; }

	// Tridiagonal solve for second derivatives, natural boundaries.
	std::vector<float> u(count, 0.f);
	for (int i = 1; i < count - 1; i++)
	{
		const float sig = (xs_[i] - xs_[i - 1]) / (xs_[i + 1] - xs_[i - 1]);
		const float p   = sig * y2_[i - 1] + 2.0f;
		y2_[i] = (sig - 1.0f) / p;
		const float d1 = (ys_[i + 1] - ys_[i]) / (xs_[i + 1] - xs_[i]);
		const float d0 = (ys_[i] - ys_[i - 1]) / (xs_[i] - xs_[i - 1]);
		u[i] = (6.0f * (d1 - d0) / (xs_[i + 1] - xs_[i - 1]) - sig * u[i - 1]) / p;
	}
	for (int k = count - 2; k >= 0; k--)
		y2_[k] = y2_[k] * y2_[k + 1] + u[k];
	return true;
}

float CDevSpline::Evaluate(float x) const
{
	const int n = (int)xs_.size();
	if (n == 0) return x;
	if (n == 1) return ys_[0];

	// Linear extrapolation outside the knots -- a spline's cubic arms diverge
	// fast, and the LUT's analytic fallback calls this well outside [0,1].
	if (x <= xs_[0])
	{
		const float slope = (ys_[1] - ys_[0]) / (xs_[1] - xs_[0]);
		return ys_[0] + slope * (x - xs_[0]);
	}
	if (x >= xs_[n - 1])
	{
		const float slope = (ys_[n - 1] - ys_[n - 2]) / (xs_[n - 1] - xs_[n - 2]);
		return ys_[n - 1] + slope * (x - xs_[n - 1]);
	}

	int lo = 0, hi = n - 1;
	while (hi - lo > 1)
	{
		const int mid = (lo + hi) / 2;
		if (xs_[mid] > x) hi = mid; else lo = mid;
	}
	const float h = xs_[hi] - xs_[lo];
	const float a = (xs_[hi] - x) / h;
	const float b = (x - xs_[lo]) / h;
	return a * ys_[lo] + b * ys_[hi]
	     + ((a * a * a - a) * y2_[lo] + (b * b * b - b) * y2_[hi]) * (h * h) / 6.0f;
}

// ---------------------------------------------------------------------------
// SDevLut1D (#2.3)
// ---------------------------------------------------------------------------

void SDevLut1D::Build(const std::function<float(float)> &fn)
{
	analytic = fn;
	table.resize(kSize);
	const float step = (kDomainHi - kDomainLo) / (float)(kSize - 1);
	for (int i = 0; i < kSize; i++)
		table[(size_t)i] = fn(kDomainLo + step * (float)i);
}

float SDevLut1D::Sample(float x) const
{
	if (x < kDomainLo || x > kDomainHi)
		return analytic ? analytic(x) : x;   // #2.3: analytic outside, never clamp
	const float t = (x - kDomainLo) / (kDomainHi - kDomainLo) * (float)(kSize - 1);
	const int   i = std::min((int)t, kSize - 2);
	const float f = t - (float)i;
	return table[(size_t)i] + f * (table[(size_t)i + 1] - table[(size_t)i]);
}

void PC_DevBuildEncodedLutFromLinearCurve(const std::function<float(float)> &linearCurve,
                                          SDevLut1D *out)
{
	// #5.2 consequence 1: encode(curve(decode(x))) is itself a 1-D function of
	// one channel, so the linear-domain seam curve folds into ONE
	// encoded-domain LUT. A LUT-construction rule, not a per-pixel round trip.
	out->Build([linearCurve](float xEnc) {
		return PC_DevEncode(linearCurve(PC_DevDecode(xEnc)));
	});
}

void PC_DevRgbTone(float rgb[3], const SDevLut1D &T)
{
	// dng_reference.cpp:1729 RefBaselineRGBTone, all seven orderings. The
	// macro's shape: curve the max and min, rebuild the middle by ratio.
	const float r = rgb[0], g = rgb[1], b = rgb[2];
	float rr, gg, bb;

	auto tone = [&T](float mx, float md, float mn,
	                 float &omx, float &omd, float &omn)
	{
		omx = T.Sample(mx);
		omn = T.Sample(mn);
		omd = omn + (omx - omn) * (md - mn) / (mx - mn);
	};

	if (r >= g)
	{
		if (g > b)
			tone(r, g, b, rr, gg, bb);            // r >= g > b
		else if (b > r)
			tone(b, r, g, bb, rr, gg);            // b > r >= g
		else if (b > g)
			tone(r, b, g, rr, bb, gg);            // r >= b > g
		else
		{
			// r >= g == b
			rr = T.Sample(r);
			gg = T.Sample(g);
			bb = gg;
		}
	}
	else
	{
		if (r >= b)
			tone(g, r, b, gg, rr, bb);            // g > r >= b
		else if (b > g)
			tone(b, g, r, bb, gg, rr);            // b > g > r
		else
			tone(g, b, r, gg, bb, rr);            // g >= b > r
	}

	rgb[0] = rr;
	rgb[1] = gg;
	rgb[2] = bb;
}

// ---------------------------------------------------------------------------
// 3x3 helpers + published constants
// ---------------------------------------------------------------------------

void PC_Mat3Mul(const float a[3][3], const float b[3][3], float out[3][3])
{
	float r[3][3];
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
	memcpy(out, r, sizeof(r));
}

bool PC_Mat3Invert(const float m[3][3], float out[3][3])
{
	const double a = m[0][0], b = m[0][1], c = m[0][2];
	const double d = m[1][0], e = m[1][1], f = m[1][2];
	const double g = m[2][0], h = m[2][1], i = m[2][2];
	const double A =  (e * i - f * h);
	const double B = -(d * i - f * g);
	const double C =  (d * h - e * g);
	const double det = a * A + b * B + c * C;
	if (std::fabs(det) < 1e-12)
		return false;
	const double s = 1.0 / det;
	out[0][0] = (float)(A * s);
	out[0][1] = (float)(-(b * i - c * h) * s);
	out[0][2] = (float)( (b * f - c * e) * s);
	out[1][0] = (float)(B * s);
	out[1][1] = (float)( (a * i - c * g) * s);
	out[1][2] = (float)(-(a * f - c * d) * s);
	out[2][0] = (float)(C * s);
	out[2][1] = (float)(-(a * h - b * g) * s);
	out[2][2] = (float)( (a * e - b * d) * s);
	return true;
}

void PC_Mat3Apply(const float m[3][3], const float in[3], float out[3])
{
	const float r0 = m[0][0] * in[0] + m[0][1] * in[1] + m[0][2] * in[2];
	const float r1 = m[1][0] * in[0] + m[1][1] * in[1] + m[1][2] * in[2];
	const float r2 = m[2][0] * in[0] + m[2][1] * in[1] + m[2][2] * in[2];
	out[0] = r0; out[1] = r1; out[2] = r2;
}

// Bradford D65 -> D50, the standard published matrix (Lindbloom / ICC).
const float PC_kBradfordD65ToD50[3][3] = {
	{ 1.0478112f, 0.0228866f, -0.0501270f },
	{ 0.0295424f, 0.9904844f, -0.0170491f },
	{ -0.0092345f, 0.0150436f, 0.7521316f },
};

// ROMM RGB -> XYZ(D50), ISO 22028-2 / ROMM reference matrix. Column sums are
// EXACTLY the ICC PCS D50 (0.9642, 1.0000, 0.8249) -- the same constants the
// linear-ProPhoto ICC builder writes as colorants (#7.1, plan T1).
const float PC_kRommToXyzD50[3][3] = {
	{ 0.7977f, 0.1352f, 0.0313f },
	{ 0.2880f, 0.7119f, 0.0001f },
	{ 0.0000f, 0.0000f, 0.8249f },
};

// Its published inverse.
const float PC_kXyzD50ToRomm[3][3] = {
	{ 1.3459433f, -0.2556075f, -0.0511118f },
	{ -0.5445989f, 1.5081673f,  0.0205351f },
	{ 0.0000000f,  0.0000000f,  1.2118128f },
};

// ProPhoto(D50) -> linear sRGB(D65): XYZ(D65)->sRGB * Bradford(D50->D65) *
// ROMM->XYZ(D50), composed offline in double precision from the three
// published matrices (derivation preserved in the RD-C plan; verify by
// applying to white: (1,1,1) -> (1.0001, 1.0000, 0.9996)). LITERALS, not a
// runtime composition: an earlier revision built this at static init through
// a const_cast into a const array -- UB, and Release constant-folded the
// visible zero initializer so every read returned 0. The literal form cannot
// regress that way and the DevelopMath test pins the white-preservation
// property either way.
const float PC_kRommToLinearSrgb[3][3] = {
	{ 2.0342194f, -0.7273499f, -0.3067801f },
	{ -0.2289147f, 1.2317716f, -0.0028476f },
	{ -0.0085588f, -0.1532920f, 1.1614137f },
};

// Linear sRGB(D65) -> linear Display P3(D65). Both spaces share the D65 white
// point, so the rows sum to 1.0 and neutral grey maps to neutral grey -- the
// property a wrong primaries matrix breaks first, and the one the test pins.
const float PC_kLinearSrgbToLinearP3[3][3] = {
	{ 0.8224621f, 0.1775380f, 0.0000000f },
	{ 0.0331941f, 0.9668058f, 0.0000000f },
	{ 0.0170827f, 0.0723974f, 0.9105199f },
};

// ---------------------------------------------------------------------------
// HSV (#4.4)
// ---------------------------------------------------------------------------

void PC_RgbToHsv(const float rgb[3], float hsv[3])
{
	const float r = rgb[0], g = rgb[1], b = rgb[2];
	const float mx = std::max(r, std::max(g, b));
	const float mn = std::min(r, std::min(g, b));
	const float d = mx - mn;

	float h = 0.f;
	if (d > 1e-20f)
	{
		if (mx == r)      h = (g - b) / d;
		else if (mx == g) h = 2.f + (b - r) / d;
		else              h = 4.f + (r - g) / d;
		if (h < 0.f) h += 6.f;
	}
	hsv[0] = h;                                    // [0, 6)
	hsv[1] = (mx > 1e-20f) ? d / mx : 0.f;
	hsv[2] = mx;                                   // may exceed 1 -- legal
}

void PC_HsvToRgb(const float hsv[3], float rgb[3])
{
	const float h = hsv[0], s = hsv[1], v = hsv[2];
	const int   i = (int)std::floor(h) % 6;
	const float f = h - std::floor(h);
	const float p = v * (1.f - s);
	const float q = v * (1.f - s * f);
	const float t = v * (1.f - s * (1.f - f));
	switch (i < 0 ? i + 6 : i)
	{
		case 0: rgb[0] = v; rgb[1] = t; rgb[2] = p; break;
		case 1: rgb[0] = q; rgb[1] = v; rgb[2] = p; break;
		case 2: rgb[0] = p; rgb[1] = v; rgb[2] = t; break;
		case 3: rgb[0] = p; rgb[1] = q; rgb[2] = v; break;
		case 4: rgb[0] = t; rgb[1] = p; rgb[2] = v; break;
		default: rgb[0] = v; rgb[1] = p; rgb[2] = q; break;
	}
}

// ---------------------------------------------------------------------------
// Correlated colour temperature -- Robertson's method (#11.1 row 1)
// ---------------------------------------------------------------------------
//
// The published 31-entry isotemperature-line table (Wyszecki & Stiles, as
// carried by the public dng_temperature: {mired, u, v, slope}); the 325-mired
// u value carries dng_sdk's documented correction (0.24792 vs W&S's misprint).
// Algorithm reimplemented from the published description; dng_sdk is a
// dev-only reference, never vendored (RD-D #8.3).

namespace
{
struct SRuvt { float r, u, v, t; };
const SRuvt kTempTable[31] = {
	{   0.f, 0.18006f, 0.26352f,  -0.24341f },
	{  10.f, 0.18066f, 0.26589f,  -0.25479f },
	{  20.f, 0.18133f, 0.26846f,  -0.26876f },
	{  30.f, 0.18208f, 0.27119f,  -0.28539f },
	{  40.f, 0.18293f, 0.27407f,  -0.30470f },
	{  50.f, 0.18388f, 0.27709f,  -0.32675f },
	{  60.f, 0.18494f, 0.28021f,  -0.35156f },
	{  70.f, 0.18611f, 0.28342f,  -0.37915f },
	{  80.f, 0.18740f, 0.28668f,  -0.40955f },
	{  90.f, 0.18880f, 0.28997f,  -0.44278f },
	{ 100.f, 0.19032f, 0.29326f,  -0.47888f },
	{ 125.f, 0.19462f, 0.30141f,  -0.58204f },
	{ 150.f, 0.19962f, 0.30921f,  -0.70471f },
	{ 175.f, 0.20525f, 0.31647f,  -0.84901f },
	{ 200.f, 0.21142f, 0.32312f,  -1.01820f },
	{ 225.f, 0.21807f, 0.32909f,  -1.21680f },
	{ 250.f, 0.22511f, 0.33439f,  -1.45120f },
	{ 275.f, 0.23247f, 0.33904f,  -1.72980f },
	{ 300.f, 0.24010f, 0.34308f,  -2.06370f },
	{ 325.f, 0.24792f, 0.34655f,  -2.46810f },
	{ 350.f, 0.25591f, 0.34951f,  -2.96410f },
	{ 375.f, 0.26400f, 0.35200f,  -3.58140f },
	{ 400.f, 0.27218f, 0.35407f,  -4.36330f },
	{ 425.f, 0.28039f, 0.35577f,  -5.37620f },
	{ 450.f, 0.28863f, 0.35714f,  -6.72620f },
	{ 475.f, 0.29685f, 0.35823f,  -8.59550f },
	{ 500.f, 0.30505f, 0.35907f, -11.32400f },
	{ 525.f, 0.31320f, 0.35968f, -15.62800f },
	{ 550.f, 0.32129f, 0.36011f, -23.32500f },
	{ 575.f, 0.32931f, 0.36038f, -40.77000f },
	{ 600.f, 0.33724f, 0.36051f, -116.45000f },
};
const float kTintScale = -3000.0f;
}

void PC_XyToUv(const float xy[2], float uv[2])
{
	const float x = xy[0], y = xy[1];
	const float d = -2.f * x + 12.f * y + 3.f;
	uv[0] = 4.f * x / d;
	uv[1] = 6.f * y / d;
}

void PC_UvToXy(const float uv[2], float xy[2])
{
	const float u = uv[0], v = uv[1];
	const float d = 2.f * u - 8.f * v + 4.f;
	xy[0] = 3.f * u / d;
	xy[1] = 2.f * v / d;
}

bool PC_XyToTempTint(const float xy[2], SDevTempTint *out)
{
	float uv[2];
	PC_XyToUv(xy, uv);
	const float u = uv[0], v = uv[1];

	float lastDt = 0.f, lastDu = 0.f, lastDv = 0.f;
	for (int index = 1; index <= 30; index++)
	{
		// Slope -> unit direction along the isotemperature line.
		float du = 1.0f, dv = kTempTable[index].t;
		const float len = std::sqrt(1.0f + dv * dv);
		du /= len; dv /= len;

		// Signed distance above/below the line through this table point.
		const float uu = u - kTempTable[index].u;
		const float vv = v - kTempTable[index].v;
		float dt = -uu * dv + vv * du;

		if (dt <= 0.0f || index == 30)
		{
			dt = -std::min(dt, 0.0f);
			const float f = (index == 1) ? 0.0f : dt / (lastDt + dt);

			out->temperatureK = 1.0e6f /
				(kTempTable[index - 1].r * f + kTempTable[index].r * (1.0f - f));

			const float uu2 = u - (kTempTable[index - 1].u * f + kTempTable[index].u * (1.0f - f));
			const float vv2 = v - (kTempTable[index - 1].v * f + kTempTable[index].v * (1.0f - f));
			float du2 = du * (1.0f - f) + lastDu * f;
			float dv2 = dv * (1.0f - f) + lastDv * f;
			const float len2 = std::sqrt(du2 * du2 + dv2 * dv2);
			du2 /= len2; dv2 /= len2;
			out->tint = (uu2 * du2 + vv2 * dv2) * kTintScale;
			return true;
		}
		lastDt = dt; lastDu = du; lastDv = dv;
	}
	return false;
}

bool PC_TempTintToXy(const SDevTempTint &tt, float xy[2])
{
	if (tt.temperatureK < 1.0f)
		return false;
	const float r = 1.0e6f / tt.temperatureK;   // mired

	for (int index = 1; index <= 30; index++)
	{
		if (r < kTempTable[index].r || index == 30)
		{
			const float f = (kTempTable[index].r - r) /
			                (kTempTable[index].r - kTempTable[index - 1].r);
			const float fc = std::clamp(f, 0.0f, 1.0f);

			float u = kTempTable[index - 1].u * fc + kTempTable[index].u * (1.0f - fc);
			float v = kTempTable[index - 1].v * fc + kTempTable[index].v * (1.0f - fc);

			// Unit vectors along the two neighbouring isotemperature lines,
			// blended, then offset perpendicular... the offset IS along the
			// isotemperature line direction (u,v move along it with tint).
			float du1 = 1.0f, dv1 = kTempTable[index - 1].t;
			float len1 = std::sqrt(1.0f + dv1 * dv1); du1 /= len1; dv1 /= len1;
			float du2 = 1.0f, dv2 = kTempTable[index].t;
			float len2 = std::sqrt(1.0f + dv2 * dv2); du2 /= len2; dv2 /= len2;
			float du = du1 * fc + du2 * (1.0f - fc);
			float dv = dv1 * fc + dv2 * (1.0f - fc);
			const float len = std::sqrt(du * du + dv * dv);
			du /= len; dv /= len;

			u += du * (tt.tint / kTintScale);
			v += dv * (tt.tint / kTintScale);

			const float uv[2] = { u, v };
			PC_UvToXy(uv, xy);
			return true;
		}
	}
	return false;
}

bool PC_TempTintToCameraNeutral(const SDevTempTint &tt,
                                const float camToXYZ[3][3],
                                float outNeutral[3])
{
	float xy[2];
	if (!PC_TempTintToXy(tt, xy))
		return false;
	if (xy[1] < 1e-6f)
		return false;

	// xy -> XYZ at Y = 1.
	const float xyz[3] = { xy[0] / xy[1], 1.0f, (1.0f - xy[0] - xy[1]) / xy[1] };

	float inv[3][3];
	if (!PC_Mat3Invert(camToXYZ, inv))
		return false;

	float n[3];
	PC_Mat3Apply(inv, xyz, n);
	if (std::fabs(n[1]) < 1e-9f)
		return false;

	// G = 1 convention (spec rev 8 / plan F2), matching SCameraResolution.
	outNeutral[0] = n[0] / n[1];
	outNeutral[1] = 1.0f;
	outNeutral[2] = n[2] / n[1];
	return true;
}

// ---------------------------------------------------------------------------
// PC_FillRawDngTags (DevelopSeams.h)
// ---------------------------------------------------------------------------

void PC_FillRawDngTags(const SRawDecodeResult &raw, SRawDngTags *out)
{
	memcpy(out->dngFields, raw.dngFields, sizeof(out->dngFields));
	memcpy(out->dngIlluminant, raw.dngIlluminant, sizeof(out->dngIlluminant));
	memcpy(out->dngColorMatrix, raw.dngColorMatrix, sizeof(out->dngColorMatrix));
	memcpy(out->dngForwardMatrix, raw.dngForwardMatrix, sizeof(out->dngForwardMatrix));
	memcpy(out->dngCalibration, raw.dngCalibration, sizeof(out->dngCalibration));
	memcpy(out->dngAnalogBalance, raw.dngAnalogBalance, sizeof(out->dngAnalogBalance));
	memcpy(out->dngAsShotNeutral, raw.dngAsShotNeutral, sizeof(out->dngAsShotNeutral));
	out->hasAsShotNeutral = raw.hasAsShotNeutral;
	memcpy(out->uniqueCameraModel, raw.uniqueCameraModel, sizeof(out->uniqueCameraModel));
	memcpy(out->camToXYZ, raw.camToXYZ, sizeof(out->camToXYZ));
	out->hasMatrix = raw.hasMatrix;
	memcpy(out->asShotWB, raw.asShotWB, sizeof(out->asShotWB));
	memcpy(out->relativeWB, raw.relativeWB, sizeof(out->relativeWB));
}
