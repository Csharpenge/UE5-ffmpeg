// Fill out your copyright notice in the Description page of Project Settings.


#include "FFmpegDirector.h"
#include "FFmpegFunctionLibrary.h"

#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Slate/SceneViewport.h"
#include "Slate/SlateViewportProvider.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/SlateRenderer.h"
#include "EncoderThread.h"
#include "EncodeData.h"
#include "Logging/LogVerbosity.h"
#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "RenderingThread.h"
#include "RHI.h"
#include "Misc/CoreDelegates.h"
#include "GameDelegates.h"
#include "Kismet/GameplayStatics.h"
#include "ImageUtils.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <objbase.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif
#include <thread>

namespace
{
	/**
	 * MediaFoundation encoders (h264_mf) call CoInitializeEx(COINIT_MULTITHREADED) internally and
	 * fail with EINVAL ("Invalid argument") when the calling thread is already in an STA apartment.
	 * The UE game thread is STA, so encoder open/close has to happen on a short lived MTA thread.
	 * The encoder thread itself is also initialized as MTA (see FEncoderThread::Init).
	 */
	template <typename FuncType>
	void RunOnMtaComThread(FuncType&& Func)
	{
#if PLATFORM_WINDOWS
		std::thread Worker([Func = std::forward<FuncType>(Func)]()
		{
			const HRESULT ComResult = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			Func();
			if (SUCCEEDED(ComResult))
			{
				::CoUninitialize();
			}
		});
		Worker.join();
#else
		Func();
#endif
	}

	/**
	 * Replaces characters Windows forbids in file names (< > : " | ? *) with '_'.
	 * A drive letter colon at index 1 is kept. Returns true when the path was modified.
	 */
	bool SanitizeWindowsPath(FString& Path)
	{
#if PLATFORM_WINDOWS
		bool bChanged = false;
		for (int32 Index = 0; Index < Path.Len(); ++Index)
		{
			TCHAR& Character = Path[Index];
			if (Character == TEXT(':'))
			{
				if (Index == 1)
				{
					continue;
				}
				Character = TEXT('_');
				bChanged = true;
				continue;
			}

			if (Character == TEXT('<') || Character == TEXT('>') || Character == TEXT('"') ||
				Character == TEXT('|') || Character == TEXT('?') || Character == TEXT('*'))
			{
				Character = TEXT('_');
				bChanged = true;
			}
		}
		return bChanged;
#else
		(void)Path;
		return false;
#endif
	}
}

UFFmpegDirector::UFFmpegDirector()
{
	video_fps = 30;
	Video_Tick_Time = 1.0f / 30.0f;
	audio_volume = 1.0f;
	GameMode = EWorldType::Game;
}

UFFmpegDirector::~UFFmpegDirector()
{
	// DestoryDirector() normally released everything already; this is a safety net for
	// objects that were garbage collected without a proper shutdown.
	FreeEncodingResources();
}

FString UFFmpegDirector::AvErrorToString(int ErrorCode)
{
	char Buffer[AV_ERROR_MAX_STRING_SIZE] = { 0 };
	av_strerror(ErrorCode, Buffer, sizeof(Buffer));
	return FString(ANSI_TO_TCHAR(Buffer));
}

bool UFFmpegDirector::Fail(const FString& Message)
{
	LastError = Message;
	bInitialized = false;
	UE_LOG(LogFFmpeg, Error, TEXT("UFFmpeg: %s"), *Message);
	return false;
}

void UFFmpegDirector::DestoryDirector()
{
	if (IsDestory)
	{
		return;
	}
	IsDestory = true;

	if (RunnableThread)
	{
		RunnableThread->Kill(true);
		delete RunnableThread;
		RunnableThread = nullptr;
	}
	if (Runnable)
	{
		delete Runnable;
		Runnable = nullptr;
	}

	if (AudioDevice && SubmixListener.IsValid())
	{
		if (USoundSubmix* Submix = RegisteredSubmix.Get())
		{
			AudioDevice->UnregisterSubmixBufferListener(SubmixListener.ToSharedRef(), *Submix);
		}
		SubmixListener.Reset();
	}

	if (FSlateApplication::IsInitialized())
	{
		if (FSlateRenderer* Renderer = FSlateApplication::Get().GetRenderer())
		{
			Renderer->OnBackBufferReadyToPresent().RemoveAll(this);
		}
	}

	FTSTicker::GetCoreTicker().RemoveTicker(TickDelegateHandle);
	FTSTicker::GetCoreTicker().RemoveTicker(CheckDelegateHandle);

	Encode_Finish();

	// Release the GC root taken in CreateFFmpegDirector(): the director is fully torn down here,
	// keeping it rooted would leak one object per recording.
	RemoveFromRoot();
}

void UFFmpegDirector::Begin_Receive_AudioData(UWorld* world)
{
	if (!world)
	{
		return;
	}

	GameMode = world->WorldType;
	AudioDevice = world->GetAudioDevice().GetAudioDevice();
	if (!AudioDevice)
	{
		UE_LOG(LogFFmpeg, Warning, TEXT("UFFmpeg: no audio device available, the recording will not contain audio."));
		return;
	}

	SubmixListener = MakeShared<FFFmpegSubmixListener, ESPMode::ThreadSafe>(this);
	USoundSubmix& MainSubmix = AudioDevice->GetMainSubmixObject();
	RegisteredSubmix = &MainSubmix;
	AudioDevice->RegisterSubmixBufferListener(SubmixListener.ToSharedRef(), MainSubmix);
}

