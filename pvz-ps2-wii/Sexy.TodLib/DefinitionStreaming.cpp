#include "DefinitionStreaming.h"

#if defined(PS2_PLATFORM) || defined(WII_PLATFORM) || defined(__3DS__)

#include "Definition.h"
#include "DefinitionStreamDecoder.h"
#include "DefinitionStreamReader.h"
#include "zlib.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

namespace
{
class DefinitionInflateStreamReader : public DefinitionStreamReader
{
public:
    DefinitionInflateStreamReader(const void* theCompressedBuffer,
        size_t theCompressedBufferSize)
        : mSize(0), mPosition(0), mValid(false), mStreamEnded(false)
    {
        memset(&mStream, 0, sizeof(mStream));
        if (theCompressedBuffer == nullptr ||
            theCompressedBufferSize < sizeof(CompressedDefinitionHeader))
        {
            return;
        }

        const CompressedDefinitionHeader* aHeader =
            (const CompressedDefinitionHeader*)theCompressedBuffer;
        if (aHeader->mCookie != 0xDEADFED4L)
            return;

        const size_t aCompressedPayloadSize = theCompressedBufferSize -
            sizeof(CompressedDefinitionHeader);
        if (aCompressedPayloadSize > UINT_MAX)
            return;

        mSize = aHeader->mUncompressedSize;
        mStream.next_in = (Bytef*)((uintptr_t)theCompressedBuffer +
            sizeof(CompressedDefinitionHeader));
        mStream.avail_in = (uInt)aCompressedPayloadSize;
        mValid = inflateInit(&mStream) == Z_OK;
    }

    ~DefinitionInflateStreamReader()
    {
        if (mStream.state != nullptr)
            inflateEnd(&mStream);
    }

    bool IsValid() const
    {
        return mValid;
    }

    bool Read(void* theDestination, size_t theSize) override
    {
        if (!mValid || theDestination == nullptr ||
            theSize > mSize - mPosition)
        {
            return theSize == 0 && mValid;
        }
        if (theSize == 0)
            return true;
        if (mStreamEnded)
            return false;

        Bytef* aDestination = (Bytef*)theDestination;
        size_t aRemaining = theSize;
        while (aRemaining > 0)
        {
            const uInt aChunk = aRemaining > UINT_MAX
                ? UINT_MAX : (uInt)aRemaining;
            mStream.next_out = aDestination;
            mStream.avail_out = aChunk;

            while (mStream.avail_out > 0)
            {
                const int aResult = inflate(&mStream, Z_NO_FLUSH);
                if (aResult == Z_STREAM_END)
                {
                    mStreamEnded = true;
                    if (mStream.avail_out != 0)
                    {
                        mValid = false;
                        return false;
                    }
                    break;
                }
                if (aResult != Z_OK ||
                    (mStream.avail_in == 0 && mStream.avail_out != 0))
                {
                    mValid = false;
                    return false;
                }
            }

            const size_t aWritten = aChunk - mStream.avail_out;
            if (aWritten == 0 || (mStreamEnded && aWritten != aChunk))
            {
                mValid = false;
                return false;
            }

            aDestination += aWritten;
            aRemaining -= aWritten;
        }

        mPosition += theSize;
        return true;
    }

    bool Finish() override
    {
        if (!mValid || mPosition != mSize)
            return false;
        if (mStreamEnded)
            return true;

        Bytef anExtraByte = 0;
        mStream.next_out = &anExtraByte;
        mStream.avail_out = 1;
        while (true)
        {
            const int aResult = inflate(&mStream, Z_FINISH);
            if (aResult == Z_STREAM_END)
            {
                mStreamEnded = true;
                return mStream.avail_out == 1;
            }
            if (aResult != Z_OK && aResult != Z_BUF_ERROR)
                return false;
            if (mStream.avail_out == 0 || mStream.avail_in == 0)
                return false;
        }
    }

    size_t Size() const override
    {
        return mSize;
    }

    size_t Position() const override
    {
        return mPosition;
    }

private:
    z_stream mStream;
    size_t mSize;
    size_t mPosition;
    bool mValid;
    bool mStreamEnded;
};
}

bool DefinitionReadCompiledStream(const void* theCompressedBuffer,
    size_t theCompressedBufferSize, DefMap* theDefMap, void* theDefinition)
{
    DefinitionInflateStreamReader aReader(theCompressedBuffer,
        theCompressedBufferSize);
    return aReader.IsValid() &&
        DefinitionDecodeCompiledStream(aReader, theDefMap, theDefinition);
}

#endif
