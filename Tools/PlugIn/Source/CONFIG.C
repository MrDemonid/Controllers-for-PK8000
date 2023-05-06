/*****************************************************************************
 * Plugin for CP/M hard disk drives PK8000.                                  *
 * Copyright (C) 2017 Andrey Hlus                                            *
 *****************************************************************************/
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "resource.h"
#include "cpmplg.h"
#include "log.h"
#include "config.h"

// секции
#define SEC_CONFIG         "CONFIG"
#define SEC_IMAGEFILE      "IMAGEFILE"

#define KEY_LOG            "LOGENABLE"
#define KEY_FIXED          "FIXEDENABLE"
#define KEY_REMOVABLE      "REMOVABLEENABLE"
#define KEY_IMAGE          "FILE"



char szIniFile[MAX_PATH];

BOOL reqReinit;            // флаг изменения настроек
int  oldLog, oldFixed, oldRemovable;


void ini_Init(char *Path)
{
    HANDLE hFile;

    strcpy(szIniFile, Path);
    hFile = CreateFile(szIniFile, GENERIC_READ, FILE_SHARE_READ, NULL, CREATE_NEW, 0, 0);
    if (hFile != INVALID_HANDLE_VALUE)
        CloseHandle(hFile);
}

/*
возвращает значения ключей LOG и HDD
*/
void ini_GetConfig(BOOL *bLog, BOOL defLog, BOOL *bFix, BOOL defFix, BOOL *bRem, BOOL defRem)
{
    *bLog = GetPrivateProfileInt(SEC_CONFIG, KEY_LOG, defLog, szIniFile);
    *bFix = GetPrivateProfileInt(SEC_CONFIG, KEY_FIXED, defFix, szIniFile);
    *bRem = GetPrivateProfileInt(SEC_CONFIG, KEY_REMOVABLE, defRem, szIniFile);
}

/*
возвращает список ключей секции IMAGEFILE
*/
char *ini_GetImagesKey()
{

    char *keys;
    HANDLE hFile;
    int    nSize;

    hFile = CreateFile(szIniFile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, 0);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        nSize = GetFileSize(hFile, NULL);
        CloseHandle(hFile);
    } else {
        nSize = 2;
    }
    keys = malloc(nSize);
    memset(keys, 0, nSize);
    GetPrivateProfileString(SEC_IMAGEFILE, NULL, "\0", keys, nSize, szIniFile);
    return keys;
}

/*
возвращает значение ключа, секции IMAGEFILE
*/
int ini_GetImagePath(char *key, char *szFileName)
{
    if ((!key) || (!szFileName))
        return 0;
    return GetPrivateProfileString(SEC_IMAGEFILE, key, "\0", szFileName, MAX_PATH-2, szIniFile);
}





void OnInitDialog(HWND hDlg)
{
    char Path[MAX_PATH];
    char *listkey, *key;
    int  count;
    HANDLE hList;

    reqReinit = FALSE;
    // подгружаем конфиг
    oldLog = GetPrivateProfileInt(SEC_CONFIG, KEY_LOG, 0, szIniFile);
    oldFixed = GetPrivateProfileInt(SEC_CONFIG, KEY_FIXED, 0, szIniFile);
    oldRemovable = GetPrivateProfileInt(SEC_CONFIG, KEY_REMOVABLE, 0, szIniFile);
    CheckDlgButton(hDlg, IDC_CHKLOG, oldLog);
    CheckDlgButton(hDlg, IDC_CHKFIXED, oldFixed);
    CheckDlgButton(hDlg, IDC_CHKREMOVABLE, oldRemovable);
    hList = GetDlgItem(hDlg, IDC_LSTIMAGES);

    listkey = ini_GetImagesKey();
    key = listkey;
    while ((key) && (strlen(key) > 0))
    {
        count = ini_GetImagePath(key, Path);
        if (count)
            SendMessage(hList, LB_ADDSTRING, 0, (LPARAM) &Path);
        key = key + strlen(key) + 1;
    }
    free(listkey);
    if (SendMessage(hList, LB_GETCOUNT, 0, 0) > 0)
        SendMessage(hList, LB_SETCURSEL, 0, 0);
}

