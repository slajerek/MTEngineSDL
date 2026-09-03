#include "DBG_Log.h"
#include "CMidiInKeyboard.h"
#include "CSlrString.h"

#if defined(LINUX)
#include <unistd.h>

// IS THERE AN ALSA SEQUENCER TO TALK TO AT ALL?
//
// RtMidi's ALSA backend opens /dev/snd/seq in its constructor. When that device
// does not exist -- a container, a CI runner, a server with snd-seq not loaded
// -- it does not fail cleanly: it carries on with a null sequencer handle and
// SEGFAULTS inside libasound, which is not an RtMidiError and so cannot be
// caught. Measured 2026-09-03 on a CI runner: the last thing the engine logged
// was CMidiInKeyboard::Init, then ALSA's own "open /dev/snd/seq failed", then
// the process died -- with neither the catch block's LOGError nor the trailing
// "done" ever printed, which is what places the fault inside the constructor.
//
// So the question is asked BEFORE constructing, the same way SND_Init asks
// before opening an audio device and carries on without one. A machine with no
// sequencer gets no MIDI and keeps running, which is the behaviour every other
// absent device already gets here.
static bool MIDI_AlsaSequencerAvailable()
{
	return access("/dev/snd/seq", R_OK | W_OK) == 0;
}
#endif

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

std::list<char *> *CMidiInKeyboard::EnumerateAvailablePorts()
{
	LOGD("CMidiInKeyboard::EnumerateAvailablePorts");
	
	std::list<char *> *availablePorts = new std::list<char *>();

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
			char *strPortName = STRALLOC(portName.c_str());
			availablePorts->push_back(strPortName);
		}
	}
	catch (RtMidiError &error)
	{
		LOGError("CMidiInKeyboard::CMidiInKeyboard: error %s", error.getMessage().c_str());
//		*errorString = new CSlrString(error.getMessage().c_str());
		
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

	// NULL BEFORE ANYTHING ELSE. This member was never initialised and the
	// catch below left it alone (the two lines that would have cleared it were
	// commented out), so a construction that threw left a garbage pointer that
	// every later IsOpen()/destructor call was free to dereference.
	midiIn = NULL;
	deviceName[0] = 0x00;

#if defined(LINUX)
	if (!MIDI_AlsaSequencerAvailable())
	{
		LOGWarning("CMidiInKeyboard::Init: no ALSA sequencer (/dev/snd/seq), continuing without MIDI input");
		return;
	}
#endif

	// Assigned to the MEMBER only on full success: openPort() and getPortName()
	// can throw too, and a half-constructed device must not be reachable.
	RtMidiIn *opened = NULL;

	try
	{
#if defined(MACOS)
		opened = new RtMidiIn(RtMidi::MACOSX_CORE);
#elif defined(WIN32)
		opened = new RtMidiIn(RtMidi::WINDOWS_MM);
#elif defined(LINUX)
		opened = new RtMidiIn(RtMidi::LINUX_ALSA);
#endif
		if (opened == NULL)
		{
			LOGWarning("CMidiInKeyboard::Init: no MIDI backend on this platform, continuing without MIDI input");
			return;
		}

		opened->setCallback( &slrMidiCallback, (void*)this );

		opened->openPort(portNum);
		//midiIn->ignoreTypes( false, false, false )
		
		std::string portNameStr = opened->getPortName(portNum);
		strncpy(deviceName, portNameStr.c_str(), 512);

		midiIn = opened;
	}
	catch (RtMidiError &error)
	{
		LOGError("CMidiInKeyboard::CMidiInKeyboard: error %s", error.getMessage().c_str());
		LOGTODO("convert errorMessage: std::string to CSlrString");
		errorString = new CSlrString(error.getMessage().c_str());

		// `opened` is NULL when the constructor threw and a live object when
		// openPort/getPortName did, so this both frees the half-open device and
		// leaves the member at a defined value.
		delete opened;
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

