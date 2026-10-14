#include "MT_UltraHdr.h"

#include "CImageData.h"
#include "MT_SrgbCurve.h"    // SrgbExtendedDecode / SrgbExtendedEncode: the gain-map twin and measure
#include "SYS_FileUtf8.h"   // SYS_FopenUtf8: a UTF-8 path on every platform
#include "DBG_Log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <thread>
#include <utility>
#include <vector>

#if MT_ENABLE_LIBULTRAHDR
#include "ultrahdr_api.h"
#endif

bool MT_UltraHdrIsAvailable()
{
#if MT_ENABLE_LIBULTRAHDR
	return true;
#else
	return false;
#endif
}

namespace {

// Option C, Decision R-2: a JPEG's ICC profile as RAW profile bytes, its APP2
// "ICC_PROFILE\0<seq><count>" chunks reassembled in sequence order. Pure byte
// parsing (no codec), so it exists in every build. Walks the markers from SOI
// to SOS or EOI, never past `size`. Returns 1 with the profile in *out, 0 when
// the JPEG carries no ICC chunk, -1 for a malformed stream or chunk set (no
// SOI, a segment length past the end, a sequence number of 0 or above the
// count, counts that disagree, a duplicate or a missing chunk).
// firstApp2 (optional): the FIRST APP2 marker in stream order whose payload
// starts with "ICC_PROFILE\0" and is longer than those 12 bytes, as its whole
// payload (chunk header included) -- exactly the block libultrahdr's
// JpegDecoderHelper keeps and classifies (jpegdecoderhelper.cpp:119-139, 238-239).
// {NULL, 0} when there is none.
int MTJpegReassembleIcc(const u8 *jpeg, size_t size, std::vector<u8> *out,
                        std::pair<const u8 *, size_t> *firstApp2 = NULL)
{
	if (firstApp2 != NULL) *firstApp2 = { (const u8 *)NULL, (size_t)0 };
	static const u8 kIccSig[12] = { 'I', 'C', 'C', '_', 'P', 'R', 'O', 'F', 'I', 'L', 'E', 0 };
	if (jpeg == NULL || size < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) return -1;
	std::vector<std::pair<const u8 *, size_t>> chunks;   // index = seq - 1
	int count = 0;
	size_t pos = 2;
	for (;;)
	{
		if (pos >= size) return -1;                       // ran out before SOS / EOI
		if (jpeg[pos] != 0xFF) return -1;
		while (pos < size && jpeg[pos] == 0xFF) pos++;    // fill bytes
		if (pos >= size) return -1;
		const u8 marker = jpeg[pos++];
		if (marker == 0xDA || marker == 0xD9) break;      // SOS / EOI: no more header segments
		if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;   // no length
		if (size - pos < 2) return -1;
		const size_t len = ((size_t)jpeg[pos] << 8) | jpeg[pos + 1];
		if (len < 2 || len > size - pos) return -1;
		const u8 *payload = jpeg + pos + 2;
		const size_t payloadSize = len - 2;
		if (firstApp2 != NULL && firstApp2->first == NULL && marker == 0xE2 && payloadSize > sizeof(kIccSig)
		    && memcmp(payload, kIccSig, sizeof(kIccSig)) == 0)
			*firstApp2 = { payload, payloadSize };
		if (marker == 0xE2 && payloadSize >= 14 && memcmp(payload, kIccSig, sizeof(kIccSig)) == 0)
		{
			const int seq = payload[12], cnt = payload[13];
			if (cnt == 0 || seq == 0 || seq > cnt || (count != 0 && cnt != count)) return -1;
			if (count == 0) { count = cnt; chunks.assign((size_t)cnt, { (const u8 *)NULL, (size_t)0 }); }
			if (chunks[(size_t)seq - 1].first != NULL) return -1;   // duplicate
			chunks[(size_t)seq - 1] = { payload + 14, payloadSize - 14 };
		}
		pos += len;
	}
	if (count == 0) return 0;
	size_t total = 0;
	for (const auto &c : chunks)
	{
		if (c.first == NULL) return -1;                   // missing
		total += c.second;
	}
	if (out != NULL)
	{
		out->clear();
		out->reserve(total);
		for (const auto &c : chunks) out->insert(out->end(), c.first, c.first + c.second);
	}
	return 1;
}

} // namespace

#if MT_ENABLE_LIBULTRAHDR

