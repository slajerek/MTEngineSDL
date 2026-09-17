#ifndef _CSLRMUSICOGG_H_
#define _CSLRMUSICOGG_H_

#include "CSlrMusicFile.h"
#include "CSlrFile.h"
#include "tremor-ivorbiscodec.h"
#include "tremor-ivorbisfile.h"
#include "SYS_Threading.h"
#include "CSlrMusicFileResampled.h"
#include <vector>

class CSlrMusicFileOgg : public CSlrMusicFileResampled
{
public:
	const char *GetActiveDecoderName() override { return "tremor"; }

	CSlrMusicFileOgg(const char *channelName);
	CSlrMusicFileOgg(const char *channelName, const char *fileName, bool seekable, bool fromResources);
	CSlrMusicFileOgg(const char *channelName, CSlrFile *file, bool seekable);
	virtual ~CSlrMusicFileOgg() override;

	bool Init(const char *fileName, bool seekable, bool fromResources);
	bool Init(CSlrFile *file, bool seekable);
	
	virtual void Play() override;
	virtual void Pause() override;
	virtual void Stop() override;
	virtual void Rewind() override;
	virtual void Seek(double second) override;
	virtual void Seek(u32 minute, u32 second) override;
	virtual u32 GetCurrentSecond() override;
	virtual u64 GetCurrentSampleNum() override;
	virtual void SeekToSample(u64 sampleNum) override;
	virtual void SeekToMillisecond(u64 millisecond) override;
	virtual u64 GetCurrentMillisecond() override;
	virtual bool IsPlaying() override;

	virtual u64 GetLengthSeconds() override;
	virtual u64 GetLengthSamples() override;

	CSlrFile *oggFileHandle;
	OggVorbis_File *oggVorbisData;

	virtual void FillBuffer(int *mixBuffer, u32 numSamples) override;

protected:
	// The base's one decode hook. OggMix already produces interleaved stereo at
	// the file's own rate and pads a short read, so this is a copy and a cap.
	u32 DecodeNativeStereo(s16 *out, u32 frames) override;

public:
	virtual void Mix(int *mixBuffer, u32 numSamples) override;
	virtual void MixFloat(float *mixBufferL, float *mixBufferR, u32 numSamples) override;
	virtual void MixIn(int *mixBuffer, u32 numSamples) override;
	virtual void MixInFloat(float *mixBufferL, float *mixBufferR, u32 numSamples) override;

	void OggMix(u32 numSamples);

	volatile bool isPlaying;

	void LockMutex();
	void UnlockMutex();

	void DeleteVorbisData();
	
	// resource should free memory, @returns memory freed
	virtual u32 ResourceDeactivate(bool async) override;

	// resource should load itself, @returns memory allocated
	virtual u32 ResourceActivate(bool async) override;

	// get size of resource in bytes
	virtual u32 ResourceGetSize() override;

private:
	bool Init(CSlrFile *file, bool seekable, bool takeOwnership);

	int *oggAudioBuffer; //= NULL;
	int *oggMixBuffer;// = NULL;
	int oggAudioBufferPos;// = 0;
	int oggAudioBufferLen;// = 0;

	bool ownsOggFileHandle;
	bool oggVorbisDataInitialized;

	CSlrMutex *oggFileMutex;

};

#endif
