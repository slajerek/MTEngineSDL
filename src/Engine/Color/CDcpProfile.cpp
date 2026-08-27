#include "CDcpProfile.h"
#include "MD5.h"
#include "SYS_FileUtf8.h"

#include <cmath>
#include <cstring>
#include <algorithm>

// ---------------------------------------------------------------------------
// Container constants (verified sources in spec #0/F11)
// ---------------------------------------------------------------------------

static const u16 kDcpMagic  = 0x4352;   // 'CR' -- dng_tag_values.h
static const u16 kTiffMagic = 42;       // DNG-embedded path

// TIFF entry types
enum
{
	kTypeByte = 1, kTypeAscii = 2, kTypeShort = 3, kTypeLong = 4,
	kTypeRational = 5, kTypeSByte = 6, kTypeUndefined = 7, kTypeSShort = 8,
	kTypeSLong = 9, kTypeSRational = 10, kTypeFloat = 11, kTypeDouble = 12,
};

static u32 TypeSize(u16 type)
{
	switch (type)
	{
		case kTypeByte: case kTypeAscii: case kTypeSByte: case kTypeUndefined:
			return 1;
		case kTypeShort: case kTypeSShort:
			return 2;
		case kTypeLong: case kTypeSLong: case kTypeFloat:
			return 4;
		case kTypeRational: case kTypeSRational: case kTypeDouble:
			return 8;
		default:
			return 0;
	}
}

// Tags (numbers from spec #3.2; DNG 1.6/1.7 refusal set verified against
// the exiftool registry 2026-08-13 -- the plan's from-memory numbers were
// WRONG, which is why the verification step exists).
enum
{
	kTagUniqueCameraModel      = 0xC614,
	kTagLocalizedCameraModel   = 0xC615,
	kTagColorMatrix1           = 0xC621,
	kTagColorMatrix2           = 0xC622,
	kTagCalibrationIlluminant1 = 0xC65A,
	kTagCalibrationIlluminant2 = 0xC65B,
	kTagForwardMatrix1         = 0xC714,
	kTagForwardMatrix2         = 0xC715,
	kTagCameraCalibrationSig   = 0xC6F3,   // FILE tag (F12)
	kTagProfileCalibrationSig  = 0xC6F4,
	kTagExtraCameraProfiles    = 0xC6F5,   // FILE tag
	kTagAsShotProfileName      = 0xC6F6,   // FILE tag
	kTagProfileName            = 0xC6F8,
	kTagHueSatMapDims          = 0xC6F9,
	kTagHueSatMapData1         = 0xC6FA,
	kTagHueSatMapData2         = 0xC6FB,   // NOT 0xC6FE (#3.2's rev-1 trap)
	kTagToneCurve              = 0xC6FC,
	kTagEmbedPolicy            = 0xC6FD,
	kTagCopyright              = 0xC6FE,
	kTagLookTableDims          = 0xC725,
	kTagLookTableData          = 0xC726,
	kTagHueSatMapEncoding      = 0xC7A3,
	kTagLookTableEncoding      = 0xC7A4,
	kTagBaselineExposureOffset = 0xC7A5,
	kTagDefaultBlackRender     = 0xC7A6,
	kTagReductionMatrix1       = 0xC625,
	kTagReductionMatrix2       = 0xC626,
};

// DNG 1.6/1.7 profile tags we cannot honour: presence of ANY of these
// refuses the profile (#3.2 -- a parser that half-applies renders wrong
// with no signal). Verified numbers (exiftool Exif.pm, 2026-08-13).
static const u16 kDng16RefusalTags[] =
{
	0xCD2D,   // ProfileGainTableMap
	0xCD31,   // CalibrationIlluminant3
	0xCD32,   // CameraCalibration3
	0xCD33,   // ColorMatrix3
	0xCD34,   // ForwardMatrix3
	0xCD35,   // IlluminantData1
	0xCD36,   // IlluminantData2
	0xCD37,   // IlluminantData3
	0xCD3A,   // ReductionMatrix3
	0xCD3F,   // RGBTables
	0xCD40,   // ProfileGainTableMap2 (1.7)
};

