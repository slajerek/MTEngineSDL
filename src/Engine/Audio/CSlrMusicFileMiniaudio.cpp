#include "CSlrMusicFileMiniaudio.h"

#if !defined(MT_CAP_MUSIC_PLAYER) || MT_CAP_MUSIC_PLAYER
#include "SND_SoundEngine.h"
#include "DBG_Log.h"

#include "../Libs/miniaudio/miniaudio.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#define MA_DEC(p) ((ma_decoder *)(p))

// ============================================================================

bool CSlrMusicFileMiniaudio::CanPlay(const char *filePath)
{
	if (filePath == NULL || filePath[0] == '\0')
		return false;
	std::string p(filePath);
	size_t dot = p.find_last_of('.');
	if (dot == std::string::npos)
		return false;
	std::string ext = p.substr(dot + 1);
	for (char &c : ext)
		c = (char)tolower((unsigned char)c);

	// Exactly what miniaudio's built-in decoders cover. `.ogg` is DELIBERATELY
	// absent: CSlrMusicFileOgg owns it, it already works, and it carries the
	// cue-point behaviour apps depend on. Two decoders competing for one
	// extension is how a working path gets silently replaced.
	return ext == "mp3" || ext == "flac" || ext == "wav";
}

CSlrMusicFileMiniaudio::CSlrMusicFileMiniaudio(const char *filePath)
: CSlrMusicFileResampled(filePath)
{
	this->shouldBeDestroyedByEngine = false;
	this->shouldBeRemovedByEngine = false;
	if (filePath != NULL)
		Init(filePath);
}

CSlrMusicFileMiniaudio::~CSlrMusicFileMiniaudio()
{
	if (decoder != nullptr)
	{
		ma_decoder_uninit(MA_DEC(decoder));
		delete MA_DEC(decoder);
		decoder = nullptr;
	}
}

bool CSlrMusicFileMiniaudio::Init(const char *filePath)
{
	if (filePath == NULL || filePath[0] == '\0')
		return false;
	path = filePath;

	ma_decoder *dec = new ma_decoder();

	// DECODE AT THE FILE'S OWN RATE AND CHANNEL COUNT.
	//
	// miniaudio can resample and downmix itself, and asking it to would be one
	// line shorter. It is not done, because the engine's sample invariant says a
	// "sample" is a FILE frame: an app stores cue points as native-frame numbers,
	// so a decoder that silently handed back engine-rate frames would move every
	// cue in every 48 kHz file. Rate conversion belongs to MT_AudioResampler,
	// which is the engine's ONE converter, and it happens in FillBuffer.
	//
	// s16 out because that is what the mixer takes; 0/0 means "keep the source's
	// own channels and rate".
	ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, 0, 0);

	ma_result r = ma_decoder_init_file(filePath, &cfg, dec);
	if (r != MA_SUCCESS)
	{
		LOGError("CSlrMusicFileMiniaudio: could not open '%s' (miniaudio result %d)", filePath, (int)r);
		delete dec;
		return false;
	}

	decoder = dec;
	sourceSampleRate = (u32)dec->outputSampleRate;
	sourceChannels = (int)dec->outputChannels;
	if (sourceChannels <= 0) sourceChannels = 2;
	if (sourceSampleRate == 0) sourceSampleRate = SND_GetEngineSampleRate();

	ma_uint64 frames = 0;
	if (ma_decoder_get_length_in_pcm_frames(dec, &frames) == MA_SUCCESS)
		lengthFrames = (u64)frames;   // NATIVE frames -- see the header
	else
		lengthFrames = 0;             // some MP3 streams cannot report a length

	ResetResampler();

	LOGD("CSlrMusicFileMiniaudio: '%s' %u Hz, %d ch, %llu native frames (engine %u Hz)",
		 filePath, sourceSampleRate, sourceChannels,
		 (unsigned long long)lengthFrames, SND_GetEngineSampleRate());
	return true;
}

