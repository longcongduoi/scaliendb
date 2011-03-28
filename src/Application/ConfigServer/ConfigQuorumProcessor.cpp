#include "ConfigQuorumProcessor.h"
#include "Application/Common/ContextTransport.h"
#include "Framework/Replication/ReplicationConfig.h"
#include "ConfigServer.h"
#include "ConfigHeartbeatManager.h"

void ConfigQuorumProcessor::Init(ConfigServer* configServer_, unsigned numConfigServers,
 StorageShardProxy* quorumPaxosShard, StorageShardProxy* quorumLogShard)
{
    configServer = configServer_;
    quorumContext.Init(this, numConfigServers, quorumPaxosShard, quorumLogShard);
    
    CONTEXT_TRANSPORT->AddQuorumContext(&quorumContext);
    
    isCatchingUp = false;
    configStateChecksum = 0;
    lastConfigChangeTime = 0;
}

void ConfigQuorumProcessor::Shutdown()
{
    configMessages.DeleteList();
    requests.DeleteList();
    listenRequests.DeleteList();
}

bool ConfigQuorumProcessor::IsMaster()
{
    return quorumContext.IsLeaseOwner();
}

int64_t ConfigQuorumProcessor::GetMaster()
{
    if (!quorumContext.IsLeaseKnown())
        return -1;

    return (int64_t) quorumContext.GetLeaseOwner();
}

uint64_t ConfigQuorumProcessor::GetQuorumID()
{
    return quorumContext.GetQuorumID();
}

uint64_t ConfigQuorumProcessor::GetPaxosID()
{
    return quorumContext.GetPaxosID();
}

void ConfigQuorumProcessor::TryAppend()
{
    assert(configMessages.GetLength() > 0);
    
    if (!quorumContext.IsAppending())
        quorumContext.Append(configMessages.First());
}

void ConfigQuorumProcessor::OnClientRequest(ClientRequest* request)
{
    uint64_t        srcNodeID, dstNodeID;
    uint64_t*       itNodeID;
    ConfigState*    configState;
    ConfigShard*    configShard;
    ConfigQuorum*   configQuorum;
    ConfigMessage*  configMessage;
    ClusterMessage  clusterMessage;

    configState = configServer->GetDatabaseManager()->GetConfigState();

    if (request->type == CLIENTREQUEST_GET_MASTER)
    {
        if (quorumContext.IsLeaseKnown())
            request->response.Number(quorumContext.GetLeaseOwner());
        else
            request->response.NoService();
            
        request->OnComplete();
        return;
    }
    else if (request->type == CLIENTREQUEST_GET_CONFIG_STATE)
    {
        listenRequests.Append(request);
        request->response.ConfigStateResponse(*configState);
        request->OnComplete(false);
        return;
    }
    
    if (!quorumContext.IsLeader())
    {
        request->response.NoService();
        request->OnComplete();
        return;
    }
    
    if (request->type == CLIENTREQUEST_CREATE_QUORUM)
    {
        // make sure all nodes are currently active
        FOREACH(itNodeID, request->nodes)
        {
            if (!configServer->GetHeartbeatManager()->HasHeartbeat(*itNodeID))
            {
                request->response.Failed();
                request->OnComplete();
                return;
            }
        }
    }
    
    if (request->type == CLIENTREQUEST_ACTIVATE_NODE)
    {
        configServer->GetActivationManager()->TryActivateShardServer(request->nodeID);
        request->response.OK();
        request->OnComplete();
        return;
    }

    if (request->type == CLIENTREQUEST_MIGRATE_SHARD)
    {
        if (configState->isMigrating)
            goto MigrationFailed;
        configShard = configState->GetShard(request->shardID);
        if (!configShard)
            goto MigrationFailed;
        configQuorum = configState->GetQuorum(configShard->quorumID);
        if (!configQuorum)
            goto MigrationFailed;
        if (!configQuorum->hasPrimary)
            goto MigrationFailed;
        srcNodeID = configQuorum->primaryID;
        configQuorum = configState->GetQuorum(request->quorumID);
        if (!configQuorum)
            goto MigrationFailed;
        if (!configQuorum->hasPrimary)
            goto MigrationFailed;
        dstNodeID = configQuorum->primaryID;

        configState->isMigrating = true;
        configState->migrateQuorumID = request->quorumID;
        configState->migrateShardID = request->shardID;
        
        UpdateListeners();

        clusterMessage.ShardMigrationInitiate(
         dstNodeID, request->quorumID, request->shardID);
        CONTEXT_TRANSPORT->SendClusterMessage(srcNodeID, clusterMessage);

        request->response.OK();
        request->OnComplete();
        return;

        MigrationFailed:
        {
            request->response.Failed();
            request->OnComplete();
            return; 
        }
    }
    
    configMessage = new ConfigMessage;
    ConstructMessage(request, configMessage);
    
    if (!configState->CompleteMessage(*configMessage))
    {
        delete configMessage;
        request->response.Failed();
        request->OnComplete();
        return;
    }
    
    requests.Append(request);
    configMessages.Append(configMessage);
    TryAppend();
}

