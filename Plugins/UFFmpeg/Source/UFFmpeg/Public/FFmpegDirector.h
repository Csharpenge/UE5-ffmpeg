// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "AudioMixerDevice.h"
#include "ISubmixBufferListener.h"
#include "RHIResources.h"
#include "RHICommandList.h"
#include "FFmpegCompat.h"
#include "FFmpegDirector.generated.h"

/**
 * 
 */
class FEncoderThread;
class UFFmpegDirector;
class ISlateViewportProvider;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRecordFinishDelegate, FString, FileAddress);

/**
 * Bridges the audio mixer listener API to the director.
 *
 * ISubmixBufferListener is a TSharedFromThis since UE 5.4, so it can no longer be
 * implemented directly by a UObject; this adapter is held by TSharedRef instead.
 */
class FFFmpegSubmixListener : public ISubmixBufferListener
{
public:
	explicit FFFmpegSubmixListener(UFFmpegDirector* InOwner)
		: Owner(InOwner)
	{
	}

	virtual void OnNewSubmixBuffer(const USoundSubmix* OwningSubmix, float* AudioData, int32 NumSamples, int32 NumChannels, const int32 SampleRate, double AudioClock) override;
	virtual bool IsRenderingAudio() const override;
	virtual const FString& GetListenerName() const override;

private:
	UFFmpegDirector* Owner;
};

UCLASS(BlueprintType)
class UFFMPEG_API UFFmpegDirector : public UObject
{
	GENERATED_BODY()
public:

	UFFmpegDirector();
	virtual ~UFFmpegDirector();

	/**
	 * Creates the muxer/encoders and starts capturing.
	 * Returns false when FFmpeg cannot provide a usable H.264/AAC encoder or when the
	 * output cannot be opened; GetLastError() then contains the FFmpeg error text.
	 * The plugin never asserts on FFmpeg failures, so a failure here is non-fatal for the game.
	 *
	 * VideoFilter format: "scale=WIDTH:HEIGHT" (for example "scale=1920:1080").
	 * Leave it empty to record at the current viewport resolution.
	 *
	 * @param VideoLength            Recording length. Seconds by default; with bVideoLengthInSeconds=false it is a frame count.
	 * @param VideoFilter            "scale=WIDTH:HEIGHT", or empty for the current viewport resolution.
	 * @param bVideoLengthInSeconds  When true (default) VideoLength is interpreted as seconds and converted with FPS.
	 * @param bIsRtmp                When true OutFileName is treated as an RTMP URL and the FLV muxer is used.
	 *                               When false (default) an URL that already starts with "rtmp" is auto detected.
	 */
	UFUNCTION(BlueprintCallable, Category = "FFmpeg", meta = (ToolTip = "Creates the muxer/encoders and starts capturing.\nVideoFilter format: \"scale=WIDTH:HEIGHT\", e.g. scale=1920:1080. Leave empty to record at the current viewport resolution.\nReturns false on failure; GetLastError() then contains the reason."))
	bool Initialize_Director(UWorld* World, int32 VideoLength, FString OutFileName, bool UseGPU, UPARAM(meta = (ToolTip = "Output scaling, format: scale=WIDTH:HEIGHT (e.g. scale=1920:1080). Leave empty to use the current viewport resolution.")) FString VideoFilter, int VideoFps, int VideoBitRate, float AudioDelay, float SoundVolume, bool bVideoLengthInSeconds = true, UPARAM(meta = (ToolTip = "Treat OutFileName as an RTMP URL and use the FLV muxer. Leave false for local files; rtmp:// URLs are auto detected anyway.")) bool bIsRtmp = false);

	/** True when Initialize_Director succeeded and capture is running. */
	UFUNCTION(BlueprintPure, Category = "FFmpeg")
	bool IsInitialized() const { return bInitialized; }

	/** Human readable reason of the last failure (empty when everything succeeded). */
	UFUNCTION(BlueprintPure, Category = "FFmpeg")
	FString GetLastError() const { return LastError; }

	/** Name of the H.264 encoder that was actually opened (e.g. "libx264", "h264_nvenc"). */
	UFUNCTION(BlueprintPure, Category = "FFmpeg")
	FString GetVideoEncoderName() const { return VideoEncoderName; }

	void Begin_Receive_AudioData(UWorld* world);
	void Begin_Receive_VideoData();

	void Encode_Video_Frame(uint8_t* rgb);
	void Encode_SetCurrentAudioTime(uint8_t* rgb);
	void Encode_Audio_Frame(uint8_t* rgb);
	void Encode_Finish();

