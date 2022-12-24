#include "CAudioChannel.h"
#include "SYS_Main.h"
#include "CSlrString.h"
#include "SYS_DefaultConfig.h"

// Note, this may evntually cause a bug. Check is not performed for performance purposes as we assume the OS buffer size will not change
#define SKIP_BUFFER_SIZE_CHECK
#define DEFAULT_BUFFER_NUM_SAMPLES 4096

//#define DIVIDE_VOLUME_BY_NUM_CHANNELS

CAudioChannel::CAudioChannel(const char *name)
{
	this->name = STRALLOC(name);

	this->isActive = false;
	this->bypass = false;
	this->destroyMe = false;
	this->removeMe = false;
	this->isMuted = false;
	this->volume = 1.0f;
	this->waveformData = NULL;
		
	this->CreateChannelBuffer(DEFAULT_BUFFER_NUM_SAMPLES);
	GetDefaultValuesFromAppConfig();
}

CAudioChannel::~CAudioChannel()
{
	STRFREE(name);
}

void CAudioChannel::GetDefaultValuesFromAppConfig()
{
	char *buf = SYS_GetCharBuf();

	sprintf(buf, "AudioChannel-%s-mute", name);
	gApplicationDefaultConfig->GetBool(buf, &isMuted, false);
	sprintf(buf, "AudioChannel-%s-volume", name);
	gApplicationDefaultConfig->GetFloat(buf, &volume, 1.0f);

	SYS_ReleaseCharBuf(buf);
}

void CAudioChannel::StoreValuesToAppConfig()
{
	char *buf = SYS_GetCharBuf();

	sprintf(buf, "AudioChannel-%s-mute", name);
	gApplicationDefaultConfig->SetBool(buf, &isMuted);
	sprintf(buf, "AudioChannel-%s-volume", name);
	gApplicationDefaultConfig->SetFloat(buf, &volume);

	SYS_ReleaseCharBuf(buf);
}

void CAudioChannel::CreateChannelBuffer(u32 numSamples)
{
	channelBufferNumSamples = numSamples;
	this->channelBuffer = new int[numSamples];
}

void CAudioChannel::Start()
{
	this->bypass = false;
}

void CAudioChannel::Stop()
{
	this->bypass = true;
}

// for backwards-compatibility reasons
void CAudioChannel::Mix(int *mixBuffer, u32 numSamples)
{
	this->FillBuffer(mixBuffer, numSamples);
}

void CAudioChannel::MixFloat(float *mixBufferL, float *mixBufferR, u32 numSamples)
{
	this->FillBufferFloat(mixBufferL, mixBufferR, numSamples);
}

void CAudioChannel::FillBuffer(int *mixBuffer, u32 numSamples)
{
	SYS_FatalExit("abstract CAudioChannel::Mix");
}

void CAudioChannel::FillBufferFloat(float *mixBufferL, float *mixBufferR, u32 numSamples)
{
	SYS_FatalExit("abstract CAudioChannel::MixFloat");
}


void CAudioChannel::MixIn(int *mixBuffer, u32 numSamples, int numAudioChannels)
{
	// we do not need to clear the buffer
//	memset(mixBuffer, 0x00, numSamples*4);

	// for performance purposes this may not be performed
#if !defined(SKIP_BUFFER_SIZE_CHECK)
	if (numSamples > channelBufferNumSamples)
	{
		delete [] this->channelBuffer;
		CreateChannelBuffer(numSamples);
	}
#endif
	
	this->FillBuffer(channelBuffer, numSamples);
	
	if (isMuted)
	{
		return;
	}
	
	i16 *inPtr = (i16*)channelBuffer;
	i16 *outPtr = (i16*)mixBuffer;
	
	if (volume == 1.0f)
	{
		for (u32 i = 0; i < numSamples; i++)
		{
	//		mixBuffer[i] = channelBuffer[i];
			
#if defined(DIVIDE_VOLUME_BY_NUM_CHANNELS)
			int sL = (int)(*inPtr++)/numAudioChannels + (int)(*outPtr);
			int sR = (int)(*inPtr++)/numAudioChannels + (int)(*(outPtr+1));
#else
			int sL = (int)*inPtr++ + (int)(*outPtr);
			int sR = (int)*inPtr++ + (int)(*(outPtr+1));
#endif
			*outPtr = sL; outPtr++;
			*outPtr = sR; outPtr++;
		}
	}
	else
	{
		for (u32 i = 0; i < numSamples; i++)
		{
	//		mixBuffer[i] = channelBuffer[i];
						
#if defined(DIVIDE_VOLUME_BY_NUM_CHANNELS)
			i16 mL = (i16)(float)(*inPtr++)/(float)numAudioChannels * volume;
			i16 mR = (i16)(float)(*inPtr++)/(float)numAudioChannels * volume;
#else
			i16 mL = (i16)(float)*inPtr++ * volume;
			i16 mR = (i16)(float)*inPtr++ * volume;
#endif
			
			int sL = (int)mL + (int)(*outPtr);
			int sR = (int)mR + (int)(*(outPtr+1));
						
			*outPtr = sL; outPtr++;
			*outPtr = sR; outPtr++;
		}
	}
}

void CAudioChannel::MixInFloat(float *mixBufferL, float *mixBufferR, u32 numSamples)
{
	SYS_FatalExit("abstract CAudioChannel::MixInFloat");
}

void CAudioChannel::SetWaveformData(CWaveformData *waveformData)
{
	this->waveformData = waveformData;
}