void ConfigQuorumProcessor::OnClientClose(ClientSession* session)
{
    ClientRequest*  it;
    ClientRequest*  next;
    
    // TODO: list iteration is not necessary, because it is an InList
    for (it = listenRequests.First(); it != NULL; it = next)
    {
        next = listenRequests.Next(it);
        if (it->session == session)
        {
            listenRequests.Remove(it);
            it->response.NoResponse();
            it->OnComplete();
            break;
        }
    }
}

bool ConfigQuorumProcessor::HasActivateMessage(uint64_t quorumID, uint64_t nodeID)
{
    ConfigMessage *it;
    
    FOREACH(it, configMessages)
    {
        if (it->type == CONFIGMESSAGE_ACTIVATE_SHARDSERVER &&
         it->quorumID == quorumID && it->nodeID == nodeID)
        {
            return true;
        }
    }
    
    return false;
}

bool ConfigQuorumProcessor::HasDeactivateMessage(uint64_t quorumID)
{
    ConfigMessage *it;
    
    FOREACH(it, configMessages)
    {
        if (it->type == CONFIGMESSAGE_DEACTIVATE_SHARDSERVER &&
         it->quorumID == quorumID)
        {
            return true;
        }
    }
    
    return false;
}

void ConfigQuorumProcessor::ActivateNode(uint64_t quorumID, uint64_t nodeID)
{
    ConfigMessage* message;
 
    if (HasActivateMessage(quorumID, nodeID))
        return;
          
    message = new ConfigMessage();
    message->fromClient = false;
    message->ActivateShardServer(quorumID, nodeID);
    configMessages.Append(message);
    
    TryAppend();
}

void ConfigQuorumProcessor::DeactivateNode(uint64_t quorumID, uint64_t nodeID)
{
    ConfigMessage* message;

    if (HasDeactivateMessage(quorumID))
        return;
    
    message = new ConfigMessage();
    message->fromClient = false;
    message->DeactivateShardServer(quorumID, nodeID);
    configMessages.Append(message);
    
    TryAppend();
}

void ConfigQuorumProcessor::TryRegisterShardServer(Endpoint& endpoint)
{
    ConfigMessage*  it;
    ConfigMessage*  msg;

    FOREACH(it, configMessages)
    {
        if (it->type == CONFIGMESSAGE_REGISTER_SHARDSERVER && it->endpoint == endpoint)
            return;
    }

    msg = new ConfigMessage;
    msg->fromClient = false;
    msg->RegisterShardServer(0, endpoint);
    if (!configServer->GetDatabaseManager()->GetConfigState()->CompleteMessage(*msg))
    {
        delete msg;
        return;        
    }
    configMessages.Append(it);
    TryAppend();
}

