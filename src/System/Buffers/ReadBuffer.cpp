#include "ReadBuffer.h"
#include "Buffer.h"

ReadBuffer::ReadBuffer()
{
    buffer = NULL;
    length = 0;
}

ReadBuffer::ReadBuffer(char* buffer_, unsigned length_)
{
    buffer = buffer_;
    length = length_;
}

ReadBuffer::ReadBuffer(const char* s)
{
    buffer = (char*) s;
    length = strlen(s);
}

ReadBuffer::ReadBuffer(Buffer& buffer)
{
    Wrap(buffer);
}

void ReadBuffer::Reset()
{
    buffer = NULL;
    length = 0;
}

void ReadBuffer::SetBuffer(char* buffer_)
{
    buffer = buffer_;
}

void ReadBuffer::SetLength(unsigned length_)
{
    length = length_;
}

void ReadBuffer::Wrap(const char* buffer_)
{
    buffer = (char*) buffer_;
    length = strlen(buffer);
}

void ReadBuffer::Wrap(char* buffer_, unsigned length_)
{
    buffer = buffer_;
    length = length_;
}

void ReadBuffer::Wrap(Buffer& buffer_)
{
    buffer = buffer_.GetBuffer();
    length = buffer_.GetLength();
}

int ReadBuffer::Readf(const char* format, ...)
{
    int         read;
    va_list     ap;
    
    va_start(ap, format);
    read = VReadf(buffer, length, format, ap);
    va_end(ap);
    
    return read;
}

char* ReadBuffer::GetBuffer() const
{
    return buffer;
}

unsigned ReadBuffer::GetLength() const
{
    return length;
}

char ReadBuffer::GetCharAt(unsigned i)
{
    if (i > length - 1)
        ASSERT_FAIL();
    
    return *(buffer + i);
}

uint32_t ReadBuffer::GetChecksum()
{
    return ChecksumBuffer(buffer, length);
}

void ReadBuffer::Advance(unsigned i)
{
    assert(length - i <= length);

    buffer += i;
    length -= i;    
}

bool ReadBuffer::BeginsWith(const char* s)
{
    unsigned len;
    
    len = strlen(s);
    
    if (length < len)
        return false;

    if (strncmp(s, buffer, len) == 0)
        return true;
    else
        return false;
}

bool ReadBuffer::ReadChar(char& x)
{
    if (length < sizeof(char))
        return false;
    
    x = *(char*) buffer;
    return true;
}

bool ReadBuffer::ReadLittle16(uint16_t& x)
{
    if (length < sizeof(uint16_t))
        return false;
    
    x = *(uint16_t*) buffer;
    x = FromLittle16(x);
    return true;
}

bool ReadBuffer::ReadLittle32(uint32_t& x)
{
    if (length < sizeof(uint32_t))
        return false;
    
    x = *(uint32_t*) buffer;
    x = FromLittle32(x);
    return true;
}

bool ReadBuffer::ReadLittle64(uint64_t& x)
{
    if (length < sizeof(uint64_t))
        return false;
    
    x = *(uint64_t*) buffer;
    x = FromLittle64(x);
    return true;
}
