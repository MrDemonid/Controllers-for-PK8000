@echo on
del ..\ROM\pk8000_hdd.bin

cls
..\Bin\ma80.exe Bios,bios=bios
if errorlevel == 1 goto error
..\Bin\ma80.exe boot,boot=boot
if errorlevel == 1 goto error
..\Bin\mlink /ws:DA00 bios
if errorlevel == 1 goto error
..\Bin\mlink /ws:4000 boot
if errorlevel == 1 goto error

@echo off
copy /b boot.com+cpm.sys+bios.com ..\ROM\pk8000_hdd.bin
rem for original MDOS 1.0 uncomment the line below
rem copy /b boot.com+cpm.fdd+bios.com ..\ROM\pk8000_hdd.bin

if not exist ..\ROM\pk8000_hdd.bin goto error

echo =================================================
echo = Yeehaw! ROM created! File: ROM\PK8000_HDD.BIN =
echo =================================================
goto end

:error
@echo off
echo ######################################
echo ######### AN ERROR OCCURED! ##########
echo ######################################

:end
pause
if exist bios.com del bios.com
if exist boot.com del boot.com
del *.rel
del *.prn

