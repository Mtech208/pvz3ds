#pragma once

#include <string>

namespace Sexy
{
class ResourceManager;

// Owns exactly one reference to a resource group.  This is intentionally
// movable but not copyable: copying a screen must never manufacture an
// unmatched DeleteResources call.
class ResourceGroupLease
{
public:
	ResourceGroupLease();
	ResourceGroupLease(ResourceManager* theManager, const std::string& theGroup);
	~ResourceGroupLease();

	ResourceGroupLease(ResourceGroupLease&& theOther);
	ResourceGroupLease& operator=(ResourceGroupLease&& theOther);

	bool Acquire(ResourceManager* theManager, const std::string& theGroup);
	void Release();
	bool IsAcquired() const { return mAcquired; }
	const std::string& GetGroup() const { return mGroup; }

private:
	ResourceGroupLease(const ResourceGroupLease&);
	ResourceGroupLease& operator=(const ResourceGroupLease&);

	ResourceManager* mManager;
	std::string mGroup;
	bool mAcquired;
};
}