namespace {

// The whole file, read ONCE into a buffer of exactly its size: one seek for
// the size and one fread, not a 64 KB stack chunk appended into a growing
// vector (log2(n) reallocations and copies of a ~5 MB file per decode). A file
// that changes size between the two is read as far as it goes; a short read is
// a failure (the decode would refuse a truncated JPEG anyway). The vector's
// allocation may throw std::bad_alloc -- the caller's to catch; the FILE is
// held by a guard so that throw does not leak it.
//
// The size is a 64-bit offset on every platform: `long` is 32 bits on Windows
// (LLP64), so a plain ftell() fails or wraps past 2 GB there. A > 2 GB JPEG is
// unusual, but a wrong size would read a truncated prefix as the whole file.
int64_t MTFileTell64(FILE *f)
{
#if defined(_WIN32)
	return (int64_t)_ftelli64(f);
#else
	return (int64_t)ftello(f);
#endif
}

int MTFileSeek64(FILE *f, int64_t off, int whence)
{
#if defined(_WIN32)
	return _fseeki64(f, (__int64)off, whence);
#else
	return fseeko(f, (off_t)off, whence);
#endif
}

struct MTFileCloser
{
	FILE *f;
	~MTFileCloser() { if (f != NULL) fclose(f); }
};

bool MTReadWholeFile(const char *path, std::vector<u8> *out)
{
	MTFileCloser file{ SYS_FopenUtf8(path, "rb") };
	if (file.f == NULL) return false;
	int64_t size = -1;
	if (MTFileSeek64(file.f, 0, SEEK_END) == 0) size = MTFileTell64(file.f);
	if (size <= 0 || (uint64_t)size > (uint64_t)SIZE_MAX || MTFileSeek64(file.f, 0, SEEK_SET) != 0)
		return false;
	std::vector<u8> buf((size_t)size);   // may throw; the guard closes the file
	const size_t n = fread(buf.data(), 1, buf.size(), file.f);
	if (n != buf.size()) return false;
	out->swap(buf);
	return true;
}

// Row-major 3x3 product a * b.
void MTMat3Mul(const float a[3][3], const float b[3][3], float out[3][3])
{
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
}

bool MTMat3IsIdentity(const float m[3][3], float eps)
{
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			if (std::fabs(m[r][c] - (r == c ? 1.f : 0.f)) > eps) return false;
	return true;
}

// The decode's one copy pass over rows [y0, y1): libultrahdr's packed halves
// (stride in pixels) into the tight output, through the optional matrix / toe /
// LUT. Returns the peak LINEAR component (before any LUT): an integer max over
// the half bit patterns where no matrix runs -- a non-negative, non-NaN half
// orders exactly as its bits do, and negatives (sign bit) and NaN (above
// 0x7C00) are excluded, as the float compare they replace excluded them.
float MTUltraHdrCopyRows(const u16 *src, size_t stride, u16 *dst, size_t w, size_t y0, size_t y1,
                         const float (*m)[3], const u16 *lut, float toeKnee, float toeSlope)
{
	if (m == NULL)
	{
		u16 maxBits = 0;
		for (size_t y = y0; y < y1; y++)
		{
			const u16 *s = &src[y * stride * 4];
			u16 *d = &dst[y * w * 4];
			if (lut == NULL)
				memcpy(d, s, w * 8);
			for (size_t i = 0; i < w * 4; i += 4)
			{
				for (int c = 0; c < 3; c++)
				{
					const u16 b = s[i + c];
					if (b <= 0x7C00u && b > maxBits) maxBits = b;
					if (lut != NULL) d[i + c] = lut[b];
				}
				if (lut != NULL) d[i + 3] = s[i + 3];
			}
		}
		return HalfToFloat(maxBits);
	}
	const u16 one = FloatToHalf(1.0f);
	float peak = 0.0f;
	for (size_t y = y0; y < y1; y++)
	{
		const u16 *s = &src[y * stride * 4];
		u16 *d = &dst[y * w * 4];
		for (size_t i = 0; i < w * 4; i += 4)
		{
			const float rgb[3] = { HalfToFloat(s[i]), HalfToFloat(s[i + 1]), HalfToFloat(s[i + 2]) };
			for (int r = 0; r < 3; r++)
			{
				const float v = m[r][0] * rgb[0] + m[r][1] * rgb[1] + m[r][2] * rgb[2];
				if (v > peak) peak = v;
				if (lut == NULL)                                   d[i + r] = FloatToHalf(v);
				else if (toeKnee > 0.f && std::fabs(v) <= toeKnee) d[i + r] = FloatToHalf(v * toeSlope);
				else                                               d[i + r] = lut[FloatToHalf(v)];
			}
			d[i + 3] = one;
		}
	}
	return peak;
}

// Linear Display P3 / BT.2020 -> linear sRGB (Rec.709), row-major, D65 both
// sides (a pure primaries change). P3's is the inverse of DevelopMath's
// PC_kLinearSrgbToLinearP3; BT.2020's is ITU-R BT.2087's. Kept HERE rather
// than taken from Develop/, which is MT_CAP_DEVELOP's and may be compiled out.
const float kMTP3ToSrgb[3][3] = {
	{  1.2249400f, -0.2249401f,  0.0000000f },
	{ -0.0420568f,  1.0420569f,  0.0000000f },
	{ -0.0196377f, -0.0786360f,  1.0982736f },
};
const float kMTBt2020ToSrgb[3][3] = {
	{  1.6604910f, -0.5876411f, -0.0728499f },
	{ -0.1245505f,  1.1328999f, -0.0083494f },
	{ -0.0181508f, -0.1005789f,  1.1187297f },
};

struct SMTEncoderRelease { uhdr_codec_private_t *e; ~SMTEncoderRelease() { if (e) uhdr_release_encoder(e); } };
struct SMTDecoderRelease { uhdr_codec_private_t *d; ~SMTDecoderRelease() { if (d) uhdr_release_decoder(d); } };

uhdr_compressed_image_t MTCompressed(const u8 *data, size_t size)
{
	uhdr_compressed_image_t c;
	c.data = (void *)data;
	c.data_sz = size;
	c.capacity = size;
	c.cg = UHDR_CG_UNSPECIFIED;
	c.ct = UHDR_CT_UNSPECIFIED;
	c.range = UHDR_CR_UNSPECIFIED;
	return c;
}

} // namespace

bool MT_UltraHdrEncodeJpeg(const SMTUltraHdrEncodeRequest &req, std::vector<u8> *out,
                           std::string *err)
{
	auto fail = [err](const char *what, const uhdr_error_info_t *e) {
		if (err != NULL)
			*err = std::string(what) + ((e != NULL && e->has_detail) ? std::string(": ") + e->detail
			                                                          : std::string());
		return false;
	};
	if (out == NULL || req.baseJpeg == NULL || req.baseJpegSize == 0 || req.hdrRgbaHalf == NULL
	    || req.width <= 0 || req.height <= 0)
		return fail("bad request", NULL);

	uhdr_codec_private_t *enc = uhdr_create_encoder();
	if (enc == NULL) return fail("uhdr_create_encoder", NULL);
	SMTEncoderRelease release{ enc };

	uhdr_raw_image_t hdr;
	memset(&hdr, 0, sizeof(hdr));
	hdr.fmt   = UHDR_IMG_FMT_64bppRGBAHalfFloat;
	hdr.cg    = (req.hdrGamut == EMTUltraHdrGamut::Bt709) ? UHDR_CG_BT_709 : UHDR_CG_DISPLAY_P3;
	hdr.ct    = UHDR_CT_LINEAR;
	hdr.range = UHDR_CR_FULL_RANGE;
	hdr.w = (unsigned)req.width;
	hdr.h = (unsigned)req.height;
	hdr.planes[UHDR_PLANE_PACKED] = (void *)req.hdrRgbaHalf;
	hdr.stride[UHDR_PLANE_PACKED] = (unsigned)req.width;
	uhdr_error_info_t e = uhdr_enc_set_raw_image(enc, &hdr, UHDR_HDR_IMG);
	if (e.error_code != UHDR_CODEC_OK) return fail("hdr intent", &e);

	uhdr_compressed_image_t base = MTCompressed(req.baseJpeg, req.baseJpegSize);
	base.cg    = (req.baseGamut == EMTUltraHdrGamut::DisplayP3) ? UHDR_CG_DISPLAY_P3 : UHDR_CG_BT_709;
	base.ct    = UHDR_CT_SRGB;
	base.range = UHDR_CR_FULL_RANGE;
	e = uhdr_enc_set_compressed_image(enc, &base, UHDR_SDR_IMG);
	if (e.error_code != UHDR_CODEC_OK) return fail("base", &e);

	const float peak = std::clamp(req.hdrPeak, 2.0f, 10000.0f / 203.0f);
	const float maxBoost = (req.maxContentBoost > 0.0f) ? req.maxContentBoost
	                                                    : std::clamp(1.15f * req.hdrPeak, 2.0f, 128.0f);
	const float minBoost = std::min(req.minContentBoost, maxBoost);
	if ((e = uhdr_enc_set_quality(enc, req.gainMapQuality, UHDR_GAIN_MAP_IMG)).error_code != UHDR_CODEC_OK
	    || (e = uhdr_enc_set_gainmap_scale_factor(enc, req.gainMapScale)).error_code != UHDR_CODEC_OK
	    || (e = uhdr_enc_set_using_multi_channel_gainmap(enc, req.multiChannelGainMap ? 1 : 0)).error_code != UHDR_CODEC_OK
	    || (e = uhdr_enc_set_gainmap_gamma(enc, req.gainMapGamma)).error_code != UHDR_CODEC_OK
	    || (e = uhdr_enc_set_preset(enc, UHDR_USAGE_BEST_QUALITY)).error_code != UHDR_CODEC_OK
	    || (e = uhdr_enc_set_min_max_content_boost(enc, minBoost, maxBoost)).error_code != UHDR_CODEC_OK
	    || (e = uhdr_enc_set_target_display_peak_brightness(enc, 203.0f * peak)).error_code != UHDR_CODEC_OK
	    || (e = uhdr_enc_set_output_format(enc, UHDR_CODEC_JPG)).error_code != UHDR_CODEC_OK)
		return fail("settings", &e);

	e = uhdr_encode(enc);
	if (e.error_code != UHDR_CODEC_OK) return fail("uhdr_encode", &e);
	const uhdr_compressed_image_t *s = uhdr_get_encoded_stream(enc);
	if (s == NULL || s->data == NULL || s->data_sz == 0) return fail("empty stream", NULL);
	out->assign((const u8 *)s->data, (const u8 *)s->data + s->data_sz);
	return true;
}

