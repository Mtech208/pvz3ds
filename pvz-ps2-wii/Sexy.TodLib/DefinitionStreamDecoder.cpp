#include "DefinitionStreamDecoder.h"

#if defined(PS2_PLATFORM) || defined(WII_PLATFORM) || defined(__3DS__)

#include "Definition.h"
#include "DefinitionStreamReader.h"
#include "SexyAppBase.h"

#include <limits.h>
#include <new>
#include <stdint.h>
#include <string.h>

namespace
{
const int DEFINITION_STREAM_MAX_NAME = 100000;
const size_t DEFINITION_STREAM_PURGE_THRESHOLD = 2u << 20;

class DefinitionTemporaryString
{
public:
    explicit DefinitionTemporaryString(size_t theSize)
        : mData(theSize <= sizeof(mInlineBuffer)
            ? mInlineBuffer
            : new (std::nothrow) char[theSize])
    {
    }

    ~DefinitionTemporaryString()
    {
        if (mData != mInlineBuffer)
            delete[] mData;
    }

    char* Data() const
    {
        return mData;
    }

private:
    DefinitionTemporaryString(const DefinitionTemporaryString&);
    DefinitionTemporaryString& operator=(const DefinitionTemporaryString&);

    char mInlineBuffer[256];
    char* mData;
};

bool DefinitionCheckedAllocationSize(int theCount, int theItemSize, int& theSize)
{
    if (theCount < 0 || theItemSize <= 0 || theCount > INT_MAX / theItemSize)
        return false;

    theSize = theCount * theItemSize;
    return true;
}

bool DefinitionReadMap(DefinitionStreamReader& theReader,
    DefMap* theDefMap, void* theDefinition);

bool DefinitionReadArray(DefinitionStreamReader& theReader,
    DefinitionArrayDef* theArray, DefMap* theDefMap)
{
    int aDefSize;
    if (!theReader.Read(&aDefSize, sizeof(aDefSize)) ||
        aDefSize != theDefMap->mDefSize)
    {
        return false;
    }

    if (theArray->mArrayCount == 0)
        return true;

    int aArraySize;
    if (!DefinitionCheckedAllocationSize(theArray->mArrayCount,
        aDefSize, aArraySize))
    {
        return false;
    }

    theArray->mArrayData = DefinitionAlloc(aArraySize);
    if (!theReader.Read(theArray->mArrayData, aArraySize))
        return false;

    for (int i = 0; i < theArray->mArrayCount; ++i)
    {
        void* aDefinition = (void*)((intptr_t)theArray->mArrayData +
            theDefMap->mDefSize * i);
        if (!DefinitionReadMap(theReader, theDefMap, aDefinition))
            return false;
    }
    return true;
}

bool DefinitionReadFloatTrack(DefinitionStreamReader& theReader,
    FloatParameterTrack* theTrack)
{
    int& aCountNodes = theTrack->mCountNodes;
    if (!theReader.Read(&aCountNodes, sizeof(aCountNodes)))
        return false;
    if (aCountNodes <= 0)
        return aCountNodes == 0;

    int aSize;
    if (!DefinitionCheckedAllocationSize(aCountNodes,
        sizeof(FloatParameterTrackNode), aSize))
    {
        return false;
    }

    FloatParameterTrackNode* aNodes =
        (FloatParameterTrackNode*)DefinitionAlloc(aSize);
    theTrack->mNodes = aNodes;
    return theReader.Read(aNodes, aSize);
}

bool DefinitionReadString(DefinitionStreamReader& theReader, char** theString)
{
    int aLength;
    if (!theReader.Read(&aLength, sizeof(aLength)) ||
        aLength < 0 || aLength > DEFINITION_STREAM_MAX_NAME)
    {
        return false;
    }

    if (aLength == 0)
    {
        *theString = (char*)"";
        return true;
    }

    char* aString = (char*)DefinitionAlloc(aLength + 1);
    *theString = aString;
    if (!theReader.Read(aString, aLength))
        return false;
    aString[aLength] = '\0';
    return true;
}

bool DefinitionReadResourceNameLength(DefinitionStreamReader& theReader,
    int& theLength)
{
    return theReader.Read(&theLength, sizeof(theLength)) &&
        theLength >= 0 && theLength <= DEFINITION_STREAM_MAX_NAME;
}

bool DefinitionReadImage(DefinitionStreamReader& theReader, Image** theImage)
{
    int aLength;
    if (!DefinitionReadResourceNameLength(theReader, aLength))
        return false;

    DefinitionTemporaryString aNameBuffer(aLength + 1);
    if (aNameBuffer.Data() == nullptr ||
        !theReader.Read(aNameBuffer.Data(), aLength))
    {
        return false;
    }
    aNameBuffer.Data()[aLength] = '\0';

    *theImage = nullptr;
    return aNameBuffer.Data()[0] == '\0' ||
        DefinitionLoadImage(theImage, aNameBuffer.Data());
}

bool DefinitionReadFont(DefinitionStreamReader& theReader, _Font** theFont)
{
    int aLength;
    if (!DefinitionReadResourceNameLength(theReader, aLength))
        return false;

    DefinitionTemporaryString aNameBuffer(aLength + 1);
    if (aNameBuffer.Data() == nullptr ||
        !theReader.Read(aNameBuffer.Data(), aLength))
    {
        return false;
    }
    aNameBuffer.Data()[aLength] = '\0';

    *theFont = nullptr;
    return aNameBuffer.Data()[0] == '\0' ||
        DefinitionLoadFont(theFont, aNameBuffer.Data());
}

bool DefinitionReadMap(DefinitionStreamReader& theReader,
    DefMap* theDefMap, void* theDefinition)
{
    for (DefField* aField = theDefMap->mMapFields;
         *aField->mFieldName != '\0'; ++aField)
    {
        void* aDestination = (void*)((intptr_t)theDefinition +
            aField->mFieldOffset);
        bool aSucceeded = true;
        switch (aField->mFieldType)
        {
        case DefFieldType::DT_STRING:
            aSucceeded = DefinitionReadString(theReader,
                (char**)aDestination);
            break;
        case DefFieldType::DT_ARRAY:
            aSucceeded = DefinitionReadArray(theReader,
                (DefinitionArrayDef*)aDestination,
                (DefMap*)aField->mExtraData);
            break;
        case DefFieldType::DT_IMAGE:
            aSucceeded = DefinitionReadImage(theReader,
                (Image**)aDestination);
            break;
        case DefFieldType::DT_FONT:
            aSucceeded = DefinitionReadFont(theReader,
                (_Font**)aDestination);
            break;
        case DefFieldType::DT_TRACK_FLOAT:
            aSucceeded = DefinitionReadFloatTrack(theReader,
                (FloatParameterTrack*)aDestination);
            break;
        default:
            break;
        }

        if (!aSucceeded)
            return false;
    }
    return true;
}
}

bool DefinitionDecodeCompiledStream(DefinitionStreamReader& theReader,
    DefMap* theDefMap, void* theDefinition)
{
    if (theDefMap == nullptr || theDefinition == nullptr ||
        theReader.Size() < (size_t)theDefMap->mDefSize + sizeof(uint))
    {
        return false;
    }

    if (theReader.Size() >= DEFINITION_STREAM_PURGE_THRESHOLD &&
        Sexy::gSexyAppBase != nullptr)
    {
        Sexy::gSexyAppBase->PurgeLazyImageBits(true);
    }

    uint aCacheHash;
    if (!theReader.Read(&aCacheHash, sizeof(aCacheHash)) ||
        aCacheHash != DefinitionCalcHash(theDefMap))
    {
        return false;
    }

    if (!theReader.Read(theDefinition, theDefMap->mDefSize) ||
        !DefinitionReadMap(theReader, theDefMap, theDefinition))
    {
        return false;
    }

    return theReader.Position() == theReader.Size() && theReader.Finish();
}

#endif
