#ifndef _TC9_GUID_H
#define _TC9_GUID_H

#include "Define.h"
#include "ObjectGuid.h"

inline ObjectGuid TC9PlayerGuidFromRawDB(uint64 value)
{
    return value ? ObjectGuid::CreatePlayerFromDBValue(value) : ObjectGuid::Empty;
}

inline uint64 TC9PlayerRawDBGuid(ObjectGuid guid)
{
    return guid.GetDBValue();
}

#endif // _TC9_GUID_H