// Decode up to `frames` NATIVE frames into the base's buffer, widened to
// interleaved stereo s16.
u32 CSlrMusicFileMiniaudio::DecodeNativeStereo(s16 *out, u32 frames)
{
	if (decoder == nullptr || frames == 0 || out == NULL)
		return 0;

	const int ch = sourceChannels;
	std::vector<s16> raw((size_t)frames * (size_t)ch);
	ma_uint64 got = 0;
	ma_result r = ma_decoder_read_pcm_frames(MA_DEC(decoder), raw.data(), frames, &got);
	if (r != MA_SUCCESS && got == 0)
	{
		endOfStream = true;
		return 0;
	}
	if (got == 0)
	{
		endOfStream = true;
		return 0;
	}

	if (ch == 2)
	{
		memcpy(out, raw.data(), (size_t)got * 2 * sizeof(s16));
	}
	else if (ch == 1)
	{
		// Mono widened here, BEFORE the resampler, so the resampler only ever
		// sees stereo -- the same order CSlrMusicFileOgg uses.
		for (ma_uint64 i = 0; i < got; i++)
		{
			s16 v = raw[(size_t)i];
			out[(size_t)i * 2 + 0] = v;
			out[(size_t)i * 2 + 1] = v;
		}
	}
	else
	{
		// More than two channels: take the front pair. A correct downmix needs a
		// per-codec channel map and normalisation (1 + 0.707 + 0.707 clips), and
		// that is tracker item C.5 rather than something to guess at here.
		static bool warned = false;
		if (!warned)
		{
			LOGWarning("CSlrMusicFileMiniaudio: %d channels in '%s'; using the front pair "
					   "(a real downmix is tracker C.5)", ch, path.c_str());
			warned = true;
		}
		for (ma_uint64 i = 0; i < got; i++)
		{
			out[(size_t)i * 2 + 0] = raw[(size_t)i * ch + 0];
			out[(size_t)i * 2 + 1] = raw[(size_t)i * ch + 1];
		}
	}
	return (u32)got;
}

void CSlrMusicFileMiniaudio::FillBuffer(int *mixBuffer, u32 numSamples)
{
	if (decoder == nullptr || !playing)
	{
		memset((signed short *)mixBuffer, 0, (size_t)numSamples * 2 * sizeof(signed short));
		return;
	}
	FillResampled(mixBuffer, numSamples);
	// The base pads a short block with silence rather than reporting it, so
	// "the track ended" is observed here, where the decoder's own state is.
	if (endOfStream)
		playing = false;
}

void CSlrMusicFileMiniaudio::Mix(int *mixBuffer, u32 numSamples)
{
	FillBuffer(mixBuffer, numSamples);
}

// ============================================================================
// Transport. Every sample number below is a NATIVE frame -- see the header.
// ============================================================================

void CSlrMusicFileMiniaudio::Play()   { if (decoder) playing = true; }
void CSlrMusicFileMiniaudio::Pause()  { playing = false; }
void CSlrMusicFileMiniaudio::Stop()   { playing = false; Rewind(); }
bool CSlrMusicFileMiniaudio::IsPlaying() { return playing; }

void CSlrMusicFileMiniaudio::Rewind()
{
	SeekToSample(0);
}

void CSlrMusicFileMiniaudio::SeekToSample(u64 sampleNum)
{
	if (decoder == nullptr)
		return;
	if (lengthFrames > 0 && sampleNum > lengthFrames)
		sampleNum = lengthFrames;
	if (ma_decoder_seek_to_pcm_frame(MA_DEC(decoder), (ma_uint64)sampleNum) != MA_SUCCESS)
	{
		LOGWarning("CSlrMusicFileMiniaudio: seek to native frame %llu failed in '%s'",
				   (unsigned long long)sampleNum, path.c_str());
		return;
	}
	// The resampler carries a phase accumulator across calls; leaving it in
	// place after a discontinuity would smear the join.
	ResetResampler();
}

u64 CSlrMusicFileMiniaudio::GetCurrentSampleNum()
{
	if (decoder == nullptr)
		return 0;
	ma_uint64 cursor = 0;
	if (ma_decoder_get_cursor_in_pcm_frames(MA_DEC(decoder), &cursor) != MA_SUCCESS)
		return 0;
	return (u64)cursor;
}

u64 CSlrMusicFileMiniaudio::GetLengthSamples() { return lengthFrames; }



u32 CSlrMusicFileMiniaudio::ResourceDeactivate(bool async) { return 0; }
u32 CSlrMusicFileMiniaudio::ResourceActivate(bool async)   { return 0; }
u32 CSlrMusicFileMiniaudio::ResourceGetSize()              { return (u32)(lengthFrames * 4); }
const char *CSlrMusicFileMiniaudio::ResourceGetTypeName()  { return "MusicFileMiniaudio"; }

#endif // MT_CAP_MUSIC_PLAYER
