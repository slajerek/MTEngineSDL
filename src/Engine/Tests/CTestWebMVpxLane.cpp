#include "CTestWebMVpxLane.h"
#include "SYS_Defs.h"
#include "DBG_Log.h"
#include <cstdio>
#include <string>

#define ASSERT_TRUE(cond, msg) do { if (!(cond)) { TestCompleted(false, (msg)); return; } } while (0)

CTestWebMVpxLane::CTestWebMVpxLane()  {}
CTestWebMVpxLane::~CTestWebMVpxLane() {}

#if MT_ENABLE_WEBM_VPX
#include "CVideoSourceWebMVpx.h"
#endif
#include "MT_AnnexB.h"
#include <vector>

void CTestWebMVpxLane::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;

#if !MT_ENABLE_WEBM_VPX
	TestSkipped("MT_ENABLE_WEBM_VPX is off in this build");
	return;
#else
	int stepNum = 1;
	char msg[224];

	std::string path = CTest::ResolveProjectPath("tests/fixtures/video/vp9_opus.webm");
	FILE *probe = fopen(path.c_str(), "rb");
	if (probe == NULL)
	{
		TestSkipped("this app ships no tests/fixtures/video/vp9_opus.webm");
		return;
	}
	fclose(probe);

	CVideoSourceWebMVpx *src = new CVideoSourceWebMVpx();
	bool opened = src->Open(path.c_str());
	if (!opened)
	{
		snprintf(msg, sizeof(msg), "FAIL: the WebM lane could not open a VP9+Opus file: %s",
				 src->GetErrorReason().c_str());
		LOGError("CTestWebMVpxLane: %s", msg);
		delete src;
		TestCompleted(false, msg);
		return;
	}
	StepCompleted(stepNum++, true, "the WebM lane opened a VP9 + Opus file");

	const SVideoInfo &info = src->Info();
	snprintf(msg, sizeof(msg), "video: %dx%d, %.2f fps, %.2fs",
			 info.width, info.height, info.fps, info.duration);
	if (info.width != 320 || info.height != 240)
	{
		delete src;
		TestCompleted(false, msg);
		return;
	}
	StepCompleted(stepNum++, true, msg);

	// Pull frames and audio. Video is known to work (the alpha fixture proves
	// it); AUDIO is the open question this test exists for.
	int videoFrames = 0;
	int audioBlocks = 0;
	u64 audioSamples = 0;
	u32 audioRate = 0;
	int audioChannels = 0;

	for (int i = 0; i < 400; i++)
	{
		SDecodedVideoFrame vf;
		if (src->ReadVideoFrame(vf))
			videoFrames++;

		SDecodedAudio af;
		if (src->ReadAudio(af))
		{
			audioBlocks++;
			audioSamples += af.pcm.size();
			if (audioRate == 0) { audioRate = af.sampleRate; audioChannels = af.channels; }
		}
		if (videoFrames >= 50 && audioBlocks > 0)
			break;
	}

	snprintf(msg, sizeof(msg), "decoded %d video frames", videoFrames);
	if (videoFrames < 10)
	{
		delete src;
		TestCompleted(false, msg);
		return;
	}
	StepCompleted(stepNum++, true, msg);

	// THE MEASUREMENT. A zero here is a finding, not a bug in the test: it means
	// tracker item 1.8 is a feature and not a routing change.
	snprintf(msg, sizeof(msg), "audio: %d blocks, %llu samples, %u Hz, %d ch",
			 audioBlocks, (unsigned long long)audioSamples,
			 (unsigned)audioRate, audioChannels);
	if (audioBlocks == 0)
	{
		LOGError("CTestWebMVpxLane: the WebM lane produced NO AUDIO for a VP9+Opus file -- "
				 "tracker 1.8 is a feature, not a routing change");
		delete src;
		TestCompleted(false, "FAIL: the WebM lane decoded video but produced NO audio for VP9+Opus");
		return;
	}
	StepCompleted(stepNum++, true, msg);

	delete src;
	src = nullptr;

	// ========================================================================
	// VP8 + Vorbis -- the most permissively licensed video file that exists,
	// and until 2026-09-10 this engine could not play it without FFmpeg
	// ========================================================================
	//
	// Tracker 2.11. libvpx was configured `--disable-vp8-decoder` and the lane
	// accepted only NESTEGG_CODEC_VP9/OPUS, so a VP8+Vorbis WebM -- BSD-3 code
	// with a royalty-free patent grant on one side and Xiph's own codec on the
	// other -- fell through to FFmpeg, i.e. did not play in a store build at
	// all. Nothing about that was a licensing requirement; it was a build flag
	// and two `==` comparisons.
	std::string vp8Path = CTest::ResolveProjectPath("tests/fixtures/video/vp8_vorbis.webm");
	FILE *vp8Probe = fopen(vp8Path.c_str(), "rb");
	if (vp8Probe != NULL)
	{
		fclose(vp8Probe);
		CVideoSourceWebMVpx *vp8 = new CVideoSourceWebMVpx();
		if (!vp8->Open(vp8Path.c_str()))
		{
			snprintf(msg, sizeof(msg), "FAIL: the lane could not open VP8+Vorbis: %s",
					 vp8->GetErrorReason().c_str());
			delete vp8;
			TestCompleted(false, msg);
			return;
		}
		const SVideoInfo &vi = vp8->Info();
		snprintf(msg, sizeof(msg), "vp8: %s %dx%d, audio '%s' %u Hz",
				 vi.videoCodecName.c_str(), vi.width, vi.height,
				 vi.audioCodecName.c_str(), vi.audioSampleRate);
		if (vi.videoCodecName != "vp8" || vi.audioCodecName != "vorbis")
		{
			delete vp8;
			TestCompleted(false, msg);
			return;
		}
		StepCompleted(stepNum++, true, msg);

		int vp8Frames = 0, vorbisBlocks = 0;
		u64 vorbisSamples = 0;
		for (int i = 0; i < 400 && vp8Frames < 20; i++)
		{
			SDecodedVideoFrame vf;
			if (vp8->ReadVideoFrame(vf))
			{
				vp8Frames++;
				// A VP8 frame with no plane would mean libvpx accepted the
				// stream and produced nothing -- exactly what a frame count
				// alone would miss.
				if (vf.plane[0] == NULL || vf.width != 320)
				{
					delete vp8;
					TestCompleted(false, "the lane returned a VP8 frame with no picture");
					return;
				}
			}
			SDecodedAudio af;
			if (vp8->ReadAudio(af))
			{
				vorbisBlocks++;
				vorbisSamples += af.pcm.size();
			}
		}
		snprintf(msg, sizeof(msg), "vp8: %d frames, vorbis: %d blocks / %llu samples",
				 vp8Frames, vorbisBlocks, (unsigned long long)vorbisSamples);
		if (vp8Frames < 10)
		{
			delete vp8;
			TestCompleted(false, msg);
			return;
		}
		// Vorbis is a SEPARATE claim: Tremor is driven through its low-level
		// synthesis API here rather than ov_read, and a wrong setup-header
		// sequence yields silence rather than an error.
		if (vorbisBlocks == 0 || vorbisSamples == 0)
		{
			delete vp8;
			TestCompleted(false, "the lane decoded VP8 but produced NO Vorbis audio");
			return;
		}
		StepCompleted(stepNum++, true, msg);
		delete vp8;
	}

	// ========================================================================
	// MT_AnnexB -- the conversion Windows needs and macOS cannot exercise
	// ========================================================================
	//
	// Tested HERE, on the platform that does NOT use it. VideoToolbox takes
	// the container's length-prefixed layout directly, so nothing on this
	// machine would ever call this code; Media Foundation is the only consumer
	// and it cannot be compiled here. Shipping the conversion untested because
	// its consumer lives on another machine is exactly how the Windows build
	// has been broken twice in this programme.
	{
		// A minimal but REAL avcC: version 1, profile/compat/level, 0xFF ->
		// nalLengthSize 4, one 5-byte SPS, one 4-byte PPS.
		const u8 avcC[] = {
			0x01, 0x64, 0x00, 0x1F, 0xFF,
			0xE1, 0x00, 0x05, 0x67, 0x64, 0x00, 0x1F, 0xAC,   // 1 SPS
			0x01, 0x00, 0x04, 0x68, 0xEE, 0x3C, 0xB0,          // 1 PPS
		};
		std::vector<u8> ps;
		int lengthSize = 0;
		ASSERT_TRUE(MT_AnnexB::ParseConfigRecord(avcC, sizeof(avcC), false, ps, lengthSize),
					"MT_AnnexB parses a well-formed avcC");
		ASSERT_TRUE(lengthSize == 4, "avcC 0xFF means a 4-byte NAL length prefix");
		// 4 + 5 + 4 + 4 = 17 bytes: two start codes and two parameter sets.
		snprintf(msg, sizeof(msg), "avcC -> %zu bytes of Annex-B parameter sets", ps.size());
		ASSERT_TRUE(ps.size() == 17, msg);
		ASSERT_TRUE(ps[0] == 0 && ps[1] == 0 && ps[2] == 0 && ps[3] == 1 && ps[4] == 0x67,
					"the SPS is start-code prefixed and unaltered");
		ASSERT_TRUE(ps[9] == 0 && ps[12] == 1 && ps[13] == 0x68,
					"the PPS follows its own start code");
		StepCompleted(stepNum++, true, msg);

		// One sample carrying two NALs, 4-byte prefixes.
		const u8 sample[] = {
			0x00, 0x00, 0x00, 0x02, 0x41, 0x9A,
			0x00, 0x00, 0x00, 0x03, 0x41, 0x9B, 0x9C,
		};
		std::vector<u8> ab;
		ASSERT_TRUE(MT_AnnexB::ConvertPacket(sample, sizeof(sample), 4, ab),
					"MT_AnnexB converts a two-NAL sample");
		ASSERT_TRUE(ab.size() == 4 + 2 + 4 + 3, "each length prefix became a 4-byte start code");
		ASSERT_TRUE(ab[3] == 1 && ab[4] == 0x41 && ab[9] == 1 && ab[10] == 0x41,
					"both NAL payloads survive the conversion byte-for-byte");

		// A LENGTH THAT OVERRUNS THE PACKET MUST BE REFUSED. Feeding a decoder
		// half a NAL is how a crash gets blamed on the decoder.
		const u8 truncated[] = { 0x00, 0x00, 0x00, 0x40, 0x41, 0x9A };
		std::vector<u8> bad;
		ASSERT_TRUE(!MT_AnnexB::ConvertPacket(truncated, sizeof(truncated), 4, bad),
					"a NAL length that overruns the packet is refused, not truncated");
		StepCompleted(stepNum++, true, "MT_AnnexB: conversion, and refusal of a corrupt sample");
	}

	// ========================================================================
	// THE MATROSKA LANE -- .mkv with H.264 + AAC, no FFmpeg anywhere near it
	// ========================================================================
	//
	// This is tracker 3.10: before it, a store build on macOS could not play
	// .mkv at all, because AVFoundation does not read Matroska and the only
	// thing that did was FFmpeg. nestegg was already vendored and already
	// parsed these tracks; the lane was VP9+Opus by OUR choice.
	std::string mkvPath = CTest::ResolveProjectPath("tests/fixtures/video/h264_aac.mkv");
	FILE *mkvProbe = fopen(mkvPath.c_str(), "rb");
	if (mkvProbe == NULL)
	{
		TestCompleted(true, "the WebM lane carries VP9 + Opus; no .mkv fixture in this app");
		return;
	}
	fclose(mkvProbe);

	ASSERT_TRUE(CVideoSourceWebMVpx::CanProbablyOpen(mkvPath.c_str()),
				"the lane claims .mkv");