void ConfigQuorumProcessor::TryShardSplitBegin(uint64_t shardID, ReadBuffer splitKey)
{
    ConfigMessage*  it;
    ConfigMessage*  msg;

    FOREACH(it, configMessages)
    {
        if (it->type == CONFIGMESSAGE_SPLIT_SHARD_BEGIN && it->shardID == shardID)
            return;
    }

    assert(configServer->GetDatabaseManager()->GetConfigState()->isSplitting == false);
    configServer->GetDatabaseManager()->GetConfigState()->isSplitting = true;

    Log_Message("Split shard process started (parent shardID = %U)...", shardID);

    msg = new ConfigMessage;
    msg->fromClient = false;    
    msg->SplitShardBegin(shardID, splitKey);
    if (!configServer->GetDatabaseManager()->GetConfigState()->CompleteMessage(*msg))
    {
        delete msg;
        return;        
    }
    configMessages.Append(msg);
    TryAppend();
}

void ConfigQuorumProcessor::TryShardSplitComplete(uint64_t shardID)
{
    ConfigMessage*  it;
    ConfigMessage*  msg;

    FOREACH(it, configMessages)
    {
        if (it->type == CONFIGMESSAGE_SPLIT_SHARD_COMPLETE && it->shardID == shardID)
            return;
    }

    Log_Message("Split shard process completed (new shardID = %U)...", shardID);

    msg = new ConfigMessage;
    msg->fromClient = false;    
    msg->SplitShardComplete(shardID);
    if (!configServer->GetDatabaseManager()->GetConfigState()->CompleteMessage(*msg))
    {
        delete msg;
        return;        
    }
    configMessages.Append(msg);
    TryAppend();
}

void ConfigQuorumProcessor::OnShardMigrationComplete(ClusterMessage& message)
{
    ConfigMessage*  configMessage;
    ConfigState*    configState;
    
    if (!quorumContext.IsLeader())
        return;
    
    configState = configServer->GetDatabaseManager()->GetConfigState();

    assert(configState->isMigrating);
    assert(configState->migrateQuorumID == message.quorumID);
    assert(configState->migrateShardID == message.shardID);
    
    configMessage = new ConfigMessage;
    configMessage->fromClient = false;
    configMessage->ShardMigrationComplete(message.quorumID, message.shardID);
    configMessages.Append(configMessage);
    TryAppend();
}

void ConfigQuorumProcessor::UpdateListeners()
{
    ConfigState*                    configState;
    ClientRequest*                  itRequest;
    ConfigShardServer*              itShardServer;
    ClusterMessage                  message;
    Buffer                          checksumBuffer;
//    uint32_t                        checksum;
    bool                            configChanged;
    uint64_t                        now;
    
    configState = configServer->GetDatabaseManager()->GetConfigState();
    
//    // check if the configState changed at all
//    configChanged = false;
//    configState->Write(checksumBuffer, true);
//    checksum = checksumBuffer.GetChecksum();
////    Log_Debug("Config state checksum: %u, prev: %u, %B", checksum, configStateChecksum, &checksumBuffer);
//    if (checksum == 0 || checksum != configStateChecksum)
//    {
////        Log_Debug("Config state changed");
//        configChanged = true;
//        configStateChecksum = checksum;
//    }

    configChanged = true;
    
    // update clients
    now = EventLoop::Now();
    FOREACH (itRequest, listenRequests)
    {
        if (configChanged || itRequest->changeTimeout < now - lastConfigChangeTime)
        {
            itRequest->response.ConfigStateResponse(*configState);
            itRequest->OnComplete(false);
            // TODO: HACK
            if (itRequest->changeTimeout != 0)
                lastConfigChangeTime = now;
        }
    }
    
    // update shard servers
    message.SetConfigState(*configState);
    FOREACH (itShardServer, configState->shardServers)
    {
        CONTEXT_TRANSPORT->SendClusterMessage(itShardServer->nodeID, message);
    }
}

void ConfigQuorumProcessor::OnLearnLease()
{
}

