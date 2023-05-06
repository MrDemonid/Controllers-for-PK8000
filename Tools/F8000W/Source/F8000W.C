#pragma pack (4)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <conio.h>
#include <string.h>
//#include <dos.h>

#define VERSION         "2.5"

#define CPM_TYPE        0x02        // тип раздела CP/M
#define MAX_DIR         0x10        // максимальное количество кластеров под директорию

// дефолтные значения параметров дисков
#define DEFAULT_ALV         170     // 170 байт = 1360 кластера
#define MAX_SYSTEM_ALV      2000
#define DEFAULT_DIRBLOCKS   2
#define DEFAULT_RESTRACKS   2


#define MAX_MODEL_NAME      16

// тип подключенного устройства
#define DEV_DRIVE           0       // жесткий диск
#define DEV_IMAGE           1       // образ файла

//typedef unsigned char       BOOL;
typedef unsigned char       UINT8;
typedef unsigned short int  UINT16;
//typedef unsigned long int   UINT32;

typedef signed char         INT8;
typedef signed short int    INT16;
//typedef signed long int     INT32;




#pragma pack (1)

typedef struct _dev {
    HANDLE      handle;                 // хэндл диска
    char        bType;                  // флаг типа устройства (DEV_XXXXX)
    char        Name[MAX_PATH];         // полное имя устройства
    char        Model[MAX_MODEL_NAME];  // модель
    ULONGLONG   Size;                   // in sectors
    struct _dev *next;
} DEVICE;


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

/*
  структура для пользовательских параметров
  Параметры BLS и ALV взаимоисключаемы, поэтому один из
  них будет равен нулю, что и определит алгоритм
  расчёта параметров диска.
*/
typedef struct {
    UINT32  BLS;        // размер кластера (2048, 4096, 8192, etc...).
    UINT32  ALV;        // размер таблицы векторов занятости блоков
    UINT32  DirBlocks;  // количество кластеров под директорию
    UINT32  ResTracks;  // резервируемеые дорожки (под систему или еще куда)
    BOOL    isFilled;   // флаг заполнения нулями всего размеченного диска
} FILESYS;

#pragma pack ()

#define DIRINSEC        (512 / sizeof(DIRREC))



DEVICE *devRoot = NULL; // список жестких дисков
char bRemovableEnable;  // разрешение на поиск CP/M на сменяемых носителях

UINT32 usedALV;         // используемое диском ALV


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

                                  FORMAT

==============================================================================
*/

char hdd_CheckSign(char buff[])
{
    if ((buff[0x1FE] != 0x55) || (buff[0x1FF] != 0xAA))
        return 0;
    else
        return -1;
}

//
// вычисляет наиболее подходящий размер BLS для текущей емкости
//  size - предварительный размер блока
//
UINT16 calck_BlockSize(UINT32 DiskSize, UINT32 alv)
{
    UINT32 BLS, tmp;

    tmp = DiskSize / ((alv - 1) * 8);       // примерный размер кластера
    BLS = 2048;                             // минимум для винта
    while (BLS < tmp)
        BLS <<= 1;
    return BLS;
}


//
// DiskSize     - размер диска в байтах
// BlockSize    - размер кластера
// ResTracks    - резерв. дорожек
// SecPerTrack  - логических секторов на дорожке (по 128 байт)
// DirBlock     - размер директории в кластерах
//
void make_DPB(DPB *dpb, UINT32 DiskSize, UINT32 BlockSize, UINT32 ResTracks, UINT32 SecPerTrack, int DirBlock)
{
    UINT32  Tracks;
    UINT32  SecInBlock;
    int     w;

    Tracks = DiskSize / (SecPerTrack * 128);
    SecInBlock = BlockSize / 128;
    dpb->SPT = SecPerTrack;
    dpb->OFF = ResTracks;
    dpb->CKS = 0;

    dpb->BLM = SecInBlock - 1;
    dpb->BSH = 3;
    dpb->EXM = 0;
    w = BlockSize/1024;
    while (w > 1)
    {
        dpb->BSH++;
        dpb->EXM = (dpb->EXM << 1) | 0x01;
        w >>= 1;
    }
    dpb->DSM = ((Tracks-ResTracks) * SecPerTrack) / SecInBlock;
    if (dpb->DSM > 256)
        dpb->EXM >>= 1;
    dpb->DSM--;
    w = 0x0000;
    dpb->DRM = DirBlock * (BlockSize / 32) - 1;
    do
    {
        w = (w >> 1) | 0x8000;
        DirBlock--;
    } while (DirBlock > 0);
    dpb->AL0 = (w >> 8) & 0xFF;
    dpb->AL1 = w & 0xFF;
}


