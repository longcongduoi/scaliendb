#ifndef CONTROLLERAPP_H
#define CONTROLLERAPP_H

#include "Controller.h"
#include "HTTPControllerContext.h"
#include "Application/HTTP/HTTPServer.h"

/*
===============================================================================================

 ControllerApp

===============================================================================================
*/

class ControllerApp
{
public:
	void					Init();

private:
	Controller				controller;
	HTTPServer				httpServer;
	HTTPControllerContext	httpContext;
};

#endif
