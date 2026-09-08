#include "fcaseopen.h"

#ifdef PS2_PLATFORM
#include "Ps2IoLock.h"
#else
static void Ps2IoLockAcquire(void) {}
static void Ps2IoLockRelease(void) {}
#endif

#include <unistd.h> // fix "implicit declaration of function chdir"

// Whether the case-insensitive casepath() fallback is compiled and used at all.
// It exists for case-SENSITIVE host filesystems (Linux/macOS dev builds), where
// an asset referenced as "IMAGES/Foo.PNG" would otherwise not resolve.
//
// It is off wherever the underlying filesystem already matches case-insensitively,
// because there it is pure cost: casepath() walks every path component with
// opendir()/readdir(), so a MISS scans whole directories. The engine misses
// constantly by design — ResourceManager probes several extensions per image and
// the auto-alpha companion "_Name.*" that most images do not have — and images/
// holds thousands of entries, so each miss becomes thousands of directory reads.
//
//   _WIN32        NTFS/FAT lookups are case-insensitive.
//   PS2_PLATFORM  every opendir/readdir is an SIF RPC; flooding the queue
//                 eventually crashes it (_request_end). PCSX2's host: FS is
//                 case-insensitive anyway.
//   WII_PLATFORM  libfat compares FAT long filenames case-insensitively, so a
//                 direct fopen already resolves any file that exists. With the
//                 fallback on, every probe miss became an opendir + full readdir
//                 scan of images/ over the SD — which is what made compiling a
//                 single reanim take ~20 seconds.
//
// On all of these a single failing direct fopen per miss is cheap and correct.
#if !defined(_WIN32) && !defined(PS2_PLATFORM) && !defined(WII_PLATFORM)
	#define FCASEOPEN_USE_CASEPATH 1
#else
	#define FCASEOPEN_USE_CASEPATH 0
#endif

#ifdef PS2_PLATFORM
#include <string.h>
#include <stdio.h>
#include "Ps2PvzServices.h" // Ps2GetResourcePrefix()

// On a CD/DVD boot, relative asset paths must resolve against the disc, not the
// launcher's cwd (which is cdrom0: — read-only and path-mangling). The services
// layer sets the read root to "cdfs:/"; prepend it to relative READ opens only.
// Writes keep going straight through: every writable path the game builds is
// already absolute (GetAppDataFolder() -> mc0:/mass:), so it carries a ':' and
// is left untouched. On host:/USB the prefix is empty -> zero behaviour change.
static const char* ps2_resolve_read_path(const char* thePath, const char* theMode,
                                          char* theBuf, size_t theBufLen)
{
    const char* aPrefix = Ps2GetResourcePrefix();
    if (aPrefix[0] == '\0' || thePath == NULL || thePath[0] == '\0')
        return thePath;                       // no CD read root: unchanged
    if (theMode == NULL || theMode[0] != 'r')
        return thePath;                       // writes are not asset reads
    if (strchr(thePath, ':') != NULL)
        return thePath;                       // already device-qualified
    snprintf(theBuf, theBufLen, "%s%s", aPrefix, thePath);
    return theBuf;
}
#endif

// casepath()/strsep() implement the case-insensitive fallback. Skip compiling it
// entirely where it is unused, to avoid unused-function warnings and needless
// dirent usage. See FCASEOPEN_USE_CASEPATH above for why a platform opts out.
#if FCASEOPEN_USE_CASEPATH
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#include <errno.h>


#ifdef __HAIKU__
// this function seems to not exist under haiku??
char *strsep(char **stringp, const char *delim)
{
	char *begin, *end;
	begin = *stringp;
	if (begin == NULL) return NULL;

	if (delim[0] == '\0' || begin[0] == '\0')
	{
		*stringp = NULL;
		return begin;
	}

	end = strpbrk(begin, delim);
	if (end)
	{
		*end = '\0';
		*stringp = end + 1;
	}
	else
	{
		*stringp = NULL;
	}
	return begin;
}
#endif


// r must have strlen(path) + 3 bytes
static int casepath(char const *path, char *r)
{
    size_t l = strlen(path);
    char *p = alloca(l + 1);
    strcpy(p, path);
    size_t rl = 0;
    
    DIR *d;
    if (p[0] == '/')
    {
        d = opendir("/");
        p = p + 1;
    }
    else
    {
        d = opendir(".");
        r[0] = '.';
        r[1] = 0;
        rl = 1;
    }
    
    int last = 0;
    char *c = strsep(&p, "/");
    while (c)
    {
        if (!d)
        {
            return 0;
        }
        
        if (last)
        {
            closedir(d);
            return 0;
        }
        
        r[rl] = '/';
        rl += 1;
        r[rl] = 0;
        
        struct dirent *e = readdir(d);
        while (e)
        {
            if (strcasecmp(c, e->d_name) == 0)
            {
                strcpy(r + rl, e->d_name);
                rl += strlen(e->d_name);

                closedir(d);
                d = opendir(r);
                
                break;
            }
            
            e = readdir(d);
        }
        
        if (!e)
        {
            strcpy(r + rl, c);
            rl += strlen(c);
            last = 1;
        }
        
        c = strsep(&p, "/");
    }
    
    if (d) closedir(d);
    return 1;
}
#endif

FILE *fcaseopen(char const *path, char const *mode)
{
    Ps2IoLockAcquire();
#ifdef PS2_PLATFORM
    char aResolved[300];
    path = ps2_resolve_read_path(path, mode, aResolved, sizeof(aResolved));
#endif
    FILE *f = fopen(path, mode);
#if FCASEOPEN_USE_CASEPATH
    if (!f)
    {
        char *r = alloca(strlen(path) + 3);
        if (casepath(path, r))
        {
            f = fopen(r, mode);
        }
    }
#endif
    Ps2IoLockRelease();
    return f;
}

void casechdir(char const *path)
{
    Ps2IoLockAcquire();
#if FCASEOPEN_USE_CASEPATH
    char *r = alloca(strlen(path) + 3);
    if (casepath(path, r))
    {
        chdir(r);
    }
    else
    {
        errno = ENOENT;
    }
#else
    chdir(path);
#endif
    Ps2IoLockRelease();
}
