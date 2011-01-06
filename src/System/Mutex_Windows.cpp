#ifdef PLATFORM_WINDOWS
#include "Mutex.h"
#include "Macros.h"

#include <windows.h>

// This is here to ensure that the mutex type has enough space to 
// hold a Windows CRITICAL_SECTION object
STATIC_ASSERT(sizeof(CRITICAL_SECTION) == CRITICAL_SECTION_BUFFER_SIZE, 
    "CRITICAL_SECTION_BUFFER_SIZE must be equal to sizeof(CRITICAL_SECTION)");

Mutex::Mutex()
{
    InitializeCriticalSection((CRITICAL_SECTION*) &mutex);
}

Mutex::~Mutex()
{
    DeleteCriticalSection((CRITICAL_SECTION*) &mutex);
}

void Mutex::Lock()
{
    EnterCriticalSection((CRITICAL_SECTION*) &mutex);
}

void Mutex::Unlock()
{
    LeaveCriticalSection((CRITICAL_SECTION*) &mutex);
}

#endif