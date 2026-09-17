#include "CSlrMusicFileAVFAudio.h"

#if defined(__APPLE__) && (!defined(MT_CAP_MUSIC_PLAYER) || MT_CAP_MUSIC_PLAYER)

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>

#include "SND_SoundEngine.h"
#include "DBG_Log.h"
#include <cctype>
#include <cstring>

struct CSlrMusicFileAVFAudio::SImpl
{
	AVURLAsset *asset = nil;
	AVAssetTrack *track = nil;
	AVAssetReader *reader = nil;
	AVAssetReaderAudioMixOutput *out = nil;
};

bool CSlrMusicFileAVFAudio::CanPlay(const char *filePath)
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

	// AAC and ALAC live in .m4a/.mp4; .aac is raw ADTS, which AVFoundation
	// reads and the packet decoders famously cannot (no AudioSpecificConfig --
	// tracker 4.6). .caf and .aiff come free with the same reader.
	// `.opus` is here and `.ogg` is NOT, and the split is deliberate. An .opus
	// file is Opus inside an Ogg container, which AVFoundation reads; a plain
	// .ogg is almost always Vorbis, which CSlrMusicFileOgg already owns through
	// Tremor. Claiming .ogg would take a working path away from the decoder
	// that carries an app's soundtrack and its cue behaviour.
	return ext == "m4a" || ext == "aac" || ext == "caf" ||
		   ext == "aiff" || ext == "aif" || ext == "opus";
}

CSlrMusicFileAVFAudio::CSlrMusicFileAVFAudio(const char *filePath)
: CSlrMusicFileResampled(filePath)
{
	impl = new SImpl();
	this->shouldBeDestroyedByEngine = false;
	this->shouldBeRemovedByEngine = false;
	if (filePath != NULL)
		Init(filePath);
}

CSlrMusicFileAVFAudio::~CSlrMusicFileAVFAudio()
{
	TeardownReader();
	if (impl) { impl->track = nil; impl->asset = nil; delete impl; impl = nullptr; }
}

bool CSlrMusicFileAVFAudio::IsOpen() const { return impl != nullptr && impl->asset != nil; }

bool CSlrMusicFileAVFAudio::Init(const char *filePath)
{
	if (filePath == NULL || filePath[0] == '\0')
		return false;
	path = filePath;

	@autoreleasepool
	{
		NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:filePath]];
		impl->asset = [AVURLAsset URLAssetWithURL:url options:nil];
		if (impl->asset == nil)
			return false;

		impl->track = [[impl->asset tracksWithMediaType:AVMediaTypeAudio] firstObject];
		if (impl->track == nil)
		{
			LOGError("CSlrMusicFileAVFAudio: no audio track in '%s'", filePath);
			impl->asset = nil;
			return false;
		}

		// THE FILE'S OWN RATE, read rather than assumed. Asking the reader for
		// the engine rate here would be one line shorter and would silently
		// move every cue point in a 48 kHz file -- the base's whole invariant.
		sourceSampleRate = 0;
		sourceChannels = 0;
		NSArray *fds = impl->track.formatDescriptions;
		if (fds.count > 0)
		{
			CMAudioFormatDescriptionRef afd = (__bridge CMAudioFormatDescriptionRef)fds[0];
			const AudioStreamBasicDescription *asbd =
				CMAudioFormatDescriptionGetStreamBasicDescription(afd);
			if (asbd != NULL)
			{
				sourceSampleRate = (u32)llround(asbd->mSampleRate);
				sourceChannels = (int)asbd->mChannelsPerFrame;
			}
		}
		if (sourceSampleRate == 0) sourceSampleRate = SND_GetEngineSampleRate();
		if (sourceChannels <= 0) sourceChannels = 2;

		const double dur = CMTimeGetSeconds(impl->asset.duration);
		lengthFrames = (dur > 0.0 && !std::isnan(dur))
					 ? (u64)llround(dur * (double)sourceSampleRate) : 0;

		if (!BuildReader(0.0))
		{
			impl->asset = nil;
			return false;
		}
		cursorFrames = 0;
		ResetResampler();

		LOGD("CSlrMusicFileAVFAudio: '%s' %u Hz, %d ch, %llu native frames (engine %u Hz)",
			 filePath, sourceSampleRate, sourceChannels,
			 (unsigned long long)lengthFrames, SND_GetEngineSampleRate());
	}
	return true;
}