void ConfigQuorumProcessor::OnLeaseTimeout()
{
    ClientRequest*  itRequest;
    ConfigState*    configState;
 
    // clear config messages   
    configMessages.DeleteList();

    // clear client requests
    FOREACH_FIRST (itRequest, requests)
    {
        requests.Remove(itRequest);
        itRequest->response.NoService();
        itRequest->OnComplete();
    }
    assert(requests.GetLength() == 0);

    // clear listen requests
    FOREACH_FIRST (itRequest, listenRequests)
    {
        listenRequests.Remove(itRequest);
        itRequest->response.NoService();
        itRequest->OnComplete();
    }
    assert(listenRequests.GetLength() == 0);
    
    configState = configServer->GetDatabaseManager()->GetConfigState();
    configState->hasMaster = false;
    configState->masterID = 0;
    configServer->OnConfigStateChanged(); // TODO: is this neccesary?
    // TODO: tell ActivationManager
}

void ConfigQuorumProcessor::OnIsLeader()
{
    bool            updateListeners;
    uint64_t        clusterID;
    ConfigState*    configState;
    ConfigMessage*  configMessage;

    configState = configServer->GetDatabaseManager()->GetConfigState();
    
    updateListeners = false;
    if (!configState->hasMaster)
        updateListeners = true;

    configState->hasMaster = true;
    configState->masterID = GetMaster();

    if (updateListeners && (uint64_t) GetMaster() == configServer->GetNodeID())
        UpdateListeners();

    // check cluster ID
    clusterID = REPLICATION_CONFIG->GetClusterID();
    if (clusterID == 0 && configMessages.GetLength() == 0)
    {
        configMessage = new ConfigMessage;
        configMessage->fromClient = false;
        
        clusterID = GenerateGUID();
        configMessage->SetClusterID(clusterID);
        configMessages.Append(configMessage);
        TryAppend();
    }
}

void ConfigQuorumProcessor::OnAppend(uint64_t paxosID, ConfigMessage& message, bool ownAppend)
{
    uint64_t        clusterID;
    ClusterMessage  clusterMessage;
    ConfigState*    configState;
    char            hexbuf[64 + 1];

    configState = configServer->GetDatabaseManager()->GetConfigState();
    
    // catchup issue:
    // if paxosID is smaller or equal to configStatePaxosID, that means
    // our state already includes the writes in this round
    if (paxosID <= configServer->GetDatabaseManager()->GetPaxosID())
        return;

    if (message.type == CONFIGMESSAGE_SHARD_MIGRATION_COMPLETE)
    {
        if (configState->masterID == MY_NODEID)
        {
            assert(configState->isMigrating);
            assert(configState->migrateQuorumID == message.quorumID);
            assert(configState->migrateShardID == message.shardID);
            configState->isMigrating = false;
            configState->migrateQuorumID = 0;
            configState->migrateShardID = 0;
        }
    }
    
    configState->OnMessage(message);
    configServer->GetDatabaseManager()->Write();
    configServer->OnConfigStateChanged(); // UpdateActivationTimeout();

    if (message.type == CONFIGMESSAGE_SET_CLUSTER_ID)
    {
        clusterID = REPLICATION_CONFIG->GetClusterID();
        if (clusterID > 0 && message.clusterID != clusterID)
            STOP_FAIL(1, "Invalid controller configuration!");
        
        UInt64ToBufferWithBase(hexbuf, sizeof(hexbuf), message.clusterID, 64);
        Log_Debug("ClusterID set to %s (%U)", hexbuf, message.clusterID);
        REPLICATION_CONFIG->SetClusterID(message.clusterID);
        CONTEXT_TRANSPORT->SetClusterID(message.clusterID);
        REPLICATION_CONFIG->Commit();
    }
    
    if (message.type == CONFIGMESSAGE_REGISTER_SHARDSERVER)
    {
        // tell ContextTransport that this connection has a new nodeID
        CONTEXT_TRANSPORT->SetConnectionNodeID(message.endpoint, message.nodeID);       
        // tell the shard server
        clusterMessage.SetNodeID(REPLICATION_CONFIG->GetClusterID(), message.nodeID);
        CONTEXT_TRANSPORT->SendClusterMessage(message.nodeID, clusterMessage);
    }
    else if (message.type == CONFIGMESSAGE_SPLIT_SHARD_COMPLETE)
    {
        configServer->GetDatabaseManager()->GetConfigState()->isSplitting = false;
    }
    
    if (IsMaster())
        UpdateListeners();

    if (ownAppend)
    {
        assert(configMessages.GetLength() > 0);
        assert(configMessages.First()->type == message.type);
        if (configMessages.First()->fromClient)
            SendClientResponse(message);
        configMessages.Delete(configMessages.First());
    }
    
    if (configMessages.GetLength() > 0)
        TryAppend();
        
    quorumContext.OnAppendComplete();
}

