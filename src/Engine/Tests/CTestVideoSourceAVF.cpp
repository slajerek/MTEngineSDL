#include "CTestVideoSourceAVF.h"
#include "SYS_Defs.h"
#include "DBG_Log.h"
#include <cstdio>
#include <cmath>
#include <string>

#define AVF_ASSERT(cond, msg) do { if (!(cond)) { \
	LOGError("CTestVideoSourceAVF: FAIL: %s", (msg)); \
	if (src) { delete src; src = NULL; } \
	TestCompleted(false, (msg)); return; } } while (0)

CTestVideoSourceAVF::CTestVideoSourceAVF()  {}
CTestVideoSourceAVF::~CTestVideoSourceAVF() {}

// The guard MUST match CVideoSourceAVFoundation.h exactly. It is gated on the
// CAPABILITY as well as the platform, so a video-off app (c64d) does not have
// the class at all -- guarding only on __APPLE__ here broke that build.
#if defined(__APPLE__) && (!defined(MT_CAP_VIDEO_PLAYBACK) || MT_CAP_VIDEO_PLAYBACK)
#include "CVideoSourceAVFoundation.h"
#define MT_TEST_HAVE_AVF 1
#else
#define MT_TEST_HAVE_AVF 0
#endif

void CTestVideoSourceAVF::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;

#if !MT_TEST_HAVE_AVF
	TestSkipped("CVideoSourceAVFoundation is macOS-only and needs MT_CAP_VIDEO_PLAYBACK");
	return;
