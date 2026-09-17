#include "MT_MusicFileFactory.h"
#include "CSlrMusicFileOgg.h"
#include "CSlrFile.h"
#include "DBG_Log.h"
#include <cctype>
#include <string>

#if !defined(MT_CAP_MUSIC_PLAYER) || MT_CAP_MUSIC_PLAYER
#include "CSlrMusicFileMiniaudio.h"
#define MT_FACTORY_HAVE_MINIAUDIO 1
#else
#define MT_FACTORY_HAVE_MINIAUDIO 0
#endif
#include "CSlrMusicFileNative.h"

#if defined(_WIN32) && defined(MT_HAVE_NATIVE_MUSIC)
#include <mfapi.h>
#include <mfidl.h>
#include "MFComThreadGuard.h"
// Linked here rather than relied on from the app's AdditionalDependencies:
// this engine file must build in an app that never listed these, and
// CCameraWindows.cpp already sets the precedent.
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
// Does an AAC decoder actually resolve on THIS machine? Enumerating the
// transform is cheaper and more honest than opening a file, and it is the same
// two-tier shape IsHEVCDecodeAvailable() uses on the video side.
static bool MusicFile_ProbeWindowsAacDecoder()
{
	MFComThreadGuard::EnsureComInitializedForThisThread();
	MFComThreadGuard::EnsureMFStarted();
	MFT_REGISTER_TYPE_INFO inType = { MFMediaType_Audio, MFAudioFormat_AAC };
	IMFActivate **activates = NULL;
	UINT32 count = 0;
	HRESULT hr = MFTEnumEx(MFT_CATEGORY_AUDIO_DECODER, MFT_ENUM_FLAG_SYNCMFT,
						   &inType, NULL, &activates, &count);
	if (activates != NULL)
	{
		for (UINT32 i = 0; i < count; i++)
			if (activates[i]) activates[i]->Release();
		CoTaskMemFree(activates);
	}
	return SUCCEEDED(hr) && count > 0;
}
#endif

static bool IsOggPath(const char *filePath)
{
	if (filePath == NULL) return false;
	std::string p(filePath);
	size_t dot = p.find_last_of('.');
	if (dot == std::string::npos) return false;
	std::string ext = p.substr(dot + 1);
	for (char &c : ext) c = (char)tolower((unsigned char)c);
	return ext == "ogg";
}

bool MusicFile_CanPlay(const char *filePath)
{
	if (filePath == NULL || filePath[0] == '\0')
		return false;
	if (IsOggPath(filePath))
		return true;
#if MT_FACTORY_HAVE_MINIAUDIO
	if (CSlrMusicFileMiniaudio::CanPlay(filePath))
		return true;
#endif
#ifdef MT_HAVE_NATIVE_MUSIC
	if (CSlrMusicFileNative::CanPlay(filePath))
		return true;
#endif
	return false;
}

CSlrMusicFile *MusicFile_Open(const char *filePath, CSlrFile *oggFileHandle)
{
	if (filePath == NULL || filePath[0] == '\0')
		return NULL;

#if MT_FACTORY_HAVE_MINIAUDIO
	if (CSlrMusicFileMiniaudio::CanPlay(filePath))
	{
		CSlrMusicFileMiniaudio *ma = new CSlrMusicFileMiniaudio(filePath);
		if (ma->IsOpen())
			return ma;
		delete ma;
		return NULL;
	}
#endif
#ifdef MT_HAVE_NATIVE_MUSIC
	if (CSlrMusicFileNative::CanPlay(filePath))
	{
		CSlrMusicFileNative *nat = new CSlrMusicFileNative(filePath);
		if (nat->IsOpen())
			return nat;
		delete nat;
		return NULL;
	}
#endif
	if (IsOggPath(filePath))
	{
		// Ogg is the one decoder that reads through the engine's file layer
		// rather than the path, so it cannot be opened without a handle.
		if (oggFileHandle == NULL)
		{
			LOGError("MusicFile_Open: '%s' is Ogg and needs a CSlrFile handle", filePath);
			return NULL;
		}
		CSlrMusicFileOgg *ogg = new CSlrMusicFileOgg(filePath);
		if (ogg->Init(oggFileHandle, true))
			return ogg;
		delete ogg;
		return NULL;
	}
	return NULL;
}

// ============================================================================
// AAC availability
// ============================================================================

EMusicAacAvailability MusicFile_GetAacAvailability()
{
#ifndef MT_HAVE_NATIVE_MUSIC
	// No OS decoder compiled in -- Linux, or a music-off build. Nothing the
	// user can install changes that, so it is reported separately from the
	// fixable case.
	return MUSIC_AAC_NOT_BUILT;
#elif defined(_WIN32)
	// PROBED, not assumed from the SKU. "Is this Windows N?" is the wrong
	// question: a user may have installed the Media Feature Pack already, and a
	// non-N machine can have media features removed. Asking Media Foundation
	// whether an AAC decoder resolves is the only answer that is true for the
	// machine in front of you.
	static int cached = -1;
	if (cached < 0)
		cached = MusicFile_ProbeWindowsAacDecoder() ? 0 : 1;
	return (cached == 0) ? MUSIC_AAC_AVAILABLE : MUSIC_AAC_MISSING_SYSTEM_CODEC;
#else
	// AudioToolbox always has AAC.
	return MUSIC_AAC_AVAILABLE;
#endif
}

const char *MusicFile_GetAacUnavailableReasonKey()
{
	switch (MusicFile_GetAacAvailability())
	{
		case MUSIC_AAC_AVAILABLE:
			return NULL;
		case MUSIC_AAC_MISSING_SYSTEM_CODEC:
			// The one case the user can act on, which is why it is surfaced.
			return "engine.music.aac.unavailable.system_codec_missing";
		case MUSIC_AAC_NOT_BUILT:
		default:
			// The user can do nothing about this. A host may reasonably stay
			// silent rather than explain its own build.
			return "engine.music.aac.unavailable.not_built";
	}
}

const char *MusicFile_GetAacInstallUrl()
{
#if defined(_WIN32)
	// ms-settings:optionalfeatures, NOT a Store product link. The Media Feature
	// Pack is an Optional Feature on modern Windows N, not a Store app -- a
	// Store PFN here would be a silently dead link, which is the worst way for
	// this to be wrong. UNVERIFIED on a real N machine; see HANDOVER-WINDOWS.
	return (MusicFile_GetAacAvailability() == MUSIC_AAC_MISSING_SYSTEM_CODEC)
		   ? "ms-settings:optionalfeatures"
		   : NULL;
#else
	return NULL;
#endif
}
