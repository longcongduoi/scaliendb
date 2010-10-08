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
    buffer += i;
    length -= i;
    
    assert(length >= 0);
}

bool ReadBuffer::BeginsWith(const char* s)
{
    unsigned len;
    
    len = strlen(s);
    
    if (len < length)
        return false;

    if (strncmp(s, buffer, len) == 0)
        return true;
    else
        return false;
}