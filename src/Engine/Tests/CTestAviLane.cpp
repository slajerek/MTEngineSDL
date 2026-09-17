#include "CTestAviLane.h"
#include "CVideoSourceAvi.h"
#include "MT_AudioFrames.h"
#include "MT_Mp4vEsds.h"
#include "SYS_Defs.h"
#include "DBG_Log.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define ASSERT_TRUE(cond, msg) do { if (!(cond)) { TestCompleted(false, (msg)); return; } } while (0)

CTestAviLane::CTestAviLane()  {}
CTestAviLane::~CTestAviLane() {}

namespace
{
	// A frame that decoded but carries no picture is the failure a frame COUNT
	// alone would miss, so every case checks the plane and its luma.
	bool FrameHasPicture(const SDecodedVideoFrame &f, int expectWidth)
	{
		if (f.plane[0] == NULL || f.width != expectWidth || f.height <= 0)
			return false;
		long sum = 0;
		for (int y = 0; y < f.height; y++)
			for (int x = 0; x < f.width; x++)
				sum += f.plane[0][(size_t)y * (size_t)f.stride[0] + (size_t)x];
		return sum > 0;
	}
}

void CTestAviLane::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;

	int stepNum = 1;
	char msg[256];

	// ========================================================================
	// The two byte-level helpers the lane is built on, tested first because a
	// failure here explains every failure below.
	// ========================================================================

	// MT_Mp4vEsds: the wrapper VideoToolbox needs around the VOL header AVI
	// carries. MEASURED against a real file: ffmpeg's esds for this content is
	// 88 bytes, and 47 bytes of VOL is what produces it.
	{
		std::vector<u8> vol(47, 0);
		vol[0] = 0x00; vol[1] = 0x00; vol[2] = 0x01; vol[3] = 0xB0;
		std::vector<u8> esds = MT_Mp4vEsds::Build(vol.data(), vol.size());
		snprintf(msg, sizeof(msg), "esds built from a 47-byte VOL is %zu bytes (88 expected)", esds.size());
		ASSERT_TRUE(esds.size() == 88, msg);
		// Structure, not just length: full-box header, then ES_Descr (0x03).
		ASSERT_TRUE(esds[0] == 0 && esds[1] == 0 && esds[2] == 0 && esds[3] == 0,
					"the esds begins with a zero version/flags word");
		ASSERT_TRUE(esds[4] == 0x03, "then the ES_Descriptor tag");
		ASSERT_TRUE(MT_Mp4vEsds::Build(NULL, 0).empty(),
					"no VOL means no esds, not an empty-but-valid one");
		StepCompleted(stepNum++, true, msg);
	}

	// MT_AudioFrames: the frame splitter. AVI packs several MP3 frames into one
	// chunk and the OS decoders take one at a time, so getting this wrong
	// desynchronises audio gradually rather than failing.
	{
		// MPEG-1 Layer III, 128 kbps, 44100 Hz, no padding -> 417 bytes.
		const u8 mp3[4] = { 0xFF, 0xFB, 0x90, 0x00 };
		size_t sz = MT_AudioFrames::MpegFrameSize(mp3, 417);
		snprintf(msg, sizeof(msg), "MP3 128kbps/44100 frame is %zu bytes (417 expected)", sz);
		ASSERT_TRUE(sz == 417, msg);
		// The padding bit adds exactly one byte, and mis-handling it is the
		// classic one-byte-per-frame drift.
		const u8 mp3pad[4] = { 0xFF, 0xFB, 0x92, 0x00 };
		ASSERT_TRUE(MT_AudioFrames::MpegFrameSize(mp3pad, 418) == 418,
					"the padding bit adds exactly one byte");
		// A frame that does not fit must report 0 so the caller carries it.
		ASSERT_TRUE(MT_AudioFrames::MpegFrameSize(mp3, 100) == 0,
					"a frame longer than the buffer reports 0, so the caller carries it");
		const u8 junk[8] = { 1,2,3,4,5,6,7,8 };
		ASSERT_TRUE(MT_AudioFrames::MpegFrameSize(junk, 8) == 0, "junk is not a frame");
		// Resync: a chunk that starts mid-frame is ordinary AVI, not corruption.
		u8 mixed[8] = { 0x00, 0x11, 0x22, 0x33, 0xFF, 0xFB, 0x90, 0x00 };
		ASSERT_TRUE(MT_AudioFrames::FindMpegSync(mixed, 8) == 4,
					"FindMpegSync skips to the first real header");
		// AC-3: 48 kHz, frmsizecod 0 -> 64 words -> 128 bytes.
		const u8 ac3[6] = { 0x0B, 0x77, 0x00, 0x00, 0x00, 0x00 };
		ASSERT_TRUE(MT_AudioFrames::Ac3FrameSize(ac3, 128) == 128,
					"AC-3 48kHz frmsizecod 0 is 128 bytes");
		StepCompleted(stepNum++, true, "MT_AudioFrames splits MP3 and AC-3, and refuses junk");
	}

	// ========================================================================
	// The lane itself
	// ========================================================================
	struct SCase { const char *file; const char *codec; const char *audio; int width; };
	const SCase cases[] = {
		// MPEG-4 Part 2 -- the codec the whole "AVI is an accepted store loss"
		// premise rested on, and the premise was wrong.
		{ "tests/fixtures/video/mpeg4_mp3.avi", "mpeg4", "mp3", 64 },
		// MJPEG needs no configuration record at all, which is a different
		// path through the decoder setup and worth its own case.
		{ "tests/fixtures/video/mjpeg_mp3.avi", "mjpeg", "mp3", 64 },
	};

	int tested = 0;
	for (const SCase &c : cases)
	{
		std::string path = CTest::ResolveProjectPath(c.file);
		FILE *probe = fopen(path.c_str(), "rb");
		if (probe == NULL)
			continue;
		fclose(probe);
		tested++;

		ASSERT_TRUE(CVideoSourceAvi::CanProbablyOpen(path.c_str()), "the lane claims .avi");

#if !defined(__APPLE__) && !defined(_WIN32)
		// NO OS DECODER HERE, so the lane must REFUSE the file -- and the
		// refusal is ASSERTED rather than skipped, because it is what lets
		// CVideoPlayer's chain fall through to FFmpeg in a private build. A
		// lane that opened the file and produced black frames would satisfy
		// a skip and fail a user.
		{
			CVideoSourceAvi *refuser = new CVideoSourceAvi();
			const bool openedHere = refuser->Open(path.c_str());
			snprintf(msg, sizeof(msg), "%s: refused, as it must be here: %s",
					 c.file, refuser->GetErrorReason().c_str());
			delete refuser;
			ASSERT_TRUE(!openedHere,
						"an AVI must be REFUSED where no OS decoder exists, not opened black");
			StepCompleted(stepNum++, true, msg);
			continue;
		}
#endif

		CVideoSourceAvi *src = new CVideoSourceAvi();
		if (!src->Open(path.c_str()))
		{
			snprintf(msg, sizeof(msg), "FAIL: %s: %s", c.file, src->GetErrorReason().c_str());
			delete src;
			TestCompleted(false, msg);
			return;
		}

		const SVideoInfo &info = src->Info();
		snprintf(msg, sizeof(msg), "%s: %s %dx%d %.2ffps, audio '%s' %u Hz",
				 c.file, info.videoCodecName.c_str(), info.width, info.height,
				 info.fps, info.audioCodecName.c_str(), info.audioSampleRate);
		if (info.videoCodecName != c.codec || info.width != c.width ||
			info.audioCodecName != c.audio)
		{
			delete src;
			TestCompleted(false, msg);
			return;
		}
		StepCompleted(stepNum++, true, msg);

		int frames = 0, audioBlocks = 0;
		u64 audioSamples = 0;
		u32 audioRate = 0;
		for (int i = 0; i < 600 && frames < 20; i++)
		{
			SDecodedVideoFrame vf;
			if (src->ReadVideoFrame(vf))
			{
				frames++;
				if (!FrameHasPicture(vf, c.width))
				{
					snprintf(msg, sizeof(msg), "%s: frame %d decoded but is blank or wrong size",
							 c.file, frames);
					delete src;
					TestCompleted(false, msg);
					return;
				}
			}
			SDecodedAudio af;
			while (src->ReadAudio(af))
			{
				audioBlocks++;
				audioSamples += af.pcm.size();
				if (audioRate == 0) audioRate = af.sampleRate;
			}
		}
		snprintf(msg, sizeof(msg), "%s: %d frames with real picture, %d audio blocks / %llu samples at %u Hz",
				 c.file, frames, audioBlocks, (unsigned long long)audioSamples, (unsigned)audioRate);
		if (frames < 10)
		{
			delete src;
			TestCompleted(false, msg);
			return;
		}
		StepCompleted(stepNum++, true, msg);

		// AUDIO IS A SEPARATE CLAIM. The MP3 in an AVI is split by our own
		// frame walker before the OS ever sees it, and getting that wrong
		// yields silence rather than an error.
		if (audioBlocks == 0 || audioSamples == 0)
		{
			snprintf(msg, sizeof(msg), "%s: video decoded but NO audio came out", c.file);
			delete src;
			TestCompleted(false, msg);
			return;
		}
		if (audioRate != 44100)
		{
			snprintf(msg, sizeof(msg), "%s: audio at %u Hz, 44100 expected", c.file, audioRate);
			delete src;
			TestCompleted(false, msg);
			return;
		}
		StepCompleted(stepNum++, true, "audio decoded through the OS at the file's own rate");

		// Seek, then decode again. AVI's keyframe table is `idx1`, and a
		// wrong offset base there lands mid-chunk and produces nothing.
		if (src->Seek(1.0))
		{
			int after = 0;
			for (int i = 0; i < 300 && after < 3; i++)
			{
				SDecodedVideoFrame vf;
				if (src->ReadVideoFrame(vf)) after++;
			}
			snprintf(msg, sizeof(msg), "%s: %d frames after a seek to 1.0s", c.file, after);
			if (after == 0)
			{
				delete src;
				TestCompleted(false, msg);
				return;
			}
			StepCompleted(stepNum++, true, msg);
		}

		delete src;
	}

	if (tested == 0)
	{
		TestSkipped("this app ships no tests/fixtures/video/*.avi");
		return;
	}

#if !defined(__APPLE__) && !defined(_WIN32)
	TestCompleted(true, "the AVI lane demuxes here and correctly refuses: no OS decoder on this platform");
#else
	TestCompleted(true, "the AVI lane plays MPEG-4 Part 2 and MJPEG with MP3 audio, with no FFmpeg");
#endif
}

void CTestAviLane::Cancel() { isRunning = false; }