bool UFFmpegDirector::Initialize_Director(UWorld* World, int32 VideoLength, FString OutFileName, bool UseGPU, FString VideoFilter, int VideoFps, int VideoBitRate, float AudioDelay, float SoundVolume, bool bVideoLengthInSeconds, bool bIsRtmp)
{
	if (bInitialized)
	{
		return true;
	}

	LastError.Reset();
	VideoEncoderName.Reset();
	IsDestory = false;
	IsClosing = false;
	FrameCount = 0;
	finishVideocounter = 0;
	finishAudiocounter = 0;
	video_pts = 0;

	if (!World)
	{
		return Fail(TEXT("Initialize_Director called with a null world."));
	}
	if (!GEngine || !GEngine->GameViewport)
	{
		return Fail(TEXT("No game viewport is available (is the game running?)."));
	}

	gameWindow = GEngine->GameViewport->GetWindow().Get();
	if (!gameWindow)
	{
		return Fail(TEXT("The Slate window of the game viewport is not available."));
	}

	if (OutFileName.IsEmpty())
	{
		return Fail(TEXT("OutFileName is empty."));
	}

	// Windows does not allow < > : " | ? * in file names, but Blueprint time stamps commonly use
	// ':' (e.g. "18:44:53"). Replace them instead of failing with an opaque EINVAL from avio_open.
	FString FinalOutFileName = OutFileName;

	// The Blueprint flag forces RTMP/FLV output. URLs that already start with "rtmp" are still
	// auto detected so existing Blueprints keep working without the new pin.
	bRtmpOutput = bIsRtmp || FinalOutFileName.StartsWith(TEXT("rtmp"));

	if (bRtmpOutput)
	{
		if (!FinalOutFileName.Contains(TEXT("://")))
		{
			UE_LOG(LogFFmpeg, Warning, TEXT("UFFmpeg: IsRtmp is enabled but OutFileName '%s' does not look like a URL (missing '://')."), *FinalOutFileName);
		}
	}
	else if (SanitizeWindowsPath(FinalOutFileName))
	{
		UE_LOG(LogFFmpeg, Warning, TEXT("UFFmpeg: OutFileName '%s' contains characters that are not allowed in a file name; using '%s' instead."),
			*OutFileName, *FinalOutFileName);
	}

	const int32 Fps = FMath::Clamp(VideoFps, 1, 240);
	if (Fps != VideoFps)
	{
		UE_LOG(LogFFmpeg, Warning, TEXT("UFFmpeg: VideoFps %d is out of range, using %d instead."), VideoFps, Fps);
	}

	FD_world = World;
	FileAddr = FinalOutFileName;
	audio_delay = AudioDelay;

	const float ClampedVolume = FMath::Clamp(SoundVolume, 0.0f, 8.0f);
	if (!FMath::IsNearlyEqual(ClampedVolume, SoundVolume))
	{
		UE_LOG(LogFFmpeg, Warning, TEXT("UFFmpeg: SoundVolume %.2f is outside [0, 8] and was clamped to %.2f. Use 1.0 for the original volume, 0.5 for 50%%."),
			SoundVolume, ClampedVolume);
	}
	else if (ClampedVolume > 2.0f)
	{
		UE_LOG(LogFFmpeg, Warning, TEXT("UFFmpeg: SoundVolume %.2f amplifies the audio by that factor and will likely clip. Use 1.0 for the original volume."), ClampedVolume);
	}
	audio_volume = ClampedVolume;

	video_fps = Fps;
	Video_Tick_Time = 1.0f / static_cast<float>(Fps);

	if (VideoLength <= 0)
	{
		UE_LOG(LogFFmpeg, Warning, TEXT("UFFmpeg: VideoLength is %d, recording will stop immediately."), VideoLength);
		TotalFrame = 0;
	}
	else if (bVideoLengthInSeconds)
	{
		// Rounded so that e.g. 2 s at 30 fps becomes exactly 60 frames.
		TotalFrame = static_cast<uint32>(FMath::Max(1, FMath::RoundToInt(static_cast<float>(VideoLength) * static_cast<float>(Fps))));
	}
	else
	{
		TotalFrame = static_cast<uint32>(VideoLength);
	}

	const FVector2D ViewportSize(gameWindow->GetViewportSize());
	const uint32 ViewportWidth = static_cast<uint32>(FMath::Max(2, FMath::RoundToInt(static_cast<float>(ViewportSize.X))));
	const uint32 ViewportHeight = static_cast<uint32>(FMath::Max(2, FMath::RoundToInt(static_cast<float>(ViewportSize.Y))));

	// Capture at the viewport resolution (rounded down to even). Deliberately no 32 pixel padding:
	// the back buffer is exactly the viewport size, and reading `width` pixels per row from a buffer
	// whose rows are packed with the real texture width shifted every row (diagonal smearing).
	width = ViewportWidth & ~1u;
	height = ViewportHeight & ~1u;

	// Default output size: the current viewport resolution. Used when VideoFilter is empty.
	out_width = width;
	out_height = height;

	if (width < 2 || height < 2 || width > 8192 || height > 8192 || out_width < 2 || out_height < 2)
	{
		return Fail(FString::Printf(TEXT("Unsupported viewport size %ux%u."), ViewportWidth, ViewportHeight));
	}

	const FString TrimmedFilter = VideoFilter.TrimStartAndEnd();
	if (TrimmedFilter.IsEmpty())
	{
		UE_LOG(LogFFmpeg, Log, TEXT("UFFmpeg: VideoFilter is empty, using the current viewport resolution %ux%u for the output."), out_width, out_height);
	}
	else
	{
		FString Scale;
		FString Resolution;
		if (TrimmedFilter.Split(TEXT("="), &Scale, &Resolution))
		{
			FString StrWidth;
			FString StrHeight;
			if (Resolution.Split(TEXT(":"), &StrWidth, &StrHeight))
			{
				const int32 RequestedWidth = FCString::Atoi(*StrWidth);
				const int32 RequestedHeight = FCString::Atoi(*StrHeight);
				if (RequestedWidth >= 2 && RequestedHeight >= 2 && RequestedWidth <= 8192 && RequestedHeight <= 8192)
				{
					out_width = static_cast<uint32>(RequestedWidth) & ~1u;
					out_height = static_cast<uint32>(RequestedHeight) & ~1u;
				}
				else
				{
					UE_LOG(LogFFmpeg, Warning, TEXT("UFFmpeg: invalid VideoFilter '%s', keeping the viewport resolution %ux%u."), *VideoFilter, out_width, out_height);
				}
			}
			else
			{
				UE_LOG(LogFFmpeg, Warning, TEXT("UFFmpeg: VideoFilter '%s' is not in 'scale=WIDTH:HEIGHT' format, keeping the viewport resolution %ux%u."), *VideoFilter, out_width, out_height);
			}
		}
		else
		{
			UE_LOG(LogFFmpeg, Warning, TEXT("UFFmpeg: VideoFilter '%s' is not in 'scale=WIDTH:HEIGHT' format, keeping the viewport resolution %ux%u."), *VideoFilter, out_width, out_height);
		}
	}

	filter_descr.Reset();
	filter_descr.Append(TEXT("[in]scale="));
	filter_descr.Append(FString::FromInt(static_cast<int32>(out_width)));
	filter_descr.Append(TEXT(":"));
	filter_descr.Append(FString::FromInt(static_cast<int32>(out_height)));
	// Force the graph output format with the "format" filter: since FFmpeg 6/8 the buffersink
	// "pix_fmts" option is not a runtime option anymore and cannot be changed after filter init.
	filter_descr.Append(TEXT(",format=yuv420p[out]"));

	buff_bgr = static_cast<uint8_t*>(FMemory::Malloc(3 * width * height));
	if (!buff_bgr)
	{
		return Fail(TEXT("Failed to allocate the BGR conversion buffer."));
	}

	avformat_network_init();

	const char* FormatName = bRtmpOutput ? "flv" : nullptr;
	// FFmpeg expects UTF-8 paths; TCHAR_TO_ANSI would turn non-ASCII characters (e.g. Chinese
	// date components in the Blueprint file name) into '?' and make avio_open fail with EINVAL.
	int Ret = avformat_alloc_output_context2(&out_format_context, nullptr, FormatName, TCHAR_TO_UTF8(*FinalOutFileName));
	if (Ret < 0 || !out_format_context)
	{
		FreeEncodingResources();
		return Fail(FString::Printf(TEXT("avformat_alloc_output_context2('%s') failed: %s"), *FinalOutFileName, *AvErrorToString(Ret)));
	}

	if (!Create_Audio_Swr() || !Create_Audio_Encoder("aac") || !Create_Video_Encoder(UseGPU, TCHAR_TO_UTF8(*FinalOutFileName), VideoBitRate) || !Alloc_Video_Filter() || !CreateEncodeThread())
	{
		// LastError has been filled by the failing step.
		FreeEncodingResources();
		return false;
	}

	bInitialized = true;
	Begin_Receive_VideoData();
	Begin_Receive_AudioData(World);
	AddTickFunction();

	UE_LOG(LogFFmpeg, Log, TEXT("UFFmpeg: recording started, encoder=%s, %ux%u -> %ux%u @ %d fps, %d bps, %u frames (%.2f s, length given in %s), volume=%.2f, output=%s (%s)"),
		*VideoEncoderName, width, height, out_width, out_height, video_fps, FMath::Max(VideoBitRate, 100000),
		TotalFrame, static_cast<float>(TotalFrame) / static_cast<float>(video_fps),
		bVideoLengthInSeconds ? TEXT("seconds") : TEXT("frames"), audio_volume, *FileAddr,
		bRtmpOutput ? TEXT("RTMP/FLV") : TEXT("file"));
	return true;
}

