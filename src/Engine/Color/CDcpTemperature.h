#pragma once

// RD-D #4.2: the neutral <-> white-point machinery -- dng_color_spec's
// NeutralToXY fixed point (F4) and MapWhiteMatrix (F6), both followed from
// source. xy <-> CCT itself is RD-C's Robertson pair in DevelopMath.

#include "SYS_Defs.h"

#include <functional>

// F6: dng_color_spec's MapWhiteMatrix -- LINEARIZED Bradford with negative
// white coordinates clamped to 0 and per-channel ratios pinned to
// [0.1, 10]. Maps XYZ relative to white1 into XYZ relative to white2.
void DCP_MapWhiteMatrix(const float white1XY[2], const float white2XY[2],
                        float outM[3][3]);

// F4: dng_color_spec::NeutralToXY. `neutral` is the INDIVIDUAL camera
// neutral (the source iterates it against the product-interpolated
// AB*CC*CM matrix -- rev 7's "reference neutral" phrasing is the
// split-algebra equivalent only at CC = I). `xyzToCamera` returns the
// product matrix resolved for a given white xy. Start D50, <= 30 passes,
// converge at |dx|+|dy| < 1e-7, average the last two on non-convergence
// (dng_sdk's known two-value oscillation; never a refusal -- #4.2).
void DCP_NeutralToXY(const float neutral[3],
                     const std::function<void(const float xy[2], float outM[3][3])> &xyzToCamera,
                     float outXY[2]);

// The D50 white the iteration starts from (dng_xy_coord's D50_xy_coord).
extern const float DCP_kD50XY[2];
// The ICC PCS white as XYZ and xy (PCStoXYZ / PCStoXY).
extern const float DCP_kPcsXYZ[3];
extern const float DCP_kPcsXY[2];