char hdd_doFormatCPM(DEVICE *p, UINT32 AbsAddr, UINT32 TotalSec, FILESYS *fsys)
{
    UINT32  DiskSize;
    SYSSEC  sec;
    UINT32  i,n;
    UINT32  DSM, BLS;
    UINT32  DirBlocks, NumClust;
    char    buf[512];

    memset(&sec, 0, sizeof(SYSSEC));

    // предварительные вычисления
    DiskSize = TotalSec * 512;
    if (fsys->BLS > 0)
    {
        BLS = fsys->BLS;

    } else if (fsys->ALV > 0) {

        BLS = calck_BlockSize(DiskSize, fsys->ALV);

    } else {
        printf("      *error* - can't calculate disk parameters!\n");
        return 0;
    }
    // делаем первую попытку создания DPB
    make_DPB(&sec.dpb, DiskSize, BLS, fsys->ResTracks, 128, fsys->DirBlocks);
    NumClust = sec.dpb.DSM+1;
    // теперь корректируем количество блоков под директорные записи
    // их не должно быть меньше, чем количество блоков на диске
    DirBlocks = ( ( ((NumClust+7) / 8) * 32) + BLS-1) / BLS;

    if (DirBlocks > fsys->DirBlocks)
    {
        make_DPB(&sec.dpb, DiskSize, BLS, fsys->ResTracks, 128, DirBlocks);
    }

    DSM =(sec.dpb.DSM+1);
    DiskSize = (DSM * BLS) / 1024;

    printf("        First sector   : 0x%08lX\n", AbsAddr);
    printf("        Disk size      : %luKb\n", DiskSize);
    printf("        Reserv sectors : %luKb\n", (sec.dpb.OFF * (128*128)) / 1024);
    printf("        Cluster size   : %lu bytes\n", BLS);
    #ifdef _DEBUG_VERSION
      printf("        Block shift    : 0x%02hX\n", sec.dpb.BSH);
      printf("        Block mask     : 0x%02hX\n", sec.dpb.BLM);
      printf("        Extent mask    : 0x%02hX\n", sec.dpb.EXM);
    #endif
    printf("        Num clusters   : %hu\n", sec.dpb.DSM+1);
    printf("        Dir entries    : %hu\n", sec.dpb.DRM+1);
    #ifdef _DEBUG_VERSION
      printf("        AL0            : 0x%02hX\n", sec.dpb.AL0);
      printf("        AL1            : 0x%02hX\n", sec.dpb.AL1);
    #endif
    printf("        ALV            : %u\n", (sec.dpb.DSM+1 + 7) / 8);

    usedALV += ((sec.dpb.DSM+1 + 7) / 8);

    // заполняем сектор
    memcpy(sec.Sign, "CP/M    ", 8);
    sec.parSign = 0xAA55;

    if (!ide_WriteSector(p, AbsAddr, &sec))
    {
        printf("      *error* - can't write DPB at 0x%08lX!\n", AbsAddr);
        return 0;
    }
    // инициируем резервные области
    if (sec.dpb.OFF)
    {
        memset((char*) &buf, 0x00, sizeof(buf));
        n = (sec.dpb.OFF * (128*128)) / 512;
        for (i = 0; i < n; i++)
        {
            AbsAddr++;
            ide_WriteSector(p, AbsAddr, &buf);
        }
    }
    // очищаем оглавление или весь диск
    if (fsys->isFilled)
        n = (sec.dpb.DSM+1)*(BLS/512)-1;
    else
        n = (sec.dpb.DRM+1) / DIRINSEC;

    memset((char*) &sec, 0xE5, sizeof(SYSSEC));
    AbsAddr++;
    printf("        Format        .. ");
    fflush(stdout);
    for (i = 0; i < n; i++)
    {
        ide_WriteSector(p, AbsAddr, &sec);
        AbsAddr++;
    }
    printf("ok\n");
    return -1;
}


