/*****************************************************************************
 * Plugin for CP/M hard disk drives PK8000.                                  *
 * Copyright (C) 2017 Andrey Hlus                                            *
 *****************************************************************************/
#pragma pack (4)
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <winioctl.h>

#include "resource.h"
#include "cpmplg.h"
#include "log.h"
#include "cpmhdd.h"
#include "config.h"



tProgressProc      ProgressProc;
tLogProc           LogProc;
tRequestProc       RequestProc;
int PluginNumber;


HINSTANCE hDll;

BOOL bLogEnable;
BOOL bFixedEnable;
BOOL bRemovableEnable;



char *MakePath(char *szFullPath, char *ext)
{
    char drive[_MAX_DRIVE];
    char dir[_MAX_DIR];
    char fname[_MAX_FNAME];
    char *dest;

    dest = malloc(strlen(szFullPath)+strlen(ext)+1);
    _splitpath(szFullPath, drive, dir, fname, NULL);
    strcpy(dest, drive);
    strcat(dest, dir);
    strcat(dest, fname);
    strcat(dest, ext);
    return dest;
}


BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{

    switch(reason)
    {
        case DLL_PROCESS_ATTACH:
            hDll = hModule;
//            InitPlugin();
            break;

        case DLL_PROCESS_DETACH:
            DonePlugin();
            break;
    }
    return TRUE;
}

int mount_Images()
{
    int  nDisk = 0;
    HANDLE Handle;
    char *listkey, *key;
    char fname[_MAX_FNAME];
    char emufile[MAX_PATH];
    int  count;

    listkey = ini_GetImagesKey();
    key = listkey;
    while ((key) && (strlen(key) > 0))
    {
        count = ini_GetImagePath(key, emufile);
        if (count)
        {
            _splitpath(emufile, NULL, NULL, fname, NULL);
            if ((Handle = CreateFile(emufile, GENERIC_READ+GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL)) != INVALID_HANDLE_VALUE)
            {
                log_Print("*mount file: \"%s\"\n", emufile);
                if (ide_AppendDevice(Handle, fname))
                {
                    nDisk++;
                } else {
                    CloseHandle(Handle);
                }
            }
        }
        key = key + strlen(key) + 1;
    }
    free(listkey);
    return nDisk;
}


int mount_Disks()
{
    HANDLE hDevice;
    char   name[32];
    int    n = 0;
    int    nDisk = 0;
    BOOL   bResult;

    DISK_GEOMETRY pdg;
    DWORD  nReads;

    STORAGE_DEVICE_DESCRIPTOR *sdd;
    STORAGE_PROPERTY_QUERY query;
    STORAGE_DESCRIPTOR_HEADER deschdr;

    for (n = 0; n < 16; n++)
    {
        sprintf(name, "\\\\.\\PhysicalDrive%u", n);
        hDevice = CreateFile(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (hDevice != INVALID_HANDLE_VALUE)
        {
            // получаем тип носителя
            memset(&pdg, 0, sizeof(pdg));
            bResult = DeviceIoControl(hDevice, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, (LPVOID) &pdg, sizeof(pdg), &nReads, NULL);
            if (bResult)
            {
                if ( ((pdg.MediaType == FixedMedia) && bFixedEnable) ||
                     ((pdg.MediaType == RemovableMedia) && bRemovableEnable) )
                {
                    // Узнаем, сколько байт нужно для выходного буфера
                    memset(&query, 0, sizeof(query));
                    query.PropertyId = StorageDeviceProperty;
                    query.QueryType = PropertyStandardQuery;
                    if ((bResult = DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &deschdr, sizeof(deschdr), &nReads, NULL)))
                    {
                        // Получаем параметры физического диска
                        sdd = (STORAGE_DEVICE_DESCRIPTOR *) malloc(deschdr.Size);
                        if ((bResult = DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), sdd, deschdr.Size, &nReads, NULL)))
                        {
                            if (sdd->ProductIdOffset)
                            {
                                log_Print("*mount disk, model: \"%s\"\n", (char *) sdd+sdd->ProductIdOffset);
                                if ((bResult = ide_AppendDevice(hDevice, (char *) sdd+sdd->ProductIdOffset)))
                                    nDisk++;
                            }
                        }
                    }
                }
            }
            if (!bResult)
                CloseHandle(hDevice);
        }
    }
    return nDisk;
}


