#ifndef _MT_GainMapUniforms_h_
#define _MT_GainMapUniforms_h_

// Option C (gain-map resident HDR): the per-draw constant block of the gain-map
// shader -- 12 float4, the MSL (platform/MacOS/shaders/MTGainMap.metal) and HLSL
// constant block BYTE FOR BYTE. Plain floats, no SIMD types, so it is the same on
// every compiler; the static_assert pins the size both shaders declare.
//
// The fields are SMTGainMapApply (MT_UltraHdr.h) laid out for a shader: each
// per-channel array in .xyz, the matrix as three rows, the two texture rects and
// the POSITION RULE as an affine map from the base's rect fraction to the map's.
// Built by MT_GainMapUniformsFrom (VID_GainMapDraw.cpp) at every draw.

class CSlrImage;
struct SMTGainMapApply;

struct SMTGainMapUniforms
{
	float logMin[4], logMax[4], invGamma[4], offSdr[4], offHdr[4];
	float m0[4], m1[4], m2[4];       // matrix rows (identity when !hasMatrix)
	float baseRect[4];               // u0 v0 u1 v1 of the base inside its texture
	float mapRect[4];                // u0 v0 u1 v1 of the map inside its texture
	float flags[4];                  // x: encodeSrgb (0/1), y: mapChannels (1/3), z: hdrMax (10000/203),
	                                 // w: 0 nearest mag / 1 linear mag (image->linearScaling), 2 = the one-time composite pass (one apply per texel)
	float mapAffine[4];              // base-rect fraction t -> map-rect fraction: t * xy + zw (the position rule, MT_GainMapEvaluate)
};
static_assert(sizeof(SMTGainMapUniforms) == 192, "MSL/HLSL layout");

// The apply's arrays, matrix and flags, the two images' texture rects, and the
// position rule read from the APPLY (mapScale / mapOffset -- never recomputed from
// sizes): at base pixel x the rect fraction is t = (x + 0.5) / bw, the twin's map
// coordinate is (x - ox) / s, and the sampler needs ((x - ox) / s + 0.5) / mw, so
// mapAffine = { bw / (s mw), bh / (s mh), (0.5 - (0.5 + ox) / s) / mw, (0.5 - (0.5 + oy) / s) / mh }
// with bw/bh/mw/mh the images' CURRENT (turned) pixel sizes and s = max(1, mapScale).
void MT_GainMapUniformsFrom(const SMTGainMapApply &a, const CSlrImage *base, const CSlrImage *map,
                            SMTGainMapUniforms *out);

#endif