bool MT_UltraHdrProbe(const u8 *data, size_t size, SMTUltraHdrInfo *out)
{
	if (data == NULL || size < 4) return false;
	// ONE probe. is_uhdr_image() is itself a whole create/set/probe/release
	// cycle, so calling it first parsed every file twice. A probe failure is
	// the plain-JPEG answer, never an error.
	uhdr_codec_private_t *dec = uhdr_create_decoder();
	if (dec == NULL) return false;
	SMTDecoderRelease release{ dec };
	uhdr_compressed_image_t in = MTCompressed(data, size);
	if (uhdr_dec_set_image(dec, &in).error_code != UHDR_CODEC_OK) return false;
	if (uhdr_dec_probe(dec).error_code != UHDR_CODEC_OK) return false;
	const uhdr_gainmap_metadata_t *m = uhdr_dec_get_gainmap_metadata(dec);
	if (m == NULL) return false;
	if (out != NULL)
	{
		out->width         = uhdr_dec_get_image_width(dec);
		out->height        = uhdr_dec_get_image_height(dec);
		out->gainMapWidth  = uhdr_dec_get_gainmap_width(dec);
		out->gainMapHeight = uhdr_dec_get_gainmap_height(dec);
		for (int c = 0; c < 3; c++)
		{
			out->maxContentBoost[c] = m->max_content_boost[c];
			out->minContentBoost[c] = m->min_content_boost[c];
			out->gamma[c]           = m->gamma[c];
			out->offsetSdr[c]       = m->offset_sdr[c];
			out->offsetHdr[c]       = m->offset_hdr[c];
		}
		out->hdrCapacityMin    = m->hdr_capacity_min;
		out->hdrCapacityMax    = m->hdr_capacity_max;
		out->useBaseColorSpace = (m->use_base_cg != 0);
	}
	return true;
}

bool CImageData::LoadUltraHdrLinear(const char *fileName, const SMTUltraHdrOutputTransform *output)
{
	if (fileName == NULL || fileName[0] == 0) return false;
	std::vector<u8> bytes;
	if (!MTReadWholeFile(fileName, &bytes)) return false;

	uhdr_codec_private_t *dec = uhdr_create_decoder();
	if (dec == NULL) return false;
	SMTDecoderRelease release{ dec };
	uhdr_compressed_image_t in = MTCompressed(bytes.data(), bytes.size());
	// The output format and transfer are set BEFORE the probe: a probe locks
	// the decoder's settings, and uhdr_decode would otherwise produce its
	// default (8-bit sRGB) whatever was asked for afterwards.
	uhdr_error_info_t e = uhdr_dec_set_image(dec, &in);
	if (e.error_code == UHDR_CODEC_OK) e = uhdr_dec_set_out_img_format(dec, UHDR_IMG_FMT_64bppRGBAHalfFloat);
	if (e.error_code == UHDR_CODEC_OK) e = uhdr_dec_set_out_color_transfer(dec, UHDR_CT_LINEAR);
	if (e.error_code != UHDR_CODEC_OK) return false;
	// A plain JPEG is the common case on this path's callers' side of the
	// branch: the probe's refusal is that answer -- silent, never an error
	// log. One probe, not is_uhdr_image()'s extra create/probe cycle first.
	if (uhdr_dec_probe(dec).error_code != UHDR_CODEC_OK) return false;
	// NO uhdr_dec_set_out_max_display_boost: unset means the WHOLE map (weight 1,
	// jpegr.cpp display_boost = min(FLT_MAX, capacity max)) -- the HDR arm's
	// contract; the display clips at its own headroom.
	e = uhdr_decode(dec);
	if (e.error_code != UHDR_CODEC_OK)
	{
		LOGError("CImageData::LoadUltraHdrLinear: '%s' failed: %s", fileName,
		         e.has_detail ? e.detail : "no detail");
		return false;
	}
	const uhdr_raw_image_t *o = uhdr_get_decoded_image(dec);
	if (o == NULL || o->fmt != UHDR_IMG_FMT_64bppRGBAHalfFloat || o->w == 0 || o->h == 0
	    || o->planes[UHDR_PLANE_PACKED] == NULL)
		return false;

	// The decoded image is in the BASE's gamut. UNSPECIFIED means the base
	// carried no recognisable ICC: an untagged JPEG is sRGB by the engine's
	// CM-F rule, so it gets no matrix -- the same reading the 8-bit path gives
	// that base.
	const float (*toSrgb)[3] = (o->cg == UHDR_CG_DISPLAY_P3) ? kMTP3ToSrgb
	                         : (o->cg == UHDR_CG_BT_2100)    ? kMTBt2020ToSrgb
	                                                         : NULL;   // BT.709 or unspecified
	// The one matrix of the copy pass: base gamut -> sRGB, then the output's
	// sRGB -> its primaries, composed; a composition that is the identity
	// (a P3 base on a P3 output) is no matrix at all.
	float composed[3][3];
	const float (*m)[3] = toSrgb;
	if (output != NULL && output->hasMatrix)
	{
		if (toSrgb != NULL) MTMat3Mul(output->matrixFromSrgb, toSrgb, composed);
		else                memcpy(composed, output->matrixFromSrgb, sizeof(composed));
		m = MTMat3IsIdentity(composed, 1.0e-5f) ? NULL : composed;
	}
	const u16 *lut = (output != NULL) ? output->lut : NULL;
	const float toeKnee  = (output != NULL) ? output->toeKnee  : 0.f;
	const float toeSlope = (output != NULL) ? output->toeSlope : 0.f;

	const size_t w = o->w, h = o->h, stride = o->stride[UHDR_PLANE_PACKED];
	// u16, not u8: every path that frees a float image's resultData does so as
	// unsigned short[] (DeallocImage, the host's orientation permute), and
	// new[]/delete[] must agree on the element type.
	// Held by a unique_ptr until it is handed to resultData: the band pass
	// below allocates (its peaks vector, the thread pool's reserve) and a
	// std::bad_alloc there -- the caller's to catch -- must not leak ~w*h*8
	// bytes (a 12 Mpx phone photo is ~96 MB).
	std::unique_ptr<u16[]> dstOwner(new (std::nothrow) u16[w * h * 4]);
	if (dstOwner == nullptr) return false;
	u16 *dst = dstOwner.get();
	const u16 *src = (const u16 *)o->planes[UHDR_PLANE_PACKED];
	// The copy pass, in row bands when the caller asked for threads (its
	// on-screen decode): libultrahdr's own workers have finished by now, so
	// these do not stack on them. Each band reports its own peak.
	const int threads = (output != NULL)
		? std::max(1, std::min({ output->copyThreads, (int)std::max(1u, std::thread::hardware_concurrency()), (int)h }))
		: 1;
	float peak = 0.0f;
	if (threads <= 1)
		peak = MTUltraHdrCopyRows(src, stride, dst, w, 0, h, m, lut, toeKnee, toeSlope);
	else
	{
		std::vector<float> peaks((size_t)threads, 0.0f);
		std::vector<std::thread> pool;
		pool.reserve((size_t)threads - 1);
		const size_t band = (h + (size_t)threads - 1) / (size_t)threads;
		try
		{
			for (int t = 1; t < threads; t++)
			{
				const size_t y0 = std::min(h, band * (size_t)t), y1 = std::min(h, y0 + band);
				pool.emplace_back([&, t, y0, y1] {
					peaks[(size_t)t] = MTUltraHdrCopyRows(src, stride, dst, w, y0, y1, m, lut, toeKnee, toeSlope);
				});
			}
		}
		catch (...) {}   // a thread that could not start: its band is done below
		const size_t started = pool.size();
		peaks[0] = MTUltraHdrCopyRows(src, stride, dst, w, 0, std::min(h, band), m, lut, toeKnee, toeSlope);
		for (size_t t = started + 1; t < (size_t)threads; t++)
		{
			const size_t y0 = std::min(h, band * t), y1 = std::min(h, y0 + band);
			peaks[t] = MTUltraHdrCopyRows(src, stride, dst, w, y0, y1, m, lut, toeKnee, toeSlope);
		}
		for (std::thread &th : pool) th.join();
		for (float p : peaks) peak = std::max(peak, p);
	}

	this->DeallocImage();
	this->width      = (int)w;
	this->height     = (int)h;
	this->type       = IMG_TYPE_RGBA_16F;
	this->resultData = (u8 *)dstOwner.release();
	// Without an output transform: LINEAR, sRGB primaries, 1.0 = SDR white --
	// the S-5 Phase 2 contract, the same as LoadWithImageIO_AppleHDR, and the
	// host encodes for its surface. With one, the caller's transform WAS that
	// encoding.
	this->floatIsSurfaceEncoded = (output != NULL);
	this->contentMaxComponent   = peak;
	LOGD("CImageData::LoadUltraHdrLinear: '%s' -> %zux%zu float, peak %.3f x SDR white",
	     fileName, w, h, peak);
	return true;
}