void UFFmpegDirector::Begin_Receive_VideoData()
{
	if (!FSlateApplication::IsInitialized())
	{
		UE_LOG(LogFFmpeg, Warning, TEXT("UFFmpeg: Slate is not initialized, the back buffer cannot be captured."));
		return;
	}

	FSlateRenderer* Renderer = FSlateApplication::Get().GetRenderer();
	if (!Renderer)
	{
		UE_LOG(LogFFmpeg, Warning, TEXT("UFFmpeg: no Slate renderer, the back buffer cannot be captured."));
		return;
	}

	BackBufferReadyHandle = Renderer->OnBackBufferReadyToPresent().AddUObject(this, &UFFmpegDirector::OnBackBufferReady_RenderThread);
}

void UFFmpegDirector::OnBackBufferReady_RenderThread(SWindow& SlateWindow, ISlateViewportProvider& ViewportProvider)
{
	if (!bInitialized || IsClosing || !Runnable)
	{
		return;
	}
	if (gameWindow != &SlateWindow)
	{
		return;
	}

	if (ticktime >= Video_Tick_Time)
	{
		if (FrameCount >= TotalFrame)
		{
			IsClosing = true;
			StopCapture();
			return;
		}

		// UE 5.8 exposes the native back buffer through ISlateViewportProvider.
		FRHITexture* BackBuffer = ViewportProvider.GetBackBufferResource();
		if (!BackBuffer)
		{
			return;
		}

		GameTexture = BackBuffer;
		ticktime -= Video_Tick_Time;
		FrameCount++;
		GetScreenVideoData();
	}
}

void UFFmpegDirector::StopCapture()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickDelegateHandle);
	CheckDelegateHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UFFmpegDirector::CheckThreadJobDone));
}

void UFFmpegDirector::Stop(UWorld* _world)
{
	FTSTicker::GetCoreTicker().RemoveTicker(CheckDelegateHandle);

	if (RunnableThread)
	{
		RunnableThread->Kill(true);
		delete RunnableThread;
		RunnableThread = nullptr;
	}
	if (Runnable)
	{
		delete Runnable;
		Runnable = nullptr;
	}

	if (AudioDevice && SubmixListener.IsValid())
	{
		if (USoundSubmix* Submix = RegisteredSubmix.Get())
		{
			AudioDevice->UnregisterSubmixBufferListener(SubmixListener.ToSharedRef(), *Submix);
		}
		SubmixListener.Reset();
	}

	if (FSlateApplication::IsInitialized())
	{
		if (FSlateRenderer* Renderer = FSlateApplication::Get().GetRenderer())
		{
			Renderer->OnBackBufferReadyToPresent().Remove(BackBufferReadyHandle);
		}
	}

	Encode_Finish();

	OnRecordFinish.Broadcast(FileAddr);

	// Recording finished: drop the GC root taken in CreateFFmpegDirector(). Removing it twice
	// (Stop + DestoryDirector) is harmless.
	RemoveFromRoot();
}

