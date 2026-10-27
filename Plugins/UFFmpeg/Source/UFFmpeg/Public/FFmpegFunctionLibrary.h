// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "FFmpegFunctionLibrary.generated.h"

/**
 * 
 */

DECLARE_LOG_CATEGORY_EXTERN(LogFFmpeg, Log, All);

class UFFmpegDirector;

UCLASS()
class UFFMPEG_API UFFmpegFunctionLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

	/**
	 * Starts recording the game viewport to a file or an RTMP stream.
	 *
	 * VIDEO FILTER FORMAT
	 *   "scale=WIDTH:HEIGHT"  e.g. "scale=1920:1080" or "scale=1280:720"
	 *   Leave it empty to record at the current viewport resolution.
	 *   Width and height are rounded down to even numbers (H.264/YUV420P requirement).
	 *   Any other string is ignored with a warning and the viewport resolution is used.
	 *
	 * RETURNS
	 *   A valid UFFmpegDirector on success, or nullptr on failure.
	 *   On failure the reason is available from GetLastError().
	 *   Always check the return value before binding OnRecordFinish.
	 *
	 * @param World                  World context (usually from Get World).
	 * @param VideoLength            Recording length in seconds, or a frame count when bVideoLengthInSeconds is false.
	 * @param OutFileName            Absolute output path (.mp4/.flv/.mkv) or an rtmp:// URL.
	 *                               Characters that Windows forbids in file names (< > : " | ? *) are replaced with '_'.
	 * @param VideoFilter            "scale=WIDTH:HEIGHT", or empty for the current viewport resolution.
	 * @param UseGPU                 Prefer hardware encoders (h264_nvenc/qsv/amf/mf); falls back automatically.
	 * @param FPS                    Frames per second (clamped to 1-240).
	 * @param VideoBitRate           Target bit rate in bits per second (e.g. 6000000 = 6 Mbps).
	 * @param AudioDelay             Audio delay in seconds.
	 * @param SoundVolume            Linear gain: 1.0 = original volume, 0.5 = 50%, values above 8.0 are clamped.
	 * @param bVideoLengthInSeconds  true: VideoLength is seconds (default). false: VideoLength is a frame count.
	 * @param bIsRtmp                true: OutFileName is an RTMP URL, use the FLV muxer (streaming).
	 *                               false (default): local file; "rtmp://" URLs are auto detected anyway.
	 */
	UFUNCTION(BlueprintCallable, Category = "FFMpegEncoder", meta = (ToolTip = "Starts recording the game viewport to a file or an RTMP stream.\nVideoFilter format: \"scale=WIDTH:HEIGHT\", e.g. scale=1920:1080. Leave empty to record at the current viewport resolution.\nSet IsRtmp to true (or pass an rtmp:// URL) to stream instead of writing a local file.\nReturns nullptr on failure; check the result before binding OnRecordFinish and read GetLastError() for the reason."))
	static UFFmpegDirector* CreateFFmpegDirector(
		UWorld* World,
		int32 VideoLength,
		FString OutFileName,
		UPARAM(meta = (ToolTip = "Output scaling, format: scale=WIDTH:HEIGHT (e.g. scale=1920:1080). Leave empty to use the current viewport resolution.")) FString VideoFilter,
		bool UseGPU,
		int FPS,
		int VideoBitRate,
		float AudioDelay,
		float SoundVolume,
		bool bVideoLengthInSeconds = true,
		UPARAM(meta = (ToolTip = "Treat OutFileName as an RTMP URL and use the FLV muxer. Leave false for local files; rtmp:// URLs are auto detected anyway.")) bool bIsRtmp = false);

	UFUNCTION(BlueprintCallable, Category = "FFMpegEncoder")
	static UWorld* GetWorldContext(UObject* WorldContextObject);

	/** Reason why the last CreateFFmpegDirector() call failed (empty on success). */
	UFUNCTION(BlueprintPure, Category = "FFMpegEncoder")
	static FString GetLastError();

	/** FFmpeg runtime version string, e.g. "8.0.1" or "9.0.2". Useful to verify which library the build actually loaded. */
	UFUNCTION(BlueprintPure, Category = "FFMpegEncoder")
	static FString GetFFmpegVersion();

private:
	static FString LastError;
};