bool CImageData::LoadUltraHdrBaseAndMap(const char *fileName, CImageData *mapOut,
                                        SMTGainMapApply *applyOut,
                                        const SMTUltraHdrOutputTransform *output)
{
	// mapOut == this: storing the map would free the base just loaded.
	if (fileName == NULL || fileName[0] == 0 || mapOut == NULL || mapOut == this || applyOut == NULL)
		return false;
	std::vector<u8> bytes;
	if (!MTReadWholeFile(fileName, &bytes)) return false;
	uhdr_codec_private_t *dec = uhdr_create_decoder();
	if (dec == NULL) return false;
	SMTDecoderRelease release{ dec };
	uhdr_compressed_image_t in = MTCompressed(bytes.data(), bytes.size());
	// SDR output: libultrahdr decodes the base to RGBA8888 (sRGB-encoded, base
	// gamut) AND the map, and applies nothing (jpegr.cpp:1479-1491, 1519-1523).
	// Format and transfer are set before the probe, which locks the settings.
	uhdr_error_info_t e = uhdr_dec_set_image(dec, &in);
	if (e.error_code == UHDR_CODEC_OK) e = uhdr_dec_set_out_img_format(dec, UHDR_IMG_FMT_32bppRGBA8888);
	if (e.error_code == UHDR_CODEC_OK) e = uhdr_dec_set_out_color_transfer(dec, UHDR_CT_SRGB);
	if (e.error_code != UHDR_CODEC_OK) return false;
	if (uhdr_dec_probe(dec).error_code != UHDR_CODEC_OK) return false;   // a plain JPEG: silent
	const uhdr_gainmap_metadata_t *md = uhdr_dec_get_gainmap_metadata(dec);
	if (md == NULL || md->use_base_cg == 0) return false;
	const uhdr_mem_block_t *mapJpeg = uhdr_dec_get_gainmap_image(dec);
	if (mapJpeg == NULL || mapJpeg->data == NULL) return false;
	// A refusal here is a DECLINE, not an error: the caller falls back to the
	// float arm, which reports a genuinely broken file itself. The expected case is
	// a grayscale base, which libultrahdr's RGB output cannot decode
	// (jpegdecoderhelper.cpp:349-360) -- LOGError would reach stderr in every build.
	e = uhdr_decode(dec);
	if (e.error_code != UHDR_CODEC_OK)
	{
		LOGD("CImageData::LoadUltraHdrBaseAndMap: '%s' declined: uhdr_decode refused: %s", fileName,
		     e.has_detail ? e.detail : "no detail");
		return false;
	}
	const uhdr_raw_image_t *b = uhdr_get_decoded_image(dec);
	const uhdr_raw_image_t *g = uhdr_get_decoded_gainmap_image(dec);
	if (b == NULL || g == NULL || b->fmt != UHDR_IMG_FMT_32bppRGBA8888 || b->w == 0 || b->h == 0
	    || b->planes[UHDR_PLANE_PACKED] == NULL || b->cg == UHDR_CG_BT_2100 || g->w == 0 || g->h == 0)
		return false;
	// Decision R-2: with use_base_cg libultrahdr converts the applied result from
	// the base's gamut to the MAP's (jpegr.cpp:1616-1623) -- not reproduced, so a map
	// whose ICC names another gamut takes the float arm. Decided on the gamuts as the
	// codec reads them (b->cg is its own reading of the base's ICC), not on profile
	// bytes: libultrahdr writes its own ICC into every map it encodes, and it differs
	// in bytes from any base profile it did not write itself.
	const EMTIccGamut baseGamut = (b->cg == UHDR_CG_BT_709) ? EMTIccGamut::Bt709
	                            : (b->cg == UHDR_CG_DISPLAY_P3) ? EMTIccGamut::DisplayP3 : EMTIccGamut::Unspecified;
	if (!MT_GainMapIccGamutMatchesBase(baseGamut, (const u8 *)mapJpeg->data, mapJpeg->data_sz))
	{
		LOGD("CImageData::LoadUltraHdrBaseAndMap: '%s' declined: the map's ICC names another gamut than "
		     "the base's, or its ICC chunks are malformed", fileName);
		return false;
	}
	const bool mapRgba = (g->fmt == UHDR_IMG_FMT_32bppRGBA8888);
	if (!mapRgba && g->fmt != UHDR_IMG_FMT_8bppYCbCr400) return false;
	if (g->planes[mapRgba ? UHDR_PLANE_PACKED : UHDR_PLANE_Y] == NULL) return false;
	// libultrahdr RESIZES a map of another aspect (> 1 %) to the base before applying
	// (jpegr.cpp:1650-1664) -- not reproduced; a map larger than the base would make
	// mapScale < 1. Both take the float arm.
	const float baseAspect = (float)b->w / (float)b->h, mapAspect = (float)g->w / (float)g->h;
	if (std::fabs(baseAspect - mapAspect) / baseAspect > 0.01f || g->w > b->w || g->h > b->h) return false;

	const size_t bw = b->w, bh = b->h, gw = g->w, gh = g->h;
	std::unique_ptr<u8[]> baseOwner(new (std::nothrow) u8[bw * bh * 4]);
	std::unique_ptr<u8[]> mapOwner(new (std::nothrow) u8[gw * gh * 4]);
	if (baseOwner == nullptr || mapOwner == nullptr) return false;
	for (size_t y = 0; y < bh; y++)
		memcpy(baseOwner.get() + y * bw * 4,
		       (const u8 *)b->planes[UHDR_PLANE_PACKED] + y * (size_t)b->stride[UHDR_PLANE_PACKED] * 4, bw * 4);
	for (size_t y = 0; y < gh; y++)
	{
		u8 *d = mapOwner.get() + y * gw * 4;
		if (mapRgba)
			memcpy(d, (const u8 *)g->planes[UHDR_PLANE_PACKED] + y * (size_t)g->stride[UHDR_PLANE_PACKED] * 4, gw * 4);
		else
		{
			const u8 *src = (const u8 *)g->planes[UHDR_PLANE_Y] + y * (size_t)g->stride[UHDR_PLANE_Y];
			for (size_t x = 0; x < gw; x++) { d[4*x] = d[4*x+1] = d[4*x+2] = src[x]; d[4*x+3] = 255; }
		}
	}

	SMTGainMapApply a;
	a.mapChannels = mapRgba ? 3 : 1;
	for (int c = 0; c < 3; c++)
	{
		const int mc = mapRgba ? c : 0;   // a single-channel map: channel 0's parameters for all three
		a.logMinBoost[c] = std::log2(md->min_content_boost[mc]);
		a.logMaxBoost[c] = std::log2(md->max_content_boost[mc]);
		a.invGamma[c]    = (md->gamma[mc] > 0.f) ? 1.0f / md->gamma[mc] : 1.0f;
		a.offsetSdr[c]   = md->offset_sdr[mc];
		a.offsetHdr[c]   = md->offset_hdr[mc];
	}
	// The base's gamut -> sRGB, composed with the output's sRGB -> its primaries,
	// exactly as LoadUltraHdrLinear composes them (BT.2100 was declined above).
	const float (*toSrgb)[3] = (b->cg == UHDR_CG_DISPLAY_P3) ? kMTP3ToSrgb : NULL;
	float composed[3][3];
	const float (*m)[3] = toSrgb;
	if (output != NULL && output->hasMatrix)
	{
		if (toSrgb != NULL) MTMat3Mul(output->matrixFromSrgb, toSrgb, composed);
		else                memcpy(composed, output->matrixFromSrgb, sizeof(composed));
		m = MTMat3IsIdentity(composed, 1.0e-5f) ? NULL : composed;
	}
	a.hasMatrix = (m != NULL);
	if (m != NULL) memcpy(a.matrix, m, sizeof(a.matrix));
	a.encodeSrgb = (output != NULL && output->lut != NULL);
	a.mapScale = (float)bw / (float)gw;       // libultrahdr's one scale, BOTH axes (jpegr.cpp:1673-1675)
	a.mapOffset[0] = a.mapOffset[1] = 0.f;    // the source frame; MT_GainMapOrient moves it

	this->DeallocImage();
	this->width = (int)bw; this->height = (int)bh; this->type = IMG_TYPE_RGBA;
	this->resultData = baseOwner.release();
	this->floatIsSurfaceEncoded = false;      // RGBA8: the float-only flags say nothing
	this->contentMaxComponent   = 0.0f;
	mapOut->DeallocImage();
	mapOut->width = (int)gw; mapOut->height = (int)gh; mapOut->type = IMG_TYPE_RGBA;
	mapOut->resultData = mapOwner.release();
	mapOut->floatIsSurfaceEncoded = false;
	mapOut->contentMaxComponent   = 0.0f;
	*applyOut = a;
	LOGD("CImageData::LoadUltraHdrBaseAndMap: '%s' -> %zux%zu base + %zux%zu map (%d ch)",
	     fileName, bw, bh, gw, gh, a.mapChannels);
	return true;
}

