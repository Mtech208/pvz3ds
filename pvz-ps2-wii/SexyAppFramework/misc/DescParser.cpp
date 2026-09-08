#include "DescParser.h"
#include "paklib/PakInterface.h"
#include "../../../Sexy.TodLib/TodDebug.h"

using namespace Sexy;

DescParser::DescParser()
{
	mCmdSep = CMDSEP_SEMICOLON;
}

DescParser::~DescParser()
{
}

bool DescParser::Error(const std::string& theError)
{
	mError = theError;
	return false;
}

DataElement* DescParser::Dereference(const std::string& theString)
{
	std::string aDefineName = StringToUpper(theString);

	DataElementMap::iterator anItr = mDefineMap.find(aDefineName);
	if (anItr != mDefineMap.end())
		return anItr->second;
	else
		return NULL;
}

bool DescParser::IsImmediate(const std::string& theString)
{
	return (((theString[0] >= '0') && (theString[0] <= '9')) || (theString[0] == '-') || 
		(theString[0] == '+') || (theString[0] == '\'') || (theString[0] == '"'));
}

std::string DescParser::Unquote(const std::string& theQuotedString)
{
	if ((theQuotedString[0] == '\'') || (theQuotedString[0] == '"'))
	{
		char aQuoteChar = theQuotedString[0];
		std::string aLiteralString;
		bool lastWasQuote = false;
					
		for (ulong i = 0; i < theQuotedString.length(); i++)
		{
			if (theQuotedString[i] == aQuoteChar)
			{
				if (lastWasQuote)
					aLiteralString += aQuoteChar;
				
				lastWasQuote = true;
			}
			else
			{
				aLiteralString += theQuotedString[i];
				lastWasQuote = false;
			}
		}

		return aLiteralString;
	}
	else
		return theQuotedString;
}

bool DescParser::GetValues(ListDataElement* theSource, ListDataElement* theValues)
{
	theValues->mElementVector.clear();
	
	for (ulong aSourceNum = 0; aSourceNum < theSource->mElementVector.size(); aSourceNum++)
	{
		if (theSource->mElementVector[aSourceNum]->mIsList)
		{
			ListDataElement* aChildList = new ListDataElement();
			theValues->mElementVector.push_back(aChildList);

			if (!GetValues((ListDataElement*) theSource->mElementVector[aSourceNum], aChildList))
				return false;
		}
		else
		{
			std::string aString = ((SingleDataElement*) theSource->mElementVector[aSourceNum])->mString;

			if (aString.length() > 0)
			{				
				if ((aString[0] == '\'') || (aString[0] == '"'))
				{
					SingleDataElement* aChildData = new SingleDataElement(Unquote(aString));
					theValues->mElementVector.push_back(aChildData);					
				}
				else if (IsImmediate(aString))
				{
					theValues->mElementVector.push_back(new SingleDataElement(aString));
				}
				else
				{
					std::string aDefineName = StringToUpper(aString);

					DataElementMap::iterator anItr = mDefineMap.find(aDefineName);

					if (anItr == mDefineMap.end())
					{
						Error("Unable to Dereference \"" + aString + "\"");
						return false;
					}
					
					theValues->mElementVector.push_back(anItr->second->Duplicate());
				}
			}

			
		}
	}

	return true;
}

std::string DescParser::DataElementToString(DataElement* theDataElement)
{
	if (theDataElement->mIsList)
	{
		ListDataElement* aListDataElement = (ListDataElement*) theDataElement;

		std::string aString = "(";

		for (ulong i = 0; i < aListDataElement->mElementVector.size(); i++)
		{
			if (i != 0)
				aString += ", ";

			aString += DataElementToString(aListDataElement->mElementVector[i]);
		}

		aString += ")";

		return aString;
	}
	else
	{
		SingleDataElement* aSingleDataElement = (SingleDataElement*) theDataElement;
		return aSingleDataElement->mString;
	}
}

