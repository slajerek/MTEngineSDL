#pragma once

#include "CTest.h"

// RD-A tasks 4-7: CRawDecoder tests. All classes here are registered
// unconditionally in the the photo app suite (a guarded registration would be
// always-false there, design #9) and skip-report at run time when
// CRawDecoder::IsAvailable() is false.
//
// Generated-tier tests (synthetic Bayer via DecodeBayer, synthetic DNG via
// the RawTestFixtures builder) have NO skip path beyond that availability
// check -- that is the point of the tier (design #10.1).

// Design #10 case 2 + case 8: synthetic RGGB decodes with the right shape and
// a flat neutral field stays flat and neutral; failure paths return false
// with a reason, never crash, and leave the result struct zeroed.
class CTestRawDecodeSynthetic : public CTest
{
public:
	virtual const char *GetName() override { return "RawDecodeSynthetic"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};

// Design #10 case 3: two synthetic exposures differing by a known factor
// produce outputs differing by the same factor -- the invariant proving no
// tone curve leaked in, and the most important property this phase owes RD-C.
class CTestRawLinearity : public CTest
{
public:
	virtual const char *GetName() override { return "RawLinearity"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};

// Design #10 case 5: an open_bayer input decodes successfully with
// hasMatrix == false -- exercising the #6.2 rule (raw_color read between
// unpack and dcraw_process), not the old cam_xyz test.
class CTestRawMissingMatrix : public CTest
{
public:
	virtual const char *GetName() override { return "RawMissingMatrix"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};

// Design #10 case 4 -- THE load-bearing test: a camera-space neutral maps
// through camToXYZ to D65 (0.3127, 0.3290), NOT illuminant E (1/3, 1/3).
// 4a runs on the builder-generated synthetic DNG (no skip path, DNG matrix
// path); 4b repeats it on a real RAW from PC_RAW_FIXTURE_DIR (adobe_coeff
// table path -- a code path 4a cannot reach), skip-reported when absent.
// The tolerance is deliberately tight: D65 and E differ by ~0.021 in xy and
// LOOSENING IT IS THE FAILURE MODE (#6.1, #10.4).
class CTestRawMatrixD65 : public CTest
{
public:
	virtual const char *GetName() override { return "RawMatrixD65"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};

// Design #10 case 10: DefaultCrop applied, provenance recorded, opt-out
// works, malformed crops never kill the decode.
class CTestRawDefaultCrop : public CTest
{
public:
	virtual const char *GetName() override { return "RawDefaultCrop"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};

// Design #10 case 11: the #5.7 DNG calibration block reaches
// SRawDecodeResult with its parsedfields mask; a non-DNG input leaves the
// whole block zeroed with dngFields == 0. The test that stops RD-D reading
// a zero matrix as an identity calibration.
class CTestRawCalibrationProvenance : public CTest
{
public:
	virtual const char *GetName() override { return "RawCalibrationProvenance"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};

// Design #10 case 6 + roadmap #2.12: the buffer is NEVER pre-rotated.
// Generated half (no skip): a non-square synthetic DNG carrying
// Orientation=6 decodes at UNSWAPPED dimensions -- only a regressed
// user_flip=0 pin would let tiff_flip reach copy_mem_image's dimension
// swap. Real half: a portrait RAW from PC_RAW_FIXTURE_DIR/portrait/,
// skip-reported when absent.
class CTestRawNoRotation : public CTest
{
public:
	virtual const char *GetName() override { return "RawNoRotation"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};

// Design #10 case 7 (#2.11 -- the point of the phase on Windows/Linux):
// build capabilities report jpeg+zlib (unconditional), and the two
// converter DNGs from PC_RAW_FIXTURE_DIR/codec/ (lossy.dng, deflate.dng)
// decode when present (skip-REPORTED; DoD 8 makes present-and-passing
// mandatory on all three machines).
class CTestRawCodecCapability : public CTest
{
public:
	virtual const char *GetName() override { return "RawCodecCapability"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};

// Design #5.8: two decodes on two threads -- separate LibRaw instances are
// claimed safe; this exercises the claim (and runs under TSan in the
// out-of-suite probe, see the task-12 commit).
class CTestRawConcurrent : public CTest
{
public:
	virtual const char *GetName() override { return "RawConcurrent"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};

// Design #10 case 9: a regression detector for the scratch budget, not a
// performance gate. Peak-RSS delta around one real-fixture Decode is
// ASSERTED on macOS (estimate + 50% margin; catching a 2x regression, not
// policing megabytes) and REPORT-ONLY elsewhere (peak RSS is noisy across
// allocators). Skip-reported without a real fixture -- a 32x32 synthetic is
// meaningless against a budget model.
class CTestRawBudget : public CTest
{
public:
	virtual const char *GetName() override { return "RawBudget"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};

// Design #10 case 8: corrupt, truncated, non-RAW-with-RAW-extension and
// wrong-size Bayer inputs.
class CTestRawFailurePaths : public CTest
{
public:
	virtual const char *GetName() override { return "RawFailurePaths"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};
