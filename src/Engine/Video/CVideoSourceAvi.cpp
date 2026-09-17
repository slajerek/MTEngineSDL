#include "CVideoSourceAvi.h"

#include "IVideoPacketDecoder.h"
#include "MT_AnnexB.h"
#include "DBG_Log.h"

#if defined(__APPLE__)
#include "CVideoDecoderHEVCVT.h"
#include "CAudioDecoderCompressedApple.h"
#elif defined(_WIN32)
#include "CVideoDecoderHEVCMF.h"
#include "CAudioDecoderCompressedMF.h"
#endif

#include <cctype>
#include <cstring>
#include <string>

// ============================================================================
// RIFF primitives
// ============================================================================
namespace
{
	#define MT_FOURCC(a,b,c,d) ((u32)(a) | ((u32)(b) << 8) | ((u32)(c) << 16) | ((u32)(d) << 24))

	const u32 kRIFF = MT_FOURCC('R','I','F','F');
	const u32 kAVI  = MT_FOURCC('A','V','I',' ');
	const u32 kLIST = MT_FOURCC('L','I','S','T');
	const u32 kHDRL = MT_FOURCC('h','d','r','l');
	const u32 kSTRL = MT_FOURCC('s','t','r','l');
	const u32 kSTRH = MT_FOURCC('s','t','r','h');
	const u32 kSTRF = MT_FOURCC('s','t','r','f');
	const u32 kMOVI = MT_FOURCC('m','o','v','i');
	const u32 kIDX1 = MT_FOURCC('i','d','x','1');
	const u32 kREC  = MT_FOURCC('r','e','c',' ');
	const u32 kVIDS = MT_FOURCC('v','i','d','s');
	const u32 kAUDS = MT_FOURCC('a','u','d','s');

	// AVI is little-endian everywhere, on every platform that writes it.
	bool ReadU32(FILE *f, u32 &out)
	{
		u8 b[4];
		if (fread(b, 1, 4, f) != 4) return false;
		out = (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24);
		return true;
	}
	u32 GetU32(const u8 *p) { return (u32)p[0] | ((u32)p[1]<<8) | ((u32)p[2]<<16) | ((u32)p[3]<<24); }
	u16 GetU16(const u8 *p) { return (u16)((u32)p[0] | ((u32)p[1]<<8)); }

	// The two ASCII digits that open every chunk id in `movi` are the stream
	// number: "00dc" is stream 0 video, "01wb" is stream 1 audio.
	int StreamIdOfChunk(u32 ckid)
	{
		const int hi = (int)((ckid      ) & 0xFF) - '0';
		const int lo = (int)((ckid >>  8) & 0xFF) - '0';
		if (hi < 0 || hi > 9 || lo < 0 || lo > 9) return -1;
		return hi * 10 + lo;
	}
	bool ChunkIsVideo(u32 ckid)
	{
		const u8 c = (u8)((ckid >> 16) & 0xFF), d = (u8)((ckid >> 24) & 0xFF);
		return (c == 'd' && (d == 'c' || d == 'b'));   // ##dc compressed, ##db uncompressed
	}
	bool ChunkIsAudio(u32 ckid)
	{
		const u8 c = (u8)((ckid >> 16) & 0xFF), d = (u8)((ckid >> 24) & 0xFF);
		return (c == 'w' && d == 'b');
	}