bool DescParser::DataToString(DataElement* theSource, std::string* theString)
{
	*theString = "";

	if (theSource->mIsList)
		return false;

	std::string aDefName = ((SingleDataElement*) theSource)->mString;

	DataElement* aDataElement = Dereference(aDefName);
	
	if (aDataElement != NULL)
	{
		if (aDataElement->mIsList)
			return false;

		*theString = Unquote(((SingleDataElement*) aDataElement)->mString);
	}
	else
		*theString = Unquote(aDefName);				

	return true;
}

bool DescParser::DataToInt(DataElement* theSource, int* theInt)
{
	*theInt = 0;

	std::string aTempString;
	if (!DataToString(theSource, &aTempString))
		return false;
	
	if (!StringToInt(aTempString, theInt))
		return false;

	return true;
}

bool DescParser::DataToStringVector(DataElement* theSource, StringVector* theStringVector)
{
	theStringVector->clear();

	ListDataElement aStaticValues = ListDataElement();
	ListDataElement* aValues;

	if (theSource->mIsList)
	{
		if (!GetValues((ListDataElement*) theSource, &aStaticValues))
			return false;

		aValues = &aStaticValues;
	}
	else
	{
		std::string aDefName = ((SingleDataElement*) theSource)->mString;

		DataElement* aDataElement = Dereference(aDefName);
		
		if (aDataElement == NULL)
		{
			Error("Unable to Dereference \"" + aDefName + "\"");
			return false;
		}

		if (!aDataElement->mIsList)
			return false;

		aValues = (ListDataElement*) aDataElement;
	}	

	for (ulong i = 0; i < aValues->mElementVector.size(); i++)
	{
		if (aValues->mElementVector[i]->mIsList)
		{
			theStringVector->clear();
			return false;
		}		

		SingleDataElement* aSingleDataElement = (SingleDataElement*) aValues->mElementVector[i];

		theStringVector->push_back(aSingleDataElement->mString);
	}

	return true;
}

bool DescParser::DataToList(DataElement* theSource, ListDataElement* theValues)
{
	if (theSource->mIsList)
	{
		return GetValues((ListDataElement*) theSource, theValues);		
	}

	DataElement* aDataElement = Dereference(((SingleDataElement*) theSource)->mString);
		
	if ((aDataElement == NULL) || (!aDataElement->mIsList))
		return false;

	ListDataElement* aListElement = (ListDataElement*) aDataElement;

	*theValues = *aListElement;

	return true;
}

bool DescParser::DataToIntVector(DataElement* theSource, IntVector* theIntVector)
{
	theIntVector->clear();
	
	StringVector aStringVector = StringVector();
	if (!DataToStringVector(theSource, &aStringVector))
		return false;

	for (ulong i = 0; i < aStringVector.size(); i++)
	{		
		int aIntVal;
		if (!StringToInt(aStringVector[i], &aIntVal))
			return false;

		theIntVector->push_back(aIntVal);
	}

	return true;
}

bool DescParser::DataToDoubleVector(DataElement* theSource, DoubleVector* theDoubleVector)
{
	theDoubleVector->clear();
	
	StringVector aStringVector;
	if (!DataToStringVector(theSource, &aStringVector))
		return false;	

	for (ulong i = 0; i < aStringVector.size(); i++)
	{		
		double aDoubleVal;
		if (!StringToDouble(aStringVector[i], &aDoubleVal))
			return false;

		theDoubleVector->push_back(aDoubleVal);
	}

	return true;
}

