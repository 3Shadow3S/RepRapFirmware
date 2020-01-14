/*
 * ObjectModel.cpp
 *
 *  Created on: 27 Aug 2018
 *      Author: David
 */

#include "ObjectModel.h"

#if SUPPORT_OBJECT_MODEL

#include <OutputMemory.h>
#include <GCodes/GCodeBuffer/StringParser.h>
#include <cstring>
#include <General/SafeStrtod.h>

void ObjectExplorationContext::AddIndex(int32_t index)
{
	if (numIndicesCounted == MaxIndices)
	{
		throw GCodeException(-1, -1, "Too many indices");
	}
	indices[numIndicesCounted] = index;
	++numIndicesCounted;
}

void ObjectExplorationContext::AddIndex()
{
	if (numIndicesCounted == numIndicesProvided)
	{
		THROW_INTERNAL_ERROR;
	}
	++numIndicesCounted;
}

void ObjectExplorationContext::RemoveIndex()
{
	if (numIndicesCounted == 0)
	{
		THROW_INTERNAL_ERROR;
	}
	--numIndicesCounted;
}

void ObjectExplorationContext::ProvideIndex(int32_t index)
{
	if (numIndicesProvided == MaxIndices)
	{
		throw GCodeException(-1, -1, "Too many indices");
	}
	indices[numIndicesProvided] = index;
	++numIndicesProvided;
}

// Constructor
ObjectModel::ObjectModel() noexcept
{
}

// ObjectExplorationContext members

ObjectExplorationContext::ObjectExplorationContext(const char *reportFlags, bool wal) noexcept
	: numIndicesProvided(0), numIndicesCounted(0), shortForm(false), onlyLive(false), includeVerbose(false), wantArrayLength(wal)
{
	while (true)
	{
		switch(*reportFlags)
		{
		case '\0':
			return;
		case 'v':
			includeVerbose = true;
			break;
		case 's':
			shortForm = true;
			break;
		case 'f':
			onlyLive = true;
			break;
		default:
			break;
		}
		++reportFlags;
	}
}

int32_t ObjectExplorationContext::GetIndex(size_t n) const
{
	if (n < numIndicesCounted)
	{
		return indices[numIndicesCounted - n - 1];
	}
	THROW_INTERNAL_ERROR;
}

int32_t ObjectExplorationContext::GetLastIndex() const
{
	if (numIndicesCounted != 0)
	{
		return indices[numIndicesCounted - 1];
	}
	THROW_INTERNAL_ERROR;
}

bool ObjectExplorationContext::ShouldReport(const ObjectModelEntryFlags f) const noexcept
{
	return (!onlyLive || ((uint8_t)f & (uint8_t)ObjectModelEntryFlags::live) != 0)
		&& (includeVerbose || ((uint8_t)f & (uint8_t)ObjectModelEntryFlags::verbose) == 0);
}

// Report this object
void ObjectModel::ReportAsJson(OutputBuffer* buf, ObjectExplorationContext& context, uint8_t tableNumber, const char* filter) const
{
	bool added = false;
	const uint8_t *descriptor;
	const ObjectModelTableEntry *tbl = GetObjectModelTable(descriptor);
	if (tableNumber < descriptor[0])
	{
		size_t numEntries = descriptor[tableNumber + 1];
		while (tableNumber != 0)
		{
			--tableNumber;
			tbl += descriptor[tableNumber + 1];
		}

		while (numEntries != 0)
		{
			if (tbl->Matches(filter, context))
			{
				if (!added)
				{
					if (*filter == 0)
					{
						buf->cat('{');
					}
					added = true;
				}
				else
				{
					buf->cat(',');
				}
				tbl->ReportAsJson(buf, context, this, filter);
			}
			--numEntries;
			++tbl;
		}
		if (added && *filter == 0)
		{
			buf->cat('}');
		}
	}
	if (!added)
	{
		buf->cat("null");
	}
}

// Construct a JSON representation of those parts of the object model requested by the user. This version is called on the root of the tree.
void ObjectModel::ReportAsJson(OutputBuffer *buf, const char *filter, const char *reportFlags, bool wantArrayLength) const
{
	ObjectExplorationContext context(reportFlags, wantArrayLength);
	ReportAsJson(buf, context, 0, filter);
}

