// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

/**
 * Central FFmpeg include + API compatibility layer.
 *
 * The plugin supports both the vendored FFmpeg 9.x copy under ThirdParty/ffmpeg and a
 * system (vcpkg / distro) FFmpeg selected at build time through UFFMPEG_SYSTEM_FFMPEG_ROOT.
 * Since those installations can be 4.x / 6.x / 8.x / 9.x, every API that changed between
 * those versions is gated by the LIBAV*_VERSION_MAJOR macros below.
 *
 * Version overview (libavcodec / libavutil / libswresample):
 *   4.x : 58 / 56 / 3
 *   6.x : 60 / 58 / 4
 *   8.x : 62 / 60 / 6
 *   9.x : 63 / 61 / 7
 */

extern "C"
{
#include "libavformat/avformat.h"
#include "libavformat/avio.h"
#include "libavcodec/avcodec.h"
#include "libavutil/avutil.h"
#include "libavutil/error.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/imgutils.h"
#include "libavutil/frame.h"
#include "libavutil/samplefmt.h"
#include "libavutil/pixfmt.h"
#include "libavutil/channel_layout.h"
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersrc.h"
#include "libavfilter/buffersink.h"

// Explicit version headers: the runtime DLL names and the feature gates below depend on them.
#include "libavutil/version.h"
#include "libavcodec/version.h"
#include "libavformat/version.h"
#include "libavfilter/version.h"
#include "libswscale/version.h"
#include "libswresample/version.h"
}

// ---------------------------------------------------------------------------
// Feature gates (based on public version macros, so they follow whatever
// headers the module is compiled against).
// ---------------------------------------------------------------------------

/** AVChannelLayout / ch_layout fields and av_channel_layout_* helpers (FFmpeg >= 5.1). */
#if LIBAVUTIL_VERSION_MAJOR >= 58
	#define UFFMPEG_HAVE_CHLAYOUT 1
#else
	#define UFFMPEG_HAVE_CHLAYOUT 0
#endif

/** swr_alloc_set_opts2 (FFmpeg >= 5.1, libswresample >= 4). */
#if UFFMPEG_HAVE_CHLAYOUT && (LIBSWRESAMPLE_VERSION_MAJOR >= 4)
	#define UFFMPEG_HAVE_SWR_OPTS2 1
#else
	#define UFFMPEG_HAVE_SWR_OPTS2 0
#endif

/** AV_PROFILE_* constants (FFmpeg >= 5.1); older versions only have FF_PROFILE_*. */
#if LIBAVCODEC_VERSION_MAJOR >= 59
	#define UFFMPEG_HAVE_AV_PROFILE 1
#else
	#define UFFMPEG_HAVE_AV_PROFILE 0
#endif

// ---------------------------------------------------------------------------
// Human readable tags used for logging / diagnostics.
// ---------------------------------------------------------------------------

#define UFFMPEG_STRINGIFY_HELPER(x) #x
#define UFFMPEG_STRINGIFY(x) UFFMPEG_STRINGIFY_HELPER(x)

/** Compile-time FFmpeg version tag, e.g. "9.x (avcodec 63, avutil 61, swresample 7)". */
#define UFFMPEG_COMPILED_VERSION_TAG \
	UFFMPEG_STRINGIFY(LIBAVCODEC_VERSION_MAJOR) ".x (avcodec " UFFMPEG_STRINGIFY(LIBAVCODEC_VERSION_MAJOR) \
	", avutil " UFFMPEG_STRINGIFY(LIBAVUTIL_VERSION_MAJOR) \
	", swresample " UFFMPEG_STRINGIFY(LIBSWRESAMPLE_VERSION_MAJOR) ")"

/** Root directory of a system FFmpeg installation, baked in by UFFmpeg.Build.cs. */
#ifndef UFFMPEG_SYSTEM_ROOT
	#define UFFMPEG_SYSTEM_ROOT ""
#endif
