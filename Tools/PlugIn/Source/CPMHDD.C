/*****************************************************************************
 * Plugin for CP/M hard disk drives PK8000.                                  *
 * Copyright (C) 2017 Andrey Hlus                                            *
 *****************************************************************************/
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#include "cpmplg.h"
#include "log.h"
#include "cpmhdd.h"


#define CPM_TYPE        0x02// тип раздела CP/M
#define MAX_DIR         0x10// максимальное количество кластеров под директорию
#define MAX_BLK         1024// размер массивов под карты директорий и блоков

#define ELEM_MAXNAMELEN 32

#pragma pack (push)
#pragma pack (1)

typedef struct {
    UINT8   Active;         // 0x80 - active partion
    UINT8   Side;           // head
    UINT16  Addr;           // cylinder/sector
    UINT8   Type;           // type partion
    UINT8   SideEnd;
    UINT16  AddrEnd;
    UINT32  RelAddr;        // относительный линейный адрес
    UINT32  Size;           // размер раздела в секторах
} PARTION;


typedef struct {
    UINT16  SPT;            // sectors per track
    UINT8   BSH;            // block shift
    UINT8   BLM;            // block mask
    UINT8   EXM;            // extent mask
    UINT16  DSM;            // disk maximum
    UINT16  DRM;            // directory maximum
    UINT8   AL0;            // allocation vector
    UINT8   AL1;
    UINT16  CKS;            // checksum vector size
    UINT16  OFF;            // track offset
    UINT8   res;
} DPB;

// первый сектор логического диска CP/M
typedef struct {
    char    Sign[8];        // сигнатура "CP/M"
    DPB     dpb;
    UINT8   res[486];       // резерв
    UINT16  parSign;        // 0xAA55;
} SYSSEC;

// запись директория
typedef struct {
    char    user;           // номер пользовательской области (user 0..15)
    char    name[8];        // старшие биты - атрибуты
    char    ext[3];         // старшие биты - атрибуты (ext[0] - R/W, ext[1] - SysFile] )
    char    ex;             // текущий экстент
    UINT16  res;
    char    rc;             // число записей в последнем экстенте
    UINT16  map[8];
} DIRREC;


#define ELEM_DEVICE     1
#define ELEM_DISK       2
#define ELEM_USER       3
#define ELEM_FILE       4

// "дерево"

typedef struct _elem {
    char    type;                   // тип элемента (ELEM_XXXX)
    DWORD   attrib;                 // атрибут - каталог или файл (FILE_ATTRIBUTE_DIRECTORY)
    char    name[ELEM_MAXNAMELEN];  // имя элемента
    struct _elem  *next_elem;       // следующий эл. этого уровня
    struct _elem  *prev_elem;
    struct _elem  *next_lev;        // следующий уровень
    struct _elem  *prev_lev;
} ELEM;

typedef struct {
    ELEM    elem;
    char    Orig[12];       // оригинальное имя на диске
    int     Size;           // размер
} CPMFILE;

// структура пользовательской области
typedef struct {
    ELEM    elem;
    int     user_no;        // номер пользовательской области
} USER;

typedef struct {
    int     size;           // размер карты (в развернутом виде)
    int     free;           // количество свободных элементов
    char    map[];          // битовая карта
} BMAP;

// структура логического диска CP/M
typedef struct {
    ELEM    elem;
    UINT32  AbsAddr;        // абсолютный адрес логического диска
    UINT32  StartSector;    // начальный сектор CP/M диска
    UINT16  NumBlocks;      // размер диска в кластерах
    UINT16  BlockSize;      // размер кластера в байтах
    UINT16  MaxDirRec;      // макс. количество записей в директории
    UINT16  DirBlocks;      // количество отведенных под оглавление блоков
    BMAP   *BlockMap;       // карта свободных кластеров диска
    BMAP   *DirMap;         // карта директорных записей
} DISK;

// физический диск
typedef struct {
    ELEM    elem;
    HANDLE  handle;         // хэндл диска
    int     device_id;      // порядковый номер записи (от 0)
} DEVICE;

typedef struct {
    ELEM   *elem;           // указатель на очередной элемент списка
    char    path[MAX_PATH]; // путь
} LASTFIND;

#define DIRINSEC        (512 / sizeof(DIRREC))

#pragma pack (pop)

DEVICE *Root = NULL;          // корень



DISK *disk_Mount(DEVICE *d, UINT32 absAddr);


//============================================================================
//▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒
//▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒ DISK IO ▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒
//▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒
//============================================================================