#else   // !MT_ENABLE_LIBULTRAHDR -- the codec is not in this build

bool MT_UltraHdrEncodeJpeg(const SMTUltraHdrEncodeRequest &, std::vector<u8> *, std::string *err)
{
	if (err != NULL) *err = "libultrahdr is not in this build (MT_ENABLE_LIBULTRAHDR=0)";
	return false;
}

bool MT_UltraHdrProbe(const u8 *, size_t, SMTUltraHdrInfo *) { return false; }

bool CImageData::LoadUltraHdrLinear(const char *, const SMTUltraHdrOutputTransform *) { return false; }

bool CImageData::LoadUltraHdrBaseAndMap(const char *, CImageData *, SMTGainMapApply *,
                                        const SMTUltraHdrOutputTransform *) { return false; }

#endif


// ===========================================================================
// Option C (2026-10-07): the codec-free half -- byte parsing and the shader's
// maths on the CPU. Present in every build: a build without libultrahdr never
// produces a base + map, but a test can still build one by hand.
// ===========================================================================

bool MT_GainMapIccMatchesBase(const u8 *baseIcc, size_t baseIccSize,
                              const u8 *mapJpeg, size_t mapJpegSize)
{
	std::vector<u8> mapIcc;
	const int state = MTJpegReassembleIcc(mapJpeg, mapJpegSize, &mapIcc);
	if (state < 0) return false;    // malformed: never guess
	if (state == 0) return true;    // no ICC in the map: libultrahdr uses the base's gamut
	return baseIcc != NULL && baseIccSize == mapIcc.size()
	    && memcmp(baseIcc, mapIcc.data(), baseIccSize) == 0;
}

namespace {

u32 MTIccBe32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3]; }

} // namespace