bool DescParser::ParseToList(const std::string& theString, ListDataElement* theList, bool expectListEnd, int* theStringPos)
{
	bool inSingleQuotes = false;
	bool inDoubleQuotes = false;
	bool escaped = false;	

	SingleDataElement* aCurSingleDataElement = NULL;	

	int aStringPos = 0;
	
	if (theStringPos == NULL)
		theStringPos = &aStringPos;

	while (*theStringPos < (int) theString.length())
	{
		bool addSingleChar = false;
		char aChar = theString[(*theStringPos)++];

		bool isSeperator = (aChar == ' ') || (aChar == '\t') || (aChar == '\n') || (aChar == ',');
		
		if (escaped)
		{
			addSingleChar = true;
			escaped = false;
		}
		else
		{
			if ((aChar == '\'') && (!inDoubleQuotes))
				inSingleQuotes = !inSingleQuotes;
			else if ((aChar == '"') && (!inSingleQuotes))
				inDoubleQuotes = !inDoubleQuotes;

			if (aChar == '\\')
			{
				escaped = true;
			}
			else if ((!inSingleQuotes) && (!inDoubleQuotes))
			{
				if (aChar == ')')
				{
					if (expectListEnd)
						return true;
					else
					{
						Error("Unexpected List End");
						return false;
					}
				}
				else if (aChar == '(') 
				{
					if (aCurSingleDataElement != NULL)
					{
						Error("Unexpected List Start");
						return false;
					}
					else
					{
						ListDataElement* aChildList = new ListDataElement();

						if (!ParseToList(theString, aChildList, true, theStringPos))
							return false;

						theList->mElementVector.push_back(aChildList);
					}
				}				
				else if (isSeperator)
				{
					if (aCurSingleDataElement != NULL)
						aCurSingleDataElement = NULL;					
				}
				else
					addSingleChar = true;
			}
			else
				addSingleChar = true;						
		}

		if (addSingleChar)
		{
			if (aCurSingleDataElement == NULL)
			{
				aCurSingleDataElement = new SingleDataElement();
				theList->mElementVector.push_back(aCurSingleDataElement);
			}

			aCurSingleDataElement->mString += aChar;
		}
	}

	if (inSingleQuotes)
	{
		Error("Unterminated Single Quotes");
		return false;
	}

	if (inDoubleQuotes)
	{
		Error("Unterminated Double Quotes");
		return false;
	}

	if (expectListEnd)
	{
		Error("Unterminated List");
		return false;
	}

	return true;
}

bool DescParser::ParseDescriptorLine(const std::string& theDescriptorLine)
{
	ListDataElement aParams;
	if (!ParseToList(theDescriptorLine, &aParams, false, NULL))
		return false;
	
	if (aParams.mElementVector.size() > 0)
	{
		if (aParams.mElementVector[0]->mIsList)
		{
			Error("Missing Command");
			return false;
		}

		if (!HandleCommand(aParams))
			return false;
	}

	return true;
}

