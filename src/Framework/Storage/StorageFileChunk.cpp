#include "StorageFileChunk.h"
#include "System/FileSystem.h"
#include "StorageBulkCursor.h"
#include "StoragePageCache.h"
#include "FDGuard.h"

static int KeyCmp(const ReadBuffer& a, const ReadBuffer& b)
{
    return ReadBuffer::Cmp(a, b);
}

static const ReadBuffer Key(StorageFileKeyValue* kv)
{
    return kv->GetKey();
}

StorageFileChunk::StorageFileChunk() : headerPage(this)
{
    prev = next = this;
    written = false;
    dataPagesSize = 64;
    dataPages = (StorageDataPage**) malloc(sizeof(StorageDataPage*) * dataPagesSize);
    memset(dataPages, 0, sizeof(StorageDataPage*) * dataPagesSize);
    indexPage = NULL;
    bloomPage = NULL;
    numDataPages = 0;
    fileSize = 0;
    minLogSegmentID = 0;
}

StorageFileChunk::~StorageFileChunk()
{
    unsigned    i;
    
    for (i = 0; i < numDataPages; i++)
    {
        if (dataPages[i] != NULL)
        {
            if (dataPages[i]->IsCached())
                StoragePageCache::RemovePage(dataPages[i]);

            delete dataPages[i];
        }
    }
    free(dataPages);
    
    if (indexPage && indexPage->IsCached())
        StoragePageCache::RemovePage(indexPage);
    delete indexPage;
    
    if (bloomPage && bloomPage->IsCached())
        StoragePageCache::RemovePage(bloomPage);
    delete bloomPage;
}

void StorageFileChunk::ReadHeaderPage()
{
    Buffer      buffer;
    uint32_t    offset;
    
    offset = 0;
    
    if (!ReadPage(offset, buffer))
    {
        Log_Message("Unable to read header page from %s at offset %U", filename.GetBuffer(), offset);
        Log_Message("This should not happen.");
        Log_Message("Possible causes: software bug, damaged file, corrupted file...");
        Log_Message("Exiting...");
        STOP_FAIL(1);
    }
    
    if (!headerPage.Read(buffer))
    {
        Log_Message("Unable to parse header page read from %s at offset %U with size %u",
         filename.GetBuffer(), offset, buffer.GetLength());
        Log_Message("This should not happen.");
        Log_Message("Possible causes: software bug, damaged file, corrupted file...");
        Log_Message("Exiting...");
        STOP_FAIL(1);
    }
    
    fileSize = FS_FileSize(filename.GetBuffer());
}

void StorageFileChunk::SetFilename(Buffer& filename_)
{
    filename.Write(filename_);
    filename.NullTerminate();
}

Buffer& StorageFileChunk::GetFilename()
{
    return filename;
}

StorageChunk::ChunkState StorageFileChunk::GetChunkState()
{
    if (written)
        return StorageChunk::Written;
    else
        return StorageChunk::Unwritten;
}

void StorageFileChunk::NextBunch(StorageCursorBunch& bunch, StorageShard* shard)
{
    uint32_t                index, offset, pos;
    ReadBuffer              nextKey, key, value;
    StorageFileKeyValue*    it;
    StorageFileKeyValue*    kv;
    
    if (!shard->RangeContains(headerPage.GetFirstKey()) && !shard->RangeContains(headerPage.GetLastKey()))
    {
        bunch.isLast = true;
        return;
    }
    nextKey = bunch.GetNextKey();

    if (indexPage == NULL)
        LoadIndexPage();

    if (!indexPage->Locate(nextKey, index, offset))
    {
        index = 0;
        offset = STORAGE_HEADER_PAGE_SIZE;
    }

    if (dataPages[index] == NULL)
        LoadDataPage(index, offset, true);
    
    bunch.buffer.Write(dataPages[index]->buffer);
    FOREACH(it, dataPages[index]->keyValues)
    {
        if (!shard->RangeContains(it->GetKey()))
            continue;

        kv = new StorageFileKeyValue;
        
        pos = it->GetKey().GetBuffer() - dataPages[index]->buffer.GetBuffer();
        key.Wrap(bunch.buffer.GetBuffer() + pos, it->GetKey().GetLength());
        if (it->GetType() == STORAGE_KEYVALUE_TYPE_SET)
        {
            pos = it->GetValue().GetBuffer() - dataPages[index]->buffer.GetBuffer();
            value.Wrap(bunch.buffer.GetBuffer() + pos, it->GetValue().GetLength());
            kv->Set(key, value);
        }
        else
        {
            kv->Delete(key);
        }
        
        bunch.keyValues.Insert(kv);
    }
    
    if (index == (numDataPages - 1))
    {
        bunch.isLast = true;
        return;
    }
    
    index++;
    if (dataPages[index] == NULL)
        LoadDataPage(index, dataPages[index - 1]->GetOffset() + dataPages[index - 1]->GetSize(), true);
    
    bunch.nextKey.Write(dataPages[index]->keyValues.First()->GetKey());
    bunch.isLast = false;
}