// Function to report a value or object as JSON
void ObjectModel::ReportItemAsJson(OutputBuffer *buf, ObjectExplorationContext& context, ExpressionValue val, const char *filter) const
{
	if (context.WantArrayLength() && *filter == 0)
	{
		// We have been asked for the length of an array and we have reached the end of the filter, so the value should be an array
		if (val.type == TYPE_OF(const ObjectModelArrayDescriptor*))
		{
			buf->catf("%u", val.omadVal->GetNumElements(this, context));
		}
		else
		{
			buf->cat("null");
		}
	}
	else
	{
		switch (val.type)
		{
		case TYPE_OF(const ObjectModelArrayDescriptor*):
			if (*filter == '[')
			{
				++filter;
				if (*filter == ']')						// if reporting on [parts of] all elements in the array
				{
					return ReportArrayAsJson(buf, context, val.omadVal, filter + 1);
				}

				const char *endptr;
				const long index = SafeStrtol(filter, &endptr);
				if (endptr == filter || *endptr != ']' || index < 0 || (size_t)index >= val.omadVal->GetNumElements(this, context))
				{
					buf->cat("null");					// avoid returning badly-formed JSON
					break;								// invalid syntax, or index out of range
				}
				if (*filter == 0)
				{
					buf->cat('[');
				}
				context.AddIndex(index);
				{
					ReadLocker lock(val.omadVal->lockPointer);
					ReportItemAsJson(buf, context, val.omadVal->GetElement(this, context), endptr + 1);
				}
				context.RemoveIndex();
				if (*filter == 0)
				{
					buf->cat(']');
				}
			}
			else if (*filter == 0)						// else reporting on all subparts of all elements in the array, or just the length
			{
				ReportArrayAsJson(buf, context, val.omadVal, filter);
			}
			break;

		case TYPE_OF(const ObjectModel*):
			if (val.omVal != nullptr)
			{
				if (*filter == '.')
				{
					++filter;
				}
				return val.omVal->ReportAsJson(buf, context, val.param, filter);
			}
			buf->cat("null");
			break;

		case TYPE_OF(float):
			buf->catf((val.param == 3) ? "%.3f" : (val.param == 2) ? "%.2f" : "%.1f", (double)val.fVal);
			break;

		case TYPE_OF(uint32_t):
			buf->catf("%" PRIu32, val.uVal);
			break;

		case TYPE_OF(int32_t):
			buf->catf("%" PRIi32, val.iVal);
			break;

		case TYPE_OF(const char*):
			buf->EncodeString(val.sVal, true);
			break;

		case TYPE_OF(Bitmap32):
			if (context.ShortFormReport())
			{
				buf->catf("%" PRIu32, val.uVal);
			}
			else
			{
				uint32_t v = val.uVal;
				buf->cat('[');
				buf->cat((v & 1) ? '1' : '0');
				for (unsigned int i = 1; i < 32; ++i)
				{
					v >>= 1;
					buf->cat(',');
					buf->cat((v & 1) ? '1' : '0');
				}
				buf->cat(']');
			}
			break;

		case TYPE_OF(Enum32):
			if (context.ShortFormReport())
			{
				buf->catf("%" PRIu32, val.uVal);
			}
			else
			{
				buf->cat("\"unimplemented\"");
				// TODO append the real name
			}
			break;

		case TYPE_OF(bool):
			buf->cat((val.bVal) ? "true" : "false");
			break;

		case TYPE_OF(char):
			buf->cat('"');
			buf->EncodeChar(val.cVal);
			buf->cat('"');
			break;

		case TYPE_OF(IPAddress):
			{
				const IPAddress ipVal(val.uVal);
				char sep = '"';
				for (unsigned int q = 0; q < 4; ++q)
				{
					buf->catf("%c%u", sep, ipVal.GetQuad(q));
					sep = '.';
				}
				buf->cat('"');
			}
			break;

		case TYPE_OF(DateTime):
			{
				const time_t time = val.Get40BitValue();
				if (time == 0)
				{
					buf->cat("null");
				}
				else
				{
					tm timeInfo;
					gmtime_r(&time, &timeInfo);
					buf->catf("\"%04u-%02u-%02uT%02u:%02u:%02u\"",
								timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday, timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);
				}
			}
			break;

		case NoType:
			buf->cat("null");
			break;
		}
	}
}

// Report an entire array as JSON
void ObjectModel::ReportArrayAsJson(OutputBuffer *buf, ObjectExplorationContext& context, const ObjectModelArrayDescriptor *omad, const char *filter) const
{
	ReadLocker lock(omad->lockPointer);

	buf->cat('[');
	const size_t count = omad->GetNumElements(this, context);
	for (size_t i = 0; i < count; ++i)
	{
		if (i != 0)
		{
			buf->cat(',');
		}
		context.AddIndex(i);
		ReportItemAsJson(buf, context, omad->GetElement(this, context), filter);
		context.RemoveIndex();
	}
	buf->cat(']');
}

