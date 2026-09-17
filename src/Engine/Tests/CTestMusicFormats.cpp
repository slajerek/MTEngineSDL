#include "CTestMusicFormats.h"
// The guard MUST match CSlrMusicFileMiniaudio.h exactly. An app that resolves
// MT_CAP_MUSIC_PLAYER=0 (c64d) does not have the class at all --
// guarding the test more loosely than the code it tests broke those builds,
// the same way CTestVideoSourceAVF did against MT_CAP_VIDEO_PLAYBACK.
#if !defined(MT_CAP_MUSIC_PLAYER) || MT_CAP_MUSIC_PLAYER
#include "CSlrMusicFileMiniaudio.h"
#include "CSlrMusicFileNative.h"
#include "MT_MusicFileFactory.h"
#include "CMusicWaveformCache.h"
#define MT_TEST_HAVE_MUSIC 1
#else
#define MT_TEST_HAVE_MUSIC 0
#endif
#include "SND_SoundEngine.h"
#include "SYS_Defs.h"
#include "DBG_Log.h"
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <string>
#include <cstring>
#include <vector>

#define MF_ASSERT(cond, msg) do { if (!(cond)) { \
	LOGError("CTestMusicFormats: FAIL: %s", (msg)); \
	TestCompleted(false, (msg)); return; } } while (0)

CTestMusicFormats::CTestMusicFormats()  {}
CTestMusicFormats::~CTestMusicFormats() {}

// Count zero crossings in one channel of an interleaved s16 block and turn that
// into a frequency. A 1 kHz tone crosses zero 2000 times a second; anything
// that resamples wrongly, halves the rate, or swaps channel counts lands
// somewhere else entirely. Tolerant by design -- this is a "did the pipeline
// stay coherent" check, not a spectrum analyser.
static double MeasureHz(const signed short *pcm, int frames, u32 sampleRate)
{
	if (frames < 2 || sampleRate == 0)
		return 0.0;
	int crossings = 0;
	for (int i = 1; i < frames; i++)
	{
		short prev = pcm[(size_t)(i - 1) * 2];
		short cur  = pcm[(size_t)i * 2];
		if ((prev < 0 && cur >= 0) || (prev >= 0 && cur < 0))
			crossings++;
	}
	const double seconds = (double)frames / (double)sampleRate;
	return (double)crossings / (2.0 * seconds);
}

void CTestMusicFormats::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
#if !MT_TEST_HAVE_MUSIC
	TestSkipped("MT_CAP_MUSIC_PLAYER is off in this build");
	return;
