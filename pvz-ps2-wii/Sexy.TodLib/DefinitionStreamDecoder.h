#ifndef __TODDEFINITIONSTREAMDECODER_H__
#define __TODDEFINITIONSTREAMDECODER_H__

class DefMap;
class DefinitionStreamReader;

bool DefinitionDecodeCompiledStream(DefinitionStreamReader& theReader,
    DefMap* theDefMap, void* theDefinition);

#endif
