#include "CDcpApply.h"
#include "CDcpTemperature.h"
#include "DevelopMath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Construction (dng_color_spec's ctor, spec F3/F14 + the signature gate)
// ---------------------------------------------------------------------------

static void MatIdentity(float m[3][3])
{
	std::memset(m, 0, sizeof(float) * 9);
	m[0][0] = m[1][1] = m[2][2] = 1.f;
}

static void MatCopy9(const float src[3][3], float dst[3][3])
{
	std::memcpy(dst, src, sizeof(float) * 9);
}

static void MatBlend(const float a[3][3], const float b[3][3], float g,
                     float out[3][3])
{
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			out[r][c] = g * a[r][c] + (1.f - g) * b[r][c];
}

CDcpSeamSet::CDcpSeamSet(const SDcpProfile &profile,
                         const std::string &fileCalibrationSignature)
	: profile_(profile)
	, fileSignature_(fileCalibrationSignature)
{
	lookProxy_.owner = this;
	MatIdentity(cc_[0]);
	MatIdentity(cc_[1]);
	MatIdentity(fm_[0]);
	MatIdentity(fm_[1]);

	// The signature gate lives HERE (spec #2.1 rev 7: RD-D holds both
	// strings, applies the gate itself, hands back calibration already
	// identity on mismatch). dng_sdk compares dng_string equality --
	// case-sensitive; we trim nothing because CDcpProfile already dropped
	// trailing NULs on both sides.
	signatureMatches_ =
		(fileSignature_ == profile_.calibrationSignature);
}

