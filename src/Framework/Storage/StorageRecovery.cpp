#include "StorageRecovery.h"
#include "System/Stopwatch.h"
#include "System/FileSystem.h"
#include "StorageEnvironment.h"
#include "FDGuard.h"
#include "System/PointerGuard.h"
#include "StorageChunkSerializer.h"
#include "StorageChunkWriter.h"

static bool LessThan(const Buffer* a, const Buffer* b)
{
    return Buffer::Cmp(*a, *b) < 0;
}

bool StorageRecovery::TryRecovery(StorageEnvironment* env_)
{
    uint64_t            trackID;
    const char*         filename;
    Buffer              tmp;
    Buffer              toc, tocNew;
    FS_Dir              dir;
    FS_DirEntry         entry;
    List<uint64_t>      replayedTrackIDs;
    
    env = env_;
    
    toc.Write(env->envPath);
    toc.Append("toc");
    toc.NullTerminate();
    tocNew.Write(env->envPath);
    tocNew.Append("toc.new");
    tocNew.NullTerminate();
    if (TryReadTOC(tocNew))
    {
        FS_Delete(toc.GetBuffer());
        FS_Rename(tocNew.GetBuffer(), toc.GetBuffer());
    }
    else
    {
        if (!TryReadTOC(toc))
            return false;
        
        FS_Delete(tocNew.GetBuffer());
    }
    
    CreateMemoChunks(); 
    
    // compute the max. (logSegmentID, commandID) for each shard's chunk
    // log entries smaller must not be applied to its memo chunk
    ComputeShardRecovery();

    tmp.Write(env->logPath);
    tmp.NullTerminate();
    
    dir = FS_OpenDir(tmp.GetBuffer());
    if (dir == FS_INVALID_DIR)
    {
        Log_Message("Unable to open log directory: %s", tmp.GetBuffer());
        STOP_FAIL(1);
    }
    
    while ((entry = FS_ReadDir(dir)) != FS_INVALID_DIR_ENTRY)
    {
        filename = FS_DirEntryName(entry);
        
        if (FS_IsSpecial(filename))
            continue;
            
        if (FS_IsDirectory(filename))
            continue;

        tmp.Write(filename);
        tmp.Readf("log.%U.", &trackID);
        if (!replayedTrackIDs.Contains(trackID))
        {
            ReplayLogSegments(trackID);
            replayedTrackIDs.Add(trackID);
        }
    }
    
    FS_CloseDir(dir);
    DeleteOrphanedChunks();
    
    return true;
}

bool StorageRecovery::TryReadTOC(Buffer& filename)
{
    uint32_t    size, rest, checksum, compChecksum, version;
    Buffer      buffer;
    ReadBuffer  parse, dataPart;
    FDGuard     fd;
    
    if (fd.Open(filename.GetBuffer(), FS_READONLY) == INVALID_FD)
        return false;

    size = 4;
    buffer.Allocate(size);
    if (FS_FileRead(fd.GetFD(), buffer.GetBuffer(), size) != (ssize_t) size)
        return false;
    buffer.SetLength(size);
        
    // first 4 bytes on all pages is the page size
    parse.Wrap(buffer);
    if (!parse.ReadLittle32(size))
        return false;
    
    if (size == 0)
        return false;
    
    rest = size - buffer.GetLength();
    // read rest
    buffer.Allocate(size);
    if (FS_FileRead(fd.GetFD(), buffer.GetPosition(), rest) != (ssize_t) rest)
        return false;
    buffer.SetLength(size);
    
    parse.Wrap(buffer);
    parse.Advance(4); // already read size
    parse.ReadLittle32(checksum);
    dataPart.Wrap(buffer.GetBuffer() + 8, buffer.GetLength() - 8);
    compChecksum = dataPart.GetChecksum();
    if (compChecksum != checksum)
        return false;
    parse.Advance(4);

    parse.ReadLittle32(version);
    parse.Advance(4);

    Log_Message("Opening chunk files...");
    ReadShards(version, parse);
    Log_Message("Opening done.");
    
    fd.Close();
    
    return true;
}

