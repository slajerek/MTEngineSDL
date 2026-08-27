#include "CTestIccProfileCodec.h"
#include "CTestIccHelpers.h"

#include "CIccProfileCodec.h"
#include "ICC_SRGBProfile.h"
#include "DBG_Log.h"
#include "zlib.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace std;

#define IC_ASSERT(cond, msg) \
	do { \
		bool icOk = (cond); \
		if (!icOk) { \
			char buf[256]; \
			snprintf(buf, sizeof(buf), "FAIL: %s", msg); \
			LOGD("CTestIccProfileCodec: %s", buf); \
			TestCompleted(false, buf); \
			return; \
		} \
		StepCompleted(stepNum++, true, msg); \
	} while(0)

namespace IccTestFixtures
{

vector<uint8_t> MakeSyntheticProfile(uint32_t totalSize)
{
	vector<uint8_t> p(totalSize, 0);
	if (totalSize >= 4)
	{
		p[0] = (uint8_t)((totalSize >> 24) & 0xFF);
		p[1] = (uint8_t)((totalSize >> 16) & 0xFF);
		p[2] = (uint8_t)((totalSize >> 8) & 0xFF);
		p[3] = (uint8_t)(totalSize & 0xFF);
	}
	if (totalSize >= 40)
	{
		p[36] = 'a'; p[37] = 'c'; p[38] = 's'; p[39] = 'p';
	}
	// tag count at 128..131 stays zero: a profile with no tags is structurally
	// valid, just not useful to a CMM.
	for (uint32_t i = 132; i < totalSize; i++)
		p[i] = (uint8_t)((i * 31u) & 0xFF);
	return p;
}

vector<uint8_t> MakeOpenableProfileVariant(int variantIndex)
{
	vector<uint8_t> p = ICC_BuildSRGBProfileV2();

	// Find the 'desc' tag and poke one byte of its text payload. Same length,
	// so every tag offset/size in the table stays correct and the profile still
	// opens in a real CMM -- only the content digest changes.
	if (p.size() < 132)
		return p;
	uint32_t nTags = ((uint32_t)p[128] << 24) | ((uint32_t)p[129] << 16) |
	                 ((uint32_t)p[130] << 8) | (uint32_t)p[131];
	for (uint32_t i = 0; i < nTags; i++)
	{
		size_t e = 132 + (size_t)i * 12;
		if (e + 12 > p.size())
			break;
		if (memcmp(&p[e], "desc", 4) != 0)
			continue;
		uint32_t off = ((uint32_t)p[e+4] << 24) | ((uint32_t)p[e+5] << 16) |
		               ((uint32_t)p[e+6] << 8) | (uint32_t)p[e+7];
		uint32_t sz  = ((uint32_t)p[e+8] << 24) | ((uint32_t)p[e+9] << 16) |
		               ((uint32_t)p[e+10] << 8) | (uint32_t)p[e+11];
		// textDescriptionType: 'desc' + reserved(4) + ASCII count(4), then the
		// ASCII string. Land inside the string, well past the type header.
		if (sz > 16 && (size_t)off + 13 < p.size())
			p[off + 12] = (uint8_t)('A' + (variantIndex % 26));
		break;
	}
	return p;
}

} // namespace IccTestFixtures

namespace
{

using IccTestFixtures::MakeSyntheticProfile;

bool DigestsEqual(const uint8_t a[16], const uint8_t b[16])
{
	return memcmp(a, b, 16) == 0;
}

// zlib-compress a buffer the way a PNG encoder writes an iCCP payload.
vector<uint8_t> Deflate(const vector<uint8_t> &in)
{
	uLongf bound = compressBound((uLong)in.size());
	vector<uint8_t> out(bound);
	if (compress2(&out[0], &bound, in.empty() ? NULL : &in[0], (uLong)in.size(), 9) != Z_OK)
		return vector<uint8_t>();
	out.resize(bound);
	return out;
}

// "name\0" + compression method byte + deflate stream
vector<uint8_t> MakeIccpPayload(const vector<uint8_t> &profile, uint8_t method)
{
	vector<uint8_t> payload;
	const char *name = "icc";
	payload.insert(payload.end(), name, name + strlen(name));
	payload.push_back(0);
	payload.push_back(method);
	vector<uint8_t> z = Deflate(profile);
	payload.insert(payload.end(), z.begin(), z.end());
	return payload;
}

} // namespace