	// FourCC -> codec, case-insensitively: the same codec appears as XVID,
	// xvid and XviD in the wild, and a case-sensitive table would refuse two
	// thirds of the files that exist.
	EMediaCodecId VideoCodecOfFourCC(u32 fcc, std::string &nameOut)
	{
		char t[5] = { (char)(fcc & 0xFF), (char)((fcc>>8)&0xFF),
					  (char)((fcc>>16)&0xFF), (char)((fcc>>24)&0xFF), 0 };
		for (int i = 0; i < 4; i++) t[i] = (char)toupper((unsigned char)t[i]);
		const std::string s(t);
		nameOut = s;

		// MPEG-4 Part 2. All of these are the same bitstream under different
		// vendor badges -- mapping them onto one decoder is what every player
		// does, and VideoToolbox exposes it only as 'mp4v'.
		if (s == "XVID" || s == "DIVX" || s == "DX50" || s == "FMP4" ||
			s == "MP4V" || s == "MP4S" || s == "M4S2" || s == "3IV2" ||
			s == "BLZ0" || s == "DXGM" || s == "FFDS")
		{
			nameOut = "mpeg4"; return EMediaCodecId::MPEG4Part2;
		}
		if (s == "MJPG" || s == "JPEG" || s == "AVRN" || s == "MJPA")
		{
			nameOut = "mjpeg"; return EMediaCodecId::MJPEG;
		}
		if (s == "H264" || s == "X264" || s == "AVC1" || s == "DAVC")
		{
			nameOut = "h264"; return EMediaCodecId::H264;
		}
		return EMediaCodecId::Unknown;
	}

	// Everything before the first VOP (0x000001B6) or GOV (0x000001B3) start
	// code is the MPEG-4 configuration: VOS, Visual Object, VOL, and the
	// user-data the encoder signs. MEASURED against a real file -- ffmpeg's
	// own esds for it carries exactly these 47 bytes and stops before the GOV.
	size_t Mpeg4ConfigLength(const u8 *d, size_t n)
	{
		if (d == NULL || n < 8) return 0;
		// It must actually START with a configuration start code, or this is
		// a chunk that begins with a frame and there is nothing to extract.
		if (!(d[0] == 0 && d[1] == 0 && d[2] == 1 &&
			  (d[3] == 0xB0 || d[3] == 0xB5 || d[3] == 0x20)))
			return 0;
		for (size_t i = 0; i + 4 <= n; i++)
		{
			if (d[i] == 0 && d[i+1] == 0 && d[i+2] == 1 &&
				(d[i+3] == 0xB6 || d[i+3] == 0xB3))
				return i;
		}
		return 0;
	}

	EMediaCodecId AudioCodecOfTag(u16 tag)
	{
		switch (tag)
		{
			case 0x0001: return EMediaCodecId::PCM_S16LE;   // WAVE_FORMAT_PCM
			case 0x0050: return EMediaCodecId::MP2;
			case 0x0055: return EMediaCodecId::MP3;
			case 0x2000: return EMediaCodecId::AC3;
			default:     return EMediaCodecId::Unknown;
		}
	}
}

// ============================================================================
// Construction
// ============================================================================
CVideoSourceAvi::CVideoSourceAvi() {}
CVideoSourceAvi::~CVideoSourceAvi() { Close(); }

bool CVideoSourceAvi::CanProbablyOpen(const char *filePath)
{
	if (filePath == NULL || filePath[0] == '\0')
		return false;
	std::string p(filePath);
	size_t dot = p.find_last_of('.');
	if (dot == std::string::npos)
		return false;
	std::string ext = p.substr(dot);
	for (char &c : ext) c = (char)tolower((unsigned char)c);
	return ext == ".avi" || ext == ".divx";
}

// A failed Open() must still be able to SAY WHY. Close() resets everything
// including errorReason -- correct for a reopen, fatal for reporting -- so the
// reason is carried across it here. Without this the lane refused files with
// an empty string, and "declined for no stated reason" is indistinguishable
// from a bug in the caller.
// BY VALUE, NOT BY REFERENCE, and that is not a style choice. Callers pass
// `errorReason` itself; Close() clears it, and a reference parameter would
// then assign the member from the member it had just emptied. That is exactly
// how this refused two perfectly good files with an empty reason.
bool CVideoSourceAvi::FailOpen(std::string reason)
{
	Close();
	errorReason = reason;
	return false;
}