uint64_t StorageFileChunk::GetChunkID()
{
    return headerPage.GetChunkID();
}

bool StorageFileChunk::UseBloomFilter()
{
    return headerPage.UseBloomFilter();
}

StorageKeyValue* StorageFileChunk::Get(ReadBuffer& key)
{
    uint32_t    index, offset;
    Buffer      buffer;

    if (headerPage.UseBloomFilter())
    {
        if (bloomPage == NULL)
            LoadBloomPage(); // evicted, load back
        if (bloomPage->IsCached())
            StoragePageCache::RegisterHit(bloomPage);
        if (!bloomPage->Check(key))
            return NULL;
    }

    if (indexPage == NULL)
        LoadIndexPage(); // evicted, load back
    if (indexPage->IsCached())
        StoragePageCache::RegisterHit(indexPage);
    if (!indexPage->Locate(key, index, offset))
        return NULL;
    
    if (dataPages[index] == NULL)
        LoadDataPage(index, offset); // evicted, load back

    if (dataPages[index]->IsCached())
        StoragePageCache::RegisterHit(dataPages[index]);
    return dataPages[index]->Get(key);
}

uint64_t StorageFileChunk::GetMinLogSegmentID()
{
    return minLogSegmentID;
}

uint64_t StorageFileChunk::GetMaxLogSegmentID()
{
    return headerPage.GetLogSegmentID();
}

uint32_t StorageFileChunk::GetMaxLogCommandID()
{
    return headerPage.GetLogCommandID();
}

ReadBuffer StorageFileChunk::GetFirstKey()
{
    return headerPage.GetFirstKey();
}

ReadBuffer StorageFileChunk::GetLastKey()
{
    return headerPage.GetLastKey();
}

uint64_t StorageFileChunk::GetSize()
{
    return fileSize;
}

ReadBuffer StorageFileChunk::GetMidpoint()
{
    return headerPage.GetMidpoint();
}

void StorageFileChunk::AddPagesToCache()
{
    uint32_t i;
    
    if (UseBloomFilter())
        StoragePageCache::AddPage(bloomPage);

    StoragePageCache::AddPage(indexPage);

    for (i = 0; i < numDataPages; i++)
        StoragePageCache::AddPage(dataPages[i]);
}

void StorageFileChunk::OnBloomPageEvicted()
{
    delete bloomPage;
    bloomPage = NULL;
}

void StorageFileChunk::OnIndexPageEvicted()
{
    delete indexPage;
    indexPage = NULL;
}

void StorageFileChunk::OnDataPageEvicted(uint32_t index)
{
    assert(dataPages[index] != NULL);
    
    delete dataPages[index];
    dataPages[index] = NULL;
}

void StorageFileChunk::LoadBloomPage()
{
    Buffer      buffer;
    uint32_t    offset;
    
    bloomPage = new StorageBloomPage(this);
    offset = headerPage.GetBloomPageOffset();
    bloomPage->SetOffset(offset);
    if (!ReadPage(offset, buffer))
    {
        Log_Message("Unable to read bloom page from %s at offset %U", filename.GetBuffer(), offset);
        Log_Message("This should not happen.");
        Log_Message("Possible causes: software bug, damaged file, corrupted file...");
        Log_Message("Exiting...");
        STOP_FAIL(1);
    }
    if (!bloomPage->Read(buffer))
    {
        Log_Message("Unable to parse bloom page read from %s at offset %U with size %u",
         filename.GetBuffer(), offset, buffer.GetLength());
        Log_Message("This should not happen.");
        Log_Message("Possible causes: software bug, damaged file, corrupted file...");
        Log_Message("Exiting...");
        STOP_FAIL(1);
    }
    StoragePageCache::AddPage(bloomPage);
}