EMTIccGamut MT_IccProfileGamut(const u8 *profile, size_t size)
{
	// libultrahdr's tables (include/ultrahdr/icc.h, kSRGB / kDisplayP3 / kRec2020,
	// D50 colorants, columns = R, G, B) and its tolerance (tagsEqualToMatrix).
	static const float kSrgb[3][3] = {
		{ 0x6FA2 / 65536.0f, 0x6299 / 65536.0f, 0x24A0 / 65536.0f },
		{ 0x38F5 / 65536.0f, 0xB785 / 65536.0f, 0x0F84 / 65536.0f },
		{ 0x0390 / 65536.0f, 0x18DA / 65536.0f, 0xB6CF / 65536.0f } };
	static const float kP3[3][3] = {
		{ 0.515102f, 0.291965f, 0.157153f },
		{ 0.241182f, 0.692236f, 0.0665819f },
		{ -0.00104941f, 0.0418818f, 0.784378f } };
	static const float kRec2020[3][3] = {
		{ 0.673459f, 0.165661f, 0.125100f },
		{ 0.279033f, 0.675338f, 0.0456288f },
		{ -0.00193139f, 0.0299794f, 0.797162f } };
	const size_t kHeader = 128;                       // + the 4-byte tag count
	if (profile == NULL || size < kHeader + 4) return EMTIccGamut::Unspecified;
	const size_t tagCount = MTIccBe32(profile + kHeader);
	if (tagCount > (size - kHeader - 4) / 12) return EMTIccGamut::Unspecified;
	size_t off[4] = { 0, 0, 0, 0 }, len[4] = { 0, 0, 0, 0 };   // rXYZ gXYZ bXYZ cicp; first of each wins
	static const char kSig[4][5] = { "rXYZ", "gXYZ", "bXYZ", "cicp" };
	for (size_t t = 0; t < tagCount; t++)
	{
		const u8 *e = profile + kHeader + 4 + t * 12;
		for (int k = 0; k < 4; k++)
			if (off[k] == 0 && memcmp(e, kSig[k], 4) == 0) { off[k] = MTIccBe32(e + 4); len[k] = MTIccBe32(e + 8); break; }
	}
	auto inside = [&](int k, size_t want) { return off[k] != 0 && len[k] == want && off[k] <= size && len[k] <= size - off[k]; };
	if (inside(3, 12))
	{
		const u8 primaries = profile[off[3] + 8];
		if (primaries == 1)  return EMTIccGamut::Bt709;
		if (primaries == 12) return EMTIccGamut::DisplayP3;
		if (primaries == 9)  return EMTIccGamut::Bt2100;
	}
	if (!inside(0, 20) || !inside(1, 20) || !inside(2, 20)) return EMTIccGamut::Unspecified;
	float xyz[3][3];   // [row X/Y/Z][column R/G/B], as the tables
	for (int c = 0; c < 3; c++)
		for (int r = 0; r < 3; r++)
			xyz[r][c] = (float)(int32_t)MTIccBe32(profile + off[c] + 8 + 4 * r) / 65536.0f;
	auto equal = [&](const float (*m)[3]) {
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++)
				if (std::fabs(xyz[r][c] - m[r][c]) > 0.001f) return false;
		return true;
	};
	if (equal(kSrgb))    return EMTIccGamut::Bt709;
	if (equal(kP3))      return EMTIccGamut::DisplayP3;
	if (equal(kRec2020)) return EMTIccGamut::Bt2100;
	return EMTIccGamut::Unspecified;
}

bool MT_GainMapIccGamutMatchesBase(EMTIccGamut baseGamut, const u8 *mapJpeg, size_t mapJpegSize)
{
	std::pair<const u8 *, size_t> first;
	const int state = MTJpegReassembleIcc(mapJpeg, mapJpegSize, NULL, &first);
	if (state < 0) return false;    // malformed: never guess (stricter than the codec, which reads on)
	if (first.first == NULL) return true;   // no ICC block in the map: hdr_cg = the base's
	// Classify exactly what libultrahdr classifies: the FIRST APP2 ICC marker in
	// stream order, after its 14-byte "ICC_PROFILE\0<seq><count>" header, alone --
	// not the reassembled profile (a tag past the first chunk is out of its reach).
	const EMTIccGamut mapGamut = (first.second >= 14) ? MT_IccProfileGamut(first.first + 14, first.second - 14)
	                                                   : EMTIccGamut::Unspecified;
	const EMTIccGamut sdrGamut = (baseGamut == EMTIccGamut::Unspecified) ? EMTIccGamut::Bt709 : baseGamut;
	return mapGamut == EMTIccGamut::Unspecified || mapGamut == sdrGamut;
}

// Option C: the draw-time shader's maths on the CPU -- the GPU parity test's
// oracle. Exact arithmetic in the shader's order. MAP POSITION RULE (shared with
// MT_GainMapUniformsFrom and MT_GainMapMeasure): libultrahdr uses ONE float scale
// s = baseW / mapW for BOTH axes (jpegr.cpp:1673-1675) and samples the map at
// (x / s, y / s) (gainmapmath.cpp:870-917) -- texel i at base pixel i * s, not a
// stretch and not a per-axis rounded scale (4031 / 2015 = 2.0005: rounding to 2 is off
// by half a texel at the far edge). In a turned frame the coordinate is
// (x - mapOffset) / mapScale (MT_GainMapOrient). Bilinear between the neighbours,
// clamp-to-edge. One output pixel per step x step block, at
// the block's top-left pixel centre. Writes surface-encoded halves; returns the
// LINEAR peak (Design step 5: after the matrix, before the encode).
float MT_GainMapEvaluate(const CImageData &base, const CImageData &map, const SMTGainMapApply &a,
                         int step, std::vector<u16> *outRgbaHalf, int *outW, int *outH)
{
	if (step < 1 || base.resultData == NULL || map.resultData == NULL
	    || base.width <= 0 || base.height <= 0 || map.width <= 0 || map.height <= 0)
		return 0.f;
	const int ow = (base.width + step - 1) / step, oh = (base.height + step - 1) / step;
	if (outRgbaHalf != NULL) outRgbaHalf->assign((size_t)ow * oh * 4, FloatToHalf(1.0f));
	if (outW) *outW = ow;
	if (outH) *outH = oh;
	const u8 *bp = (const u8 *)base.resultData, *mp = (const u8 *)map.resultData;
	const float hdrMax = 10000.0f / 203.0f;
	const float s = std::max(1.0f, a.mapScale);
	float peak = 0.f;
	for (int oy = 0; oy < oh; oy++)
		for (int ox = 0; ox < ow; ox++)
		{
			const int x = ox * step, y = oy * step;
			const float mx = ((float)x - a.mapOffset[0]) / s;   // the position rule above
			const float my = ((float)y - a.mapOffset[1]) / s;
			const float fmx = std::floor(mx), fmy = std::floor(my);
			const int x0 = std::clamp((int)fmx, 0, map.width - 1), x1 = std::clamp((int)fmx + 1, 0, map.width - 1);
			const int y0 = std::clamp((int)fmy, 0, map.height - 1), y1 = std::clamp((int)fmy + 1, 0, map.height - 1);
			const float wx = mx - fmx, wy = my - fmy;   // clamp-to-edge: the clamped indices repeat the edge texel
			float hdr[3];
			for (int c = 0; c < 3; c++)
			{
				auto M = [&](int xx, int yy) { return mp[((size_t)yy * map.width + xx) * 4 + c] / 255.0f; };
				float g = (M(x0, y0) * (1 - wx) + M(x1, y0) * wx) * (1 - wy)
				        + (M(x0, y1) * (1 - wx) + M(x1, y1) * wx) * wy;
				if (a.invGamma[c] != 1.0f) g = std::pow(g, a.invGamma[c]);
				const float logBoost = a.logMinBoost[c] * (1.0f - g) + a.logMaxBoost[c] * g;
				const float e = SrgbExtendedDecode(bp[((size_t)y * base.width + x) * 4 + c] / 255.0f);
				hdr[c] = std::clamp((e + a.offsetSdr[c]) * std::exp2(logBoost) - a.offsetHdr[c], 0.0f, hdrMax);
			}
			for (int r = 0; r < 3; r++)
			{
				const float o = a.hasMatrix ? a.matrix[r][0] * hdr[0] + a.matrix[r][1] * hdr[1] + a.matrix[r][2] * hdr[2]
				                            : hdr[r];
				peak = std::max(peak, o);
				if (outRgbaHalf != NULL)
					(*outRgbaHalf)[((size_t)oy * ow + ox) * 4 + r] = FloatToHalf(a.encodeSrgb ? SrgbExtendedEncode(o) : o);
			}
		}
	return peak;
}