//
// показываем информацию по CP/M диску
//
char disk_ShowInfo(DEVICE *p, ULONGLONG AbsSec)
{
    SYSSEC sec;
    UINT32 DiskSize;

    UINT16 NumBlocks;          // размер диска в кластерах
    UINT16 BlockSize;          // размер кластера
    UINT32 DirBlocks;
    UINT32 NeedBlocks;

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
    NumBlocks = sec.dpb.DSM + 1;
    BlockSize = (sec.dpb.BLM + 1) * 128;
    DiskSize  = (NumBlocks * BlockSize) / 1024;

    NeedBlocks = ( ( ((NumBlocks+7) / 8) * 32) + BlockSize-1) / BlockSize;
    DirBlocks = ((sec.dpb.DRM+1)*32) / BlockSize;

    printf("        - Start sector     : 0x%lX\n", AbsSec);
    printf("        - Disk size        : %uKb\n", DiskSize);
    printf("        - Reserved sectors : %uKb\n", (sec.dpb.OFF * (128*128)) / 1024);
    printf("        - Cluster size     : %hu bytes\n", BlockSize);
    #ifdef _DEBUG_VERSION
      printf("        - Sec per tracks   : %hu\n", sec.dpb.SPT);
      printf("        - Extent mask      : 0x%02hX\n", sec.dpb.EXM);
    #endif
    printf("        - Num clusters     : %hu\n", NumBlocks);
    printf("        - Dir entries      : %hu\n", sec.dpb.DRM+1);
    printf("        - ALV              : %u\n", (sec.dpb.DSM+1 + 7) / 8);
    usedALV += ((sec.dpb.DSM+1 + 7) / 8);

    if (NeedBlocks > DirBlocks)
    {
        printf("    WARNING! Directory space is too small!\n             Need reformat disk!\n");
    }
    return -1;
}




char disk_GetInfo(DEVICE *p, ULONGLONG AbsSec)
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
    usedALV += ((sec.dpb.DSM+1 + 7) / 8);
    return -1;
}


