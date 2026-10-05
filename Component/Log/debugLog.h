#pragma once

//#include "../../CommonDef.h"
#include <Windows.h>
#include <cstdarg>
#include <assert.h>
#include <debugapi.h>
#include <stdio.h>

void StartTextLogThread();
void MyDbgStr(LPCSTR lpszFormat, ...);
void EndTextLogThread();