bool UFFmpegDirector::AddTickTime(float time)
{
	ticktime += time;

	// PIE/world teardown without an explicit Stop(): release the encoder thread, the FFmpeg
	// resources and the GC root so nothing leaks. The audio device and Slate are intentionally
	// not touched here because they are already being torn down.
	if (bInitialized && FD_world && FD_world->bIsTearingDown)
	{
		UE_LOG(LogFFmpeg, Log, TEXT("UFFmpeg: world is tearing down, stopping the recording."));

		if (RunnableThread)
		{
			RunnableThread->Kill(true);
			delete RunnableThread;
			RunnableThread = nullptr;
		}
		if (Runnable)
		{
			delete Runnable;
			Runnable = nullptr;
		}
		SubmixListener.Reset();

		Encode_Finish();
		RemoveFromRoot();
		return false;
	}

	return true;
}

bool UFFmpegDirector::CheckThreadJobDone(float time)
{
	if (!Runnable)
	{
		return false;
	}

	if (Runnable->IsQueneEmpty() && finishAudiocounter >= TotalFrame && finishVideocounter >= TotalFrame)
	{
		Stop(FD_world);
		return false;
	}
	return true;
}

void UFFmpegDirector::AddTickFunction()
{
	TickDelegateHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UFFmpegDirector::AddTickTime));
}

void UFFmpegDirector::GetScreenVideoData()
{
	if (!GameTexture.IsValid())
	{
		return;
	}

	// The capture buffer is always width * height with a width*4 stride. Skip frames whose back
	// buffer is smaller (the window was resized after recording started).
	const uint32 TextureWidth = GameTexture->GetSizeX();
	const uint32 TextureHeight = GameTexture->GetSizeY();
	if (TextureWidth < width || TextureHeight < height)
	{
		static bool bWarnedSmaller = false;
		if (!bWarnedSmaller)
		{
			bWarnedSmaller = true;
			UE_LOG(LogFFmpeg, Warning, TEXT("UFFmpeg: back buffer %ux%u is smaller than the capture size %ux%u, frames are skipped."),
				TextureWidth, TextureHeight, width, height);
		}
		return;
	}

	FRHICommandListImmediate& list = GRHICommandList.GetImmediateCommandList();
	const FString RHIName = GDynamicRHI ? GDynamicRHI->GetName() : FString();

	// DirectX 11
	// FRHICommandListImmediate::LockTexture2D() will cause crash in DirectX 12, use DirectX 11.
	if (RHIName == TEXT("D3D11"))
	{
		uint8* TextureData = static_cast<uint8*>(list.LockTexture2D(GameTexture.GetReference(), 0, EResourceLockMode::RLM_ReadOnly, LolStride, false));
		if (Runnable && TextureData)
		{
			// LolStride is the real row pitch reported by the lock, the encoder uses it to advance rows.
			Runnable->InsertVideo(TextureData);
		}
		list.UnlockTexture2D(GameTexture.GetReference(), 0, false);
		return;
	}

	// DirectX 12 & Vulkan: ReadSurfaceData returns tightly packed rows (stride == texture width),
	// so repack into our own width*height buffer instead of assuming a different row pitch.
	if (RHIName == TEXT("D3D12") || RHIName == TEXT("Vulkan"))
	{
		FIntRect Rect(0, 0, static_cast<int32>(TextureWidth), static_cast<int32>(TextureHeight));
		TArray<FColor> Data;
		list.ReadSurfaceData(GameTexture.GetReference(), Rect, Data, FReadSurfaceDataFlags());

		if (Data.Num() < static_cast<int32>(TextureWidth * TextureHeight))
		{
			return;
		}

		const uint32 BufferSize = width * height * sizeof(uint32);
		uint32* TextureData = static_cast<uint32*>(FMemory::Malloc(BufferSize));
		if (!TextureData)
		{
			return;
		}
		FMemory::Memzero(TextureData, BufferSize);

		LolStride = width * sizeof(uint32);

		// width/height are the even-rounded viewport size and can be at most one pixel smaller than
		// the texture, so copy the overlapping area and leave the remaining border black.
		//
		// FColor is stored as B,G,R,A in memory (same layout as the D3D back buffer), which is what
		// Encode_Video_Frame unpacks. Do not swizzle here: shifting R/G/B into custom bit ranges
		// previously overlapped the alpha term and saturated the red channel (pink/red recording).
		const uint32 CopyWidth = FMath::Min(width, TextureWidth);
		const uint32 CopyHeight = FMath::Min(height, TextureHeight);
		for (uint32 Row = 0; Row < CopyHeight; ++Row)
		{
			FMemory::Memcpy(TextureData + Row * width, Data.GetData() + Row * TextureWidth, CopyWidth * sizeof(uint32));
		}

		if (Runnable)
		{
			Runnable->InsertVideo(reinterpret_cast<uint8*>(TextureData));
		}
		// InsertVideo copies the pixels into the encoder queue.
		FMemory::Free(TextureData);
		return;
	}

	static bool bWarnedUnsupportedRhi = false;
	if (!bWarnedUnsupportedRhi)
	{
		bWarnedUnsupportedRhi = true;
		UE_LOG(LogFFmpeg, Warning, TEXT("UFFmpeg: screen capture is not implemented for RHI '%s'."), *RHIName);
	}
}

bool UFFmpegDirector::CreateEncodeThread()
{
	Runnable = new FEncoderThread();
	if (!Runnable)
	{
		return Fail(TEXT("Failed to allocate the encoder thread."));
	}

	Runnable->CreateQueue(4 * width * height, 2048 * sizeof(float), 30, 40);
	Runnable->GetAudioProcessDelegate().BindUObject(this, &UFFmpegDirector::Encode_Audio_Frame);
	Runnable->video_encode_delegate.BindUObject(this, &UFFmpegDirector::Encode_Video_Frame);
	Runnable->GetAudioTimeProcessDelegate().BindUObject(this, &UFFmpegDirector::Encode_SetCurrentAudioTime);

	RunnableThread = FRunnableThread::Create(Runnable, TEXT("EncoderThread"));
	if (!RunnableThread)
	{
		delete Runnable;
		Runnable = nullptr;
		return Fail(TEXT("Failed to create the encoder thread."));
	}
	return true;
}