// ============================================================================
// Open
// ============================================================================
bool CVideoSourceAvi::Open(const char *filePath)
{
	Close();

	file = fopen(filePath, "rb");
	if (!file)
	{
		errorReason = "failed to open file";
		return false;
	}

	u32 riff = 0, riffSize = 0, form = 0;
	if (!ReadU32(file, riff) || !ReadU32(file, riffSize) || !ReadU32(file, form) ||
		riff != kRIFF || form != kAVI)
	{
		return FailOpen("not a RIFF/AVI file");
	}

	if (!ParseHeaders())
		return FailOpen(errorReason.empty() ? "could not parse the AVI headers" : errorReason);

	if (videoStreamId < 0)
		return FailOpen("no decodable video stream in this AVI");

	// THE CONFIGURATION IS IN-BAND MORE OFTEN THAN NOT. ffmpeg's own AVI
	// writer puts nothing after the BITMAPINFOHEADER and leads the first
	// video chunk with the VOS/VOL header instead; both are ordinary AVI, and
	// a lane that only handled the first would refuse most files that exist.
	if (videoCodec == EMediaCodecId::MPEG4Part2 && videoCodecConfig.empty())
	{
		std::vector<u8> firstChunk;
		if (PeekFirstVideoChunk(firstChunk))
		{
			const size_t configLen = Mpeg4ConfigLength(firstChunk.data(), firstChunk.size());
			if (configLen > 0)
			{
				videoCodecConfig.assign(firstChunk.begin(), firstChunk.begin() + configLen);
				LOGD("CVideoSourceAvi: took %zu bytes of MPEG-4 configuration from the first chunk",
					 configLen);
			}
		}
	}

	if (!InitVideoDecoder())
	{
		// InitVideoDecoder names the codec, which is the whole point for DIV3
		// and for Annex-B H.264.
		return FailOpen(errorReason);
	}

	if (audioStreamId >= 0 && !InitAudioDecoder())
	{
		// Silent video beats no video. The picture is what the user asked for.
		LOGWarning("CVideoSourceAvi: audio stream present but not decodable here; continuing silent");
		audioStreamId = -1;
		info.hasAudio = false;
	}

	readPos = moviStart;
	nextVideoFrameNumber = 0;
	audioFramesEmitted = 0;
	errorReason.clear();

	LOGD("CVideoSourceAvi: '%s' %s %dx%d %.2ffps, audio '%s' %u Hz x%d, %zu keyframes indexed",
		 filePath, info.videoCodecName.c_str(), info.width, info.height, info.fps,
		 info.audioCodecName.c_str(), info.audioSampleRate, info.audioChannels,
		 videoIndex.size());
	return true;
}

// ============================================================================
// Header parsing
// ============================================================================
bool CVideoSourceAvi::ParseHeaders()
{
	int streamIndex = 0;
	for (;;)
	{
		u32 ckid = 0, cksize = 0;
		if (!ReadU32(file, ckid) || !ReadU32(file, cksize))
			break;                       // clean end of file
		const long dataPos = ftell(file);

		if (ckid == kLIST)
		{
			u32 listType = 0;
			if (!ReadU32(file, listType))
				break;
			if (listType == kHDRL)
			{
				// Walk the stream lists inside. The `avih` main header is
				// skipped deliberately: its dwMicroSecPerFrame is advisory and
				// often wrong, while each stream's dwScale/dwRate is not.
				const long hdrlEnd = dataPos + (long)cksize;
				while (ftell(file) + 8 <= hdrlEnd)
				{
					u32 sub = 0, subSize = 0;
					if (!ReadU32(file, sub) || !ReadU32(file, subSize)) break;
					const long subData = ftell(file);
					if (sub == kLIST)
					{
						u32 subType = 0;
						if (!ReadU32(file, subType)) break;
						if (subType == kSTRL)
							ParseStreamList(subData + (long)subSize, streamIndex++);
					}
					fseek(file, subData + (long)subSize + ((subSize & 1) ? 1 : 0), SEEK_SET);
				}
			}
			else if (listType == kMOVI)
			{
				moviStart = ftell(file);
				moviEnd = dataPos + (long)cksize;
			}
		}
		else if (ckid == kIDX1)
		{
			ReadIdx1(dataPos, cksize);
		}

		// Chunks are word-aligned; the pad byte is not counted in cksize, and
		// forgetting it desynchronises the whole file by one byte.
		const long next = dataPos + (long)cksize + ((cksize & 1) ? 1 : 0);
		if (next <= dataPos) break;
		if (fseek(file, next, SEEK_SET) != 0) break;
	}
	return moviStart > 0 && moviEnd > moviStart;
}

