@echo off
rem Builds build\archwindows.exe with MSVC. No CRT, no third-party libraries.
setlocal
set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSOUT=%TEMP%\_arch_vs_path.txt"
"%VSW%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%VSOUT%"
set /p VS=<"%VSOUT%"
if not defined VS (echo Visual Studio C++ tools not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build

rem  /Os /Gy /Gw           size-first code, function/data sections (single TU: no /GL needed)
rem  /GS- /Gs...           no stack cookies / probes (no CRT to provide them)
rem  /GR- /EHs-c-          no RTTI, no exceptions
rem  /Zc:threadSafeInit-   no guarded statics (needs CRT)
cl /nologo /std:c++20 /O1 /Os /Gy /Gw /GS- /Gs1000000 /GR- /EHs-c- /Zc:threadSafeInit- /Zc:inline ^
   /W4 /utf-8 /DUNICODE /D_UNICODE /DNDEBUG src\main.cpp /Fobuild\ /Fe:build\archwindows.exe ^
   /link /NODEFAULTLIB /ENTRY:start /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF ^
   /FILEALIGN:512 /DEBUG:NONE /MANIFEST:NO ^
   kernel32.lib user32.lib gdi32.lib advapi32.lib dwmapi.lib shell32.lib ole32.lib shlwapi.lib
if errorlevel 1 exit /b 1
for %%f in (build\archwindows.exe) do echo built %%f  %%~zf bytes