bool StorageRecovery::ReadShards(uint32_t version, ReadBuffer& parse)
{
    uint32_t    numShards, i;
    
    if (!parse.ReadLittle32(numShards))
        return false;
    parse.Advance(4);
    
    for (i = 0; i < numShards; i++)
    {
        switch (version)
        {
            case 1:
                if (!ReadShardVersion1(parse))
                    return false;
                break;
            default:
                STOP_FAIL(1, "TOC file version is newer than current ScalienDB version!");
                break;
        }
    }
    
    return true;
}

bool StorageRecovery::ReadShardVersion1(ReadBuffer& parse)
{
    uint32_t            numChunks, i;
    uint64_t            chunkID;
    StorageShard*       shard;
    StorageFileChunk*   fileChunk;

    PointerGuard<StorageShard> shardGuard(new StorageShard);
    
    shard = shardGuard.Get();
    
    if (!parse.ReadLittle64(shard->trackID))
        return false;
    parse.Advance(8);
    if (!parse.ReadLittle16(shard->contextID))
        return false;
    parse.Advance(2);
    if (!parse.ReadLittle64(shard->tableID))
        return false;
    parse.Advance(8);
    if (!parse.ReadLittle64(shard->shardID))
        return false;
    parse.Advance(8);
    if (!parse.ReadLittle64(shard->logSegmentID))
        return false;
    parse.Advance(8);
    if (!parse.ReadLittle32(shard->logCommandID))
        return false;
    parse.Advance(4);
    parse.Advance(parse.Readf("%#B", &shard->firstKey));
    parse.Advance(parse.Readf("%#B", &shard->lastKey));
    parse.Advance(parse.Readf("%b", &shard->useBloomFilter));
    parse.Advance(parse.Readf("%c", &shard->storageType));

    if (!parse.ReadLittle32(numChunks))
        return false;
    parse.Advance(4);

    for (i = 0; i < numChunks; i++)
    {
        if (!parse.ReadLittle64(chunkID))
            return false;
        parse.Advance(8);
        
        if (chunkID >= env->nextChunkID)
            env->nextChunkID = chunkID + 1;
        
        fileChunk = env->GetFileChunk(chunkID);

        if (fileChunk == NULL)
        {
            fileChunk = new StorageFileChunk;

            fileChunk->SetFilename(env->chunkPath, chunkID);
            fileChunk->written = true;
            
            fileChunk->ReadHeaderPage();
            
            env->fileChunks.Append(fileChunk);
        }
        
        shard->chunks.Add(fileChunk);
    }

    env->shards.Append(shardGuard.Release());
    
    return true;
}

void StorageRecovery::CreateMemoChunks()
{
    StorageShard* it;

    FOREACH (it, env->shards)
        it->memoChunk = new StorageMemoChunk(env->nextChunkID++, it->UseBloomFilter());
}

void StorageRecovery::ComputeShardRecovery()
{
    StorageShard*       itShard;
    StorageChunk**      itChunk;
    StorageFileChunk*   fileChunk;

    FOREACH (itShard, env->shards)
    {
        FOREACH (itChunk, itShard->chunks)
        {
            fileChunk = (StorageFileChunk*) (*itChunk);

            if (fileChunk->GetMaxLogSegmentID() > itShard->recoveryLogSegmentID)
            {
                itShard->recoveryLogSegmentID = fileChunk->GetMaxLogSegmentID();
                itShard->recoveryLogCommandID = fileChunk->GetMaxLogCommandID();
            }
            else if (fileChunk->GetMaxLogSegmentID() == itShard->recoveryLogSegmentID)
            {
                if (fileChunk->GetMaxLogCommandID() > itShard->recoveryLogCommandID)
                    itShard->recoveryLogCommandID = fileChunk->GetMaxLogCommandID();
            }
        }
    }
}

