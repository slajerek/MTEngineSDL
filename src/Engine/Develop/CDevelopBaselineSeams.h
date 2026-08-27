#pragma once

// RD-C: the five baseline seam implementations (#5's table, right column) --
// what renders when NO camera profile is selected, which roadmap #2.6 makes
// the out-of-box default (Adobe DCPs are never bundled). RD-D's DCP
// implementations replace these per-seam; nothing here branches on "is there
// a profile".

#include "DevelopSeams.h"

// Seam 1 baseline: offset 0, identity calibration, unit AB (#5.1 -- "not a
// simplification but the same statement #4.0's table already makes": stage 3
// must be identity whenever stage 4 is the camToXYZ fallback, because LibRaw
// pre-folds AB*CC into that matrix).
class CBaselineCameraInputs : public IProfileCameraInputs
{
public:
	virtual bool Resolve(const SRawDngTags &file,
	                     const SWhiteBalanceRequest &requested,
	                     SCameraResolution *out) override;
};

// Seam 2 baseline: LibRaw's camToXYZ (D65-normalised, RD-A #6.1) with the
// Bradford D65->D50 adaptation folded in AT CONSTRUCTION -- the adaptation
// lives INSIDE this implementation so the seam is uniform and a DCP path
// (natively D50 via ForwardMatrix) needs none (#5 "Why XYZ(D50)").
class CBaselineMatrixStage : public IProfileMatrixStage
{
public:
	CBaselineMatrixStage(const float camToXYZ[3][3], bool hasMatrix);
	virtual void GetMatrix(float outM[3][3]) override;
	bool HasMatrix() const { return hasMatrix_; }

private:
	float folded_[3][3];
	bool  hasMatrix_;
};

// Seams 3+4 baselines: identity -- the stages are skipped entirely (#5).
class CBaselineHueSatMap : public IProfileHueSatMap
{
public:
	virtual bool IsIdentity() const override { return true; }
	virtual void ApplyPixel(float rgb[3]) override { (void)rgb; }
};

class CBaselineLookTable : public IProfileLookTable
{
public:
	virtual bool IsIdentity() const override { return true; }
	virtual void ApplyPixelChainDomain(float rgb[3]) override { (void)rgb; }
};

// Seam 5 baseline: the default-render approximation (#4.8) -- the PUBLISHED
// dng_tone_curve_acr3_default 1025-entry table (linear domain), individually
// switchable so #9.1's identity test can disable it. Boundary ruling (plan
// F4): published DNG SDK data, engine-eligible by the same #5.1 precedent as
// dng_temperature -- NOT an ACR-behaviour-derived number.
class CBaselineToneCurve : public IProfileToneCurve
{
public:
	explicit CBaselineToneCurve(bool enableApproximation)
		: enabled_(enableApproximation) {}
	virtual std::function<float(float)> GetLinearCurve() override;

private:
	bool enabled_;
};

// The published table itself, exposed for tests (count is 1025; endpoints
// exactly 0 and 1; monotone nondecreasing -- all three pinned by
// CTestDevelopPipeline so a transcription error cannot ship silently).
const float *PC_Acr3DefaultCurveTable(int *outCount);

// The curve as a function: the table's own lerp inside [0,1] (the published
// Evaluate semantics), slope-extended OUTSIDE -- our container is
// scene-referred and stage 9 sees values above 1 (the published code clamps
// because dng_render's input is already display-limited; ours is not, and
// clamping here would be the silent clipper #2.2 forbids). Documented
// deviation, endpoints and interior identical to the published curve.
float PC_Acr3DefaultCurveEvaluate(float x);
