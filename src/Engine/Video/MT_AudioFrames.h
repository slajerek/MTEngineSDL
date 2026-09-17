#ifndef _MT_AUDIOFRAMES_H_
#define _MT_AUDIOFRAMES_H_

#pragma once

// Where does the next MP3 / MPEG-1 Layer II / AC-3 frame end?
//
// WHY A CONTAINER NEEDS THIS. AVI does not store one audio frame per chunk. A
// single `01wb` chunk routinely holds several MP3 frames, and OS decoders
// take one packet at a time -- so something has to find the boundaries, and
// the only honest place is a frame-header parse. Guessing a constant frame
// size works on CBR files and produces drifting, gradually-desynchronised
// audio on VBR ones, which is the kind of bug nobody reports as a bug.
//
// PLATFORM-NEUTRAL AND SEPARATELY TESTED, for the same reason MT_AnnexB is:
// the Windows lane needs the identical splitting and cannot be compiled on
// the machine this was written on.

#include "SYS_Defs.h"
#include <cstddef>

namespace MT_AudioFrames
{
	// Bytes in the MPEG audio frame starting at `data`, or 0 if `data` does
	// not begin with a valid frame header (or the frame does not fit).
	// Handles MPEG-1/2/2.5 Layers I, II and III -- one function, because the
	// header layout is one layout and splitting them would duplicate the
	// tables that do the work.
	size_t MpegFrameSize(const u8 *data, size_t avail);

	// Bytes in the AC-3 (or E-AC-3) syncframe starting at `data`, or 0.
	size_t Ac3FrameSize(const u8 *data, size_t avail);

	// Scan forward for the next plausible frame start, for resynchronising
	// after a chunk that begins mid-frame or carries junk. Returns the offset,
	// or `avail` when none is found.
	size_t FindMpegSync(const u8 *data, size_t avail);
	size_t FindAc3Sync(const u8 *data, size_t avail);
}

#endif
//_MT_AUDIOFRAMES_H_