void ConfigQuorumProcessor::OnStartCatchup()
{
    CatchupMessage msg;
    
    if (isCatchingUp || !quorumContext.IsLeaseKnown())
        return;
    
    quorumContext.StopReplication();
    
    msg.CatchupRequest(configServer->GetNodeID(), quorumContext.GetQuorumID());
    
    CONTEXT_TRANSPORT->SendQuorumMessage(
     quorumContext.GetLeaseOwner(), quorumContext.GetQuorumID(), msg);
     
    isCatchingUp = true;
    
    Log_Message("Catchup started from node %U", quorumContext.GetLeaseOwner());
}

void ConfigQuorumProcessor::OnCatchupMessage(CatchupMessage& imsg)
{
    bool            hasMaster;
    uint64_t        masterID;
    Buffer          buffer;
    ReadBuffer      key;
    ReadBuffer      value;
    CatchupMessage  omsg;
    ConfigState*    configState;
    
    configState = configServer->GetDatabaseManager()->GetConfigState();
    
    switch (imsg.type)
    {
        case CATCHUPMESSAGE_REQUEST:
            if (!quorumContext.IsLeader())
                return;
            assert(imsg.quorumID == quorumContext.GetQuorumID());
            // send configState
            key.Wrap("state");
            configServer->GetDatabaseManager()->GetConfigState()->Write(buffer);
            value.Wrap(buffer);
            omsg.Set(key, value);
            CONTEXT_TRANSPORT->SendQuorumMessage(imsg.nodeID, quorumContext.GetQuorumID(), omsg);
            omsg.Commit(quorumContext.GetPaxosID() - 1);
            // send the paxosID whose value is in the db
            CONTEXT_TRANSPORT->SendQuorumMessage(imsg.nodeID, quorumContext.GetQuorumID(), omsg);
            break;
        case CATCHUPMESSAGE_BEGIN_SHARD:
            ASSERT_FAIL();
            break;
        case CATCHUPMESSAGE_SET:
            if (!isCatchingUp)
                return;
            hasMaster = configState->hasMaster;
            masterID = configState->masterID;
            if (!configState->Read(imsg.value))
                ASSERT_FAIL();
            configState->hasMaster = hasMaster;
            configState->masterID = masterID;
            break;
        case CATCHUPMESSAGE_COMMIT:
            if (!isCatchingUp)
                return;
            configServer->GetDatabaseManager()->SetPaxosID(imsg.paxosID);
            configServer->GetDatabaseManager()->Write();
            quorumContext.OnCatchupComplete(imsg.paxosID);      // this commits
            isCatchingUp = false;
            quorumContext.ContinueReplication();
            Log_Message("Catchup complete");
            break;
        default:
            ASSERT_FAIL();
    }
}

