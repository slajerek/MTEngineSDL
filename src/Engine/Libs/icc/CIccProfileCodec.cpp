#include "CIccProfileCodec.h"

#include "MD5.h"
#include "zlib.h"

#include <cstring>

namespace
{

const uint32_t kIccHeaderSize = 128;
const uint32_t kTagCountOffset = 128;
const uint32_t kTagEntrySize = 12;
const uint32_t kIccIdOffset = 84;
const uint32_t kIccIdSize = 16;
const uint32_t kRenderingIntentOffset = 64;
const uint32_t kProfileFlagsOffset = 44;

const uint8_t kIccApp2Id[12] = { 'I', 'C', 'C', '_', 'P', 'R', 'O', 'F', 'I', 'L', 'E', '\0' };
const uint32_t kIccApp2IdSize = 12;
const uint32_t kIccApp2HeaderSize = kIccApp2IdSize + 2;   // identifier + seq + count

uint32_t ReadU32BE(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

void Md5Of(const uint8_t *bytes, uint32_t size, uint8_t out[16])
{
    MD5 md5;
    md5.append(bytes, (int)size);
    md5.finish();
    memcpy(out, md5.getDigest(), 16);
}

} // namespace

bool CIccProfileCodec::ValidateHeader(const uint8_t *bytes, uint32_t size)
{
    if (bytes == NULL || size < kIccHeaderSize)
        return false;

    // The header's own size field must describe a profile that fits in what we
    // were handed. Larger means truncated; smaller than a header is nonsense.
    const uint32_t declared = ReadU32BE(bytes);
    if (declared < kIccHeaderSize || declared > size)
        return false;

    if (memcmp(bytes + 36, "acsp", 4) != 0)
        return false;

    // The tag count sits immediately after the header, so a profile that is
    // exactly 128 bytes has nowhere to put it. Such a profile carries no tags
    // at all and is useless to a CMM, so reject rather than accept-and-crash.
    if (declared < kTagCountOffset + 4)
        return false;

    const uint32_t tagCount = ReadU32BE(bytes + kTagCountOffset);

    // Compute the table's extent in 64-bit: tagCount is attacker-controlled and
    // tagCount * 12 overflows 32 bits from tagCount = 0x15555556 upwards, which
    // would wrap to a small number and let the bounds check pass.
    const uint64_t tableEnd = (uint64_t)kTagCountOffset + 4 + (uint64_t)tagCount * kTagEntrySize;
    if (tableEnd > (uint64_t)declared)
        return false;

    for (uint32_t i = 0; i < tagCount; i++)
    {
        const uint8_t *entry = bytes + kTagCountOffset + 4 + i * kTagEntrySize;
        const uint64_t tagOffset = ReadU32BE(entry + 4);
        const uint64_t tagSize = ReadU32BE(entry + 8);
        if (tagOffset + tagSize > (uint64_t)declared)
            return false;
    }

    return true;
}

void CIccProfileCodec::GetProfileId(const uint8_t *bytes, uint32_t size, uint8_t outId[16])
{
    memset(outId, 0, 16);
    if (bytes == NULL || size < kIccHeaderSize)
        return;

    // A non-zero ID field is the profile's declared identity: return it as-is.
    bool idPresent = false;
    for (uint32_t i = 0; i < kIccIdSize; i++)
    {
        if (bytes[kIccIdOffset + i] != 0)
        {
            idPresent = true;
            break;
        }
    }
    if (idPresent)
    {
        memcpy(outId, bytes + kIccIdOffset, kIccIdSize);
        return;
    }

    // Otherwise compute it the way the spec says: MD5 over the profile with the
    // three fields that do not participate zeroed out. Those fields can differ
    // between two copies of the same profile, so hashing them would make the ID
    // depend on how the profile was embedded rather than on what it describes.
    std::vector<uint8_t> tmp(bytes, bytes + size);
    memset(&tmp[kProfileFlagsOffset], 0, 4);
    memset(&tmp[kRenderingIntentOffset], 0, 4);
    memset(&tmp[kIccIdOffset], 0, kIccIdSize);
    Md5Of(&tmp[0], size, outId);
}

void CIccProfileCodec::GetContentDigest(const uint8_t *bytes, uint32_t size, uint8_t outDigest[16])
{
    memset(outDigest, 0, 16);
    if (bytes == NULL || size == 0)
        return;
    Md5Of(bytes, size, outDigest);
}

std::vector<std::vector<uint8_t> > CIccProfileCodec::SplitApp2(const uint8_t *profile, uint32_t size)
{
    std::vector<std::vector<uint8_t> > out;
    if (profile == NULL || size == 0)
        return out;

    const uint32_t segments = (size + kMaxApp2ProfileBytes - 1) / kMaxApp2ProfileBytes;
    if (segments > 255)   // the count is a single byte
        return out;

    uint32_t consumed = 0;
    for (uint32_t i = 0; i < segments; i++)
    {
        uint32_t chunk = size - consumed;
        if (chunk > kMaxApp2ProfileBytes)
            chunk = kMaxApp2ProfileBytes;

        std::vector<uint8_t> seg;
        seg.reserve(kIccApp2HeaderSize + chunk);
        seg.insert(seg.end(), kIccApp2Id, kIccApp2Id + kIccApp2IdSize);
        seg.push_back((uint8_t)(i + 1));          // sequence numbers are 1-based
        seg.push_back((uint8_t)segments);
        seg.insert(seg.end(), profile + consumed, profile + consumed + chunk);
        out.push_back(seg);

        consumed += chunk;
    }
    return out;
}

std::vector<uint8_t> CIccProfileCodec::JoinApp2(const std::vector<std::vector<uint8_t> > &segments)
{
    std::vector<uint8_t> empty;

    // Collect the ICC-bearing segments, indexed by their sequence number, and
    // insist the whole set agrees on the count.
    int declaredCount = -1;
    std::vector<const std::vector<uint8_t> *> bySeq;

    for (size_t i = 0; i < segments.size(); i++)
    {
        const std::vector<uint8_t> &seg = segments[i];
        if (seg.size() < kIccApp2HeaderSize)
            continue;
        if (memcmp(&seg[0], kIccApp2Id, kIccApp2IdSize) != 0)
            continue;   // some other APP2 (Flashpix, ...) -- not ours

        const int seq = seg[kIccApp2IdSize];
        const int count = seg[kIccApp2IdSize + 1];
        if (count == 0 || seq == 0 || seq > count)
            return empty;

        if (declaredCount < 0)
        {
            declaredCount = count;
            bySeq.assign((size_t)count, (const std::vector<uint8_t> *)NULL);
        }
        else if (count != declaredCount)
        {
            return empty;   // segments disagree about how many there are
        }

        if (bySeq[(size_t)(seq - 1)] != NULL)
            return empty;   // duplicate sequence number
        bySeq[(size_t)(seq - 1)] = &seg;
    }

    if (declaredCount < 0)
        return empty;   // no ICC segments at all

    for (size_t i = 0; i < bySeq.size(); i++)
    {
        if (bySeq[i] == NULL)
            return empty;   // a gap: yield nothing rather than a partial profile
    }

    std::vector<uint8_t> profile;
    for (size_t i = 0; i < bySeq.size(); i++)
    {
        const std::vector<uint8_t> &seg = *bySeq[i];
        profile.insert(profile.end(), seg.begin() + kIccApp2HeaderSize, seg.end());
    }

    if (!ValidateHeader(profile.empty() ? NULL : &profile[0], (uint32_t)profile.size()))
        return empty;

    // Trim to the profile's own declared size. The last segment is padded to
    // whatever the writer put there, and anything past the header's size field
    // is bytes we neither need nor trust -- do not hand them to a CMM.
    const uint32_t declared = ReadU32BE(&profile[0]);
    if (declared < profile.size())
        profile.resize(declared);

    return profile;
}

std::vector<uint8_t> CIccProfileCodec::InflateIccp(const uint8_t *payload, uint32_t size, uint32_t maxOut)
{
    std::vector<uint8_t> empty;
    if (payload == NULL || size == 0)
        return empty;

    // Profile name: NUL-terminated, 1..79 bytes per the PNG spec. We only need
    // to step over it, but it must actually be terminated inside the payload.
    uint32_t nameLen = 0;
    while (nameLen < size && payload[nameLen] != 0)
        nameLen++;
    if (nameLen >= size)
        return empty;   // unterminated name

    const uint32_t afterName = nameLen + 1;
    if (afterName >= size)
        return empty;   // no compression byte

    if (payload[afterName] != 0)
        return empty;   // only method 0 (zlib/deflate) is defined

    const uint32_t streamOffset = afterName + 1;
    if (streamOffset >= size)
        return empty;   // no deflate data

    // Inflate in bounded steps rather than with uncompress(), which needs the
    // output size up front -- and the whole point here is that we do not trust
    // the declared size of anything in this file.
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    if (inflateInit(&strm) != Z_OK)
        return empty;

    strm.next_in = (Bytef *)(payload + streamOffset);
    strm.avail_in = (uInt)(size - streamOffset);

    std::vector<uint8_t> out;
    uint8_t buf[16384];
    int ret = Z_OK;
    do
    {
        strm.next_out = buf;
        strm.avail_out = (uInt)sizeof(buf);
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END)
        {
            inflateEnd(&strm);
            return empty;
        }
        const size_t produced = sizeof(buf) - strm.avail_out;
        if (out.size() + produced > (size_t)maxOut)
        {
            inflateEnd(&strm);
            return empty;   // a small chunk claiming to be a huge profile
        }
        out.insert(out.end(), buf, buf + produced);
    }
    while (ret != Z_STREAM_END);