bool CVideoSourceAvi::ParseStreamList(long listEnd, int streamIndex)
{
	u32 fccType = 0, fccHandler = 0, scale = 1, rate = 0, length = 0;
	bool haveStrh = false;

	while (ftell(file) + 8 <= listEnd)
	{
		u32 ckid = 0, cksize = 0;
		if (!ReadU32(file, ckid) || !ReadU32(file, cksize)) break;
		const long dataPos = ftell(file);

		if (ckid == kSTRH && cksize >= 40)
		{
			u8 h[40];
			if (fread(h, 1, 40, file) == 40)
			{
				fccType    = GetU32(h + 0);
				fccHandler = GetU32(h + 4);
				scale      = GetU32(h + 20);
				rate       = GetU32(h + 24);
				length     = GetU32(h + 32);
				haveStrh = true;
			}
		}
		else if (ckid == kSTRF && haveStrh && cksize > 0)
		{
			std::vector<u8> strf(cksize);
			if (fread(strf.data(), 1, cksize, file) == cksize)
			{
				if (fccType == kVIDS && videoStreamId < 0 && cksize >= 40)
				{
					// biCompression is the real codec id; fccHandler in strh
					// is frequently 0 or a lie, so it is only the fallback.
					u32 fcc = GetU32(strf.data() + 16);
					if (fcc == 0) fcc = fccHandler;
					std::string name;
					const EMediaCodecId codec = VideoCodecOfFourCC(fcc, name);

					videoStreamId = streamIndex;
					videoCodec = codec;
					info.videoCodecName = name;
					info.width  = (int)GetU32(strf.data() + 4);
					info.height = (int)(i32)GetU32(strf.data() + 8);
					if (info.height < 0) info.height = -info.height;  // bottom-up DIB
					if (cksize > 40)
						videoCodecConfig.assign(strf.begin() + 40, strf.end());

					if (rate > 0 && scale > 0)
					{
						info.fps = (double)rate / (double)scale;
						videoFrameDuration = (double)scale / (double)rate;
						if (length > 0)
							info.duration = (double)length * videoFrameDuration;
					}
				}
				else if (fccType == kAUDS && audioStreamId < 0 && cksize >= 16)
				{
					const u16 tag = GetU16(strf.data() + 0);
					audioCodec = AudioCodecOfTag(tag);
					if (audioCodec != EMediaCodecId::Unknown)
					{
						audioStreamId = streamIndex;
						audioChannels = (int)GetU16(strf.data() + 2);
						audioSampleRate = GetU32(strf.data() + 4);
						audioBitsPerSample = (int)GetU16(strf.data() + 14);
						info.hasAudio = true;
						info.audioChannels = audioChannels;
						info.audioSampleRate = audioSampleRate;
						info.audioCodecName =
							(audioCodec == EMediaCodecId::MP3) ? "mp3" :
							(audioCodec == EMediaCodecId::MP2) ? "mp2" :
							(audioCodec == EMediaCodecId::AC3) ? "ac3" : "pcm";
					}
					else
					{
						LOGWarning("CVideoSourceAvi: audio wFormatTag 0x%04x is not decodable here", tag);
					}
				}
			}
		}

		const long next = dataPos + (long)cksize + ((cksize & 1) ? 1 : 0);
		if (next <= dataPos) break;
		if (fseek(file, next, SEEK_SET) != 0) break;
	}
	return true;
}