// Find the requested entry
const ObjectModelTableEntry* ObjectModel::FindObjectModelTableEntry(uint8_t tableNumber, const char* idString) const noexcept
{
	const uint8_t *descriptor;
	const ObjectModelTableEntry *tbl = GetObjectModelTable(descriptor);
	if (tableNumber >= descriptor[0])
	{
		return nullptr;
	}

	const size_t numEntries = descriptor[tableNumber + 1];
	while (tableNumber != 0)
	{
		--tableNumber;
		tbl += descriptor[tableNumber + 1];
	}

	size_t low = 0, high = numEntries;
	while (high > low)
	{
		const size_t mid = (high - low)/2 + low;
		const int t = tbl[mid].IdCompare(idString);
		if (t == 0)
		{
			return &tbl[mid];
		}
		if (t > 0)
		{
			low = mid + 1u;
		}
		else
		{
			high = mid;
		}
	}
	return (low < numEntries && tbl[low].IdCompare(idString) == 0) ? &tbl[low] : nullptr;
}

/*static*/ const char* ObjectModel::GetNextElement(const char *id) noexcept
{
	while (*id != 0 && *id != '.' && *id != '[' && *id != '^')
	{
		++id;
	}
	return id;
}

bool ObjectModelTableEntry::Matches(const char* filterString, const ObjectExplorationContext& context) const noexcept
{
	return IdCompare(filterString) == 0 && context.ShouldReport(flags);
}

// Add the value of this element to the buffer, returning true if it matched and we did
void ObjectModelTableEntry::ReportAsJson(OutputBuffer* buf, ObjectExplorationContext& context, const ObjectModel *self, const char* filter) const noexcept
{
	if (*filter == 0)
	{
		buf->cat('"');
		buf->cat(name);
		buf->cat("\":");
	}
	const char * nextElement = ObjectModel::GetNextElement(filter);
	if (*nextElement == '.')
	{
		++nextElement;
	}
	self->ReportItemAsJson(buf, context, func(self, context), nextElement);
}

// Compare an ID with the name of this object
int ObjectModelTableEntry::IdCompare(const char *id) const noexcept
{
	if (id[0] == 0 || id[0] == '*')
	{
		return 0;
	}

	const char *n = name;
	while (*id == *n && *n != 0)
	{
		++id;
		++n;
	}
	return (*n == 0 && (*id == 0 || *id == '.' || *id == '[' || *id == '^')) ? 0
		: (*id > *n) ? 1
			: -1;
}

// Get the value of an object
ExpressionValue ObjectModel::GetObjectValue(const StringParser& sp, ObjectExplorationContext& context, const char *idString, uint8_t tableNumber) const
{
	const ObjectModelTableEntry *const e = FindObjectModelTableEntry(tableNumber, idString);
	if (e == nullptr)
	{
		throw sp.ConstructParseException("unknown value %s", idString);
	}

	idString = GetNextElement(idString);
	ExpressionValue val = e->func(this, context);
	return GetObjectValue(sp, context, val, idString);
}

ExpressionValue ObjectModel::GetObjectValue(const StringParser& sp, ObjectExplorationContext& context, ExpressionValue val, const char *idString) const
{
	if (val.type == TYPE_OF(const ObjectModelArrayDescriptor*))
	{
		if (*idString == 0 && context.WantArrayLength())
		{
			ReadLocker lock(val.omadVal->lockPointer);
			return ExpressionValue((int32_t)val.omadVal->GetNumElements(this, context));
		}
		if (*idString != '^')
		{
			throw sp.ConstructParseException("missing array index");
		}

		context.AddIndex();
		ReadLocker lock(val.omadVal->lockPointer);

		if (context.GetLastIndex() < 0 || (size_t)context.GetLastIndex() >= val.omadVal->GetNumElements(this, context))
		{
			throw sp.ConstructParseException("array index out of bounds");
		}

		const ExpressionValue arrayElement = val.omadVal->GetElement(this, context);
		return GetObjectValue(sp, context, arrayElement, idString + 1);
	}

	if (val.type == TYPE_OF(const ObjectModel*))
	{
		if (*idString == '.')
		{
			return val.omVal->GetObjectValue(sp, context, idString + 1, val.param);
		}
		throw sp.ConstructParseException((*idString == 0) ? "selected value has non-primitive type" : "syntax error in value selector string");
	}

	if (*idString == 0)
	{
		return val;
	}

	throw sp.ConstructParseException("reached primitive type before end of selector string");
}

#endif

// End