    inflateEnd(&strm);

    if (!ValidateHeader(out.empty() ? NULL : &out[0], (uint32_t)out.size()))
        return empty;

    return out;
}

// ---------------------------------------------------------------------------
// Profile description (CM-B #5.4)
// ---------------------------------------------------------------------------

namespace
{

// Append one Unicode code point as UTF-8.
void AppendUtf8(std::string &out, uint32_t cp)
{
    if (cp < 0x80)
    {
        out += (char)cp;
    }
    else if (cp < 0x800)
    {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
    else
    {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

// UTF-16BE -> UTF-8, surrogate pairs included. Malformed input is dropped
// rather than guessed at: a display name is cosmetic, so a partial name beats
// inventing code points from broken bytes.
std::string Utf16BEToUtf8(const uint8_t *p, uint32_t byteLen)
{
    std::string out;
    for (uint32_t i = 0; i + 1 < byteLen; i += 2)
    {
        uint32_t u = ((uint32_t)p[i] << 8) | p[i + 1];
        if (u == 0)
            break;                       // NUL terminates, even if the field is longer
        if (u >= 0xD800 && u <= 0xDBFF)  // high surrogate
        {
            if (i + 3 >= byteLen)
                break;
            const uint32_t lo = ((uint32_t)p[i + 2] << 8) | p[i + 3];
            if (lo < 0xDC00 || lo > 0xDFFF)
                break;                   // unpaired: stop rather than emit garbage
            u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
        }
        else if (u >= 0xDC00 && u <= 0xDFFF)
        {
            break;                       // lone low surrogate
        }
        AppendUtf8(out, u);
    }
    return out;
}

// Trim trailing NULs and whitespace -- ICC ASCII fields are NUL-padded and
// some writers pad with spaces too.
std::string TrimTail(std::string s)
{
    while (!s.empty() && (s.back() == '\0' || s.back() == ' ' || s.back() == '\t'
                          || s.back() == '\r' || s.back() == '\n'))
        s.pop_back();
    return s;
}

} // namespace

std::string CIccProfileCodec::GetDescription(const uint8_t *bytes, uint32_t size)
{
    if (bytes == NULL || size < kTagCountOffset + 4)
        return std::string();

    // Stay inside what the header itself declares, and inside what we were
    // handed -- whichever is smaller.
    uint32_t limit = ReadU32BE(bytes);
    if (limit < kIccHeaderSize || limit > size)
        limit = size;
    if (limit < kTagCountOffset + 4)
        return std::string();

    const uint32_t tagCount = ReadU32BE(bytes + kTagCountOffset);
    const uint64_t tableEnd = (uint64_t)kTagCountOffset + 4 + (uint64_t)tagCount * kTagEntrySize;
    if (tableEnd > (uint64_t)limit)
        return std::string();

    for (uint32_t i = 0; i < tagCount; i++)
    {
        const uint8_t *entry = bytes + kTagCountOffset + 4 + (uint64_t)i * kTagEntrySize;
        if (memcmp(entry, "desc", 4) != 0)
            continue;

        const uint32_t tagOffset = ReadU32BE(entry + 4);
        const uint32_t tagSize   = ReadU32BE(entry + 8);
        if (tagSize < 8 || (uint64_t)tagOffset + tagSize > (uint64_t)limit)
            return std::string();

        const uint8_t *tag = bytes + tagOffset;

        if (memcmp(tag, "desc", 4) == 0)
        {
            // ICC v2 textDescriptionType: sig(4) reserved(4) asciiCount(4)
            // then asciiCount bytes including the terminating NUL.
            if (tagSize < 12)
                return std::string();
            const uint32_t asciiCount = ReadU32BE(tag + 8);
            if (asciiCount == 0 || (uint64_t)12 + asciiCount > (uint64_t)tagSize)
                return std::string();
            return TrimTail(std::string((const char *)(tag + 12),
                                        (size_t)asciiCount));
        }

        if (memcmp(tag, "mluc", 4) == 0)
        {
            // ICC v4 multiLocalizedUnicodeType: sig(4) reserved(4) count(4)
            // recSize(4) then records of {lang(2) country(2) length(4)
            // offset(4)}, offsets relative to the START of the tag.
            if (tagSize < 16)
                return std::string();
            const uint32_t recCount = ReadU32BE(tag + 8);
            const uint32_t recSize  = ReadU32BE(tag + 12);
            if (recCount == 0 || recSize < 12)
                return std::string();
            if ((uint64_t)16 + (uint64_t)recSize > (uint64_t)tagSize)
                return std::string();

            // The first record. Profiles in the wild put the English name
            // first; picking by language would need a locale policy this layer
            // has no business holding.
            const uint8_t *rec = tag + 16;
            const uint32_t strLen    = ReadU32BE(rec + 4);
            const uint32_t strOffset = ReadU32BE(rec + 8);
            if (strLen == 0 || (uint64_t)strOffset + strLen > (uint64_t)tagSize)
                return std::string();

            return TrimTail(Utf16BEToUtf8(tag + strOffset, strLen));
        }

        return std::string();   // a 'desc' tag in some type we do not read
    }

    return std::string();
}