void StorageRecovery::ReplayLogSegments(uint64_t trackID)
{
    const char*         filename;
    Buffer              tmp, prefix;
    FS_Dir              dir;
    FS_DirEntry         entry;
    SortedList<Buffer*> segments;
    Buffer*             segmentName;
    Buffer**            itSegment;
    
    Log_Message("Replaying log segments in track %U...", trackID);
    
    tmp.Write(env->logPath);
    tmp.NullTerminate();
    
    dir = FS_OpenDir(tmp.GetBuffer());
    if (dir == FS_INVALID_DIR)
    {
        Log_Message("Unable to open log directory: %s", tmp.GetBuffer());
        STOP_FAIL(1);
    }

    prefix.Writef("log.%020U", trackID);
    prefix.NullTerminate();
    
    while ((entry = FS_ReadDir(dir)) != FS_INVALID_DIR_ENTRY)
    {
        filename = FS_DirEntryName(entry);
        
        if (FS_IsSpecial(filename))
            continue;
            
        if (FS_IsDirectory(filename))
            continue;
        
        tmp.Write(filename);
        if (ReadBuffer(tmp).BeginsWith(prefix.GetBuffer()))
        {
            tmp.Write(env->logPath);
            tmp.Append(filename);
            segmentName = new Buffer(tmp);
            segments.Add(segmentName);
        }
    }

    FOREACH (itSegment, segments)
    {
        segmentName = *itSegment;
        ReplayLogSegment(trackID, *segmentName);
        delete segmentName;

        TryWriteChunks();
    }
    
    FS_CloseDir(dir);
    
    Log_Message("Replaying done.");
}

ReadBuffer StorageRecovery::ReadFromFileBuffer(FD fd, uint64_t len)
{
    ssize_t     ret;
    ReadBuffer  rb;

    if (fileBuffer.GetLength() == 0 || fileBuffer.GetLength() - fileBufferPos < len)
    {
        // realign buffer
        fileBuffer.Write(fileBuffer.GetBuffer() + fileBufferPos, fileBuffer.GetLength() - fileBufferPos);

        if (len + fileBuffer.GetLength() > fileBuffer.GetSize())
            fileBuffer.Allocate(len + fileBuffer.GetLength(), /*keepold*/ true);

        ret = FS_FileRead(fd, fileBuffer.GetPosition(), fileBuffer.GetRemaining());
        if (ret <= 0) // end of file or error
            return ReadBuffer();

        fileBuffer.Lengthen(ret);
        fileBufferPos = 0;
    }

    rb.Wrap(fileBuffer.GetBuffer() + fileBufferPos, len);
    fileBufferPos += len;

    return rb;
}

