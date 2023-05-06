#ifndef _CPMHDD_H_
#define _CPMHDD_H_


typedef unsigned char       UINT8;
typedef unsigned short int  UINT16;
//typedef unsigned long int   UINT32;

typedef signed char         INT8;
typedef signed short int    INT16;
//typedef signed long int     INT32;

void ide_Done();


// эмуляция подключения физических устройств
BOOL ide_AppendDevice(HANDLE Handle, char *model);


// поиск
HANDLE plg_FindFirst(char* Path, WIN32_FIND_DATA *FindData);
BOOL   plg_FindNext(HANDLE Hdl, WIN32_FIND_DATA *FindData);
BOOL   plg_DeleteFile(char* RemoteName);
int    plg_RenMovFile(char* OldName,char* NewName, BOOL Move, BOOL OverWrite,RemoteInfoStruct* ri);
int    plg_PutFile(char* LocalName,char* RemoteName,int CopyFlags);
int    plg_GetFile(char* RemoteName,char* LocalName,int CopyFlags,RemoteInfoStruct* ri);


#endif

