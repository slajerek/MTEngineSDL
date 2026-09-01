#include "CVideoSourceFFmpeg.h"

#if MT_ENABLE_FFMPEG

#include "DBG_Log.h"

#include <cstring>
#include <cstdio>
#include <cmath>
#include <ctime>
#include <cctype>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h> // av_bsf_* -- FFmpeg 7 moved these out of avcodec.h; needed for the hevc_mp4toannexb plumbing below
#include <libavutil/display.h>
#include <libavutil/pixdesc.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#ifdef __APPLE__
#include "CVideoDecoderHEVCVT.h"
#elif defined(_WIN32)
#include "CVideoDecoderHEVCMF.h"
#include "CVideoDecoderWMVMF.h"
#endif

#ifdef MT_HAVE_NATIVE_AAC
	#ifdef __APPLE__
	#include "CAudioDecoderAACApple.h"
	#elif defined(_WIN32)
	#include "CAudioDecoderAACMF.h"
	#endif
#endif

#ifdef MT_HAVE_NATIVE_WMA
	#include "CAudioDecoderWMAMF.h" // _WIN32-only (see CAudioDecoderWMANative.h)
#endif

// ============================================================================
// Local helpers
// ============================================================================
namespace
{
	// Parses "YYYY-MM-DDTHH:MM:SSZ" (the only creation_time shape FFmpeg emits
	// for the containers we care about). Returns 0 (unknown) on any mismatch.
	time_t ParseCreationTime(const char *iso8601)
	{
		if (!iso8601)
			return 0;

		int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
		if (sscanf(iso8601, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6)
			return 0;

		struct tm tmv;
		memset(&tmv, 0, sizeof(tmv));
		tmv.tm_year = y - 1900;
		tmv.tm_mon = mo - 1;
		tmv.tm_mday = d;
		tmv.tm_hour = h;
		tmv.tm_min = mi;
		tmv.tm_sec = s;

#ifdef _WIN32
		return _mkgmtime(&tmv);
#else
		return timegm(&tmv);
#endif
	}
}

// ============================================================================
// Constructor / Destructor
// ============================================================================
CVideoSourceFFmpeg::CVideoSourceFFmpeg()
{
}

CVideoSourceFFmpeg::~CVideoSourceFFmpeg()
{
	Close();
}

// ============================================================================
// Abort plumbing (spec #2.3; contract in IVideoSource.h)
// ============================================================================
int CVideoSourceFFmpeg::StaticInterruptCb(void *opaque)
{
	CVideoSourceFFmpeg *self = static_cast<CVideoSourceFFmpeg *>(opaque);
	return (self && self->IsAborted()) ? 1 : 0;
}

void CVideoSourceFFmpeg::SetAbortPredicate(std::function<bool()> pred)
{
	abortPredicate = std::move(pred);

	// FORWARD it to the packet decoder, so the decoder's own blocking waits can
	// join it into their wait predicates (CVideoDecoderHEVCMF). The predicate is
	// LEVEL-triggered -- true exactly while the in-flight operation is condemned
	// -- so a decoder that holds a copy of it needs no flag, no clear and no
	// re-arm, and cannot miss an abort that lands between two DecodePacket()
	// calls. Open() forwards it too (for the decoder it installs itself), which
	// covers the reverse call order.
	if (nativeDecoder)
		nativeDecoder->SetAbortPredicate(abortPredicate);
}

void CVideoSourceFFmpeg::WakeAbort()
{
	// The predicate check the seek/read loops do once per frame cannot help when
	// a single DecodePacket() call is blocked INSIDE the decoder -- Media
	// Foundation's HEVC pipeline waits on its condvar for up to 10 seconds per
	// step. Poke it so that wait re-evaluates the predicate NOW. The caller has
	// ALREADY published the state the predicate reads (raised shouldStop / bumped
	// seekGeneration) before calling this: state first, wake second, waiter
	// re-evaluates -- and because the predicate is level-triggered, a wake that
	// arrives when nobody is waiting is simply redundant, never lost.
	//
	// The libavcodec path needs nothing here: its blocking I/O is covered by the
	// AVIOInterruptCB above and its CPU decode is bounded by one frame, which the
	// per-iteration checks already catch.
	if (nativeDecoder)
		nativeDecoder->Abort();
}

// ============================================================================
// Open
// ============================================================================
bool CVideoSourceFFmpeg::Open(const char *filePath)
{
	LOGD("CVideoSourceFFmpeg::Open: %s", filePath);

	Close();
	errorReason.clear();

	// Allocate the demuxer context OURSELVES (rather than letting
	// avformat_open_input() do it) so the AVIOInterruptCB is attached BEFORE the
	// open: FFmpeg COPIES the callback struct into the AVIOContext/URLContext it
	// creates during avformat_open_input(), and that copy -- not fmtCtx's field --
	// is what its blocking I/O layer polls afterwards. A callback assigned to
	// fmtCtx AFTER the open would therefore never reach the read path at all, and
	// av_read_frame()/av_seek_frame() would be uninterruptible.
	//
	// SCOPE, honestly (do not over-promise):
	//  * It does NOT make the OPEN itself interruptible today. Every caller
	//    installs the predicate AFTER Open() returns (CVideoPlayer::Open() does so
	//    for the constructed, injected and preopened paths alike; the preopened
	//    source is opened on a worker thread before any player owns it), so during
	//    avformat_open_input()/avformat_find_stream_info() abortPredicate is empty
	//    and the trampoline always returns 0. A future caller that wants an
	//    interruptible open must call SetAbortPredicate() BEFORE Open() -- the hook
	//    is already wired for it, which is the second reason this pre-allocation
	//    stays.
	//  * Even then, AVIOInterruptCB is POLLED by FFmpeg's I/O layer between
	//    operations -- it does NOT interrupt a read(2) already blocked in the
	//    kernel on a dataless/cloud-evicted file. The iCloud stall is handled
	//    upstream (async open + dataless detection), not here.
	fmtCtx = avformat_alloc_context();
	if (!fmtCtx)
	{
		errorReason = "cannot allocate demuxer context";
		return false;
	}
	fmtCtx->interrupt_callback.callback = &CVideoSourceFFmpeg::StaticInterruptCb;
	fmtCtx->interrupt_callback.opaque = this;

	if (avformat_open_input(&fmtCtx, filePath, nullptr, nullptr) < 0)
	{
		// avformat_open_input() frees and nulls the context it was handed on
		// failure -- do not free it again here.
		fmtCtx = nullptr;
		errorReason = "cannot open container";
		return false;
	}
	if (avformat_find_stream_info(fmtCtx, nullptr) < 0)
	{
		errorReason = "cannot read stream info";
		Close();
		return false;
	}

	videoStream = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	audioStream = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

	if (videoStream < 0)
	{
		errorReason = "no video stream";
		Close();
		return false;
	}

	AVStream *vstream = fmtCtx->streams[videoStream];
	AVCodecParameters *vp = vstream->codecpar;
	videoTimeBase = av_q2d(vstream->time_base);
	if (audioStream >= 0)
		audioTimeBase = av_q2d(fmtCtx->streams[audioStream]->time_base);

	// Clip-relative time contract (IVideoSource.h): establish the
	// container-absolute -> clip-relative translation point up front from
	// the demuxer's own start_time when it knows one (MPEG-TS/AVCHD always
	// do -- ~1.46s in this project's h264_ac3.mts fixture). Containers that
	// don't report one (AV_NOPTS_VALUE) fall back to the first decoded
	// frame's own raw pts, established lazily by NormalizePtsSeconds()'s
	// first call.
	if (fmtCtx->start_time != (int64_t)AV_NOPTS_VALUE)
	{
		startTime = (double)fmtCtx->start_time * av_q2d(AV_TIME_BASE_Q);
		startTimeKnown = true;
	}
	else
	{
		startTime = 0.0;
		startTimeKnown = false;
	}

	FillInfo();

	// WMV family (2026-07-18 WMV spec): wmv1/wmv2/wmv3/vc1 route to the
	// Windows-native MF decoder below when available; otherwise to FFmpeg
	// software decode (full builds only) or the edition refusal (commercial
	// builds, MT_COMMERCIAL_BUILD gate at the generic decoder path).
	const bool isWMVFamily = (vp->codec_id == AV_CODEC_ID_WMV1 ||
							  vp->codec_id == AV_CODEC_ID_WMV2 ||
							  vp->codec_id == AV_CODEC_ID_WMV3 ||
							  vp->codec_id == AV_CODEC_ID_VC1);

#ifdef __APPLE__
	// Zero-bundled-HEVC-decoder path (licensing): the bundled FFmpeg build
	// carries no HEVC decoder on purpose, so on Apple platforms auto-install
	// the platform-native VideoToolbox decoder unless the caller already
	// injected one (SetHEVCPacketDecoder) or explicitly disabled
	// auto-install (SetDisableAutoNativeDecoders -- used to test the "no
	// native decoder available" refusal path independently of platform).
	//
	// (No WMV arm here: VideoToolbox has no WMV/VC-1 support at all --
	// isWMVFamily falls through to the FFmpeg/refusal logic below.)
	if (vp->codec_id == AV_CODEC_ID_HEVC && !nativeDecoder && !disableAutoNativeDecoders)
	{
		ownedNativeDecoder = std::make_unique<CVideoDecoderHEVCVT>();
		nativeDecoder = ownedNativeDecoder.get();
	}
#elif defined(_WIN32)
	// Windows twin of the Apple auto-install above, via Media Foundation's
	// HEVC decoder MFT instead of VideoToolbox. Unlike VT (always available
	// since macOS 10.13), an HEVC MFT may not resolve on a given Windows
	// install (pre-HEVC-hardware GPU + no "HEVC Video Extensions" from the
	// Store) -- gate auto-install on IsHEVCDecodeAvailable() so that case
	// leaves nativeDecoder null and falls through to the !hevcAllowed refusal
	// below with the platform's exact refusal message, rather than
	// installing a decoder doomed to fail its own Init().
	if (vp->codec_id == AV_CODEC_ID_HEVC && !nativeDecoder && !disableAutoNativeDecoders &&
		IVideoPacketDecoder::IsHEVCDecodeAvailable())
	{
		ownedNativeDecoder = std::make_unique<CVideoDecoderHEVCMF>();
		nativeDecoder = ownedNativeDecoder.get();
	}

	// WMV family -> the in-box WMVideo Decoder MFT, in BOTH build modes (so
	// the MF path is exercised by full/dev builds too, and commercial builds
	// -- whose FFmpeg carries no WMV decoders -- still play WMV on Windows).
	// IsWMVDecodeAvailable() is effectively always true on non-N Windows;
	// an N edition without the Media Feature Pack leaves nativeDecoder null
	// here and falls through to FFmpeg software decode (full builds) or the
	// edition refusal (commercial builds) below.
	if (isWMVFamily && !nativeDecoder && !disableAutoNativeDecoders &&
		IVideoPacketDecoder::IsWMVDecodeAvailable())
	{
		ownedNativeDecoder = std::make_unique<CVideoDecoderWMVMF>((int)vp->codec_id);
		nativeDecoder = ownedNativeDecoder.get();
	}
#endif

	bool hevcAllowed = !disableAutoNativeDecoders && (nativeDecoder != nullptr);
	bool videoUsesNativeDecoder = false;

	// Forward the abort predicate down to whichever decoder we just settled
	// on (auto-installed above, or injected via SetHEVCPacketDecoder()). The
	// other half of this is SetAbortPredicate(), which forwards to an ALREADY-
	// established decoder -- between them, the decoder ends up with the
	// predicate whichever order the two calls happen in (the player installs it
	// AFTER Open(); a re-Open() on a source that already has one takes this
	// path).
	if (vp->codec_id == AV_CODEC_ID_HEVC && nativeDecoder)
		nativeDecoder->SetAbortPredicate(abortPredicate);

#ifdef MT_COMMERCIAL_BUILD
	// Commercial builds keep the hard refusal when no native HEVC decoder
	// resolved (unchanged behavior, platform-specific message -- Windows
	// carries the "HEVC Video Extensions" install hint). Full builds instead
	// fall through to the generic FFmpeg software path below (2026-07-19
	// codec-superset spec).
	if (vp->codec_id == AV_CODEC_ID_HEVC && !hevcAllowed)
	{
		errorReason = kVideoErrorHEVCNeedsNativeDecoder;
		Close();
		return false;
	}
#endif

	// Full builds with no native HEVC decoder (none resolved, or auto-install
	// disabled via the test seam) skip this branch entirely and software-
	// decode through the generic FFmpeg path below. Software HEVC consumes
	// the container's hvcC extradata directly -- no Annex-B bsf on that path
	// (the bsf is keyed to WantsAnnexB(), which only native MF decoders set).
	if (vp->codec_id == AV_CODEC_ID_HEVC && hevcAllowed)
	{
		// Annex-B bitstream-filter setup (Task 6 plumbing, WantsAnnexB()
		// decoders only -- Media Foundation on Windows; VideoToolbox never
		// takes this branch). hevc_mp4toannexb converts the container's
		// length-prefixed hvcC NAL layout to start-code-prefixed Annex-B
		// (parameter sets inline in the bitstream, which is what the MF HEVC
		// decoder MFT expects) -- both the parameter-set extradata handed to
		// Init() below AND every per-packet access unit fed to DecodePacket()
		// go through this filter from here on.
		const u8 *initExtradata = vp->extradata;
		int initExtradataSize = vp->extradata_size;

		if (nativeDecoder->WantsAnnexB())
		{
			const AVBitStreamFilter *bsfFilter = av_bsf_get_by_name("hevc_mp4toannexb");
			if (!bsfFilter || av_bsf_alloc(bsfFilter, &hevcBsf) < 0)
			{
				errorReason = "failed to allocate hevc_mp4toannexb bitstream filter";
				Close();
				return false;
			}
			if (avcodec_parameters_copy(hevcBsf->par_in, vp) < 0)
			{
				errorReason = "failed to copy codec parameters into hevc_mp4toannexb bitstream filter";
				Close();
				return false;
			}
			hevcBsf->time_base_in = vstream->time_base;
			if (av_bsf_init(hevcBsf) < 0)
			{
				errorReason = "failed to initialize hevc_mp4toannexb bitstream filter";
				Close();
				return false;
			}
			bsfPacket = av_packet_alloc();
			if (!bsfPacket)
			{
				errorReason = "allocation failure (bsf output packet)";
				Close();
				return false;
			}

			// par_out is populated by av_bsf_init() itself for
			// hevc_mp4toannexb (it converts the hvcC parameter sets to
			// Annex-B immediately at init time, not lazily on first packet)
			// -- Init() below receives the Annex-B-converted parameter sets,
			// never the raw hvcC extradata.
			initExtradata = hevcBsf->par_out->extradata;
			initExtradataSize = hevcBsf->par_out->extradata_size;
		}

		// The trc goes in HERE, at the HEVC site, and only here: this is the
		// only path a PQ/HLG clip takes, and the decoder chooses its output
		// bit depth from it (S-5 Phase 5). The WMV site below keeps the
		// default -- that codec family is 8-bit throughout.
		if (!nativeDecoder->Init(initExtradata, initExtradataSize, vp->width, vp->height,
								 (int)vp->color_trc))
		{
#ifndef MT_COMMERCIAL_BUILD
			if (nativeDecoder == ownedNativeDecoder.get())
			{
				// Full builds, AUTO-INSTALLED decoder failed Init ("absent or
				// fails", 2026-07-19 codec-superset spec): tear down the owned
				// decoder and any Annex-B bsf state, then retry through the
				// generic FFmpeg software path below. An externally INJECTED
				// decoder (SetHEVCPacketDecoder) keeps the error propagation
				// in both modes so injected-failure tests stay deterministic.
				LOGWarning("CVideoSourceFFmpeg::Open: native HEVC decoder Init failed: %s -- falling back to FFmpeg software decode",
						   nativeDecoder->GetErrorReason().c_str());
				nativeDecoder = nullptr;
				ownedNativeDecoder.reset();
				if (hevcBsf)
					av_bsf_free(&hevcBsf);
				if (bsfPacket)
					av_packet_free(&bsfPacket);
			}
			else
#endif
			{
				errorReason = nativeDecoder->GetErrorReason();
				Close();
				return false;
			}
		}
		else
		{
			videoUsesNativeDecoder = true;
		}
	}
	else if (isWMVFamily && nativeDecoder)
	{
		// Native WMV/VC-1 decode (Windows MF, auto-installed above -- or a
		// test-injected decoder via SetHEVCPacketDecoder()). Same generic
		// packet-decoder loop as HEVC from here on. No Annex-B conditioning:
		// the asf demuxer hands out complete frames, and the WMVideo Decoder
		// MFT takes the sequence-header extradata via MF_MT_USER_DATA
		// (CVideoDecoderWMVMF::WantsAnnexB() is false, so no bsf is built).
		nativeDecoder->SetAbortPredicate(abortPredicate);
		if (!nativeDecoder->Init(vp->extradata, vp->extradata_size, vp->width, vp->height))
		{
			errorReason = nativeDecoder->GetErrorReason();
			Close();
			return false;
		}
		videoUsesNativeDecoder = true;
	}

	if (!videoUsesNativeDecoder)
	{
#ifdef MT_COMMERCIAL_BUILD
		// Guard #4 (2026-07-18 WMV spec): a commercial/store build never
		// software-decodes the patent-encumbered WMV/VC-1 family -- not even
		// if a full FFmpeg were accidentally linked (this refusal is compiled
		// in ahead of avcodec_find_decoder). Reached on platforms with no
		// native WMV decoder (macOS/Linux) or a Windows N edition without
		// the Media Feature Pack.
		if (isWMVFamily)
		{
			errorReason = kVideoErrorWMVEditionUnsupported;
			Close();
			return false;
		}
		// Guard #4 extension (2026-07-19 codec-superset spec): HEVC joins the
		// encumbered set -- even a mislinked full FFmpeg must not software-
		// decode it in a commercial binary. Normally unreachable (the
		// !hevcAllowed refusal above fires first); defense in depth.
		if (vp->codec_id == AV_CODEC_ID_HEVC)
		{
			errorReason = kVideoErrorHEVCNeedsNativeDecoder;
			Close();
			return false;
		}
#endif
		const AVCodec *dec = avcodec_find_decoder(vp->codec_id);
		if (!dec)
		{
			errorReason = std::string("no decoder for ") + avcodec_get_name(vp->codec_id);
			Close();
			return false;
		}
		vctx = avcodec_alloc_context3(dec);
		if (!vctx)
		{
			errorReason = "cannot allocate video decoder context";
			Close();
			return false;
		}
		avcodec_parameters_to_context(vctx, vp);
		vctx->pkt_timebase = vstream->time_base;
		vctx->thread_count = 0;
		if (avcodec_open2(vctx, dec, nullptr) < 0)
		{
			errorReason = "video decoder open failed";
			Close();
			return false;
		}
	}

	// Missing/failed audio decoder (e.g. AAC -- not bundled, licensing) is
	// non-fatal: video-only playback. info.hasAudio was already set from
	// stream presence in FillInfo() and is not revised here.
	//
	// openVideoOnly (poster/thumbnail extraction): skip the audio decoder
	// deliberately -- with no decoder open, the packet loop's no-decoder
	// branch discards audio packets at demux, so ReadVideoFrame() never
	// decodes or queues audio and ReadAudio() never yields data. Saves the
	// (sometimes failing) native codec init and the per-frame audio decode
	// work that a video-only consumer always threw away.
	if (openVideoOnly)
	{
		// intentionally no OpenAudioDecoder()
	}
	else if (!OpenAudioDecoder())
	{
		LOGWarning("CVideoSourceFFmpeg::Open: continuing without audio decode for '%s'", filePath);
	}

	frame = av_frame_alloc();
	audioFrame = av_frame_alloc();
	packet = av_packet_alloc();
	if (!frame || !audioFrame || !packet)
	{
		errorReason = "allocation failure";
		Close();
		return false;
	}

	errorReason.clear();
	LOGD("CVideoSourceFFmpeg::Open: successfully opened '%s' (%dx%d %s)",
		 filePath, info.width, info.height, info.videoCodecName.c_str());
	return true;
}

// ============================================================================
// ProbeIsAlphaVP9WebM -- routing helper for CVideoPlayer
// ============================================================================
bool CVideoSourceFFmpeg::ProbeIsAlphaVP9WebM(const char *filePath)
{
	// Extension gate: only Matroska/WebM containers can be alpha-VP9 WebM,
	// and callers route by extension everywhere else. Answering from the
	// path alone avoids opening the file at all -- the old unconditional
	// avformat_open_input probe did blocking reads on the CALLER'S thread,
	// which on a dataless (cloud-evicted) file stalls until the provider
	// downloads the entire file.
	const char *dot = strrchr(filePath, '.');
	if (!dot)
		return false;
	char ext[8] = {};
	const size_t extLen = strlen(dot + 1);
	if (extLen == 0 || extLen >= sizeof(ext))
		return false;
	for (size_t i = 0; i < extLen; i++)
		ext[i] = (char)tolower((unsigned char)dot[1 + i]);
	if (strcmp(ext, "webm") != 0 && strcmp(ext, "mkv") != 0)
		return false;

	AVFormatContext *probeCtx = nullptr;
	if (avformat_open_input(&probeCtx, filePath, nullptr, nullptr) < 0)
		return false;

	bool isAlphaVP9 = false;
	bool isMatroska = probeCtx->iformat && probeCtx->iformat->name &&
					   strstr(probeCtx->iformat->name, "matroska") != nullptr;
	if (isMatroska)
	{
		int vs = av_find_best_stream(probeCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
		if (vs >= 0 && probeCtx->streams[vs]->codecpar->codec_id == AV_CODEC_ID_VP9)
		{
			AVDictionaryEntry *alpha = av_dict_get(probeCtx->streams[vs]->metadata, "alpha_mode", nullptr, 0);
			isAlphaVP9 = (alpha != nullptr);
		}
	}

	avformat_close_input(&probeCtx);
	return isAlphaVP9;
}

// ============================================================================
// FillInfo -- dims, fps, duration, creation_time, rotation, audio presence
// ============================================================================
void CVideoSourceFFmpeg::FillInfo()
{
	AVStream *vstream = fmtCtx->streams[videoStream];
	AVCodecParameters *vp = vstream->codecpar;

	// mov_read_tkhd assigns st->id from the `tkhd` track_id, so for mp4/mov this
	// IS the container's track id (the photo app fu-e5 #4.2). Other demuxers leave
	// it at their own numbering or -1; a consumer that cares must know which
	// container it is looking at, which is why this is documented as 0-when-
	// unknown rather than "the stream index".
	info.videoTrackId = (vstream->id > 0) ? vstream->id : 0;

	info.width = vp->width;
	info.height = vp->height;
	info.videoCodecName = avcodec_get_name(vp->codec_id);

	AVRational fr = av_guess_frame_rate(fmtCtx, vstream, nullptr);
	info.fps = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 0.0;

	if (fmtCtx->duration != (int64_t)AV_NOPTS_VALUE && fmtCtx->duration > 0)
		info.duration = (double)fmtCtx->duration / (double)AV_TIME_BASE;
	else if (vstream->duration != (int64_t)AV_NOPTS_VALUE && vstream->duration > 0)
		info.duration = (double)vstream->duration * av_q2d(vstream->time_base);
	else
		info.duration = 0.0;

	// Container-level creation_time first (mp4/mov/mkv carry it there); fall
	// back to the stream's own tag. TS/PS containers (mpegts/mpeg) carry
	// neither -- creationTime stays 0 ("unknown"), matching the fixture table.
	AVDictionaryEntry *tag = av_dict_get(fmtCtx->metadata, "creation_time", nullptr, 0);
	if (!tag)
		tag = av_dict_get(vstream->metadata, "creation_time", nullptr, 0);
	info.creationTime = tag ? ParseCreationTime(tag->value) : 0;

	// Rotation from the AV_PKT_DATA_DISPLAYMATRIX side data attached to the
	// stream's codec parameters (modern FFmpeg's coded_side_data, replacing
	// the older av_stream_get_side_data() API).
	//
	// Sign convention (verified empirically against tests/fixtures/video/
	// h264_rot90.mp4, which was produced with `ffmpeg -display_rotation 90`):
	// av_display_rotation_get() returns the angle by which its own
	// documented transformation rotates the frame -- applying that
	// transformation directly (no negation) reproduces exactly what FFmpeg's
	// own "-autorotate" (default-on) does when transcoding the fixture, and
	// matches ffprobe's reported `rotation=90` for the same file (confirmed
	// via `ffmpeg -noautorotate` vs default-autorotate frame diffs, and via
	// `transpose=cclock` producing a pixel-identical result to the
	// autorotated output). A previous version of this code negated the
	// angle, which flipped the sign and produced rotationDegrees==270 for
	// this fixture -- normalize WITHOUT negating.
	info.rotationDegrees = 0;
	const AVPacketSideData *sd = av_packet_side_data_get(
		vp->coded_side_data, vp->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
	if (sd && sd->data && sd->size >= 9 * sizeof(int32_t))
	{
		double angle = av_display_rotation_get(reinterpret_cast<const int32_t *>(sd->data));
		if (!std::isnan(angle))
		{
			int deg = ((int)std::lround(angle)) % 360;
			if (deg < 0)
				deg += 360;
			info.rotationDegrees = deg;
		}
	}

	// CM-E: container-level colour metadata, read at open. This is what covers
	// the native-decoder paths (VideoToolbox / Media Foundation) -- their
	// frames bypass EmitFrame(), so without this the info stayed at its
	// constructed defaults (0/false -> BT.601 limited) for exactly the HEVC
	// content most likely to be BT.709/BT.2020. Frame-level values override
	// these in EmitFrame() -- but only when the frame is actually tagged.
	info.colorSpace = VideoColor_NormalizeMatrix((int)vp->color_space,
												 vp->width, vp->height);
	info.fullRange = (vp->color_range == AVCOL_RANGE_JPEG);
	if (vp->color_primaries != AVCOL_PRI_UNSPECIFIED)
		info.colorPrimaries = (int)vp->color_primaries;
	if (vp->color_trc != AVCOL_TRC_UNSPECIFIED)
		info.colorTrc = (int)vp->color_trc;

	info.hasAudio = (audioStream >= 0);
	if (audioStream >= 0)
	{
		AVCodecParameters *ap = fmtCtx->streams[audioStream]->codecpar;
		info.audioCodecName = avcodec_get_name(ap->codec_id);
		info.audioChannels = ap->ch_layout.nb_channels;
		info.audioSampleRate = (u32)ap->sample_rate;
	}
	else
	{
		info.audioCodecName.clear();
		info.audioChannels = 0;
		info.audioSampleRate = 0;
	}
}

// ============================================================================
// OpenAudioDecoder
// ============================================================================
bool CVideoSourceFFmpeg::OpenAudioDecoder()
{
	if (audioStream < 0)
		return true; // no audio track at all -- not an error

	AVStream *astream = fmtCtx->streams[audioStream];
	AVCodecParameters *ap = astream->codecpar;

	const bool isWMAFamily = (ap->codec_id == AV_CODEC_ID_WMAV1 ||
							  ap->codec_id == AV_CODEC_ID_WMAV2 ||
							  ap->codec_id == AV_CODEC_ID_WMAPRO);
	(void)isWMAFamily; // referenced only under MT_HAVE_NATIVE_WMA / MT_COMMERCIAL_BUILD below

#ifdef MT_HAVE_NATIVE_WMA
	// Native WMA path (2026-07-18 WMV spec): the WMA family prefers the
	// in-box WMAudio Decoder MFT on Windows in BOTH build modes (commercial
	// FFmpeg carries no WMA decoders at all; full builds exercise the same
	// path dev-side). On init failure, fall through to the FFmpeg path below
	// -- which decodes in full builds and cleanly yields video-only in
	// commercial ones (same non-fatal contract as every other audio-decoder
	// failure). Skipped when the disable-natives test seam is set
	// (SetDisableAutoNativeDecoders covers ALL native decoders, video and
	// audio -- 2026-07-19 codec-superset spec).
	if (isWMAFamily && !disableAutoNativeDecoders)
	{
		wmaInit.codecId = (int)ap->codec_id;
		wmaInit.extradata.assign(ap->extradata, ap->extradata + (ap->extradata && ap->extradata_size > 0 ? ap->extradata_size : 0));
		wmaInit.channels = ap->ch_layout.nb_channels;
		wmaInit.sampleRate = ap->sample_rate;
		wmaInit.blockAlign = ap->block_align;
		wmaInit.avgBytesPerSec = (int)(ap->bit_rate > 0 ? ap->bit_rate / 8 : 0);

		wmaDecoder = std::make_unique<CAudioDecoderWMANative>();
		if (wmaDecoder->Init(wmaInit.codecId,
							 wmaInit.extradata.empty() ? nullptr : wmaInit.extradata.data(),
							 (int)wmaInit.extradata.size(),
							 wmaInit.channels, wmaInit.sampleRate,
							 wmaInit.blockAlign, wmaInit.avgBytesPerSec))
		{
			return true;
		}
		LOGWarning("CVideoSourceFFmpeg::OpenAudioDecoder: CAudioDecoderWMANative::Init failed: %s -- falling back to FFmpeg audio path",
				   wmaDecoder->GetErrorReason().c_str());
		wmaDecoder.reset();
		wmaInit = SWMAInitParams();
	}
#endif

#ifdef MT_COMMERCIAL_BUILD
	// Guard #4 (2026-07-18 WMV spec), audio half: a commercial build never
	// software-decodes the WMA family. Non-fatal by the same contract as any
	// missing audio decoder -- playback continues video-only.
	if (isWMAFamily)
	{
		LOGWarning("CVideoSourceFFmpeg::OpenAudioDecoder: WMA audio is not decoded in this edition -- continuing video-only");
		return false;
	}
#endif

#ifdef MT_HAVE_NATIVE_AAC
	// Native-first AAC path (2026-07-19 codec-superset spec, section 1.5: OS
	// decoders always win): AAC audio routes to the OS-native decoder here
	// first (all profiles -- LC, HE, HE-v2 -- the native decoder negotiates
	// SBR/PS from the AudioSpecificConfig itself; see CAudioDecoderAACNative.h
	// for the AudioToolbox/Media Foundation selection seam). When the native
	// path is unavailable (no extradata) or Init fails, fall THROUGH to the
	// generic FFmpeg path below -- which decodes in full builds and yields
	// video-only in commercial ones (AAC/EAC3 gate below). Every other audio
	// codec (pcm, mp3, opus, vorbis, ac3, ...) stays on the FFmpeg path
	// below, unchanged. Skipped when the disable-natives test seam is set
	// (the seam covers ALL native decoders, video and audio).
	if (ap->codec_id == AV_CODEC_ID_AAC && !disableAutoNativeDecoders)
	{
		if (!ap->extradata || ap->extradata_size <= 0)
		{
			// ADTS-only (e.g. raw TS) AAC carries no out-of-band
			// AudioSpecificConfig for the native decoder -- exactly the case
			// the FFmpeg fallthrough exists for (FFmpeg's aac decoder handles
			// ADTS framing natively, full builds).
			LOGWarning("CVideoSourceFFmpeg::OpenAudioDecoder: AAC stream has no AudioSpecificConfig extradata (ADTS-only) -- falling through to FFmpeg audio path");
		}
		else
		{
			aacExtradata.assign(ap->extradata, ap->extradata + ap->extradata_size);
			aacDecoder = std::make_unique<CAudioDecoderAACNative>();
			if (aacDecoder->Init(aacExtradata.data(), (int)aacExtradata.size()))
				return true;

			// Init failure (possibly malformed extradata): the FFmpeg attempt
			// below may fail differently, but the non-fatal video-only
			// contract holds either way (the generic path warns and returns
			// false on open failure).
			LOGWarning("CVideoSourceFFmpeg::OpenAudioDecoder: CAudioDecoderAACNative::Init failed: %s -- falling back to FFmpeg audio path",
					   aacDecoder->GetErrorReason().c_str());
			aacDecoder.reset();
			aacExtradata.clear();
		}
	}
#endif

#ifdef MT_COMMERCIAL_BUILD
	// Guard #4 extension, audio half (2026-07-19 codec-superset spec): a
	// commercial build never software-decodes AAC or E-AC-3 -- not even if a
	// full FFmpeg were accidentally linked. Deliberately placed AFTER the
	// native AAC attempt above (spec section 1.5: OS decoders always win;
	// this gate must never preempt them). Non-fatal by the same contract as
	// WMA: playback continues video-only.
	if (ap->codec_id == AV_CODEC_ID_AAC || ap->codec_id == AV_CODEC_ID_EAC3)
	{
		LOGWarning("CVideoSourceFFmpeg::OpenAudioDecoder: %s audio is not decoded in this edition -- continuing video-only",
				   avcodec_get_name(ap->codec_id));
		return false;
	}
#endif

	const AVCodec *dec = avcodec_find_decoder(ap->codec_id);
	if (!dec)
	{
		LOGWarning("CVideoSourceFFmpeg::OpenAudioDecoder: no decoder for %s", avcodec_get_name(ap->codec_id));
		return false;
	}

	actx = avcodec_alloc_context3(dec);
	if (!actx)
		return false;
	avcodec_parameters_to_context(actx, ap);
	actx->pkt_timebase = astream->time_base;
	if (avcodec_open2(actx, dec, nullptr) < 0)
	{
		LOGWarning("CVideoSourceFFmpeg::OpenAudioDecoder: avcodec_open2 failed for %s", avcodec_get_name(ap->codec_id));
		avcodec_free_context(&actx);
		actx = nullptr;
		return false;
	}
	return true;
}

// ============================================================================
// ReadVideoFrame
// ============================================================================
bool CVideoSourceFFmpeg::ReadVideoFrame(SDecodedVideoFrame &out)
{
	errorReason.clear();

	while (true)
	{
		// ABORT CHECK, once per packet iteration (spec #2.3): this is what bounds
		// abort latency to a single frame's decode instead of to the whole
		// scan-to-target a precise seek performs through this very loop. Aborted
		// is NEITHER an error NOR end-of-stream: return false with an EMPTY
		// errorReason and let the CALL SITE -- which triggered the abort and is
		// therefore the only one who knows -- classify it (see the abort-
		// classification comments at every `false` handler below and in
		// CVideoPlayer).
		if (IsAborted())
		{
			errorReason.clear();
			return false;
		}

		if (vctx && avcodec_receive_frame(vctx, frame) == 0)
			return EmitFrame(out);

		// Packet acquisition: video packets PARKED by PumpAudioAhead() come
		// first, in their original demux order, so the video stream sees the
		// exact same sequence with or without pumping; only then read fresh
		// packets from the demuxer.
		int r;
		if (!parkedVideoPackets.empty())
		{
			AVPacket *parked = parkedVideoPackets.front();
			parkedVideoPackets.pop_front();
			parkedVideoBytes -= (size_t)(parked->size > 0 ? parked->size : 0);
			av_packet_move_ref(packet, parked);
			av_packet_free(&parked);
			r = 0;
		}
		else
		{
			r = av_read_frame(fmtCtx, packet);
		}
		if (r < 0)
		{
			// An abort raised INSIDE the blocking read (the AVIOInterruptCB fired
			// and av_read_frame returned AVERROR_EXIT) reaches here looking exactly
			// like EOS. Classify it before the EOS drain below -- draining would
			// push a null packet into a decoder we are trying to cut short, and the
			// empty errorReason would read as a clean "short clip" upstream.
			if (IsAborted())
			{
				errorReason.clear();
				return false;
			}

			if (vctx)
			{
				avcodec_send_packet(vctx, nullptr); // flush
				if (avcodec_receive_frame(vctx, frame) == 0)
					return EmitFrame(out);
			}
			else if (nativeDecoder)
			{
				if (hevcBsf)
				{
					// EOS bsf-drain FIRST: hand the filter its own EOS signal
					// and drain any access unit still buffered inside it
					// (hevc_mp4toannexb is 1:1 per input in practice, so at
					// most one, but this is a real loop per the plumbing
					// spec) into the decoder before the decoder's own
					// null-packet drain below -- otherwise a packet the bsf
					// was still holding onto would be silently lost instead
					// of reaching the decoder at all.
					int sendRet = av_bsf_send_packet(hevcBsf, nullptr);
					if (sendRet < 0 && sendRet != AVERROR_EOF)
					{
						errorReason = "av_bsf_send_packet (hevc_mp4toannexb) EOS-signal failed";
						return false;
					}

					while (true)
					{
						int recvRet = av_bsf_receive_packet(hevcBsf, bsfPacket);
						if (recvRet == AVERROR(EAGAIN) || recvRet == AVERROR_EOF)
							break;
						if (recvRet < 0)
						{
							errorReason = "av_bsf_receive_packet (hevc_mp4toannexb) EOS-drain failed";
							return false;
						}

						av_packet_rescale_ts(bsfPacket, fmtCtx->streams[videoStream]->time_base, AV_TIME_BASE_Q);
						bool got = nativeDecoder->DecodePacket(bsfPacket, out);
						av_packet_unref(bsfPacket);
						if (got)
						{
							out.pts = NormalizePtsSeconds(out.pts);
							debugFramesDecodedAfterSeek++;
							return true;
						}
						// ABORT CLASSIFICATION (spec: no third return state on
						// DecodePacket()). A false return means "buffering" (empty
						// reason) or "decode failure" (non-empty) -- an ABORT is a
						// third meaning, and it is resolved HERE, at the call site
						// that triggered it, by checking the predicate FIRST:
						// whatever the decoder wrote into its error reason on the
						// way out of an interrupted wait, an aborted result is
						// discarded, never surfaced as an error (it would latch
						// EVideoPlayerState::Error and wedge the player) and never
						// mistaken for buffering (we would loop forever).
						if (IsAborted())
						{
							errorReason.clear();
							return false;
						}
						if (!nativeDecoder->GetErrorReason().empty())
						{
							errorReason = nativeDecoder->GetErrorReason();
							return false;
						}
						// else: buffering -- keep draining the bsf.
					}
				}

				// EOS: drain the decoder's internal pts-reorder queue (VT) or
				// internal DRAIN command (MF) one frame per ReadVideoFrame()
				// call, same shape as the vctx flush path above.
				if (nativeDecoder->DecodePacket(nullptr, out))
				{
					out.pts = NormalizePtsSeconds(out.pts);
					debugFramesDecodedAfterSeek++;
					return true;
				}
				// Abort classification first (see the bsf-drain site above): an
				// interrupted drain is not a decode failure.
				if (IsAborted())
				{
					errorReason.clear();
					return false;
				}
				// Genuine decode failure surfaced during the drain (rare --
				// the drain call itself is just returning buffered frames --
				// but wire it for consistency with the main-loop check below).
				if (!nativeDecoder->GetErrorReason().empty())
					errorReason = nativeDecoder->GetErrorReason();
			}
			return false; // EOS (errorReason empty) or genuine decode failure (errorReason set above)
		}

		if (packet->stream_index == videoStream)
		{
			if (nativeDecoder)
			{
				// nativeDecoder only ever sees the raw pts in a timebase it
				// doesn't otherwise know about; rescale to AV_TIME_BASE_Q
				// (microseconds) so CVideoDecoderHEVCVT (and any future
				// native decoder) has a single fixed, well-known unit to
				// convert from, matching the seconds-based pts EmitFrame()
				// produces for every other codec path.
				bool got = false;

				if (hevcBsf)
				{
					// Annex-B bitstream-filter path (Media Foundation on
					// Windows -- see WantsAnnexB()). av_bsf_send_packet MOVES
					// `packet`'s ref into the filter (packet comes back
					// blank); the av_packet_unref() right after is therefore
					// a documented no-op, kept only so this branch mirrors
					// the unconditional unref every other packet-consuming
					// branch in this function does. `packet` is still in the
					// stream's own time_base at this point -- unlike the
					// non-bsf branch below, the AV_TIME_BASE_Q rescale
					// happens AFTER av_bsf_receive_packet(), on bsfPacket,
					// since hevc_mp4toannexb passes pts/dts through
					// unmodified and bsfPacket is what actually reaches
					// DecodePacket().
					int sendRet = av_bsf_send_packet(hevcBsf, packet);
					av_packet_unref(packet); // no-op after the successful move above -- see comment
					if (sendRet < 0)
					{
						// AVERROR(EAGAIN) lands here too and is treated as a
						// hard error deliberately: it can only mean "drain
						// receive_packet before sending more", which cannot
						// happen for hevc_mp4toannexb (1:1, never buffers,
						// and the loop below fully drains after every send).
						// If a future filter change makes EAGAIN reachable,
						// the fix is a drain-then-retry here, not a silent
						// skip. The message names the code to aid that day.
						char errBuf[64];
						snprintf(errBuf, sizeof(errBuf),
								 "av_bsf_send_packet (hevc_mp4toannexb) failed (%d)", sendRet);
						errorReason = errBuf;
						return false;
					}

					// hevc_mp4toannexb is 1:1 per access unit in practice --
					// still a real loop, not a single receive, per the
					// plumbing spec.
					while (true)
					{
						int recvRet = av_bsf_receive_packet(hevcBsf, bsfPacket);
						if (recvRet == AVERROR(EAGAIN) || recvRet == AVERROR_EOF)
							break; // filter needs another input packet -- fall through to read the next one
						if (recvRet < 0)
						{
							errorReason = "av_bsf_receive_packet (hevc_mp4toannexb) failed";
							return false;
						}

						av_packet_rescale_ts(bsfPacket, fmtCtx->streams[videoStream]->time_base, AV_TIME_BASE_Q);
						got = nativeDecoder->DecodePacket(bsfPacket, out);
						av_packet_unref(bsfPacket);
						if (got)
							break;
						// Abort classification first -- see the bsf EOS-drain site.
						if (IsAborted())
						{
							errorReason.clear();
							return false;
						}
						if (!nativeDecoder->GetErrorReason().empty())
						{
							errorReason = nativeDecoder->GetErrorReason();
							return false;
						}
						// else: buffering -- loop again for any further bsf output.
					}
				}
				else
				{
					// VT path (hevcBsf never allocated here): byte-identical
					// to the code before this seam existed.
					av_packet_rescale_ts(packet, fmtCtx->streams[videoStream]->time_base, AV_TIME_BASE_Q);
					got = nativeDecoder->DecodePacket(packet, out);
					av_packet_unref(packet);
				}

				if (got)
				{
					out.pts = NormalizePtsSeconds(out.pts);
					debugFramesDecodedAfterSeek++;
					return true;
				}
				// Abort classification BEFORE the error check (spec #2.3): a
				// DecodePacket() cut short by WakeAbort() returns false, and false
				// with an empty reason is also how the decoder says "still
				// buffering" -- so the predicate, not the reason, is what tells the
				// two apart here, and it must be asked FIRST. DEFENCE IN DEPTH on
				// the reason: every in-tree decoder clears errorReason on its abort
				// return (CVideoDecoderHEVCMF::DecodePacket()), so an aborted call
				// leaves it empty per the IVideoSource contract -- but a decoder
				// that did not would have its "error" latch EVideoPlayerState::Error
				// for a seek the user merely superseded, and this ordering makes
				// that impossible. See the bsf EOS-drain site for the full note.
				if (IsAborted())
				{
					errorReason.clear();
					return false;
				}
				if (!nativeDecoder->GetErrorReason().empty())
				{
					// Genuine decode failure (Plan-2 Task 2): propagate now
					// instead of looping toward EOS -- a corrupt/unsupported
					// HEVC stream must surface as a real error, not a false
					// "clean, short clip".
					errorReason = nativeDecoder->GetErrorReason();
					return false;
				}
				// else: buffering (e.g. reorder window not full yet) -- keep
				// reading packets.
			}
			else
			{
				avcodec_send_packet(vctx, packet);
				av_packet_unref(packet);
			}
		}
		else if (packet->stream_index == audioStream)
		{
#ifdef MT_HAVE_NATIVE_AAC
			if (aacDecoder)
			{
				QueueAACPacket(packet); // unrefs internally
			}
			else
#endif
#ifdef MT_HAVE_NATIVE_WMA
			if (wmaDecoder)
			{
				QueueWMAPacket(packet); // unrefs internally
			}
			else
#endif
			if (actx)
			{
				QueueAudioPacket(packet); // unrefs internally
			}
			else
			{
				av_packet_unref(packet);
			}
		}
		else
		{
			av_packet_unref(packet);
		}
	}
}

// ============================================================================
// EmitFrame -- map AVFrame planes/format to SDecodedVideoFrame
// ============================================================================
bool CVideoSourceFFmpeg::EmitFrame(SDecodedVideoFrame &out)
{
	int64_t rawPts = (frame->best_effort_timestamp != (int64_t)AV_NOPTS_VALUE)
						  ? frame->best_effort_timestamp
						  : frame->pts;
	double pts = (rawPts != (int64_t)AV_NOPTS_VALUE) ? NormalizePtsSeconds((double)rawPts * videoTimeBase) : 0.0;

	AVPixelFormat fmt = (AVPixelFormat)frame->format;
	bool fullRange = false;
	bool handled = true;

	switch (fmt)
	{
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_YUVJ420P:
		out.pixelFormat = EVideoPixelFormat::YUV420P;
		out.plane[0] = frame->data[0]; out.plane[1] = frame->data[1]; out.plane[2] = frame->data[2]; out.plane[3] = nullptr;
		out.stride[0] = frame->linesize[0]; out.stride[1] = frame->linesize[1]; out.stride[2] = frame->linesize[2]; out.stride[3] = 0;
		fullRange = (fmt == AV_PIX_FMT_YUVJ420P);
		break;
	case AV_PIX_FMT_NV12:
		out.pixelFormat = EVideoPixelFormat::NV12;
		out.plane[0] = frame->data[0]; out.plane[1] = frame->data[1]; out.plane[2] = nullptr; out.plane[3] = nullptr;
		out.stride[0] = frame->linesize[0]; out.stride[1] = frame->linesize[1]; out.stride[2] = 0; out.stride[3] = 0;
		break;
	case AV_PIX_FMT_YUV420P10LE:
		out.pixelFormat = EVideoPixelFormat::YUV420P10;
		out.plane[0] = frame->data[0]; out.plane[1] = frame->data[1]; out.plane[2] = frame->data[2]; out.plane[3] = nullptr;
		out.stride[0] = frame->linesize[0]; out.stride[1] = frame->linesize[1]; out.stride[2] = frame->linesize[2]; out.stride[3] = 0;
		break;
	case AV_PIX_FMT_YUV422P:
	case AV_PIX_FMT_YUVJ422P:
		out.pixelFormat = EVideoPixelFormat::YUV422P;
		out.plane[0] = frame->data[0]; out.plane[1] = frame->data[1]; out.plane[2] = frame->data[2]; out.plane[3] = nullptr;
		out.stride[0] = frame->linesize[0]; out.stride[1] = frame->linesize[1]; out.stride[2] = frame->linesize[2]; out.stride[3] = 0;
		fullRange = (fmt == AV_PIX_FMT_YUVJ422P);
		break;
	case AV_PIX_FMT_YUV422P10LE:
		out.pixelFormat = EVideoPixelFormat::YUV422P10;
		out.plane[0] = frame->data[0]; out.plane[1] = frame->data[1]; out.plane[2] = frame->data[2]; out.plane[3] = nullptr;
		out.stride[0] = frame->linesize[0]; out.stride[1] = frame->linesize[1]; out.stride[2] = frame->linesize[2]; out.stride[3] = 0;
		break;
	case AV_PIX_FMT_YUVA420P:
		out.pixelFormat = EVideoPixelFormat::YUVA420P;
		out.plane[0] = frame->data[0]; out.plane[1] = frame->data[1]; out.plane[2] = frame->data[2]; out.plane[3] = frame->data[3];
		out.stride[0] = frame->linesize[0]; out.stride[1] = frame->linesize[1]; out.stride[2] = frame->linesize[2]; out.stride[3] = frame->linesize[3];
		break;
	default:
		handled = false;
		break;
	}

	if (!handled)
	{
		// Anything else (e.g. yuv444p, various RGB orders) -- convert once via
		// a lazily-created/refreshed SwsContext so the pixel format enum stays
		// closed at YUV420P for unusual inputs.
		if (!swsCtx || swsSrcFormat != (int)fmt || swsSrcW != frame->width || swsSrcH != frame->height)
		{
			if (swsCtx) { sws_freeContext(swsCtx); swsCtx = nullptr; }
			swsCtx = sws_getContext(frame->width, frame->height, fmt,
									 frame->width, frame->height, AV_PIX_FMT_YUV420P,
									 SWS_BILINEAR, nullptr, nullptr, nullptr);
			swsSrcFormat = (int)fmt;
			swsSrcW = frame->width;
			swsSrcH = frame->height;
			swsSrcRange = -1;   // force the range setup below for the new context
		}
		if (!swsCtx)
		{
			errorReason = std::string("unsupported pixel format: ") +
						  (av_get_pix_fmt_name(fmt) ? av_get_pix_fmt_name(fmt) : "unknown");
			return false;
		}

		// Programme review round 2 (F1): sws only auto-detects full range from
		// J pix formats. A non-J planar source whose METADATA says full range
		// (yuv444p + color_range=JPEG, the modern tagging) would otherwise be
		// subsampled with NO range conversion while the code below tags the
		// output limited -- the shader then expands already-full values and
		// clips. Configure srcRange explicitly so the forced-limited output
		// tag is true for every input. dstRange stays limited (the emitted
		// tag), 601 both sides (the normalizer maps these sources to 601).
		// The return is deliberately ignored: for RGB sources sws reports
		// "not supported" (-1) and keeps its own correct RGB->limited-YUV
		// default -- exactly the CVideoFrameExtractor precedent.
		{
			const int wantSrcRange =
				(fullRange || frame->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
			if (wantSrcRange != swsSrcRange)
			{
				const int *coefs = sws_getCoefficients(SWS_CS_ITU601);
				sws_setColorspaceDetails(swsCtx, coefs, wantSrcRange,
										 coefs, /*dstRange*/0,
										 0, 1 << 16, 1 << 16);
				swsSrcRange = wantSrcRange;
			}
		}

		int uvW = (frame->width + 1) / 2;
		int uvH = (frame->height + 1) / 2;
		size_t ySize = (size_t)frame->width * (size_t)frame->height;
		size_t uvSize = (size_t)uvW * (size_t)uvH;
		swsBuffer.resize(ySize + 2 * uvSize);

		uint8_t *dstData[4] = { swsBuffer.data(), swsBuffer.data() + ySize, swsBuffer.data() + ySize + uvSize, nullptr };
		int dstLinesize[4] = { frame->width, uvW, uvW, 0 };
		sws_scale(swsCtx, frame->data, frame->linesize, 0, frame->height, dstData, dstLinesize);

		out.pixelFormat = EVideoPixelFormat::YUV420P;
		out.plane[0] = dstData[0]; out.plane[1] = dstData[1]; out.plane[2] = dstData[2]; out.plane[3] = nullptr;
		out.stride[0] = dstLinesize[0]; out.stride[1] = dstLinesize[1]; out.stride[2] = dstLinesize[2]; out.stride[3] = 0;
	}

	out.width = frame->width;
	out.height = frame->height;
	out.pts = pts;

	// CM-E: frame-level colour metadata overrides the container-level values
	// FillInfo() stored -- but ONLY when the frame is actually tagged. An
	// unconditional overwrite here was the shipped bug: (a) it stored the raw
	// AVCOL_SPC_* value against the shader's VPX_CS_* convention (BT.709 == 1
	// vs 2 -- every tagged HD clip took the 601 branch), and (b) files whose
	// tag lives in the container's `colr` box commonly decode with
	// UNSPECIFIED frame-level tags, so the heuristic would revert the
	// codecpar value FillInfo just resolved.
	if (frame->colorspace != AVCOL_SPC_UNSPECIFIED)
		info.colorSpace = VideoColor_NormalizeMatrix((int)frame->colorspace,
													 frame->width, frame->height);
	// Range, symmetric with the tagged-frame-wins rule above (CM-E review):
	// a YUVJ pix format is definitionally full-range and always wins; else an
	// explicitly tagged frame overrides the container value in BOTH
	// directions (a plain |= latched full forever and could never honour an
	// explicit limited tag); untagged keeps FillInfo's codecpar answer.
	if (fullRange)
		info.fullRange = true;
	else if (frame->color_range != AVCOL_RANGE_UNSPECIFIED)
		info.fullRange = (frame->color_range == AVCOL_RANGE_JPEG);
	// Programme review 2026-08-11 (F4): when the sws fallback above produced
	// the pixels, ITS defaults decided the encoding -- limited-range 601 --
	// not the source frame's tags. A full-range RGB source (qtrle,
	// PNG-in-MOV, screen recordings) would otherwise be tagged full while the
	// converted pixels are limited, lifting contrast. The matrix half
	// (RGB -> 601) is already folded by NormalizeMatrix; this is the range
	// half of the same case.
	if (!handled)
		info.fullRange = false;
	if (frame->color_primaries != AVCOL_PRI_UNSPECIFIED)
		info.colorPrimaries = (int)frame->color_primaries;
	// KNOWN LIMITATION, S-5 Phase 5: a frame-level trc that ARRIVES HERE is too
	// late to change the decoder's output bit depth. The native decoders'
	// sessions are created once, in Init(), from the CONTAINER's color_trc (see
	// the HEVC call site above), so a clip whose container says "unspecified"
	// while its frames say PQ/HLG decodes at 8 bits for its whole life.
	//
	// Not a regression, and not currently reachable for the decoders that care:
	// frames from a native decoder bypass EmitFrame entirely, so this line only
	// runs on the FFmpeg software path -- which already emits YUV420P10 for
	// 10-bit sources regardless of trc. Recorded here rather than left for the
	// next reader to rediscover from first principles.
	if (frame->color_trc != AVCOL_TRC_UNSPECIFIED)
		info.colorTrc = (int)frame->color_trc;

	debugFramesDecodedAfterSeek++;
	return true;
}

// ============================================================================
// NormalizePtsSeconds -- absolute (container-clock) seconds -> clip-relative
// ============================================================================
double CVideoSourceFFmpeg::NormalizePtsSeconds(double absSeconds)
{
	// Lazy fallback anchor (see startTime's doc comment / Open()): only taken
	// when Open() couldn't determine start_time from the demuxer, so the
	// first frame decoded from EITHER stream (whichever wins the race)
	// defines clip-relative 0 for both video and audio pts alike.
	if (!startTimeKnown)
	{
		startTime = absSeconds;
		startTimeKnown = true;
	}
	return absSeconds - startTime;
}

// ============================================================================
// QueueAudioPacket / DecodeAudioFrame
// ============================================================================
void CVideoSourceFFmpeg::QueueAudioPacket(AVPacket *pkt)
{
	int sendRet = avcodec_send_packet(actx, pkt);
	av_packet_unref(pkt);
	if (sendRet < 0)
		return;

	while (avcodec_receive_frame(actx, audioFrame) == 0)
	{
		DecodeAudioFrame(audioFrame);
		av_frame_unref(audioFrame);
	}
}

void CVideoSourceFFmpeg::DecodeAudioFrame(AVFrame *f)
{
	// Convert format only (fltp/s32/etc -> interleaved s16); keep the source
	// sample rate and channel layout as-is (no resample, no downmix).
	if (!swrCtx || swrInFormat != f->format || swrInSampleRate != f->sample_rate ||
		swrInChannels != f->ch_layout.nb_channels)
	{
		if (swrCtx) { swr_free(&swrCtx); swrCtx = nullptr; }
		int rc = swr_alloc_set_opts2(&swrCtx,
									  &f->ch_layout, AV_SAMPLE_FMT_S16, f->sample_rate,
									  &f->ch_layout, (AVSampleFormat)f->format, f->sample_rate,
									  0, nullptr);
		if (rc < 0 || !swrCtx || swr_init(swrCtx) < 0)
		{
			if (swrCtx) { swr_free(&swrCtx); swrCtx = nullptr; }
			return;
		}
		swrInFormat = f->format;
		swrInSampleRate = f->sample_rate;
		swrInChannels = f->ch_layout.nb_channels;
	}

	int outSamples = (int)swr_get_out_samples(swrCtx, f->nb_samples);
	if (outSamples <= 0)
		outSamples = f->nb_samples;

	SDecodedAudio audio;
	audio.channels = f->ch_layout.nb_channels;
	audio.sampleRate = (u32)f->sample_rate;
	int64_t rawPts = (f->best_effort_timestamp != (int64_t)AV_NOPTS_VALUE) ? f->best_effort_timestamp : f->pts;
	audio.pts = (rawPts != (int64_t)AV_NOPTS_VALUE) ? NormalizePtsSeconds((double)rawPts * audioTimeBase) : 0.0;
	audio.pcm.resize((size_t)outSamples * (size_t)audio.channels);

	uint8_t *outPtr = reinterpret_cast<uint8_t *>(audio.pcm.data());
	int converted = swr_convert(swrCtx, &outPtr, outSamples, (const uint8_t **)f->data, f->nb_samples);
	if (converted < 0)
		return;

	audio.pcm.resize((size_t)converted * (size_t)audio.channels);
	PushPendingAudio(std::move(audio));
}

#ifdef MT_HAVE_NATIVE_AAC
// ============================================================================
// QueueAACPacket -- native AAC path (mirrors QueueAudioPacket/
// DecodeAudioFrame above, but for AV_CODEC_ID_AAC via CAudioDecoderAACNative
// -- AudioToolbox on Apple, Media Foundation on Windows)
// ============================================================================
void CVideoSourceFFmpeg::QueueAACPacket(AVPacket *pkt)
{
	std::vector<s16> pcm;
	int channels = 0;
	u32 sampleRate = 0;
	bool ok = aacDecoder->DecodePacket(pkt->data, pkt->size, pcm, channels, sampleRate);
	int64_t rawPts = pkt->pts;
	av_packet_unref(pkt);

	if (!ok)
	{
		LOGWarning("CVideoSourceFFmpeg::QueueAACPacket: DecodePacket failed: %s", aacDecoder->GetErrorReason().c_str());
		return;
	}
	if (pcm.empty())
		return; // priming -- native decoder hasn't produced output yet, not an error

	SDecodedAudio audio;
	audio.pcm = std::move(pcm);
	audio.channels = channels;
	audio.sampleRate = sampleRate;
	audio.pts = (rawPts != (int64_t)AV_NOPTS_VALUE) ? NormalizePtsSeconds((double)rawPts * audioTimeBase) : 0.0;
	PushPendingAudio(std::move(audio));
}
#endif // MT_HAVE_NATIVE_AAC

#ifdef MT_HAVE_NATIVE_WMA
// ============================================================================
// QueueWMAPacket -- native WMA path (mirrors QueueAACPacket above, but for
// the WMA family via CAudioDecoderWMANative -- Media Foundation's in-box
// WMAudio Decoder MFT; Windows only, see CAudioDecoderWMANative.h)
// ============================================================================
void CVideoSourceFFmpeg::QueueWMAPacket(AVPacket *pkt)
{
	std::vector<s16> pcm;
	int channels = 0;
	u32 sampleRate = 0;
	bool ok = wmaDecoder->DecodePacket(pkt->data, pkt->size, pcm, channels, sampleRate);
	int64_t rawPts = pkt->pts;
	av_packet_unref(pkt);

	if (!ok)
	{
		LOGWarning("CVideoSourceFFmpeg::QueueWMAPacket: DecodePacket failed: %s", wmaDecoder->GetErrorReason().c_str());
		return;
	}
	if (pcm.empty())
		return; // priming -- native decoder hasn't produced output yet, not an error

	SDecodedAudio audio;
	audio.pcm = std::move(pcm);
	audio.channels = channels;
	audio.sampleRate = sampleRate;
	audio.pts = (rawPts != (int64_t)AV_NOPTS_VALUE) ? NormalizePtsSeconds((double)rawPts * audioTimeBase) : 0.0;
	PushPendingAudio(std::move(audio));
}
#endif // MT_HAVE_NATIVE_WMA

// ============================================================================
// PumpAudioAhead -- demux forward for AUDIO only (contract in IVideoSource.h).
// Called by CVideoPlayer's decode thread when the mixer's PCM backlog runs
// low while the (4-frame) video ring is full -- the lazily-interleaved-
// container case (real-world ASF/WMV) where waiting on the ring would starve
// the consumption-driven A/V clock. Audio packets take the exact same decode
// paths the ReadVideoFrame() demux loop uses; video packets are parked
// compressed (kilobytes) for the acquisition splice above to replay in
// order. DECODE THREAD ONLY (shares the `packet` scratch).
// ============================================================================
bool CVideoSourceFFmpeg::PumpAudioAhead()
{
	if (!fmtCtx || audioStream < 0 || openVideoOnly)
		return false;
	// Decoded-PCM queue nearly full: ReadAudio()'s consumer is behind, not
	// the demuxer -- pumping more would only churn the drop-oldest cap.
	if (pendingAudioFrames.size() + 4 >= kMaxPendingAudioFrames)
		return false;

	int audioQueued = 0;
	while (audioQueued < kPumpAudioBuffersPerCall)
	{
		if (IsAborted())
			break;
		if (parkedVideoPackets.size() >= kMaxParkedPackets ||
			parkedVideoBytes >= kMaxParkedBytes)
			break; // pathological interleave -- give up, the player's free-run fallback takes over

		if (av_read_frame(fmtCtx, packet) < 0)
			break; // EOS (or abort mid-read) -- no more audio is coming

		if (packet->stream_index == audioStream)
		{
			audioQueued++;
#ifdef MT_HAVE_NATIVE_AAC
			if (aacDecoder)
			{
				QueueAACPacket(packet); // unrefs internally
				continue;
			}
#endif
#ifdef MT_HAVE_NATIVE_WMA
			if (wmaDecoder)
			{
				QueueWMAPacket(packet); // unrefs internally
				continue;
			}
#endif
			if (actx)
				QueueAudioPacket(packet); // unrefs internally
			else
				av_packet_unref(packet);
		}
		else if (packet->stream_index == videoStream)
		{
			AVPacket *parked = av_packet_alloc();
			if (!parked)
			{
				av_packet_unref(packet);
				break;
			}
			av_packet_move_ref(parked, packet);
			parkedVideoBytes += (size_t)(parked->size > 0 ? parked->size : 0);
			parkedVideoPackets.push_back(parked);
		}
		else
		{
			av_packet_unref(packet);
		}
	}
	return audioQueued > 0;
}

void CVideoSourceFFmpeg::ClearParkedPackets()
{
	for (AVPacket *parked : parkedVideoPackets)
	{
		AVPacket *p = parked;
		av_packet_free(&p);
	}
	parkedVideoPackets.clear();
	parkedVideoBytes = 0;
}

// ============================================================================
// PushPendingAudio -- bounded queue push shared by every audio decode path.
// See kMaxPendingAudioFrames (CVideoSourceFFmpeg.h) for the cap rationale.
// ============================================================================
void CVideoSourceFFmpeg::PushPendingAudio(SDecodedAudio &&audio)
{
	pendingAudioFrames.push_back(std::move(audio));
	if (pendingAudioFrames.size() > kMaxPendingAudioFrames)
	{
		size_t dropped = 0;
		while (pendingAudioFrames.size() > kMaxPendingAudioFrames)
		{
			pendingAudioFrames.pop_front();
			dropped++;
		}
		if (!droppingPendingAudio)
		{
			LOGWarning("CVideoSourceFFmpeg::PushPendingAudio: pendingAudioFrames exceeded cap (%zu) -- dropped %zu oldest frame(s); consumer is reading video without draining audio via ReadAudio()",
					   kMaxPendingAudioFrames, dropped);
		}
		droppingPendingAudio = true;
	}
	else
	{
		droppingPendingAudio = false;
	}
}

// ============================================================================
// ReadAudio
// ============================================================================
bool CVideoSourceFFmpeg::ReadAudio(SDecodedAudio &out)
{
	if (pendingAudioFrames.empty())
		return false;

	out = std::move(pendingAudioFrames.front());
	pendingAudioFrames.pop_front();
	return true;
}

// ============================================================================
// Seek
// ============================================================================
bool CVideoSourceFFmpeg::Seek(double seconds)
{
	return SeekCore(seconds, /*stopAtFirstDecodedFrame=*/false, /*forwardOnly=*/false, nullptr);
}

bool CVideoSourceFFmpeg::Seek(double seconds, bool stopAtFirstDecodedFrame)
{
	return SeekCore(seconds, stopAtFirstDecodedFrame, /*forwardOnly=*/false, nullptr);
}

// Flush codec/bitstream/audio state after a raw av_seek_frame() lands. Extracted
// (Coarse-seek task) from SeekCore()'s backward margin loop so the forward path
// shares the exact same reset; behaviour is byte-identical to the old inline
// block, which ran this on every margin retry.
void CVideoSourceFFmpeg::FlushDecodersAfterSeek()
{
	if (vctx) avcodec_flush_buffers(vctx);
	if (actx) avcodec_flush_buffers(actx);
	if (nativeDecoder) nativeDecoder->Flush(); // discard now-stale pts-reordered frames
	if (hevcBsf) av_bsf_flush(hevcBsf); // discard any access unit the filter was still holding onto
	ClearParkedPackets(); // pump-ahead parking is pre-seek demux position -- stale
#ifdef MT_HAVE_NATIVE_AAC
	// CAudioDecoderAACNative has no separate reset API (see the class's
	// interface) -- re-running Init() with the cached ASC tears down and
	// recreates the underlying native decoder (AudioConverter on Apple,
	// IMFTransform on Windows), discarding any stale internal decode
	// state left over from before the seek. On the (unexpected) failure
	// path, drop the decoder entirely rather than leaving a half-torn-down
	// instance around: without this, every subsequent QueueAACPacket() would
	// call DecodePacket() on a decoder with no live underlying transform,
	// which fails and LOGWarning()s on every single packet for the rest of
	// playback (unlike PushPendingAudio's one-shot drop warning,
	// DecodePacket's failure path has no dedupe) instead of cleanly
	// falling back to video-only, same as any other failed/missing audio
	// decoder.
	if (aacDecoder && !aacExtradata.empty())
	{
		if (!aacDecoder->Init(aacExtradata.data(), (int)aacExtradata.size()))
		{
			LOGWarning("CVideoSourceFFmpeg::Seek: CAudioDecoderAACNative re-Init failed (%s) -- dropping audio for the rest of this file",
					   aacDecoder->GetErrorReason().c_str());
			aacDecoder.reset();
			aacExtradata.clear();
		}
	}
#endif
#ifdef MT_HAVE_NATIVE_WMA
	// Same reset-by-re-Init contract as the AAC block above (see that
	// comment): a full re-Init() with the cached WAVEFORMATEX-style params
	// discards the MFT's stale pre-seek state; on failure, drop to
	// video-only cleanly rather than warn-per-packet forever.
	if (wmaDecoder && wmaInit.codecId != 0)
	{
		if (!wmaDecoder->Init(wmaInit.codecId,
							  wmaInit.extradata.empty() ? nullptr : wmaInit.extradata.data(),
							  (int)wmaInit.extradata.size(),
							  wmaInit.channels, wmaInit.sampleRate,
							  wmaInit.blockAlign, wmaInit.avgBytesPerSec))
		{
			LOGWarning("CVideoSourceFFmpeg::Seek: CAudioDecoderWMANative re-Init failed (%s) -- dropping audio for the rest of this file",
					   wmaDecoder->GetErrorReason().c_str());
			wmaDecoder.reset();
			wmaInit = SWMAInitParams();
		}
	}
#endif
	pendingAudioFrames.clear();
	droppingPendingAudio = false;
	errorReason.clear();
	debugFramesDecodedAfterSeek = 0;
}

// SeekFast (Coarse-seek task): the keyframe-only landing, now reporting the
// landed pts. Same backward-keyframe primitive the poster path uses.
bool CVideoSourceFFmpeg::SeekFast(double seconds, double *outLandedPts)
{
	return SeekCore(seconds, /*stopAtFirstDecodedFrame=*/true, /*forwardOnly=*/false, outLandedPts);
}

// SeekFastForward (Coarse-seek task): first keyframe AT OR AFTER `seconds`.
// Returns false (outLandedPts untouched) when there is no later keyframe.
bool CVideoSourceFFmpeg::SeekFastForward(double seconds, double *outLandedPts)
{
	return SeekCore(seconds, /*stopAtFirstDecodedFrame=*/true, /*forwardOnly=*/true, outLandedPts);
}

bool CVideoSourceFFmpeg::SeekCore(double seconds, bool stopAtFirstDecodedFrame,
								  bool forwardOnly, double *outLandedPts)
{
	if (!fmtCtx || videoStream < 0)
		return false;

	// Clip-relative clamp FIRST (IVideoSource.h contract), THEN translate to
	// the container-absolute av_seek_frame() target below. Clamping after
	// adding startTime would compare an absolute value against a
	// clip-relative duration and silently defeat the clamp for containers
	// with a nonzero start_time -- the original MPEG-TS/AVCHD bug shape this
	// contract fixes.
	if (seconds < 0.0)
		seconds = 0.0;
	if (info.duration > 0.0 && seconds > info.duration)
		seconds = info.duration;

	const double absSeconds = seconds + startTime;
	const double target = seconds - 0.05; // clip-relative; f.pts (below) is clip-relative too

	// FORWARD keyframe seek (Coarse-seek task): CVideoPlayer's Coarse mode calls
	// this via SeekFastForward() when a BACKWARD keyframe landing made no forward
	// progress. Seek to the first keyframe AT OR AFTER the target (no
	// AVSEEK_FLAG_BACKWARD, no margin loop -- the margins exist to walk BACKWARD to
	// find a decodable keyframe, which is the opposite intent), then stop at the
	// first decoded frame. No later keyframe, or EOF before one, -> false with an
	// EMPTY errorReason: the caller falls back to a Precise seek so progress is
	// still guaranteed. Never surfaces as a decode error (that would wedge the
	// player).
	if (forwardOnly)
	{
		if (IsAborted())
		{
			errorReason.clear();
			return false;
		}
		int64_t ts = (videoTimeBase > 0.0) ? (int64_t)std::llround(absSeconds / videoTimeBase) : 0;
		if (av_seek_frame(fmtCtx, videoStream, ts, 0) < 0)
		{
			// No keyframe at/after the target (single-keyframe/long-GOP file), or
			// an aborted seek -- either way the caller precise-fallbacks.
			errorReason.clear();
			return false;
		}
		FlushDecodersAfterSeek();

		SDecodedVideoFrame f;
		if (!ReadVideoFrame(f))
		{
			// Aborted, or EOF before any frame decoded past the forward landing:
			// treat both as "no usable later keyframe" and let the caller fall
			// back to Precise. errorReason left empty so it is never an error.
			errorReason.clear();
			return false;
		}
		if (outLandedPts)
			*outLandedPts = f.pts;
		return true;
	}

	// MPEG-TS's av_seek_frame() lands on a BYTE-POSITION ESTIMATE (there's no
	// real index for most TS streams), which can overshoot the intended
	// keyframe -- landing on a later, non-key packet instead of at/before it
	// -- especially near a short clip's tail where few PCR samples exist to
	// interpolate from (confirmed empirically against this project's
	// h264_ac3.mts fixture: seeking a hair before its last keyframe can skip
	// straight past it into an undecodable tail with no SPS/PPS-bearing
	// packet left before EOF). Retry with progressively larger backward
	// safety margins -- each retry re-seeks and re-scans forward to the same
	// clip-relative `target` -- until the scan actually reaches it instead of
	// hitting a corrupt/exhausted tail; the last candidate always drives the
	// seek to the clip's absolute start, which is guaranteed decodable
	// (Open()'s own starting position), so this loop always terminates
	// correctly.
	const double marginCandidates[] = {0.0, 0.3, 0.6, 1.0, absSeconds};
	for (double margin : marginCandidates)
	{
		// Abort (spec #2.3) between margin retries: each retry re-seeks and
		// re-scans the whole GOP, so this is the coarse escape; the fine one is
		// the per-frame check inside the scan loop below.
		if (IsAborted())
		{
			errorReason.clear();
			return false;
		}

		const double seekAbs = std::max(0.0, absSeconds - margin);
		int64_t ts = (videoTimeBase > 0.0) ? (int64_t)std::llround(seekAbs / videoTimeBase) : 0;
		if (av_seek_frame(fmtCtx, videoStream, ts, AVSEEK_FLAG_BACKWARD) < 0)
		{
			// The AVIOInterruptCB firing mid-seek makes av_seek_frame() fail with
			// AVERROR_EXIT, which is indistinguishable here from a real seek
			// failure -- classify it at this call site (which triggered the abort)
			// and return WITHOUT an errorReason: an aborted seek is not an error.
			if (IsAborted())
			{
				errorReason.clear();
				return false;
			}
			errorReason = "seek failed";
			return false;
		}

		FlushDecodersAfterSeek();

		// STREAM-HEAD seek (seconds == 0): the clip's opening frame IS the frame
		// to show, but the scan-forward loop below would consume it. That loop
		// stops at the first frame with `f.pts >= target` and DISCARDS it (the
		// next external ReadVideoFrame() returns the frame AFTER it) -- a trick
		// tuned by `target = seconds - 0.05` so a mid-clip seek discards the frame
		// ~one period before `seconds` and the caller reads the frame AT `seconds`.
		// At the head there is no earlier frame to sacrifice: target is negative,
		// so the opening frame (pts ~0) itself satisfies `pts >= target`, gets
		// discarded, and the caller is handed frame 2 -- making pts 0 unreachable
		// (a backward frame-step / seek-to-0 floors at the second frame). The
		// av_seek_frame(BACKWARD) above already parked the demuxer at the clip's
		// first keyframe, so return WITHOUT scanning: the caller's next
		// ReadVideoFrame() then returns the opening frame in presentation order.
		// Excludes the fast poster path (stopAtFirstDecodedFrame), whose own
		// first-frame stop is unaffected, and which never targets the head anyway.
		if (seconds <= 0.0 && !stopAtFirstDecodedFrame)
		{
			if (outLandedPts)
				*outLandedPts = 0.0;
			return true;
		}

		// Decode forward (discarding frames) until we reach the target; the
		// next external ReadVideoFrame() call picks up from here and returns
		// the frame at/after the seek point. Reaching here (loop falls
		// through rather than returning) means EOF arrived before the
		// target -- either this margin's landing point overshot into an
		// undecodable tail (try the next, larger margin) or the target is
		// genuinely at/past EOS (only possible once seekAbs has bottomed out
		// at the clip's absolute start, handled below).
		SDecodedVideoFrame f;
		while (true)
		{
			if (!ReadVideoFrame(f))
			{
				// ABORT CLASSIFICATION, FIRST (spec #2.3): this loop is the thing
				// the abort exists to cut short, so after ANY false return check
				// the predicate before anything else. Aborted -> discard the
				// partial scan and return false with NO errorReason: it must not
				// be surfaced as a decode error (that latches the player into
				// EVideoPlayerState::Error) and must not be taken for the
				// "EOF before the target, retry with a bigger margin" case below
				// (that would re-seek and re-scan the GOP we were told to abandon).
				if (IsAborted())
				{
					errorReason.clear();
					return false;
				}
				if (!errorReason.empty())
					return false; // genuine decode error -- not a seek-precision retry case
				break;
			}
			// stopAtFirstDecodedFrame (fast poster seek): any successfully
			// decoded frame proves this margin's landing is decodable -- stop
			// here instead of walking the GOP to the target pts.
			if (stopAtFirstDecodedFrame || f.pts >= target)
			{
				// Report the landed pts (Coarse-seek task). For the fast/coarse
				// path this is the keyframe landing -- seconds before `target` on
				// long-GOP content; for the precise walk it is the frame at/after
				// the target. Left untouched on the EOF fallthroughs below, so a
				// caller that seeded it negative falls back to the requested target.
				if (outLandedPts)
					*outLandedPts = f.pts;
				return true;
			}
		}

		if (seekAbs <= 0.0)
			return true; // exhausted every margin down to the clip's absolute start -- target is genuinely at/past EOS, matches the original documented "near EOS" behavior
	}
	return true;
}

// ============================================================================
// Close / FreeResources
// ============================================================================
void CVideoSourceFFmpeg::Close()
{
	FreeResources();
}

void CVideoSourceFFmpeg::FreeResources()
{
	// NOTE: does not touch errorReason -- Open()'s failure paths set
	// errorReason and then call Close() to unwind partial state; the message
	// must survive for the caller's subsequent GetErrorReason().
	ClearParkedPackets();
	if (packet) { av_packet_free(&packet); packet = nullptr; }
	if (bsfPacket) { av_packet_free(&bsfPacket); bsfPacket = nullptr; }
	if (hevcBsf) { av_bsf_free(&hevcBsf); hevcBsf = nullptr; }
	if (frame) { av_frame_free(&frame); frame = nullptr; }
	if (audioFrame) { av_frame_free(&audioFrame); audioFrame = nullptr; }
	if (vctx) { avcodec_free_context(&vctx); vctx = nullptr; }
	if (actx) { avcodec_free_context(&actx); actx = nullptr; }
	if (swsCtx) { sws_freeContext(swsCtx); swsCtx = nullptr; }
	if (swrCtx) { swr_free(&swrCtx); swrCtx = nullptr; }
	if (fmtCtx) { avformat_close_input(&fmtCtx); fmtCtx = nullptr; }

	// Hand back the abort predicate before letting go of the decoder (Task 6).
	// SetAbortPredicate()/Open() FORWARD our predicate into it, and that
	// std::function captures the CVideoPlayer that made it. An EXTERNALLY-owned
	// decoder (SetHEVCPacketDecoder(), a test-only seam) outlives this source, so
	// without this it would keep holding a predicate pointing at a player that is
	// already gone -- and the next DecodePacket() on it (say, after the decoder is
	// handed to a second player) would evaluate it. Nothing in-tree reuses a
	// decoder across players today; this closes the door anyway. The owned decoder
	// below is destroyed immediately after, so for it this is merely tidy.
	if (nativeDecoder)
		nativeDecoder->SetAbortPredicate(nullptr);

	// Only tear down nativeDecoder when it's the instance Open() auto-installed
	// (ownedNativeDecoder); an externally-injected decoder (SetHEVCPacketDecoder)
	// is owned by the caller and must survive Close()/re-Open() cycles on this
	// same CVideoSourceFFmpeg instance.
	if (ownedNativeDecoder && nativeDecoder == ownedNativeDecoder.get())
		nativeDecoder = nullptr;
	ownedNativeDecoder.reset();

#ifdef MT_HAVE_NATIVE_AAC
	aacDecoder.reset();
	aacExtradata.clear();
#endif
#ifdef MT_HAVE_NATIVE_WMA
	wmaDecoder.reset();
	wmaInit = SWMAInitParams();
#endif

	pendingAudioFrames.clear();
	droppingPendingAudio = false;
	swsBuffer.clear();

	videoStream = -1;
	audioStream = -1;
	videoTimeBase = 0.0;
	audioTimeBase = 0.0;
	swsSrcFormat = -1;
	swsSrcW = 0;
	swsSrcH = 0;
	swrInFormat = -1;
	swrInSampleRate = 0;
	swrInChannels = -1;
	startTime = 0.0;
	startTimeKnown = false;
	debugFramesDecodedAfterSeek = 0;

	info = SVideoInfo();
}

#if !defined(__APPLE__) && !defined(_WIN32)
// Builds with no native HEVC decoder story at all (Linux today) have no
// IVideoPacketDecoder implementation registered anywhere -- this is the
// "false stub elsewhere" IVideoPacketDecoder.h's comment refers to, kept
// alongside the interface's only other FFmpeg-enabled TU so the symbol
// always exists once FFmpeg is linked in. Apple defines this in
// CVideoDecoderHEVCVT.cpp (always true -- VideoToolbox); Windows defines it
// in CVideoDecoderHEVCMF.cpp (true iff an HEVC decoder MFT resolves).
bool IVideoPacketDecoder::IsHEVCDecodeAvailable()
{
	return false;
}
#endif

#if !defined(_WIN32)
// Native WMV decode exists only on Windows (the in-box WMVideo Decoder MFT
// -- see CVideoDecoderWMVMF.cpp for the true implementation). Apple AND
// Linux take this false stub: VideoToolbox has no WMV/VC-1 support at all,
// so unlike HEVC there is no Apple-side implementation file to host it.
bool IVideoPacketDecoder::IsWMVDecodeAvailable()
{
	return false;
}
#endif

#endif // MT_ENABLE_FFMPEG
