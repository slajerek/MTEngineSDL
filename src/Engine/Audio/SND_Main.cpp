#include "SND_Main.h"
#include "SND_SoundEngine.h"
#include "CSlrMusicFileOgg.h"
#include "SYS_Main.h"

std::list<CAudioChannel *> *gMainMixerAudioChannels = NULL;

void SND_MainInitialize()
{
	LOGA("SND_MainInitialize");
	gMainMixerAudioChannels = new std::list<CAudioChannel *>();
}

void SND_MainMixer(int *mixBuffer, u32 numSamples)
{
	//LOGD("SND_MainMixer");
	
	memset(mixBuffer, 0x00, numSamples*4);
	
	for (std::list<CAudioChannel *>::iterator itAudioChannel = gMainMixerAudioChannels->begin();
		 itAudioChannel !=  gMainMixerAudioChannels->end(); )
	{
		CAudioChannel *audioChannel = *itAudioChannel;

		std::list<CAudioChannel *>::iterator itCurrentAudioChannel = itAudioChannel;
		itAudioChannel++;

		if (audioChannel->destroyMe)
		{
			gMainMixerAudioChannels->erase(itCurrentAudioChannel);
			delete audioChannel;
			continue;
		}

		if (audioChannel->removeMe)
		{
			gMainMixerAudioChannels->erase(itCurrentAudioChannel);
			continue;
		}
			
		if (audioChannel->bypass)
			continue;

//		LOGD("mixin channel: '%s'", audioChannel->name);
		audioChannel->MixIn(mixBuffer, numSamples, gMainMixerAudioChannels->size());
	}
	
	//LOGD("SND_MainMixer done");
}

void SND_AddChannel(CAudioChannel *channel)
{
	gSoundEngine->LockMutex("SND_AddChannel");
	SND_AddChannel_NoMutex(channel);
	gSoundEngine->UnlockMutex("SND_AddChannel");
}

void SND_RemoveChannel(CAudioChannel *channel)
{
	gSoundEngine->LockMutex("SND_RemoveChannel");
	SND_RemoveChannel_NoMutex(channel);
	gSoundEngine->UnlockMutex("SND_RemoveChannel");
}

void SND_AddChannel_NoMutex(CAudioChannel *channel)
{
	LOGA("SND_AddChannel_NoMutex: %s", channel->name);
	gMainMixerAudioChannels->push_back(channel);
	channel->isActive = true;
}

void SND_RemoveChannel_NoMutex(CAudioChannel *channel)
{
	LOGA("SND_RemoveChannel_NoMutex: %s", channel->name);
	gMainMixerAudioChannels->remove(channel);
	channel->isActive = false;
}