// Stated maxima (untrusted input, #3.1): dims per axis, and total table
// bytes. Adobe Standard is ~90x30x1; 256 is generous headroom.
static const u32 kMaxDivisions   = 256;
static const u32 kMaxTableBytes  = 16u * 1024u * 1024u;
static const u32 kMaxIfdEntries  = 4096;
static const u32 kMaxStringBytes = 4096;
static const u32 kMaxCurvePoints = 65536;
static const u32 kMaxExtraProfiles = 64;

// ---------------------------------------------------------------------------
// Bounds-checked reader
// ---------------------------------------------------------------------------

struct SDcpReader
{
	const u8 *data = nullptr;
	size_t len = 0;
	bool bigEndian = false;

	bool InRange(size_t off, size_t count) const
	{
		return off <= len && count <= len - off;
	}
	u16 U16(size_t off) const
	{
		return bigEndian ? (u16)((data[off] << 8) | data[off + 1])
		                 : (u16)(data[off] | (data[off + 1] << 8));
	}
	u32 U32(size_t off) const
	{
		return bigEndian
			? ((u32)data[off] << 24) | ((u32)data[off + 1] << 16)
			  | ((u32)data[off + 2] << 8) | (u32)data[off + 3]
			: (u32)data[off] | ((u32)data[off + 1] << 8)
			  | ((u32)data[off + 2] << 16) | ((u32)data[off + 3] << 24);
	}
	float F32(size_t off) const
	{
		u32 v = U32(off);
		float f;
		std::memcpy(&f, &v, 4);
		return f;
	}
};

struct SDcpEntry
{
	u16 tag = 0;
	u16 type = 0;
	u32 count = 0;
	size_t valueOffset = 0;   // absolute offset of the value bytes
	bool valid = false;       // offsets checked
};

// Reads one IFD's entries. Returns false on structural damage.
static bool ReadIfdEntries(const SDcpReader &r, u32 ifdOffset,
                           std::vector<SDcpEntry> *out, std::string *outError)
{
	if (!r.InRange(ifdOffset, 2))
	{
		*outError = "dcp: IFD offset past EOF";
		return false;
	}
	const u32 count = r.U16(ifdOffset);
	if (count > kMaxIfdEntries)
	{
		*outError = "dcp: absurd IFD entry count";
		return false;
	}
	if (!r.InRange(ifdOffset + 2, (size_t)count * 12 + 4))
	{
		*outError = "dcp: IFD truncated";
		return false;
	}
	out->reserve(count);
	for (u32 i = 0; i < count; i++)
	{
		const size_t e = ifdOffset + 2 + (size_t)i * 12;
		SDcpEntry entry;
		entry.tag = r.U16(e);
		entry.type = r.U16(e + 2);
		entry.count = r.U32(e + 4);
		const u32 ts = TypeSize(entry.type);
		if (ts == 0)
			continue;   // unknown type: skip the entry, not the file
		const u64 total = (u64)ts * entry.count;
		if (total > kMaxTableBytes)
		{
			*outError = "dcp: entry larger than the stated maximum";
			return false;
		}
		if (total <= 4)
		{
			entry.valueOffset = e + 8;
			entry.valid = true;
		}
		else
		{
			const u32 off = r.U32(e + 8);
			if (!r.InRange(off, (size_t)total))
			{
				*outError = "dcp: entry value offset past EOF";
				return false;
			}
			entry.valueOffset = off;
			entry.valid = true;
		}
		out->push_back(entry);
	}
	return true;
}

static const SDcpEntry *FindEntry(const std::vector<SDcpEntry> &entries, u16 tag)
{
	for (const SDcpEntry &e : entries)
		if (e.tag == tag && e.valid)
			return &e;
	return nullptr;
}