bool CVideoSourceAvi::ReadIdx1(long idx1Pos, u32 idx1Size)
{
	if (idx1Size < 16 || videoStreamId < 0)
		return false;
	if (fseek(file, idx1Pos, SEEK_SET) != 0)
		return false;

	const u32 count = idx1Size / 16;
	std::vector<u8> raw((size_t)idx1Size);
	if (fread(raw.data(), 1, idx1Size, file) != idx1Size)
		return false;

	// THE OFFSET BASE IS NOT SPECIFIED CONSISTENTLY. Most writers store it
	// relative to the `movi` list's data start minus 4 (i.e. the offset of the
	// chunk HEADER within the list); a minority store absolute file offsets.
	// Detect it rather than guess: an entry whose offset already lands on its
	// own chunk id is absolute.
	long base = moviStart - 4;
	if (count > 0)
	{
		const u32 firstOffset = GetU32(raw.data() + 8);
		const u32 firstCkid   = GetU32(raw.data() + 0);
		u32 probe = 0;
		if (fseek(file, (long)firstOffset, SEEK_SET) == 0 && ReadU32(file, probe) && probe == firstCkid)
			base = 0;
	}

	u32 frameNumber = 0;
	for (u32 i = 0; i < count; i++)
	{
		const u8 *e = raw.data() + (size_t)i * 16;
		const u32 ckid = GetU32(e + 0);
		const u32 flags = GetU32(e + 4);
		const u32 offset = GetU32(e + 8);
		const u32 size = GetU32(e + 12);
		if (StreamIdOfChunk(ckid) != videoStreamId || !ChunkIsVideo(ckid))
			continue;
		if (flags & 0x00000010)   // AVIIF_KEYFRAME
		{
			SIndexEntry en;
			en.offset = (u32)(base + (long)offset);   // offset of the chunk HEADER
			en.size = size;
			en.keyframe = true;
			en.frameNumber = frameNumber;
			videoIndex.push_back(en);
		}
		frameNumber++;
	}
	// A duration the index disagrees with is the index's, not the header's:
	// dwLength in strh counts the frames the writer INTENDED.
	if (frameNumber > 0 && videoFrameDuration > 0.0 && info.duration <= 0.0)
		info.duration = (double)frameNumber * videoFrameDuration;
	return true;
}

bool CVideoSourceAvi::PeekFirstVideoChunk(std::vector<u8> &out)
{
	out.clear();
	if (!file || moviStart <= 0) return false;
	const long saved = ftell(file);
	long pos = moviStart;
	bool found = false;
	while (pos + 8 <= moviEnd)
	{
		if (fseek(file, pos, SEEK_SET) != 0) break;
		u32 ckid = 0, cksize = 0;
		if (!ReadU32(file, ckid) || !ReadU32(file, cksize)) break;
		const long dataPos = ftell(file);
		if (ckid == kLIST)
		{
			u32 listType = 0;
			if (ReadU32(file, listType) && listType == kREC) { pos = dataPos + 4; continue; }
		}
		else if (StreamIdOfChunk(ckid) == videoStreamId && ChunkIsVideo(ckid) && cksize > 0)
		{
			out.resize(cksize);
			found = (fread(out.data(), 1, cksize, file) == cksize);
			break;
		}
		pos = dataPos + (long)cksize + ((cksize & 1) ? 1 : 0);
	}
	fseek(file, saved, SEEK_SET);
	if (!found) out.clear();
	return found;
}