#if !defined(__APPLE__) && !defined(_WIN32)
	// LINUX HAS NO OS VIDEO DECODER to route H.264 to, so the lane must REFUSE
	// this file -- and that refusal is asserted rather than skipped, because it
	// is what lets CVideoPlayer's chain fall through to FFmpeg in a private
	// build. A lane that opened the file and showed black would pass a skip.
	src = new CVideoSourceWebMVpx();
	const bool openedOnLinux = src->Open(mkvPath.c_str());
	snprintf(msg, sizeof(msg), "no OS decoder here, so the lane refuses H.264 .mkv: %s",
			 src->GetErrorReason().c_str());
	delete src;
	src = nullptr;
	ASSERT_TRUE(!openedOnLinux, "H.264 .mkv must be REFUSED where no OS decoder exists, not opened black");
	StepCompleted(stepNum++, true, msg);
	TestCompleted(true, "the Matroska lane plays VP9+Opus; H.264 .mkv correctly refused on this platform");
	return;
#else
	src = new CVideoSourceWebMVpx();
	if (!src->Open(mkvPath.c_str()))
	{
		snprintf(msg, sizeof(msg), "FAIL: the Matroska lane could not open H.264+AAC: %s",
				 src->GetErrorReason().c_str());
		delete src;
		TestCompleted(false, msg);
		return;
	}
	const SVideoInfo &mkvInfo = src->Info();
	snprintf(msg, sizeof(msg), "mkv: %s %dx%d, audio '%s' %u Hz",
			 mkvInfo.videoCodecName.c_str(), mkvInfo.width, mkvInfo.height,
			 mkvInfo.audioCodecName.c_str(), mkvInfo.audioSampleRate);
	if (mkvInfo.videoCodecName != "h264" || mkvInfo.width != 64 || mkvInfo.height != 64)
	{
		delete src;
		TestCompleted(false, msg);
		return;
	}
	StepCompleted(stepNum++, true, msg);

	int mkvFrames = 0, mkvAudio = 0;
	u32 mkvRate = 0;
	for (int i = 0; i < 400 && mkvFrames < 20; i++)
	{
		SDecodedVideoFrame vf;
		if (src->ReadVideoFrame(vf))
		{
			mkvFrames++;
			// A frame that decoded but carries no plane is the failure mode a
			// frame COUNT alone would not catch.
			if (vf.plane[0] == NULL || vf.width != 64)
			{
				delete src;
				TestCompleted(false, "the Matroska lane returned a frame with no picture");
				return;
			}
		}
		SDecodedAudio af;
		if (src->ReadAudio(af))
		{
			mkvAudio++;
			if (mkvRate == 0) mkvRate = af.sampleRate;
		}
	}
	snprintf(msg, sizeof(msg), "mkv: %d H.264 frames decoded by the OS, %d AAC blocks at %u Hz",
			 mkvFrames, mkvAudio, mkvRate);
	if (mkvFrames < 10)
	{
		delete src;
		TestCompleted(false, msg);
		return;
	}
	StepCompleted(stepNum++, true, msg);

	// AUDIO IS A SEPARATE CLAIM from video, and worth failing on separately:
	// AAC-in-Matroska is the case whose AudioSpecificConfig comes from the
	// container rather than from an ADTS header, and getting it wrong yields
	// silence rather than an error.
	if (mkvAudio == 0)
	{
		delete src;
		TestCompleted(false, "the Matroska lane decoded H.264 but produced NO AAC audio");
		return;
	}
	if (mkvRate != 44100)
	{
		snprintf(msg, sizeof(msg), "mkv audio rate %u, expected 44100", mkvRate);
		delete src;
		TestCompleted(false, msg);
		return;
	}
	StepCompleted(stepNum++, true, "mkv: AAC decoded through the OS at the fixture's own rate");

	// Seek, then decode again: the reorder queue and the AAC converter both
	// carry state that describes the position BEFORE the seek.
	if (src->Seek(1.0))
	{
		int afterSeek = 0;
		for (int i = 0; i < 200 && afterSeek < 3; i++)
		{
			SDecodedVideoFrame vf;
			if (src->ReadVideoFrame(vf))
				afterSeek++;
		}
		snprintf(msg, sizeof(msg), "mkv: %d frames after a seek to 1.0s", afterSeek);
		if (afterSeek == 0)
		{
			delete src;
			TestCompleted(false, msg);
			return;
		}
		StepCompleted(stepNum++, true, msg);
	}

	delete src;
	TestCompleted(true, "the Matroska lane plays VP9+Opus and H.264+AAC, with no FFmpeg");
#endif // __APPLE__ || _WIN32
#endif // MT_ENABLE_WEBM_VPX
}

void CTestWebMVpxLane::Cancel()
{
	isRunning = false;
}
