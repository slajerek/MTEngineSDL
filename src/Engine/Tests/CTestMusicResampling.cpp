#include "CTestMusicResampling.h"
#include "SYS_Defs.h"
#include "CSlrMusicFileOgg.h"
#include "CSlrFileFromDocuments.h"
#include "SND_SoundEngine.h"
#include "DBG_Log.h"
#include <cmath>
#include <cstdio>
#include <vector>

#define ASSERT_TRUE(cond, msg) do { if (!(cond)) { TestCompleted(false, (msg)); return; } } while (0)

CTestMusicResampling::CTestMusicResampling()  {}
CTestMusicResampling::~CTestMusicResampling() {}

// Rising zero crossings on the left channel -> Hz. No FFT needed: the defect is
// a rate error, and a rate error moves the whole spectrum.
static double MeasureHz(const std::vector<int> &mix, u32 frames, double rateHz)
{
	if (frames < 2 || rateHz <= 0.0) return 0.0;
	const signed short *s = (const signed short *)mix.data();
	u32 crossings = 0;
	for (u32 i = 1; i < frames; i++)
	{
		signed short a = s[(size_t)(i - 1) * 2];
		signed short b = s[(size_t)i * 2];
		if (a <= 0 && b > 0)
			crossings++;
	}
	return (double)crossings / ((double)frames / rateHz);
}

void CTestMusicResampling::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;
	char msg[224];

	// The fixtures live in the host app. Four other applications link this
	// archive and ship none, so an absent fixture is a skip, not a failure --
	// the shape CTestKtx2Transcode uses for the same reason.
	std::string path48 = CTest::ResolveProjectPath("tests/fixtures/audio/tone1k_48k_stereo.ogg");
	std::string path44 = CTest::ResolveProjectPath("tests/fixtures/audio/tone1k_44k_stereo.ogg");
	FILE *probe = fopen(path48.c_str(), "rb");
	if (probe == NULL)
	{
		TestSkipped("this app ships no tests/fixtures/audio/tone1k_48k_stereo.ogg");
		return;
	}
	fclose(probe);

	const u32 engineRate = SND_GetEngineSampleRate();
	const u32 kBlock = 1024;

	// ---- the 48 kHz file: the case that shipped broken --------------------
	{
		// Opened the way CPlaylistPlaybackManager::EnsureTrackPlaybackLoaded
		// opens a track -- a CSlrFileFromDocuments over an absolute path. The
		// (const char*, seekable, fromResources) overload goes through
		// RES_OpenFile, which does not take absolute paths and hands back a
		// half-built file that faults inside stdio.
		CSlrFileFromDocuments *f48 = new CSlrFileFromDocuments(path48.c_str(), SLR_FILE_MODE_READ, true);
		ASSERT_TRUE(f48 != NULL && f48->Exists(), "FAIL: the 48 kHz fixture did not open");
		CSlrMusicFileOgg *m = new CSlrMusicFileOgg("test48");
		ASSERT_TRUE(m->Init(f48, true), "FAIL: could not decode the 48 kHz fixture");

		snprintf(msg, sizeof(msg), "48 kHz fixture reports %u Hz, %d ch",
				 (unsigned)m->GetSourceSampleRate(), m->GetSourceChannels());
		ASSERT_TRUE(m->GetSourceSampleRate() == 48000 && m->GetSourceChannels() == 2, msg);
		StepCompleted(stepNum++, true, msg);

		// THE INVARIANT. Length is in the FILE's frames -- 2 s at 48 kHz is
		// 96000, not 88200. Every stored cue depends on this not moving.
		u64 len = m->GetLengthSamples();
		snprintf(msg, sizeof(msg), "GetLengthSamples() = %llu native frames (2 s at 48 kHz = 96000)",
				 (unsigned long long)len);
		ASSERT_TRUE(len >= 95000 && len <= 97000, msg);
		StepCompleted(stepNum++, true, msg);

		// Pull one engine-second through the real mix path.
		m->Play();
		std::vector<int> mix((size_t)engineRate + kBlock, 0);
		u32 got = 0;
		while (got + kBlock <= engineRate)
		{
			m->FillBuffer(mix.data() + got, kBlock);
			got += kBlock;
		}
		double hz = MeasureHz(mix, got, (double)engineRate);
		snprintf(msg, sizeof(msg),
				 "the 48 kHz tone plays at %.1f Hz (1000 expected; unresampled it reads 919)", hz);
		ASSERT_TRUE(hz > 985.0 && hz < 1015.0, msg);
		StepCompleted(stepNum++, true, msg);

		// Seconds convert with the FILE's rate, not the engine's.
		m->Seek(1.0);
		u64 pos = m->GetCurrentSampleNum();
		snprintf(msg, sizeof(msg), "Seek(1.0) landed at native frame %llu (48000 expected, not 44100)",
				 (unsigned long long)pos);
		ASSERT_TRUE(pos >= 47000 && pos <= 49000, msg);
		StepCompleted(stepNum++, true, msg);

		delete m;
	}

	// ---- the 44.1 kHz file: the path that must not have changed -----------
	{
		CSlrFileFromDocuments *f44 = new CSlrFileFromDocuments(path44.c_str(), SLR_FILE_MODE_READ, true);
		ASSERT_TRUE(f44 != NULL && f44->Exists(), "FAIL: the 44.1 kHz fixture did not open");
		CSlrMusicFileOgg *m = new CSlrMusicFileOgg("test44");
		ASSERT_TRUE(m->Init(f44, true), "FAIL: could not decode the 44.1 kHz fixture");
		ASSERT_TRUE(m->GetSourceSampleRate() == 44100, "FAIL: 44.1 kHz fixture misreported");

		m->Play();
		std::vector<int> mix((size_t)engineRate + kBlock, 0);
		u32 got = 0;
		while (got + kBlock <= engineRate)
		{
			m->FillBuffer(mix.data() + got, kBlock);
			got += kBlock;
		}
		double hz = MeasureHz(mix, got, (double)engineRate);
		snprintf(msg, sizeof(msg), "the 44.1 kHz tone still plays at %.1f Hz", hz);
		ASSERT_TRUE(hz > 985.0 && hz < 1015.0, msg);
		StepCompleted(stepNum++, true, msg);

		delete m;
	}

	TestCompleted(true, "the music path converts rate, and the sample domain stays native");
}

void CTestMusicResampling::Cancel()
{
	isRunning = false;
}
