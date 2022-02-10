#pragma once
#include <windef.h>

#ifndef _DEBUG

#define ASSERT( expr ) ( (void)0 )

#else//_DEBUG
int __cdecl AssertFail(char szErr[], char szFileName[], int nLine, char szMessage[]);
#define MDebugBreak() __asm { int 3 }
#define ASSERT(f) \
		do {\
			if(!(f))\
			{\
				if(AssertFail(#f, __FILE__, __LINE__,NULL))\
				{\
					MDebugBreak();\
				}\
			}\
		} while(0)

#endif//_DEBUG 

void ERRORM(BOOL b,char *format, ...);

#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <io.h>
#include <stdio.h>
#include "..\BGraph\array.h"

#define MAXSTRLEN 1024