//bool StorageRecovery::ReplayLogSegment(uint64_t trackID, Buffer& filename)
//{
//    // create a StorageLogSegment for each
//    // and for each (logSegmentID, commandID) => (contextID, shardID)
//    // look at that shard's computed max., if the log is bigger, then execute the command
//    // against the MemoChunk
//
//    bool                r, usePrevious;
//    char                type;
//    uint16_t            contextID, klen;
//    uint32_t            checksum, /*compChecksum,*/ vlen, version;
//    uint64_t            logSegmentID, tmp, shardID, logCommandID, size, rest;
//    ReadBuffer          parse, dataPart, key, value;
//    Buffer              buffer;
//    FDGuard             fd;
//    StorageLogSegment*  logSegment;
//    uint64_t            uncompressedLength;
//    
//    Log_Message("Replaying log segment %B...", &filename);
//    
//    filename.NullTerminate();
//    
//    if (fd.Open(filename.GetBuffer(), FS_READONLY) == INVALID_FD)
//    {
//        Log_Message("Unable to open log file: %s", filename.GetBuffer());
//        STOP_FAIL(1);
//    }
//    
//    fileBuffer.Allocate(STORAGE_RECOVERY_PRELOAD_SIZE);
//    fileBuffer.Clear();
//    fileBufferPos = 0;
//    size = 4 + 8;
//
//    parse = ReadFromFileBuffer(fd.GetFD(), size);
//    if (parse.GetLength() != size)
//        return false;
//
//    // first 4 byte is the version
//    if (!parse.ReadLittle32(version))
//        return false;
//    parse.Advance(4);
//        
//    // next 8 byte is the logSegmentID
//    if (!parse.ReadLittle64(logSegmentID))
//        return false;
//    parse.Advance(8);
//    
//    logCommandID = 1;
//
//    while (true)
//    {
//        // read header that contains the size of the file
//        size = sizeof(uint64_t);
//        parse = ReadFromFileBuffer(fd.GetFD(), size);
//        if (parse.GetLength() != size)
//            break;
//
//        // parse actual file size
//        if (!parse.ReadLittle64(size))
//            break;
//
//        rest = size - parse.GetLength();
//        parse = ReadFromFileBuffer(fd.GetFD(), rest);
//        if (parse.GetLength() != rest)
//            break;
//
//        if (!parse.ReadLittle64(uncompressedLength))
//            break;
//        parse.Advance(sizeof(uint64_t));
//        
//        if (!parse.ReadLittle32(checksum))
//            break;
//        parse.Advance(sizeof(uint32_t));
//
//        while (parse.GetLength() > 0)
//        {            
//            if (parse.GetLength() < 1)
//                break;
//            parse.ReadChar(type);
//            parse.Advance(1);
//            
//            if (parse.GetLength() < 1)
//                break;
//            parse.Readf("%b", &usePrevious);
//            parse.Advance(1);
//            
//            if (!usePrevious)
//            {
//                if (parse.GetLength() < 2)
//                    break;
//                parse.ReadLittle16(contextID);
//                parse.Advance(2);
//
//                if (parse.GetLength() < 8)
//                    break;
//                parse.ReadLittle64(shardID);
//                parse.Advance(8);
//            }
//            
//            if (parse.GetLength() < 2)
//                break;
//            if (!parse.ReadLittle16(klen))
//                break;
//            parse.Advance(2);
//            
//            if (parse.GetLength() < klen)
//                break;
//            key.Wrap(parse.GetBuffer(), klen);
//            parse.Advance(klen);
//
//            if (type == STORAGE_LOGSEGMENT_COMMAND_SET)
//            {
//                if (parse.GetLength() < 4)
//                    break;
//                if (!parse.ReadLittle32(vlen))
//                    break;
//                parse.Advance(4);
//                
//                if (parse.GetLength() < vlen)
//                    break;
//                value.Wrap(parse.GetBuffer(), vlen);
//                parse.Advance(vlen);
//            }
//            
//            if (type == STORAGE_LOGSEGMENT_COMMAND_SET)
//                ExecuteSet(logSegmentID, logCommandID, contextID, shardID, key, value);
//            else if (type == STORAGE_LOGSEGMENT_COMMAND_DELETE)
//                ExecuteDelete(logSegmentID, logCommandID, contextID, shardID, key);
//            else
//                ASSERT_FAIL();
//            
//            logCommandID++;
//        }
//    }
//    
//    logSegment = new StorageLogSegment;
//    logSegment->trackID = trackID;
//    logSegment->logSegmentID = logSegmentID;
//    logSegment->filename.Write(filename);
//    env->logSegments.Append(logSegment);
//    r = env->logSegmentIDMap.Get(trackID, tmp);
//    if (!r || tmp <= logSegmentID)
//        env->logSegmentIDMap.Set(trackID, ++logSegmentID);
//    
//    fileBuffer.Reset();
//    fd.Close();
//    
//    return true;
//}