//
// сканирование SMBR на предмет логических дисков CP/M
//  relAdd - относительный линейный адрес начала раздела SMBR
//
void hdd_ParseSMBR(DEVICE *p, ULONGLONG relAddr, char *lastDev, FILESYS *fsys)
{
    PARTION    *par;
    PARTION    *nxt;
    UINT8       buff[512];
    ULONGLONG   base = relAddr;     // абсолютный адрес начала SMBR
    char        c;

    do
    {
        // подгружаем SMBR
        if (!ide_ReadSector(p, relAddr, &buff))
        {
            printf("    *error* - can't read SMBR at 0x%12I64X!\n", relAddr);
            return;
        }
        if (!hdd_CheckSign(buff))
        {
            printf("    *error* - SMBR at 0x%12I64X is corrupt!\n", relAddr);
            return;
        }
        par = (PARTION *) &buff[0x1BE];
        nxt = (PARTION *) &buff[0x1CE];
        switch (par->Type)
        {
            case 131:
            case 0x01:
            case 0x04:
            case 0x06:
            case 0x0B:
            case 0x0E: {        // DOS logic disk
                (*lastDev)++;
                printf("    -found DOS disk [%c]      %9luKb\tFormat drive (Yes/No)? ", (*lastDev)+'A', par->Size / 2);
                fflush(stdout);
                c = getch();
                if ((c == 'y') || (c == 'Y'))
                {
                    printf("\r                                                                               \r");
                    printf("    -created CP/M disk [%c]\n", (*lastDev)+'A');
                    if (hdd_doFormatCPM(p, par->RelAddr+relAddr, par->Size, fsys))
                    {
                        par->Type = CPM_TYPE;
                        ide_WriteSector(p, relAddr, &buff);
                    }
                } else {
                    printf("\r                                                                               \r");
                    printf("    -skip DOS disk [%c]       %9luKb\n", (*lastDev)+'A', par->Size / 2);
                }
                break;
            }
            case CPM_TYPE: {
                (*lastDev)++;
                printf("    -found CP/M disk [%c]     %9luKb\tUnformat/Skip/Info (U/S/I)? ", (*lastDev)+'A', par->Size / 2);
                fflush(stdout);
                c = getch();
                if ((c == 'u') || (c == 'U'))
                {
                    par->Type = 1;
                    if ((par->Size / 2) > 15*1024)
                        par->Type += 3;
                    if ((par->Size / 2) > 32*1024)
                        par->Type += 2;
                    ide_WriteSector(p, relAddr, &buff);
                    printf("\r                                                                               \r");
                    printf("    -unformat disk [%c] to DOS disk\n", (*lastDev)+'A');
                } else if ((c == 'i') || (c == 'I')) {
                    printf("\r                                                                               \r");
                    printf("    -info of CP/M disk [%c]   %9luKb\n", (*lastDev)+'A', par->Size / 2);
                    disk_ShowInfo(p, par->RelAddr+relAddr);
                    fflush(stdout);
                } else {
                    printf("\r                                                                               \r");
                    printf("    -skip CP/M disk [%c]      %9luKb\n", (*lastDev)+'A', par->Size / 2);
                    disk_GetInfo(p, par->RelAddr+relAddr);
                }
                break;
            default: {
                printf("    -skip unkown disk     %9luKb\n", par->Size / 2);
                printf("          -type: %u\n",(UINT16) par->Type);
                break;
                }
            }
        } // switch
        relAddr = nxt->RelAddr+base;
    } while (nxt->Type != 0);
}



//
// ищет диски CP/M во всех расширенных разделах dos
//
void hdd_ParseMBR(DEVICE *p, FILESYS *fsys)
{
    PARTION    *par;
    UINT8       buff[512];
    int         i;
    char        lastDev;

    // подгружаем MBR
    if (!ide_ReadSector(p, 0, &buff))
    {
        printf("  *error* - can't read MBR!\n");
        return;
    }
    if (!hdd_CheckSign(buff))
    {
        printf("  *error* - MBR is corrupt!\n");
        return;
    }
    // сканируем партиции в MBR
    par = (PARTION*) &buff[0x1BE];
    lastDev = -1;
    for(i = 0; i < 4; i++)
    {
        if (par->Active)
        {
            printf("    -skip primary partion    %9luKb\n", par->Size / 2);
        } else {
            if ((par->Type == 0x05) || (par->Type == 0x0C) || (par->Type == 0x0F))
                    hdd_ParseSMBR(p, par->RelAddr, &lastDev, fsys);
        }
        par++;
    }
}



/*
==============================================================================

                                   MENU

==============================================================================
*/

/*
typedef struct _dev {
    HANDLE      handle;                 // хэндл диска
    char        Name[MAX_PATH];         // полное имя устройства
    char        Model[MAX_MODEL_NAME];  // модель
    ULONGLONG   Size;                   // in sectors
    struct _dev *next;
} DEVICE;
*/




// выбор устройства
DEVICE *dev_Select()
{
    ULONGLONG size;
    char      c;
    DEVICE   *t = devRoot;
    char      i = 1;

    printf(">Select drive:\n");
    while (t)
    {

        if (t->Size / (1024*1024) > 1024)
        {
            size = t->Size / (1024*1024*1024);
            c = 1;

        } else {
            size = t->Size / (1024*1024);
            c = 0;
        }
        printf("[%u]: %-20s - %I64d%s\n",
                  i, t->Model, size, c?"Gb":"Mb");
        t = t->next;
        i++;
    }
    printf("[any other key] to exit\n");
    fflush(stdout);
    c = getch();
    i = '1';
    t = devRoot;
    while (t)
    {
        if (i == c)
            return t;
        t = t->next;
        i++;
    }
    return NULL;
}