bool CDcpSeamSet::Resolve(const SRawDngTags &file,
                          const SWhiteBalanceRequest &requested,
                          SCameraResolution *out)
{
	*out = SCameraResolution();
	resolved_ = false;

	if (!profile_.hasColorMatrix1)
		return false;

	// ---- construction-equivalent state (redone per render: the FILE's
	// tags arrive here, not at ctor -- #4.0 passes them in) --------------

	// AB from the file.
	analogBalance_[0] = file.dngAnalogBalance[0];
	analogBalance_[1] = file.dngAnalogBalance[1];
	analogBalance_[2] = file.dngAnalogBalance[2];
	for (int i = 0; i < 3; i++)
		if (analogBalance_[i] <= 0.f)
			analogBalance_[i] = 1.f;

	// CC from the file, gated by the signature comparison (identity on
	// mismatch; the bool survives as diagnostics only).
	MatIdentity(cc_[0]);
	MatIdentity(cc_[1]);
	if (signatureMatches_)
	{
		// The tracker's standing rule for RD-D: dngFields is AUTHORITATIVE
		// for presence -- never zero-test a matrix (LIBRAW_DNGFM_CALIBRATION
		// is bit 3; RD-A reconstructs the mask precisely so consumers do
		// not re-derive presence from values).
		const unsigned kDngFmCalibration = 1u << 3;
		for (int k = 0; k < 2; k++)
			if (file.dngFields[k] & kDngFmCalibration)
				MatCopy9(file.dngCalibration[k], cc_[k]);
	}
	out->calibrationSignatureMatches = signatureMatches_;

	// Product matrices AB*CC_i*CM_i (F3: dng interpolates the PRODUCT).
	float cm[2][3][3];
	MatCopy9(profile_.colorMatrix1, cm[0]);
	if (profile_.hasColorMatrix2)
		MatCopy9(profile_.colorMatrix2, cm[1]);
	else
		MatCopy9(profile_.colorMatrix1, cm[1]);
	for (int k = 0; k < 2; k++)
	{
		float ccCm[3][3];
		PC_Mat3Mul(cc_[k], cm[k], ccCm);
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++)
				productCM_[k][r][c] = analogBalance_[r] * ccCm[r][c];
	}

	hasFm_[0] = profile_.hasForwardMatrix1;
	hasFm_[1] = profile_.hasForwardMatrix2;
	if (hasFm_[0]) MatCopy9(profile_.forwardMatrix1, fm_[0]);
	if (hasFm_[1]) MatCopy9(profile_.forwardMatrix2, fm_[1]);

	temp1_ = DCP_IlluminantToTemperature(profile_.calibrationIlluminant1);
	temp2_ = DCP_IlluminantToTemperature(profile_.calibrationIlluminant2);
	sortedMap1_ = &profile_.hueSatMap1;
	sortedMap2_ = &profile_.hueSatMap2;

	// F14: single-illuminant collapse.
	if (!profile_.hasColorMatrix2 || temp1_ <= 0.0 || temp2_ <= 0.0
	    || temp1_ == temp2_)
	{
		temp1_ = 5000.0;
		temp2_ = 5000.0;
		MatCopy9(productCM_[0], productCM_[1]);
		MatCopy9(cc_[0], cc_[1]);
		if (hasFm_[0]) { MatCopy9(fm_[0], fm_[1]); hasFm_[1] = hasFm_[0]; }
		sortedMap2_ = &profile_.hueSatMap1;
	}
	else if (temp1_ > temp2_)
	{
		// The out-of-order swap: EVERYTHING swaps together (#4.2), the
		// hue maps included (F9's reverseOrder, folded into one sort).
		std::swap(temp1_, temp2_);
		float t[3][3];
		MatCopy9(productCM_[0], t); MatCopy9(productCM_[1], productCM_[0]); MatCopy9(t, productCM_[1]);
		MatCopy9(cc_[0], t); MatCopy9(cc_[1], cc_[0]); MatCopy9(t, cc_[1]);
		MatCopy9(fm_[0], t); MatCopy9(fm_[1], fm_[0]); MatCopy9(t, fm_[1]);
		std::swap(hasFm_[0], hasFm_[1]);
		std::swap(sortedMap1_, sortedMap2_);
	}

	// ---- the white point -------------------------------------------------
	float whiteXY[2] = { DCP_kD50XY[0], DCP_kD50XY[1] };
	bool warned = false;

	auto resolver = [this](const float xy[2], float outM[3][3])
	{
		ComputeProductMatrix(WeightForXY(xy), outM);
	};

	auto asShotXY = [&](float outXY[2])
	{
		// The INDIVIDUAL camera neutral (F4): dngAsShotNeutral when the
		// file carries it, else derived from what LibRaw applied -- the
		// same derivation the baseline uses.
		float neutral[3];
		if (file.hasAsShotNeutral)
		{
			neutral[0] = file.dngAsShotNeutral[0];
			neutral[1] = file.dngAsShotNeutral[1];
			neutral[2] = file.dngAsShotNeutral[2];
		}
		else
		{
			const float g = (file.relativeWB[1] > 1e-9f) ? file.relativeWB[1] : 1.f;
			for (int c = 0; c < 3; c++)
			{
				const float m = (file.relativeWB[c] > 1e-9f) ? file.relativeWB[c] : g;
				neutral[c] = g / m;
			}
		}
		DCP_NeutralToXY(neutral, resolver, outXY);
	};

	switch (requested.mode)
	{
		case SWhiteBalanceRequest::AsShot:
			asShotXY(whiteXY);
			break;
		case SWhiteBalanceRequest::TempTint:
		{
			if (requested.temperatureIsKelvin && requested.temperature > 0.f)
			{
				SDevTempTint tt;
				tt.temperatureK = requested.temperature;
				tt.tint = requested.tint;
				if (!PC_TempTintToXy(tt, whiteXY))
				{
					asShotXY(whiteXY);
					warned = true;
				}
			}
			else
			{
				// The relative +/-100 scale has no meaning without a
				// profile REFERENCE render -- same policy as the baseline.
				asShotXY(whiteXY);
				warned = true;
			}
			break;
		}
		case SWhiteBalanceRequest::Preset:
		default:
			// Named preset without numbers: fall back to as-shot + warn
			// (#4.1's declared limitation, unchanged under a profile).
			asShotXY(whiteXY);
			warned = true;
			break;
	}

	// ---- SetWhiteXY equivalents (F3/F6) -----------------------------------
	const float g = WeightForXY(whiteXY);
	resolvedWeight_ = g;

	float colorMatrix[3][3];
	ComputeProductMatrix(g, colorMatrix);

	// cameraWhite = pin(0.001..1, maxnorm(colorMatrix * XYZ(white))).
	float whiteXYZ[3] = { whiteXY[0] / whiteXY[1], 1.f,
	                      (1.f - whiteXY[0] - whiteXY[1]) / whiteXY[1] };
	float cameraWhite[3];
	PC_Mat3Apply(colorMatrix, whiteXYZ, cameraWhite);
	float maxW = std::max(cameraWhite[0],
	                      std::max(cameraWhite[1], cameraWhite[2]));
	if (maxW <= 0.f)
		return false;
	for (int i = 0; i < 3; i++)
		cameraWhite[i] = std::clamp(cameraWhite[i] / maxW, 0.001f, 1.f);

	// CC interpolated ALONE (the FM branch divides it back out, F3).
	float ccInterp[3][3];
	MatBlend(cc_[0], cc_[1], g, ccInterp);

	// referenceNeutral = Invert(AB*CC_interp) * cameraWhite.
	float abcc[3][3];
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			abcc[r][c] = analogBalance_[r] * ccInterp[r][c];
	float abccInv[3][3];
	if (!PC_Mat3Invert(abcc, abccInv))
		return false;
	float refNeutral[3];
	PC_Mat3Apply(abccInv, cameraWhite, refNeutral);

	// ---- the stage-4 matrix ------------------------------------------------
	// FM branch (F3, exact decomposition onto RD-C's 3/3b/4): FM_interp
	// alone -- stages 3 and 3b already undid AB*CC and divided refNeutral.
	// F15: a lone FM is USED (has1 -> FM1), not bypassed.
	if (hasFm_[0] && hasFm_[1])
		MatBlend(fm_[0], fm_[1], g, stage4_);
	else if (hasFm_[0])
		MatCopy9(fm_[0], stage4_);
	else if (hasFm_[1])
		MatCopy9(fm_[1], stage4_);
	else
	{
		// CM-only branch (F6): CameraToPCS = Invert(rescale(colorMatrix *
		// MapWhite(PCS -> white))); then re-compose diag(refNeutral) *
		// (AB*CC_interp) on the right for RD-C's already-divided input.
		float mapWhite[3][3];
		DCP_MapWhiteMatrix(DCP_kPcsXY, whiteXY, mapWhite);
		float pcsToCamera[3][3];
		PC_Mat3Mul(colorMatrix, mapWhite, pcsToCamera);

		float pcsWhiteCam[3];
		PC_Mat3Apply(pcsToCamera, DCP_kPcsXYZ, pcsWhiteCam);
		const float scale = std::max(pcsWhiteCam[0],
		                    std::max(pcsWhiteCam[1], pcsWhiteCam[2]));
		if (scale <= 0.f)
			return false;
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++)
				pcsToCamera[r][c] /= scale;

		float cameraToPcs[3][3];
		if (!PC_Mat3Invert(pcsToCamera, cameraToPcs))
			return false;

		float diagRef[3][3] = {};
		diagRef[0][0] = refNeutral[0];
		diagRef[1][1] = refNeutral[1];
		diagRef[2][2] = refNeutral[2];
		float t[3][3];
		PC_Mat3Mul(diagRef, abcc, t);
		PC_Mat3Mul(cameraToPcs, t, stage4_);
	}

	// ---- hue map interpolation at the resolved white (F9) ------------------
	resolvedHueSat_ = SDcpHueSatMap();
	if (sortedMap1_->IsValid())
	{
		if (!sortedMap2_->IsValid() || sortedMap2_ == sortedMap1_)
			resolvedHueSat_ = *sortedMap1_;
		else
			InterpolateMap(*sortedMap1_, *sortedMap2_, g, &resolvedHueSat_);
	}

	// ---- outputs -----------------------------------------------------------
	out->baselineExposureOffset = profile_.hasBaselineExposureOffset
		? profile_.baselineExposureOffset : 0.f;
	out->whiteXY[0] = whiteXY[0];
	out->whiteXY[1] = whiteXY[1];
	SDevTempTint tt;
	if (PC_XyToTempTint(whiteXY, &tt))
	{
		out->correlatedColorTemp = tt.temperatureK;
		out->tintValue = tt.tint;
	}
	out->interpolationWeight = g;
	MatCopy9(ccInterp, out->calibration);
	out->analogBalance[0] = analogBalance_[0];
	out->analogBalance[1] = analogBalance_[1];
	out->analogBalance[2] = analogBalance_[2];
	out->referenceNeutral[0] = refNeutral[0];
	out->referenceNeutral[1] = refNeutral[1];
	out->referenceNeutral[2] = refNeutral[2];
	out->warnedPresetFallback = warned;

	resolved_ = true;
	return true;
}

