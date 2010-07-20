#ifndef TCPSERVER_H
#define TCPSERVER_H

#include "System/Events/Callable.h"
#include "System/IO/Socket.h"
#include "System/IO/IOOperation.h"
#include "System/IO/IOProcessor.h"
#include "System/Containers/InQueue.h"
#include "System/Containers/InList.h"
#include "TCPConnection.h"

/*
===============================================================================

 TCPServer

 TCPServer is a template class for listening for incoming TCP connections
 on a port. TCPServer will create and manage connections given by template
 parameter Conn.

===============================================================================
*/

template<class T, class Conn>
class TCPServer
{
public:
	TCPServer();
	~TCPServer();
	
	bool					Init(int port, int backlog = 3);
	void					Close();
	void					DeleteConn(Conn* conn);

protected:
	void					OnConnect();
	Conn*					GetConn();
	void					InitConn(Conn* conn);

	
	TCPRead					tcpread;
	Socket					listener;
	int						backlog;
	InList<Conn>			activeConns;
	InQueue<Conn>			inactiveConns;
};

/*
===============================================================================
*/

template<class T, class Conn>
TCPServer<T, Conn>::TCPServer()
{
}

template<class T, class Conn>
TCPServer<T, Conn>::~TCPServer()
{
	Conn* conn;
	while ((conn = inactiveConns.Dequeue()) != NULL)
		delete conn;
}

template<class T, class Conn>
bool TCPServer<T, Conn>::Init(int port_, int backlog_)
{
	bool ret;
	
	Log_Trace();
	backlog = backlog_;

	ret = listener.Create(Socket::TCP);
	if (!ret)
		return false;
	ret = listener.Listen(port_);
	if (!ret)
		return false;	
	ret = listener.SetNonblocking();
	if (!ret)
		return false;

	tcpread.Listen(listener.fd, MFUNC(TCPServer, OnConnect));
	return IOProcessor::Add(&tcpread);	
}

template<class T, class Conn>
void TCPServer<T, Conn>::Close()
{
	Conn* it;
	Conn* next;

	for (it = activeConns.Head(); it != NULL; /* advanced in body */)
	{
		next = activeConns.Next(it);
		it->OnClose();
		it = next;
	}

	listener.Close();
}

template<class T, class Conn>
void TCPServer<T, Conn>::DeleteConn(Conn* conn)
{
	Log_Trace();
	
	numActive--;
	assert(numActive >= 0);
	
	activeConns.Remove(conn);

	if (inactiveConns.GetLength() >= backlog)
		delete conn;
	else
		inactiveConns.Enqueue(conn);
}

template<class T, class Conn>
void TCPServer<T, Conn>::OnConnect()
{
	T* pT = static_cast<T*>(this);
	Conn* conn = pT->GetConn();
	numActive++;
	if (listener.Accept(&(conn->GetSocket())))
	{
		conn->GetSocket().SetNonblocking();
		conn->GetSocket().SetNodelay();
		activeConns.Append(conn);
		pT->InitConn(conn);
	}
	else
	{
		Log_Trace("Accept() failed");
		pT->DeleteConn(conn);
	}
	
	IOProcessor::Add(&tcpread);		
}

template<class T, class Conn>
Conn* TCPServer<T, Conn>::GetConn()
{
	if (inactiveConns.GetLength() > 0)
		return inactiveConns.Dequeue();
	
	return new Conn;
}

template<class T, class Conn>
void TCPServer<T, Conn>::InitConn(Conn* conn)
{
	T* pT = static_cast<T*>(this);
	conn->Init(pT);
}

#endif
