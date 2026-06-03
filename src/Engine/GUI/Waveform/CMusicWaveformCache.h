#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct CMusicWaveformRenderPoint
{
	float minValue = 0.0f;
	float maxValue = 0.0f;
};

struct CMusicWaveformSourceInfo
{
	uint64_t sourceFileSize = 0;
	uint64_t sourceFileMtime = 0;
	uint32_t envelopePointCount = 0;
};

// Thread safety: CMusicWaveformCache is NOT thread-safe. Concurrent access to
// the same instance must be externally synchronized. When loading asynchronously,
// ensure loading completes before reading any data (e.g. via std::atomic flag).
class CMusicWaveformCache
{
public:
	bool BuildFromInterleavedPcm16(const int16_t *samples, size_t frameCount, int channels, size_t envelopePoints);
	bool LoadFromOggFile(const std::string &filePath, size_t envelopePoints = 8192);
	bool SaveToBinaryFile(const std::string &cachePath, const CMusicWaveformSourceInfo &sourceInfo) const;
	bool LoadFromBinaryFile(const std::string &cachePath, const CMusicWaveformSourceInfo &expectedSourceInfo);
	bool BuildRenderWindow(size_t pixelWidth, float centerNorm, float zoom, std::vector<CMusicWaveformRenderPoint> &outPoints) const;
	size_t GetEnvelopePointCount() const;
	size_t GetSourceFrameCount() const;
	int GetSourceChannels() const;
	bool HasData() const;

private:
	void FillMissingBuckets(const std::vector<uint8_t> &hasData);

private:
	std::vector<float> minEnvelope;
	std::vector<float> maxEnvelope;
	size_t sourceFrameCount = 0;
	int sourceChannels = 0;
};
