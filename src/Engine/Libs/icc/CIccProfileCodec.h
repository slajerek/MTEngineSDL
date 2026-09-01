#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Byte-level ICC profile plumbing: structural validation, identity, and the
// container framings profiles arrive in (JPEG APP2 segments, PNG iCCP chunks).
//
// Deliberately knows nothing about colour. Opening a profile, building a
// transform and converting pixels belong to the CMS backends (Engine/Color);
// this file only decides whether a byte range is a plausible profile, what to
// call it, and how to get it in and out of a file container.
class CIccProfileCodec
{
public:
    // Largest profile payload we put in one APP2 segment. A JPEG marker's
    // length field covers at most 65535 bytes including the 2 length bytes,
    // and each ICC segment spends 12 on "ICC_PROFILE\0" plus 2 on seq/count,
    // leaving 65519. We use a slightly smaller round number, as common writers
    // do, so the segment stays comfortably inside every decoder's limits.
    static const uint32_t kMaxApp2ProfileBytes = 65517;

    // Structural sanity of a 128-byte ICC header plus its tag table:
    //   - size >= 128
    //   - header profile-size field (bytes 0..3, big-endian) in [128, size]
    //   - 'acsp' signature at offset 36
    //   - tag count at offset 128, and every (offset, size) entry of the
    //     12-byte tag table lies inside the declared profile size
    // This is what keeps a truncated or hostile profile out of the CMM, where
    // it is a crash rather than a colour bug. Profiles arrive from untrusted
    // files, so every caller validates before opening.
    static bool ValidateHeader(const uint8_t *bytes, uint32_t size);

    // ICC.1 clause 7.2.18 profile ID: header bytes 84..99 when any of them is
    // non-zero, otherwise an MD5 computed over the profile with the ID field,
    // the rendering-intent field (64..67) and the flags field (44..47) zeroed.
    //
    // INFORMATIONAL ONLY. Never key a cache on this and never decide identity
    // with it: the header ID field is just bytes in an untrusted file, and this
    // function hands them back verbatim when set. Use GetContentDigest for any
    // decision. See its comment for what goes wrong otherwise.
    static void GetProfileId(const uint8_t *bytes, uint32_t size, uint8_t outId[16]);

    // MD5 over the profile bytes exactly as they arrived: no field zeroing, no
    // header shortcut. This is what keys the transform cache and decides the
    // identity fast path.
    //
    // The distinction from GetProfileId is a trust boundary, not a detail. A
    // crafted file can carry any profile ID it likes -- including the one
    // belonging to the user's display profile. Keyed on that, the file would
    // test as identity and skip conversion entirely, or collide with an
    // unrelated cached transform and be converted through the wrong pair.
    // Hashing the actual bytes cannot be spoofed that way.
    //
    // Distinct bytes give distinct digests; byte-identical profiles share one,
    // which is the case that matters (a folder of camera JPEGs all carrying the
    // same embedded sRGB profile shares a single transform).
    //
    // Cost is MD5 over the profile, so memoise it per image rather than
    // recomputing per transform; real profiles are a few KB.
    static void GetContentDigest(const uint8_t *bytes, uint32_t size, uint8_t outDigest[16]);

    // Split a profile into JPEG APP2 payloads: each is "ICC_PROFILE\0" (12
    // bytes) + a 1-based sequence number + the total count + up to
    // kMaxApp2ProfileBytes profile bytes. The single-segment result is
    // byte-identical to the framing the photo app's exporter already writes.
    // Returns an empty vector for empty input or for a profile too large to
    // number in one byte (255 segments).
    static std::vector<std::vector<uint8_t> > SplitApp2(const uint8_t *profile, uint32_t size);

    // Reassemble a profile from APP2 payloads. Input is every APP2 segment
    // found in the file, in file order, each starting at the byte after the
    // marker's 2-byte length field; segments that do not carry the
    // "ICC_PROFILE\0" identifier are ignored, so passing unrelated APP2s is
    // harmless.
    //
    // Returns empty -- never a partial profile -- when the set is not exactly
    // one complete sequence: no ICC segments at all, disagreeing count bytes,
    // a duplicate or out-of-range sequence number, a missing segment, or a
    // result that fails ValidateHeader. A partial profile handed to a CMM is a
    // crash risk, so a gap must yield nothing rather than a prefix.
    // Out-of-order segments are fine: they are placed by sequence number.
    static std::vector<uint8_t> JoinApp2(const std::vector<std::vector<uint8_t> > &segments);

    // Decode a PNG iCCP chunk payload into profile bytes. The payload is a
    // null-terminated profile name, one compression-method byte (only 0,
    // zlib/deflate, is defined) and the deflate stream.
    //
    // Output is capped at maxOut because this is untrusted input and a small
    // chunk can inflate without bound; real profiles top out around 4 MB.
    // Returns empty on a bad name field, an unknown compression method, a
    // broken stream, output exceeding the cap, or a result that fails
    // ValidateHeader.
    static std::vector<uint8_t> InflateIccp(const uint8_t *payload, uint32_t size,
                                            uint32_t maxOut = 16u * 1024u * 1024u);

    // The profile's human-readable name, from its 'desc' tag. This is what a
    // colour-management settings pane shows for "the display profile actually
    // in effect" -- a panel that cannot name its own choice is a support
    // burden, and the bytes are the only place the name exists.
    //
    // Handles both encodings in the wild: 'desc' (textDescriptionType, ICC v2 --
    // an ASCII string with its own length field) and 'mluc' (ICC v4 -- UTF-16BE
    // records per language, of which the first is returned, transcoded to
    // UTF-8). Returns "" when the tag is absent, empty, or malformed.
    //
    // Bounds-checked against `size` at every step and never throws: profiles
    // come from untrusted files, same posture as ValidateHeader. Callers should
    // still ValidateHeader first; this function does not assume they did.
    static std::string GetDescription(const uint8_t *bytes, uint32_t size);
};