// Option C: the histogram's and the peak's source, at the MAP's resolution.
// Texel i's BLOCK is the base pixels whose map coordinate (x - mapOffset) / mapScale
// floors to i (the position rule, MT_GainMapEvaluate); pixels before texel 0 belong to
// it, the remainder after the last texel to the last.
// HISTOGRAM: one sample per texel -- the block's first pixel, whose coordinate is
// within 1 / mapScale of the texel, with the texel's exact gain from a 256-entry table.
// ~3 Mpx of samples for a 12 Mpx photo with a half-size map.
// PEAK: an UPPER BOUND, never below the true per-pixel maximum (the twin's), because
// it drives the surface's EDR metadata (CurrentSurfacePeak -> SetSurfaceEdrMetadata)
// and an under-reported highlight is clipped. Every pixel of block i interpolates
// texels {i, i+1} x {j, j+1} (clamped), so its gain lies between those four texels'
// extreme table gains, and its base byte between the block's extreme bytes; the
// apply is monotonic in each, so per channel the four corner combinations bound it.
// The matrix has NEGATIVE coefficients (kMTP3ToSrgb, above): transforming
// per-channel maxima is neither exact nor conservative (P3 (1,0,0) in one
// pixel and (0,1,0) in another give sRGB R = 1.0 from the maxima, 1.22 from the first
// pixel). So each output row takes a channel's HIGH bound where its coefficient is
// >= 0 and its LOW bound where it is negative. Exact for a uniform block. The block
// scan touches every base pixel once (bytes only): ~5 ms for 12 Mpx.
// The extended sRGB curve as a half -> half table, built once on first use
// (65,536 evaluations, thread-safe static init), immutable after: entry h is
// FloatToHalf(SrgbExtendedEncode(HalfToFloat(h))). A host app's sRGB half
// table, copied as a file-static, used with
// the same linear toe (|v| <= 0.0031308 -> v * 12.92 directly) as its callers.
static const u16 *MTSrgbEncodeHalfLut()
{
	static const std::vector<u16> lut = [] {
		std::vector<u16> t(65536);
		for (u32 h = 0; h < 65536; h++)
			t[h] = FloatToHalf(SrgbExtendedEncode(HalfToFloat((u16)h)));
		return t;
	}();
	return lut.data();
}

