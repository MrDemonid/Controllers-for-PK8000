#include <windows.h>
#include <stdio.h>
#include <io.h>
#include <stdlib.h>
#include <conio.h>
#include <string.h>
//#include <dos.h>

#define VERSION        "1.6"

#define CPM_TYPE        0x02        // тип раздела CP/M
#define MAX_DIR         0x10        // максимальное количество кластеров под директорию

#define MAX_MODEL_NAME  16          // макс. длина модели устройства


typedef unsigned char       UINT8;
typedef unsigned short int  UINT16;
//typedef unsigned long int   UINT32;

typedef signed char         INT8;
typedef signed short int    INT16;
//typedef signed long int     INT32;


#pragma pack (1)

typedef struct {
    UINT8   Active;     // 0x80 - active partion
    UINT8   Side;       // head
    UINT16  Addr;       // cylinder/sector
    UINT8   Type;       // type partion
    UINT8   SideEnd;
    UINT16  AddrEnd;
    UINT32  RelAddr;    // относительный линейный адрес
    UINT32  Size;       // размер раздела в секторах
} PARTION;


typedef struct _dev {
    HANDLE      handle;                 // хэндл диска
    char        Name[MAX_PATH];         // полное имя устройства
    char        Model[MAX_MODEL_NAME];  // модель
    // ULONGLONG   Size;                   // in sectors
} DEVICE;


typedef struct {
    UINT16  SPT;        // sectors per track
    UINT8   BSH;        // block shift
    UINT8   BLM;        // block mask
    UINT8   EXM;        // extent mask
    UINT16  DSM;        // disk maximum
    UINT16  DRM;        // directory maximum
    UINT8   AL0;        // allocation vector
    UINT8   AL1;
    UINT16  CKS;        // checksum vector size
    UINT16  OFF;        // track offset
    UINT8   res;
} DPB;

typedef struct {
    char    Sign[8];    // сигнатура "CP/M"
    DPB     dpb;
    UINT8   res[486];   // резерв
    UINT16  parSign;    // 0xAA55;
} SYSSEC;

typedef struct {
    char    user;
    char    name[8];
    char    ext[3];
    char    ex;
    UINT16  res;
    char    rc;
    UINT16  map[8];
} DIRREC;

typedef struct {
    int     lastDisk;
    UINT32  AbsAddr[32];
} DISKS;


#define DIRINSEC        (512 / sizeof(DIRREC))

/*
==============================================================================

                                 DISK IO

==============================================================================
*/

