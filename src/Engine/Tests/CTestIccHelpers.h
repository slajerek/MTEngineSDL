#pragma once

#include <cstdint>
#include <vector>

// Fixture builders shared by the ICC/CMS engine tests (CTestIccProfileCodec,
// CTestIccExtract, CTestCms). They live in a header rather than each test's
// anonymous namespace because three separate test translation units need the
// same synthetic profiles, and copying them would let the copies drift.
namespace IccTestFixtures
{

// A structurally valid profile of an arbitrary size: 128-byte header whose
// size field matches the buffer, 'acsp' at offset 36, a zero tag count, and
// deterministic filler. Passes CIccProfileCodec::ValidateHeader.
//
// This is NOT openable by a real CMM -- it declares no tags, so ColorSync and
// lcms2 both reject it. Use it only for byte-level codec tests. Anything that
// reaches a CMS backend must use MakeOpenableProfileVariant() instead.
std::vector<uint8_t> MakeSyntheticProfile(uint32_t totalSize);

// A real, CMM-openable profile with a distinct content digest per index:
// ICC_BuildSRGBProfileV2() with one byte of its 'desc' text payload replaced.
// Same length, so every tag offset and size stays valid and the profile still
// opens; different bytes, so GetContentDigest() differs per variant.
//
// This is what cache/eviction tests need: variants built from the structurally
// minimal synthetic above would be refused by the backend, fail soft to
// identity, never be cached, and make an eviction test pass while exercising
// nothing.
std::vector<uint8_t> MakeOpenableProfileVariant(int variantIndex);

} // namespace IccTestFixtures