static bool ReadAsciiValue(const SDcpReader &r, const SDcpEntry &e,
                           std::string *out)
{
	if (e.type != kTypeAscii || e.count > kMaxStringBytes)
		return false;
	const char *p = (const char *)r.data + e.valueOffset;
	size_t n = e.count;
	while (n > 0 && p[n - 1] == '\0')
		n--;
	out->assign(p, n);
	return true;
}

static double ReadRationalAt(const SDcpReader &r, size_t off, bool isSigned)
{
	const u32 nu = r.U32(off);
	const u32 de = r.U32(off + 4);
	const double num = isSigned ? (double)(int32_t)nu : (double)nu;
	const double den = isSigned ? (double)(int32_t)de : (double)de;
	if (den == 0.0)
		return 0.0;
	return num / den;
}

static bool ReadMatrix3x3(const SDcpReader &r, const SDcpEntry &e,
                          float out[3][3])
{
	if (e.count != 9)
		return false;
	if (e.type != kTypeSRational && e.type != kTypeRational)
		return false;
	for (int i = 0; i < 9; i++)
		out[i / 3][i % 3] = (float)ReadRationalAt(
			r, e.valueOffset + (size_t)i * 8, e.type == kTypeSRational);
	return true;
}

// ---------------------------------------------------------------------------
// Load-time normalisations (spec F1/F2 -- from dng_camera_profile.cpp)
// ---------------------------------------------------------------------------

// ICC PCS D50 white, the same constants the ICC builders pin.
static const double kPcsXYZ[3] = { 0.9642, 1.0000, 0.8249 };

// F2: NormalizeColorMatrix -- scale so MaxEntry(CM * PCSwhiteXYZ) == 1
// whenever it falls outside [0.99, 1.01].
static void NormalizeColorMatrix(float m[3][3])
{
	double coord[3];
	for (int r = 0; r < 3; r++)
		coord[r] = m[r][0] * kPcsXYZ[0] + m[r][1] * kPcsXYZ[1]
		         + m[r][2] * kPcsXYZ[2];
	double maxCoord = std::max(coord[0], std::max(coord[1], coord[2]));
	if (maxCoord > 0.0 && (maxCoord < 0.99 || maxCoord > 1.01))
	{
		const double s = 1.0 / maxCoord;
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++)
				m[r][c] = (float)(m[r][c] * s);
	}
}

// F1: NormalizeForwardMatrix -- diag(PCS) * diag(1/(FM*1)) * FM. A DIAGONAL
// white rescale, NOT Bradford (rev 7's claim, corrected from source).
static void NormalizeForwardMatrix(float m[3][3])
{
	double xyz[3];
	for (int r = 0; r < 3; r++)
		xyz[r] = m[r][0] + m[r][1] + m[r][2];
	if (xyz[0] == 0.0 || xyz[1] == 0.0 || xyz[2] == 0.0)
		return;
	for (int r = 0; r < 3; r++)
	{
		const double s = kPcsXYZ[r] / xyz[r];
		for (int c = 0; c < 3; c++)
			m[r][c] = (float)(m[r][c] * s);
	}
}

// ---------------------------------------------------------------------------
// The illuminant table (spec F5, dng_camera_profile.cpp:81)
// ---------------------------------------------------------------------------

double DCP_IlluminantToTemperature(int light)
{
	switch (light)
	{
		case 17: case 3:  return 2850.0;               // StdA, Tungsten
		case 24:          return 3200.0;               // ISO studio tungsten
		case 23:          return 5000.0;               // D50
		case 20: case 1: case 9: case 4: case 18:
		                  return 5500.0;               // D55, Daylight, Fine, Flash, StdB
		case 21: case 19: case 10:
		                  return 6500.0;               // D65, StdC, Cloudy
		case 22: case 11: return 7500.0;               // D75, Shade
		case 12:          return (5700.0 + 7100.0) * 0.5;   // daylight fluor
		case 13:          return (4600.0 + 5500.0) * 0.5;   // day white fluor
		case 14: case 2:  return (3800.0 + 4500.0) * 0.5;   // cool white fluor
		case 15:          return (3250.0 + 3800.0) * 0.5;   // white fluor
		case 16:          return (2600.0 + 3250.0) * 0.5;   // warm white fluor
		default:          return 0.0;   // unknown (incl. 255 "Other") -> refuse
	}
}

