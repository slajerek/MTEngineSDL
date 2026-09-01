#include "MT_DcpFixtureWriter.h"

#include <cmath>
#include <cstring>

// Little-endian packers (mirrors RawTestFixtures' internal shape; kept
// local so the writer is self-contained for the .dcp container).
namespace {

enum
{
	WT_ASCII = 2, WT_SHORT = 3, WT_LONG = 4,
	WT_SRATIONAL = 10, WT_FLOAT = 11
};

void Put16(std::vector<unsigned char> &v, unsigned short x)
{
	v.push_back((unsigned char)(x & 0xFF));
	v.push_back((unsigned char)(x >> 8));
}

void Put32(std::vector<unsigned char> &v, unsigned x)
{
	v.push_back((unsigned char)(x & 0xFF));
	v.push_back((unsigned char)((x >> 8) & 0xFF));
	v.push_back((unsigned char)((x >> 16) & 0xFF));
	v.push_back((unsigned char)((x >> 24) & 0xFF));
}

void PutF32(std::vector<unsigned char> &v, float f)
{
	unsigned u;
	static_assert(sizeof(u) == sizeof(f), "f32");
	std::memcpy(&u, &f, 4);
	Put32(v, u);
}

SDngRawTag ShortTag(unsigned short tag, unsigned short value)
{
	SDngRawTag t{ tag, WT_SHORT, 1, {} };
	Put16(t.value, value);
	return t;
}

SDngRawTag SRationalMatrixTag(unsigned short tag, const float *vals, unsigned n)
{
	SDngRawTag t{ tag, WT_SRATIONAL, n, {} };
	for (unsigned i = 0; i < n; i++)
	{
		const int num = (int)std::lround((double)vals[i] * 10000.0);
		Put32(t.value, (unsigned)num);
		Put32(t.value, 10000u);
	}
	return t;
}

SDngRawTag SRationalScalarTag(unsigned short tag, float v)
{
	return SRationalMatrixTag(tag, &v, 1);
}

SDngRawTag LongsTag(unsigned short tag, const unsigned *vals, unsigned n)
{
	SDngRawTag t{ tag, WT_LONG, n, {} };
	for (unsigned i = 0; i < n; i++)
		Put32(t.value, vals[i]);
	return t;
}

SDngRawTag FloatsTag(unsigned short tag, const float *vals, size_t n)
{
	SDngRawTag t{ tag, WT_FLOAT, (unsigned)n, {} };
	for (size_t i = 0; i < n; i++)
		PutF32(t.value, vals[i]);
	return t;
}

} // namespace

SDngRawTag PC_DngAsciiTag(unsigned short tag, const std::string &value)
{
	SDngRawTag t{ tag, WT_ASCII, 0, {} };
	for (char c : value)
		t.value.push_back((unsigned char)c);
	t.value.push_back(0);
	t.count = (unsigned)t.value.size();
	return t;
}

SDngRawTag PC_DngLongTag(unsigned short tag, unsigned value)
{
	SDngRawTag t{ tag, WT_LONG, 1, {} };
	Put32(t.value, value);
	return t;
}