char ide_ReadSector(HANDLE handle, UINT32 Sector, void* buff)
{
    DWORD nReads;

    long long int AbsAddr = Sector * 512;
    long loAddr = AbsAddr & 0xFFFFFFFF;
    long hiAddr = AbsAddr >> 32;

    if (SetFilePointer(handle, loAddr, &hiAddr, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
        return 0;
    if (!ReadFile(handle, buff, 512, &nReads, NULL))
        return 0;
    return -1;
}

char ide_WriteSector(HANDLE handle, UINT32 Sector, void* buff)
{
    DWORD nWritten;

    long long int AbsAddr = Sector * 512;
    long loAddr = AbsAddr & 0xFFFFFFFF;
    long hiAddr = AbsAddr >> 32;

    if (SetFilePointer(handle, loAddr, &hiAddr, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
        return 0;
    if (!WriteFile(handle, buff, 512, &nWritten, NULL))
        return 0;
    return -1;
}

/*
проверка буфера на наличие сигнатуры системного сектора
*/
char ide_CheckSign(char buff[])
{
    if ((buff[0x1FE] != 0x55) || (buff[0x1FF] != 0xAA))
        return 0;
    else
        return -1;
}


/*
сканирование SMBR на предмет логических дисков CP/M
на входе:
    relAdd - относительный линейный адрес начала раздела SMBR
на выходе: - количество найденных и смонтированных разделов CP/M
*/
int ide_ParseSMBR(DEVICE *dev, UINT32 relAddr)
{
    PARTION    *part;
    PARTION    *next;
    UINT8       buff[512];
    UINT32      base = relAddr;     // абсолютный адрес начала SMBR
    DISK       *disk;
    int         num_disks;

    num_disks = 0;
    do
    {
        // подгружаем SMBR
        if (!ide_ReadSector(dev->handle, relAddr, &buff))
        {
            log_Print("  *error ide_ParseSMBR(base: 0x%08X) - can't read SMBR at 0x%08X, with code: %u\n", base, relAddr, GetLastError());
            break;
        }
        if (!ide_CheckSign(buff))
            break;
        part = (PARTION *) &buff[0x1BE];
        next = (PARTION *) &buff[0x1CE];
        if (part->Type == CPM_TYPE)
        {
            disk = disk_Mount(dev, part->RelAddr+relAddr);
            if (disk)
            {
                num_disks++;
                log_Print("  -found CP/M disk [%s] at: 0x%08X, size: %u Kb, dirs: %u\n", disk->elem.name, disk->StartSector, ((int)disk->NumBlocks*disk->BlockSize)/1024, disk->MaxDirRec);
            }
        }
        relAddr = next->RelAddr+base;
    } while (next->Type != 0);
    return num_disks;
}



/*
поиск и монитрование дисков CP/M (во всех расширенных разделах dos)
на входе:
    d       - текущий физический диск
    filename- имя файла образа физ. диска
на выходе:  - количество найденных и смонтированных разделов CP/M
*/
int ide_ParseMBR(DEVICE *dev)
{
    PARTION    *par;
    UINT8       buff[512];
    int         i, num_disks;

    // подгружаем MBR
    if (!ide_ReadSector(dev->handle, 0, &buff))
    {
        log_Print("  *error ide_ParseMBR() - can't read sector!\n");
        return 0;
    }
    if (!ide_CheckSign(buff))
        return 0;
    // сканируем партиции в MBR
    num_disks = 0;
    par = (PARTION *) &buff[0x1BE];
    for(i = 0; i < 4; i++)
    {
        if (par->Active)
        {
            log_Print("  -skip primary DOS partion.\n");
        } else {
            if ((par->Type == 0x05) || (par->Type == 0x0C) || (par->Type == 0x0F))
            {
                num_disks += ide_ParseSMBR(dev, par->RelAddr);
            }
        }
        par++;
    }
    return num_disks;
}




/*
эмуляция добавления жесткого диска в список устройств
на входе:
    model   - имя модели "жесткого диска"
    dev_no  - номер физического устройства (PhysicalDrive)
    filename- имя файла с образом винта
на выходе:
*/
BOOL ide_AppendDevice(HANDLE Handle, char *model)
{
    DEVICE *dev, *t;
    char    ch;
    char   *mdl = model;

    // создаем новый элемент DEVICE
    if ((dev = malloc(sizeof(DEVICE))) == NULL)
    {
        log_Print("  *error ide_AppendDevice() - not enought memory!\n");
        return FALSE;
    }
    memset(dev, 0, sizeof(DEVICE));
    dev->elem.type = ELEM_DEVICE;
    dev->elem.attrib = FILE_ATTRIBUTE_DIRECTORY;
    dev->handle = Handle;
    // dev->elem.next_elem = NULL;
    // dev->elem.prev_elem = NULL;
    // dev->elem.next_lev  = NULL;
    // dev->elem.prev_lev  = NULL;
    // ищем CP/M диски
    if (ide_ParseMBR(dev) == 0)
    {
        free(dev);
        return FALSE;
    }
    // добавляем в список
    ch = 0;
    t = Root;
    if (t == NULL)
    {
        Root = dev;
    } else {
        while (t->elem.next_elem != NULL)
        {
            t = (DEVICE *) t->elem.next_elem;
            ch++;
        }
        ch++;
        t->elem.next_elem = (ELEM *) dev;
        dev->elem.prev_elem = (ELEM *) t;
    }
    if ((!model) || (strlen(model) == 0))
        mdl = "Noname";
    dev->device_id = ch;
    sprintf(dev->elem.name, "%u:", ch);
    strncat(dev->elem.name, mdl, ELEM_MAXNAMELEN-(3+1)-1);
    return TRUE;
}


void elem_Delete(void *elem)
{
    ELEM *n;
    ELEM *t = elem;
    while (t)
    {
        n = t;
        if (t->next_lev)
            elem_Delete(t->next_lev);
        t = t->next_elem;
        switch (n->type){
            case ELEM_DEVICE:
                CloseHandle( ((DEVICE *)n)->handle);
                break;
            case ELEM_DISK:
                free(((DISK *)n)->DirMap);
                free(((DISK *)n)->BlockMap);
                break;
            case ELEM_USER:
                break;
            case ELEM_FILE:
                break;
        }
        free(n);
    }
}


void ide_Done()
{
    elem_Delete(Root);
    Root = NULL;
}



//============================================================================
//▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒
//▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒ UTIL ▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒
//▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒
//============================================================================

// помечает кластер/запись занятым
BOOL map_Set(BMAP* map, UINT16 nRec)
{
    UINT16 n = 1 << (nRec % 8);
    if (map)
        if (nRec < map->size)
            if ((map->map[nRec/8] & n) == 0)
            {
                map->map[nRec/8] |= n;
                map->free--;
                return TRUE;
            }
    return FALSE;
}

// помечает кластер/запись свободным
BOOL map_Free(BMAP *map, UINT16 nRec)
{
    UINT16 n = 1 << (nRec % 8);
    if (map)
        if (nRec < map->size)
            if ((map->map[nRec/8] & n) > 0)
            {
                map->map[nRec/8] &= ~n;
                map->free++;
                return TRUE;
            }
    return FALSE;
}
/*
возвращает статус кластера/записи: 0      - свободен
                                   1..80h - занят
                                   -1     - ошибка
*/
char map_Get(BMAP *map, UINT16 nRec)
{
    if (map)
    {
        if (nRec < map->size)
        {
            return map->map[nRec / 8] & (1 << (nRec % 8));
        } else {
            return -1;
        }
    } else {
        return -1;
    }
}

// проверка наличия файла
//
BOOL FileExist(char *LocalName)
{
    FILE *f;
    BOOL b = FALSE;
    if ((f = fopen(LocalName, "r")) != NULL)
    {
        b = TRUE;
        fclose(f);
    }
    return b;
}

// чтение файла в память
// на входе:
//    LocalName - имя файла
// на выходе:
//    nReads    - количество считанных байт
char *FileLoad(char *LocalName, DWORD *nReads)
{
    HANDLE  file;
    char   *buff;
    DWORD   size;
    DWORD  nTemp;

    if ((file = CreateFile(LocalName, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL)) == INVALID_HANDLE_VALUE)
    {
        log_Print("    *error FileLoad() - can`t open file \"%s\"\n", LocalName);
        return NULL;
    }
    size = GetFileSize(file, NULL);
    if ((buff = malloc(size+512*2)) == NULL)
    {
        log_Print("    *error FileLoad() - not enought memory\n");
        return NULL;
    }
    memset(buff, 0, size+512*2);
    ReadFile(file, buff, size, &nTemp, NULL);
    CloseHandle(file);
    if (nReads)
        *nReads = nTemp;
    return buff;
}

// разбивает строку на путь и имя файла
BOOL SplitPath(char *FullPath, char *Path, char *FileName)
{
    int  lendir;
    char dir[_MAX_DIR];
    char fname[_MAX_FNAME];
    char ext[_MAX_EXT];

    _splitpath(FullPath , NULL, dir, fname, ext );
    lendir = strlen(dir);
    if (lendir != 0)
    {
        lendir--;
        if (dir[lendir] == '\\')
            dir[lendir] = '\0';
    }
    strcpy(Path, dir);
    strcpy(FileName, fname);
    if (strlen(fname) > 0)
    {
        strcat(FileName, ext);
        return TRUE;
    }
    return FALSE;
}



// извлекает из имени CP/M файла аттрибуты и
// преобразует их в виндовс-вид
//
DWORD fcpm_GetAttrib(char *cpmname)
{
    DWORD attr = 0;

    if (cpmname[8] & 0x80)
        attr |= FILE_ATTRIBUTE_READONLY;
    if (cpmname[9] & 0x80)
        attr |= FILE_ATTRIBUTE_HIDDEN; //   FILE_ATTRIBUTE_SYSTEM;

    if (attr == 0)
        attr = FILE_ATTRIBUTE_NORMAL;
    return attr;
}

void fcpm_SetAttrib(char *cpmname, DWORD Attr)
{
    // устанавливам аттрибуты
    if (Attr & FILE_ATTRIBUTE_READONLY)
        cpmname[8] |= 0x80;
    if (Attr & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
        cpmname[9] |= 0x80;
}


// преобразование CP/M имени файла в текстовый
//
void fcpm_CPM2Ansi(char *ansi, char *cpm)
{
    int   i;
    char  ch;
    int  id = 0;

    for (i = 0; i < 8; i++)
    {
        ch = cpm[i] & 0x7F;
        if (ch <= ' ')
            break;
        ansi[id++] = ch;
    }
    ansi[id] = '.';
    for (i = 8; i < 11; i++)
    {
        ch = cpm[i] & 0x7F;
        if (ch <= ' ')
            break;
        ansi[++id] = ch;
    }
    if (ansi[id] != '.')
        id++;
    ansi[id] = '\0';
}

/*
преобразует имя файла в формат CP/M
*/
void fcpm_Ansi2CPM(char *cpm, char *ansi)
{
    int     i;
    int     is = 0;
    char    ch;

    for (i = 0; i < 8; i++)
    {
        ch = ansi[is];
        if ((ch > ' ') && (ch != '.'))
        {
            cpm[i] = ch;
            is++;
        } else {
            cpm[i] = ' ';
        }
    }
    if (ansi[is] != '\0')
        is++;
    for (i = 8; i < 11; i++)
    {
        ch = ansi[is];
        if ((ch > ' ') && (ch != '.'))
        {
            cpm[i] = ch;
            is++;
        } else {
            cpm[i] = ' ';
        }
    }
}

/*
сравнение имен двух файлов CP/M
на входе:
    name1, name2 - имена в CP/M-формате: 11 символов без точки
*/
BOOL fcpm_CompareName(char *name1, char *name2)
{
    int i;
    for (i = 0; i < 11; i++)
    {
        if ((*name1++ & 0x7F) != (*name2++ & 0x7F))
            return FALSE;
    }
    return TRUE;
}




/*
возвращает указатель на заданный тип элемента пути
*/
void *elem_Get(char type, void *elem)
{
    ELEM *t = elem;
    while (t)
    {
        if (t->type == type)
            return t;
        t = t->prev_lev;
    }
    // пробуем поискать в другом направлении
    t = elem;
    while (t)
    {
        if (t->type == type)
            return t;
        t = t->next_lev;
    }
    return NULL;
}

/*
уничтожение элемента из списка
*/
void elem_DeleteFile(CPMFILE *elem)
{
    ELEM *paren;
    ELEM *prev;
    ELEM *next;

    if (!elem)
        return;
    if (elem->elem.type != ELEM_FILE)
        return;
    paren = elem->elem.prev_lev;
    prev  = elem->elem.prev_elem;
    next  = elem->elem.next_elem;

    // корректируем указатели
    if (next)
    {
        next->prev_elem = prev;
    }
    if (prev)
    {
        prev->next_elem = next;
    } else {
        if (paren)
            paren->next_lev = next;
    }
    free(elem);
}

// возвращает указатель на последний элемент пути
ELEM *elem_GetLast(char *Path)
{
    int  t;
    char buf[MAX_PATH];
    ELEM *n;
    ELEM *k = (ELEM *) Root;
    char *s = Path;

    while (*s != '\0')
    {
        n = k;
        // выделяем очередную подстроку пути
        s = strchr(s, '\\');
        if (s == NULL)
            return n;
        s++;
        t = strcspn(s, "\\");
        if (t == 0)
            return NULL;            // ошибочка вышла
        memcpy(buf, s, t);
        buf[t] = '\0';
        // и ищем совпадение
        while (n)
        {
            if (stricmp(n->name, buf) == 0)
                break;
            n = n->next_elem;
        }
        if (!n)
            return NULL;            // ничего не нашли
        // переходим к следующей подстроке
        s += t;
        k = n->next_lev;
    }
    return n;
}



//============================================================================
//▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒
//▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒ DISK CP/M IO ▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒
//▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒
//============================================================================

/*
откат резервирования директорных записей
*/
void disk_FreeDir(DISK *disk, DIRREC *dir, int nDirs)
{
    DIRREC *r = dir;

    if ((!disk) || (!dir))
        return;

    while (nDirs > 0)
    {
        if (r->res != 0)
            map_Free(disk->DirMap, r->res);
        r++;
        nDirs--;
    }
}

/*
откат резервирования кластеров
*/
void disk_FreeBlock(DISK *disk, DIRREC *dir, int nDirs)
{
    DIRREC *r = dir;
    int     i;

    if ((!disk) || (!dir))
        return;

    while (nDirs > 0)
    {
        for (i = 0; i < 8; i++)
            if (r->map[i] != 0)
                map_Free(disk->BlockMap, r->map[i]);
        r++;
        nDirs--;
    }
}


/*
резервирует память под директорные записи
*/
DIRREC *disk_AllocDir(DISK *disk, int nDirs, char *name, DWORD Attr)
{
    int     i, n = nDirs;
    char    cpmname[16];
    DIRREC *r, *dir;

    // выделяем память под DIRREC
    if ((dir = (DIRREC *) malloc(nDirs * sizeof(DIRREC))) == NULL)
    {
        log_Print("    *error disk_AllocDir(\"%s\") - insufficient memory\n", name);
        return NULL;
    }
    r = dir;
    memset(dir, 0, nDirs * sizeof(DIRREC));
    fcpm_Ansi2CPM(&cpmname, name);
    fcpm_SetAttrib(&cpmname, Attr);
    i = 0;
    while ((i < disk->MaxDirRec) && (n > 0))
    {
        if (map_Get(disk->DirMap, i) == 0)
        {
            memcpy(r->name, cpmname, 11);
            // нашли свободный блок в директории
            r->res = i;                 // временно запоминаем номер записи
            map_Set(disk->DirMap, i);
            n--;
            r++;
        }
        i++;
    }
    if (n > 0)
    {
        log_Print("    *error disk_AllocDir(\"%s\") - insufficient directory space\n", name);
        disk_FreeDir(disk, dir, nDirs);
        free(dir);
        dir = NULL;
    }
    return dir;
}

/*
выделяет место на диске и возвращает проинициализированный массив DIRREC
*/
DIRREC *disk_AllocSpace(DISK *disk, char nUser, char *filename, int nSize, DWORD Attr)
{
    UINT16  i, j;
    DIRREC *r, *dir;
    int     nBlocks;
    UINT16  nDirs;
    UINT16  curBlock;

    if ((!disk) || (nUser >= 16) || (!filename))
    {
        log_Print("    *error disk_AllocSpace() - bad parametr\n");
        return NULL;
    }
    nBlocks = (nSize+disk->BlockSize-1) / disk->BlockSize;
    nDirs = (nBlocks + 7) / 8;

    if ((disk->BlockMap->free < nBlocks) || (disk->DirMap->free < nDirs))
    {
        log_Print("    *error disk_AllocSpace() - disk full\n");
        return NULL;
    }
    // выделяем память под DIRREC
    if ((dir = disk_AllocDir(disk, nDirs, filename, Attr)) == NULL)
    {
        return NULL;
    }
    r = dir;
    curBlock = disk->DirBlocks;

    for (i = 0; i < nDirs; i++)             // для каждой директорной записям
    {
        r->user = nUser;
        j = 0;
        while ((j < 8) && (nBlocks > 0))
        {
            while (curBlock < disk->NumBlocks)
            {
                if (map_Get(disk->BlockMap, curBlock) == 0)
                {
                    // нашли свободный кластер
                    map_Set(disk->BlockMap, curBlock);
                    r->map[j] = curBlock;
                    nBlocks--;
                    break;
                }
                curBlock++;
            }
            j++;
        }
        r++;
    }
    if (curBlock >= disk->NumBlocks)
    {
        log_Print("    *error disk_AllocSpace() - can`t alloc blocks\n");
        disk_FreeDir(disk, dir, nDirs);
        disk_FreeBlock(disk, dir, nDirs);
        free(dir);
        dir = NULL;
    }
    return dir;
}



/*
получение параметров раздела CP/M
на входе:
    p       - хэндл жесткого диска
    n       - структура текущего раздела CP/M
    AbsSec  - абсолютный адрес начала диска CP/M
*/
BOOL disk_GetParam(UINT32 AbsSec, DISK *disk)
{
    SYSSEC  sec;
    DEVICE  *dev = elem_Get(ELEM_DEVICE, disk);

    if (!dev)
        return FALSE;
    // считываем блок параметров диска
    if (!ide_ReadSector(dev->handle, AbsSec, &sec))
    {
        log_Print("  *error disk_GetParam() - can't read sector at 0x%08X!\n", AbsSec);
        return FALSE;
    }
    if ((!ide_CheckSign((char *) &sec)) || (memcmp(sec.Sign, "CP/M    ", 8) != 0))
    {
        log_Print("  *info disk_GetParam() - disk is not CP/M!\n");
        return FALSE;
    }
    disk->AbsAddr = AbsSec;
    disk->StartSector = ((sec.dpb.OFF*sec.dpb.SPT)*128) / 512 + AbsSec + 1;
    disk->NumBlocks   = sec.dpb.DSM + 1;
    disk->BlockSize   = (sec.dpb.BLM + 1) * 128;
    disk->MaxDirRec   = sec.dpb.DRM + 1;
    disk->DirBlocks   = disk->MaxDirRec / (disk->BlockSize/sizeof(DIRREC));
    // создаем массивы под битовые карты занятости блоков и директорных записей
    disk->DirMap = malloc(sizeof(BMAP)+(disk->MaxDirRec+7)/8);
    disk->BlockMap = malloc(sizeof(BMAP)+(disk->NumBlocks+7)/8);
    if ((!disk->DirMap) || (!disk->BlockMap))
    {
        free(disk->DirMap);
        free(disk->BlockMap);
        return FALSE;
    }
    memset(disk->BlockMap, 0, sizeof(BMAP)+(disk->NumBlocks+7)/8);
    memset(disk->DirMap, 0, sizeof(BMAP)+(disk->MaxDirRec+7)/8);
    disk->BlockMap->size = disk->NumBlocks;
    disk->BlockMap->free = disk->NumBlocks;
    disk->DirMap->size   = disk->MaxDirRec;
    disk->DirMap->free   = disk->MaxDirRec;
    return TRUE;
}


/*
создает каталоги USER0-USER15, для эмуляции пользовательских разделов
на входе:
    disk    - текущий диск CP/M
*/
USER *disk_NewUsersRec(DISK *disk)
{
    int   i;
    USER *user;
    USER *prev = NULL;

    // создаем каталоги USER
    for (i=0; i<16; i++)
    {
        if ((user = malloc(sizeof(USER))) == NULL)
            break;
        user->elem.type = ELEM_USER;
        user->user_no = i;
        user->elem.attrib = FILE_ATTRIBUTE_DIRECTORY;
        sprintf(user->elem.name, "%s%02i", "user", i);
        user->elem.next_lev  = NULL;
        user->elem.prev_lev  = (ELEM *) disk;
        user->elem.next_elem = NULL;
        user->elem.prev_elem = (ELEM *) prev;
        // добавляем в список диска
        if (prev)
        {
            prev->elem.next_elem = (ELEM *) user;
        } else {  // первый элемент
            disk->elem.next_lev = (ELEM *) user;
        }
        prev = user;
    }
    if (i != 16)
    {
        // не хватило памяти
        log_Print("    *error disk_NewUsersRec() - insufficient memory!\n");
        prev = (USER *) disk->elem.next_lev;
        disk->elem.next_lev = NULL;
        while (prev)
        {
            user = prev;
            prev = (USER *) prev->elem.next_elem;
            free(user);
        }
    }
    return (USER *) disk->elem.next_lev;
}

void disk_UserUpCase(USER *user)
{
    if (user)
        if ((user->elem.next_lev) && (user->elem.type == ELEM_USER))
        {
            sprintf(user->elem.name, "%s%02i", "USER", user->user_no);
        } else {
            sprintf(user->elem.name, "%s%02i", "user", user->user_no);
        }
}

/*
создает новую структуту под файл
*/
CPMFILE *disk_NewFileRec(DIRREC *dir)
{
    CPMFILE *file = malloc(sizeof(CPMFILE));

    if (file)
    {
        file->elem.type = ELEM_FILE;
        file->Size = dir->ex * 16384 + (dir->rc * 128);
        // сохраняем оригинальное имя
        memcpy(file->Orig, dir->name, 11);
        // конвертируем имя в строковой вид
        fcpm_CPM2Ansi(file->elem.name, dir->name);
        // извлекаем аттрибуты из имени файла
        file->elem.attrib = fcpm_GetAttrib(dir->name);
        file->elem.next_elem = NULL;
        file->elem.prev_elem = NULL;
        file->elem.next_lev = NULL;
        file->elem.prev_lev = NULL;
    }
    return file;
}


void disk_InsertFile(DISK *disk, DIRREC *dir)
{
    CPMFILE *file, *first;
    USER *user = elem_Get(ELEM_USER, disk);

    // ищем ссылку на USER № dir->user
    while (user)
    {
        if (user->user_no == dir->user)
            break;
        user = (USER *) user->elem.next_elem;
    }
    if (!user)
        return;

    if (!user->elem.next_lev)
    {
        // первый в списке
        file = disk_NewFileRec(dir);
        file->elem.prev_lev    = (ELEM *) user;
        user->elem.next_lev = (ELEM *) file;
    } else {
        // ищем файл в списке файлов USER
        file = elem_Get(ELEM_FILE, user);
        while (file)
        {
            if (fcpm_CompareName(file->Orig, dir->name))
                break;
            file = (CPMFILE *) file->elem.next_elem;
        }
        if (file)
        {
            // файл уже присутствует, обновляем его размер
            file->Size = dir->ex * 16384 + (dir->rc * 128);
        } else {
            // добавляем файл в список
            file = disk_NewFileRec(dir);
            first = (CPMFILE *) user->elem.next_lev;  // пред файловый элемент
            first->elem.prev_elem = (ELEM *) file;
            file->elem.next_elem  = (ELEM *) first;
            file->elem.prev_lev   = (ELEM *) user;
            user->elem.next_lev   = (ELEM *) file;
        }
    }
}


/* сканирование директории диска и создание списка файлов */
BOOL disk_CreateFileList(DISK *disk)
{
    UINT16  i, j;
    USER   *user;
    DIRREC  dir[DIRINSEC];
    int     nDir;
    UINT16  nSec = 0;
    DEVICE *dev  = elem_Get(ELEM_DEVICE, disk);

    if (!dev)
    {
        log_Print("    *error disk_CreateFileList() - bad disk '%s'\n", disk->elem.name);
        return FALSE;
    }
    if (!dev->handle)
    {
        log_Print("    *error disk_CreateFileList() - bad handle\n");
        return FALSE;
    }
    // подключаем подкаталоги USER
    if ((user = disk_NewUsersRec(disk)) == NULL)
        return FALSE;

    // сразу помечаем блоки, занятые под директорию
    for (i = 0; i < disk->DirBlocks; i++)
        map_Set(disk->BlockMap, i);

    nDir = disk->MaxDirRec;
    // сканируем директорий
    while (nDir > 0)
    {
        // подгружаем очередной сектор
        if (!ide_ReadSector(dev->handle, disk->StartSector + nSec, &dir))
        {
            log_Print("    *error disk_CreateFileList() - can't read directory sector at 0x%08X\n", disk->StartSector+nSec);
            return FALSE;
        }
        // сканируем очередной сектор
        i = 0;
        while ((i < DIRINSEC) && (nDir > 0))
        {
            if (dir[i].user != 0xE5)
            {
                // нашли файл (или часть), помечаем занимаемые блоки
                map_Set(disk->DirMap, disk->MaxDirRec-nDir);
                for (j = 0; j < 8; j++)
                    if (dir[i].map[j] >= disk->DirBlocks)
                        if (map_Get(disk->BlockMap, dir[i].map[j]) == 0)
                            map_Set(disk->BlockMap, dir[i].map[j]);
                // и добавляем файл в список
                disk_InsertFile(disk, &dir[i]);
            }
            nDir--;
            i++;
        }
        nSec++;
    } // while

    // меняем регистр символов для пустых папок
    while (user)
    {
        disk_UserUpCase(user);
        user = (USER *) user->elem.next_elem;
    }
    return TRUE;
}


/*
монтирует новый диск CP/M
на входе:
на входе:
    dev     - структура теукщего жесткого диска
    absAddr - абсолютный адрес начала раздела логического диска CP/M
на выходе:  - структура добавленного диска или NULL при ошибке
*/
DISK *disk_Mount(DEVICE *dev, UINT32 absAddr)
{
    DISK *disk, *last;
    char ch;

    if ((disk = malloc(sizeof(DISK))) == NULL)
        return NULL;
    disk->elem.type = ELEM_DISK;
    disk->elem.attrib = FILE_ATTRIBUTE_DIRECTORY;
    disk->elem.next_elem = NULL;
    disk->elem.prev_elem = NULL;
    disk->elem.next_lev = NULL;
    disk->elem.prev_lev = (ELEM *) dev;

    // получаем параметры диска
    if (disk_GetParam(absAddr, disk) == 0)
    {
        free(disk);
        return NULL;
    }
    // включаем элемент в список
    last = elem_Get(ELEM_DISK, dev);
    ch = 0;
    if (last == NULL)
    {
        // первый элемент в списке
        dev->elem.next_lev = (ELEM *) disk;
    } else {
        while (last->elem.next_elem != NULL)
        {
            last = (DISK *) last->elem.next_elem;
            ch++;
        }
        ch++;
        last->elem.next_elem = (ELEM *) disk;
        disk->elem.prev_elem = (ELEM *) last;
    }
    sprintf(disk->elem.name, "%c", ch+'A');

    // добавляем список файлов
    if (!disk_CreateFileList(disk))
        return NULL;

    return disk;
}





//============================================================================
//▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒
//▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒ FILE CP/M IO ▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒
//▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒
//============================================================================



/*
на входе:
  file    - файл
  dstBuff - буфер, если NULL, то выделяется память под новый
            должен иметь запас в 512 байт
на выходе:
          - буфер (созданный или dstBuff)
*/
char *file_Read(CPMFILE *file, char *dstBuff)
{
    DEVICE *dev   = elem_Get(ELEM_DEVICE, file);
    DISK   *disk  = elem_Get(ELEM_DISK, file);
    USER   *user  = elem_Get(ELEM_USER, file);
    int     nDir;
    int     nSize;
    UINT16  nSec  = 0;
    UINT16  i, j, k;
    char   *b;
    DIRREC  dir[DIRINSEC];
    UINT16  nSecPerBlock;

    if ((!file) || (!dev) || (!disk) || (!user))
        return NULL;

    nDir  = disk->MaxDirRec;
    nSize = file->Size;
    nSecPerBlock = disk->BlockSize / 512;
    if (!dstBuff)
    {
        dstBuff = malloc(nSize+512);
        if (!dstBuff)
        {
            log_Print("    *error file_Read(\"%s\") - not enought memory\n", file->elem.name);
            return NULL;
        }
    }
    b = dstBuff;
    memset(b, 0, nSize+512);

    while ((nDir > 0) && (nSize > 0))
    {
        // подгружаем очередной сектор
        if (!ide_ReadSector(dev->handle, disk->StartSector + nSec, &dir))
        {
            log_Print("    *error file_Read(\"%s\") - can't read directory sector at %u\n", file->elem.name, disk->StartSector + nSec);
            free(b);
            return NULL;
        }
        // сканируем очередной сектор
        i = 0;
        while ((i < DIRINSEC) && (nDir > 0) && (nSize > 0))
        {
            if ((dir[i].user == user->user_no) && (fcpm_CompareName(dir[i].name, file->Orig)))
            {
                j = 0;
                while ((j < 8) && (nSize > 0))
                {
                    // читаем занимаемые блоки, если не выходят за пределы диска
                    if ((dir[i].map[j] >= disk->DirBlocks) && (dir[i].map[j] < disk->NumBlocks))
                    {
                        if (map_Get(disk->BlockMap, dir[i].map[j]) > 0)
                        {
                            k = 0;
                            while ((k < nSecPerBlock) && (nSize > 0))
                            {
                                ide_ReadSector(dev->handle, disk->StartSector+(dir[i].map[j]*nSecPerBlock)+k, b);
                                if (nSize >= 512)
                                {
                                    b += 512;
                                    nSize -= 512;
                                } else {
                                    b += nSize;
                                    nSize = 0;
                                }
                                k++;
                            }
                        } else {
                             log_Print("    *error file_Read(\"%s\") - sector %u is empty\n", file->elem.name, dir[i].map[j]);
                        }
                    } else {
                        log_Print("    *error file_Read(\"%s\") - sector %u is not bound\n", file->elem.name, dir[i].map[j]);
                    }
                    j++;
                }           // j
            }
            nDir--;
            i++;
        }                   // i
        nSec++;
    }                       // while
    return dstBuff;
}

// запись одной директорной записи в оглавление диска
BOOL file_WriteDir(DISK *disk, UINT16 nDir, DIRREC *dir)
{
    DEVICE *dev = elem_Get(ELEM_DEVICE, disk);
    DIRREC  buff[DIRINSEC];
    UINT32  nSec;
    UINT16  nRec;

    if ((!disk) || (!dev) || (!dir))
    {
        log_Print("    *error file_WriteDir() - bad parametrs!\n");
        return FALSE;
    }

    if (nDir >= disk->MaxDirRec)
    {
        log_Print("    *error file_WriteDir() - bad directory number!\n");
        return FALSE;
    }
    nSec = disk->StartSector + (nDir / DIRINSEC);
    nRec = nDir % DIRINSEC;
    ide_ReadSector(dev->handle, nSec, &buff);
    memcpy(&buff[nRec], dir, sizeof(DIRREC));
    ide_WriteSector(dev->handle, nSec, &buff);
    return TRUE;
}

BOOL file_Write(USER *user, char *fname, char *buff, DWORD nSize, DWORD Attr)
{
    DEVICE *dev     = elem_Get(ELEM_DEVICE, user);
    DISK   *disk    = elem_Get(ELEM_DISK, user);
    UINT8   ex      = 0;
    UINT32  total   = 0;
    char   *b       = buff;
    DIRREC *dir;
    DIRREC *r;
    UINT16  nBlocks;
    UINT16  nDirs;
    UINT16  nSecPerBlock;
    UINT16  j, k;

    if ((!dev) || (!disk) || (!buff) || (!fname))
    {
        log_Print("    *error file_Write() - bad parameters\n");
        return FALSE;
    }
    strupr(fname);
    if ((dir = disk_AllocSpace(disk, user->user_no, fname, nSize, Attr)) == NULL)
    {
        log_Print("    *error file_Write(\"%s\") - can`t allocate space for file\n", fname);
        return FALSE;
    }
    r       = dir;
    nBlocks = (nSize+disk->BlockSize-1) / disk->BlockSize;
    nDirs   = (nBlocks + 7) / 8;
    nSecPerBlock = disk->BlockSize / 512;

    // пишем на диск
    while ((nDirs) && (nSize > 0))
    {
        // пишем данные на диск
        j = 0;
        while ((j < 8) && (nSize > 0))
        {
            // пишем кластеры данной дир. записи
            if ((r->map[j] >= disk->DirBlocks) && (r->map[j] < disk->NumBlocks))
            {
                if (map_Get(disk->BlockMap, r->map[j] > 0))
                {
                    // пишем очередной кластер
                    k = 0;
                    while ((k < nSecPerBlock) && (nSize > 0))
                    {
                        ide_WriteSector(dev->handle, disk->StartSector+(r->map[j]*nSecPerBlock)+k, b);
                        b += 512;
                        if (nSize >= 512)
                        {
                            total += 512;
                            nSize -= 512;
                        } else {
                            total += nSize;
                            nSize = 0;
                        }
                        if (total > 16384)
                        {
                            total -= 16384;
                            ex++;
                        }
                        k++;
                    } // while
                } else {
                     log_Print("    *fatal error file_Write(\"%s\") - sector %u is empty\n", fname, r->map[j]);
                }
            } else {
                log_Print("    *fatal error file_Write(\"%s\") - sector %u is not bound\n", fname, r->map[j]);
            }

            j++;
        } // for j
        r->ex = ex;
        r->rc = (total+127) / 128;
        k = r->res;                    // n - номер директории
        r->res = 0;
        // записываем саму директорную запись
        if (!file_WriteDir(disk, k, r))
        {
            log_Print("    *error file_Write(\"%s\") - can`t write directory rec\n", fname);
            return FALSE;
        }
        // заносим в список
        disk_InsertFile(disk, r);
        r++;
        nDirs--;
    } // for i
    return TRUE;
}



/*
получает путь, имя и указатель на структуру пути к файлу назначения
на входе:
    RemoteName - полный путь к файлу на устройстве CP/M
на выходе:
               - возвращает путь к файлу (USER *)
    Name       - имя файла на устройстве CP/M
*/
USER *SplitRemoteName(char *RemoteName, char *Name)
{
    char Path[MAX_PATH];
    USER *user;

    // создаем пути назначения
    if (!SplitPath(RemoteName, &Path, Name))
    {
        log_Print("    *error SplitRemoteName() - bad destination: \"%s\"!\n", RemoteName);
        return NULL;
    }
    if (!(user = (USER *) elem_GetLast(Path)))
    {
        log_Print("    *error SplitRemoteName() - bad path: \"%s\"!\n", Path);
        return NULL;
    }
    if (user->elem.type != ELEM_USER)
    {
        log_Print("    *error SplitRemoteName() - path must be only USER!\n");
        return NULL;
    }
    return user;
}






//============================================================================
//▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒
//▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒ INTERFACE WITH PLUGIN TC ▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒
//▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒
//============================================================================

HANDLE plg_FindFirst(char *Path, WIN32_FIND_DATA *FindData)
{
    LASTFIND *lf;

    if (Root == NULL)
        return INVALID_HANDLE_VALUE;

    lf = malloc(sizeof(LASTFIND));
    if (lf == NULL)
        return INVALID_HANDLE_VALUE;

    if (strcmp(Path, "\\") == 0)
    {
        // начинаем вывод корня
        lf->elem = (ELEM *) Root;

        FindData->dwFileAttributes=FILE_ATTRIBUTE_DIRECTORY;
        FindData->ftLastWriteTime.dwHighDateTime=0xFFFFFFFF;
        FindData->ftLastWriteTime.dwLowDateTime=0xFFFFFFFE;
        strcpy(FindData->cFileName, lf->elem->name);
        strcpy(lf->path, Path);
    } else {
        if ((lf->elem = elem_GetLast(Path)) == NULL)
            return INVALID_HANDLE_VALUE;                // путь не найден
        FindData->ftLastWriteTime.dwHighDateTime = 0xFFFFFFFF;
        FindData->ftLastWriteTime.dwLowDateTime = 0xFFFFFFFE;
        if (lf->elem->next_lev != NULL)
        {
            lf->elem = lf->elem->next_lev;
            FindData->dwFileAttributes = lf->elem->attrib;
            strcpy(FindData->cFileName, lf->elem->name);
            if (lf->elem->type == ELEM_FILE)
            {
                CPMFILE *t = (CPMFILE *) lf->elem;
                FindData->nFileSizeLow = t->Size;
            }
        } else {
            // каталог пуст
            FindData->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
            strcpy(FindData->cFileName, "..");
            lf->elem = lf->elem->next_lev;
        }
        strcpy(lf->path, Path);
    }
    return (HANDLE) lf;
}


BOOL plg_FindNext(HANDLE Hdl, WIN32_FIND_DATA *FindData)
{
    LASTFIND *lf;

    lf = (LASTFIND *) Hdl;
    if (!lf->elem)
        return FALSE;
    lf->elem = lf->elem->next_elem;
    if (!lf->elem)
        return FALSE;

    strcpy(FindData->cFileName, lf->elem->name);
    FindData->dwFileAttributes = lf->elem->attrib;
    if (lf->elem->type == ELEM_FILE)
    {
        CPMFILE *t = (CPMFILE *) lf->elem;
        FindData->nFileSizeLow = t->Size;
    }
    return TRUE;
}

/*
удаление файла
*/
BOOL plg_DeleteFile(char* RemoteName)
{
    DISK  *disk;
    DEVICE *dev;
    USER  *user;
    int    nDir;
    DIRREC dir[DIRINSEC];
    UINT16 i, j;
    BOOL   update;
    UINT16 nSec = 0;
    CPMFILE *file = (CPMFILE *) elem_GetLast(RemoteName);

    if ((!file) || (file->elem.type != ELEM_FILE))
    {
        log_Print("    *error file_Delete() - bad path \"%s\"\n", RemoteName);
        return FALSE;
    }
    user = elem_Get(ELEM_USER, file);
    disk = elem_Get(ELEM_DISK, file);
    dev = elem_Get(ELEM_DEVICE, file);
    if ((!user) || (!disk) || (!dev))
    {
        log_Print("    *error file_Delete() - bad path \"%s\"\n", RemoteName);
        return FALSE;
    }
    nDir = disk->MaxDirRec;
    while (nDir > 0)
    {
        // подгружаем очередной сектор
        if (!ide_ReadSector(dev->handle, disk->StartSector + nSec, &dir))
        {
            log_Print("    *error file_Delete() - can't read directory sector at 0x%08X\n", disk->StartSector+nSec);
            return FALSE;
        }
        update = FALSE;
        // сканируем очередной сектор
        i = 0;
        while ((i < DIRINSEC) && (nDir > 0))
        {
            if (dir[i].user == user->user_no)
            {
                if (fcpm_CompareName(dir[i].name, file->Orig))
                {
                    // освобождаем занимаемые блоки
                    update = TRUE;          // сектор нужно перезаписать
                    dir[i].user = 0xE5;     // помечаем директорную запись недействительной
                    map_Free(disk->DirMap, disk->MaxDirRec-nDir);
                    for (j = 0; j < 8; j++)
                        if (dir[i].map[j] >= disk->DirBlocks)
                        {
                            if (map_Get(disk->BlockMap, dir[i].map[j]) > 0)
                                map_Free(disk->BlockMap, dir[i].map[j]);
                        }
                }
            }
            nDir--;
            i++;
        }
        if (update)
            ide_WriteSector(dev->handle, disk->StartSector + nSec, &dir);
        nSec++;
    } // while
    // исключаем из списка
    elem_DeleteFile(file);
    disk_UserUpCase(user);
    return TRUE;
}



int plg_GetFile(char* RemoteName, char* LocalName, int CopyFlags, RemoteInfoStruct* ri)
{
    CPMFILE *src;
    HANDLE  *dst;
    char    *buff;
    DWORD    nWritten;

    if (!(src = (CPMFILE *) elem_GetLast(RemoteName)))
        return FS_FILE_NOTFOUND;

    if ( (FileExist(LocalName) && !(CopyFlags & (FS_COPYFLAGS_OVERWRITE | FS_COPYFLAGS_RESUME))))
        return FS_FILE_EXISTSRESUMEALLOWED;


    if (CopyFlags & FS_COPYFLAGS_OVERWRITE)
    {
        // необходимо произвести перезапись
        dst = CreateFile(LocalName, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS , ri->Attr, NULL);
        if (dst == INVALID_HANDLE_VALUE)
        {
            log_Print("    *error plg_GetFile() - can`t overwrite file \"%s\"\n", LocalName);
            return FS_FILE_OK;
        }

    } else if(CopyFlags & FS_COPYFLAGS_RESUME) {
        // необходимо произвести дозапись
        dst = CreateFile(LocalName, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (dst == INVALID_HANDLE_VALUE)
        {
            log_Print("    *error plg_GetFile() - can`t open file \"%s\"\n", LocalName);
            return FS_FILE_OK;
        }
    } else {
        //действия, которые необходимо произвести при экспорте без перезаписи и дозаписи
        dst = CreateFile(LocalName, GENERIC_WRITE, 0, NULL, CREATE_NEW, ri->Attr, NULL);
        if (dst == INVALID_HANDLE_VALUE)
        {
            log_Print("    *error plg_GetFile() - can`t create file \"%s\"\n", LocalName);
            return FS_FILE_OK;
        }
    }

    // копируем
    if ((buff = file_Read(src, NULL)) == NULL)
    {
        log_Print("    *error plg_GetFile() - can`t read from \"%s\"\n", RemoteName);
        CloseHandle(dst);
        return FS_FILE_READERROR;
    }

    WriteFile(dst, buff, src->Size, &nWritten, NULL);
    CloseHandle(dst);
    free(buff);
    // корректируем его атрибуты
    SetFileAttributes(LocalName, fcpm_GetAttrib(src->Orig));

    if (CopyFlags & FS_COPYFLAGS_MOVE)
    {
        plg_DeleteFile(RemoteName);
    }
    return FS_FILE_OK;
}


int plg_PutFile(char* LocalName, char* RemoteName, int CopyFlags)
{
    char Name[MAX_PATH];

    CPMFILE *fil = (CPMFILE *) elem_GetLast(RemoteName);
    USER    *path;
    char    *lBuff;
    DWORD    lSize;
    DWORD    Attr;

    // буфер и размер находящегося на диске файла
    char    *rBuff = NULL;

    log_Print("  *info plg_PutFile(%s)\n", RemoteName);
    // проверяем наличие существующего файла на диске
    if ((fil) && (!(CopyFlags & (FS_COPYFLAGS_OVERWRITE | FS_COPYFLAGS_RESUME))))
        return FS_FILE_EXISTSRESUMEALLOWED;

    // создаем пути назначения
    if ((path = SplitRemoteName(RemoteName, &Name)) == NULL)
        return FS_FILE_WRITEERROR;

    // подгружаем исходный файл с диска и получаем его атрибуты
    if ((lBuff = FileLoad(LocalName, &lSize)) == NULL)
        return FS_FILE_READERROR;
    Attr = GetFileAttributes(LocalName);

    if (lSize == 0)
        return FS_FILE_OK;

    if (CopyFlags & FS_COPYFLAGS_RESUME)
    {
        // дополняем в конец существующего
        if ((rBuff = malloc(lSize + fil->Size + 512*2)) == NULL)
        {
            log_Print("    *error plg_PutFile() - not enought memory!\n");
            free(lBuff);
            return FS_FILE_OK;
        }
        memset(rBuff, 0, lSize + fil->Size + 512*2);
        if (file_Read(fil, rBuff) == NULL)
        {
            log_Print("    *error plg_PutFile() - can`t read file \"%s\"!\n", fil->elem.name);
            free(lBuff);
            free(rBuff);
            return FS_FILE_OK;
        }
        memcpy(rBuff+fil->Size, lBuff, lSize);
        free(lBuff);
        lBuff = rBuff;
        lSize += fil->Size;
        Attr = fil->elem.attrib;        // будем использовать атрибуты существующего
        plg_DeleteFile(RemoteName);
    } else if (CopyFlags & FS_COPYFLAGS_OVERWRITE)
    {
        // перезаписываем существующий файл
        plg_DeleteFile(RemoteName);
    }

    // пишем на диск
    if (!file_Write(path, Name, lBuff, lSize, Attr))
        return FS_FILE_WRITEERROR;

    disk_UserUpCase(path);
    return FS_FILE_OK;
}


/*
копирование/перемещение файла
*/
int plg_RenMovFile(char* OldName, char* NewName, BOOL Move, BOOL OverWrite, RemoteInfoStruct* ri)
{
    char Name[MAX_PATH];
    CPMFILE *src = (CPMFILE *) elem_GetLast(OldName);
    CPMFILE *dst = (CPMFILE *) elem_GetLast(NewName);
    USER    *srcPath;
    USER    *dstPath;
    char    *buff;
    DWORD    nSize;
    DWORD    Attr;

    if ((!src) || (src->elem.type != ELEM_FILE))
        return FS_FILE_NOTSUPPORTED;

    Attr = src->elem.attrib;

    if (dst && !OverWrite)
        return FS_FILE_EXISTS;

    srcPath = elem_Get(ELEM_USER, src);

    // создаем пути назначения
    if ((dstPath = SplitRemoteName(NewName, &Name)) == NULL)
        return FS_FILE_WRITEERROR;

    // подгружаем исходный файл с диска
    buff = file_Read(src, NULL);
    nSize= src->Size;

    if (OverWrite)
    {
        // перезаписываем существующий файл
        plg_DeleteFile(NewName);
    }

    // копируем
    if (!file_Write(dstPath, Name, buff, nSize, Attr))
        return FS_FILE_WRITEERROR;

    if (Move)
    {
        plg_DeleteFile(OldName);
    }

    disk_UserUpCase(srcPath);
    disk_UserUpCase(dstPath);
    return FS_FILE_OK;
}


