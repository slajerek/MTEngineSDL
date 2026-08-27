#pragma once

// RD-C: the five profile seams (design #5/#5.1) -- the boundary RD-D's DCP
// support plugs into WITHOUT cutting RD-C's pixel chain. Baselines live in
// CDevelopBaselineSeams; RD-C's chain consumes ONLY these interfaces, so a
// DCP implementation is an add, never surgery.
//
// Domain contract (#5.2): THE SEAM CONVERTS, NOT THE CHAIN. Each seam is
// handed data in the chain's domain at its stage and declares below what it
// consumes; converting is the implementation's job.
//
// This header is UNCONDITIONAL (the CRawDecoder.h precedent): no develop
// define anywhere in it.

#include "SYS_Defs.h"
#include <cstring>
#include <functional>

struct SRawDecodeResult;

// ---------------------------------------------------------------------------
// Inputs to Resolve (#5.1)
// ---------------------------------------------------------------------------

// RD-A #5.7's DNG block, PLUS camToXYZ/hasMatrix (spec rev 8 F2): a non-DNG
// raw has dngFields[0]|dngFields[1] == 0, and the ONLY matrix available for
// the baseline Kelvin->neutral conversion is camToXYZ.
struct SRawDngTags
{
	unsigned dngFields[2]     = {};
	unsigned dngIlluminant[2] = {};
	float dngColorMatrix[2][3][3]   = {};
	float dngForwardMatrix[2][3][3] = {};
	float dngCalibration[2][3][3]   = {};
	float dngAnalogBalance[3]       = { 1.f, 1.f, 1.f };
	float dngAsShotNeutral[3]       = {};
	bool  hasAsShotNeutral          = false;
	char  uniqueCameraModel[64]     = {};

	// Not DNG tags -- the decode's own matrix + the as-shot multipliers, so
	// the baseline Resolve can convert Kelvin->neutral and derive the as-shot
	// reference neutral (spec #5.1 rev 8, #4.1).
	float camToXYZ[3][3] = {};
	bool  hasMatrix      = false;
	float asShotWB[4]    = {};
	// The multipliers LibRaw ACTUALLY applied (pre_mul post-process). The
	// baseline As-Shot referenceNeutral derives from THESE, not asShotWB:
	// normalise_G(1/relativeWB) makes stages 2 and 3b compose to identity at
	// As Shot BY CONSTRUCTION -- including the zeroed-cam_mul case where
	// LibRaw applied daylight (RD-A #5.3 trap 1) and asShotWB says nothing.
	float relativeWB[4]  = { 1.f, 1.f, 1.f, 1.f };
};

// Fills the struct straight off a decode result. Lives here so every caller
// (production render, tests) assembles the SAME inputs.
void PC_FillRawDngTags(const SRawDecodeResult &raw, SRawDngTags *out);

// The requested white balance (#4.1).
struct SWhiteBalanceRequest
{
	enum EMode { AsShot = 0, TempTint = 1, Preset = 2 };
	EMode mode = AsShot;
	float temperature = 0.f;
	float tint        = 0.f;
	// RD-B's flag: raw files carry real Kelvin; non-raw sidecars carry the
	// +/-100 relative scale (#4.1, #11.1 row 1). The baseline honours Kelvin
	// only; the relative scale has no meaning without a profile reference.
	bool  temperatureIsKelvin = true;
};

// ---------------------------------------------------------------------------
// Seam 1: IProfileCameraInputs -- ONE pre-pixel Resolve call (#5.1)
// ---------------------------------------------------------------------------

// NOT a pixel stage. Called ONCE per render, before the chain starts.
// Everything returned is ALREADY interpolated, ordered and gated -- RD-C
// branches on none of it.
struct SCameraResolution
{
	// DCP tag. SUMS with the DNG's BaselineExposure at stage 1 (#4.7) --
	// linear, individual camera space. For NON-DNG raws this is the ONLY
	// source of the per-camera offset (RD-D #4.5).
	float baselineExposureOffset = 0.f;

	// The profile's ProfileCalibrationSignature (0xC6F4) vs the FILE's
	// CameraCalibrationSignature (0xC6F3). LibRaw parses NEITHER tag
	// (RD-A #5.7), so both strings AND the comparison live in RD-D.
	// DIAGNOSTICS ONLY: the gate is already applied to `calibration` below,
	// so RD-E can say WHY a profile rendered without calibration.
	bool calibrationSignatureMatches = true;

