#ifndef READBUFFER_H
#define READBUFFER_H

#include "System/Common.h"
#include <string.h>

class Buffer; // forward

/*
===============================================================================================

 ReadBuffer

===============================================================================================
*/

class ReadBuffer
{
public:
    ReadBuffer();
    ReadBuffer(char* buffer, unsigned length);
    ReadBuffer(const char* s);
    ReadBuffer(const Buffer& buffer);
    
    void                Reset();
    void                SetBuffer(char* buffer);
    void                SetLength(unsigned length);
    void                Wrap(const char* buffer);
    void                Wrap(char* buffer, unsigned length);
    void                Wrap(const Buffer& buffer);
    
    int                 Readf(const char* format, ...);

    char*               GetBuffer() const;
    unsigned            GetLength() const;
    char                GetCharAt(unsigned i) const;
    uint32_t            GetChecksum();
    
    void                Advance(unsigned i);
    
    bool                BeginsWith(const char* s);
    bool                BeginsWith(ReadBuffer& other);
    bool                Equals(const char* s);
    bool                Equals(const ReadBuffer& other);
    
    int                 Find(const ReadBuffer& other);
    
    bool                IsAsciiPrintable();
    
    bool                ReadChar(char& x);
    bool                ReadLittle16(uint16_t& x);
    bool                ReadLittle32(uint32_t& x);
    bool                ReadLittle64(uint64_t& x);

    static bool         LessThan(const ReadBuffer& a, const ReadBuffer& b);
    static int          Cmp(const ReadBuffer& a, const ReadBuffer& b);
    static int          CmpWithOffset(const ReadBuffer& a, const ReadBuffer& b, unsigned offset);

private:
    char*               buffer;
    unsigned            length;
};


inline bool ReadBuffer::LessThan(const ReadBuffer& a, const ReadBuffer& b)
{
    return Cmp(a, b) < 0 ? true : false;
}

inline int ReadBuffer::Cmp(const ReadBuffer& a, const ReadBuffer& b)
{
    int ret;
    unsigned alen, blen;

    alen = a.GetLength();
    blen = b.GetLength();
    ret = memcmp(a.GetBuffer(), b.GetBuffer(), MIN(alen, blen));
    
    if (ret != 0)
        return ret;
        
    if (alen < blen)
        return -1;
    else if (blen < alen)
        return 1;
    else
        return 0;
}

inline int ReadBuffer::CmpWithOffset(const ReadBuffer& a, const ReadBuffer& b, unsigned offset)
{
    int         ret;
    unsigned    alen;
    unsigned    blen;
    unsigned    min;

    alen = a.GetLength();
    blen = b.GetLength();
    min = MIN(alen, blen);
    ASSERT(min >= offset);
    min -= offset;
    ret = memcmp(a.GetBuffer() + offset, b.GetBuffer() + offset, min);
    
    if (ret != 0)
        return ret;
        
    if (alen < blen)
        return -1;
    else if (blen < alen)
        return 1;
    else
        return 0;
}

#endif