void ConfigQuorumProcessor::ConstructMessage(ClientRequest* request, ConfigMessage* message)
{
    message->fromClient = true;
    
    switch (request->type)
    {
        case CLIENTREQUEST_CREATE_QUORUM:
            message->type = CONFIGMESSAGE_CREATE_QUORUM;
            message->nodes = request->nodes;
            return;
        case CLIENTREQUEST_DELETE_QUORUM:
            message->type = CONFIGMESSAGE_DELETE_QUORUM;
            message->quorumID = request->quorumID;
            return;
        case CLIENTREQUEST_ADD_NODE:
            message->type = CONFIGMESSAGE_ADD_NODE;
            message->quorumID = request->quorumID;
            message->nodeID = request->nodeID;
            return;
        case CLIENTREQUEST_REMOVE_NODE:
            message->type = CONFIGMESSAGE_REMOVE_NODE;
            message->quorumID = request->quorumID;
            message->nodeID = request->nodeID;
            return;
        case CLIENTREQUEST_CREATE_DATABASE:
            message->type = CONFIGMESSAGE_CREATE_DATABASE;
            message->name.Wrap(request->name);
            return;
        case CLIENTREQUEST_RENAME_DATABASE:
            message->type = CONFIGMESSAGE_RENAME_DATABASE;
            message->databaseID = request->databaseID;
            message->name.Wrap(request->name);
            return;
        case CLIENTREQUEST_DELETE_DATABASE:
            message->type = CONFIGMESSAGE_DELETE_DATABASE;
            message->databaseID = request->databaseID;
            return;
        case CLIENTREQUEST_CREATE_TABLE:
            message->type = CONFIGMESSAGE_CREATE_TABLE;
            message->databaseID = request->databaseID;
            message->quorumID = request->quorumID;
            message->name.Wrap(request->name);
            return;
        case CLIENTREQUEST_RENAME_TABLE:
            message->type = CONFIGMESSAGE_RENAME_TABLE;
            message->tableID = request->tableID;
            message->name.Wrap(request->name);
            return;
        case CLIENTREQUEST_DELETE_TABLE:
            message->type = CONFIGMESSAGE_DELETE_TABLE;
            message->tableID = request->tableID;
            return;
        case CLIENTREQUEST_TRUNCATE_TABLE:
            message->type = CONFIGMESSAGE_TRUNCATE_TABLE;
            message->tableID = request->tableID;
            return;
        case CLIENTREQUEST_FREEZE_TABLE:
            message->type = CONFIGMESSAGE_FREEZE_TABLE;
            message->tableID = request->tableID;
            return;
        case CLIENTREQUEST_UNFREEZE_TABLE:
            message->type = CONFIGMESSAGE_UNFREEZE_TABLE;
            message->tableID = request->tableID;
            return;
        case CLIENTREQUEST_SPLIT_SHARD:
            message->type = CONFIGMESSAGE_SPLIT_SHARD_BEGIN;
            message->shardID = request->shardID;
            message->splitKey = request->key;
            return;
        default:
            ASSERT_FAIL();
    }
}

void ConfigQuorumProcessor::ConstructResponse(ConfigMessage* message, ClientResponse* response)
{
    switch (response->request->type)
    {
        case CLIENTREQUEST_CREATE_QUORUM:
            response->Number(message->quorumID);
            return;
        case CLIENTREQUEST_DELETE_QUORUM:
            response->OK();
            return;
        case CLIENTREQUEST_ADD_NODE:
            response->OK();
            return;
        case CLIENTREQUEST_REMOVE_NODE:
            response->OK();
            return;
        case CLIENTREQUEST_CREATE_DATABASE:
            response->Number(message->databaseID);
            return;
        case CLIENTREQUEST_RENAME_DATABASE:
            response->OK();
            return;
        case CLIENTREQUEST_DELETE_DATABASE:
            response->OK();
            return;
        case CLIENTREQUEST_CREATE_TABLE:
            response->Number(message->tableID);
            return;
        case CLIENTREQUEST_RENAME_TABLE:
            response->OK();
            return;
        case CLIENTREQUEST_DELETE_TABLE:
            response->OK();
            return;
        case CLIENTREQUEST_TRUNCATE_TABLE:
            response->OK();
            return;
        case CLIENTREQUEST_FREEZE_TABLE:
            response->OK();
            return;
        case CLIENTREQUEST_UNFREEZE_TABLE:
            response->OK();
            return;
        case CLIENTREQUEST_SPLIT_SHARD:
            response->Number(message->newShardID);
            return;
        default:
            ASSERT_FAIL();
    }
}

void ConfigQuorumProcessor::SendClientResponse(ConfigMessage& message)
{
    ClientRequest* request;
    
    assert(requests.GetLength() > 0);
    
    request = requests.First();
    requests.Remove(request);
    
    ConstructResponse(&message, &request->response);
    request->OnComplete();
}