	float whiteXY[2]            = {};       // the resolved rendering white
	float correlatedColorTemp   = 0.f;      // K   -- diagnostics/UI, never an
	float tintValue             = 0.f;      //        input to the render
	float interpolationWeight   = 0.f;      // RD-D #4.2's g, clamped [0,1]

	// CC_interp: interpolated at `g`, sorted per RD-D #4.2, and IDENTITY when
	// the signature mismatches. Stage 3 applies (AB * calibration)^-1 as given.
	float calibration[3][3]  = { {1,0,0}, {0,1,0}, {0,0,1} };
	float analogBalance[3]   = { 1.f, 1.f, 1.f };
	// Stage 3b divides by this (#4.0). THE CAMERA NEUTRAL, G = 1 convention
	// (spec rev 8 / plan F2): for as-shot it is the normalised RECIPROCAL of
	// the cam_mul multipliers, or dngAsShotNeutral directly when present.
	float referenceNeutral[3]= { 1.f, 1.f, 1.f };

	// Baseline-only diagnostic: a named WB preset had no Temp/Tint and the
	// baseline cannot derive one from a single matrix -- fell back to as-shot
	// (#4.1). RD-E badge food.
	bool warnedPresetFallback = false;
};

class IProfileCameraInputs
{
public:
	virtual ~IProfileCameraInputs() {}
	// `file` is RD-A #5.7's DNG block, straight off SRawDecodeResult. RD-C
	// passes it IN -- the direction matters: the reference neutral, the
	// temperature and the interpolation weight are a fixed point only the
	// implementation can close (spec #5.1, RD-D #4.0).
	virtual bool Resolve(const SRawDngTags &file,
	                     const SWhiteBalanceRequest &requested,
	                     SCameraResolution *out) = 0;
};

// ---------------------------------------------------------------------------
// Seam 2: IProfileMatrixStage -- stage 4
// ---------------------------------------------------------------------------

class IProfileMatrixStage
{
public:
	virtual ~IProfileMatrixStage() {}
	// Reference-space camera RGB (ALREADY neutral-divided -- stage 3b owns
	// the division, #4.0 "who divides") -> XYZ(D50). The chain applies the
	// matrix itself (fused); the seam only supplies it. A DCP implementation
	// that divided internally would apply the white balance twice.
	virtual void GetMatrix(float outM[3][3]) = 0;
};

// ---------------------------------------------------------------------------
// Seams 3+4: IProfileHueSatMap (stage 6) / IProfileLookTable (stage 8b)
// ---------------------------------------------------------------------------

class IProfileHueSatMap
{
public:
	virtual ~IProfileHueSatMap() {}
	// Identity => stage 6 is skipped entirely (free baseline path).
	virtual bool IsIdentity() const = 0;
	// One pixel, in-place. Chain domain at stage 6 is LINEAR ProPhoto -- the
	// seam applies its own tag encoding internally (#5, #5.2 row 1).
	virtual void ApplyPixel(float rgb[3]) = 0;
};

class IProfileLookTable
{
public:
	virtual ~IProfileLookTable() {}
	// Identity => stage 8b is skipped entirely (free baseline path).
	virtual bool IsIdentity() const = 0;
	// One pixel, in-place. Chain domain at stage 8b is GAMMA ProPhoto (#2.3);
	// the seam owns the decode -> HSV -> table -> re-encode round trip
	// (#5.2 consequence 2). Wired through the chain's SeamRgbOp descriptor so
	// it runs INSIDE the fused loop -- no chain surgery when RD-D lands.
	virtual void ApplyPixelChainDomain(float rgb[3]) = 0;
};

// ---------------------------------------------------------------------------
// Seam 5: IProfileToneCurve -- stage 9
// ---------------------------------------------------------------------------

class IProfileToneCurve
{
public:
	virtual ~IProfileToneCurve() {}
	// The LINEAR-domain curve (#5.2) as a callable -- an empty function means
	// "no curve": stage 9 is skipped (e.g. the baseline with its
	// approximation disabled, #4.8). A callable, not a fixed point array:
	// the baseline is the published 1025-entry acr3 default table and RD-D's
	// is a solved spline -- both wrap naturally, neither fits a small
	// control-point array. The CHAIN folds encode(curve(decode(x))) into the
	// stage LUT (#5.2 consequence 1) -- the seam never sees gamma-encoded
	// data.
	virtual std::function<float(float)> GetLinearCurve() = 0;
};
