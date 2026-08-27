#pragma once

// RD-C: the develop chain executor (#6's split: the APP computes LUTs and
// hands this an ORDERED operator chain; ACR's numbers stay in the app, the
// pixel loop stays here). Buffer: interleaved float RGB, 3 channels,
// scene-referred (#2.2 -- negatives and >1 legal until the rolloff op).
//
// EXECUTION MODEL, a declared deviation from #8's fused-single-pass ideal:
// v1 runs each op as its own measured pass. That is what makes DoD 10's
// per-operator timings EXACT rather than segment-attributed, and on the
// bandwidth numbers of the dev machine (~25 ms for 16 passes over a 24 MP
// float buffer) it is affordable. Fusion of adjacent pointwise ops is the
// recorded optimisation to take WHEN the published timings demand it
// (roadmap #2.9's "SIMD/GPU only if benchmarks demand" discipline) -- the
// descriptor list is already shaped for it: everything except SpatialOp and
// CropResampleOp is pointwise.
//
// SeamRgbOp (plan F3) is the seam-owned per-pixel escape: function pointer +
// context, receives/returns CHAIN-domain RGB, everything inside is the
// seam's business. Stage 8b (RD-D's LookTable) maps to it, so the #5.2
// decode->HSV->table->re-encode round trip runs inside the pixel loop with
// no chain surgery when RD-D lands.

#include "SYS_Defs.h"
#include <functional>
#include <string>
#include <vector>

struct SDevLut1D;

enum class EDevelopOpKind
{
	Matrix,          // 3x3 on RGB
	DiagMul,         // per-channel multiply (stages 2/3b, exposure gain)
	Lut1DAll,        // one LUT applied to all three channels
	Lut1DPerChannel, // three LUTs, R/G/B (per-channel point curves)
	Encode,          // stage 7: linear -> gamma (PC_DevEncode)
	Decode,          // stage 14: gamma -> linear
	RgbToneCurve,    // stage 9 (RD-D F7): decode -> PC_DevRgbTone with the
	                 // LINEAR-domain op.lut -> encode. NOT per-channel --
	                 // dng_sdk's RGBTone curves max+min and rebuilds the
	                 // middle by ratio in linear space.
	HsvOp,           // fn(ctx, hsv[3]) in the CURRENT domain's HSV
	SeamRgbOp,       // fn(ctx, rgb[3]) -- seam-owned, chain-domain in/out
	SpatialOp,       // whole-buffer callback; NOT pointwise
	CropResample,    // replaces the buffer + dims; NOT pointwise
	Rolloff,         // fn(ctx, rgb[3]) -- pointwise (stage 16 shape)
};

struct SDevelopOp
{
	EDevelopOpKind kind;
	const char    *name = "";        // for the published timings (DoD 10)

	float m[3][3] = {};              // Matrix
	float d[3]    = { 1, 1, 1 };     // DiagMul
	const SDevLut1D *lut  = nullptr; // Lut1DAll
	const SDevLut1D *lutR = nullptr; // Lut1DPerChannel
	const SDevLut1D *lutG = nullptr;
	const SDevLut1D *lutB = nullptr;

	void (*pixelFn)(void *ctx, float rgb[3]) = nullptr;   // HsvOp (hsv in
	                                 // rgb[]'s place), SeamRgbOp, Rolloff
	void *pixelCtx = nullptr;

	// SpatialOp / CropResample: whole-buffer. For CropResample the callback
	// REPLACES the vector and updates dims.
	std::function<void(std::vector<float> &rgb, int &w, int &h)> bufferFn;
};

struct SDevelopOpTiming
{
	std::string name;
	float       ms = 0.f;
};

struct SDevelopChainStats
{
	std::vector<SDevelopOpTiming> timings;      // one per executed op
	size_t scratchHighWaterBytes = 0;           // spatial/crop scratch (#8)
};

class CDevelopChain
{
public:
	void Clear() { ops_.clear(); }
	void Add(const SDevelopOp &op) { ops_.push_back(op); }
	size_t OpCount() const { return ops_.size(); }

	// Executes in order over the interleaved RGB float buffer. `w`/`h` may
	// change (CropResample). Returns false only on a malformed op.
	bool Execute(std::vector<float> &rgb, int &w, int &h,
	             SDevelopChainStats *outStats) const;

private:
	std::vector<SDevelopOp> ops_;
};
