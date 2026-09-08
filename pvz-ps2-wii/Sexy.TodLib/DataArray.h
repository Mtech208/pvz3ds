#ifndef __DATAARRAY_H__
#define __DATAARRAY_H__

#include <string.h>
#include "TodDebug.h"
#include "TodCommon.h"

enum 
{
	DATA_ARRAY_INDEX_MASK = 65535,
	DATA_ARRAY_KEY_MASK = -65536,
	DATA_ARRAY_KEY_SHIFT = 16,
	DATA_ARRAY_MAX_SIZE = 65536,
	DATA_ARRAY_KEY_FIRST = 1
};

template <typename T> class DataArray
{
public:
	class DataArrayItem
	{
	public:
		T					mItem;
		unsigned int		mID;
	};
	
public:
	DataArrayItem*			mBlock;
	unsigned int			mMaxUsedCount;
	unsigned int			mMaxSize;
	unsigned int			mFreeListHead;
	unsigned int			mSize;
	unsigned int			mNextKey;
	const char*				mName;

public:
	DataArray()
	{
		mBlock = nullptr;
		mMaxUsedCount = 0U;
		mMaxSize = 0U;
		mFreeListHead = 0U;
		mSize = 0U;
		mNextKey = 1U;
		mName = nullptr;
	}

	~DataArray()
	{
		DataArrayDispose();
	}

	void DataArrayInitialize(unsigned int theMaxSize, const char* theName)
	{
		TOD_ASSERT(mBlock == nullptr);
#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
		unsigned int aBitWordCount = (theMaxSize + 31U) >> 5;
		mBlock = (DataArrayItem*)operator new(sizeof(DataArrayItem) * theMaxSize + sizeof(unsigned int) * aBitWordCount);
#else
		mBlock = (DataArrayItem*)operator new(sizeof(DataArrayItem) * theMaxSize);
#endif
		mMaxSize = theMaxSize;
#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
		memset(DataArrayGetIterationBits(), 0, sizeof(unsigned int) * aBitWordCount);
#endif
		mNextKey = 1001U;
		mName = theName;
	}

	void DataArrayDispose()
	{
		if (mBlock != nullptr)
		{
			DataArrayFreeAll();
			operator delete(mBlock);
			mBlock = nullptr;
			mMaxUsedCount = 0U;
			mMaxSize = 0U;
			mFreeListHead = 0U;
			mSize = 0U;
			mName = nullptr;
		}
	}

	void DataArrayFree(T* theItem)
	{
		DataArrayItem* aItem = (DataArrayItem*)theItem;
		TOD_ASSERT(DataArrayGet(aItem->mID) == theItem, "Failed: DataArrayFree(0x%x) in %s", theItem, mName);
		theItem->~T();
		unsigned int anId = aItem->mID & DATA_ARRAY_INDEX_MASK;
#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
		DataArraySetIterationBit(anId, false);
#endif
		aItem->mID = mFreeListHead;
		mFreeListHead = anId;
		mSize--;
	}

	void DataArrayFreeAll()
	{
		T* aItem = nullptr;
		while (IterateNext(aItem))
			DataArrayFree(aItem);

		mFreeListHead = 0U;
		mMaxUsedCount = 0U;
#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
		memset(DataArrayGetIterationBits(), 0, sizeof(unsigned int) * DataArrayGetIterationBitWordCount());
#endif
	}

	inline unsigned int DataArrayGetID(T* theItem)
	{
		DataArrayItem* aItem = (DataArrayItem*)theItem;
		TOD_ASSERT(DataArrayGet(aItem->mID) == theItem, "Failed: DataArrayGetID(0x%x) for %s", theItem, mName);
		return aItem->mID;
	}

	bool IterateNext(T*& theItem)
	{
#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
		if (mMaxUsedCount == 0U)
			return false;

		unsigned int anIndex = 0U;
		if (theItem != nullptr)
			anIndex = (unsigned int)((DataArrayItem*)theItem - mBlock) + 1U;

		if (anIndex >= mMaxUsedCount)
			return false;

		unsigned int* anIterationBits = DataArrayGetIterationBits();
		unsigned int aWordIndex = anIndex >> 5;
		unsigned int aBitOffset = anIndex & 31U;
		unsigned int aWord = anIterationBits[aWordIndex] & (~0U << aBitOffset);

		while (true)
		{
			if (aWord != 0U)
			{
				unsigned int aFoundIndex;
				if (aWord & (1U << aBitOffset))
					aFoundIndex = anIndex;
				else
					aFoundIndex = (aWordIndex << 5) + DataArrayFindFirstSetBit(aWord);
				if (aFoundIndex < mMaxUsedCount)
				{
					theItem = (T*)&mBlock[aFoundIndex];
					return true;
				}
				return false;
			}

			aWordIndex++;
			if ((aWordIndex << 5) >= mMaxUsedCount)
				return false;
			anIndex = aWordIndex << 5;
			aBitOffset = 0U;
			aWord = anIterationBits[aWordIndex];
		}
#else
		DataArray<T>::DataArrayItem* aItem = (DataArray<T>::DataArrayItem*)theItem;
		if (aItem == nullptr)
			aItem = &mBlock[0];
		else
			aItem++;

		DataArray<T>::DataArrayItem* aLast = &mBlock[mMaxUsedCount];
		while ((intptr_t)aItem < (intptr_t)aLast)
		{
			if (aItem->mID & DATA_ARRAY_KEY_MASK)
			{
				theItem = (T*)aItem;
				return true;
			}
			aItem++;
		}
		return false;
#endif
	}

	T* DataArrayAlloc()
	{
		TOD_ASSERT(mSize < mMaxSize, "Data array full: %s", mName);
		TOD_ASSERT(mFreeListHead <= mMaxUsedCount, "DataArrayAlloc error in %s", mName);
		unsigned int aNext = mMaxUsedCount;
		if (mFreeListHead == mMaxUsedCount)
			mFreeListHead = ++mMaxUsedCount;
		else
		{
			aNext = mFreeListHead;
			mFreeListHead = mBlock[mFreeListHead].mID;
		}

		DataArray<T>::DataArrayItem* aNewItem = &mBlock[aNext];
		memset(aNewItem, 0, sizeof(DataArrayItem));
		aNewItem->mID = (mNextKey++ << DATA_ARRAY_KEY_SHIFT) | aNext;
		if (mNextKey == DATA_ARRAY_MAX_SIZE) mNextKey = 1;
		mSize++;
#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
		DataArraySetIterationBit(aNext, true);
#endif

		new (aNewItem)T();
		return (T*)aNewItem;
	}

#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
	unsigned int DataArrayGetIterationBitWordCount() const
	{
		return (mMaxSize + 31U) >> 5;
	}

	unsigned int* DataArrayGetIterationBits()
	{
		return (unsigned int*)(mBlock + mMaxSize);
	}

	void DataArraySetIterationBit(unsigned int theIndex, bool theUsed)
	{
		unsigned int* anIterationBits = DataArrayGetIterationBits();
		unsigned int aMask = 1U << (theIndex & 31U);
		unsigned int& aWord = anIterationBits[theIndex >> 5];
		if (theUsed)
			aWord |= aMask;
		else
			aWord &= ~aMask;
	}

	static unsigned int DataArrayFindFirstSetBit(unsigned int theValue)
	{
#if defined(__GNUC__) || defined(__clang__)
		return (unsigned int)__builtin_ctz(theValue);
#else
		unsigned int aBit = 0U;
		while ((theValue & 1U) == 0U)
		{
			theValue >>= 1;
			aBit++;
		}
		return aBit;
#endif
	}

	void DataArrayRebuildIterationBits()
	{
		unsigned int* anIterationBits = DataArrayGetIterationBits();
		memset(anIterationBits, 0, sizeof(unsigned int) * DataArrayGetIterationBitWordCount());
		for (unsigned int i = 0U; i < mMaxUsedCount; i++)
		{
			if (mBlock[i].mID & DATA_ARRAY_KEY_MASK)
				anIterationBits[i >> 5] |= 1U << (i & 31U);
		}
	}
#else
	void DataArrayRebuildIterationBits()
	{
	}
#endif

	T* DataArrayTryToGet(unsigned int theId)
	{
		if (!theId || (theId & DATA_ARRAY_INDEX_MASK) >= mMaxSize)
			return nullptr;

		DataArrayItem* aBlock = &mBlock[theId & DATA_ARRAY_INDEX_MASK];
		return (aBlock->mID == theId) ? &aBlock->mItem : nullptr;
	}

	T* DataArrayGet(unsigned int theId)
	{
		TOD_ASSERT(DataArrayTryToGet(theId) != nullptr, "Failed: DataArrayGet(0x%x) for %s", theId, mName);
		return &mBlock[(short)theId].mItem;
	}
};

#endif