float MT_GainMapMeasure(const CImageData &base, const CImageData &map, const SMTGainMapApply &a,
                        std::vector<u16> *outRgbaHalf, int *outW, int *outH, int threads)
{
	if (base.resultData == NULL || map.resultData == NULL || base.width <= 0 || map.width <= 0
	    || base.height <= 0 || map.height <= 0)
		return 0.f;
	static const std::vector<float> eotfTable = [] {
		std::vector<float> t(256);
		for (int i = 0; i < 256; i++) t[i] = SrgbExtendedDecode(i / 255.0f);
		return t;
	}();
	const float *eotf = eotfTable.data();
	float gain[3][256];
	for (int c = 0; c < 3; c++)
		for (int i = 0; i < 256; i++)
		{
			float g = i / 255.0f;
			if (a.invGamma[c] != 1.0f) g = std::pow(g, a.invGamma[c]);
			gain[c][i] = std::exp2(a.logMinBoost[c] * (1.0f - g) + a.logMaxBoost[c] * g);
		}
	const int ow = map.width, oh = map.height;
	if (outRgbaHalf != NULL) outRgbaHalf->resize((size_t)ow * oh * 4);   // every value written below
	if (outW) *outW = ow;
	if (outH) *outH = oh;
	u16 *out = (outRgbaHalf != NULL) ? outRgbaHalf->data() : NULL;
	// Loop invariants, hoisted (the output writes would otherwise force the
	// compiler to reload `a`'s fields per texel).
	const u16 *encLut = (out != NULL && a.encodeSrgb) ? MTSrgbEncodeHalfLut() : NULL;
	const float kToeKnee = 0.0031308f, kToeSlope = 12.92f;   // SrgbExtendedEncode's own toe
	const float offSdr[3] = { a.offsetSdr[0], a.offsetSdr[1], a.offsetSdr[2] };
	const float offHdr[3] = { a.offsetHdr[0], a.offsetHdr[1], a.offsetHdr[2] };
	const bool hasM = a.hasMatrix;
	float M[3][3];
	bool mPos[3][3];
	for (int r = 0; r < 3; r++)
		for (int k = 0; k < 3; k++) { M[r][k] = a.matrix[r][k]; mPos[r][k] = (a.matrix[r][k] >= 0.f); }
	const u16 kOne = FloatToHalf(1.0f);
	const int bw = base.width, bh = base.height, mw = map.width;
	const u8 *bp = (const u8 *)base.resultData, *mp = (const u8 *)map.resultData;
	const float hdrMax = 10000.0f / 203.0f;
	const float s = std::max(1.0f, a.mapScale);
	const float offX = a.mapOffset[0], offY = a.mapOffset[1];
	// [lo, hi) base pixels of texel i on one axis
	auto blockLo = [&](int i, float off, int B) { return (i == 0) ? 0 : std::clamp((int)std::ceil(off + i * s), 0, B); };
	auto blockHi = [&](int i, int m, float off, int B) { return (i == m - 1) ? B : std::clamp((int)std::ceil(off + (i + 1) * s), 0, B); };
	// The x blocks are the same for every row: computed once.
	std::vector<int> xLo((size_t)ow), xHi((size_t)ow);
	for (int mx = 0; mx < ow; mx++)
	{
		int xb0 = blockLo(mx, offX, bw), xb1 = blockHi(mx, ow, offX, bw);
		if (xb0 >= xb1) { xb0 = std::min(xb0, bw - 1); xb1 = xb0 + 1; }   // empty: as for rows
		xLo[(size_t)mx] = xb0;
		xHi[(size_t)mx] = xb1;
	}
	// Texel rows [yA, yB): each texel's output and bound are independent of every
	// other's, and the peak is a max, so any split of the rows gives the same bits.
	auto measureRows = [&](int yA, int yB) -> float {
	float peak = 0.f;
	for (int my = yA; my < yB; my++)
	{
		int yb0 = blockLo(my, offY, bh), yb1 = blockHi(my, oh, offY, bh);
		// An EMPTY block (the map overhangs the base: 4000x3000 over 1000x757 leaves
		// 7 texel rows past the last pixel) takes the last base pixel, rather than
		// leaving the prefilled 1.0 to spike SDR white in the histogram. It only
		// widens the peak bound's block.
		if (yb0 >= yb1) { yb0 = std::min(yb0, bh - 1); yb1 = yb0 + 1; }
		const int my1 = std::min(my + 1, oh - 1);
		const u8 *mrow0 = mp + (size_t)my * mw * 4, *mrow1 = mp + (size_t)my1 * mw * 4;
		for (int mx = 0; mx < ow; mx++)
		{
			const int xb0 = xLo[(size_t)mx], xb1 = xHi[(size_t)mx];
			const int mx1 = std::min(mx + 1, ow - 1);
			const u8 *mpx = mrow0 + (size_t)mx * 4;
			// the histogram sample: the block's first pixel with the texel's exact gain
			const u8 *bpx = bp + ((size_t)yb0 * bw + xb0) * 4;
			float hdr[3];
			for (int c = 0; c < 3; c++)
				hdr[c] = std::clamp((eotf[bpx[c]] + offSdr[c]) * gain[c][mpx[c]] - offHdr[c], 0.0f, hdrMax);
			// the peak bound: the block's byte extremes ...
			u8 lo[3] = { 255, 255, 255 }, hi[3] = { 0, 0, 0 };
			for (int yy = yb0; yy < yb1; yy++)
			{
				const u8 *row = bp + ((size_t)yy * bw + xb0) * 4;
				for (int xx = xb0; xx < xb1; xx++, row += 4)
					for (int c = 0; c < 3; c++) { lo[c] = std::min(lo[c], row[c]); hi[c] = std::max(hi[c], row[c]); }
			}
			// ... and the four interpolated texels' gain extremes
			const u8 *t1 = mrow0 + (size_t)mx1 * 4, *t2 = mrow1 + (size_t)mx * 4, *t3 = mrow1 + (size_t)mx1 * 4;
			float hLo[3], hHi[3];
			for (int c = 0; c < 3; c++)
			{
				const float *gc = gain[c];
				const float g0 = gc[mpx[c]], g1 = gc[t1[c]], g2 = gc[t2[c]], g3 = gc[t3[c]];
				const float gLo = std::min(std::min(g0, g1), std::min(g2, g3));
				const float gHi = std::max(std::max(g0, g1), std::max(g2, g3));
				// The apply (e + offSdr) * g - offHdr with g > 0 is non-decreasing in e
				// (and eotf is non-decreasing in the byte), so of the four corners the
				// minimum is one of the two at the LOW byte and the maximum one of the two
				// at the HIGH byte -- the same values the four-corner min / max picks,
				// with IEEE rounding monotone, so the result is bit-identical.
				const float eLo = eotf[lo[c]] + offSdr[c], eHi = eotf[hi[c]] + offSdr[c];
				hLo[c] = std::clamp(std::min(eLo * gLo - offHdr[c], eLo * gHi - offHdr[c]), 0.0f, hdrMax);
				hHi[c] = std::clamp(std::max(eHi * gLo - offHdr[c], eHi * gHi - offHdr[c]), 0.0f, hdrMax);
			}
			u16 *o4 = (out != NULL) ? out + ((size_t)my * ow + mx) * 4 : NULL;
			for (int r = 0; r < 3; r++)
			{
				float op = hHi[r];
				if (hasM)
				{
					op = 0.f;
					for (int k = 0; k < 3; k++) op += M[r][k] * (mPos[r][k] ? hHi[k] : hLo[k]);
				}
				peak = std::max(peak, op);
				if (o4 != NULL)
				{
					const float o = hasM ? M[r][0] * hdr[0] + M[r][1] * hdr[1] + M[r][2] * hdr[2] : hdr[r];
					if (encLut == NULL)                o4[r] = FloatToHalf(o);
					else if (std::fabs(o) <= kToeKnee) o4[r] = FloatToHalf(o * kToeSlope);
					else                               o4[r] = encLut[FloatToHalf(o)];
				}
			}
			if (o4 != NULL) o4[3] = kOne;
		}
	}
	return peak;
	};
	// In row bands when the caller asked for threads -- the copy pass's mechanism
	// and clamp (LoadUltraHdrLinear, copyThreads): std::threads for bands 1..n-1,
	// band 0 on the calling thread, a band whose thread could not start done here.
	const int nThreads = std::max(1, std::min({ threads, (int)std::max(1u, std::thread::hardware_concurrency()), oh }));
	float peak = 0.f;
	if (nThreads <= 1)
		peak = measureRows(0, oh);
	else
	{
		std::vector<float> peaks((size_t)nThreads, 0.0f);
		std::vector<std::thread> pool;
		pool.reserve((size_t)nThreads - 1);
		const int band = (oh + nThreads - 1) / nThreads;
		try
		{
			for (int t = 1; t < nThreads; t++)
			{
				const int y0 = std::min(oh, band * t), y1 = std::min(oh, y0 + band);
				pool.emplace_back([&, t, y0, y1] { peaks[(size_t)t] = measureRows(y0, y1); });
			}
		}
		catch (...) {}   // a thread that could not start: its band is done below
		const size_t started = pool.size();
		peaks[0] = measureRows(0, std::min(oh, band));
		for (size_t t = started + 1; t < (size_t)nThreads; t++)
		{
			const int y0 = std::min(oh, band * (int)t), y1 = std::min(oh, y0 + band);
			peaks[t] = measureRows(y0, y1);
		}
		for (std::thread &th : pool) th.join();
		for (float p : peaks) peak = std::max(peak, p);
	}
	// The bound above is exact in real arithmetic, but three float rounding
	// sources can put the twin a few ULPs above it: (a) block assignment here is
	// ceil(off + i*s) while the twin floors (x - off) / s, so a boundary pixel can
	// carry a ~1e-7 bilinear weight on texel i-1, outside the four texels bounded;
	// (b) the twin's bilinear blend of four equal bytes can round one step above
	// v/255 -- at byte 255 its gain exceeds exp2(logMax), the table's value; (c) the
	// twin's single-expression matrix row and this loop sum can differ under FMA
	// contraction. A relative margin of 1e-4 covers all three by orders of
	// magnitude and keeps the contract: never below MT_GainMapEvaluate's at step 1.
	return peak * (1.0f + 1.0e-4f);
}

// Option C: after PC_ApplyExifOrientation turned the base and the map, re-express the
// position rule in the turned frame. libultrahdr's rule has ONE scale for both axes, so
// a turn that swaps the axes keeps mapScale; an axis the turn MIRRORS moves the
// fractional remainder to the other edge: coordinate' = (x' - off) / s with
// off = (B - 1) - (m - 1) * s, B and m the turned base and map sizes along that axis
// (from x' = B-1-x and i' = m-1-i). Which edge owns the remainder cannot be recovered
// from the turned sizes, which is why this runs once, with the orientation, and the
// uniforms read mapScale/mapOffset rather than recomputing from dimensions.
void MT_GainMapOrient(SMTGainMapApply *a, int orientation, int turnedBaseW, int turnedBaseH,
                      int turnedMapW, int turnedMapH)
{
	// EXIF orientation -> is the TURNED x (y) axis mirrored relative to the source axis it
	// came from: 2 flip-x, 3 rot180, 4 flip-y, 5 transpose, 6 rot90 CW (x' = H-1-y),
	// 7 transverse, 8 rot90 CCW (y' = W-1-x). Matches the EXIF
	// orientation table row for row.
	static const bool kMirrorX[9] = { false, false, true,  true,  false, false, true,  true,  false };
	static const bool kMirrorY[9] = { false, false, false, true,  true,  false, false, true,  true  };
	if (a == NULL || orientation < 2 || orientation > 8) return;
	const float s = a->mapScale;
	a->mapOffset[0] = kMirrorX[orientation] ? (float)(turnedBaseW - 1) - (float)(turnedMapW - 1) * s : 0.f;
	a->mapOffset[1] = kMirrorY[orientation] ? (float)(turnedBaseH - 1) - (float)(turnedMapH - 1) * s : 0.f;
}
