#include "ResourceGroupLease.h"

#include "ResourceManager.h"
#include "../../Sexy.TodLib/TodCommon.h"

using namespace Sexy;

ResourceGroupLease::ResourceGroupLease()
	: mManager(NULL), mAcquired(false)
{
}

ResourceGroupLease::ResourceGroupLease(ResourceManager* theManager, const std::string& theGroup)
	: mManager(NULL), mAcquired(false)
{
	Acquire(theManager, theGroup);
}

ResourceGroupLease::~ResourceGroupLease()
{
	Release();
}

ResourceGroupLease::ResourceGroupLease(ResourceGroupLease&& theOther)
	: mManager(theOther.mManager), mGroup(theOther.mGroup), mAcquired(theOther.mAcquired)
{
	theOther.mManager = NULL;
	theOther.mGroup.clear();
	theOther.mAcquired = false;
}

ResourceGroupLease& ResourceGroupLease::operator=(ResourceGroupLease&& theOther)
{
	if (this != &theOther)
	{
		Release();
		mManager = theOther.mManager;
		mGroup = theOther.mGroup;
		mAcquired = theOther.mAcquired;
		theOther.mManager = NULL;
		theOther.mGroup.clear();
		theOther.mAcquired = false;
	}
	return *this;
}

bool ResourceGroupLease::Acquire(ResourceManager* theManager, const std::string& theGroup)
{
	Release();
	if (theManager == NULL || theGroup.empty())
		return false;

	// TodLoadResources owns the matching refcount increment.  Do not set the
	// acquired flag until extraction has also succeeded.
	mManager = theManager;
	mGroup = theGroup;
	if (!((TodResourceManager*)mManager)->TodLoadResources(mGroup))
	{
		mManager = NULL;
		mGroup.clear();
		return false;
	}

	mAcquired = true;
	return true;
}

void ResourceGroupLease::Release()
{
	if (mAcquired && mManager != NULL)
		mManager->DeleteResources(mGroup);
	mManager = NULL;
	mGroup.clear();
	mAcquired = false;
}