void OnInsert(HWND hDlg, DWORD idListBox)
{
    HANDLE       hList;
    OPENFILENAME of;
    char         szFileName[MAX_PATH];

    memset(&of, 0, sizeof(OPENFILENAME));
    memset(&szFileName, 0, sizeof(szFileName));
    of.lStructSize = sizeof(OPENFILENAME);
    of.hwndOwner = hDlg;
    of.lpstrFile = szFileName;
    of.nMaxFile = sizeof(szFileName);
    of.lpstrFilter = "All\0*.*\0";
    of.nFilterIndex = 1;
    of.lpstrFileTitle = NULL;
    of.nMaxFileTitle = 0;
    of.lpstrInitialDir = NULL;
    of.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    if (GetOpenFileName(&of))
    {
        hList = GetDlgItem(hDlg, idListBox);
        SendMessage(hList, LB_ADDSTRING, 0, (LPARAM) &szFileName);
        if (SendMessage(hList, LB_GETCOUNT, 0, 0) == 1)
            SendMessage(hList, LB_SETCURSEL, 0, 0);
        reqReinit = TRUE;
    }
}

void OnDelete(HWND hDlg, DWORD idListBox)
{
    HANDLE hList;
    int nItem, nCount;
    hList = GetDlgItem(hDlg, idListBox);
    if ((nCount = SendMessage(hList, LB_GETCOUNT, 0, 0))  > 0)
    {
        nItem = SendMessage(hList, LB_GETCURSEL, 0, 0);
        if (nItem != LB_ERR)
        {
            SendMessage(hList, LB_DELETESTRING, nItem, 0);
            nCount--;
            if (nCount > 0)
            {
                while (nItem >= nCount)
                    nItem--;
                SendMessage(hList, LB_SETCURSEL, nItem, 0);
            }
            reqReinit = TRUE;
        }
    }
}

void OnSave(HWND hDlg, DWORD idCheckLog, DWORD idCheckFixed, DWORD idCheckRemovable, DWORD idListBox)
{
    HANDLE hList;
    int    nItem, nCount;
    int    nLen;
    char   szFile[MAX_PATH];
    char   key[32];

    // сохраняем новые настройки
    sprintf(key, "%u", IsDlgButtonChecked(hDlg, idCheckLog));
    WritePrivateProfileString(SEC_CONFIG, KEY_LOG, key, szIniFile);
    sprintf(key, "%u", IsDlgButtonChecked(hDlg, idCheckFixed));
    WritePrivateProfileString(SEC_CONFIG, KEY_FIXED, key, szIniFile);
    sprintf(key, "%u", IsDlgButtonChecked(hDlg, idCheckRemovable));
    WritePrivateProfileString(SEC_CONFIG, KEY_REMOVABLE, key, szIniFile);
    // удаляем секцию IMAGE
    WritePrivateProfileString(SEC_IMAGEFILE, NULL, NULL, szIniFile);
    // сохраняем имена файлов-образов
    hList = GetDlgItem(hDlg, idListBox);
    nCount = SendMessage(hList, LB_GETCOUNT, 0, 0);
    nItem = 0;
    while (nCount > 0)
    {
        nLen = SendMessage(hList, LB_GETTEXTLEN, nItem, 0);
        if ((nLen > 0) && (nLen < MAX_PATH) && (nLen != LB_ERR))
        {
            if (SendMessage(hList, LB_GETTEXT, nItem, (LPARAM) &szFile) != LB_ERR)
            {
                sprintf(key, "%s%u", KEY_IMAGE, nItem);
                WritePrivateProfileString(SEC_IMAGEFILE, key, szFile, szIniFile);

            }
        }
        nItem++;
        nCount--;
    }
    // изменяем флаг реинициализации
    if ((reqReinit) || (oldLog != IsDlgButtonChecked(hDlg, idCheckLog)) || (oldFixed != IsDlgButtonChecked(hDlg, idCheckFixed)) || (oldRemovable != IsDlgButtonChecked(hDlg, idCheckRemovable)))
    {
        // переинициализируем плагин
        DonePlugin();
        InitPlugin();
    }
}


BOOL CALLBACK dlgProcConfig(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
        case WM_INITDIALOG:
            OnInitDialog(hDlg);
            return TRUE;

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
                case IDC_BTNINSERT:
                {
                    OnInsert(hDlg, IDC_LSTIMAGES);
                    return TRUE;
                }

                case IDC_BTNDELETE:
                {
                    OnDelete(hDlg, IDC_LSTIMAGES);
                    return TRUE;
                }

                case IDC_BTNOK:
                {
                    OnSave(hDlg, IDC_CHKLOG, IDC_CHKFIXED, IDC_CHKREMOVABLE, IDC_LSTIMAGES);
                    DestroyWindow(hDlg);
                    return TRUE;
                }

                case IDC_BTNCANCEL:
                {
                    DestroyWindow(hDlg);
                    return TRUE;
                }
            }
            break;

        case WM_CLOSE:
            DestroyWindow(hDlg);
            return TRUE;
    }
    return FALSE;

}
