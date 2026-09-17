// miniaudio's single translation unit, compiled DECODE-ONLY.
//
// WHY IT IS HERE. The store rule forbids FFmpeg in any distributed build, so
// MP3/FLAC/WAV need a decoder whose licence survives an app store. miniaudio is
// public domain / MIT-0 and carries dr_mp3, dr_flac and dr_wav internally, so
// one vendored header covers three formats with no patent or copyleft question.
// (MP3's patents expired in 2017, worldwide.)
//
// WHY DECODE-ONLY, and why that is not a limitation. miniaudio is mostly a
// PLAYBACK library -- CoreAudio, WASAPI, ALSA, PulseAudio backends, a device
// abstraction, a resampler, a node graph. The engine already has all of that:
// SDL owns the device, SND_SoundEngine owns the mixer, MT_AudioResampler owns
// rate conversion. Compiling the device half in would add a second audio stack
// to a process that has one, and roughly 4 MB of source nobody calls.
//
// So MA_NO_DEVICE_IO and friends are the whole point of this file rather than
// an optimisation: what is wanted is ma_decoder, and nothing else.
//
// ONE COPY, NOT TWO. docs/audio-backend-low-latency/implementation-plan.md
// plans miniaudio for the LOW-LATENCY DEVICE path -- the half switched off
// here. If that lands, it must reuse this header and flip the defines, not
// vendor a second copy at a second version.

#define MA_NO_DEVICE_IO        // no CoreAudio/WASAPI/ALSA backends: SDL owns the device
#define MA_NO_THREADING        // no worker threads: the engine's mixer thread drives us
#define MA_NO_ENCODING         // decode only; nothing here writes audio files
#define MA_NO_GENERATION       // no waveform/noise generators
#define MA_NO_RESOURCE_MANAGER // the engine owns caching and streaming
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_API                 // default linkage

// Only where the music player is compiled in. MT_CAP_MUSIC_PLAYER=0 apps
// (c64d) then carry no ma_* symbol at all, which is exactly what
// the capability's forbidden_when_off pattern asserts -- an assertion that
// would be a lie if this TU built unconditionally.
#if !defined(MT_CAP_MUSIC_PLAYER) || MT_CAP_MUSIC_PLAYER
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#endif