char ide_ReadSector(DEVICE *p, ULONGLONG Sector, void* buff)
{
    DWORD nReads;

    ULONGLONG AbsAddr = Sector * 512;
    long      loAddr = AbsAddr & 0xFFFFFFFF;
    long      hiAddr = AbsAddr >> 32;

    if (SetFilePointer(p->handle, loAddr, &hiAddr, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
        return 0;
    if (!ReadFile(p->handle, buff, 512, &nReads, NULL))
        return 0;
    return -1;
}

char ide_WriteSector(DEVICE *p, ULONGLONG Sector, void* buff)
{
    DWORD nWritten;

    ULONGLONG AbsAddr = Sector * 512;
    long      loAddr = AbsAddr & 0xFFFFFFFF;
    long      hiAddr = AbsAddr >> 32;

    if (SetFilePointer(p->handle, loAddr, &hiAddr, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
        return 0;
    if (!WriteFile(p->handle, buff, 512, &nWritten, NULL))
        return 0;
    return -1;
}





/*
==============================================================================

                                    HDD

==============================================================================
*/

char hdd_CheckSign(unsigned char buff[])
{
    if ((buff[0x1FE] != 0x55) || (buff[0x1FF] != 0xAA))
        return 0;
    else
        return -1;
}


//
// сканирование SMBR на предмет логических дисков CP/M
//  relAdd - относительный линейный адрес начала раздела SMBR
//
void hdd_ParseSMBR(DEVICE *p, ULONGLONG relAddr, DISKS *dsk)
{
    PARTION    *par;
    PARTION    *nxt;
    UINT8       buff[512];
    ULONGLONG   base = relAddr;     // абсолютный адрес начала SMBR

    do
    {
        // подгружаем SMBR
        if (!ide_ReadSector(p, relAddr, &buff))
        {
            printf("    *error* - can't read SMBR at 0x%12I64X!\n", relAddr); //printf("    *error* - can't read SMBR at 0x%08lX!\n", relAddr);
            return;
        }
        if (!hdd_CheckSign(buff))
        {
            printf("    *error* - SMBR at 0x%12I64X is corrupt!\n", relAddr); //printf("    *error* - SMBR at 0x%08lX is corrupt!\n", relAddr);
            return;
        }
        par = (PARTION *) &buff[0x1BE];
        nxt = (PARTION *) &buff[0x1CE];
        if (par->Type == CPM_TYPE)
        {
            dsk->lastDisk++;
            dsk->AbsAddr[dsk->lastDisk] = par->RelAddr+relAddr;
            printf("    -found CP/M disk [%c]   size: %12lu Kb\n", (dsk->lastDisk+'A'), par->Size / 2);
        }
        relAddr = nxt->RelAddr+base;
    } while (nxt->Type != 0);
}


//
// ищет диски CP/M во всех расширенных разделах dos
//
char hdd_FindDisks(DEVICE *p, DISKS *dsk)
{
    PARTION    *par;
    UINT8       buff[512];
    int         i;

    // подгружаем MBR
    if (!ide_ReadSector(p, 0, &buff))
    {
        printf("  *error* - can't read MBR!\n");
        return 0;
    }
    if (!hdd_CheckSign(buff))
    {
        printf("  *error* - MBR is corrupt!\n");
        return 0;
    }

    dsk->lastDisk = -1;
    // сканируем партиции в MBR
    par = (PARTION *) &buff[0x1BE];
    for(i = 0; i < 4; i++)
    {
        if (par->Active)
        {
            printf("    -skip primary partion  size: %9luKb\n", par->Size / 2);
        } else {
            if ((par->Type == 0x05) || (par->Type == 0x0C) || (par->Type == 0xF))
            {
                hdd_ParseSMBR(p, par->RelAddr, dsk);
            }
        }
        par++;
    }
    return -1;
}



/*
==============================================================================

                                   COPY

==============================================================================
*/


// структура текущего логического диска CP/M
ULONGLONG   StartSector;        // начальный сектор диска
UINT16      NumBlock;           // размер диска в кластерах
UINT16      BlockSize;          // размер кластера
char        BlockMap[1024];     // карта свободных кластеров
char        DirMap[1024];       // карта директорных записей
UINT16      NumDir;             // макс. количество записей в директории


// помечает кластер занятым
void disk_SetBlock(UINT16 nBlock)
{
    BlockMap[nBlock / 8] |= (1 << (nBlock % 8));
}

// помечает кластер свободным
void disk_FreeBlock(UINT16 nBlock)
{
    BlockMap[nBlock / 8] &= (~(1 << (nBlock % 8)));
}

// возвращает статус кластера: 0 - свободен, >0 - занят
char disk_CheckBlock(UINT16 nBlock)
{
    return BlockMap[nBlock / 8] & (1 << (nBlock % 8));
}

// помечает директорную запись занятой
void disk_SetDir(UINT16 nDir)
{
    DirMap[nDir/8] |= (1 << (nDir % 8));
}

// помечает директорную запись свободной
void disk_FreeDir(UINT16 nDir)
{
    DirMap[nDir/8] &= (~(1 << (nDir % 8)));
}

// возвращает статус директорной записи: 0 - свободна, >0 - занята
char disk_CheckDir(UINT16 nDir)
{
    return DirMap[nDir/8] & (1 << (nDir % 8));
}


//
// составляет карты занятости блоков и директорных записей
//
char disk_ScanDir(DEVICE *p)
{
    UINT16  i, j, k;
    DIRREC  dir[DIRINSEC];

    memset(&BlockMap, 0, sizeof(BlockMap));
    memset(&DirMap, 0, sizeof(DirMap));
    // помечаем блоки директорий
    for (i = 0; i < NumDir / (BlockSize/sizeof(DIRREC)); i++)
        disk_SetBlock(i);
    // сканируем директорий и помечаем блоки файлов
    for (i = 0; i < ((NumDir+(DIRINSEC-1)) / DIRINSEC); i++)
    {
        if (!ide_ReadSector(p, StartSector + i, &dir))
        {
            printf("    *error* - can't read directory sector at 0x%12I64X\n", StartSector+i); //printf("    *error* - can't read directory sector at 0x%08lX\n", StartSector+i);
            return 0;
        }
        for (j = 0; j < DIRINSEC; j++)          // цикл по записям
        {
            if (dir[j].user != 0xE5)
            {
                disk_SetDir(i*DIRINSEC+j);
                for (k = 0; k < 8; k++)         // цикл по карте блоков записи
                    disk_SetBlock(dir[j].map[k]);
            }
        }
    }
    return -1;
}


//
// "подключаем" диск CP/M для дальнейшей работы с ним
//
char disk_Mount(DEVICE *p, ULONGLONG AbsSec)
{
    SYSSEC  sec;

    // считываем блок параметров диска
    if (!ide_ReadSector(p, AbsSec, &sec))
    {
        printf("  *error* - can't read sector at 0x%12I64X!\n", AbsSec); //printf("  *error* - can't read sector at 0x%08lX!\n", AbsSec);
        return 0;
    }
    if ((!hdd_CheckSign((char *) &sec)) || (memcmp(sec.Sign, "CP/M    ", 8) != 0))
    {
        printf("  *error* - disk is not CP/M!\n");
        return 0;
    }
    NumBlock  = sec.dpb.DSM + 1;
    BlockSize = (sec.dpb.BLM + 1) * 128;
    NumDir    = sec.dpb.DRM + 1;
    StartSector = ((sec.dpb.OFF*sec.dpb.SPT)*128) / 512 + AbsSec + 1;
    // составляем карты занятости блоков и директорий
    return disk_ScanDir(p);
}


//
// конвертирует полное имя файла в формат, используемый на диске CP/M (11 bytes)
//
void disk_FrmName(char *filename, char *dskname)
{
    char drive[_MAX_DRIVE];
    char dir[_MAX_DIR];
    char fname[_MAX_FNAME];
    char ext[_MAX_EXT];
    char *e;

    memset(dskname, 0x20, 11);
    _splitpath(filename, drive, dir, fname, ext);
    strupr(fname);
    strupr(ext);
        e = &ext[0];
    if (*e == '.')
        e++;
    memcpy(dskname, fname, strlen(fname));
    memcpy(dskname+8, e, strlen(e));
}

char *disk_GetPath(char *fullname)
{
    char drive[_MAX_DRIVE];
    char dir[_MAX_DIR];
    char fname[_MAX_FNAME];
    char ext[_MAX_EXT];
    char *m;
    _splitpath(fullname, drive, dir, fname, ext);
    m = (char *) malloc(strlen(drive) + strlen(dir)+sizeof(char));
    if (m)
    {
        strcpy(m, drive);
        strcat(m, dir);
    }
    return m;
}



//
// выделение nBlocks свободных блоков директорным записям
//
char disk_AllocBlock(DIRREC *dir, UINT16 nBlocks)
{
    DIRREC *r;
    UINT16  i, j, k;
    UINT16  nDirs;

    r = dir;
    nDirs = ((nBlocks + 7) / 8);
    for (i = 0; i < nDirs; i++)             // для каждой директорной записям
    {
        for (j = 0; j < 8; j++)             // для каждого элемента массива ном. блоков директорной записям
        {
            r->map[j] = 0;
            if (!nBlocks)
                continue;
            // сканируем карту свободных блоков
            for (k = 0; k < NumBlock; k++)
            {
                if (!disk_CheckBlock(k))
                {
                    // блок свободен
                    disk_SetBlock(k);
                    r->map[j] = k;
                    nBlocks--;
                    break;
                }
            } // k
        } // j
        r++;
    }
    if (nBlocks)
        return 0;
    return -1;
}

//
// выделяет место в директории диска
// и создает массив из nDirs директорных записей
//
DIRREC *disk_AllocDir(char *name, UINT16 nDirs)
{
    UINT16   i, j, f;
    DIRREC  *dir, *r;

    dir = (DIRREC *) malloc(nDirs*sizeof(DIRREC));
    if (dir == NULL)
        return NULL;
    r = dir;
    for (i = 0; i < nDirs; i++)             // для каждой директорной записи
    {
        memset(r, 0, sizeof(DIRREC));
        disk_FrmName(name, &r->name[0]);
        f = 0;
        for (j = 0; j < NumDir; j++)
        {
            if (!disk_CheckDir(j))
            {
                disk_SetDir(j);
                r->res = j;                 // временно запоминаем номер записи
                f++;
                break;
            }
        }   // for j
        if (!f)
        {
            free(dir);                      // свободные директории кончились
            return NULL;
        }
        r++;
    }
    return dir;
}


//
// запись одного кластера на диск
// возвращает количество действительно записанных байт
//
UINT16 disk_WriteBlock(DEVICE *p, FILE *src, UINT16 nBlock)
{
    UINT32  i;
    UINT32  SecInBlock;
    char    buff[512];
    UINT16  total;
    ULONGLONG n;

    total = 0;
    SecInBlock = BlockSize / 512;
    for (i = 0; i < SecInBlock; i++)
    {
        memset(buff, 0xE5, sizeof(buff));
        total += fread(&buff, 1, 512, src);
        n = StartSector + (nBlock * SecInBlock) + i;
        if (!ide_WriteSector(p, n, &buff))
            break;
    }
    return total;
}


//
// запись одной директорной записи на диск
//
char disk_WriteDir(DEVICE *p, UINT16 nDir, DIRREC *dir)
{
    DIRREC  buff[DIRINSEC];
    DIRREC *r;
    UINT16  Rec;
    ULONGLONG Sec;

    Sec = StartSector + (nDir / DIRINSEC);
    Rec = nDir % DIRINSEC;
    r = &buff[Rec];
    ide_ReadSector(p, Sec, &buff);
    memcpy(r, dir, sizeof(DIRREC));
    ide_WriteSector(p, Sec, &buff);
    return -1;
}


//
// удаляет файл с диска CP/M
//
char disk_DeleteFile(DEVICE *p, char *name)
{
    UINT16  i, j, f, k;
    DIRREC  dir[DIRINSEC];
    char    fname[14];

    disk_FrmName(name, fname);
    for (i = 0; i < ((NumDir+(DIRINSEC-1)) / DIRINSEC); i++)
    {
        // подгружаем очередной сектор директория
        if (!ide_ReadSector(p, StartSector + i, &dir))
        {
            printf("    *error* - can't read directory sector at 0x%12I64X\n", StartSector+i); //printf("    *error* - can't read directory sector at 0x%08lX\n", StartSector+i);
            return 0;
        }
        f = 0;
        for (j = 0; j < DIRINSEC; j++)          // цикл по записям в текущем секторе
        {
            if (dir[j].user == 0xE5)            // свободная запись?
                continue;
            if (!memcmp(dir[j].name, fname, 11))
            {
                f++;
                dir[j].user = 0xE5;             // освобождаем директорную запись
                disk_FreeDir(i*DIRINSEC+j);
                for (k = 0; k < 8; k++)         // и занимаемые ей блоки
                    if (dir[j].map[k] != 0)
                        disk_FreeBlock(dir[j].map[k]);
            }
        }
        if (f)
            ide_WriteSector(p, StartSector + i, &dir);
    }
    return -1;
}


//
// проверяет наличие файла на диске
//
int disk_IsFilePresent(DEVICE *p, char *name)
{
    UINT16  i, j;
    DIRREC  dir[DIRINSEC];
    char    fname[14];

    disk_FrmName(name, fname);
    for (i = 0; i < ((NumDir+(DIRINSEC-1)) / DIRINSEC); i++)
    {
        // подгружаем очередной сектор директория
        if (!ide_ReadSector(p, StartSector + i, &dir))
        {
            printf("      *error* - can't read directory sector at 0x%12I64X\n", StartSector+i); //printf("      *error* - can't read directory sector at 0x%08lX\n", StartSector+i);
            return 0;
        }
        for (j = 0; j < DIRINSEC; j++)          // цикл по записям
        {
            if (dir[j].user == 0xE5)            // свободная запись?
                continue;
            if (!memcmp(dir[j].name, fname, 11))
                return -1;
        }
    }
    return 0;
}


//
// копирует на диск один файл
//
int disk_Copy(DEVICE *p, char *name, UINT32 size, char rwmode)
{
    DIRREC *dir, *r;
    UINT16  nBlocks, nDirs;
    FILE   *src;
    UINT16  i, j, k, n;
    UINT8   ex;
    UINT32  total;
    char    c;

    nBlocks = (size + BlockSize-1) / BlockSize;
    nDirs = (nBlocks+7) / 8;    // резервируем место на диске под директорные записи

    if (disk_IsFilePresent(p, name))
    {
        if (rwmode)
        {
            printf("    -overwrite file %s\n", name);
            disk_DeleteFile(p, name);
        } else {
            // файл уже существует
            printf("    -file %s is present! overwrite (y/n)? ", name);
            fflush(stdout);
            c = getch();
            if ((c == 'y') || (c == 'Y'))
            {
                printf("\r                                                                               \r");
                disk_DeleteFile(p, name);
            } else {
                printf("\r                                                                               \r");
                printf("    -skip: %s\n", name);
                return -1;
            }
        }
    }
    if ( (dir = disk_AllocDir(name, nDirs)) == NULL)
    {
        printf("    -skip: %s - not enought directory space\n", name);
        return 0;
    }
    if (!disk_AllocBlock(dir, nBlocks))
    {
        free(dir);
        printf("    -skip: %s - not enought disk space\n", name);
        return 0;
    }
    // открываем файл для чтения
    if (( src = fopen(name, "rb")) == NULL)
    {
        free(dir);
        printf("    -skip: %s - can't open\n", name);
        return 0;
    }
    // копируем файл на диск
    printf("    -copy: %-42s %12lu bytes\n", name, size);
    r = dir;
    ex = 0;
    total = 0;
    for (i = 0; i < nDirs; i++)
    {
        for (j = 0; j < 8; j++)
        {
            if (r->map[j] != 0)
            {
                k = disk_WriteBlock(p, src, r->map[j]);
                total += k;
                if (total > 16384)
                {
                    total -= 16384;
                    ex += 1;
                }
            }
        }
        r->ex = ex;
        r->rc = (total+127) / 128;
        n = r->res;
        r->res = 0;
        disk_WriteDir(p, n, r);
        r++;
    }

    fclose(src);
    free(dir);
    return -1;
}


//
// ищет файлы по маске и отправляет на диск
//
int disk_CopyFiles(DEVICE *p, char *files, char rwmode)
{
    struct _finddata_t  fileinfo;
    long  handle;
    int   rc;
    char *path;
    char *fname;
    int   result;

    path = disk_GetPath(files);
    fname = (char *) malloc(strlen(path) + _MAX_FNAME + _MAX_EXT + sizeof(char));
    if ((!path) || (!fname))
    {
        printf("  *error* - not enought memory!\n");
        return 0;
    }
    handle = _findfirst(files, &fileinfo );
    rc = handle;
    result = -1;
    while( rc != -1 )
    {
        if (!(fileinfo.attrib & _A_SUBDIR))
        {
            strcpy(fname, path);
            strcat(fname, fileinfo.name);
            if (!disk_Copy(p, fname, fileinfo.size, rwmode))
            {
                result = 0;
                // корректируем возможные ошибки
                disk_DeleteFile(p, fname);
                disk_ScanDir(p);
            }

        }
        rc = _findnext(handle, &fileinfo);
    }
    _findclose(handle);
    return result;
}



/*
==============================================================================

                                   MENU

==============================================================================
*/



DEVICE* hdd_Find(char *src)
{
    DEVICE *p;
    int n;

    p = (DEVICE *) malloc(sizeof(DEVICE));
    if (!p)
    {
        printf("    *error* - not enought memory!\n");
        return 0;
    }

    if ( (strlen(src) == 2) && (src[1] == ':') )
    {
        if ( (src[0] >= 'C') && (src[0] <= 'Z') )
        {
            n = src[0] - 'C';
            // на входе имя реального диска
            sprintf(p->Name, "\\\\.\\PhysicalDrive%u", n);
        } else {
            printf("    *error* - drive '%c' not support!\n", src[0]);
            return 0;
        }
    } else {
        strcpy(p->Name, src);
    }

    // на входе имя файла с образом диска
    p->handle = CreateFile(p->Name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (p->handle == INVALID_HANDLE_VALUE)
    {
        printf("    *error [%u]* - can't open drive '%s'\n", GetLastError(), p->Name);
        free(p);
        return 0;
    }
    printf("    -open device: %s\n", p->Name);
    return p;
    }



void do_Usage(void)
{
    printf("Usage: C8000W.EXE [-R] dest disk: file.ext\n");
    printf("  dest     - name physic drive or image file\n");
    printf("  disk:    - CP/M disk in hard disk or image file [A,B,C,D, etc.]\n");
    printf("  file.ext - filename, include mask '?' and '*'\n");
    printf("  -R       - rewrite file\n");
}

char do_argv(int argc, char *argv[], char *DiskName, int *CPMDrive, char *SrcFiles, char *rwmode)
{
    char drv;
    int  par;

    if (argc < 4)
    {
        do_Usage();
        return 0;
    }
    *rwmode = 0;
    par = 1;
    if ((argv[par][0] == '-') || (argv[par][0] == '/'))
    {
        if (strlen(&argv[par][0]) != 2)
        {
            printf("  *error* - request parametr 'r' or 'R'!\n");
            return 0;
        }
        if ((argv[par][1] != 'r') && (argv[par][1] != 'R'))
        {
            printf("  *error* - request parametr 'r' or 'R'!\n");
            return 0;
        }
        *rwmode = -1;
        par++;
    }
    strcpy(DiskName, &argv[par][0]);
    par++;

    if (strlen(&argv[par][0]) != 2)
    {
        printf("  *error* - CP/M drive parametr request!\n");
        return 0;
    }
    strupr(&argv[par][0]);
    drv = argv[par][0];
    if ( (drv < 'A') || (drv > 'Z') || (argv[par][1] != ':') )
        {
            printf("  *error* - unknown parametr '%s'\n", argv[par]);
            return 0;
        }
    *CPMDrive = drv - 'A';

    par++;
    strcpy(SrcFiles, &argv[par][0]);
    return -1;
}

int do_Save(int argc, char *argv[])
{
    DEVICE *p;
    DISKS   dsk;
    char   *SrcFiles;
    char   *DiskName;
    int     CPMDrive;
    char    rwmode;
    int     result;

    while (kbhit()) getch();

    SrcFiles = (char*) malloc(512);
    DiskName = (char*) malloc(_MAX_PATH);
    if ( (SrcFiles == NULL) || (DiskName == NULL) )
    {
        printf("*error* - not enought memory!\n");
        return 0;
    }
    if (!do_argv(argc, argv, DiskName, &CPMDrive, SrcFiles, &rwmode))
    {
        free(SrcFiles);
        return 0;
    }
    if ( !(p = hdd_Find(DiskName)) )
    {
        return 0;
    }
    // сканирование на предмет логических дисков
    memset(&dsk, 0, sizeof(DISKS));
    if (!hdd_FindDisks(p, &dsk))
        return 0;
    if (dsk.lastDisk < CPMDrive)
    {
        printf("*error* - CP/M disk [%c] not found!\n", CPMDrive+'A');
        return 0;
    }
    // копируем файл[ы]
    if (!disk_Mount(p, dsk.AbsAddr[CPMDrive]))
        return 0;

    result = disk_CopyFiles(p, SrcFiles, rwmode);
    free(SrcFiles);
    free(DiskName);
    while (kbhit()) getch();
    return result;
}


int main(int argc, char *argv[])
{
    int result;

    printf("Copy files to CP/M hard disk or image file for PK8000.\tver %s\n", VERSION);
    result = do_Save(argc, argv);
    printf("Bye!\n");

    // возвращаем ERRORLEVEL
    if (! result)
        return 1;
    return 0;
}