// ---------------------------------------------------------------------------
// The profile parse itself
// ---------------------------------------------------------------------------

static bool ReadHueSatTable(const SDcpReader &r,
                            const std::vector<SDcpEntry> &entries,
                            u16 dimsTag, u16 dataTag, SDcpHueSatMap *out,
                            bool *outPresent, std::string *outError,
                            const char *what)
{
	*outPresent = false;
	const SDcpEntry *data = FindEntry(entries, dataTag);
	if (data == nullptr)
		return true;   // absent is legal
	const SDcpEntry *dims = FindEntry(entries, dimsTag);
	if (dims == nullptr || dims->type != kTypeLong || dims->count != 3)
	{
		*outError = std::string("dcp: ") + what + " data without valid dims";
		return false;
	}
	const u32 hue = r.U32(dims->valueOffset);
	const u32 sat = r.U32(dims->valueOffset + 4);
	const u32 val = r.U32(dims->valueOffset + 8);
	// #3.3's bounds: hue >= 1, sat >= 2, val >= 1, each capped.
	if (hue < 1 || hue > kMaxDivisions || sat < 2 || sat > kMaxDivisions
	    || val < 1 || val > kMaxDivisions)
	{
		*outError = std::string("dcp: ") + what + " dims out of bounds";
		return false;
	}
	const u64 expect = (u64)hue * sat * val * 3;
	if (data->type != kTypeFloat || data->count != expect
	    || expect * 4 > kMaxTableBytes)
	{
		*outError = std::string("dcp: ") + what + " count != hue*sat*val*3";
		return false;
	}
	out->hueDivisions = hue;
	out->satDivisions = sat;
	out->valDivisions = val;
	out->deltas.resize((size_t)expect);
	for (size_t i = 0; i < (size_t)expect; i++)
		out->deltas[i] = r.F32(data->valueOffset + i * 4);
	*outPresent = true;
	return true;
}

