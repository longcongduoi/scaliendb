#ifndef STORAGEKEYVALUE_H
#define STORAGEKEYVALUE_H

#include "System/Buffers/ReadBuffer.h"

#define STORAGE_KEYVALUE_TYPE_SET      's'
#define STORAGE_KEYVALUE_TYPE_DELETE   'd'

#define STORAGE_KEY_LESS_THAN(a, b) \
    ((b).GetLength() == 0 || ReadBuffer::Cmp(a, b) < 0)

#define STORAGE_KEY_GREATER_THAN(a, b) \
    (ReadBuffer::Cmp(a, b) >= 0)

/*
===============================================================================================

 StorageKeyValue

===============================================================================================
*/

class StorageKeyValue
{
public:
    virtual ~StorageKeyValue() {}

    virtual char            GetType() = 0;
    virtual ReadBuffer      GetKey() const = 0;
    virtual ReadBuffer      GetValue() const = 0;
};

#endif
