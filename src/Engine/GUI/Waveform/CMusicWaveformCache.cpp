#include "CMusicWaveformCache.h"

#include "tremor-ivorbiscodec.h"
#include "tremor-ivorbisfile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{
struct SWaveformCacheFileHeader
{
	char magic[4];
	uint32_t version;
	uint64_t sourceFileSize;
	uint64_t sourceFileMtime;
	uint32_t envelopePointCount;
	uint64_t sourceFrameCount;
	uint32_t sourceChannels;
	uint32_t minEnvelopeCount;
	uint32_t maxEnvelopeCount;
};

constexpr std::array<char, 4> kWaveformCacheMagic = {'M', 'W', 'H', 'F'};
constexpr uint32_t kWaveformCacheVersion = 1;

template <typename T>
bool WriteValue(std::ofstream &out, const T &value)
{
	out.write(reinterpret_cast<const char *>(&value), sizeof(T));
	return out.good();
}

template <typename T>
bool ReadValue(std::ifstream &in, T &value)
{
	in.read(reinterpret_cast<char *>(&value), sizeof(T));
	return in.good();
}
}

bool CMusicWaveformCache::BuildFromInterleavedPcm16(const int16_t *samples, size_t frameCount, int channels, size_t envelopePoints)
{
	if (samples == nullptr || frameCount == 0 || channels <= 0 || envelopePoints == 0)
		return false;

	minEnvelope.assign(envelopePoints, 1.0f);
	maxEnvelope.assign(envelopePoints, -1.0f);
	std::vector<uint8_t> hasData(envelopePoints, 0);

	for (size_t frame = 0; frame < frameCount; frame++)
	{
		double sampleSum = 0.0;
		for (int ch = 0; ch < channels; ch++)
		{
			sampleSum += (double)samples[frame * (size_t)channels + (size_t)ch];
		}

		float mono = (float)(sampleSum / (32767.0 * (double)channels));
		mono = std::clamp(mono, -1.0f, 1.0f);

		size_t bucket = (frame * envelopePoints) / frameCount;
		if (bucket >= envelopePoints)
			bucket = envelopePoints - 1;

		minEnvelope[bucket] = std::min(minEnvelope[bucket], mono);
		maxEnvelope[bucket] = std::max(maxEnvelope[bucket], mono);
		hasData[bucket] = 1;
	}

	FillMissingBuckets(hasData);
	sourceFrameCount = frameCount;
	sourceChannels = channels;
	return true;
}

bool CMusicWaveformCache::LoadFromOggFile(const std::string &filePath, size_t envelopePoints)
{
	if (filePath.empty() || envelopePoints == 0)
		return false;

	OggVorbis_File vf;
	FILE *file = fopen(filePath.c_str(), "rb");
	if (file == nullptr)
		return false;

	if (ov_open(file, &vf, NULL, 0) != 0)
	{
		fclose(file);
		return false;
	}

	vorbis_info *info = ov_info(&vf, -1);
	int channels = info ? info->channels : 2;
	if (channels <= 0)
		channels = 2;

	ogg_int64_t totalFrames64 = ov_pcm_total(&vf, -1);
	if (totalFrames64 <= 0)
	{
		ov_clear(&vf);
		return false;
	}

	size_t totalFrames = (size_t)totalFrames64;
	minEnvelope.assign(envelopePoints, 1.0f);
	maxEnvelope.assign(envelopePoints, -1.0f);
	std::vector<uint8_t> hasData(envelopePoints, 0);

	const int kReadBytes = 8192;
	std::vector<char> readBuffer(kReadBytes);
	size_t frameCursor = 0;
	int currentSection = 0;

	while (true)
	{
		long bytesRead = ov_read(&vf, readBuffer.data(), (int)readBuffer.size(), &currentSection);
		if (bytesRead == 0)
			break;
		if (bytesRead < 0)
		{
			ov_clear(&vf);
			return false;
		}

		size_t sampleCount = (size_t)bytesRead / sizeof(int16_t);
		if (sampleCount < (size_t)channels)
			continue;

		size_t framesRead = sampleCount / (size_t)channels;
		const int16_t *pcm = (const int16_t *)readBuffer.data();

		for (size_t frame = 0; frame < framesRead && frameCursor < totalFrames; frame++, frameCursor++)
		{
			double sampleSum = 0.0;
			size_t base = frame * (size_t)channels;
			for (int ch = 0; ch < channels; ch++)
			{
				sampleSum += (double)pcm[base + (size_t)ch];
			}

			float mono = (float)(sampleSum / (32767.0 * (double)channels));
			mono = std::clamp(mono, -1.0f, 1.0f);

			size_t bucket = (frameCursor * envelopePoints) / totalFrames;
			if (bucket >= envelopePoints)
				bucket = envelopePoints - 1;

			minEnvelope[bucket] = std::min(minEnvelope[bucket], mono);
			maxEnvelope[bucket] = std::max(maxEnvelope[bucket], mono);
			hasData[bucket] = 1;
		}
	}

	ov_clear(&vf);
	FillMissingBuckets(hasData);
	sourceFrameCount = totalFrames;
	sourceChannels = channels;
	return true;
}