float CDcpSeamSet::WeightForXY(const float xy[2]) const
{
	SDevTempTint tt;
	if (!PC_XyToTempTint(xy, &tt) || tt.temperatureK <= 0.f)
		return 1.f;
	const double T = tt.temperatureK;
	if (T <= temp1_)
		return 1.f;
	if (T >= temp2_)
		return 0.f;
	// Linear in mireds (#4.2), weight 1 to the LOWER temperature.
	const double invT = 1.0 / T;
	return (float)((invT - 1.0 / temp2_)
	               / (1.0 / temp1_ - 1.0 / temp2_));
}

void CDcpSeamSet::ComputeProductMatrix(float g, float outM[3][3]) const
{
	if (g >= 1.f)
		MatCopy9(productCM_[0], outM);
	else if (g <= 0.f)
		MatCopy9(productCM_[1], outM);
	else
		MatBlend(productCM_[0], productCM_[1], g, outM);
}

void CDcpSeamSet::InterpolateMap(const SDcpHueSatMap &m1,
                                 const SDcpHueSatMap &m2, float g,
                                 SDcpHueSatMap *out) const
{
	// F9: weight >= 1 -> map1, <= 0 -> map2, else element-wise blend
	// (dims equality was enforced at parse).
	if (g >= 1.f) { *out = m1; return; }
	if (g <= 0.f) { *out = m2; return; }
	out->hueDivisions = m1.hueDivisions;
	out->satDivisions = m1.satDivisions;
	out->valDivisions = m1.valDivisions;
	out->deltas.resize(m1.deltas.size());
	for (size_t i = 0; i < m1.deltas.size(); i++)
		out->deltas[i] = g * m1.deltas[i] + (1.f - g) * m2.deltas[i];
}