CTestIccProfileCodec::CTestIccProfileCodec() {}
CTestIccProfileCodec::~CTestIccProfileCodec() {}

void CTestIccProfileCodec::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;

	LOGD("CTestIccProfileCodec: starting");

	const vector<uint8_t> srgb = ICC_BuildSRGBProfileV2();
	const vector<uint8_t> big = MakeSyntheticProfile(70000);

	// ---------------------------------------------------------------- 1. ValidateHeader
	IC_ASSERT(CIccProfileCodec::ValidateHeader(&srgb[0], (uint32_t)srgb.size()),
	          "ValidateHeader accepts the built-in sRGB profile");
	IC_ASSERT(CIccProfileCodec::ValidateHeader(&big[0], (uint32_t)big.size()),
	          "ValidateHeader accepts a 70000-byte synthetic (tag count 0 is valid)");
	{
		vector<uint8_t> tiny(100, 0xAB);
		IC_ASSERT(!CIccProfileCodec::ValidateHeader(&tiny[0], (uint32_t)tiny.size()),
		          "ValidateHeader rejects a 100-byte buffer (below the header size)");
	}
	{
		// Size field claims 80000, buffer holds 70000: truncated.
		vector<uint8_t> t = big;
		t[0] = 0; t[1] = 0x01; t[2] = 0x38; t[3] = 0x80;   // 80000
		IC_ASSERT(!CIccProfileCodec::ValidateHeader(&t[0], (uint32_t)t.size()),
		          "ValidateHeader rejects a size field larger than the buffer");
	}
	{
		vector<uint8_t> t = big;
		t[37] = 'X';   // break 'acsp'
		IC_ASSERT(!CIccProfileCodec::ValidateHeader(&t[0], (uint32_t)t.size()),
		          "ValidateHeader rejects a bad 'acsp' signature");
	}
	{
		// Tag count 4 but the profile ends right after the count field.
		vector<uint8_t> t = MakeSyntheticProfile(136);
		t[131] = 4;
		IC_ASSERT(!CIccProfileCodec::ValidateHeader(&t[0], (uint32_t)t.size()),
		          "ValidateHeader rejects a tag table extending past the profile");
	}

	// ------------------------------------------------- 1b. boundaries and overflow
	{
		vector<uint8_t> t = MakeSyntheticProfile(128);
		IC_ASSERT(!CIccProfileCodec::ValidateHeader(&t[0], 128),
		          "ValidateHeader rejects exactly 128 bytes (tag-count field absent)");
	}
	{
		vector<uint8_t> t = MakeSyntheticProfile(131);
		IC_ASSERT(!CIccProfileCodec::ValidateHeader(&t[0], 131),
		          "ValidateHeader rejects 131 bytes (tag-count field truncated)");
	}
	{
		// 0x15555556 * 12 == 0x100000008, i.e. 8 once it wraps u32 -- so with
		// 32-bit bounds math the table "ends" at offset 140 and sails through.
		// (A larger count like 0x20000000 wraps to 0x80000000 and is still
		// caught by accident, which is why the value here is exact.)
		vector<uint8_t> t = big;
		t[128] = 0x15; t[129] = 0x55; t[130] = 0x55; t[131] = 0x56;
		IC_ASSERT(!CIccProfileCodec::ValidateHeader(&t[0], (uint32_t)t.size()),
		          "ValidateHeader rejects a tag count whose table size overflows u32");
	}

	// ------------------------------------------------------------- 2. GetProfileId
	uint8_t idSrgb[16], idSrgb2[16], idBig[16];
	CIccProfileCodec::GetProfileId(&srgb[0], (uint32_t)srgb.size(), idSrgb);
	CIccProfileCodec::GetProfileId(&srgb[0], (uint32_t)srgb.size(), idSrgb2);
	CIccProfileCodec::GetProfileId(&big[0], (uint32_t)big.size(), idBig);
	IC_ASSERT(DigestsEqual(idSrgb, idSrgb2), "GetProfileId is stable across calls");
	IC_ASSERT(!DigestsEqual(idSrgb, idBig), "GetProfileId differs between distinct profiles");
	{
		vector<uint8_t> t = srgb;
		for (int i = 0; i < 16; i++)
			t[84 + i] = (uint8_t)(0xA0 + i);
		uint8_t id[16];
		CIccProfileCodec::GetProfileId(&t[0], (uint32_t)t.size(), id);
		bool verbatim = true;
		for (int i = 0; i < 16; i++)
			if (id[i] != (uint8_t)(0xA0 + i)) { verbatim = false; break; }
		IC_ASSERT(verbatim, "GetProfileId returns a non-zero header ID field verbatim");
	}

	// ------------------------------------ 2b. ICC 7.2.18 field zeroing on the compute path
	{
		vector<uint8_t> a = big;                       // ID field is zero -> computed
		vector<uint8_t> b = big;
		b[64] = 0x00; b[65] = 0x00; b[66] = 0x00; b[67] = 0x02;   // rendering intent
		uint8_t ia[16], ib[16];
		CIccProfileCodec::GetProfileId(&a[0], (uint32_t)a.size(), ia);
		CIccProfileCodec::GetProfileId(&b[0], (uint32_t)b.size(), ib);
		IC_ASSERT(DigestsEqual(ia, ib), "GetProfileId ignores the rendering-intent field");

		vector<uint8_t> c = big;
		c[44] = 0xFF; c[45] = 0xFF;                    // profile flags
		uint8_t ic[16];
		CIccProfileCodec::GetProfileId(&c[0], (uint32_t)c.size(), ic);
		IC_ASSERT(DigestsEqual(ia, ic), "GetProfileId ignores the profile-flags field");

		vector<uint8_t> d = big;
		d[200] = (uint8_t)(d[200] ^ 0xFF);             // an ordinary payload byte
		uint8_t idd[16];
		CIccProfileCodec::GetProfileId(&d[0], (uint32_t)d.size(), idd);
		IC_ASSERT(!DigestsEqual(ia, idd), "GetProfileId changes when any other byte changes");
	}

	// ------------------------------------------- 2c. the trust boundary: content digest
	{
		uint8_t dPlain[16], dForged[16], idForged[16];
		CIccProfileCodec::GetContentDigest(&srgb[0], (uint32_t)srgb.size(), dPlain);

		// Forge a profile ID: a hostile file can claim any identity it likes.
		vector<uint8_t> forged = srgb;
		for (int i = 0; i < 16; i++)
			forged[84 + i] = (uint8_t)(0xA0 + i);
		CIccProfileCodec::GetProfileId(&forged[0], (uint32_t)forged.size(), idForged);
		CIccProfileCodec::GetContentDigest(&forged[0], (uint32_t)forged.size(), dForged);

		bool idIsForgery = true;
		for (int i = 0; i < 16; i++)
			if (idForged[i] != (uint8_t)(0xA0 + i)) { idIsForgery = false; break; }
		IC_ASSERT(idIsForgery, "GetProfileId hands back the forged ID (why it must not key a cache)");
		IC_ASSERT(!DigestsEqual(dPlain, dForged),
		          "GetContentDigest sees the forged ID as a byte change, not an identity");

		vector<uint8_t> copy = srgb;
		uint8_t dCopy[16];
		CIccProfileCodec::GetContentDigest(&copy[0], (uint32_t)copy.size(), dCopy);
		IC_ASSERT(DigestsEqual(dPlain, dCopy), "GetContentDigest matches for byte-identical profiles");

		vector<uint8_t> flip = srgb;
		flip[150] = (uint8_t)(flip[150] ^ 0x01);
		uint8_t dFlip[16];
		CIccProfileCodec::GetContentDigest(&flip[0], (uint32_t)flip.size(), dFlip);
		IC_ASSERT(!DigestsEqual(dPlain, dFlip), "GetContentDigest changes on a single byte flip");
	}

	// ----------------------------------------------------------------- 3. SplitApp2
	{
		vector<uint8_t> smallProfile = MakeSyntheticProfile(1000);
		vector<vector<uint8_t> > segs = CIccProfileCodec::SplitApp2(&smallProfile[0], (uint32_t)smallProfile.size());
		IC_ASSERT(segs.size() == 1, "SplitApp2 emits one segment for a 1000-byte profile");
		IC_ASSERT(segs[0].size() == 14 + 1000, "single segment is identifier + seq/count + payload");
		IC_ASSERT(memcmp(&segs[0][0], "ICC_PROFILE\0", 12) == 0, "segment carries the ICC_PROFILE identifier");
		IC_ASSERT(segs[0][12] == 1 && segs[0][13] == 1, "single segment is numbered 1 of 1");

		vector<vector<uint8_t> > bigSegs = CIccProfileCodec::SplitApp2(&big[0], (uint32_t)big.size());
		IC_ASSERT(bigSegs.size() == 2, "SplitApp2 emits two segments for a 70000-byte profile");
		IC_ASSERT(bigSegs[0][12] == 1 && bigSegs[0][13] == 2, "first segment is 1 of 2");
		IC_ASSERT(bigSegs[1][12] == 2 && bigSegs[1][13] == 2, "second segment is 2 of 2");
	}

	// ------------------------------------------------------------------ 4-6. JoinApp2
	{
		vector<vector<uint8_t> > segs = CIccProfileCodec::SplitApp2(&big[0], (uint32_t)big.size());
		vector<uint8_t> rt = CIccProfileCodec::JoinApp2(segs);
		IC_ASSERT(rt.size() == big.size() && memcmp(&rt[0], &big[0], big.size()) == 0,
		          "JoinApp2(SplitApp2(p)) round-trips a >64KB profile byte-for-byte");

		vector<vector<uint8_t> > swapped;
		swapped.push_back(segs[1]);
		swapped.push_back(segs[0]);
		vector<uint8_t> rt2 = CIccProfileCodec::JoinApp2(swapped);
		IC_ASSERT(rt2.size() == big.size() && memcmp(&rt2[0], &big[0], big.size()) == 0,
		          "JoinApp2 reassembles out-of-order segments by sequence number");

		// A non-ICC APP2 mixed in must be ignored, not break the set.
		vector<vector<uint8_t> > withNoise = swapped;
		withNoise.insert(withNoise.begin(), vector<uint8_t>(200, 0x5A));
		vector<uint8_t> rt3 = CIccProfileCodec::JoinApp2(withNoise);
		IC_ASSERT(rt3.size() == big.size() && memcmp(&rt3[0], &big[0], big.size()) == 0,
		          "JoinApp2 ignores unrelated APP2 payloads");
	}
	{
		// Three segments with the middle one missing: a gap must yield nothing,
		// never a partial profile -- a truncated profile in a CMM is a crash.
		vector<uint8_t> huge = MakeSyntheticProfile(140000);
		vector<vector<uint8_t> > segs = CIccProfileCodec::SplitApp2(&huge[0], (uint32_t)huge.size());
		IC_ASSERT(segs.size() == 3, "a 140000-byte profile splits into three segments");
		vector<vector<uint8_t> > gap;
		gap.push_back(segs[0]);
		gap.push_back(segs[2]);
		IC_ASSERT(CIccProfileCodec::JoinApp2(gap).empty(),
		          "JoinApp2 yields nothing when a segment is missing");

		vector<vector<uint8_t> > dup;
		dup.push_back(segs[0]);
		dup.push_back(segs[0]);
		dup.push_back(segs[2]);
		IC_ASSERT(CIccProfileCodec::JoinApp2(dup).empty(),
		          "JoinApp2 yields nothing on a duplicate sequence number");

		vector<vector<uint8_t> > mixedCount;
		mixedCount.push_back(segs[0]);
		vector<uint8_t> badCount = segs[1];
		badCount[13] = 9;   // claims 9 total
		mixedCount.push_back(badCount);
		IC_ASSERT(CIccProfileCodec::JoinApp2(mixedCount).empty(),
		          "JoinApp2 yields nothing when segments disagree on the count");
	}
	{
		vector<vector<uint8_t> > none;
		IC_ASSERT(CIccProfileCodec::JoinApp2(none).empty(), "JoinApp2 of nothing is empty");
		vector<vector<uint8_t> > onlyNoise;
		onlyNoise.push_back(vector<uint8_t>(200, 0x5A));
		IC_ASSERT(CIccProfileCodec::JoinApp2(onlyNoise).empty(),
		          "JoinApp2 of only non-ICC APP2s is empty");
	}

	// ---------------------------------------------------------------- 7. InflateIccp
	{
		vector<uint8_t> payload = MakeIccpPayload(srgb, 0);
		vector<uint8_t> got = CIccProfileCodec::InflateIccp(&payload[0], (uint32_t)payload.size());
		IC_ASSERT(got.size() == srgb.size() && memcmp(&got[0], &srgb[0], srgb.size()) == 0,
		          "InflateIccp recovers the profile from a zlib iCCP payload");

		vector<uint8_t> badMethod = MakeIccpPayload(srgb, 1);
		IC_ASSERT(CIccProfileCodec::InflateIccp(&badMethod[0], (uint32_t)badMethod.size()).empty(),
		          "InflateIccp rejects an unknown compression method");

		// Cap the output below the real size: a hostile chunk must not balloon.
		IC_ASSERT(CIccProfileCodec::InflateIccp(&payload[0], (uint32_t)payload.size(), 256).empty(),
		          "InflateIccp refuses output exceeding maxOut");

		vector<uint8_t> truncated(payload.begin(), payload.begin() + payload.size() / 2);
		IC_ASSERT(CIccProfileCodec::InflateIccp(&truncated[0], (uint32_t)truncated.size()).empty(),
		          "InflateIccp rejects a truncated deflate stream");

		vector<uint8_t> unterminated(20, 'x');   // no NUL: name never ends
		IC_ASSERT(CIccProfileCodec::InflateIccp(&unterminated[0], (uint32_t)unterminated.size()).empty(),
		          "InflateIccp rejects an unterminated profile name");
	}

	// ---------------------------------------------------------------- 8. GetDescription
	// CM-B #5.4 needs the profile's own name for the settings pane's
	// "which profile is actually in effect" readout.
	{
		const vector<uint8_t> adobe = ICC_BuildAdobeRGBProfileV2();

		const string srgbDesc  = CIccProfileCodec::GetDescription(&srgb[0], (uint32_t)srgb.size());
		const string adobeDesc = CIccProfileCodec::GetDescription(&adobe[0], (uint32_t)adobe.size());

		IC_ASSERT(!srgbDesc.empty(),  "the built-in sRGB profile has a readable description");
		IC_ASSERT(!adobeDesc.empty(), "the built-in Adobe RGB profile has a readable description");
		IC_ASSERT(srgbDesc != adobeDesc,
		          "the two built-ins describe themselves differently (a UI can tell them apart)");

		// No trailing NUL or padding leaks into the string -- it goes straight
		// into a settings pane.
		IC_ASSERT(srgbDesc.find('\0') == string::npos, "no interior NUL in the description");
		IC_ASSERT(!srgbDesc.empty() && srgbDesc.back() != ' ',
		          "trailing padding is trimmed");

		// Untrusted input: every one of these must return "" rather than read
		// out of bounds. (ValidateHeader would reject them, but GetDescription
		// does not assume the caller ran it.)
		IC_ASSERT(CIccProfileCodec::GetDescription(NULL, 0).empty(), "NULL yields no description");
		vector<uint8_t> tiny(64, 0x00);
		IC_ASSERT(CIccProfileCodec::GetDescription(&tiny[0], (uint32_t)tiny.size()).empty(),
		          "a too-short buffer yields no description");
		vector<uint8_t> noise(4096, 0xAB);
		IC_ASSERT(CIccProfileCodec::GetDescription(&noise[0], (uint32_t)noise.size()).empty(),
		          "random bytes yield no description");

		// A truncated real profile: the tag table still claims tags whose data
		// now lies past the end. The bounds checks, not luck, must catch it.
		vector<uint8_t> truncatedProfile(srgb.begin(), srgb.begin() + srgb.size() / 2);
		const string truncDesc = CIccProfileCodec::GetDescription(&truncatedProfile[0],
		                                                          (uint32_t)truncatedProfile.size());
		IC_ASSERT(truncDesc.empty() || truncDesc == srgbDesc,
		          "a truncated profile yields either nothing or the intact name, never garbage");
	}

	TestCompleted(true, "ICC codec: validation, digests, APP2 split/join, iCCP inflate and description verified");
}

void CTestIccProfileCodec::Cancel()
{
	isRunning = false;
}