bool CMusicWaveformCache::SaveToBinaryFile(const std::string &cachePath, const CMusicWaveformSourceInfo &sourceInfo) const
{
	if (cachePath.empty() || !HasData())
		return false;

	std::ofstream out(cachePath, std::ios::binary | std::ios::trunc);
	if (!out.is_open())
		return false;

	SWaveformCacheFileHeader header;
	header.magic[0] = kWaveformCacheMagic[0];
	header.magic[1] = kWaveformCacheMagic[1];
	header.magic[2] = kWaveformCacheMagic[2];
	header.magic[3] = kWaveformCacheMagic[3];
	header.version = kWaveformCacheVersion;
	header.sourceFileSize = sourceInfo.sourceFileSize;
	header.sourceFileMtime = sourceInfo.sourceFileMtime;
	header.envelopePointCount = sourceInfo.envelopePointCount;
	header.sourceFrameCount = static_cast<uint64_t>(sourceFrameCount);
	header.sourceChannels = static_cast<uint32_t>(sourceChannels);
	header.minEnvelopeCount = static_cast<uint32_t>(minEnvelope.size());
	header.maxEnvelopeCount = static_cast<uint32_t>(maxEnvelope.size());

	if (!WriteValue(out, header))
		return false;

	if (!minEnvelope.empty())
	{
		out.write(reinterpret_cast<const char *>(minEnvelope.data()), static_cast<std::streamsize>(minEnvelope.size() * sizeof(float)));
		if (!out.good())
			return false;
	}

	if (!maxEnvelope.empty())
	{
		out.write(reinterpret_cast<const char *>(maxEnvelope.data()), static_cast<std::streamsize>(maxEnvelope.size() * sizeof(float)));
		if (!out.good())
			return false;
	}

	return out.good();
}

bool CMusicWaveformCache::LoadFromBinaryFile(const std::string &cachePath, const CMusicWaveformSourceInfo &expectedSourceInfo)
{
	if (cachePath.empty())
		return false;

	std::ifstream in(cachePath, std::ios::binary);
	if (!in.is_open())
		return false;

	SWaveformCacheFileHeader header;
	if (!ReadValue(in, header))
		return false;

	if (std::memcmp(header.magic, kWaveformCacheMagic.data(), kWaveformCacheMagic.size()) != 0)
		return false;
	if (header.version != kWaveformCacheVersion)
		return false;
	if (header.sourceFileSize != expectedSourceInfo.sourceFileSize)
		return false;
	if (header.sourceFileMtime != expectedSourceInfo.sourceFileMtime)
		return false;
	if (header.envelopePointCount != expectedSourceInfo.envelopePointCount)
		return false;
	if (header.minEnvelopeCount == 0 || header.maxEnvelopeCount == 0)
		return false;
	if (header.minEnvelopeCount != header.maxEnvelopeCount)
		return false;

	std::vector<float> loadedMin(header.minEnvelopeCount);
	std::vector<float> loadedMax(header.maxEnvelopeCount);

	in.read(reinterpret_cast<char *>(loadedMin.data()), static_cast<std::streamsize>(loadedMin.size() * sizeof(float)));
	if (!in.good())
		return false;
	in.read(reinterpret_cast<char *>(loadedMax.data()), static_cast<std::streamsize>(loadedMax.size() * sizeof(float)));
	if (!in.good())
		return false;

	minEnvelope = std::move(loadedMin);
	maxEnvelope = std::move(loadedMax);
	sourceFrameCount = static_cast<size_t>(header.sourceFrameCount);
	sourceChannels = static_cast<int>(header.sourceChannels);
	return HasData();
}