// ============================================================================
// Decoder setup
// ============================================================================
bool CVideoSourceAvi::InitVideoDecoder()
{
	if (videoCodec == EMediaCodecId::Unknown)
	{
		// NAMED, not "unsupported file". DIV3/MP43 lands here and a user with
		// a DivX 3 file deserves to be told which codec it was.
		errorReason = "no OS decoder for this AVI's video codec (" + info.videoCodecName + ")";
		return false;
	}

#if defined(__APPLE__)
	CVideoDecoderHEVCVT *vt = new CVideoDecoderHEVCVT();
	vt->SetCodec(videoCodec);
	packetDecoder.reset(vt);
#elif defined(_WIN32)
	CVideoDecoderHEVCMF::ECodec c = CVideoDecoderHEVCMF::ECodec::MPEG4V;
	if (videoCodec == EMediaCodecId::MJPEG) c = CVideoDecoderHEVCMF::ECodec::MJPEG;
	else if (videoCodec == EMediaCodecId::H264) c = CVideoDecoderHEVCMF::ECodec::H264;
	packetDecoder.reset(new CVideoDecoderHEVCMF(c));
#else
	errorReason = "no OS video decoder on this platform";
	return false;
#endif

	if (!packetDecoder)
	{
		errorReason = "could not create an OS video decoder";
		return false;
	}
	if (abortPredicate)
		packetDecoder->SetAbortPredicate(abortPredicate);

	// H.264 IN AVI IS USUALLY ANNEX-B with no avcC anywhere in the file, and
	// VideoToolbox will not take that. Refused by name rather than opened to
	// show black -- and refusing lets CVideoPlayer fall through to FFmpeg in
	// a private build, which is exactly what should happen.
	wantsAnnexB = packetDecoder->WantsAnnexB();
	const u8 *initData = videoCodecConfig.empty() ? NULL : videoCodecConfig.data();
	int initSize = (int)videoCodecConfig.size();

	if (videoCodec == EMediaCodecId::H264 && wantsAnnexB)
	{
		if (!MT_AnnexB::ParseConfigRecord(initData, videoCodecConfig.size(), false,
										  annexBParameterSets, nalLengthSize))
		{
			errorReason = "H.264-in-AVI without an avcC configuration record is not supported";
			packetDecoder.reset();
			return false;
		}
		initData = annexBParameterSets.data();
		initSize = (int)annexBParameterSets.size();
	}
	else
	{
		// MPEG-4 Part 2 and MJPEG are not NAL-carried, so no conversion
		// applies to them whatever the decoder says it prefers.
		if (videoCodec != EMediaCodecId::H264)
			wantsAnnexB = false;
	}

	if (!packetDecoder->Init(initData, initSize, info.width, info.height, 2))
	{
		errorReason = "OS decoder rejected this AVI's " + info.videoCodecName +
					  " stream: " + packetDecoder->GetErrorReason();
		packetDecoder.reset();
		return false;
	}
	return true;
}

bool CVideoSourceAvi::InitAudioDecoder()
{
	if (audioCodec == EMediaCodecId::PCM_S16LE)
	{
		// Nothing to decode. 8-bit and 24-bit PCM in AVI exist but are rare
		// enough that converting them is not worth being wrong about
		// untested; they are refused here and the video still plays.
		if (audioBitsPerSample != 16)
		{
			LOGWarning("CVideoSourceAvi: %d-bit PCM is not handled; only 16-bit", audioBitsPerSample);
			return false;
		}
		return true;
	}

#ifdef MT_HAVE_NATIVE_COMPRESSED_AUDIO
	audioDecoder.reset(new CAudioDecoderCompressedNative());
	if (!audioDecoder->Init(audioCodec, audioSampleRate, audioChannels))
	{
		LOGWarning("CVideoSourceAvi: %s decoder init failed: %s",
				   info.audioCodecName.c_str(), audioDecoder->GetErrorReason().c_str());
		audioDecoder.reset();
		return false;
	}
	return true;
#else
	return false;
#endif
}

