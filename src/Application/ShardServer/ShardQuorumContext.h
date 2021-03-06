#ifndef SHARDQUORUMCONTEXT_H
#define SHARDQUORUMCONTEXT_H

#include "System/Containers/InQueue.h"
#include "Framework/Replication/Quorums/QuorumContext.h"
#include "Framework/Replication/Quorums/TotalQuorum.h"
#include "Framework/Replication/ReplicatedLog/ReplicatedLog.h"
#include "Framework/Replication/PaxosLease/PaxosLease.h"
#include "Application/ConfigState/ConfigQuorum.h"
#include "ShardMessage.h"

class ShardQuorumProcessor; // forward

/*
===============================================================================================

 ShardQuorumContext

===============================================================================================
*/

class ShardQuorumContext : public QuorumContext
{
public:
    void                            Init(ConfigQuorum* configQuorum,
                                     ShardQuorumProcessor* quorumProcessor_);
    void                            Shutdown();
    
    void                            SetQuorumNodes(SortedList<uint64_t>& activeNodes);
    void                            RestartReplication();
    void                            TryReplicationCatchup();
    void                            AppendDummy();
    void                            Append(); // nextValue was filled up using GetNextValue()
    bool                            IsAppending();
    void                            OnAppendComplete();
    void                            WriteReplicationState();
    
    uint64_t                        GetMemoryUsage();

    // ========================================================================================
    // QuorumContext interface:
    //
    virtual bool                    IsLeaseOwner();
    virtual bool                    IsLeaseKnown();
    virtual uint64_t                GetLeaseOwner();
    // multiPaxos
    virtual bool                    IsLeader();

    virtual void                    OnLearnLease();
    virtual void                    OnLeaseTimeout();
    virtual void                    OnIsLeader();

    virtual uint64_t                GetQuorumID();
    virtual void                    SetPaxosID(uint64_t paxosID);
    virtual uint64_t                GetPaxosID();
    virtual uint64_t                GetHighestPaxosID();
    virtual uint64_t                GetLastLearnChosenTime();
    virtual uint64_t                GetReplicationThroughput();

    virtual Quorum*                 GetQuorum();
    virtual QuorumDatabase*         GetDatabase();
    virtual QuorumTransport*        GetTransport();

    virtual void                    OnStartProposing();
    virtual void                    OnAppend(uint64_t paxosID, Buffer& value, bool ownAppend);
    virtual bool                    UseSyncCommit();
    virtual bool                    UseProposeTimeouts();
    virtual bool                    UseCommitChaining();
    virtual bool                    AlwaysUseDatabaseCatchup();
    virtual bool                    IsPaxosBlocked();
    virtual Buffer&                 GetNextValue();
    virtual void                    OnMessage(ReadBuffer msg);
    virtual void                    OnMessageProcessed();
    virtual void                    OnStartCatchup();
    virtual void                    OnCatchupStarted();
    virtual void                    OnCatchupComplete(uint64_t paxosID);

    virtual void                    StopReplication();
    virtual void                    ContinueReplication();

    virtual bool                    IsWaitingOnAppend();

    void                            RegisterPaxosID(uint64_t paxosID);

private:
    void                            OnPaxosMessage(ReadBuffer buffer);
    
    bool                            isReplicationActive;
    uint64_t                        quorumID;
    uint64_t                        highestPaxosID;
    ShardQuorumProcessor*           quorumProcessor;
    TotalQuorum                     quorum;
    QuorumDatabase                  database;
    QuorumTransport                 transport;
    ReplicatedLog                   replicatedLog;
    Buffer                          nextValue;

    unsigned                        numPendingPaxos;
    InQueue<Buffer>                 paxosMessageQueue;
};

#endif
