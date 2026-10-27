// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
/**
 * 
 */

DECLARE_DELEGATE_OneParam(EncodeDelegate,uint8*)


class UFFMPEG_API FEncodeData
{
public:

	FEncodeData();
	~FEncodeData();
	void InitializeData(int size);
	void SetEncodeData(uint8* Src);
	uint8* GetData();
private:
	uint8* DataMemory;
	int datasize;
};

/**
 * Plain (non-UObject) ring buffer used by the encoder thread.
 *
 * It used to be a UObject created with NewObject() and destroyed manually with
 * ConditionalBeginDestroy() + BeginDestroy(), which removed it from the UObject hash twice and
 * made UE 5.8 fatal with "hash itself may be corrupted". The queues are owned exclusively by
 * FEncoderThread, so they need neither GC nor rooting.
 */
class UFFMPEG_API UCircleQueue
{
public:
	UCircleQueue();
	~UCircleQueue();

	void Init(int queue_len,int data_size);
	bool InsertEncodeData(uint8* Src);
	bool PrcessEncodeData();
	bool IsFull();
	bool IsEmpty();
	EncodeDelegate encode_delegate;
private:
	int queue_num;
	// Number of free slots in the queue
	int queue_freenum;
	int queue_head;
	int queue_tail;
	FEncodeData* queue_ptr;
};



