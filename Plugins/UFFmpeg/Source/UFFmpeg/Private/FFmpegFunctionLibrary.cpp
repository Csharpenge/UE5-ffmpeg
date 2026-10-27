// Fill out your copyright notice in the Description page of Project Settings.


#include "FFmpegFunctionLibrary.h"
#include "FFmpegDirector.h"
#include "FFmpegCompat.h"
#include "Kismet/GameplayStatics.h"

DEFINE_LOG_CATEGORY(LogFFmpeg);

FString UFFmpegFunctionLibrary::LastError;

UFFmpegDirector* UFFmpegFunctionLibrary::CreateFFmpegDirector(UWorld* World, int32 VideoLength, FString OutFileName, FString VideoFilter, bool UseGPU, int FPS, int VideoBitRate, float AudioDelay, float SoundVolume, bool bVideoLengthInSeconds, bool bIsRtmp)
{
	if (!World)
	{
		LastError = TEXT("Cannot start recording: the world is null.");
		UE_LOG(LogFFmpeg, Error, TEXT("UFFmpeg: %s"), *LastError);
		return nullptr;
	}

	UFFmpegDirector* Director = NewObject<UFFmpegDirector>();
	if (!Director)
	{
		LastError = TEXT("Cannot start recording: failed to allocate UFFmpegDirector.");
		UE_LOG(LogFFmpeg, Error, TEXT("UFFmpeg: %s"), *LastError);
		return nullptr;
	}

	Director->AddToRoot();

	if (!Director->Initialize_Director(World, VideoLength, OutFileName, UseGPU, VideoFilter, FPS, VideoBitRate, AudioDelay, SoundVolume, bVideoLengthInSeconds, bIsRtmp))
	{
		LastError = Director->GetLastError();
		UE_LOG(LogFFmpeg, Error, TEXT("UFFmpeg: recording could not be started: %s"), *LastError);

		Director->RemoveFromRoot();
		Director->ConditionalBeginDestroy();
		return nullptr;
	}

	LastError.Reset();
	return Director;
}


UWorld* UFFmpegFunctionLibrary::GetWorldContext(UObject* WorldContextObject)
{
	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull) : nullptr;
	return World;
}

FString UFFmpegFunctionLibrary::GetLastError()
{
	return LastError;
}

FString UFFmpegFunctionLibrary::GetFFmpegVersion()
{
	return FString(ANSI_TO_TCHAR(av_version_info()));
}
