#ifndef _CViewWaveform_h_
#define _CViewWaveform_h_

#include "SYS_Defs.h"
#include "CGuiView.h"
#include "CSlrFont.h"

#include <vector>
#include <list>

class CWaveformData;

class CViewWaveform : public CGuiView
{
public:
	CViewWaveform(float posX, float posY, float posZ, float sizeX, float sizeY, int waveformDataLength);
	CViewWaveform(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY, CWaveformData *waveform);
	~CViewWaveform();

	bool calculateWaveform;

	CWaveformData *waveform;

	const char *channelName;
	CSlrFont *font;
		
	void AddSample(i16 sample);
	
	void UpdateWaveform();
	void Render();
		
	void SetChannelName(CSlrFont *font, const char *channelName);
	void SetRenderMutex(CSlrMutex *renderMutex);
};


#endif