static bool ParseProfileIfd(const SDcpReader &r, u32 ifdOffset,
                            SDcpProfile *out, std::string *outError)
{
	std::vector<SDcpEntry> entries;
	if (!ReadIfdEntries(r, ifdOffset, &entries, outError))
		return false;

	// DNG 1.6+ refusal first (#3.2): half-applying is worse than refusing.
	for (const SDcpEntry &e : entries)
		for (u16 t : kDng16RefusalTags)
			if (e.tag == t)
			{
				*outError = "dcp: DNG 1.6+ profile tag present -- refused, "
				            "not half-applied";
				return false;
			}

	const SDcpEntry *e;

	if ((e = FindEntry(entries, kTagColorMatrix1)) != nullptr)
	{
		if (!ReadMatrix3x3(r, *e, out->colorMatrix1))
		{
			// dng_sdk infers ColorPlanes from the element count: 9 = 3
			// planes, anything else is a >3-plane or monochrome profile.
			*outError = "dcp: ColorMatrix1 is not 3x3 (ColorPlanes != 3)";
			return false;
		}
		NormalizeColorMatrix(out->colorMatrix1);
		out->hasColorMatrix1 = true;
	}
	if ((e = FindEntry(entries, kTagColorMatrix2)) != nullptr)
	{
		if (!ReadMatrix3x3(r, *e, out->colorMatrix2))
		{
			*outError = "dcp: ColorMatrix2 is not 3x3";
			return false;
		}
		NormalizeColorMatrix(out->colorMatrix2);
		out->hasColorMatrix2 = true;
	}
	if ((e = FindEntry(entries, kTagForwardMatrix1)) != nullptr)
	{
		if (!ReadMatrix3x3(r, *e, out->forwardMatrix1))
		{
			*outError = "dcp: ForwardMatrix1 is not 3x3";
			return false;
		}
		NormalizeForwardMatrix(out->forwardMatrix1);
		out->hasForwardMatrix1 = true;
	}
	if ((e = FindEntry(entries, kTagForwardMatrix2)) != nullptr)
	{
		if (!ReadMatrix3x3(r, *e, out->forwardMatrix2))
		{
			*outError = "dcp: ForwardMatrix2 is not 3x3";
			return false;
		}
		NormalizeForwardMatrix(out->forwardMatrix2);
		out->hasForwardMatrix2 = true;
	}

	// ReductionMatrix presence means >3 planes -> refuse (#3.3).
	if (FindEntry(entries, kTagReductionMatrix1) != nullptr
	    || FindEntry(entries, kTagReductionMatrix2) != nullptr)
	{
		*outError = "dcp: ReductionMatrix present (>3-plane camera) -- refused";
		return false;
	}

	if ((e = FindEntry(entries, kTagCalibrationIlluminant1)) != nullptr
	    && e->type == kTypeShort && e->count == 1)
		out->calibrationIlluminant1 = r.U16(e->valueOffset);
	if ((e = FindEntry(entries, kTagCalibrationIlluminant2)) != nullptr
	    && e->type == kTypeShort && e->count == 1)
		out->calibrationIlluminant2 = r.U16(e->valueOffset);

	bool present = false;
	if (!ReadHueSatTable(r, entries, kTagHueSatMapDims, kTagHueSatMapData1,
	                     &out->hueSatMap1, &present, outError, "HueSatMap1"))
		return false;
	if (!ReadHueSatTable(r, entries, kTagHueSatMapDims, kTagHueSatMapData2,
	                     &out->hueSatMap2, &present, outError, "HueSatMap2"))
		return false;
	if (!ReadHueSatTable(r, entries, kTagLookTableDims, kTagLookTableData,
	                     &out->lookTable, &present, outError, "LookTable"))
		return false;

	// #3.3's valid combinations: Data2 alone is the ONLY invalid one.
	if (out->hueSatMap2.IsValid() && !out->hueSatMap1.IsValid())
	{
		*outError = "dcp: HueSatMapData2 without Data1";
		return false;
	}

	if ((e = FindEntry(entries, kTagHueSatMapEncoding)) != nullptr
	    && e->type == kTypeLong && e->count == 1)
		out->hueSatMapEncoding = (int)r.U32(e->valueOffset);
	if ((e = FindEntry(entries, kTagLookTableEncoding)) != nullptr
	    && e->type == kTypeLong && e->count == 1)
		out->lookTableEncoding = (int)r.U32(e->valueOffset);

	if ((e = FindEntry(entries, kTagToneCurve)) != nullptr)
	{
		if (e->type != kTypeFloat || e->count < 4 || (e->count & 1) != 0
		    || e->count / 2 > kMaxCurvePoints)
		{
			*outError = "dcp: ToneCurve malformed";
			return false;
		}
		const u32 points = e->count / 2;
		out->toneCurve.reserve(points);
		for (u32 i = 0; i < points; i++)
		{
			const float x = r.F32(e->valueOffset + (size_t)i * 8);
			const float y = r.F32(e->valueOffset + (size_t)i * 8 + 4);
			out->toneCurve.push_back({ x, y });
		}
		// #3.2's validation: >= 2 points, strictly increasing x, x0 = 0,
		// xN = 1.
		bool ok = out->toneCurve.front().first == 0.f
		       && out->toneCurve.back().first == 1.f;
		for (size_t i = 1; ok && i < out->toneCurve.size(); i++)
			if (out->toneCurve[i].first <= out->toneCurve[i - 1].first)
				ok = false;
		if (!ok)
		{
			*outError = "dcp: ToneCurve x not strictly increasing over [0,1]";
			return false;
		}
	}

	if ((e = FindEntry(entries, kTagProfileName)) != nullptr)
		ReadAsciiValue(r, *e, &out->profileName);
	if ((e = FindEntry(entries, kTagUniqueCameraModel)) != nullptr)
		ReadAsciiValue(r, *e, &out->uniqueCameraModel);
	if ((e = FindEntry(entries, kTagLocalizedCameraModel)) != nullptr)
		ReadAsciiValue(r, *e, &out->localizedCameraModel);
	if ((e = FindEntry(entries, kTagProfileCalibrationSig)) != nullptr)
		ReadAsciiValue(r, *e, &out->calibrationSignature);
	if ((e = FindEntry(entries, kTagCopyright)) != nullptr)
		ReadAsciiValue(r, *e, &out->copyright);

	if ((e = FindEntry(entries, kTagEmbedPolicy)) != nullptr
	    && e->type == kTypeLong && e->count == 1)
		out->embedPolicy = r.U32(e->valueOffset);

	if ((e = FindEntry(entries, kTagBaselineExposureOffset)) != nullptr
	    && (e->type == kTypeSRational || e->type == kTypeRational)
	    && e->count == 1)
	{
		out->baselineExposureOffset = (float)ReadRationalAt(
			r, e->valueOffset, e->type == kTypeSRational);
		out->hasBaselineExposureOffset = true;
	}
	if ((e = FindEntry(entries, kTagDefaultBlackRender)) != nullptr
	    && e->type == kTypeLong && e->count == 1)
		out->defaultBlackRender = (int)r.U32(e->valueOffset);

	// #4.1: ColorMatrix1 is REQUIRED regardless of ForwardMatrix -- the
	// temperature/neutral resolution runs through CM.
	if (!out->hasColorMatrix1)
	{
		*outError = "dcp: no ColorMatrix1 -- not renderable";
		return false;
	}
	// Dual-illuminant sanity: CM2 present requires a usable illuminant pair.
	if (out->hasColorMatrix2)
	{
		if (DCP_IlluminantToTemperature(out->calibrationIlluminant1) == 0.0
		    || DCP_IlluminantToTemperature(out->calibrationIlluminant2) == 0.0)
		{
			*outError = "dcp: unknown CalibrationIlluminant -- refused";
			return false;
		}
	}
	else if (out->calibrationIlluminant1 != 0
	         && DCP_IlluminantToTemperature(out->calibrationIlluminant1) == 0.0)
	{
		*outError = "dcp: unknown CalibrationIlluminant1 -- refused";
		return false;
	}

	if (out->hueSatMap1.IsValid() && out->hueSatMap2.IsValid())
	{
		if (out->hueSatMap1.hueDivisions != out->hueSatMap2.hueDivisions
		    || out->hueSatMap1.satDivisions != out->hueSatMap2.satDivisions
		    || out->hueSatMap1.valDivisions != out->hueSatMap2.valDivisions)
		{
			*outError = "dcp: HueSatMap1/2 dims differ";
			return false;
		}
	}

	return true;
}