bool CMusicWaveformCache::BuildRenderWindow(size_t pixelWidth, float centerNorm, float zoom, std::vector<CMusicWaveformRenderPoint> &outPoints) const
{
	outPoints.clear();
	if (pixelWidth == 0 || minEnvelope.empty() || maxEnvelope.empty())
		return false;

	float z = zoom;
	if (!std::isfinite(z) || z < 1.0f)
		z = 1.0f;
	if (z > 128.0f)
		z = 128.0f;

	float span = 1.0f / z;
	if (span > 1.0f)
		span = 1.0f;

	float center = centerNorm;
	if (!std::isfinite(center))
		center = 0.5f;
	center = std::clamp(center, 0.0f, 1.0f);

	float start = center - span * 0.5f;
	if (start < 0.0f)
		start = 0.0f;
	if (start + span > 1.0f)
		start = 1.0f - span;
	if (start < 0.0f)
		start = 0.0f;

	float end = start + span;
	size_t envelopeSize = minEnvelope.size();

	outPoints.resize(pixelWidth);
	for (size_t x = 0; x < pixelWidth; x++)
	{
		float t0 = start + (span * ((float)x / (float)pixelWidth));
		float t1 = start + (span * ((float)(x + 1) / (float)pixelWidth));
		if (t1 > end)
			t1 = end;

		size_t i0 = (size_t)std::floor(t0 * (float)envelopeSize);
		size_t i1 = (size_t)std::ceil(t1 * (float)envelopeSize);
		if (i1 > 0)
			i1 -= 1;

		if (i0 >= envelopeSize)
			i0 = envelopeSize - 1;
		if (i1 >= envelopeSize)
			i1 = envelopeSize - 1;
		if (i1 < i0)
			i1 = i0;

		float minV = 1.0f;
		float maxV = -1.0f;
		for (size_t i = i0; i <= i1; i++)
		{
			minV = std::min(minV, minEnvelope[i]);
			maxV = std::max(maxV, maxEnvelope[i]);
		}

		if (minV > maxV)
			minV = maxV = 0.0f;

		outPoints[x].minValue = std::clamp(minV, -1.0f, 1.0f);
		outPoints[x].maxValue = std::clamp(maxV, -1.0f, 1.0f);
	}

	return true;
}

size_t CMusicWaveformCache::GetEnvelopePointCount() const
{
	return minEnvelope.size();
}

size_t CMusicWaveformCache::GetSourceFrameCount() const
{
	return sourceFrameCount;
}

int CMusicWaveformCache::GetSourceChannels() const
{
	return sourceChannels;
}

bool CMusicWaveformCache::HasData() const
{
	return !minEnvelope.empty() && !maxEnvelope.empty() && sourceFrameCount > 0;
}

void CMusicWaveformCache::FillMissingBuckets(const std::vector<uint8_t> &hasData)
{
	if (hasData.empty())
		return;

	size_t firstValid = hasData.size();
	for (size_t i = 0; i < hasData.size(); i++)
	{
		if (hasData[i])
		{
			firstValid = i;
			break;
		}
	}

	if (firstValid == hasData.size())
	{
		for (size_t i = 0; i < hasData.size(); i++)
		{
			minEnvelope[i] = 0.0f;
			maxEnvelope[i] = 0.0f;
		}
		return;
	}

	for (size_t i = 0; i < firstValid; i++)
	{
		minEnvelope[i] = minEnvelope[firstValid];
		maxEnvelope[i] = maxEnvelope[firstValid];
	}

	size_t prevValid = firstValid;
	for (size_t i = firstValid + 1; i < hasData.size(); i++)
	{
		if (hasData[i])
		{
			for (size_t fill = prevValid + 1; fill < i; fill++)
			{
				minEnvelope[fill] = minEnvelope[prevValid];
				maxEnvelope[fill] = maxEnvelope[prevValid];
			}
			prevValid = i;
		}
	}

	for (size_t i = prevValid + 1; i < hasData.size(); i++)
	{
		minEnvelope[i] = minEnvelope[prevValid];
		maxEnvelope[i] = maxEnvelope[prevValid];
	}
}