#else
	int stepNum = 1;
	char msg[256];

	// The extension gate first: it is what the playlist and the file dialog
	// both branch on, so a mistake here is a format that silently never opens.
	MF_ASSERT(CSlrMusicFileMiniaudio::CanPlay("/x/a.mp3"),  "CanPlay accepts .mp3");
	MF_ASSERT(CSlrMusicFileMiniaudio::CanPlay("/x/a.FLAC"), "CanPlay is case-insensitive");
	MF_ASSERT(CSlrMusicFileMiniaudio::CanPlay("/x/a.wav"),  "CanPlay accepts .wav");
	MF_ASSERT(!CSlrMusicFileMiniaudio::CanPlay("/x/a.ogg"),
			  "CanPlay REFUSES .ogg -- CSlrMusicFileOgg owns it, and two decoders "
			  "competing for one extension is how a working path gets replaced");
	MF_ASSERT(!CSlrMusicFileMiniaudio::CanPlay("/x/noext"), "CanPlay refuses an extensionless path");
	StepCompleted(stepNum++, true, "the extension gate accepts mp3/flac/wav and leaves ogg alone");

	struct { const char *file; u32 rate; } cases[] = {
		{ "tests/fixtures/audio/sine1k_44100.wav",  44100 },
		{ "tests/fixtures/audio/sine1k_44100.flac", 44100 },
		{ "tests/fixtures/audio/sine1k_44100.mp3",  44100 },
		{ "tests/fixtures/audio/sine1k_48000.wav",  48000 },
	};

	int tested = 0;
	for (const auto &c : cases)
	{
		std::string path = CTest::ResolveProjectPath(c.file);
		FILE *probe = fopen(path.c_str(), "rb");
		if (probe == NULL)
			continue;                       // this app ships no such fixture
		fclose(probe);

		CSlrMusicFileMiniaudio music(path.c_str());
		snprintf(msg, sizeof(msg), "%s: opened", c.file);
		MF_ASSERT(music.IsOpen(), msg);

		// THE SAMPLE INVARIANT. GetSourceSampleRate is the FILE's rate, and
		// GetLengthSamples counts FILE frames -- never engine frames. An app
		// stores cue points as native-frame numbers, so a 48 kHz file that
		// reported engine-rate positions would move every cue in it. This is
		// the defect that once made a whole soundtrack play 1.45 semitones flat.
		snprintf(msg, sizeof(msg), "%s: reports its OWN rate %u (engine is %u)",
				 c.file, music.GetSourceSampleRate(), SND_GetEngineSampleRate());
		MF_ASSERT(music.GetSourceSampleRate() == c.rate, msg);
		StepCompleted(stepNum++, true, msg);

		const u64 len = music.GetLengthSamples();
		snprintf(msg, sizeof(msg), "%s: %llu native frames (~1s at %u Hz)",
				 c.file, (unsigned long long)len, c.rate);
		MF_ASSERT(len > (u64)(c.rate * 0.8) && len < (u64)(c.rate * 1.3), msg);
		StepCompleted(stepNum++, true, msg);

		// Decode a block and measure it. Zero pcm, or a tone at the wrong
		// frequency, both fail here -- which is the point: "it linked" and "it
		// decodes correctly" are different claims and only the second matters.
		music.Play();
		const int frames = 8192;
		std::vector<int> mix((size_t)frames);      // 2 s16 per int, as the mixer uses
		music.FillBuffer(mix.data(), (u32)frames);
		const signed short *pcm = (const signed short *)mix.data();

		long energy = 0;
		for (int i = 0; i < frames * 2; i++)
			energy += (long)abs(pcm[i]);
		snprintf(msg, sizeof(msg), "%s: decoded energy %ld over %d frames", c.file, energy, frames);
		MF_ASSERT(energy > frames, msg);          // not silence
		StepCompleted(stepNum++, true, msg);

		// Everything reaching the mixer is at the ENGINE rate, whatever the file
		// was -- that is what the resampler is for, and the 48 kHz fixture is
		// here to prove the conversion happens rather than being assumed.
		const double hz = MeasureHz(pcm, frames, SND_GetEngineSampleRate());
		snprintf(msg, sizeof(msg), "%s: measured %.0f Hz at the mixer (1000 expected)", c.file, hz);
		MF_ASSERT(hz > 900.0 && hz < 1100.0, msg);
		StepCompleted(stepNum++, true, msg);

		// Seeking is in NATIVE frames too.
		music.SeekToSample(c.rate / 2);           // half a second in
		const u64 cursor = music.GetCurrentSampleNum();
		snprintf(msg, sizeof(msg), "%s: seek to native frame %u landed at %llu",
				 c.file, c.rate / 2, (unsigned long long)cursor);
		MF_ASSERT(cursor > (u64)(c.rate / 2) - 4096 && cursor < (u64)(c.rate / 2) + 4096, msg);
		StepCompleted(stepNum++, true, msg);

		// THE LICENCE RULE, as an assertion rather than a hope. Correct PCM
		// alone does not say WHICH decoder produced it, and in a private build
		// FFmpeg is linked for video -- so "no libav symbol" proves nothing
		// here. Naming the decoder is what makes the claim testable.
		snprintf(msg, sizeof(msg), "%s: decoded by '%s'", c.file, music.GetActiveDecoderName());
		MF_ASSERT(strcmp(music.GetActiveDecoderName(), "miniaudio") == 0, msg);
		StepCompleted(stepNum++, true, msg);

		tested++;
	}