void PC_AppendDcpProfileTags(const SDcpWriterSpec &s,
                             std::vector<SDngRawTag> *out)
{
	if (!s.profileName.empty())
		out->push_back(PC_DngAsciiTag(0xC6F8, s.profileName));
	if (!s.uniqueCameraModel.empty())
		out->push_back(PC_DngAsciiTag(0xC614, s.uniqueCameraModel));
	if (s.hasColorMatrix1)
	{
		out->push_back(ShortTag(0xC65A, (unsigned short)s.illuminant1));
		out->push_back(SRationalMatrixTag(0xC621, s.colorMatrix1, 9));
	}
	if (s.hasColorMatrix2)
	{
		out->push_back(ShortTag(0xC65B, (unsigned short)s.illuminant2));
		out->push_back(SRationalMatrixTag(0xC622, s.colorMatrix2, 9));
	}
	if (s.hasForwardMatrix1)
		out->push_back(SRationalMatrixTag(0xC714, s.forwardMatrix1, 9));
	if (s.hasForwardMatrix2)
		out->push_back(SRationalMatrixTag(0xC715, s.forwardMatrix2, 9));

	if (s.hueDivisions > 0)
	{
		const unsigned dims[3] = { s.hueDivisions, s.satDivisions, s.valDivisions };
		out->push_back(LongsTag(0xC6F9, dims, 3));
	}
	if (!s.hueSatData1.empty())
		out->push_back(FloatsTag(0xC6FA, s.hueSatData1.data(), s.hueSatData1.size()));
	if (!s.hueSatData2.empty())
		out->push_back(FloatsTag(0xC6FB, s.hueSatData2.data(), s.hueSatData2.size()));
	if (s.lookHueDivisions > 0)
	{
		const unsigned dims[3] = { s.lookHueDivisions, s.lookSatDivisions, s.lookValDivisions };
		out->push_back(LongsTag(0xC725, dims, 3));
	}
	if (!s.lookData.empty())
		out->push_back(FloatsTag(0xC726, s.lookData.data(), s.lookData.size()));
	if (s.hueSatMapEncoding >= 0)
		out->push_back(PC_DngLongTag(0xC7A3, (unsigned)s.hueSatMapEncoding));
	if (s.lookTableEncoding >= 0)
		out->push_back(PC_DngLongTag(0xC7A4, (unsigned)s.lookTableEncoding));

	if (!s.toneCurve.empty())
	{
		std::vector<float> flat;
		flat.reserve(s.toneCurve.size() * 2);
		for (const auto &p : s.toneCurve)
		{
			flat.push_back(p.first);
			flat.push_back(p.second);
		}
		out->push_back(FloatsTag(0xC6FC, flat.data(), flat.size()));
	}

	if (!s.calibrationSignature.empty())
		out->push_back(PC_DngAsciiTag(0xC6F4, s.calibrationSignature));
	if (!s.copyright.empty())
		out->push_back(PC_DngAsciiTag(0xC6FE, s.copyright));
	if (s.embedPolicy >= 0)
		out->push_back(PC_DngLongTag(0xC6FD, (unsigned)s.embedPolicy));
	if (s.hasBaselineExposureOffset)
		out->push_back(SRationalScalarTag(0xC7A5, s.baselineExposureOffset));
	if (s.defaultBlackRender >= 0)
		out->push_back(PC_DngLongTag(0xC7A6, (unsigned)s.defaultBlackRender));

	if (s.extraRefusalTag != 0)
		out->push_back(PC_DngLongTag(s.extraRefusalTag, 0));
}

std::vector<unsigned char> PC_BuildDcpBytes(const SDcpWriterSpec &spec)
{
	std::vector<SDngRawTag> tags;
	PC_AppendDcpProfileTags(spec, &tags);

	// Layout: 8-byte header, one IFD, overflow values.
	const unsigned ifdOffset = 8;
	const unsigned ifdBytes = 2 + 12 * (unsigned)tags.size() + 4;
	unsigned pos = ifdOffset + ifdBytes;
	std::vector<unsigned> offs(tags.size(), 0);
	for (size_t i = 0; i < tags.size(); i++)
		if (tags[i].value.size() > 4)
		{
			if (pos & 1) pos++;
			offs[i] = pos;
			pos += (unsigned)tags[i].value.size();
		}

	std::vector<unsigned char> out;
	out.reserve(pos);
	out.push_back('I');
	out.push_back('I');
	Put16(out, spec.magic);
	Put32(out, ifdOffset);

	Put16(out, (unsigned short)tags.size());
	for (size_t i = 0; i < tags.size(); i++)
	{
		const SDngRawTag &t = tags[i];
		Put16(out, t.tag);
		Put16(out, t.type);
		Put32(out, t.count);
		if (t.value.size() <= 4)
		{
			std::vector<unsigned char> inl = t.value;
			while (inl.size() < 4)
				inl.push_back(0);
			out.insert(out.end(), inl.begin(), inl.end());
		}
		else
		{
			Put32(out, offs[i]);
		}
	}
	Put32(out, 0);   // next IFD

	for (size_t i = 0; i < tags.size(); i++)
		if (tags[i].value.size() > 4)
		{
			while (out.size() < offs[i])
				out.push_back(0);
			out.insert(out.end(), tags[i].value.begin(), tags[i].value.end());
		}

	return out;
}