bool UFFmpegDirector::Create_Audio_Swr()
{
	int Ret = 0;
#if UFFMPEG_HAVE_SWR_OPTS2
	AVChannelLayout InLayout;
	AVChannelLayout OutLayout;
	av_channel_layout_default(&InLayout, 2);
	av_channel_layout_default(&OutLayout, 2);

	Ret = swr_alloc_set_opts2(&swr, &OutLayout, AV_SAMPLE_FMT_FLTP, 48000, &InLayout, AV_SAMPLE_FMT_FLT, 48000, 0, nullptr);
	if (Ret < 0 || !swr)
	{
		return Fail(FString::Printf(TEXT("swr_alloc_set_opts2 failed: %s"), *AvErrorToString(Ret)));
	}
#else
	// FFmpeg 4.x / 5.0: channel layouts are plain uint64 masks and there is no *_opts2 helper.
	swr = swr_alloc_set_opts(nullptr, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_FLTP, 48000,
		AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_FLT, 48000, 0, nullptr);
	if (!swr)
	{
		return Fail(TEXT("swr_alloc_set_opts failed (out of memory)."));
	}
#endif

	Ret = swr_init(swr);
	if (Ret < 0)
	{
		return Fail(FString::Printf(TEXT("swr_init failed: %s"), *AvErrorToString(Ret)));
	}
	return true;
}