// добавляет в список очередное устройство
//
BOOL dev_Insert(char type, char *name, char *model, ULONGLONG Size)
{
    DEVICE *d, *t;

    if (!name)
        return FALSE;

    // создаем новый элемент DEVICE
    if ((d = malloc(sizeof(DEVICE))) == NULL)
    {
        printf("\n*error hdd_Insert() - not enought memory!\n");
        return FALSE;
    }
    // инициализируем поля
    memset(d, 0, sizeof(DEVICE));
    strncpy(d->Name, name, MAX_PATH-1);
    strncpy(d->Model, model, MAX_MODEL_NAME-1);
    d->Size  = Size;
    d->bType = type;
    // добавляем запись в список устройств
    if (devRoot == NULL)
    {
        devRoot = d;
    } else {
        t = devRoot;
        while (t->next != NULL)
        {
            t = t->next;
        }
        t->next = d;
    }
    return TRUE;
}

/*
  возвращает количество подключенных устройств/образов
*/
int dev_Count(void)
{
    DEVICE *t;
    int num;

    t = devRoot;
    num = 0;
    while (t)
    {
        num++;
        t = t->next;
    }
    return num;
}


// составление списка доступных устройств
void dev_FindDevices()
{
    HANDLE hDevice;
    char   name[32];
    int    n = 0;

    ULONGLONG size;
    DISK_GEOMETRY pdg;
    DWORD  nReads;

    STORAGE_DEVICE_DESCRIPTOR *sdd;
    STORAGE_PROPERTY_QUERY query;
    STORAGE_DESCRIPTOR_HEADER deschdr;

    BOOL   bResult;

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
                if ( (pdg.MediaType == FixedMedia) ||
                     ((pdg.MediaType == RemovableMedia) && bRemovableEnable) )
                {
                    size = pdg.Cylinders.QuadPart*pdg.TracksPerCylinder*pdg.SectorsPerTrack*pdg.BytesPerSector;
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
                                dev_Insert(DEV_DRIVE, name, (char *) sdd+sdd->ProductIdOffset, size);
                            } else {
                                dev_Insert(DEV_DRIVE, name, name, size);
                            }
                        }
                    } else {
                        printf("*error (%u)* - can't read data on disk!\n", GetLastError(), name);
                    }
                }
            }
            CloseHandle(hDevice);
        }
    }
}


void do_Usage()
{
    printf("Usage: F8000W.EXE [-options | /options]\n");
    printf("  options:\n");
    printf("    h|?         - this help\n");
    printf("    f<filename> - set name file of image HDD\n");
    printf("    r           - enable search on removable devices\n");
    printf("    a<xxxx>     - size ALV with 'xxx', [128..512] (default %u)\n", DEFAULT_ALV);
    printf("                  this parameter disabled parameter '-c'\n");
    printf("    c<xxxx>     - clusters size in bytes (2048, 4096, 8192, etc.)\n");
    printf("                  this parameter disabled parameter '-a'\n");
    printf("    d<xxxx>     - size directory in clusters [1..16] (default 2)\n");
    printf("    t<xxxx>     - reserved tracks [0..9] (default 2)\n");
    printf("    u           - clear all disk space (default clear only directory)\n");
}

