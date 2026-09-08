#pragma once

// Serializes platform file/console IO only on targets that need it. Generic
// engine code should depend on this seam instead of including a console lock.
#ifdef PS2_PLATFORM
#include "platform/ps2/Ps2IoLock.h"
#endif

inline void PlatformIoLockAcquire()
{
#ifdef PS2_PLATFORM
    Ps2IoLockAcquire();
#endif
}

inline void PlatformIoLockRelease()
{
#ifdef PS2_PLATFORM
    Ps2IoLockRelease();
#endif
}