bool DescParser::LoadDescriptor(const std::string& theFileName)
{
	mCurrentLineNum = 0;
	int aLineCount = 0;
	bool hasErrors = false;

	//Apparently VC6 doesn't have a clear() function for basic_strings
	mError.clear();
	//mError.erase();
	//mError.erase(mError.begin());

	TodTraceAndLog("[FONTDESC] LoadDescriptor start: %s", theFileName.c_str());
	PFILE *aStream = p_fopen(theFileName.c_str(),"r");
	TodTraceAndLog("[FONTDESC] p_fopen result: %p", aStream);
	if (aStream==NULL)
	{
		TodTraceAndLog("[FONTDESC] p_fopen FAILED");
		return false;	
	}
	// Strip UTF-8 BOM (EF BB BF) that the packed PC font descriptors start
	// with. Without this the BOM bytes are seen as the first token on line 1
	// causing "Unknown Command on Line 1" and falling back to LoadLegacy.
	// Read the first three bytes: if they are exactly the BOM, keep the stream
	// positioned after it; otherwise rewind. The old fgetc/ungetc dance left a
	// stream-backed PFILE with its logical position different from the bytes
	// read, so the BOM survived and the parse still failed.
	{
		unsigned char aHead[3] = { 0, 0, 0 };
		int aGot = (int)p_fread(aHead, 1, 3, aStream);
		if (aGot == 3 && aHead[0] == 0xEF && aHead[1] == 0xBB && aHead[2] == 0xBF)
		{
			TodTraceAndLog("[FONTDESC] UTF-8 BOM stripped");
		}
		else
		{
			p_fseek(aStream, 0, SEEK_SET);
		}
	}
	TodTraceAndLog("[FONTDESC] entering while !p_feof loop");
	char aBuffChar = 0;
	int aIter = 0;

	while (!p_feof(aStream))
	{		
		if ((aIter++ % 50000) == 0)
			TodTraceAndLog("[FONTDESC] outer iter %d, p_feof=%d, mPos=%d, mSize=%d", aIter, p_feof(aStream), aStream ? aStream->mPos : -1, aStream && aStream->mRecord ? aStream->mRecord->mSize : -1);
		if (aIter > 200000)
		{
			TodTraceAndLog("[FONTDESC] SAFETY BREAK: too many iterations");
			break;
		}
		int aChar;
						
		bool skipLine = false;
		bool atLineStart = true;
		bool inSingleQuotes = false;
		bool inDoubleQuotes = false;
		bool escaped = false; 
		bool isIndented = false;

		for (;;)
		{
			if (aBuffChar != 0)
			{
				aChar = aBuffChar;
				aBuffChar = 0;
			}
			else
			{
				aChar = p_fgetc(aStream);
				if (aChar==EOF)
				{
					TodTraceAndLog("[FONTDESC] inner EOF at mPos=%d", aStream ? aStream->mPos : -1);
					break;
				}
			}
			if ((aIter & 0xFFF) == 0)
				TodTraceAndLog("[FONTDESC] inner mPos=%d", aStream ? aStream->mPos : -1);
			
			if (aChar != '\r')
			{
				if (aChar == '\n')
					aLineCount++;

				if (((aChar == ' ') || (aChar == '\t')) && (atLineStart))
					isIndented = true;

				if ((!atLineStart) || ((aChar != ' ') && (aChar != '\t') && (aChar != '\n')))
				{
					if (atLineStart)
					{
						if ((mCmdSep & CMDSEP_NO_INDENT) && (!isIndented) && (mCurrentLine.size() > 0))
						{
							// Start a new non-indented line
							aBuffChar = aChar;
							break;
						}

						if (aChar == '#')
							skipLine = true;

						atLineStart = false;
					}					

					if (aChar == '\n')		
					{
						isIndented = false;
						atLineStart = true;				
					}

					if ((aChar == '\n') && (skipLine))
					{
						skipLine = false;						
					}
					else if (!skipLine)
					{
						if (aChar == '\\' && (inSingleQuotes || inDoubleQuotes) && !escaped)
							escaped = true;
						else
						{
							if ((aChar == '\'') && (!inDoubleQuotes) && (!escaped))
								inSingleQuotes = !inSingleQuotes;

							if ((aChar == '"') && (!inSingleQuotes) && (!escaped))
								inDoubleQuotes = !inDoubleQuotes;
							
							if ((aChar == ';') && (mCmdSep & CMDSEP_SEMICOLON) && (!inSingleQuotes) && (!inDoubleQuotes))
								break;
							
							if(escaped) // stay escaped for when this is actually parsed
							{
								mCurrentLine += '\\';
								escaped = false;
							}

							if (mCurrentLine.size() == 0)
								mCurrentLineNum = aLineCount + 1;

							mCurrentLine += aChar;
						}
					}
				}
			}
		}

		if (mCurrentLine.length() > 0)
		{
			if (!ParseDescriptorLine(mCurrentLine))
			{
				hasErrors = true;
				break;
			}

			//Apparently VC6 doesn't have a clear() function for basic_strings
			//mCurrentLine.clear();
			mCurrentLine.clear();
		}
	}

	//Apparently VC6 doesn't have a clear() function for basic_strings
	mCurrentLine.clear();
	//mCurrentLine.erase();
	mCurrentLineNum = 0;

	p_fclose(aStream);
	return !hasErrors;
}
