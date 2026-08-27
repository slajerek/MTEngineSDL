#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Assembles a valid Exif APP1 payload for embedding in a JPEG via
// JPEGWriter::writeMarker(0xE1, ...).
//
// Knows nothing about any application's metadata model: callers set tags by
// number and type; this class handles the TIFF header, IFD layout, the
// value-vs-offset packing rule, offset fixups and endianness.
//
// Deliberately construct-only. It never parses, patches or round-trips an
// existing block -- that is where EXIF writers become unbounded. There is no
// thumbnail IFD, no IFD1 and no MakerNote.
class CExifBuilder
{
public:
    enum class Ifd { Primary = 0, Exif = 1, Gps = 2 };   // IFD0 / ExifIFD / GPSIFD

    void SetAscii     (Ifd ifd, uint16_t tag, const std::string &value);
    void SetByte      (Ifd ifd, uint16_t tag, uint8_t value);
    void SetShort     (Ifd ifd, uint16_t tag, uint16_t value);
    void SetLong      (Ifd ifd, uint16_t tag, uint32_t value);
    void SetRational  (Ifd ifd, uint16_t tag, uint32_t num, uint32_t den);
    void SetSRational (Ifd ifd, uint16_t tag, int32_t num, int32_t den);

    // Multi-value RATIONAL. Required for GPSLatitude / GPSLongitude /
    // GPSTimeStamp, which are count 3 (degrees, minutes, seconds). Without
    // this, GPS cannot be expressed at all.
    void SetRationalArray(Ifd ifd, uint16_t tag,
                          const std::vector<std::pair<uint32_t, uint32_t>> &values);

    // TIFF type 7 (UNDEFINED) -- ExifVersion (0x9000) and friends.
    void SetUndefined (Ifd ifd, uint16_t tag, const uint8_t *data, size_t len);

    // Tags refused because the value could not be represented. A refused tag
    // is never half-written: it is absent from the output and listed here.
    // Triggers: a zero denominator, and an ASCII value containing an interior
    // NUL (which would truncate the field for every reader).
    //
    // NOT a trigger: non-ASCII bytes in an ASCII field. EXIF types Artist and
    // Copyright as 7-bit ASCII, but dropping "Jose Garcia" or a (c) symbol
    // would destroy exactly the attribution such fields exist to carry, and
    // would not round-trip our own reader, which treats them as bytes. They
    // are written as given.
    const std::vector<uint16_t> &Dropped() const { return dropped; }

    bool IsEmpty() const;

    // Complete APP1 payload: the "Exif\0\0" identifier (45 78 69 66 00 00)
    // followed by the TIFF block. Empty when no tags were set, or when the
    // payload would exceed what a JPEG APP marker can carry -- the builder
    // does not choose which of the caller's tags to sacrifice.
    std::vector<uint8_t> BuildApp1(bool littleEndian = true) const;

    // Largest APP1 payload a JPEG marker segment can hold: libjpeg caps a
    // marker at 65533 bytes and the identifier costs six of them.
    static constexpr size_t kMaxApp1Payload = 65533;

private:
    struct Entry
    {
        uint16_t tag   = 0;
        uint16_t type  = 0;   // TIFF type: 1 BYTE, 2 ASCII, 3 SHORT, 4 LONG,
                              // 5 RATIONAL, 7 UNDEFINED, 10 SRATIONAL
        uint32_t count = 0;
        std::vector<uint32_t> vals;    // numeric components (rationals: num,den,...)
        std::vector<uint8_t>  bytes;   // ASCII / UNDEFINED payload
    };

    std::vector<Entry>    ifds[3];
    std::vector<uint16_t> dropped;

    void Put(Ifd ifd, Entry &&e);      // replaces an existing tag in that IFD
    void Drop(uint16_t tag);           // record a refused tag, once
};