// ============================================================================
// Reading
// ============================================================================
bool CVideoSourceAvi::ReadNextVideoChunk(std::vector<u8> &out, bool &keyframe, double &pts)
{
	if (!file) return false;

	while (readPos + 8 <= moviEnd)
	{
		if (fseek(file, readPos, SEEK_SET) != 0)
			return false;
		u32 ckid = 0, cksize = 0;
		if (!ReadU32(file, ckid) || !ReadU32(file, cksize))
			return false;
		const long dataPos = ftell(file);
		const long next = dataPos + (long)cksize + ((cksize & 1) ? 1 : 0);

		if (ckid == kLIST)
		{
			// A `rec ` list groups the chunks of one frame for interleaving.
			// Descend into it rather than skipping: its contents are the data.
			u32 listType = 0;
			if (ReadU32(file, listType) && listType == kREC)
			{
				readPos = dataPos + 4;
				continue;
			}
			readPos = next;
			continue;
		}

		const int sid = StreamIdOfChunk(ckid);
		if (sid == videoStreamId && ChunkIsVideo(ckid))
		{
			out.resize(cksize);
			if (cksize > 0 && fread(out.data(), 1, cksize, file) != cksize)
				return false;
			// AVI marks keyframes in the index, not in the chunk. Without an
			// index every frame reads as "not a keyframe", which is only
			// advisory here -- both OS decoders find their own.
			keyframe = false;
			pts = (double)nextVideoFrameNumber * videoFrameDuration;
			nextVideoFrameNumber++;
			readPos = next;
			return true;
		}
		if (sid == audioStreamId && ChunkIsAudio(ckid) && info.hasAudio && cksize > 0)
		{
			chunkScratch.resize(cksize);
			if (fread(chunkScratch.data(), 1, cksize, file) == cksize)
				DecodeAudioChunk(chunkScratch.data(), cksize);
		}
		readPos = next;
	}
	return false;
}

bool CVideoSourceAvi::ReadVideoFrame(SDecodedVideoFrame &out)
{
	if (!packetDecoder)
		return false;

	for (;;)
	{
		std::vector<u8> chunk;
		bool keyframe = false;
		double pts = 0.0;
		if (!ReadNextVideoChunk(chunk, keyframe, pts))
		{
			// Clean end of `movi`: drain the decoder one frame per call, the
			// same contract the other lanes use.
			return packetDecoder->DecodePacket(NULL, out);
		}
		if (chunk.empty())
			continue;   // AVI writes zero-length chunks for dropped frames

		const u8 *feed = chunk.data();
		int feedSize = (int)chunk.size();
		if (wantsAnnexB)
		{
			if (!MT_AnnexB::ConvertPacket(chunk.data(), chunk.size(), nalLengthSize, annexBScratch))
				continue;
			feed = annexBScratch.data();
			feedSize = (int)annexBScratch.size();
		}

		SVideoPacket pkt(feed, feedSize, (i64)(pts * 1000000.0), keyframe);
		if (packetDecoder->DecodePacket(&pkt, out))
			return true;

		const std::string &err = packetDecoder->GetErrorReason();
		if (!err.empty())
		{
			errorReason = err;
			return false;
		}
		// Empty reason: the decoder is buffering. Keep feeding.
	}
}