#ifdef MT_HAVE_NATIVE_MUSIC
	// AAC and ALAC, decoded by the OS. These are the two formats that would
	// otherwise have needed an MP4 demuxer plus an esds/magic-cookie builder
	// (tracker 4.3); measured here instead through the same 1 kHz yardstick as
	// everything else, so "the OS decoded it" and "it decoded correctly" stay
	// separate claims.
	MF_ASSERT(CSlrMusicFileNative::CanPlay("/x/a.m4a"), "the native gate accepts .m4a");
	MF_ASSERT(!CSlrMusicFileNative::CanPlay("/x/a.mp3"),
			  "the native gate REFUSES .mp3 -- miniaudio owns it on every platform, "
			  "and one extension must have one owner");
	StepCompleted(stepNum++, true, "the OS-decoder gate takes m4a/aac and leaves the rest alone");

	const char *nativeCases[] = {
		"tests/fixtures/audio/sine1k_44100.m4a",        // AAC
		"tests/fixtures/audio/sine1k_44100_alac.m4a",   // ALAC, lossless
		"tests/fixtures/audio/sine1k_48000.opus",       // Opus, and 48 kHz
		// RAW ADTS. No container, so no `esds` and no AudioSpecificConfig --
		// the exact case tracker 4.6 is about. A native PACKET decoder cannot
		// open this without synthesising an ASC from the ADTS header (and
		// getting implicit-SBR HE-AAC wrong if it does so naively); the OS
		// reader parses ADTS itself, which is why this works here and would
		// not through CAudioDecoderAACNative.
		"tests/fixtures/audio/sine1k_44100_adts.aac",
	};
	int aacFixturesSeen = 0;
	int aacFixturesOpened = 0;
	for (const char *file : nativeCases)
	{
		std::string path = CTest::ResolveProjectPath(file);
		FILE *probe = fopen(path.c_str(), "rb");
		if (probe == NULL)
			continue;
		fclose(probe);

		const bool isAac = (strstr(file, ".m4a") != NULL && strstr(file, "alac") == NULL)
						|| (strstr(file, ".aac") != NULL);
		if (isAac) aacFixturesSeen++;

		CSlrMusicFileNative music(path.c_str());
		if (isAac && music.IsOpen()) aacFixturesOpened++;
		snprintf(msg, sizeof(msg), "%s: opened through the OS decoder", file);
		MF_ASSERT(music.IsOpen(), msg);

		snprintf(msg, sizeof(msg), "%s: %u Hz native, %llu frames", file,
				 music.GetSourceSampleRate(), (unsigned long long)music.GetLengthSamples());
		// Opus is always 48 kHz by design; the rest of these fixtures are 44.1.
		// Either way the mixer must see 1 kHz, which is what proves the
		// resampler ran rather than the rate being assumed.
		MF_ASSERT(music.GetSourceSampleRate() == 44100 || music.GetSourceSampleRate() == 48000, msg);
		MF_ASSERT(music.GetLengthSamples() > 30000 && music.GetLengthSamples() < 70000, msg);
		StepCompleted(stepNum++, true, msg);

		music.Play();
		const int frames = 8192;
		std::vector<int> mix((size_t)frames);
		music.FillBuffer(mix.data(), (u32)frames);
		const signed short *pcm = (const signed short *)mix.data();

		long energy = 0;
		for (int i = 0; i < frames * 2; i++)
			energy += (long)abs(pcm[i]);
		snprintf(msg, sizeof(msg), "%s: energy %ld", file, energy);
		MF_ASSERT(energy > frames, msg);

		const double hz = MeasureHz(pcm, frames, SND_GetEngineSampleRate());
		snprintf(msg, sizeof(msg), "%s: measured %.0f Hz at the mixer (1000 expected)", file, hz);
		MF_ASSERT(hz > 900.0 && hz < 1100.0, msg);
		StepCompleted(stepNum++, true, msg);

		// The OS decoded it -- which is the whole reason AAC, ALAC and Opus are
		// shippable at all. A store build may carry these bytes because Apple
		// and Microsoft hold the codec licences, not because we vendored one.
		const char *dec = music.GetActiveDecoderName();
		snprintf(msg, sizeof(msg), "%s: decoded by '%s'", file, dec);
		MF_ASSERT(strcmp(dec, "avfoundation") == 0 || strcmp(dec, "mediafoundation") == 0, msg);
		StepCompleted(stepNum++, true, msg);
		tested++;
	}

	// --- tracker 4.7: the availability API must agree with what happened ----
	//
	// This is the whole point of MusicFile_GetAacAvailability(): a Windows N
	// machine has no MF AAC decoder, and the UI offers an install link based on
	// that answer. An answer nobody cross-checks is a rumour, so it is checked
	// here against the fixtures that just did or did not open -- the two must
	// never disagree, in either direction. A machine that decodes AAC while the
	// API advertises an install link would nag every user forever; a machine
	// that cannot while the API says all is well fails silently, which is the
	// behaviour 4.7 exists to end.
	if (aacFixturesSeen > 0)
	{
		const EMusicAacAvailability avail = MusicFile_GetAacAvailability();
		const char *url = MusicFile_GetAacInstallUrl();
		const bool decoded = (aacFixturesOpened == aacFixturesSeen);

		snprintf(msg, sizeof(msg),
				 "AAC availability %d agrees with %d/%d fixtures opened",
				 (int)avail, aacFixturesOpened, aacFixturesSeen);
		MF_ASSERT(decoded == (avail == MUSIC_AAC_AVAILABLE), msg);

		// NULL, not "": callers test the pointer. And it is non-NULL in exactly
		// one state -- the one a user can act on.
		MF_ASSERT((url != NULL) == (avail == MUSIC_AAC_MISSING_SYSTEM_CODEC),
				  "the install URL is offered only for the fixable case");
		MF_ASSERT((MusicFile_GetAacUnavailableReasonKey() == NULL) == (avail == MUSIC_AAC_AVAILABLE),
				  "a reason key exists exactly when there is something to say");
		StepCompleted(stepNum++, true, msg);
	}
