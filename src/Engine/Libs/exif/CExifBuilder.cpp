#include "CExifBuilder.h"
#include <algorithm>
#include <cstring>

namespace {

// Bytes occupied by one component of each TIFF type.
unsigned TypeSize(uint16_t type)
{
    switch (type)
    {
        case 1:  return 1;   // BYTE
        case 2:  return 1;   // ASCII
        case 3:  return 2;   // SHORT
        case 4:  return 4;   // LONG
        case 5:  return 8;   // RATIONAL
        case 7:  return 1;   // UNDEFINED
        case 10: return 8;   // SRATIONAL
        default: return 0;
    }
}

void Put16(std::vector<uint8_t> &v, uint16_t x, bool le)
{
    if (le) { v.push_back(uint8_t(x & 0xFF)); v.push_back(uint8_t(x >> 8)); }
    else    { v.push_back(uint8_t(x >> 8));   v.push_back(uint8_t(x & 0xFF)); }
}

void Put32(std::vector<uint8_t> &v, uint32_t x, bool le)
{
    if (le)
    {
        v.push_back(uint8_t( x        & 0xFF)); v.push_back(uint8_t((x >>  8) & 0xFF));
        v.push_back(uint8_t((x >> 16) & 0xFF)); v.push_back(uint8_t((x >> 24) & 0xFF));
    }
    else
    {
        v.push_back(uint8_t((x >> 24) & 0xFF)); v.push_back(uint8_t((x >> 16) & 0xFF));
        v.push_back(uint8_t((x >>  8) & 0xFF)); v.push_back(uint8_t( x        & 0xFF));
    }
}

const uint16_t kExifIfdPointer = 0x8769;
const uint16_t kGpsIfdPointer  = 0x8825;

} // namespace

// ---------------------------------------------------------------------------
// Setters
// ---------------------------------------------------------------------------

void CExifBuilder::Put(Ifd ifd, Entry &&e)
{
    // A tag that failed once and then succeeded must not still be reported as
    // dropped: Dropped() would name a tag that IS in the output, and a caller
    // reporting "these fields were lost" would lie about it.
    for (size_t i = 0; i < dropped.size(); )
    {
        if (dropped[i] == e.tag) dropped.erase(dropped.begin() + i);
        else                     i++;
    }

    std::vector<Entry> &v = ifds[(int)ifd];
    for (size_t i = 0; i < v.size(); i++)
    {
        if (v[i].tag == e.tag) { v[i] = std::move(e); return; }   // last write wins
    }
    v.push_back(std::move(e));
}

void CExifBuilder::Drop(uint16_t tag)
{
    for (size_t i = 0; i < dropped.size(); i++)
        if (dropped[i] == tag) return;      // report each tag once
    dropped.push_back(tag);
}

void CExifBuilder::SetAscii(Ifd ifd, uint16_t tag, const std::string &value)
{
    // An interior NUL would truncate the field for every reader, so what
    // survived would not be what the caller set. Refuse rather than mislead.
    if (value.find('\0') != std::string::npos) { Drop(tag); return; }

    Entry e;
    e.tag = tag; e.type = 2;
    e.bytes.assign(value.begin(), value.end());
    e.bytes.push_back(0);                       // EXIF ASCII is NUL-terminated
    e.count = (uint32_t)e.bytes.size();
    Put(ifd, std::move(e));
}

void CExifBuilder::SetByte(Ifd ifd, uint16_t tag, uint8_t value)
{
    Entry e; e.tag = tag; e.type = 1; e.count = 1; e.vals.push_back(value);
    Put(ifd, std::move(e));
}

void CExifBuilder::SetShort(Ifd ifd, uint16_t tag, uint16_t value)
{
    Entry e; e.tag = tag; e.type = 3; e.count = 1; e.vals.push_back(value);
    Put(ifd, std::move(e));
}

void CExifBuilder::SetLong(Ifd ifd, uint16_t tag, uint32_t value)
{
    Entry e; e.tag = tag; e.type = 4; e.count = 1; e.vals.push_back(value);
    Put(ifd, std::move(e));
}

void CExifBuilder::SetRational(Ifd ifd, uint16_t tag, uint32_t num, uint32_t den)
{
    if (den == 0) { Drop(tag); return; }   // not a valid RATIONAL
    Entry e; e.tag = tag; e.type = 5; e.count = 1;
    e.vals.push_back(num); e.vals.push_back(den);
    Put(ifd, std::move(e));
}

void CExifBuilder::SetSRational(Ifd ifd, uint16_t tag, int32_t num, int32_t den)
{
    if (den == 0) { Drop(tag); return; }
    Entry e; e.tag = tag; e.type = 10; e.count = 1;
    e.vals.push_back((uint32_t)num); e.vals.push_back((uint32_t)den);
    Put(ifd, std::move(e));
}

void CExifBuilder::SetRationalArray(Ifd ifd, uint16_t tag,
                                    const std::vector<std::pair<uint32_t, uint32_t>> &values)
{
    if (values.empty()) { Drop(tag); return; }
    for (size_t i = 0; i < values.size(); i++)
    {
        if (values[i].second == 0) { Drop(tag); return; }
    }
    Entry e; e.tag = tag; e.type = 5; e.count = (uint32_t)values.size();
    for (size_t i = 0; i < values.size(); i++)
    {
        e.vals.push_back(values[i].first);
        e.vals.push_back(values[i].second);
    }
    Put(ifd, std::move(e));
}

void CExifBuilder::SetUndefined(Ifd ifd, uint16_t tag, const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) { Drop(tag); return; }
    Entry e; e.tag = tag; e.type = 7; e.count = (uint32_t)len;
    e.bytes.assign(data, data + len);
    Put(ifd, std::move(e));
}

