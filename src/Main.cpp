#include "System/Config.h"
#include "Framework/Database/Database.h"
#include "Framework/Replication/ReplicationManager.h"
#include "Application/Controller/ControlConfigContext.h"
#include "Application/Controller/Controller.h"
#include "Application/DataNode/DataNode.h"
#include "Application/HTTP/HttpServer.h"

int main(int argc, char** argv)
{
	DatabaseConfig				dbConfig;
	Database					db;
	ControlConfigContext		configContext;
	ControlChunkContext			chunkContext;
	Controller					controller;
	DataNode					dataNode;
	SingleQuorum				singleQuorum;
	DoubleQuorum				doubleQuorum;
	QuorumDatabase				qdb;
	QuorumTransport				qtransport;
	Buffer						prefix;
	HttpServer					httpServer;
	Endpoint					endpoint;
	uint64_t					nodeID;
	bool						isController;
	const char*					role;
	
	randseed();
	configFile.Init(argv[1]);
	role = configFile.GetValue("role", "data");
	if (strcmp(role, "data") == 0)
		isController = false;
	else
		isController = true;

	Log_SetTarget(LOG_TARGET_STDOUT);
	Log_SetTrace(true);
	Log_SetTimestamping(true);

	IOProcessor::Init(1024);

	dbConfig.dir = configFile.GetValue("database.dir", DATABASE_CONFIG_DIR);
	db.Init(dbConfig);

	if (isController)
	{
		nodeID = configFile.GetIntValue("nodeID", 0);
		RMAN->SetNodeID(nodeID);
		const char* s = configFile.GetListValue("controllers", RMAN->GetNodeID(), NULL);
		endpoint.Set(s);
		RMAN->GetTransport()->Init(RMAN->GetNodeID(), endpoint);
	}
	else
	{
		dataNode.Init(db.GetTable("keyspace")); // sets RMAN->nodeID
		httpServer.Init(configFile.GetIntValue("http.port", 8080));
		httpServer.RegisterHandler(&dataNode);
		const char* s = configFile.GetValue("endpoint", NULL);
		endpoint.Set(s);
		RMAN->GetTransport()->Init(RMAN->GetNodeID(), endpoint);
	}

	unsigned numNodes = configFile.GetListNum("controllers");
	for (unsigned i = 0; i < numNodes; i++)
	{
		const char* s = configFile.GetListValue("controllers", i, NULL);
		endpoint.Set(s);
		singleQuorum.AddNode(i);
		doubleQuorum.AddNode(QUORUM_CONTROL_NODE, i);

		// TODO: i will not be a nodeID once we start using UIDs
		RMAN->GetTransport()->AddEndpoint(i, endpoint);
	}

	if (isController)
	{
		qdb.Init(db.GetTable("keyspace"));
		prefix.Write("0");
		qtransport.SetPriority(true);
		qtransport.SetPrefix(prefix);
		qtransport.SetQuorum(&singleQuorum);
		
		configContext.SetContextID(0);
		configContext.SetController(&controller);
		configContext.SetQuorum(singleQuorum);
		configContext.SetDatabase(qdb);
		configContext.SetTransport(qtransport);
		configContext.Start();
		
		chunkContext.SetContextID(1);
		chunkContext.SetQuorum(doubleQuorum);
		chunkContext.SetDatabase(qdb); // TODO: hack
		chunkContext.SetTransport(qtransport);
		chunkContext.Start();

		controller.Init(db.GetTable("keyspace"));
		controller.SetConfigContext(&configContext);
		controller.SetChunkContext(&chunkContext);
		RMAN->GetTransport()->SetController(&controller); // TODO: hack
		RMAN->AddContext(&configContext);

		httpServer.Init(configFile.GetIntValue("http.port", 8080));
		httpServer.RegisterHandler(&controller);
	}
	
	
	EventLoop::Init();
	EventLoop::Run();
	EventLoop::Shutdown();
}
