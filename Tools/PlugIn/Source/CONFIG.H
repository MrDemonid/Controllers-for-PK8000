#ifndef _CONFIG_H_
#define _CONFIG_H_

void ini_Init(char *Path);
void ini_GetConfig(BOOL *bLog, BOOL defLog, BOOL *bFix, BOOL defFix, BOOL *bRem, BOOL defRem);
char *ini_GetImagesKey();
int ini_GetImagePath(char *key, char *szFileName);


BOOL CALLBACK dlgProcConfig(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);

#endif