bool CExifBuilder::IsEmpty() const
{
    return ifds[0].empty() && ifds[1].empty() && ifds[2].empty();
}

// ---------------------------------------------------------------------------
// Serialisation
// ---------------------------------------------------------------------------

std::vector<uint8_t> CExifBuilder::BuildApp1(bool littleEndian) const
{
    std::vector<uint8_t> out;
    if (IsEmpty())
        return out;

    const bool le = littleEndian;

    // Working copies: IFD0 additionally carries the sub-IFD pointer tags, and
    // TIFF requires entries in ascending tag order within an IFD.
    std::vector<Entry> ifd0 = ifds[0];
    std::vector<Entry> exif = ifds[1];
    std::vector<Entry> gps  = ifds[2];

    if (!exif.empty())
    {
        Entry e; e.tag = kExifIfdPointer; e.type = 4; e.count = 1; e.vals.push_back(0);
        ifd0.push_back(e);
    }
    if (!gps.empty())
    {
        Entry e; e.tag = kGpsIfdPointer; e.type = 4; e.count = 1; e.vals.push_back(0);
        ifd0.push_back(e);
    }

    struct ByTag { bool operator()(const Entry &a, const Entry &b) const { return a.tag < b.tag; } };
    std::sort(ifd0.begin(), ifd0.end(), ByTag());
    std::sort(exif.begin(), exif.end(), ByTag());
    std::sort(gps.begin(),  gps.end(),  ByTag());

    // An IFD block is: entry count (2) + entries (12 each) + next-IFD (4),
    // followed by the data area holding every value larger than 4 bytes.
    struct Local
    {
        static size_t DataSize(const std::vector<Entry> &v)
        {
            size_t n = 0;
            for (size_t i = 0; i < v.size(); i++)
            {
                size_t sz = (size_t)TypeSize(v[i].type) * v[i].count;
                if (sz > 4) n += sz + (sz & 1);          // data area entries are word-aligned
            }
            return n;
        }
        static size_t BlockSize(const std::vector<Entry> &v)
        {
            return 2 + 12 * v.size() + 4 + DataSize(v);
        }
    };

    const size_t ifd0Off = 8;                                  // right after the TIFF header
    const size_t exifOff = ifd0Off + Local::BlockSize(ifd0);
    const size_t gpsOff  = exifOff + (exif.empty() ? 0 : Local::BlockSize(exif));
    const size_t total   = gpsOff  + (gps.empty()  ? 0 : Local::BlockSize(gps));

    // Refuse rather than silently choosing which of the caller's tags to lose.
    if (6 + total > kMaxApp1Payload)
        return out;

    // Patch the pointer tags now that the offsets are known.
    for (size_t i = 0; i < ifd0.size(); i++)
    {
        if (ifd0[i].tag == kExifIfdPointer) ifd0[i].vals[0] = (uint32_t)exifOff;
        if (ifd0[i].tag == kGpsIfdPointer)  ifd0[i].vals[0] = (uint32_t)gpsOff;
    }

    std::vector<uint8_t> tiff;
    tiff.reserve(total);

    // TIFF header
    if (le) { tiff.push_back('I'); tiff.push_back('I'); }
    else    { tiff.push_back('M'); tiff.push_back('M'); }
    Put16(tiff, 42, le);
    Put32(tiff, (uint32_t)ifd0Off, le);

    // Serialise one IFD at its known absolute offset within the TIFF block.
    struct Ser
    {
        static void Emit(std::vector<uint8_t> &tiff, const std::vector<Entry> &v,
                         size_t ifdOffset, bool le)
        {
            // Values over 4 bytes live after the entry table and the next-IFD field.
            size_t dataCursor = ifdOffset + 2 + 12 * v.size() + 4;
            std::vector<uint8_t> data;

            Put16(tiff, (uint16_t)v.size(), le);
            for (size_t i = 0; i < v.size(); i++)
            {
                const Entry &e = v[i];
                Put16(tiff, e.tag,   le);
                Put16(tiff, e.type,  le);
                Put32(tiff, e.count, le);

                std::vector<uint8_t> payload;
                if (e.type == 2 || e.type == 7)
                {
                    payload = e.bytes;
                }
                else
                {
                    for (size_t k = 0; k < e.vals.size(); k++)
                    {
                        if (e.type == 1)      payload.push_back(uint8_t(e.vals[k] & 0xFF));
                        else if (e.type == 3) Put16(payload, uint16_t(e.vals[k] & 0xFFFF), le);
                        else                  Put32(payload, e.vals[k], le);
                    }
                }

                if (payload.size() > 4)
                {
                    Put32(tiff, (uint32_t)(dataCursor + data.size()), le);
                    data.insert(data.end(), payload.begin(), payload.end());
                    if (data.size() & 1) data.push_back(0);      // keep the area word-aligned
                }
                else
                {
                    payload.resize(4, 0);                        // pad the inline value field
                    tiff.insert(tiff.end(), payload.begin(), payload.end());
                }
            }
            Put32(tiff, 0, le);                                  // no next IFD
            tiff.insert(tiff.end(), data.begin(), data.end());
        }
    };

    Ser::Emit(tiff, ifd0, ifd0Off, le);
    if (!exif.empty()) Ser::Emit(tiff, exif, exifOff, le);
    if (!gps.empty())  Ser::Emit(tiff, gps,  gpsOff,  le);

    // "Exif\0\0" then the TIFF block. TIFF offsets are relative to the start
    // of the header, i.e. to the byte just past this identifier.
    static const uint8_t kId[6] = { 'E', 'x', 'i', 'f', 0, 0 };
    out.insert(out.end(), kId, kId + 6);
    out.insert(out.end(), tiff.begin(), tiff.end());
    return out;
}
