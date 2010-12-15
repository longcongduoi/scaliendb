#ifndef STORAGEINDEXPAGE_H
#define STORAGEINDEXPAGE_H

#include "System/Buffers/Buffer.h"
#include "System/Containers/InTreeMap.h"
#include "StoragePage.h"

class StorageFileChunk;

/*
===============================================================================================

 StorageIndexRecord

===============================================================================================
*/

class StorageIndexRecord
{
    typedef InTreeNode<StorageIndexRecord> TreeNode;

public:
    ReadBuffer      key;
    uint32_t        index;
    uint32_t        offset;
    
    TreeNode        treeNode;
};

/*
===============================================================================================

 StorageIndexPage

===============================================================================================
*/

class StorageIndexPage : public StoragePage
{
    typedef InTreeMap<StorageIndexRecord> IndexRecordTree;
public:
    StorageIndexPage(StorageFileChunk* owner);

    uint32_t            GetSize();

    bool                Locate(ReadBuffer& key, uint32_t& index, uint32_t& offset);

    void                Append(ReadBuffer key, uint32_t index, uint32_t offset);
    void                Finalize();

    bool                Read(Buffer& buffer);
    void                Write(Buffer& buffer);

    bool                IsLoaded();
    void                Unload();

private:
    uint32_t            size;
    Buffer              buffer;
    IndexRecordTree     indexTree;
    StorageFileChunk*   owner;
};

#endif