	void OnNewSubmixBuffer(const USoundSubmix* OwningSubmix, float* AudioData, int32 NumSamples, int32 NumChannels, const int32 SampleRate, double AudioClock);
	void OnBackBufferReady_RenderThread(SWindow& SlateWindow, ISlateViewportProvider& ViewportProvider);
	bool AddTickTime(float time);
	bool CheckThreadJobDone(float time);

	void DestoryDirector();

	UPROPERTY(BlueprintAssignable, Category = "FFmpeg")
		FOnRecordFinishDelegate OnRecordFinish;

private:
	static FString AvErrorToString(int ErrorCode);

	/** Stores the message, logs it and clears bInitialized. Always returns false. */
	bool Fail(const FString& Message);

	// Encoder / muxer setup. All of them return false and fill LastError on failure.
	bool Create_Audio_Swr();
	bool Create_Audio_Encoder(const char* audioencoder_name);
	bool Create_Video_Encoder(bool bPreferGPU, const char* out_file_name, int bit_rate);
	bool Alloc_Video_Filter();
	bool CreateEncodeThread();
	bool OpenVideoEncoderCandidate(const AVCodec* Codec, int32 BitRate);

	bool Video_Frame_YUV_From_BGR(uint8_t* rgb);
	void GetScreenVideoData();

	void AddTickFunction();
	void Set_Audio_Volume(AVFrame* frame);

	//Stop capture Frame and wait for the encode finish
	void StopCapture();

	//AllDone, Send the Video and Quit game.
	void Stop(UWorld* _world);

	/** Frees every FFmpeg resource exactly once (idempotent). */
	void FreeEncodingResources();

	uint32 TotalFrame = 0;
	uint32 FrameCount = 0;

private:
	bool bInitialized = false;
	bool bHeaderWritten = false;
	bool IsDestory = false;
	bool IsClosing = false;
	/** True when the output is an RTMP stream (FLV muxer) instead of a local file. */
	bool bRtmpOutput = false;
	FString filter_descr;
	FString FileAddr;
	FString LastError;
	FString VideoEncoderName;

	int video_fps = 30;
	float Video_Tick_Time = 0.0f;
	double CurrentAuidoTime = 0.0;
	float audio_delay = 0.0f;
	float audio_volume = 1.0f;
	uint32 width = 0;
	uint32 height = 0;
	uint32 out_width = 0;
	uint32 out_height = 0;

	uint32 finishVideocounter = 0;
	uint32 finishAudiocounter = 0;

	FTextureRHIRef GameTexture;

	AVFilterInOut* outputs = nullptr;
	AVFilterInOut* inputs = nullptr;
	AVFilterGraph* filter_graph = nullptr;
	AVFilterContext* buffersink_ctx = nullptr;
	AVFilterContext* buffersrc_ctx = nullptr;

	FAudioDevice* AudioDevice = nullptr;
	SWindow* gameWindow = nullptr;
	TArray<FColor> TexturePixel;
	float ticktime = 0.0f;
	int64_t video_pts = 0;
	uint8_t* buff_bgr = nullptr;
	int32_t video_index = -1;
	int32_t audio_index = -1;

	/** Submix listener registered with the audio device (shared reference required by UE 5.4+). */
	TSharedPtr<FFFmpegSubmixListener, ESPMode::ThreadSafe> SubmixListener;
	/** Submix the listener was registered with, used to unregister it. */
	TWeakObjectPtr<USoundSubmix> RegisteredSubmix;

	FEncoderThread* Runnable = nullptr;
	FRunnableThread* RunnableThread = nullptr;

	AVFormatContext* out_format_context = nullptr;
	AVCodecContext* video_encoder_codec_context = nullptr;
	AVCodecContext* audio_encoder_codec_context = nullptr;

	SwsContext* sws_context = nullptr;
	AVStream* out_video_stream = nullptr;
	AVStream* out_audio_stream = nullptr;
	SwrContext* swr = nullptr;

	AVFrame* audio_frame = nullptr;
	AVFrame* video_frame = nullptr;
	AVFrame* filt_frame = nullptr;
	AVPacket* video_pkt = nullptr;
	AVPacket* audio_pkt = nullptr;
	int32 AudioFrameCapacity = 0;

	FTSTicker::FDelegateHandle TickDelegateHandle;
	FTSTicker::FDelegateHandle CheckDelegateHandle;
	FDelegateHandle BackBufferReadyHandle;

	UWorld* FD_world = nullptr;

	uint32 LolStride = 0;
	TEnumAsByte<EWorldType::Type> GameMode;
};