void InitPlugin()
{
    char szFullPath[MAX_PATH];
    char *szIniFile;
    char *szLogFile;

    // получаем путь к файлу настроек
    GetModuleFileName(hDll, szFullPath, MAX_PATH);
    szLogFile = MakePath(szFullPath, ".log");
    szIniFile = MakePath(szFullPath, ".ini");
    // получаем настройки плагина
    ini_Init(szIniFile);
    ini_GetConfig(&bLogEnable, FALSE, &bFixedEnable, FALSE, &bRemovableEnable, FALSE);
    if (bLogEnable)
        log_Init(szLogFile);
    free(szIniFile);
    free(szLogFile);
    // монтируем имиджи и диски
    mount_Images();
    if (bFixedEnable || bRemovableEnable)
        mount_Disks();
}

void DonePlugin()
{
    ide_Done();
    log_Done();
}

__declspec(dllexport) int __stdcall FsInit(int PluginNr,tProgressProc pProgressProc,tLogProc pLogProc,tRequestProc pRequestProc)
{
    ProgressProc=pProgressProc;
    LogProc=pLogProc;
    RequestProc=pRequestProc;
    PluginNumber=PluginNr;
    InitPlugin();
    return 0;
}

__declspec(dllexport) HANDLE __stdcall FsFindFirst(char* Path,WIN32_FIND_DATA *FindData)   //FindFirstFile
{
    memset(FindData, 0, sizeof(WIN32_FIND_DATA));
    return plg_FindFirst(Path, FindData);
}


__declspec(dllexport) BOOL __stdcall FsFindNext (HANDLE Hdl,WIN32_FIND_DATA *FindData)
{
    return plg_FindNext(Hdl, FindData);
}

__declspec(dllexport) int __stdcall FsFindClose(HANDLE Hdl)
{
    void *lf;

    lf = (void *) Hdl;
    if (lf != NULL)
        free(lf);
    return 0;
}

__declspec(dllexport) void __stdcall FsGetDefRootName(char* DefRootName,int maxlen)
{
    strlcpy(DefRootName,"CP/M HDD",maxlen);
}

__declspec(dllexport) BOOL __stdcall FsDeleteFile(char* RemoteName)
{
    return plg_DeleteFile(RemoteName);
}

__declspec(dllexport) int __stdcall FsRenMovFile(char* OldName,char* NewName,BOOL Move,BOOL OverWrite,RemoteInfoStruct* ri)
{
    return plg_RenMovFile(OldName, NewName, Move, OverWrite, ri);
}

__declspec(dllexport) int __stdcall FsPutFile(char* LocalName,char* RemoteName,int CopyFlags)
{
    return plg_PutFile(LocalName, RemoteName, CopyFlags);
}

__declspec(dllexport) int __stdcall FsGetFile(char* RemoteName,char* LocalName,int CopyFlags,RemoteInfoStruct* ri)
{
    return plg_GetFile(RemoteName, LocalName, CopyFlags, ri);
}


__declspec(dllexport) int __stdcall FsExecuteFile(HWND MainWin,char* RemoteName,char* Verb)
{
    HWND hDlg;

    if (!strcmp(Verb, "properties") && !strcmp(RemoteName, "\\"))
    {
        // свойства плугина
        hDlg = CreateDialog(hDll, MAKEINTRESOURCE(IDD_DIALOG), MainWin, (DLGPROC) dlgProcConfig);
        if (!hDlg)
            log_Print("  *error FsExecuteFile() - can't create dialog with error %u\n", GetLastError());
        return FS_EXEC_OK;
    }
    return FS_EXEC_YOURSELF;
}

