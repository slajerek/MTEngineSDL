#include "DBG_Log.h"
#include "CMidiInKeyboard.h"
#include "CSlrString.h"

void slrMidiCallback( double deltatime, std::vector< unsigned char > *message, void *userData );

CMidiInKeyboard::CMidiInKeyboard(int portNum, CMidiInKeyboardCallback *callback)
{
	this->Init(portNum, callback);
}

std::list<CSlrString *> *CMidiInKeyboard::EnumerateAvailablePorts(CSlrString **errorString)
{
	LOGD("CMidiInKeyboard::EnumerateAvailablePorts");
	
	std::list<CSlrString *> *availablePorts = new std::list<CSlrString *>();

	RtMidiIn *midiIn;
	
	try
	{
		midiIn = new RtMidiIn();
		
		unsigned int nPorts = midiIn->getPortCount();
		LOGD("There are %d MIDI input sources available.", nPorts);
		
		std::string portName;
		for (u32 i=0; i < nPorts; i++)
		{
			portName = midiIn->getPortName(i);
			LOGD("%d = '%s'", i+1, portName.c_str());
			CSlrString *strPortName = new CSlrString(portName.c_str());
			availablePorts->push_back(strPortName);
		}
	}
	catch (RtMidiError &error)
	{
		LOGError("CMidiInKeyboard::CMidiInKeyboard: error %s", error.getMessage().c_str());
		*errorString = new CSlrString(error.getMessage().c_str());
		
		return availablePorts;
	}
	delete midiIn;

	LOGD("CMidiInKeyboard::EnumerateAvailablePorts done");

	return availablePorts;
}

void CMidiInKeyboard::Init(int portNum, CMidiInKeyboardCallback *callback)
{
	this->callback = callback;
	this->Init(portNum);
}

void CMidiInKeyboard::Init(int portNum)
{
	LOGD("CMidiInKeyboard::Init: portNum=%d", portNum);
	errorString = NULL;
	
	try
	{
#if defined(MACOS)
		midiIn = new RtMidiIn(RtMidi::MACOSX_CORE);
#elif defined(WIN32)
		midiIn = new RtMidiIn(RtMidi::WINDOWS_MM);
#elif defined(LINUX)
		midiIn = new RtMidiIn(RtMidi::LINUX_ALSA);
#endif
		midiIn->openPort(portNum);
		//midiIn->ignoreTypes( false, false, false )
		midiIn->setCallback( &slrMidiCallback, (void*)this );
		
		std::string portNameStr = midiIn->getPortName(portNum);
		strncpy(deviceName, portNameStr.c_str(), 512);
		
	}
	catch (RtMidiError &error)
	{
		LOGError("CMidiInKeyboard::CMidiInKeyboard: error %s", error.getMessage().c_str());
		LOGTODO("convert errorMessage: std::string to CSlrString");
		errorString = new CSlrString(error.getMessage().c_str());
		delete midiIn;
		midiIn = NULL;
		
		deviceName[0] = 0x00;
	}
	
	LOGD("CMidiInKeyboard::CMidiInKeyboard done");

}

void slrMidiCallback( double deltatime, std::vector< unsigned char > *message, void *userData )
{
//     Voice Message           Status Byte      Data Byte1          Data Byte2
//     -------------           -----------   -----------------   -----------------
//     Note off                      8x      Key number          Note Off velocity
//     Note on                       9x      Key number          Note on velocity
//     Polyphonic Key Pressure       Ax      Key number          Amount of pressure
//     Control Change (switch)       Bx      Controller number   Controller value
//     Program Change                Cx      Program number      None
//     Channel Pressure              Dx      Pressure value      None
//     Pitch Bend                    Ex      MSB                 LSB
	
	unsigned int nBytes = message->size();
	for (unsigned int i=0; i < nBytes; i++)
	{
		LOGD("Byte %d = 0x%2.2x", i, (int)message->at(i));
	}

	if ( nBytes == 3 )
	{
		LOGD("stamp = %f", deltatime);
		u8 status = (int)message->at(0);
		u8 data1 = (int)message->at(1);
		u8 data2 = (int)message->at(2);
		
		u8 cmd = (status & 0xF0);
		u8 channel = (status & 0x0F);
		
		if (cmd == 0x90)
		{
			// channel note on
			int note = data1;
			int pressure = data2;
			
			CMidiInKeyboard *midiInKeyboard = (CMidiInKeyboard*)userData;
			CMidiInKeyboardCallback *callback = midiInKeyboard->callback;
			callback->MidiInKeyboardCallbackNoteOn(channel, note, pressure);
		}
		else if (cmd == 0x80)
		{
			// channel note off
			int note = data1;
			int pressure = data2;
			CMidiInKeyboard *midiInKeyboard = (CMidiInKeyboard*)userData;
			CMidiInKeyboardCallback *callback = midiInKeyboard->callback;
			callback->MidiInKeyboardCallbackNoteOff(channel, note, pressure);
		}
	}
}

CMidiInKeyboard::~CMidiInKeyboard()
{
	if (midiIn != NULL)
		delete midiIn;
	
	if (errorString != NULL)
		delete errorString;
}



void CMidiInKeyboardCallback::MidiInKeyboardCallbackNoteOn(int channel, int note, int pressure)
{
}

void CMidiInKeyboardCallback::MidiInKeyboardCallbackNoteOff(int channel, int note, int pressure)
{
}

void CMidiInKeyboardCallback::MidiInKeyboardCallbackControlChange(int knobNum, int value)
{
}

void CMidiInKeyboardCallback::MidiInKeyboardCallbackPitchBend(int value)
{
}

