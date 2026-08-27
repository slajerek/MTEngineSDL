#include "CDcpTemperature.h"
#include "DevelopMath.h"

#include <cmath>

const float DCP_kD50XY[2]  = { 0.3457f, 0.3585f };   // dng_xy_coord.h:147
const float DCP_kPcsXYZ[3] = { 0.9642f, 1.0000f, 0.8249f };
const float DCP_kPcsXY[2]  = { 0.3457f, 0.3585f };   // PCStoXY() == D50

static void XyToXYZ(const float xy[2], float outXYZ[3])
{
	// dng_xy_coord's XYtoXYZ with its degenerate-y guard shape: Y = 1.
	float x = xy[0], y = xy[1];
	if (y <= 0.f)
		y = 1e-6f;
	outXYZ[0] = x / y;
	outXYZ[1] = 1.f;
	outXYZ[2] = (1.f - x - y) / y;
}

void DCP_MapWhiteMatrix(const float white1XY[2], const float white2XY[2],
                        float outM[3][3])
{
	// F6: the LINEARIZED Bradford matrix, verbatim from
	// dng_color_spec.cpp:27-63 including the clamps and pins.
	static const float Mb[3][3] =
	{
		{ 0.8951f,  0.2664f, -0.1614f },
		{ -0.7502f, 1.7135f,  0.0367f },
		{ 0.0389f, -0.0685f,  1.0296f },
	};
	float mbInv[3][3];
	PC_Mat3Invert(Mb, mbInv);

	float xyz1[3], xyz2[3], w1[3], w2[3];
	XyToXYZ(white1XY, xyz1);
	XyToXYZ(white2XY, xyz2);
	PC_Mat3Apply(Mb, xyz1, w1);
	PC_Mat3Apply(Mb, xyz2, w2);

	// Negative white coordinates are kind of meaningless (source comment).
	for (int i = 0; i < 3; i++)
	{
		if (w1[i] < 0.f) w1[i] = 0.f;
		if (w2[i] < 0.f) w2[i] = 0.f;
	}

	// Limit scaling to something reasonable: pin ratios to [0.1, 10].
	float A[3][3] = {};
	for (int i = 0; i < 3; i++)
	{
		float ratio = (w1[i] > 0.f) ? w2[i] / w1[i] : 10.f;
		if (ratio < 0.1f) ratio = 0.1f;
		if (ratio > 10.f) ratio = 10.f;
		A[i][i] = ratio;
	}

	float t[3][3];
	PC_Mat3Mul(A, Mb, t);
	PC_Mat3Mul(mbInv, t, outM);
}

void DCP_NeutralToXY(const float neutral[3],
                     const std::function<void(const float xy[2], float outM[3][3])> &xyzToCamera,
                     float outXY[2])
{
	// F4: dng_color_spec::NeutralToXY, followed exactly -- D50 start,
	// <= 30 passes, |dx|+|dy| < 1e-7, average-the-last-two fallback (the
	// known two-value oscillation; NOT a refusal, #4.2).
	const int kMaxPasses = 30;
	float last[2] = { DCP_kD50XY[0], DCP_kD50XY[1] };

	for (int pass = 0; pass < kMaxPasses; pass++)
	{
		float m[3][3], inv[3][3];
		xyzToCamera(last, m);
		if (!PC_Mat3Invert(m, inv))
			break;   // singular resolver matrix: keep the last estimate

		float xyz[3];
		PC_Mat3Apply(inv, neutral, xyz);
		const float sum = xyz[0] + xyz[1] + xyz[2];
		float next[2];
		if (sum <= 0.f)
			break;
		next[0] = xyz[0] / sum;
		next[1] = xyz[1] / sum;

		if (std::fabs(next[0] - last[0]) + std::fabs(next[1] - last[1])
		    < 0.0000001f)
		{
			outXY[0] = next[0];
			outXY[1] = next[1];
			return;
		}
		if (pass == kMaxPasses - 1)
		{
			next[0] = (last[0] + next[0]) * 0.5f;
			next[1] = (last[1] + next[1]) * 0.5f;
		}
		last[0] = next[0];
		last[1] = next[1];
	}

	outXY[0] = last[0];
	outXY[1] = last[1];
}