void CDcpSeamSet::GetMatrix(float outM[3][3])
{
	MatCopy9(stage4_, outM);
}

// ---------------------------------------------------------------------------
// The table sampler (spec F8 -- RefBaselineHueSatMap, followed verbatim,
// with the ONE documented deviation: the value pin is index-only)
// ---------------------------------------------------------------------------

namespace {

struct SHsvSample
{
	float hueShiftDeg = 0.f;
	float satScale = 1.f;
	float valScale = 1.f;
};

// h in [0,6); s, vForIndex in [0,1] (caller clamps for the INDEX only).
SHsvSample SampleMap(const SDcpHueSatMap &map, float h6, float s,
                     float vForIndex)
{
	SHsvSample out;
	const int hueDiv = (int)map.hueDivisions;
	const int satDiv = (int)map.satDivisions;
	const int valDiv = (int)map.valDivisions;

	const float hScale = (hueDiv < 2) ? 0.f : hueDiv * (1.f / 6.f);
	const float sScale = (float)(satDiv - 1);
	const int maxHueIndex0 = hueDiv - 1;
	const int maxSatIndex0 = satDiv - 2;
	const int hueStep = satDiv;
	const int valStep = hueDiv * hueStep;
	const float *base = map.deltas.data();

	const float hScaled = h6 * hScale;
	const float sScaled = s * sScale;
	int hIndex0 = (int)hScaled;
	int sIndex0 = std::min((int)sScaled, maxSatIndex0);
	int hIndex1 = hIndex0 + 1;
	if (hIndex0 >= maxHueIndex0)
	{
		hIndex0 = maxHueIndex0;
		hIndex1 = 0;
	}
	const float hFract1 = hScaled - (float)hIndex0;
	const float sFract1 = sScaled - (float)sIndex0;
	const float hFract0 = 1.f - hFract1;
	const float sFract0 = 1.f - sFract1;

	auto lerpPair = [&](const float *e00, const float *e01, float outHSV[3])
	{
		for (int i = 0; i < 3; i++)
			outHSV[i] = hFract0 * e00[i] + hFract1 * e01[i];
	};

	if (valDiv < 2)
	{
		// The 2.5-D path (Adobe Standard's common case).
		const float *e00 = base + (size_t)(hIndex0 * hueStep + sIndex0) * 3;
		const float *e01 = base + (size_t)(hIndex1 * hueStep + sIndex0) * 3;
		float lo[3], hi[3];
		lerpPair(e00, e01, lo);
		lerpPair(e00 + 3, e01 + 3, hi);
		out.hueShiftDeg = sFract0 * lo[0] + sFract1 * hi[0];
		out.satScale    = sFract0 * lo[1] + sFract1 * hi[1];
		out.valScale    = sFract0 * lo[2] + sFract1 * hi[2];
		return out;
	}

	const float vScale = (float)(valDiv - 1);
	const int maxValIndex0 = valDiv - 2;
	const float vScaled = vForIndex * vScale;
	const int vIndex0 = std::min((int)vScaled, maxValIndex0);
	const float vFract1 = vScaled - (float)vIndex0;
	const float vFract0 = 1.f - vFract1;

	auto tri = [&](int comp) -> float
	{
		const float *e00 = base + (size_t)(vIndex0 * valStep + hIndex0 * hueStep + sIndex0) * 3;
		const float *e01 = base + (size_t)(vIndex0 * valStep + hIndex1 * hueStep + sIndex0) * 3;
		const float *e10 = e00 + (size_t)valStep * 3;
		const float *e11 = e01 + (size_t)valStep * 3;
		const float lo = vFract0 * (hFract0 * e00[comp] + hFract1 * e01[comp])
		               + vFract1 * (hFract0 * e10[comp] + hFract1 * e11[comp]);
		const float *f00 = e00 + 3, *f01 = e01 + 3, *f10 = e10 + 3, *f11 = e11 + 3;
		const float hi = vFract0 * (hFract0 * f00[comp] + hFract1 * f01[comp])
		               + vFract1 * (hFract0 * f10[comp] + hFract1 * f11[comp]);
		return sFract0 * lo + sFract1 * hi;
	};
	out.hueShiftDeg = tri(0);
	out.satScale    = tri(1);
	out.valScale    = tri(2);
	return out;
}

// DNG_RGBtoHSV / DNG_HSVtoRGB (dng_utils.h), hexcone with h in [0,6).
void DngRgbToHsv(float r, float g, float b, float &h, float &s, float &v)
{
	v = std::max(r, std::max(g, b));
	const float gap = v - std::min(r, std::min(g, b));
	if (gap > 0.f && v > 0.f)
	{
		if (r == v)
		{
			h = (g - b) / gap;
			if (h < 0.f)
				h += 6.f;
		}
		else if (g == v)
			h = 2.f + (b - r) / gap;
		else
			h = 4.f + (r - g) / gap;
		s = gap / v;
	}
	else
	{
		h = 0.f;
		s = 0.f;
	}
}

void DngHsvToRgb(float h, float s, float v, float &r, float &g, float &b)
{
	if (s > 0.f)
	{
		if (h < 0.f)  h += 6.f;
		if (h >= 6.f) h -= 6.f;
		const int i = (int)h;
		const float f = h - (float)i;
		const float p = v * (1.f - s);
		const float q = v * (1.f - s * f);
		const float t = v * (1.f - s * (1.f - f));
		switch (i)
		{
			case 0: r = v; g = t; b = p; break;
			case 1: r = q; g = v; b = p; break;
			case 2: r = p; g = v; b = t; break;
			case 3: r = p; g = q; b = v; break;
			case 4: r = t; g = p; b = v; break;
			default: r = v; g = p; b = q; break;
		}
	}
	else
	{
		r = v;
		g = v;
		b = v;
	}
}

// The shared table application (stage 6 and stage 8b both call this with
// LINEAR ProPhoto, #4.3).
void ApplyMapLinear(const SDcpHueSatMap &map, int encoding, float rgb[3])
{
	float h, s, v;
	DngRgbToHsv(rgb[0], rgb[1], rgb[2], h, s, v);
	if (v <= 0.f)
		return;   // non-positive pixel: nothing meaningful to look up

	// Index inputs: s and v clamped to [0,1]; v ENCODED when the tag says
	// so (F8). The pixel's own v is NOT pinned -- the deviation.
	const float sIndex = std::clamp(s, 0.f, 1.f);
	float vIndex = std::clamp(v, 0.f, 1.f);
	if (encoding == 1 && map.valDivisions >= 2)
		vIndex = PC_DevEncode(vIndex);

	const SHsvSample d = SampleMap(map, h, sIndex, vIndex);

	h += d.hueShiftDeg * (6.f / 360.f);
	s = std::min(s * d.satScale, 1.f);
	if (s < 0.f)
		s = 0.f;

	if (v <= 1.f && map.valDivisions >= 2 && encoding == 1)
	{
		// dng-exact arm inside [0,1]: scale in the ENCODED domain, decode.
		float vEnc = PC_DevEncode(std::clamp(v, 0.f, 1.f));
		vEnc = std::clamp(vEnc * d.valScale, 0.f, 1.f);
		v = PC_DevDecode(vEnc);
	}
	else
	{
		// Linear arm; above 1.0 the scale applies unpinned (the documented
		// deviation -- no hidden clipper in a scene-referred container).
		v = v * d.valScale;
		if (v < 0.f)
			v = 0.f;
	}

	DngHsvToRgb(h, s, v, rgb[0], rgb[1], rgb[2]);
}

} // namespace