bool CSlrMusicFileAVFAudio::BuildReader(double startSeconds)
{
	TeardownReader();
	@autoreleasepool
	{
		NSError *err = nil;
		impl->reader = [[AVAssetReader alloc] initWithAsset:impl->asset error:&err];
		if (impl->reader == nil)
			return false;
		if (startSeconds > 0.0)
		{
			impl->reader.timeRange =
				CMTimeRangeMake(CMTimeMakeWithSeconds(startSeconds, 600), kCMTimePositiveInfinity);
		}

		// STEREO S16 AT THE SOURCE RATE. Stereo because the base's contract is
		// interleaved stereo and it wants mono widened before it sees it; the
		// source rate because rate conversion is MT_AudioResampler's job, not
		// the OS's -- one converter, one place the invariant can be wrong.
		NSDictionary *settings = @{
			AVFormatIDKey               : @(kAudioFormatLinearPCM),
			AVLinearPCMBitDepthKey      : @16,
			AVLinearPCMIsFloatKey       : @NO,
			AVLinearPCMIsBigEndianKey   : @NO,
			AVLinearPCMIsNonInterleaved : @NO,
			AVSampleRateKey             : @((double)sourceSampleRate),
			AVNumberOfChannelsKey       : @2,
		};
		impl->out = [[AVAssetReaderAudioMixOutput alloc]
						initWithAudioTracks:@[impl->track] audioSettings:settings];
		if (![impl->reader canAddOutput:impl->out])
		{
			TeardownReader();
			return false;
		}
		[impl->reader addOutput:impl->out];
		if (![impl->reader startReading])
		{
			TeardownReader();
			return false;
		}
	}
	carry.clear();
	carryPos = 0;
	return true;
}

void CSlrMusicFileAVFAudio::TeardownReader()
{
	if (impl == nullptr) return;
	if (impl->reader != nil && impl->reader.status == AVAssetReaderStatusReading)
		[impl->reader cancelReading];
	impl->reader = nil;
	impl->out = nil;
}

u32 CSlrMusicFileAVFAudio::DecodeNativeStereo(s16 *out, u32 frames)
{
	if (impl == nullptr || impl->out == nil || out == NULL || frames == 0)
		return 0;

	u32 written = 0;
	while (written < frames)
	{
		// Hand over anything left from the previous CMSampleBuffer first. The
		// reader delivers whole buffers on its own schedule, so without this
		// the tail of each one would be dropped and the track would play fast.
		if (carryPos < carry.size())
		{
			const size_t availFrames = (carry.size() - carryPos) / 2;
			const size_t take = std::min((size_t)(frames - written), availFrames);
			memcpy(out + (size_t)written * 2, carry.data() + carryPos, take * 2 * sizeof(s16));
			carryPos += take * 2;
			written += (u32)take;
			continue;
		}

		@autoreleasepool
		{
			CMSampleBufferRef sb = [impl->out copyNextSampleBuffer];
			if (sb == NULL)
				break;   // end of stream

			AudioBufferList abl;
			CMBlockBufferRef block = NULL;
			OSStatus st = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
				sb, NULL, &abl, sizeof(abl), NULL, NULL,
				kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &block);
			if (st == noErr && abl.mNumberBuffers > 0 && abl.mBuffers[0].mData != NULL)
			{
				const size_t bytes = abl.mBuffers[0].mDataByteSize;
				carry.resize(bytes / sizeof(s16));
				memcpy(carry.data(), abl.mBuffers[0].mData, bytes);
				carryPos = 0;
			}
			if (block) CFRelease(block);
			CFRelease(sb);
			if (carry.empty())
				break;
		}
	}

	cursorFrames += written;
	return written;
}

void CSlrMusicFileAVFAudio::FillBuffer(int *mixBuffer, u32 numSamples)
{
	if (!IsOpen() || !playing)
	{
		memset((signed short *)mixBuffer, 0, (size_t)numSamples * 2 * sizeof(signed short));
		return;
	}
	FillResampled(mixBuffer, numSamples);
	if (lengthFrames > 0 && cursorFrames >= lengthFrames)
		playing = false;
}

void CSlrMusicFileAVFAudio::Mix(int *mixBuffer, u32 numSamples) { FillBuffer(mixBuffer, numSamples); }

void CSlrMusicFileAVFAudio::Play()  { if (IsOpen()) playing = true; }
void CSlrMusicFileAVFAudio::Pause() { playing = false; }
void CSlrMusicFileAVFAudio::Stop()  { playing = false; Rewind(); }
bool CSlrMusicFileAVFAudio::IsPlaying() { return playing; }
void CSlrMusicFileAVFAudio::Rewind() { SeekToSample(0); }

u64 CSlrMusicFileAVFAudio::GetCurrentSampleNum() { return cursorFrames; }
u64 CSlrMusicFileAVFAudio::GetLengthSamples()    { return lengthFrames; }
u32 CSlrMusicFileAVFAudio::ResourceGetSize()     { return (u32)(lengthFrames * 4); }

void CSlrMusicFileAVFAudio::SeekToSample(u64 sampleNum)
{
	if (!IsOpen() || sourceSampleRate == 0)
		return;
	if (lengthFrames > 0 && sampleNum > lengthFrames)
		sampleNum = lengthFrames;

	// AVAssetReader cannot seek; a seek IS a new reader over a time range. The
	// argument is native frames, so the conversion to seconds uses the FILE's
	// rate -- using the engine's here is precisely the bug the invariant exists
	// to prevent.
	if (!BuildReader((double)sampleNum / (double)sourceSampleRate))
		return;
	cursorFrames = sampleNum;
	// The resampler carries a phase accumulator; leaving it across a
	// discontinuity smears the join.
	ResetResampler();
}

#endif // __APPLE__ && MT_CAP_MUSIC_PLAYER
