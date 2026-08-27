#include "CDevelopChain.h"
#include "DevelopMath.h"

#include <chrono>

bool CDevelopChain::Execute(std::vector<float> &rgb, int &w, int &h,
                            SDevelopChainStats *outStats) const
{
	if (outStats != nullptr)
	{
		outStats->timings.clear();
		outStats->scratchHighWaterBytes = 0;
	}

	for (const SDevelopOp &op : ops_)
	{
		const auto t0 = std::chrono::steady_clock::now();
		const size_t count = (size_t)w * (size_t)h;
		float *px = rgb.data();

		switch (op.kind)
		{
			case EDevelopOpKind::Matrix:
			{
				for (size_t i = 0; i < count; i++)
					PC_Mat3Apply(op.m, px + i * 3, px + i * 3);
				break;
			}
			case EDevelopOpKind::DiagMul:
			{
				const float d0 = op.d[0], d1 = op.d[1], d2 = op.d[2];
				for (size_t i = 0; i < count; i++)
				{
					px[i * 3 + 0] *= d0;
					px[i * 3 + 1] *= d1;
					px[i * 3 + 2] *= d2;
				}
				break;
			}
			case EDevelopOpKind::Lut1DAll:
			{
				if (op.lut == nullptr) return false;
				for (size_t i = 0; i < count * 3; i++)
					px[i] = op.lut->Sample(px[i]);
				break;
			}
			case EDevelopOpKind::Lut1DPerChannel:
			{
				if (op.lutR == nullptr || op.lutG == nullptr || op.lutB == nullptr)
					return false;
				for (size_t i = 0; i < count; i++)
				{
					px[i * 3 + 0] = op.lutR->Sample(px[i * 3 + 0]);
					px[i * 3 + 1] = op.lutG->Sample(px[i * 3 + 1]);
					px[i * 3 + 2] = op.lutB->Sample(px[i * 3 + 2]);
				}
				break;
			}
			case EDevelopOpKind::Encode:
				PC_DevEncodeBuffer(px, count * 3);
				break;
			case EDevelopOpKind::Decode:
				PC_DevDecodeBuffer(px, count * 3);
				break;
			case EDevelopOpKind::RgbToneCurve:
			{
				if (op.lut == nullptr) return false;
				for (size_t i = 0; i < count; i++)
				{
					float lin[3];
					lin[0] = PC_DevDecode(px[i * 3 + 0]);
					lin[1] = PC_DevDecode(px[i * 3 + 1]);
					lin[2] = PC_DevDecode(px[i * 3 + 2]);
					PC_DevRgbTone(lin, *op.lut);
					px[i * 3 + 0] = PC_DevEncode(lin[0]);
					px[i * 3 + 1] = PC_DevEncode(lin[1]);
					px[i * 3 + 2] = PC_DevEncode(lin[2]);
				}
				break;
			}
			case EDevelopOpKind::HsvOp:
			{
				if (op.pixelFn == nullptr) return false;
				for (size_t i = 0; i < count; i++)
				{
					float hsv[3];
					PC_RgbToHsv(px + i * 3, hsv);
					op.pixelFn(op.pixelCtx, hsv);
					PC_HsvToRgb(hsv, px + i * 3);
				}
				break;
			}
			case EDevelopOpKind::SeamRgbOp:
			case EDevelopOpKind::Rolloff:
			{
				if (op.pixelFn == nullptr) return false;
				for (size_t i = 0; i < count; i++)
					op.pixelFn(op.pixelCtx, px + i * 3);
				break;
			}
			case EDevelopOpKind::SpatialOp:
			case EDevelopOpKind::CropResample:
			{
				if (!op.bufferFn) return false;
				const size_t before = rgb.capacity() * sizeof(float);
				op.bufferFn(rgb, w, h);
				const size_t after = rgb.capacity() * sizeof(float);
				if (outStats != nullptr && after > before)
					outStats->scratchHighWaterBytes =
						std::max(outStats->scratchHighWaterBytes, after - before);
				break;
			}
		}

		if (outStats != nullptr)
		{
			SDevelopOpTiming t;
			t.name = op.name;
			t.ms = (float)std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - t0).count();
			outStats->timings.push_back(t);
		}
	}
	return true;
}
