#pragma once

// RD-D #4: the five seam implementations over a parsed DCP -- the objects
// RD-C's SDevelopRenderSeams points at when a profile renders. All
// arithmetic follows dng_sdk as pinned in spec #0 (F3/F4/F6/F8/F9/F14/F15);
// the ONE documented deviation is the HueSatMap value pin (F8): dng_sdk
// pins v*valScale to [0,1], we clamp only the table INDEX and leave the
// scale unpinned above 1 -- RD-C #2.2's scene-referred container must not
// acquire a hidden clipper inside stage 6/8b.
//
// Lifetime: one CDcpSeamSet per render, constructed from the parsed
// profile + the file's DNG block + the file's CameraCalibrationSignature
// (read by the caller via CDcpProfile::ReadFileTags -- the F12 route).
// Resolve() runs before any pixel work (RD-C guarantees the order); the
// matrix/table seams serve what Resolve computed.

#include "SYS_Defs.h"
#include "DevelopSeams.h"
#include "CDcpProfile.h"

#include <string>

// The LookTable interface ALSO declares IsIdentity(); a single class
// implementing both table seams would fuse two different identity answers
// into one override, so the LookTable half lives on a nested proxy.
class CDcpSeamSet : public IProfileCameraInputs,
                    public IProfileMatrixStage,
                    public IProfileHueSatMap,
                    public IProfileToneCurve
{
public:
	CDcpSeamSet(const SDcpProfile &profile,
	            const std::string &fileCalibrationSignature);

	// The five seam pointers (SDevelopRenderSeams is app-side; the app's
	// PC_BuildDcpSeamSet fills it from these). ToneCurveSeam() is nullptr
	// when the profile carries no ProfileToneCurve (spec F13: the baseline
	// approximation stays ON).
	IProfileCameraInputs *CameraInputsSeam() { return this; }
	IProfileMatrixStage  *MatrixStageSeam()  { return this; }
	IProfileHueSatMap    *HueSatMapSeam()    { return this; }
	IProfileLookTable    *LookTableSeam()    { return &lookProxy_; }
	IProfileToneCurve    *ToneCurveSeam()
	{
		return profile_.toneCurve.empty() ? nullptr : this;
	}

	// IProfileCameraInputs (spec #4.0: THE one pre-pixel call).
	virtual bool Resolve(const SRawDngTags &file,
	                     const SWhiteBalanceRequest &requested,
	                     SCameraResolution *out) override;

	// IProfileMatrixStage: the stage-4 matrix Resolve computed (FM branch
	// or the CM-only branch re-composed for RD-C's already-divided
	// convention, spec F3/F6).
	virtual void GetMatrix(float outM[3][3]) override;

	// IProfileHueSatMap (stage 6, linear ProPhoto).
	virtual bool IsIdentity() const override;
	virtual void ApplyPixel(float rgb[3]) override;

	// IProfileToneCurve (stage 9): the validated points as a spline.
	virtual std::function<float(float)> GetLinearCurve() override;

	// Diagnostics for tests: the resolved interpolation weight.
	float ResolvedWeight() const { return resolvedWeight_; }

private:
	struct SLookProxy : public IProfileLookTable
	{
		CDcpSeamSet *owner = nullptr;
		virtual bool IsIdentity() const override;
		virtual void ApplyPixelChainDomain(float rgb[3]) override;
	};
	SLookProxy lookProxy_;

	void ComputeProductMatrix(float g, float outM[3][3]) const;
	float WeightForXY(const float xy[2]) const;
	void InterpolateMap(const SDcpHueSatMap &m1, const SDcpHueSatMap &m2,
	                    float g, SDcpHueSatMap *out) const;

	SDcpProfile profile_;              // copy: seam set outlives caller scope
	std::string fileSignature_;
	bool signatureMatches_ = false;

	// Construction-time state (F3/F14): product matrices AB*CC*CM per
	// illuminant, CCs, FMs, temperatures -- sorted so T1 <= T2, maps sorted
	// with them.
	float productCM_[2][3][3];
	float cc_[2][3][3];
	float fm_[2][3][3];
	bool  hasFm_[2] = { false, false };
	float analogBalance_[3] = { 1.f, 1.f, 1.f };
	double temp1_ = 5000.0, temp2_ = 5000.0;
	const SDcpHueSatMap *sortedMap1_ = nullptr;   // into profile_
	const SDcpHueSatMap *sortedMap2_ = nullptr;

	// Resolve() products.
	bool  resolved_ = false;
	float resolvedWeight_ = 0.f;
	float stage4_[3][3] = { {1,0,0}, {0,1,0}, {0,0,1} };
	SDcpHueSatMap resolvedHueSat_;     // interpolated at the resolved white
};