bool UFFmpegDirector::Create_Audio_Encoder(const char* audioencoder_name)
{
	const AVCodec* AudioCodec = avcodec_find_encoder_by_name(audioencoder_name);
	if (!AudioCodec)
	{
		AudioCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
	}
	if (!AudioCodec)
	{
		return Fail(TEXT("No AAC encoder available in the linked FFmpeg build."));
	}

	out_audio_stream = avformat_new_stream(out_format_context, nullptr);
	if (!out_audio_stream)
	{
		return Fail(TEXT("avformat_new_stream failed for the audio stream."));
	}
	audio_index = out_audio_stream->index;

	audio_encoder_codec_context = avcodec_alloc_context3(AudioCodec);
	if (!audio_encoder_codec_context)
	{
		return Fail(TEXT("avcodec_alloc_context3 failed for the audio encoder."));
	}

	AVCodecContext* Ctx = audio_encoder_codec_context;
	Ctx->bit_rate = 192000;
	Ctx->sample_rate = 48000;
	Ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
#if UFFMPEG_HAVE_CHLAYOUT
	av_channel_layout_default(&Ctx->ch_layout, 2);
#else
	Ctx->channels = 2;
	Ctx->channel_layout = AV_CH_LAYOUT_STEREO;
#endif

	if (out_format_context->oformat->flags & AVFMT_GLOBALHEADER)
	{
		Ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	Ctx->codec_tag = 0;
	out_audio_stream->codecpar->codec_tag = 0;

	int Ret = avcodec_open2(Ctx, AudioCodec, nullptr);
	if (Ret < 0)
	{
		return Fail(FString::Printf(TEXT("Failed to open the AAC encoder: %s"), *AvErrorToString(Ret)));
	}

	Ret = avcodec_parameters_from_context(out_audio_stream->codecpar, Ctx);
	if (Ret < 0)
	{
		return Fail(FString::Printf(TEXT("avcodec_parameters_from_context failed for audio: %s"), *AvErrorToString(Ret)));
	}

	audio_frame = av_frame_alloc();
	audio_pkt = av_packet_alloc();
	if (!audio_frame || !audio_pkt)
	{
		return Fail(TEXT("Failed to allocate the audio frame/packet."));
	}

	audio_frame->format = Ctx->sample_fmt;
	audio_frame->sample_rate = Ctx->sample_rate;
#if UFFMPEG_HAVE_CHLAYOUT
	av_channel_layout_copy(&audio_frame->ch_layout, &Ctx->ch_layout);
#else
	audio_frame->channels = Ctx->channels;
	audio_frame->channel_layout = Ctx->channel_layout;
#endif
	AudioFrameCapacity = Ctx->frame_size > 0 ? Ctx->frame_size : 1024;
	audio_frame->nb_samples = AudioFrameCapacity;

	Ret = av_frame_get_buffer(audio_frame, 0);
	if (Ret < 0)
	{
		return Fail(FString::Printf(TEXT("av_frame_get_buffer failed for audio: %s"), *AvErrorToString(Ret)));
	}
	return true;
}

bool UFFmpegDirector::OpenVideoEncoderCandidate(const AVCodec* Codec, int32 BitRate)
{
	if (!Codec || !out_format_context)
	{
		return false;
	}

	AVCodecContext* Ctx = avcodec_alloc_context3(Codec);
	if (!Ctx)
	{
		return false;
	}

	Ctx->bit_rate = FMath::Max(BitRate, 100000);
	Ctx->width = static_cast<int>(out_width);
	Ctx->height = static_cast<int>(out_height);
	Ctx->time_base = AVRational{ 1, video_fps };
	Ctx->framerate = AVRational{ video_fps, 1 };
	Ctx->pix_fmt = AV_PIX_FMT_YUV420P;
	Ctx->gop_size = FMath::Max(1, video_fps * 2);
	Ctx->max_b_frames = 0;
#if UFFMPEG_HAVE_AV_PROFILE
	Ctx->profile = AV_PROFILE_H264_BASELINE; // widest RTMP/player compatibility
#else
	Ctx->profile = FF_PROFILE_H264_BASELINE;
#endif

	if (out_format_context->oformat->flags & AVFMT_GLOBALHEADER)
	{
		Ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	// Optional tuning knob; encoders that do not expose "preset" simply ignore the error.
	av_opt_set(Ctx->priv_data, "preset", "fast", 0);

	int OpenResult = 0;
	RunOnMtaComThread([&OpenResult, &Ctx, Codec]()
	{
		OpenResult = avcodec_open2(Ctx, Codec, nullptr);
		if (OpenResult < 0)
		{
			// Release inside the MTA thread: mf_close() calls CoUninitialize().
			avcodec_free_context(&Ctx);
		}
	});

	if (OpenResult < 0)
	{
		UE_LOG(LogFFmpeg, Warning, TEXT("UFFmpeg: encoder '%s' cannot be opened: %s (%ux%u @ %d fps, %d bps)"),
			ANSI_TO_TCHAR(Codec->name), *AvErrorToString(OpenResult),
			out_width, out_height, video_fps, FMath::Max(BitRate, 100000));
		return false;
	}

	video_encoder_codec_context = Ctx;
	VideoEncoderName = ANSI_TO_TCHAR(Codec->name);
	UE_LOG(LogFFmpeg, Log, TEXT("UFFmpeg: using H.264 encoder '%s'."), *VideoEncoderName);
	return true;
}

bool UFFmpegDirector::Create_Video_Encoder(bool bPreferGPU, const char* out_file_name, int bit_rate)
{
	static const char* GpuCandidates[] = { "h264_nvenc", "h264_qsv", "h264_amf", "h264_mf" };
	static const char* CpuCandidates[] = { "libx264", "h264_openh264", "libopenh264", "h264_mf" };

	TArray<const char*> Candidates;
	auto AddCandidate = [&Candidates](const char* Name)
	{
		if (!Name)
		{
			return;
		}

		// Compare by string: both lists contain "h264_mf" as separate literals, so a pointer
		// comparison would add it twice and log the same failure twice.
		const bool bAlreadyAdded = Candidates.ContainsByPredicate([Name](const char* Existing)
		{
			return Existing && FCStringAnsi::Strcmp(Existing, Name) == 0;
		});

		if (!bAlreadyAdded)
		{
			Candidates.Add(Name);
		}
	};

	if (bPreferGPU)
	{
		for (const char* Name : GpuCandidates)
		{
			AddCandidate(Name);
		}
	}
	for (const char* Name : CpuCandidates)
	{
		AddCandidate(Name);
	}
	if (!bPreferGPU)
	{
		for (const char* Name : GpuCandidates)
		{
			AddCandidate(Name);
		}
	}

	for (const char* Name : Candidates)
	{
		const AVCodec* Codec = avcodec_find_encoder_by_name(Name);
		if (Codec && OpenVideoEncoderCandidate(Codec, bit_rate))
		{
			break;
		}
	}

	if (!video_encoder_codec_context)
	{
		// Last resort: whatever H.264 encoder the build registers.
		const AVCodec* AnyCodec = avcodec_find_encoder(AV_CODEC_ID_H264);
		if (AnyCodec)
		{
			OpenVideoEncoderCandidate(AnyCodec, bit_rate);
		}
	}

	if (!video_encoder_codec_context)
	{
		FString Tried;
		for (const char* Name : Candidates)
		{
			if (Tried.Len() > 0)
			{
				Tried.Append(TEXT(", "));
			}
			Tried.Append(ANSI_TO_TCHAR(Name));
		}
		return Fail(FString::Printf(TEXT("No usable H.264 encoder in the linked FFmpeg build (tried: %s). Rebuild FFmpeg with --enable-libx264 (software) and/or --enable-nvenc."), *Tried));
	}

	out_video_stream = avformat_new_stream(out_format_context, nullptr);
	if (!out_video_stream)
	{
		return Fail(TEXT("avformat_new_stream failed for the video stream."));
	}
	video_index = out_video_stream->index;

	int Ret = avcodec_parameters_from_context(out_video_stream->codecpar, video_encoder_codec_context);
	if (Ret < 0)
	{
		return Fail(FString::Printf(TEXT("avcodec_parameters_from_context failed for video: %s"), *AvErrorToString(Ret)));
	}

	video_frame = av_frame_alloc();
	video_pkt = av_packet_alloc();
	filt_frame = av_frame_alloc();
	if (!video_frame || !video_pkt || !filt_frame)
	{
		return Fail(TEXT("Failed to allocate the video frame/packet/filter frame."));
	}

	video_frame->format = AV_PIX_FMT_YUV420P;
	video_frame->width = static_cast<int>(out_width);
	video_frame->height = static_cast<int>(out_height);
	Ret = av_frame_get_buffer(video_frame, 32);
	if (Ret < 0)
	{
		return Fail(FString::Printf(TEXT("av_frame_get_buffer failed for video: %s"), *AvErrorToString(Ret)));
	}

	sws_context = sws_getCachedContext(sws_context,
		static_cast<int>(width), static_cast<int>(height), AV_PIX_FMT_BGR24,
		static_cast<int>(out_width), static_cast<int>(out_height), AV_PIX_FMT_YUV420P,
		SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
	if (!sws_context)
	{
		return Fail(TEXT("sws_getCachedContext failed."));
	}

	Ret = avio_open(&out_format_context->pb, out_file_name, AVIO_FLAG_WRITE);
	if (Ret < 0)
	{
		const TCHAR* Hint = bRtmpOutput
			? TEXT(" Check that the FFmpeg build has networking enabled and the RTMP URL is valid.")
			: TEXT(" Check that the target directory exists and the path is writable.");
		return Fail(FString::Printf(TEXT("avio_open('%s') failed: %s.%s"),
			UTF8_TO_TCHAR(out_file_name), *AvErrorToString(Ret), Hint));
	}

	Ret = avformat_write_header(out_format_context, nullptr);
	if (Ret < 0)
	{
		return Fail(FString::Printf(TEXT("avformat_write_header failed: %s"), *AvErrorToString(Ret)));
	}
	bHeaderWritten = true;
	return true;
}

bool UFFmpegDirector::Video_Frame_YUV_From_BGR(uint8_t* rgb)
{
	if (!sws_context || !video_frame)
	{
		return false;
	}

	if (av_frame_make_writable(video_frame) < 0)
	{
		return false;
	}

	const int InLinesize[1] = { static_cast<int>(3 * width) };
	sws_scale(sws_context, (const uint8_t* const*)&rgb, InLinesize, 0, static_cast<int>(height), video_frame->data, video_frame->linesize);

	video_frame->width = static_cast<int>(out_width);
	video_frame->height = static_cast<int>(out_height);
	video_frame->format = AV_PIX_FMT_YUV420P;
	return true;
}

bool UFFmpegDirector::Alloc_Video_Filter()
{
	if (!video_encoder_codec_context)
	{
		return Fail(TEXT("Alloc_Video_Filter called before the video encoder was created."));
	}

	outputs = avfilter_inout_alloc();
	inputs = avfilter_inout_alloc();
	filter_graph = avfilter_graph_alloc();
	if (!outputs || !inputs || !filter_graph)
	{
		return Fail(TEXT("Failed to allocate the FFmpeg filter graph."));
	}

	const AVFilter* buffersrc = avfilter_get_by_name("buffer");
	const AVFilter* buffersink = avfilter_get_by_name("buffersink");
	if (!buffersrc || !buffersink)
	{
		return Fail(TEXT("The FFmpeg 'buffer'/'buffersink' filters are missing from this build."));
	}

	const AVRational TimeBase = video_encoder_codec_context->time_base;

	AVRational Sar = video_encoder_codec_context->sample_aspect_ratio;
	if (Sar.num <= 0 || Sar.den <= 0)
	{
		Sar = AVRational{ 1, 1 };
	}

	char Args[256] = { 0 };
	snprintf(Args, sizeof(Args),
		"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
		static_cast<int>(out_width), static_cast<int>(out_height), static_cast<int>(AV_PIX_FMT_YUV420P),
		TimeBase.num, TimeBase.den, Sar.num, Sar.den);

	int Ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in", Args, nullptr, filter_graph);
	if (Ret < 0)
	{
		return Fail(FString::Printf(TEXT("avfilter_graph_create_filter(buffer) failed: %s"), *AvErrorToString(Ret)));
	}

	Ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", nullptr, nullptr, filter_graph);
	if (Ret < 0)
	{
		return Fail(FString::Printf(TEXT("avfilter_graph_create_filter(buffersink) failed: %s"), *AvErrorToString(Ret)));
	}

	// The output pixel format is constrained by "format=yuv420p" inside filter_descr: since
	// FFmpeg 6 the buffersink "pix_fmts" option is not a runtime option and cannot be set after
	// the filter has been initialized.

	outputs->name = av_strdup("in");
	outputs->filter_ctx = buffersrc_ctx;
	outputs->pad_idx = 0;
	outputs->next = nullptr;

	inputs->name = av_strdup("out");
	inputs->filter_ctx = buffersink_ctx;
	inputs->pad_idx = 0;
	inputs->next = nullptr;

	if (!outputs->name || !inputs->name)
	{
		return Fail(TEXT("av_strdup failed while building the filter graph."));
	}

	Ret = avfilter_graph_parse_ptr(filter_graph, TCHAR_TO_ANSI(*filter_descr), &inputs, &outputs, nullptr);
	if (Ret < 0)
	{
		return Fail(FString::Printf(TEXT("avfilter_graph_parse_ptr('%s') failed: %s"), *filter_descr, *AvErrorToString(Ret)));
	}

	Ret = avfilter_graph_config(filter_graph, nullptr);
	if (Ret < 0)
	{
		return Fail(FString::Printf(TEXT("avfilter_graph_config failed: %s"), *AvErrorToString(Ret)));
	}
	return true;
}

void UFFmpegDirector::FreeEncodingResources()
{
	if (out_format_context)
	{
		if (bHeaderWritten)
		{
			av_write_trailer(out_format_context);
			bHeaderWritten = false;
		}
		if (out_format_context->pb)
		{
			avio_close(out_format_context->pb);
			out_format_context->pb = nullptr;
		}
		avformat_free_context(out_format_context);
		out_format_context = nullptr;
	}

	if (video_encoder_codec_context)
	{
		// MediaFoundation encoders call CoUninitialize() when closed, so release them on an MTA thread.
		RunOnMtaComThread([this]()
		{
			avcodec_free_context(&video_encoder_codec_context);
		});
	}
	avcodec_free_context(&audio_encoder_codec_context);

	if (swr)
	{
		swr_free(&swr);
	}
	if (sws_context)
	{
		sws_freeContext(sws_context);
		sws_context = nullptr;
	}

	avfilter_graph_free(&filter_graph);
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);

	av_frame_free(&video_frame);
	av_frame_free(&audio_frame);
	av_frame_free(&filt_frame);
	av_packet_free(&video_pkt);
	av_packet_free(&audio_pkt);

	out_video_stream = nullptr;
	out_audio_stream = nullptr;
	video_index = -1;
	audio_index = -1;
	AudioFrameCapacity = 0;

	if (buff_bgr)
	{
		FMemory::Free(buff_bgr);
		buff_bgr = nullptr;
	}

	bInitialized = false;
}

void UFFmpegDirector::Encode_Finish()
{
	FreeEncodingResources();
}

void UFFmpegDirector::OnNewSubmixBuffer(const USoundSubmix* OwningSubmix, float* AudioData, int32 NumSamples, int32 NumChannels, const int32 SampleRate, double AudioClock)
{
	if (!bInitialized || !Runnable || !AudioData)
	{
		return;
	}
	Runnable->InsertAudio(reinterpret_cast<uint8_t*>(AudioData), reinterpret_cast<uint8_t*>(&AudioClock));
}

void UFFmpegDirector::Encode_Audio_Frame(uint8_t* rgb)
{
	if (!audio_encoder_codec_context || !audio_frame || !swr || !audio_pkt || !out_audio_stream || !rgb)
	{
		return;
	}

	const uint8_t* InData = rgb;
	const int OutSamples = swr_convert(swr, audio_frame->data, AudioFrameCapacity, &InData, 1024);
	if (OutSamples < 0)
	{
		return;
	}

	audio_frame->nb_samples = OutSamples;
	audio_frame->pts = static_cast<int64_t>((CurrentAuidoTime + audio_delay) * 48000.0 + 0.5);

	int Ret = avcodec_send_frame(audio_encoder_codec_context, audio_frame);
	if (Ret < 0 && Ret != AVERROR(EAGAIN))
	{
		UE_LOG(LogFFmpeg, Warning, TEXT("UFFmpeg: avcodec_send_frame(audio) failed: %s"), *AvErrorToString(Ret));
		finishAudiocounter++;
		return;
	}

	while (true)
	{
		Ret = avcodec_receive_packet(audio_encoder_codec_context, audio_pkt);
		if (Ret == AVERROR(EAGAIN) || Ret == AVERROR_EOF)
		{
			break;
		}
		if (Ret < 0)
		{
			UE_LOG(LogFFmpeg, Warning, TEXT("UFFmpeg: avcodec_receive_packet(audio) failed: %s"), *AvErrorToString(Ret));
			break;
		}

		audio_pkt->pts = audio_pkt->dts = audio_frame->pts;
		av_packet_rescale_ts(audio_pkt, audio_encoder_codec_context->time_base, out_audio_stream->time_base);
		audio_pkt->stream_index = audio_index;

		const int WriteRet = av_interleaved_write_frame(out_format_context, audio_pkt);
		av_packet_unref(audio_pkt);
		if (WriteRet < 0)
		{
			UE_LOG(LogFFmpeg, Warning, TEXT("UFFmpeg: writing an audio packet failed: %s"), *AvErrorToString(WriteRet));
			break;
		}
	}

	finishAudiocounter++;
}

void UFFmpegDirector::Encode_Video_Frame(uint8_t* rgb)
{
	if (!video_encoder_codec_context || !video_frame || !buffersrc_ctx || !buffersink_ctx || !video_pkt || !filt_frame || !rgb)
	{
		return;
	}

	uint8* TextureDataPtr = rgb;
	uint8_t* FirstPtr = buff_bgr;
	for (uint32 Row = 0; Row < height; ++Row)
	{
		uint32* PixelPtr = reinterpret_cast<uint32*>(TextureDataPtr);
		for (uint32 Col = 0; Col < width; ++Col)
		{
			const uint32 Pixel = *PixelPtr;
			// Source pixels are 32 bit BGRA (FColor memory order / D3D back buffer format).
			//	AV_PIX_FMT_BGR24: convert to BGR for now
			//	AV_PIX_FMT_RGB24: severe frame drops, cause unknown for now
			*(buff_bgr + 2) = static_cast<uint8>((Pixel >> 16) & 0xFF); // R
			*(buff_bgr + 1) = static_cast<uint8>((Pixel >> 8) & 0xFF);  // G
			*(buff_bgr) = static_cast<uint8>(Pixel & 0xFF);             // B
			buff_bgr += 3;
			++PixelPtr;
		}
		TextureDataPtr += LolStride;
	}
	buff_bgr = FirstPtr;

	if (!Video_Frame_YUV_From_BGR(buff_bgr))
	{
		return;
	}

	video_frame->pts = video_pts++;

	int Ret = av_buffersrc_add_frame_flags(buffersrc_ctx, video_frame, AV_BUFFERSRC_FLAG_KEEP_REF);
	if (Ret < 0)
	{
		UE_LOG(LogFFmpeg, Warning, TEXT("UFFmpeg: av_buffersrc_add_frame_flags failed: %s"), *AvErrorToString(Ret));
		return;
	}

	while (true)
	{
		Ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);
		if (Ret < 0)
		{
			break;
		}

		Ret = avcodec_send_frame(video_encoder_codec_context, filt_frame);
		if (Ret < 0)
		{
			UE_LOG(LogFFmpeg, Warning, TEXT("UFFmpeg: avcodec_send_frame(video) failed: %s"), *AvErrorToString(Ret));
			av_frame_unref(filt_frame);
			break;
		}

		while (true)
		{
			const int PacketRet = avcodec_receive_packet(video_encoder_codec_context, video_pkt);
			if (PacketRet == AVERROR(EAGAIN) || PacketRet == AVERROR_EOF)
			{
				break;
			}
			if (PacketRet < 0)
			{
				UE_LOG(LogFFmpeg, Warning, TEXT("UFFmpeg: avcodec_receive_packet(video) failed: %s"), *AvErrorToString(PacketRet));
				break;
			}

			video_pkt->pts = video_pkt->dts = filt_frame->pts;
			av_packet_rescale_ts(video_pkt, video_encoder_codec_context->time_base, out_video_stream->time_base);
			video_pkt->stream_index = video_index;

			const int WriteRet = av_interleaved_write_frame(out_format_context, video_pkt);
			av_packet_unref(video_pkt);
			if (WriteRet < 0)
			{
				UE_LOG(LogFFmpeg, Warning, TEXT("UFFmpeg: writing a video packet failed: %s"), *AvErrorToString(WriteRet));
				break;
			}
		}

		av_frame_unref(filt_frame);
	}

	finishVideocounter++;
}

void UFFmpegDirector::Encode_SetCurrentAudioTime(uint8_t* rgb)
{
	if (!rgb)
	{
		return;
	}
	CurrentAuidoTime = *reinterpret_cast<double*>(rgb);
}

void UFFmpegDirector::Set_Audio_Volume(AVFrame* frame)
{
#if UFFMPEG_HAVE_CHLAYOUT
	if (!frame || !frame->data[0] || !frame->data[1] || frame->ch_layout.nb_channels < 2)
#else
	if (!frame || !frame->data[0] || !frame->data[1] || frame->channels < 2)
#endif
	{
		return;
	}

	float* ChLeft = reinterpret_cast<float*>(frame->data[0]);
	float* ChRight = reinterpret_cast<float*>(frame->data[1]);
	for (int32 i = 0; i < frame->nb_samples; i++)
	{
		ChLeft[i] *= audio_volume;
		ChRight[i] *= audio_volume;
	}
}

void FFFmpegSubmixListener::OnNewSubmixBuffer(const USoundSubmix* OwningSubmix, float* AudioData, int32 NumSamples, int32 NumChannels, const int32 SampleRate, double AudioClock)
{
	if (Owner)
	{
		Owner->OnNewSubmixBuffer(OwningSubmix, AudioData, NumSamples, NumChannels, SampleRate, AudioClock);
	}
}

bool FFFmpegSubmixListener::IsRenderingAudio() const
{
	return Owner != nullptr;
}

const FString& FFFmpegSubmixListener::GetListenerName() const
{
	static const FString ListenerName(TEXT("UFFmpegDirector"));
	return ListenerName;
}