#endif

	if (tested == 0)
	{
		TestSkipped("this app ships no tests/fixtures/audio/sine1k_* fixtures");
		return;
	}

	// THE WAVEFORM, for a format that is not .ogg. Until now the playlist called
	// LoadFromOggFile unconditionally, so every format added since had no
	// waveform at all -- it would have drawn a flat line and nobody would have
	// called it a bug. A 1 kHz sine must produce an envelope that actually
	// swings, so a flat or empty one fails here.
	{
		std::string wavPath = CTest::ResolveProjectPath("tests/fixtures/audio/sine1k_44100.flac");
		FILE *probe = fopen(wavPath.c_str(), "rb");
		if (probe != NULL)
		{
			fclose(probe);
			CMusicWaveformCache cache;
			MF_ASSERT(cache.LoadFromMusicFile(wavPath, 512),
					  "the waveform cache builds an envelope from a FLAC, not just an Ogg");
			MF_ASSERT(cache.GetEnvelopePointCount() == 512, "it produced the requested point count");

			std::vector<CMusicWaveformRenderPoint> pts;
			MF_ASSERT(cache.BuildRenderWindow(64, 0.5f, 1.0f, pts), "it renders a window");
			float lo = 1.0f, hi = -1.0f;
			for (const auto &p : pts) { lo = std::min(lo, p.minValue); hi = std::max(hi, p.maxValue); }
			// The threshold is derived, not guessed. ffmpeg's `sine` source is
			// not full-scale: the decode energy check above measures a mean
			// |sample| of ~1843, so the peak is ~1843/0.637 = ~2894, i.e.
			// ~0.088 of full scale and a span of ~0.176. A first attempt at
			// 0.2 failed for that reason -- the waveform was right and the
			// assertion was invented. 0.05 is comfortably below the real
			// value and far above the ~0 a flat or empty envelope gives.
			snprintf(msg, sizeof(msg), "FLAC envelope spans %.3f..%.3f (flat would be ~0)", lo, hi);
			MF_ASSERT(hi - lo > 0.05f, msg);
			StepCompleted(stepNum++, true, msg);
		}
	}

	snprintf(msg, sizeof(msg), "%d formats decode correctly with no FFmpeg linked", tested);
	TestCompleted(true, msg);
#endif
}

void CTestMusicFormats::Cancel() { isRunning = false; }