bool StorageRecovery::ReplayLogSegment(uint64_t trackID, Buffer& filename)
{
    // create a StorageLogSegment for each
    // and for each (logSegmentID, commandID) => (contextID, shardID)
    // look at that shard's computed max., if the log is bigger, then execute the command
    // against the MemoChunk

    bool                usePrevious;
    char                type;
    uint16_t            contextID, klen;
    uint32_t            checksum, /*compChecksum,*/ vlen, version;
    uint64_t            logSegmentID, shardID, logCommandID, size, rest;
    ReadBuffer          parse, dataPart, key, value;
    Buffer              buffer;
    FDGuard             fd;
    uint64_t            uncompressedLength;
    ssize_t             ret;
    StorageLogManager::Track*  track;
    
    Log_Message("Replaying log segment %B...", &filename);
    
    filename.NullTerminate();
    
    if (fd.Open(filename.GetBuffer(), FS_READONLY) == INVALID_FD)
    {
        Log_Message("Unable to open log file: %s", filename.GetBuffer());
        STOP_FAIL(1);
    }
    
    size = 4 + 8;
    buffer.Allocate(size);
    ret = FS_FileRead(fd.GetFD(), buffer.GetBuffer(), size);
    if (ret < 0 || (uint64_t) ret != size)
        return false;
    buffer.SetLength(size);
    parse.Wrap(buffer);

    // first 4 byte is the version
    if (!parse.ReadLittle32(version))
        return false;
    parse.Advance(4);
        
    // next 8 byte is the logSegmentID
    if (!parse.ReadLittle64(logSegmentID))
        return false;
    parse.Advance(8);
    
    logCommandID = 1;

    buffer.Allocate(1024 * 1024);

    while (true)
    {
        // read header that contains the size of the file
        size = sizeof(uint64_t);
        buffer.Allocate(size);
        ret = FS_FileRead(fd.GetFD(), buffer.GetBuffer(), size);
        if (ret < 0 || (uint64_t) ret != size)
            break;
        buffer.SetLength(size);
        
        // parse actual file size
        parse.Wrap(buffer);
        if (!parse.ReadLittle64(size))
            break;
        buffer.Allocate(size);
        
        rest = size - buffer.GetLength();
        if (rest < sizeof(uint64_t) + sizeof(uint32_t)) // size of uncompressed + checksum
            break;
        ret = FS_FileRead(fd.GetFD(), buffer.GetPosition(), rest);
        if (ret < 0 || (uint64_t) ret != rest)
            break;
        buffer.SetLength(size);
     
        parse.Wrap(buffer.GetBuffer() + sizeof(uint64_t), sizeof(uint64_t) + sizeof(uint32_t));
        if (!parse.ReadLittle64(uncompressedLength))
            break;
        parse.Advance(sizeof(uint64_t));
        
        if (!parse.ReadLittle32(checksum))
            break;
        dataPart.Wrap(buffer.GetBuffer() + STORAGE_LOGSEGMENT_BLOCK_HEAD_SIZE,
         buffer.GetLength() - STORAGE_LOGSEGMENT_BLOCK_HEAD_SIZE);

//        compChecksum = dataPart.GetChecksum();
//        if (checksum != compChecksum)
//            break;

        parse = dataPart;
        while (parse.GetLength() > 0)
        {            
            if (parse.GetLength() < 1)
                break;
            parse.ReadChar(type);
            parse.Advance(1);
            
            if (parse.GetLength() < 1)
                break;
            parse.Readf("%b", &usePrevious);
            parse.Advance(1);
            
            if (!usePrevious)
            {
                if (parse.GetLength() < 2)
                    break;
                parse.ReadLittle16(contextID);
                parse.Advance(2);

                if (parse.GetLength() < 8)
                    break;
                parse.ReadLittle64(shardID);
                parse.Advance(8);
            }
            
            if (parse.GetLength() < 2)
                break;
            if (!parse.ReadLittle16(klen))
                break;
            parse.Advance(2);
            
            if (parse.GetLength() < klen)
                break;
            key.Wrap(parse.GetBuffer(), klen);
            parse.Advance(klen);

            ASSERT(key.GetLength() > 0);
            if (type == STORAGE_LOGSEGMENT_COMMAND_SET)
            {
                if (parse.GetLength() < 4)
                    break;
                if (!parse.ReadLittle32(vlen))
                    break;
                parse.Advance(4);
                
                if (parse.GetLength() < vlen)
                    break;
                value.Wrap(parse.GetBuffer(), vlen);
                parse.Advance(vlen);
            }
            
            if (type == STORAGE_LOGSEGMENT_COMMAND_SET)
                ExecuteSet(logSegmentID, logCommandID, contextID, shardID, key, value);
            else if (type == STORAGE_LOGSEGMENT_COMMAND_DELETE)
                ExecuteDelete(logSegmentID, logCommandID, contextID, shardID, key);
            else
                ASSERT_FAIL();
            
            logCommandID++;
        }
    }
    
    track = env->logManager.GetTrack(trackID);
    if (!track)
        env->logManager.CreateTrack(trackID);
    env->logManager.Add(trackID, logSegmentID, filename);
    
    fd.Close();
    
    return true;
}