static bool SetupReader(const u8 *data, size_t len, u16 expectMagic,
                        SDcpReader *r, u32 *outFirstIfd, std::string *outError)
{
	if (data == nullptr || len < 8)
	{
		*outError = "dcp: file too small";
		return false;
	}
	r->data = data;
	r->len = len;
	if (data[0] == 'I' && data[1] == 'I')
		r->bigEndian = false;
	else if (data[0] == 'M' && data[1] == 'M')
		r->bigEndian = true;
	else
	{
		*outError = "dcp: not a TIFF-shaped container";
		return false;
	}
	const u16 magic = r->U16(2);
	if (magic != expectMagic)
	{
		*outError = (magic == kTiffMagic || magic == kDcpMagic)
			? "dcp: wrong magic for this path (0x4352 is a .dcp, 42 is a DNG)"
			: "dcp: unknown magic";
		return false;
	}
	*outFirstIfd = r->U32(4);
	return true;
}

bool CDcpProfile::ParseDcpBytes(const u8 *data, size_t len, SDcpProfile *out,
                                std::string *outError)
{
	*out = SDcpProfile();
	SDcpReader r;
	u32 firstIfd = 0;
	if (!SetupReader(data, len, kDcpMagic, &r, &firstIfd, outError))
		return false;
	return ParseProfileIfd(r, firstIfd, out, outError);
}

