// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "UFFmpeg.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "FFmpegCompat.h"

#define LOCTEXT_NAMESPACE "FUffmpegModule"

#ifndef UFFMPEG_SYSTEM_FFMPEG
	#define UFFMPEG_SYSTEM_FFMPEG 0
#endif

namespace
{
	/** Root of a system (vcpkg/distro) FFmpeg selected at build time, empty for the vendored copy. */
	FString GetSystemFFmpegRoot()
	{
#if UFFMPEG_SYSTEM_FFMPEG
		FString FromEnvironment = FPlatformMisc::GetEnvironmentVariable(TEXT("UFFMPEG_SYSTEM_FFMPEG_ROOT"));
		if (!FromEnvironment.IsEmpty())
		{
			return FromEnvironment;
		}
		return FString(ANSI_TO_TCHAR(UFFMPEG_SYSTEM_ROOT));
#else
		return FString();
#endif
	}

	/** Logs the runtime FFmpeg version and warns when it does not match the headers used to compile. */
	void LogRuntimeVersion()
	{
		const unsigned int CodecVersion = avcodec_version();
		const unsigned int UtilVersion = avutil_version();
		const FString VersionInfo(ANSI_TO_TCHAR(av_version_info()));

		UE_LOG(LogTemp, Log, TEXT("UFFmpeg: FFmpeg runtime '%s' (avcodec %d.%d.%d, avutil %d.%d.%d); compiled against avcodec %d / avutil %d / swresample %d"),
			*VersionInfo,
			AV_VERSION_MAJOR(CodecVersion), AV_VERSION_MINOR(CodecVersion), AV_VERSION_MICRO(CodecVersion),
			AV_VERSION_MAJOR(UtilVersion), AV_VERSION_MINOR(UtilVersion), AV_VERSION_MICRO(UtilVersion),
			LIBAVCODEC_VERSION_MAJOR, LIBAVUTIL_VERSION_MAJOR, LIBSWRESAMPLE_VERSION_MAJOR);

		if (AV_VERSION_MAJOR(CodecVersion) != LIBAVCODEC_VERSION_MAJOR || AV_VERSION_MAJOR(UtilVersion) != LIBAVUTIL_VERSION_MAJOR)
		{
			UE_LOG(LogTemp, Error,
				TEXT("UFFmpeg: FFmpeg runtime/header version mismatch (runtime avcodec %d, avutil %d; headers avcodec %d, avutil %d). Recording may crash - use matching headers and binaries."),
				AV_VERSION_MAJOR(CodecVersion), AV_VERSION_MAJOR(UtilVersion),
				LIBAVCODEC_VERSION_MAJOR, LIBAVUTIL_VERSION_MAJOR);
		}
	}
}

void FUFFmpegModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
	//
	// The runtime library names follow the headers the module was compiled against, so the same
	// code works with FFmpeg 4.x (avcodec-58), 6.x (avcodec-60), 8.x (avcodec-62) and 9.x (avcodec-63).
	const FString SystemRoot = GetSystemFFmpegRoot();
	if (!SystemRoot.IsEmpty())
	{
#if PLATFORM_WINDOWS
		const FString BinaryDirectory = FPaths::Combine(SystemRoot, TEXT("bin"));
		if (FPaths::DirectoryExists(BinaryDirectory))
		{
			FPlatformProcess::PushDllDirectory(*BinaryDirectory);
		}
#endif
		UE_LOG(LogTemp, Log, TEXT("UFFmpeg: using system FFmpeg installation at '%s'."), *SystemRoot);
	}

	AVUtilLibrary = LoadLibrary(TEXT("avutil"), FString::FromInt(LIBAVUTIL_VERSION_MAJOR));
	SWResampleLibrary = LoadLibrary(TEXT("swresample"), FString::FromInt(LIBSWRESAMPLE_VERSION_MAJOR));
	AVCodecLibrary = LoadLibrary(TEXT("avcodec"), FString::FromInt(LIBAVCODEC_VERSION_MAJOR));
	AVFormatLibrary = LoadLibrary(TEXT("avformat"), FString::FromInt(LIBAVFORMAT_VERSION_MAJOR));
	SWScaleLibrary = LoadLibrary(TEXT("swscale"), FString::FromInt(LIBSWSCALE_VERSION_MAJOR));
	AVFilterLibrary = LoadLibrary(TEXT("avfilter"), FString::FromInt(LIBAVFILTER_VERSION_MAJOR));

	Initialized = true;

#if PLATFORM_WINDOWS || PLATFORM_MAC
	if (AVUtilLibrary && AVCodecLibrary && AVFormatLibrary)
	{
		LogRuntimeVersion();
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("UFFmpeg: one or more FFmpeg runtime libraries could not be loaded. Install FFmpeg %s or set UFFMPEG_SYSTEM_FFMPEG_ROOT. Recording will not be available."),
			ANSI_TO_TCHAR(UFFMPEG_COMPILED_VERSION_TAG));
	}
#else
	// Linux / Android link FFmpeg statically, the version symbols are always available.
	LogRuntimeVersion();
#endif
}

void FUFFmpegModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
	if (!Initialized)
	{
		return;
	}
	if (AVFilterLibrary) FPlatformProcess::FreeDllHandle(AVFilterLibrary);
	if (SWScaleLibrary) FPlatformProcess::FreeDllHandle(SWScaleLibrary);
	if (AVFormatLibrary) FPlatformProcess::FreeDllHandle(AVFormatLibrary);
	if (AVCodecLibrary) FPlatformProcess::FreeDllHandle(AVCodecLibrary);
	if (SWResampleLibrary) FPlatformProcess::FreeDllHandle(SWResampleLibrary);
	if (AVUtilLibrary) FPlatformProcess::FreeDllHandle(AVUtilLibrary);

	Initialized = false;
}

void* FUFFmpegModule::LoadLibrary(const FString& name, const FString& version)
{
	FString LibDir;
	FString extension;
	FString prefix;
	FString separator;

	const FString SystemRoot = GetSystemFFmpegRoot();

#if PLATFORM_MAC
	extension = TEXT(".dylib");
	prefix = "lib";
	separator = ".";
	if (!SystemRoot.IsEmpty())
	{
		LibDir = FPaths::Combine(SystemRoot, TEXT("lib"));
	}
	else if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UFFmpeg")))
	{
		LibDir = FPaths::Combine(*Plugin->GetBaseDir(), TEXT("ThirdParty/ffmpeg/lib/Mac"));
	}
#elif PLATFORM_WINDOWS
	// UE5 only ships 64-bit targets, so the x64 binaries are always used.
	extension = TEXT(".dll");
	prefix = "";
	separator = "-";
	if (!SystemRoot.IsEmpty())
	{
		LibDir = FPaths::Combine(SystemRoot, TEXT("bin"));
	}
	else if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UFFmpeg")))
	{
		LibDir = FPaths::Combine(*Plugin->GetBaseDir(), TEXT("ThirdParty/ffmpeg/bin/Win64"));
	}
#endif

	if (LibDir.IsEmpty())
	{
		// Linux / Android: FFmpeg is linked statically, nothing to load here.
		return nullptr;
	}

	const FString LibraryPath = FPaths::Combine(*LibDir, prefix + name + separator + version + extension);
	void* Handle = FPlatformProcess::GetDllHandle(*LibraryPath);
	if (!Handle)
	{
		UE_LOG(LogTemp, Warning, TEXT("UFFmpeg: failed to load '%s'."), *LibraryPath);
	}
	return Handle;
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FUFFmpegModule, UFFmpeg)