bool CDcpSeamSet::IsIdentity() const
{
	return !resolvedHueSat_.IsValid();
}

void CDcpSeamSet::ApplyPixel(float rgb[3])
{
	ApplyMapLinear(resolvedHueSat_, profile_.hueSatMapEncoding, rgb);
}

bool CDcpSeamSet::SLookProxy::IsIdentity() const
{
	return !owner->profile_.lookTable.IsValid();
}

void CDcpSeamSet::SLookProxy::ApplyPixelChainDomain(float rgb[3])
{
	// Stage 8b arrives gamma-encoded; the seam owns decode -> apply ->
	// re-encode (#4.3). The profile's Encoding == 1 and the chain's gamma
	// are BOTH the sRGB TRC and would cancel inside [0,1] -- implemented
	// as the pair anyway (RD-C #5.2: the cancellation is exact only there).
	float lin[3];
	for (int i = 0; i < 3; i++)
		lin[i] = PC_DevDecode(rgb[i]);
	ApplyMapLinear(owner->profile_.lookTable,
	               owner->profile_.lookTableEncoding, lin);
	for (int i = 0; i < 3; i++)
		rgb[i] = PC_DevEncode(lin[i]);
}

std::function<float(float)> CDcpSeamSet::GetLinearCurve()
{
	if (profile_.toneCurve.empty())
		return {};
	// The validated points as dng_spline_solver's natural cubic (#4.4) --
	// CDevSpline is RD-C's implementation of the same solver, with the
	// 2-point linear special case inside Solve.
	auto spline = std::make_shared<CDevSpline>();
	std::vector<std::array<float, 2>> pts;
	pts.reserve(profile_.toneCurve.size());
	for (const auto &p : profile_.toneCurve)
		pts.push_back({ p.first, p.second });
	if (!spline->Solve(reinterpret_cast<const float (*)[2]>(pts.data()),
	                   (int)pts.size()))
		return {};
	return [spline](float x) { return spline->Evaluate(x); };
}
