#include "CViewWaveform.h"
#include "CWaveformData.h"

// TODO: add synchronization like in SidWiz2: https://github.com/Zeinok/SidWiz2F/blob/master/SidWiz/Form1.cs
// alternatives:
// https://github.com/Zeinok/OVGen
// https://github.com/corrscope/corrscope/tree/master/corrscope
// https://github.com/maxim-zhao/SidWizPlus

// waveform views
CViewWaveform::CViewWaveform(float posX, float posY, float posZ, float sizeX, float sizeY, int waveformDataLength)
: CGuiView(posX, posY, posZ, sizeX, sizeY)
{
	this->waveform = new CWaveformData(waveformDataLength);
	this->calculateWaveform = true;
	
	this->channelName = NULL;
	this->font = NULL;
}

CViewWaveform::CViewWaveform(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY, CWaveformData *waveform)
: CGuiView(name, posX, posY, posZ, sizeX, sizeY)
{
	this->calculateWaveform = false;
	this->waveform = waveform;

	this->channelName = NULL;
	this->font = NULL;
}

CViewWaveform::~CViewWaveform()
{
	if (calculateWaveform)
		delete waveform;
}

void CViewWaveform::UpdateWaveform()
{
	if (calculateWaveform)
	{
		waveform->CopySampleData();
	}
}

void CViewWaveform::Render()
{
	if (calculateWaveform)
	{
		waveform->CalculateTriggerPos();
	}

	ImU32 color;
	
	if (!waveform->isMuted)
	{
		BlitRectangle(posX, posY, posZ, sizeX, sizeY, 0.5f, 0.0f, 0.0f, 1.0f);
		color = 0xEEFFFFFF;
	}
	else
	{
		BlitRectangle(posX, posY, posZ, sizeX, sizeY, 0.3f, 0.3f, 0.3f, 1.0f);
		color = 0x4C4C4CFF;
	}
	
	waveform->Render(posX, posY, sizeX, sizeY, color);
	
	if (channelName)
	{
		font->BlitText(channelName, posX, posY, posZ, 6.0f);
	}
}

void CViewWaveform::AddSample(i16 sample)
{
	waveform->AddSample(sample);
}

void CViewWaveform::SetChannelName(CSlrFont *font, const char *channelName)
{
	this->font = font;
	this->channelName = channelName;
}

void CViewWaveform::SetRenderMutex(CSlrMutex *renderMutex)
{
	this->waveform->SetRenderMutex(renderMutex);
}
