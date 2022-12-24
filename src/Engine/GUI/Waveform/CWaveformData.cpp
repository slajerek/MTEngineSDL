#include "CWaveformData.h"

//#include "M_Circlebuf.h"
//	circlebuf_init(&audioBufferCircle);
//	circlebuf_reserve(&audioBufferCircle, waveformDataLength * sizeof(i16));
//	circlebuf_push_back_zero(&audioBufferCircle, waveformDataLength * sizeof(i16));

CWaveformData::CWaveformData(int dataLength)
{
	this->waveformDataLength = dataLength;
	waveformData = new i16[dataLength];
	waveformDataRender = new i16[dataLength];
	memset(this->waveformData, 0, dataLength*sizeof(i16));
	memset(this->waveformDataRender, 0, dataLength*sizeof(i16));
	waveformPos = 0;
	waveformTriggerPos = dataLength / 2;
	jac = waveformDataLength / 8;
	isMuted = false;

	renderMutex = NULL;
}

CWaveformData::~CWaveformData()
{
	delete [] waveformData;
	delete [] waveformDataRender;
}

void CWaveformData::SetRenderMutex(CSlrMutex *renderMutex)
{
	this->renderMutex = renderMutex;
}

void CWaveformData::UpdateWaveform()
{
	CopySampleData();
	CalculateTriggerPos();
}

void CWaveformData::CopySampleData()
{
	if (renderMutex)
	{
		renderMutex->Lock();
	}

	int p = waveformPos;
	for (int i = 0; i < waveformDataLength; i++)
	{
		if (p == waveformDataLength)
			p = 0;
		waveformDataRender[i] = waveformData[p];
		p++;
	}
	
	if (renderMutex)
	{
		renderMutex->Unlock();
	}
}

void CWaveformData::CalculateTriggerPos()
{
	waveformTriggerPos = 0;
	i16 triggerValue;
	
	{
		i16 sampleMax = 0;
		i16 sampleMin = 255;
		for (int i = jac*2; i < jac*3*2; i++)
		{
			i16 value = waveformDataRender[i];
			if (value > sampleMax)
				sampleMax = value;
			if (value < sampleMin)
				sampleMin = value;
		}
		triggerValue = (sampleMax + sampleMin) / 2;

		// rising edge trigger
		int risingEdgeTrigger = jac*2;
		while (waveformDataRender[risingEdgeTrigger] > triggerValue && risingEdgeTrigger < jac*3*2)	// positive
		{
			risingEdgeTrigger += 1;
		}
		while (waveformDataRender[risingEdgeTrigger] <= triggerValue && risingEdgeTrigger < jac*3*2)	// negative
		{
			risingEdgeTrigger += 1;
		}
		
		waveformTriggerPos = risingEdgeTrigger;
	}
	
	//
	//	printf("----------\n");
	//	printf("waveform data:\n");
	//	for (int i = 0; i < waveformDataLength; i++)
	//	{
	//		printf("%3d: %d\n", i, waveformData[i]);
	//	}
	//	printf("waveform pos %d:\n", waveformPos);
	//	printf("waveform render data:\n");
	//	for (int i = 0; i < waveformDataLength; i++)
	//	{
	//		printf("%3d: %d\n", i, waveformDataRender[i]);
	//	}
//	printf("triggerValue=%d\n", triggerValue);
//	printf("triggerPos=%d\n", triggerPos);
}

void CWaveformData::Render(float posX, float posY, float sizeX, float sizeY, ImU32 color)
{
	ImDrawList *drawList = ImGui::GetWindowDrawList();

	float step = (float)jac*2 / sizeX;
	float samplePos = waveformTriggerPos - jac;

	// first sample
	float value = waveformDataRender[(int)samplePos];
	
	float prevpx = posX;
	float prevpy = posY + (((float)value + 32767.0) / 65536.0) * sizeY;

	samplePos += step;
	
	for (float x = 1.0f; x <= sizeX; x+= 1.0f)
	{
		int sp = (int)samplePos;
		if (sp < 0)
			sp = 0;
		else if (sp >= waveformDataLength)
			sp = waveformDataLength-1;
		
		value = waveformDataRender[sp];
	
		float px = posX + x;
		float py = posY + (((float)value + 32767.0) / 65536.0) * sizeY;
		
		// draw line
		ImVec2 p1(prevpx, prevpy);
		ImVec2 p2(px, py);

		drawList->AddLine(p1, p2, color);

		// next step
		prevpx = px;
		prevpy = py;

		samplePos += step;
		if ((int)samplePos >= waveformDataLength)
		{
			samplePos -= (float)waveformDataLength;
		}
	}
}

void CWaveformData::AddSample(i16 sample)
{
	waveformData[waveformPos] = sample;
	waveformPos = (waveformPos + 1) % waveformDataLength;
}