bool CDcpProfile::ParseDngEmbedded(const u8 *data, size_t len, u32 ifdOffset,
                                   SDcpProfile *out, std::string *outError)
{
	*out = SDcpProfile();
	SDcpReader r;
	u32 firstIfd = 0;
	if (!SetupReader(data, len, kTiffMagic, &r, &firstIfd, outError))
		return false;
	return ParseProfileIfd(r, ifdOffset != 0 ? ifdOffset : firstIfd,
	                       out, outError);
}

static bool ReadWholeFile(const char *utf8Path, std::vector<u8> *out)
{
	FILE *f = SYS_FopenUtf8(utf8Path, "rb");
	if (f == nullptr)
		return false;
	fseek(f, 0, SEEK_END);
	const long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0)
	{
		fclose(f);
		return false;
	}
	out->resize((size_t)sz);
	const size_t got = fread(out->data(), 1, (size_t)sz, f);
	fclose(f);
	return got == (size_t)sz;
}

bool CDcpProfile::ParseDcpFile(const char *utf8Path, SDcpProfile *out,
                               std::string *outError)
{
	std::vector<u8> bytes;
	if (!ReadWholeFile(utf8Path, &bytes))
	{
		*outError = "dcp: cannot read file";
		return false;
	}
	return ParseDcpBytes(bytes.data(), bytes.size(), out, outError);
}

bool CDcpProfile::ParseDngEmbeddedFromFile(const char *utf8Path, u32 ifdOffset,
                                           SDcpProfile *out, std::string *outError)
{
	std::vector<u8> bytes;
	if (!ReadWholeFile(utf8Path, &bytes))
	{
		*outError = "dcp: cannot read file";
		return false;
	}
	return ParseDngEmbedded(bytes.data(), bytes.size(), ifdOffset, out, outError);
}

bool CDcpProfile::ReadFileTagsFromBytes(const u8 *data, size_t len,
                                        SFileTags *out)
{
	*out = SFileTags();
	SDcpReader r;
	u32 firstIfd = 0;
	std::string err;
	// Either container: a .dcp's own IFD or a DNG's IFD0.
	if (!SetupReader(data, len, kDcpMagic, &r, &firstIfd, &err)
	    && !SetupReader(data, len, kTiffMagic, &r, &firstIfd, &err))
		return false;

	std::vector<SDcpEntry> entries;
	if (!ReadIfdEntries(r, firstIfd, &entries, &err))
		return false;

	const SDcpEntry *e;
	if ((e = FindEntry(entries, kTagUniqueCameraModel)) != nullptr)
		ReadAsciiValue(r, *e, &out->uniqueCameraModel);
	if ((e = FindEntry(entries, kTagProfileName)) != nullptr)
		ReadAsciiValue(r, *e, &out->profileName);
	if ((e = FindEntry(entries, kTagCameraCalibrationSig)) != nullptr)
		ReadAsciiValue(r, *e, &out->cameraCalibrationSignature);
	if ((e = FindEntry(entries, kTagAsShotProfileName)) != nullptr)
		ReadAsciiValue(r, *e, &out->asShotProfileName);
	if ((e = FindEntry(entries, kTagExtraCameraProfiles)) != nullptr
	    && e->type == kTypeLong && e->count <= kMaxExtraProfiles)
	{
		for (u32 i = 0; i < e->count; i++)
			out->extraProfileOffsets.push_back(
				r.U32(e->valueOffset + (size_t)i * 4));
	}
	return true;
}

bool CDcpProfile::ReadFileTags(const char *utf8Path, SFileTags *out)
{
	std::vector<u8> bytes;
	if (!ReadWholeFile(utf8Path, &bytes))
		return false;
	return ReadFileTagsFromBytes(bytes.data(), bytes.size(), out);
}

