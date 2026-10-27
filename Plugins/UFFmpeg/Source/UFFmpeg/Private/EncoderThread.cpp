// Fill out your copyright notice in the Description page of Project Settings.


#include "EncoderThread.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <objbase.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif


FEncoderThread::FEncoderThread()
{
}

FEncoderThread::~FEncoderThread()
{
	// The queues are plain C++ objects owned by this thread object. They are not UObjects anymore:
	// manually calling ConditionalBeginDestroy() + BeginDestroy() on a UObject removed it from the
	// UObject hash twice and made UE 5.8 fatal with "hash itself may be corrupted".
	delete audio_queue;
	audio_queue = nullptr;

	delete videobuffer_queue;
	videobuffer_queue = nullptr;

	delete audio_time_queue;
	audio_time_queue = nullptr;
}

bool FEncoderThread::Init()
{
	counter = 0;

#if PLATFORM_WINDOWS
	// The MediaFoundation encoder (h264_mf) refuses to work in an STA apartment and the UE game
	// thread is STA, so every encode call runs on this MTA initialized thread instead.
	const HRESULT ComResult = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	bComInitialized = SUCCEEDED(ComResult);
	if (!bComInitialized)
	{
		UE_LOG(LogTemp, Warning, TEXT("UFFmpeg: encoder thread could not initialize COM as MTA (0x%08x); h264_mf may be unavailable."), static_cast<uint32>(ComResult));
	}
#endif

	return true;
}

uint32 FEncoderThread::Run()
{
	while (!IsDone)
	{
		RunEncode();
	}
	return 1;
}

void FEncoderThread::Stop()
{
	IsDone = true;
}

void FEncoderThread::Exit()
{
#if PLATFORM_WINDOWS
	if (bComInitialized)
	{
		::CoUninitialize();
		bComInitialized = false;
	}
#endif
}

void FEncoderThread::CreateQueue(int video_data_size, int auido_data_size, int video_data_num, int auido_data_num)
{
	videobuffer_queue = new UCircleQueue();
	videobuffer_queue->Init(video_data_num, video_data_size);
	videobuffer_queue->encode_delegate.BindRaw(this, &FEncoderThread::GetBufferData);

	audio_queue = new UCircleQueue();
	audio_queue->Init(auido_data_num, auido_data_size);


	audio_time_queue = new UCircleQueue();
	audio_time_queue->Init(auido_data_num, auido_data_num*sizeof(double));
}

EncodeDelegate& FEncoderThread::GetAudioProcessDelegate()
{
	return audio_queue->encode_delegate; 
}

EncodeDelegate& FEncoderThread::GetAudioTimeProcessDelegate()
{
	return audio_time_queue->encode_delegate;
}

bool FEncoderThread::InsertVideo(uint8* Src)
{
	if (!videobuffer_queue)
		return false;
	{
		FScopeLock ScopeLock(&VideoBufferMutex);		
		while (!videobuffer_queue->InsertEncodeData(Src))
		{
			//GEngine->AddOnScreenDebugMessage(-1, 0.02f, FColor::Red, FString("video  now  encode"));
			videobuffer_queue->PrcessEncodeData();
			EncodeVideo();
		}
	}		
	return true;
}

bool FEncoderThread::InsertAudio(uint8* Src, uint8* time)
{
	if (!audio_queue || !audio_time_queue)
		return false;
	{
		FScopeLock ScopeLock(&AudioMutex);

		while (!audio_queue->InsertEncodeData(Src) || !audio_time_queue->InsertEncodeData(time))
		{
			//GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Red, FString("audio  now  encode"));
			EncodeAudio();
		}
	}
	return true;
}

void FEncoderThread::GetBufferData(uint8* data)
{
	video_data = data;	
}


bool FEncoderThread::IsQueneEmpty()
{
	const bool bVideoEmpty = !videobuffer_queue || videobuffer_queue->IsEmpty();
	const bool bAudioEmpty = !audio_queue || audio_queue->IsEmpty();
	const bool bAudioTimeEmpty = !audio_time_queue || audio_time_queue->IsEmpty();
	return bVideoEmpty && bAudioEmpty && bAudioTimeEmpty;
}

void FEncoderThread::RunEncode()
{
	bool IsNeedEncode = false;

	{
		FScopeLock ScopeLock(&AudioMutex);
		EncodeAudio();
	}

	{
		FScopeLock ScopeLock1(&VideoBufferMutex);
		IsNeedEncode = videobuffer_queue ? videobuffer_queue->PrcessEncodeData() : false;

		if (IsNeedEncode)
			EncodeVideo();
	}

}

void FEncoderThread::EncodeVideo()
{
	if (video_data)
	{
		counter++;
		video_encode_delegate.ExecuteIfBound(video_data);
		video_data = nullptr;
	}
}

void FEncoderThread::EncodeAudio()
{
	if (audio_queue && audio_time_queue)
	{
		audio_time_queue->PrcessEncodeData();
		audio_queue->PrcessEncodeData();
	}
}