void StorageRecovery::DeleteOrphanedChunks()
{
    bool                found;
    uint64_t            chunkID;
    const char*         filename;
    Buffer              tmp;
    FS_Dir              dir;
    FS_DirEntry         entry;
    StorageFileChunk*   itChunk;
    
    Log_Debug("Checking for orphaned chunks...");
    
    tmp.Write(env->chunkPath);
    tmp.NullTerminate();
    
    dir = FS_OpenDir(tmp.GetBuffer());
    if (dir == FS_INVALID_DIR)
    {
        Log_Message("Unable to open chunk directory: %s", tmp.GetBuffer());
        STOP_FAIL(1);
    }
    
    while ((entry = FS_ReadDir(dir)) != FS_INVALID_DIR_ENTRY)
    {
        filename = FS_DirEntryName(entry);
        
        if (FS_IsSpecial(filename))
            continue;
            
        if (FS_IsDirectory(filename))
            continue;
        
        if (ReadBuffer(filename).BeginsWith("chunk."))
        {
            ReadBuffer(filename).Readf("chunk.%U", &chunkID);
            found = false;
            FOREACH (itChunk, env->fileChunks)
            {
                if (itChunk->GetChunkID() == chunkID)
                {
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                tmp.Write(env->chunkPath);
                tmp.Append(filename);
                Log_Debug("Deleting orphaned chunk file %B...", &tmp);
                tmp.NullTerminate();
                FS_Delete(tmp.GetBuffer());
            }
        }
    }

    Log_Message("Checking done.");

    FS_CloseDir(dir);
}

void StorageRecovery::ExecuteSet(
                         uint64_t logSegmentID, uint32_t logCommandID,
                         uint16_t contextID, uint64_t shardID,
                         ReadBuffer& key, ReadBuffer& value)
{
    StorageShard*       shard;
    StorageMemoChunk*   memoChunk;

    shard  = env->GetShard(contextID, shardID);
    if (shard == NULL)
        return; // shard was deleted

    // shard was split and key now belongs to another shard
    if (!shard->RangeContains(key))
    {
        shard = env->GetShardByKey(contextID, shard->tableID, key);
        if (shard == NULL)
            return;
    }
    
    //if (shard->logSegmentID > logSegmentID)
    //    return; // shard was deleted and re-created
    //
    //if (shard->logSegmentID == logSegmentID && shard->logCommandID > logCommandID)
    //    return; // shard was deleted and re-created 

    if (shard->recoveryLogSegmentID > logSegmentID)
        return; // this command is already present in a file chunk

    if (shard->recoveryLogSegmentID == logSegmentID && shard->recoveryLogCommandID >= logCommandID)
        return; // this command is already present in a file chunk

    memoChunk = shard->GetMemoChunk();
    ASSERT(memoChunk != NULL);
    if (!memoChunk->Set(key, value))
        ASSERT_FAIL();

    if (shard->GetStorageType() == STORAGE_SHARD_TYPE_LOG)
    {
        // remove old entries from the head of the log if its size exceeds chunkSize and we don't want filechunks
        while (memoChunk->GetSize() > env->GetConfig().chunkSize && env->config.numLogSegmentFileChunks == 0)
            memoChunk->RemoveFirst();
    }

    memoChunk->RegisterLogCommand(logSegmentID, logCommandID);
}

void StorageRecovery::ExecuteDelete(
                         uint64_t logSegmentID, uint32_t logCommandID,
                         uint16_t contextID, uint64_t shardID,
                         ReadBuffer& key)
{
    StorageShard*       shard;
    StorageMemoChunk*   memoChunk;
    
    shard  = env->GetShard(contextID, shardID);
    if (shard == NULL)
        return; // shard was deleted

    // shard was split and key now belongs to another shard
    if (!shard->RangeContains(key))
    {
        shard = env->GetShardByKey(contextID, shard->tableID, key);
        if (shard == NULL)
            return;
    }

    if (shard->GetStorageType() == STORAGE_SHARD_TYPE_LOG)
        ASSERT_FAIL();
    
    //if (shard->logSegmentID > logSegmentID)
    //    return; // shard was deleted and re-created
    //
    //if (shard->logSegmentID == logSegmentID && shard->logCommandID > logCommandID)
    //    return; // shard was deleted and re-created 

    if (shard->recoveryLogSegmentID > logSegmentID)
        return; // this command is already present in a file chunk

    if (shard->recoveryLogSegmentID == logSegmentID && shard->recoveryLogCommandID >= logCommandID)
        return; // this command is already present in a file chunk
        
    memoChunk = shard->GetMemoChunk();
    ASSERT(memoChunk != NULL);
    if (!memoChunk->Delete(key))
        ASSERT_FAIL();

    memoChunk->RegisterLogCommand(logSegmentID, logCommandID);
}

void StorageRecovery::TryWriteChunks()
{
    StorageShard*           shard;
    StorageMemoChunk*       memoChunk;
    StorageFileChunk*       fileChunk;
    StorageChunkSerializer  serializer;
    StorageChunkWriter      writer;
    Stopwatch               sw;
    bool                    ret;    

    // mtrencseni:
    // this is terrible code, but we're on a schedule

    FOREACH (shard, env->shards)
    {
        if (shard->GetStorageType() == STORAGE_SHARD_TYPE_LOG && env->config.numLogSegmentFileChunks == 0)
            continue; // never serialize log storage shards
        
        memoChunk = shard->GetMemoChunk();
        
        if (memoChunk->GetSize() > env->config.chunkSize)
        {
            Log_Debug("Serializing chunk %U, size: %s", memoChunk->GetChunkID(),
                HUMAN_BYTES(memoChunk->GetSize()));

            shard->PushMemoChunk(new StorageMemoChunk(env->nextChunkID++, shard->UseBloomFilter()));

            // from StorageSerializeChunkJob::Execute()
            Log_Debug("Serializing chunk %U in memory...", memoChunk->GetChunkID());
            sw.Start();
            ret = serializer.Serialize(env, memoChunk);
            ASSERT(ret);
            sw.Stop();
            Log_Debug("Done serializing, elapsed: %U", (uint64_t) sw.Elapsed());

            // from StorageEnvironment::OnChunkSerialize()
            fileChunk = memoChunk->RemoveFileChunk();
            ASSERT(fileChunk);
            env->OnChunkSerialized(memoChunk, fileChunk);
            env->fileChunks.Append(fileChunk);

            delete memoChunk;
            memoChunk = NULL;

            // from StorageWriteChunkJob::Execute()
            Log_Debug("Writing chunk %U to file...", fileChunk->GetChunkID());
            sw.Start();
            ret = writer.Write(env, fileChunk);
            sw.Stop();

            if (fileChunk->writeError)
            {
                // write failed
                Log_Message("Unable to write chunk file %U to disk.", fileChunk->GetChunkID());
                Log_Message("Free disk space: %s", HUMAN_BYTES(FS_FreeDiskSpace(fileChunk->GetFilename().GetBuffer())));
                Log_Message("This should not happen.");
                Log_Message("Possible causes: not enough disk space, software bug...");
                STOP_FAIL(1);
            }

            Log_Debug("Chunk %U written, elapsed: %U, size: %s, bps: %sB/s",
             fileChunk->GetChunkID(),
             (uint64_t) sw.Elapsed(), HUMAN_BYTES(fileChunk->GetSize()), 
             HUMAN_BYTES((uint64_t)(fileChunk->GetSize() / (sw.Elapsed() / 1000.0))));

            // from StorageEnvironment::OnChunkWrite()
            fileChunk->written = true;    
            fileChunk->AddPagesToCache();
            env->WriteTOC();
        }
    }
}

static size_t Hash(uint64_t h)
{
    return h;
}


