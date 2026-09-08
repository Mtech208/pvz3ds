#ifndef __TODDEFINITIONSTREAMREADER_H__
#define __TODDEFINITIONSTREAMREADER_H__

#include <stddef.h>

class DefinitionStreamReader
{
public:
    virtual ~DefinitionStreamReader() {}

    virtual bool Read(void* theDestination, size_t theSize) = 0;
    virtual bool Finish() = 0;
    virtual size_t Size() const = 0;
    virtual size_t Position() const = 0;
};

#endif