#else
	int stepNum = 1;
	char msg[256];
	CVideoSourceAVFoundation *src = NULL;

	// DELIBERATELY NOT gated on MT_ENABLE_FFMPEG. This source exists precisely
	// for the build that has no FFmpeg, so a gate would make the test vanish in
	// the one configuration it is for -- the hollow-green shape that left the
	// whole video suite passing while executing nothing.
	std::string path = CTest::ResolveProjectPath("tests/fixtures/video/h264_aac.mp4");
	FILE *probe = fopen(path.c_str(), "rb");
	if (probe == NULL)
	{
		TestSkipped("this app ships no tests/fixtures/video/h264_aac.mp4");
		return;
	}
	fclose(probe);

	// --- the routing predicate ------------------------------------------------
	AVF_ASSERT(CVideoSourceAVFoundation::CanProbablyOpen(path.c_str()),
			   "CanProbablyOpen() accepts .mp4");
	AVF_ASSERT(!CVideoSourceAVFoundation::CanProbablyOpen("/tmp/x.mkv"),
			   "CanProbablyOpen() REJECTS .mkv -- AVFoundation cannot demux it, and "
			   "claiming it would turn a routable file into a hard failure");
	StepCompleted(stepNum++, true, "the container probe accepts what AVFoundation demuxes and nothing else");

	// --- open + metadata ------------------------------------------------------
	src = new CVideoSourceAVFoundation();
	if (!src->Open(path.c_str()))
	{
		snprintf(msg, sizeof(msg), "FAIL: AVFoundation could not open h264_aac.mp4: %s",
				 src->GetErrorReason().c_str());
		LOGError("CTestVideoSourceAVF: %s", msg);
		delete src;
		TestCompleted(false, msg);
		return;
	}
	StepCompleted(stepNum++, true, "opened an H.264 + AAC MP4 with no FFmpeg involved");

	const SVideoInfo &info = src->Info();
	snprintf(msg, sizeof(msg), "%dx%d %.2ffps %.2fs codec=%s audio=%s %dch %uHz",
			 info.width, info.height, info.fps, info.duration,
			 info.videoCodecName.c_str(), info.audioCodecName.c_str(),
			 info.audioChannels, info.audioSampleRate);
	AVF_ASSERT(info.width == 64 && info.height == 64, msg);
	AVF_ASSERT(info.duration > 1.5 && info.duration < 2.5, msg);
	// The engine's contract is FFmpeg's spelling, because apps display
	// this string and several tests assert it literally.
	AVF_ASSERT(info.videoCodecName == "h264", msg);
	AVF_ASSERT(info.hasAudio && info.audioCodecName == "aac", msg);
	AVF_ASSERT(info.audioChannels == 2 && info.audioSampleRate == 44100, msg);
	AVF_ASSERT(info.rotationDegrees == 0, "an unrotated clip reports rotationDegrees 0");
	StepCompleted(stepNum++, true, msg);

	// colorSpace must be NORMALIZED (VPX_CS_*), never a raw AVCOL_SPC_* value.
	// 1 = BT.601, 2 = BT.709; a 64x64 clip resolves to 601 by the SD heuristic.
	AVF_ASSERT(info.colorSpace == 1 || info.colorSpace == 2,
			   "colorSpace is a definite normalized matrix, not a raw AVCOL value");
	StepCompleted(stepNum++, true, "colour metadata mapped from CoreVideo's CFStrings into the engine's numbering");

	// --- decode ---------------------------------------------------------------
	SDecodedVideoFrame frame;
	AVF_ASSERT(src->ReadVideoFrame(frame), "decoded the first video frame");
	AVF_ASSERT(frame.width == 64 && frame.height == 64, "the decoded frame is 64x64");
	AVF_ASSERT(frame.pixelFormat == EVideoPixelFormat::NV12,
			   "8-bit content decodes to NV12 (10-bit is reserved for PQ/HLG)");
	AVF_ASSERT(frame.plane[0] != NULL && frame.stride[0] >= frame.width,
			   "the luma plane is populated with a sane stride");
	AVF_ASSERT(frame.plane[1] != NULL, "the interleaved chroma plane is populated");
	AVF_ASSERT(frame.pts >= 0.0 && frame.pts < 0.1, "the first frame's pts is at the clip head");
	StepCompleted(stepNum++, true, "first frame decoded through VideoToolbox via AVFoundation");

	// The frame must not be uniformly zero -- a decoder that hands back a blank
	// buffer would satisfy every check above.
	{
		long sum = 0;
		for (int r = 0; r < frame.height; r++)
			for (int c = 0; c < frame.width; c++)
				sum += frame.plane[0][(size_t)r * frame.stride[0] + c];
		AVF_ASSERT(sum > 0, "the decoded luma is not uniformly zero -- real pixels, not a blank buffer");
		snprintf(msg, sizeof(msg), "luma sum %ld over %d pixels", sum, frame.width * frame.height);
		StepCompleted(stepNum++, true, msg);
	}

	// --- several frames, monotonic pts ---------------------------------------
	{
		double prev = frame.pts;
		int decoded = 1;
		for (int i = 0; i < 10 && src->ReadVideoFrame(frame); i++)
		{
			AVF_ASSERT(frame.pts >= prev, "pts is non-decreasing -- AVFoundation delivers presentation order");
			prev = frame.pts;
			decoded++;
		}
		AVF_ASSERT(decoded >= 5, "decoded a run of frames rather than stopping after one");
		snprintf(msg, sizeof(msg), "%d frames in presentation order, last pts %.3f", decoded, prev);
		StepCompleted(stepNum++, true, msg);
	}

	// --- audio ----------------------------------------------------------------
	{
		SDecodedAudio audio;
		bool gotAudio = false;
		for (int i = 0; i < 8 && !gotAudio; i++)
			gotAudio = src->ReadAudio(audio);
		AVF_ASSERT(gotAudio, "the OS decoded AAC to PCM -- this is what keeps audio working without FFmpeg");
		AVF_ASSERT(audio.channels == 2 && audio.sampleRate == 44100,
				   "decoded audio matches the format Info() advertised");
		AVF_ASSERT(!audio.pcm.empty(), "the PCM buffer is not empty");
		snprintf(msg, sizeof(msg), "%zu interleaved s16 samples at %u Hz", audio.pcm.size(), audio.sampleRate);
		StepCompleted(stepNum++, true, msg);
	}

	// --- seek -----------------------------------------------------------------
	// The fixture carries keyframes at 0.0 and 1.0 on purpose. An earlier spike
	// in this programme measured "0.5ms seeks" that all landed on pts=0 because
	// its 2-second fixture had a single keyframe; a seek test on such a file
	// proves nothing.
	AVF_ASSERT(src->Seek(1.0), "seek to 1.0s");
	AVF_ASSERT(src->ReadVideoFrame(frame), "decoded a frame after seeking");
	snprintf(msg, sizeof(msg), "seek(1.0) landed at pts %.3f", frame.pts);
	AVF_ASSERT(frame.pts >= 0.95 && frame.pts <= 1.30, msg);
	StepCompleted(stepNum++, true, msg);

	// Seek back to the head. The stream-head case is called out separately
	// because a range-based seek is where an off-by-one loses the opening frame.
	AVF_ASSERT(src->Seek(0.0), "seek back to 0.0");
	AVF_ASSERT(src->ReadVideoFrame(frame), "decoded a frame after seeking to the head");
	snprintf(msg, sizeof(msg), "seek(0.0) landed at pts %.3f -- the opening frame is reachable", frame.pts);
	AVF_ASSERT(frame.pts < 0.1, msg);
	StepCompleted(stepNum++, true, msg);

	// --- abort ----------------------------------------------------------------
	// An aborted read returns false with an EMPTY error reason; the interface
	// makes the CALLER classify abort, and a non-empty reason here would make
	// CVideoPlayer latch EVideoPlayerState::Error on an ordinary scrub.
	{
		src->SetAbortPredicate([]() { return true; });
		SDecodedVideoFrame aborted;
		bool ok = src->ReadVideoFrame(aborted);
		AVF_ASSERT(!ok, "a read under a raised abort predicate returns false");
		AVF_ASSERT(src->GetErrorReason().empty(),
				   "an aborted read leaves GetErrorReason() EMPTY -- abort is not an error");
		src->SetAbortPredicate(nullptr);
		StepCompleted(stepNum++, true, "abort returns false with an empty error reason");
	}

	// --- metadata-only open ---------------------------------------------------
	{
		CVideoSourceAVFoundation probeOnly;
		probeOnly.SetOpenVideoOnly(true);
		AVF_ASSERT(probeOnly.Open(path.c_str()), "metadata-only open succeeds");
		AVF_ASSERT(probeOnly.Info().width == 64 && probeOnly.Info().duration > 1.5,
				   "metadata-only open still fills Info() -- what the folder scanner needs");
		StepCompleted(stepNum++, true, "metadata-only open costs no decoder");
	}

	delete src;
	src = NULL;
	TestCompleted(true, "AVFoundation demuxes and decodes H.264 + AAC with no FFmpeg linked");
#endif
}

void CTestVideoSourceAVF::Cancel()
{
	isRunning = false;
}