// ---------------------------------------------------------------------------
// Fingerprint (spec F10 -- dng_camera_profile::CalculateFingerprint,
// little-endian MD5; matrices as SRATIONAL pairs with denominator 10000,
// dng_image_writer's tag_matrix)
// ---------------------------------------------------------------------------

struct SFingerprintStream
{
	MD5 md5;

	void PutBytes(const void *p, size_t n) { md5.append(p, (int)n); }
	void PutU16(u16 v)
	{
		u8 b[2] = { (u8)(v & 0xff), (u8)(v >> 8) };
		PutBytes(b, 2);
	}
	void PutU32(u32 v)
	{
		u8 b[4] = { (u8)(v & 0xff), (u8)((v >> 8) & 0xff),
		            (u8)((v >> 16) & 0xff), (u8)(v >> 24) };
		PutBytes(b, 4);
	}
	void PutI32(int32_t v) { PutU32((u32)v); }
	void PutF32(float f)
	{
		u32 v;
		std::memcpy(&v, &f, 4);
		PutU32(v);
	}
	void PutF64(double d)
	{
		u64 v;
		std::memcpy(&v, &d, 8);
		PutU32((u32)(v & 0xffffffffu));
		PutU32((u32)(v >> 32));
	}
	void PutMatrix(const float m[3][3])
	{
		// tag_matrix: each element as SRATIONAL round(v*10000)/10000.
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++)
			{
				PutI32((int32_t)std::lround((double)m[r][c] * 10000.0));
				PutI32(10000);
			}
	}
	void PutHueSatMap(const SDcpHueSatMap &map)
	{
		if (!map.IsValid())
			return;
		PutU32(map.hueDivisions);
		PutU32(map.satDivisions);
		PutU32(map.valDivisions);
		// Storage order IS the dump order (val slowest, hue, sat fastest).
		for (float f : map.deltas)
			PutF32(f);
	}
};

void CDcpProfile::ComputeFingerprint(const SDcpProfile &p, u8 outDigest[16])
{
	SFingerprintStream s;

	if (p.hasColorMatrix1)
	{
		s.PutU16((u16)p.calibrationIlluminant1);
		s.PutMatrix(p.colorMatrix1);
		if (p.hasForwardMatrix1)
			s.PutMatrix(p.forwardMatrix1);
		if (p.hasColorMatrix2)
		{
			s.PutU16((u16)p.calibrationIlluminant2);
			s.PutMatrix(p.colorMatrix2);
			if (p.hasForwardMatrix2)
				s.PutMatrix(p.forwardMatrix2);
		}
		s.PutBytes(p.profileName.data(), p.profileName.size());
		s.PutBytes(p.calibrationSignature.data(), p.calibrationSignature.size());
		s.PutU32(p.embedPolicy);
		s.PutBytes(p.copyright.data(), p.copyright.size());

		const bool haveHueSat1 = p.hueSatMap1.IsValid();
		const bool haveHueSat2 = p.hueSatMap2.IsValid() && p.hasColorMatrix2;
		if (haveHueSat1)
			s.PutHueSatMap(p.hueSatMap1);
		if (haveHueSat2)
			s.PutHueSatMap(p.hueSatMap2);
		if ((haveHueSat1 || haveHueSat2) && p.hueSatMapEncoding != 0)
			s.PutU32((u32)p.hueSatMapEncoding);

		if (p.lookTable.IsValid())
		{
			s.PutHueSatMap(p.lookTable);
			if (p.lookTableEncoding != 0)
				s.PutU32((u32)p.lookTableEncoding);
		}

		if (p.hasBaselineExposureOffset && p.baselineExposureOffset != 0.f)
			s.PutF64((double)p.baselineExposureOffset);
		if (p.defaultBlackRender != 0)
			s.PutI32(p.defaultBlackRender);

		for (const auto &pt : p.toneCurve)
		{
			s.PutF32(pt.first);
			s.PutF32(pt.second);
		}
	}

	s.md5.finish();
	std::memcpy(outDigest, s.md5.getDigest(), 16);
}
