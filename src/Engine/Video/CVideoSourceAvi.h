#ifndef _CVIDEOSOURCEAVI_H_
#define _CVIDEOSOURCEAVI_H_

#pragma once

// THE AVI LANE. RIFF demuxed here; every codec decoded by the OS.
//
// WHY IT EXISTS, and why the reason is not what it looked like. AVI was
// written off as "an accepted store-build loss -- it carries MPEG-4 Part 2".
// That premise was never measured and it is false: VideoToolbox decodes
// MPEG-4 Part 2 and MJPEG, and AudioToolbox decodes MP3, MP2 and AC-3. So
// there was never a licensing problem and never a missing decoder -- what was
// missing was a demuxer, which is the one part no licence covers, because
// RIFF is a file format. Same argument that opened Matroska, one container
// further on.
//
//   video: MPEG-4 Part 2 (DivX/XviD/DX50/FMP4), MJPEG, H.264  -> IVideoPacketDecoder
//   audio: MP3, MP2, AC-3 -> CAudioDecoderCompressedNative; PCM -> straight through
//
// WHAT IT DELIBERATELY DOES NOT DO:
//   * MS-MPEG4 v3 (DIV3/MP43, "DivX 3"). MEASURED: VideoToolbox answers
//     kVTCouldNotFindVideoDecoderErr. There is no OS decoder, so the lane
//     REFUSES the file and says which codec it was, rather than showing black.
//   * OpenDML (AVI 2.0, files over 2 GB, `indx`/`ix##` super-indices). The
//     first RIFF chunk plays; later ones are not walked. Refusing the whole
//     file would be worse, so it plays what it can and logs the rest.
//   * H.264-in-AVI usually stores Annex-B with no `avcC`, which VideoToolbox
//     will not take. Handled as a refusal with a named reason, not a guess.
//
// The lane is tried AFTER the OS tier, so on Windows -- where Media
// Foundation's own AVI source may well open the file -- the OS gets first
// refusal and this is the fallback.

#include "IVideoSource.h"
#include "MT_MediaPacket.h"
#include "CAudioDecoderCompressedNative.h"
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class IVideoPacketDecoder;

class CVideoSourceAvi : public IVideoSource
{
public:
	CVideoSourceAvi();
	virtual ~CVideoSourceAvi();

	// Extension-based, like every other probe in the chain: opening the file
	// to find out is what the folder scanner would pay per row.
	static bool CanProbablyOpen(const char *filePath);

	bool Open(const char *filePath) override;
	const SVideoInfo &Info() const override { return info; }
	bool ReadVideoFrame(SDecodedVideoFrame &out) override;
	bool ReadAudio(SDecodedAudio &out) override;
	bool Seek(double seconds) override;
	const std::string &GetErrorReason() const override { return errorReason; }
	void Close() override;

	void SetAbortPredicate(std::function<bool()> pred) override;
	void WakeAbort() override;

private:
	// One chunk of the `movi` list, as the index describes it.
	struct SIndexEntry
	{
		u32 offset = 0;      // absolute file offset of the chunk's DATA
		u32 size = 0;
		bool keyframe = false;
		u32 frameNumber = 0; // video frames only: position in presentation order
	};

	bool FailOpen(std::string reason);
	bool ParseHeaders();
	bool ParseStreamList(long listEnd, int streamIndex);
	bool ReadIdx1(long idx1Pos, u32 idx1Size);
	// AVI writers put the MPEG-4 configuration in one of two places and both
	// are ordinary: as trailing bytes of the BITMAPINFOHEADER, or IN-BAND at
	// the head of the first video chunk. This reads the first chunk so the
	// second case works; it does not consume it.
	bool PeekFirstVideoChunk(std::vector<u8> &out);
	bool InitVideoDecoder();
	bool InitAudioDecoder();

	// Pull the next chunk belonging to a stream we care about. Audio is
	// decoded and queued on the way past, exactly as the Matroska lane does.
	// Returns false at end of `movi` or on a read error.
	bool ReadNextVideoChunk(std::vector<u8> &out, bool &keyframe, double &pts);
	void DecodeAudioChunk(const u8 *data, size_t size);

	FILE *file = nullptr;
	SVideoInfo info;
	std::string errorReason;

	// --- stream layout ---
	int videoStreamId = -1;
	int audioStreamId = -1;
	EMediaCodecId videoCodec = EMediaCodecId::Unknown;
	EMediaCodecId audioCodec = EMediaCodecId::Unknown;
	std::vector<u8> videoCodecConfig;   // BITMAPINFOHEADER tail: the VOL header
	u32 audioSampleRate = 0;
	int audioChannels = 0;
	int audioBitsPerSample = 0;
	double videoFrameDuration = 0.0;    // seconds; AVI video is constant-rate

	long moviStart = 0;                 // file offset of the first chunk header
	long moviEnd = 0;
	long readPos = 0;                   // where ReadNextVideoChunk resumes

	std::vector<SIndexEntry> videoIndex; // keyframes only -- all Seek() needs
	u32 nextVideoFrameNumber = 0;

	// --- decoders ---
	std::unique_ptr<IVideoPacketDecoder> packetDecoder;
#ifdef MT_HAVE_NATIVE_COMPRESSED_AUDIO
	std::unique_ptr<CAudioDecoderCompressedNative> audioDecoder;
#endif
	std::function<bool()> abortPredicate;

	std::vector<u8> chunkScratch;
	std::vector<u8> annexBParameterSets;
	std::vector<u8> annexBScratch;
	int nalLengthSize = 4;
	bool wantsAnnexB = false;

	std::deque<SDecodedAudio> pendingAudio;
	u64 audioFramesEmitted = 0;         // for the audio pts, in output frames
};

#endif
//_CVIDEOSOURCEAVI_H_
