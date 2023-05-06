:: free environment space
SET PROCESSOR_ARCHITECTURE=
SET PROCESSOR_IDENTIFIER=
SET PROCESSOR_LEVEL=
SET PROCESSOR_REVISION=
SET PROGRAMFILES=
SET USERPROFILE=
SET ALLUSERSPROFILE=
SET DXSDKROOT=
SET APPDATA=
SET COMMONPROGRAMFILES=
SET COMPUTERNAME=

:: set WATCOM path's
SET DRIVE=D
SET PATH=%DRIVE%:\WATCOM\BINNT;%DRIVE%:\WATCOM\BINW;%PATH%
SET INCLUDE=%DRIVE%:\WATCOM\H;%DRIVE%:\WATCOM\H\NT;%INCLUDE%
SET LIB=%DRIVE%:\WATCOM\lib386;%DRIVE%:\WATCOM\lib386\NT
SET WATCOM=%DRIVE%:\WATCOM

:: clear
del *.wfx
del *.obj
del *.dll
del *.lib

cls
wcc386 config.c -i="%INCLUDE%" -w4 -e25 -ei -zq -os -of -d2 -bd -6r -bt=nt -fo=.\obj\config.obj -mf
wcc386 cpmhdd.c -i="%INCLUDE%" -w4 -e25 -ei -zq -os -of -d2 -bd -6r -bt=nt -fo=.\obj\cpmhdd.obj -mf
wcc386 cpmplg.c -i="%INCLUDE%" -w4 -e25 -ei -zq -os -of -d2 -bd -6r -bt=nt -fo=.\obj\cpmplg.obj -mf
wcc386 log.c -i="%INCLUDE%" -w4 -e25 -ei -zq -os -of -d2 -bd -6r -bt=nt -fo=.\obj\log.obj -mf
wrc cpmplg.rc -bt=nt -dWIN32 -d_WIN32 -d__NT__ -i="$[:;%INCLUDE%" -q -ad -r -fo=.\obj\cpmplg.res

wlink name .\obj\cpmplg d all sys nt_dll op nostdcall op maxe=25 op q op symf FIL .\obj\config.obj,.\obj\cpmhdd.obj,.\obj\cpmplg.obj,.\obj\log.obj RES .\obj\cpmplg.res

:: link with map-file
::wlink name .\obj\cpmplg d all sys nt_dll op m op nostdcall op maxe=25 op q op symf FIL .\obj\config.obj,.\obj\cpmhdd.obj,.\obj\cpmplg.obj,.\obj\log.obj RES .\obj\cpmplg.res


if exist .\obj\cpmplg.dll copy /b .\obj\cpmplg.dll ..\*.wfx
echo Y | del .\obj\*.*




