#ifndef _MT_MUSICFILEFACTORY_H_
#define _MT_MUSICFILEFACTORY_H_

// One place that decides which decoder opens a music file.
//
// The routing lived inline in CPlaylistPlaybackManager, and the moment the
// waveform cache needed the same decision it would have become two copies of
// a list of extensions -- which is how a format ends up playable but with no
// waveform, or offered in a file dialog nothing can open. There is now exactly
// one answer to "who plays this?".
//
// The tiers, in order, and the order matters for the same reason it does in
// the video chain: the decoder that ALREADY works for an extension keeps it.
//
//     .ogg                     CSlrMusicFileOgg      (Tremor)
//     .mp3 .flac .wav          CSlrMusicFileMiniaudio (public domain)
//     .m4a .aac .opus .caf ... CSlrMusicFileNative   (the OS, where one exists)

#include "CSlrMusicFile.h"

// Can anything in this build play it? Extension-based -- the playlist asks
// this per row while browsing, so it must not open the file to answer.
bool MusicFile_CanPlay(const char *filePath);

// Opens and returns a ready-to-play source, or NULL. The caller owns it.
// Ogg needs the engine's CSlrFile handle so it can read from a packed resource
// bundle; the others open the path themselves. Pass a handle when you have one
// and NULL when you do not -- an Ogg open without a handle fails rather than
// silently reading a different file.
CSlrMusicFile *MusicFile_Open(const char *filePath, class CSlrFile *oggFileHandle);

// --- AAC availability, and the one case a user can fix ----------------------
//
// Windows N and KN ship WITHOUT the media features, so Media Foundation has no
// AAC decoder and .m4a/.aac simply do not open. That is a real configuration
// on machines sold in the EU and Korea, not a corner case, and the user CAN
// fix it -- which is the whole reason it is worth surfacing rather than
// failing quietly.
//
// Shaped after CImageData::GetHeifCodecInstallUrl(), deliberately: a host
// already renders that notice and should not need a second idiom.
enum EMusicAacAvailability
{
	MUSIC_AAC_AVAILABLE = 0,        // decodes; nothing to say
	MUSIC_AAC_MISSING_SYSTEM_CODEC, // the fixable one -- the install URL is non-NULL
	MUSIC_AAC_NOT_BUILT,            // no OS decoder in this build at all (Linux)
};

EMusicAacAvailability MusicFile_GetAacAvailability();

// A localisation KEY, never display copy. NULL when there is nothing to say.
const char *MusicFile_GetAacUnavailableReasonKey();

// NULL, not "", when there is nothing to install -- `if (url)` is the whole
// test a caller needs.
const char *MusicFile_GetAacInstallUrl();

#endif