char do_argv(int argc, char *argv[], FILESYS *fsys)
{
    int     i;
    long    n;
    int     t;
    char    file[MAX_PATH];
    char    imgname[MAX_PATH];
    HANDLE  h;

    bRemovableEnable = 0;

    fsys->BLS = 0;                      // по умолчанию вычисляем по ALV
    fsys->ALV = DEFAULT_ALV;
    fsys->DirBlocks = DEFAULT_DIRBLOCKS;
    fsys->ResTracks = DEFAULT_RESTRACKS;
    fsys->isFilled = 0;

    usedALV = 0;

    i = 1;
    while (i < argc)
    {
        if ((argv[i][0] == '-') || (argv[i][0] == '/'))
        {
            switch (argv[i][1])
            {
                case 'R':
                case 'r':
                    bRemovableEnable = 1;
                    break;
                case 'F':
                case 'f': {
                    memset(file, 0, MAX_PATH);
                    if (strlen(&argv[i][0]) > 2)
                    {
                        strncpy(file, &argv[i][2], MAX_PATH-1);
                    } else {
                        i++;
                        if (i < argc) {
                            strncpy(file, &argv[i][0], MAX_PATH-1);
                        }
                    }
                    // пробуем открыть образ
                    if ((h = CreateFile(file, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL)) == INVALID_HANDLE_VALUE)
                    {
                        printf("  *error* - parametr '-f' is bad! Use hard drives.\n");
                        break;
                    }
                    strcpy(imgname, "file '");
                    strcat(imgname, file);
                    strcat(imgname, "'");
                    dev_Insert(DEV_IMAGE, file, imgname, GetFileSize(h, NULL));
                    CloseHandle(h);
                    break;
                }
                case 'A':
                case 'a': {     // ALV size
                    n = atol(&argv[i][2]);
                    if ((n >= 128) && (n <= 512))
                    {
                        fsys->ALV = n;
                        fsys->BLS = 0;
                    } else
                        printf("  *error* - parameter '-a' is bad! Use default.\n");
                    break;
                }
                case 'C':
                case 'c':
                    n = atol(&argv[i][2]);
                    // проверяем на соответствие степени двойки
                    t = n & (-n);
                    if ((n & ~t) == 0)
                    {
                        fsys->BLS = n;
                        fsys->ALV = 0;
                    } else
                        printf("  *error* - parameter '-c' is bad! Ignored.\n");
                    break;
                case 'D':
                case 'd': {
                    n = atol(&argv[i][2]);
                    if ((n >= 1) && (n <= 16))
                        fsys->DirBlocks = n;
                    else
                        printf("  *error* - parametr '-d' is bad! Use default.\n");
                    break;
                }
                case 'T':
                case 't': {
                    n = atol(&argv[i][2]);
                    if ((n >= 0) && (n <= 9))
                        fsys->ResTracks = n;
                    else
                        printf("  *error* - parametr '-t' is bad! Use default.\n");
                    break;
                }
                case 'U':
                case 'u': {
                    fsys->isFilled = 1;
                    break;
                }
                case 'H':
                case 'h':
                case '?': {
                    do_Usage();
                    return 0;
                }

            } // switch

        }   // if
        i++;
    }
    fflush(stdout);
    return -1;
}


void do_Format(int argc, char *argv[])
{
    FILESYS fsys;
    DEVICE         *p;


    while (kbhit()) getch();

    if (!do_argv(argc, argv, &fsys))
        return;

    if (!devRoot)
        dev_FindDevices();

    if (dev_Count() == 1 && devRoot->bType == DEV_IMAGE)
    {
        p = devRoot;
    } else {
        p = dev_Select();
    }
    if (p)
    {
        printf("\nScanning: %s\n", p->Model);
        p->handle = CreateFile(p->Name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (p->handle != INVALID_HANDLE_VALUE)
        {
            hdd_ParseMBR(p, &fsys);
            CloseHandle(p->handle);
            // выводим статистику по ALV
            printf("\n    Used ALV [%u] of [%u]\n", usedALV, MAX_SYSTEM_ALV);
        } else {
            printf("*error [%u]* - can't open drive '%s'\n", GetLastError(), p->Name);
        }
    }
    // начинаем сканирование и формат
    fflush(stdout);
    while (kbhit()) getch();
}


int main(int argc, char *argv[])
{
    printf("Format CP/M hard disk or image file for PK8000.\t\tver %s\n", VERSION);
    printf("run with parametr '-?' for help\n\n");
    do_Format(argc, argv);
    printf("Bye!\n");
    return 0;
}






