#pragma once
#include <cstdint>
#include <vector>

// A minimal, self-contained ICC v2 matrix/TRC sRGB profile (~500-1000 bytes),
// built from published IEC 61966-2.1 colorimetry (decision #2). Deterministic:
// no time/random. Returns the raw profile bytes (NOT wrapped in an APP2 marker --
// the caller frames it with "ICC_PROFILE\0" + seq/count; spec #4.0).
std::vector<uint8_t> ICC_BuildSRGBProfileV2();

// The same minimal matrix/TRC shape with Adobe RGB (1998) primaries. Needed by
// the colour-management assumed-profile setting and by RAW preview
// inheritance. Built from published colorimetry -- no Adobe-distributed file
// is shipped -- and deterministic, so the profile ID is identical on every
// platform and the transform cache behaves the same everywhere.
std::vector<uint8_t> ICC_BuildAdobeRGBProfileV2();

// CM-E: sRGB primaries/white point with a single-gamma 2.4 TRC (u8Fixed8
// 0x0266 = 2.3984375, nearest encodable to 2.4) -- the LUT-workflow stand-in
// for Rec.709/BT.1886, used as the optional pinned video source profile.
// Deterministic, same rationale as the two builders above.
std::vector<uint8_t> ICC_BuildRec709ProfileV2();

// CM-F: Display P3 -- P3 primaries, D65 white, and the SAME single-gamma
// 0x0233 TRC approximation the sRGB builtin ships (Display P3's true transfer
// function IS the sRGB piecewise curve, so approximating it identically keeps
// sRGB<->P3 transforms through our own profiles self-consistent). Used as an
// export output space. "Display P3" is a generic space name (CSS Color 4's
// display-p3; "P3" is DCI's projector gamut), so no compatibility phrasing is
// needed. Deterministic, same rationale as the builders above.
std::vector<uint8_t> ICC_BuildDisplayP3ProfileV2();

// RD-C: linear ProPhoto (ROMM) -- the develop pipeline's working-space
// profile, handed to CM-B's 16-bit entry point as the transform SOURCE.
// UNLIKE every sibling above, ProPhoto is natively D50: the colorants are
// written UNADAPTED (no Bradford anywhere -- applying the neighbours'
// D65->D50 adaptation here is the "by-analogy double-adapt" trap RD-C #7.1
// documents, and it flips the blue colorant's sign while passing the sum
// check). Constants are the published ISO 22028-2 ROMM->XYZ(D50) matrix,
// whose column sums are exactly the ICC PCS D50 -- the "renormalised to the
// ICC PCS D50" arm of #7.1's tolerance decision, and byte-for-byte the same
// constants the pipeline's PC_kRommToXyzD50 uses, so profile and pipeline
// can never disagree. TRC: gamma 1.0, exactly representable (0x0100).
std::vector<uint8_t> ICC_BuildLinearProPhotoProfileV2();