void StorageFileChunk::LoadIndexPage()
{
    Buffer      buffer;
    uint32_t    offset;
    
    indexPage = new StorageIndexPage(this);
    offset = headerPage.GetIndexPageOffset();
    indexPage->SetOffset(offset);
    if (!ReadPage(offset, buffer))
    {
        Log_Message("Unable to read index page from %s at offset %U", filename.GetBuffer(), offset);
        Log_Message("This should not happen.");
        Log_Message("Possible causes: software bug, damaged file, corrupted file...");
        Log_Message("Exiting...");
        STOP_FAIL(1);
    }
    if (!indexPage->Read(buffer))
    {
        Log_Message("Unable to parse index page read from %s at offset %U with size %u",
         filename.GetBuffer(), offset, buffer.GetLength());
        Log_Message("This should not happen.");
        Log_Message("Possible causes: software bug, damaged file, corrupted file...");
        Log_Message("Exiting...");
        STOP_FAIL(1);
    }
    StoragePageCache::AddPage(indexPage);
    
    // TODO: HACK
    if (numDataPages == 0)
    {
        numDataPages = indexPage->GetNumDataPages();
        StorageDataPage** newDataPages;
        newDataPages = (StorageDataPage**) malloc(sizeof(StorageDataPage*) * numDataPages);
        
        for (unsigned i = 0; i < numDataPages; i++)
            newDataPages[i] = NULL;
        
        free(dataPages);
        dataPages = newDataPages;
        dataPagesSize = numDataPages;
    }
}

void StorageFileChunk::LoadDataPage(uint32_t index, uint32_t offset, bool bulk)
{
    Buffer buffer;
    
    dataPages[index] = new StorageDataPage(this, index);
    dataPages[index]->SetOffset(offset);
    if (!ReadPage(offset, buffer))
    {
        Log_Message("Unable to read data page from %s at offset %U", filename.GetBuffer(), offset);
        Log_Message("This should not happen.");
        Log_Message("Possible causes: software bug, damaged file, corrupted file...");
        Log_Message("Exiting...");
        STOP_FAIL(1);
    }
    if (!dataPages[index]->Read(buffer))
    {
        Log_Message("Unable to parse data page read from %s at offset %U with size %u",
         filename.GetBuffer(), offset, buffer.GetLength());
        Log_Message("This should not happen.");
        Log_Message("Possible causes: software bug, damaged file, corrupted file...");
        Log_Message("Exiting...");
        STOP_FAIL(1);
    }
    StoragePageCache::AddPage(dataPages[index], bulk);
}

bool StorageFileChunk::RangeContains(ReadBuffer key)
{
    int         cf, cl;
    ReadBuffer  firstKey, lastKey;

    firstKey = headerPage.GetFirstKey();
    lastKey = headerPage.GetLastKey();

    cf = ReadBuffer::Cmp(firstKey, key);
    cl = ReadBuffer::Cmp(key, lastKey);

    if (firstKey.GetLength() == 0)
    {
        if (lastKey.GetLength() == 0)
            return false;
        else
            return (cl <= 0);        // (key < lastKey);
    }
    else if (lastKey.GetLength() == 0)
    {
        return (cf <= 0);           // (firstKey <= key);
    }
    else
        return (cf <= 0 && cl <= 0); // (firstKey <= key <= lastKey);
}

void StorageFileChunk::AppendDataPage(StorageDataPage* dataPage)
{
    if (numDataPages == dataPagesSize)
        ExtendDataPageArray();
    
    dataPages[numDataPages] = dataPage;
    numDataPages++;
}

void StorageFileChunk::ExtendDataPageArray()
{
    StorageDataPage**   newDataPages;
    unsigned            newSize, i;
    
    newSize = dataPagesSize * 2;
    newDataPages = (StorageDataPage**) malloc(sizeof(StorageDataPage*) * newSize);
    
    for (i = 0; i < numDataPages; i++)
        newDataPages[i] = dataPages[i];
    
    free(dataPages);
    dataPages = newDataPages;
    dataPagesSize = newSize;
}

bool StorageFileChunk::ReadPage(uint32_t offset, Buffer& buffer)
{
    uint32_t    size, rest;
    ReadBuffer  parse;
    FDGuard     fd;
    
    if (fd.Open(filename.GetBuffer(), FS_READONLY) == INVALID_FD)
        return false;

    size = STORAGE_DEFAULT_PAGE_GRAN;
    buffer.Allocate(size);
    if (FS_FileReadOffs(fd.GetFD(), buffer.GetBuffer(), size, offset) != size)
        return false;
    buffer.SetLength(size);
        
    // first 4 bytes on all pages is the page size
    parse.Wrap(buffer);
    if (!parse.ReadLittle32(size))
        return false;
    
    rest = size - buffer.GetLength();
    if (rest > 0)
    {
        // read rest
        buffer.Allocate(size);
        if (FS_FileReadOffs(fd.GetFD(), buffer.GetPosition(), rest, offset + buffer.GetLength()) != rest)
            return false;
        buffer.SetLength(size);
    }
    
    fd.Close();
    
    return true;
}