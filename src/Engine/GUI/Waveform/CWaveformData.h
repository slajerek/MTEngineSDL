#ifndef _CWaveformData_h_
#define _CWaveformData_h_

#include "SYS_Defs.h"
#include "GUI_Main.h"
// TODO: make a template with sample size

class CWaveformData
{
public:
	CWaveformData(int dataLength);
	~CWaveformData();
	signed short *waveformData;
	signed short *waveformDataRender;
	int waveformDataLength;
	int waveformPos;
	int waveformTriggerPos;
	bool isMuted;

	void UpdateWaveform();
	
	void CopySampleData();
	void CalculateTriggerPos();
	void AddSample(i16 sample);

	void Render(float posX, float posY, float sizeX, float sizeY, ImU32 color);

	CSlrMutex *renderMutex;
	void SetRenderMutex(CSlrMutex *renderMutex);

private:
	int jac;
};

#endif