void CVideoSourceAvi::DecodeAudioChunk(const u8 *data, size_t size)
{
	if (size == 0 || audioSampleRate == 0 || audioChannels <= 0)
		return;

	SDecodedAudio audio;
	audio.channels = audioChannels;
	audio.sampleRate = audioSampleRate;
	audio.pts = (double)audioFramesEmitted / (double)audioSampleRate;

	if (audioCodec == EMediaCodecId::PCM_S16LE)
	{
		const size_t samples = size / sizeof(s16);
		audio.pcm.resize(samples);
		memcpy(audio.pcm.data(), data, samples * sizeof(s16));
	}
#ifdef MT_HAVE_NATIVE_COMPRESSED_AUDIO
	else if (audioDecoder)
	{
		int ch = audioChannels;
		u32 rate = audioSampleRate;
		if (!audioDecoder->DecodeChunk(data, (int)size, audio.pcm, ch, rate))
		{
			LOGError("CVideoSourceAvi: audio decode failed: %s",
					 audioDecoder->GetErrorReason().c_str());
			return;
		}
		// THE DECODER'S ANSWER WINS over the container's. AVI headers are
		// written by muxers and are wrong often enough that trusting them over
		// the decoder is how audio ends up at the wrong pitch.
		if (ch > 0) { audio.channels = ch; info.audioChannels = ch; }
		if (rate > 0) { audio.sampleRate = rate; info.audioSampleRate = rate; }
	}
#endif
	else
	{
		return;
	}

	if (audio.pcm.empty())
		return;   // priming
	audioFramesEmitted += audio.pcm.size() / (size_t)(audio.channels > 0 ? audio.channels : 1);
	pendingAudio.push_back(std::move(audio));
}

bool CVideoSourceAvi::ReadAudio(SDecodedAudio &out)
{
	if (pendingAudio.empty())
		return false;
	out = std::move(pendingAudio.front());
	pendingAudio.pop_front();
	return true;
}

// ============================================================================
// Seek
// ============================================================================
bool CVideoSourceAvi::Seek(double seconds)
{
	if (!file || videoFrameDuration <= 0.0)
		return false;
	if (seconds < 0.0) seconds = 0.0;

	// No index means no keyframe table, and starting a decode from an
	// arbitrary inter-frame produces garbage. Rewinding to the head is the
	// honest answer -- and it is what an unindexed AVI gives every player.
	if (videoIndex.empty())
	{
		readPos = moviStart;
		nextVideoFrameNumber = 0;
	}
	else
	{
		const u32 targetFrame = (u32)(seconds / videoFrameDuration);
		const SIndexEntry *chosen = &videoIndex[0];
		for (const SIndexEntry &e : videoIndex)
		{
			if (e.frameNumber <= targetFrame) chosen = &e;
			else break;
		}
		readPos = (long)chosen->offset;
		nextVideoFrameNumber = chosen->frameNumber;
	}

	if (packetDecoder) packetDecoder->Flush();
#ifdef MT_HAVE_NATIVE_COMPRESSED_AUDIO
	if (audioDecoder) audioDecoder->Init(audioCodec, audioSampleRate, audioChannels);
#endif
	pendingAudio.clear();
	audioFramesEmitted = (u64)((double)nextVideoFrameNumber * videoFrameDuration *
							   (double)audioSampleRate);
	errorReason.clear();
	return true;
}

// ============================================================================
// Abort plumbing / teardown
// ============================================================================
void CVideoSourceAvi::SetAbortPredicate(std::function<bool()> pred)
{
	abortPredicate = pred;
	if (packetDecoder) packetDecoder->SetAbortPredicate(pred);
}

void CVideoSourceAvi::WakeAbort()
{
	if (packetDecoder) packetDecoder->Abort();
}

void CVideoSourceAvi::Close()
{
	packetDecoder.reset();
#ifdef MT_HAVE_NATIVE_COMPRESSED_AUDIO
	audioDecoder.reset();
#endif
	if (file) { fclose(file); file = nullptr; }
	videoIndex.clear();
	videoCodecConfig.clear();
	annexBParameterSets.clear();
	annexBScratch.clear();
	chunkScratch.clear();
	pendingAudio.clear();
	videoStreamId = audioStreamId = -1;
	videoCodec = audioCodec = EMediaCodecId::Unknown;
	audioSampleRate = 0; audioChannels = 0; audioBitsPerSample = 0;
	videoFrameDuration = 0.0;
	moviStart = moviEnd = readPos = 0;
	nextVideoFrameNumber = 0;
	audioFramesEmitted = 0;
	nalLengthSize = 4;
	wantsAnnexB = false;
	info = SVideoInfo();
	errorReason.clear();
}
