/*****************************************************************************
 * Plugin for CP/M hard disk drives PK8000.                                  *
 * Copyright (C) 2017 Andrey Hlus                                            *
 *****************************************************************************/
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#include "log.h"


FILE *LogFile = NULL;


void log_Init(char *filename)
{
    SYSTEMTIME time;

    LogFile = fopen(filename, "at");
    if (LogFile == NULL)
    {
        return;
    }
    GetLocalTime(&time);
    log_Print("\n=== Open log at %hu/%hu/%hu, %hu:%hu:%hu ===\n", time.wDay, time.wMonth, time.wYear, time.wHour, time.wMinute, time.wSecond);
}

void log_Done()
{
    SYSTEMTIME time;
    if (LogFile != NULL)
    {
        GetLocalTime(&time);
        log_Print("Close log at %hu/%hu/%hu, %hu:%hu:%hu\n", time.wDay, time.wMonth, time.wYear, time.wHour, time.wMinute, time.wSecond);
        fclose(LogFile);
        LogFile = NULL;
    }
}

void log_Print(char *format, ...)
{
    va_list arglist;

    va_start( arglist, format);
    if (LogFile != NULL) {
        va_start(arglist, format);
        vfprintf(LogFile, format, arglist);
        va_end(arglist);
    }
}
