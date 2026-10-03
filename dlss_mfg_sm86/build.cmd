@echo off
:: Builds the dlss_mfg_sm86 proxy DLLs and game bootstraps, runs every test
:: suite, and refreshes the copy-ready binaries in each game's mod\ folder.
::   build\release\          generic release (proxies + default INI)
::   games\<category>\<game>\mod\  working files for that game, laid out like its folder
:: /Brepro keeps unchanged sources byte-identical, so committed binaries only
:: change when the code does.
setlocal
set "root=%~dp0"
set "out=%root%build"
set "mh=%root%vendor\minhook"
set "sdk=%root%vendor\streamline\include"
set "src=%root%src"
if not exist "%out%" mkdir "%out%"
set "vs=C:\Program Files\Microsoft Visual Studio\18\Community"
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
    for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "vs=%%i"
)
call "%vs%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "cflags=/nologo /O2 /MT /Brepro"
set "lflags=/nologo /Brepro"

:: MinHook
for %%f in (buffer hook trampoline) do (
    cl %cflags% /c /W3 /I"%mh%\include" /Fo"%out%\%%f.obj" "%mh%\src\%%f.c" >nul
    if errorlevel 1 exit /b 1
)
cl %cflags% /c /W3 /I"%mh%\include" /Fo"%out%\hde64.obj" "%mh%\src\hde\hde64.c" >nul
if errorlevel 1 exit /b 1
set "hooks=%out%\buffer.obj %out%\hook.obj %out%\trampoline.obj %out%\hde64.obj"

:: Core DLL object and its tests. It only ships as forwarding proxies.
cl %cflags% /c /W4 /WX /EHsc /I"%sdk%" /I"%mh%\include" /Fo"%out%\dlss_mfg_sm86.obj" "%src%\dlss_mfg_sm86.cpp"
if errorlevel 1 exit /b 1
cl %cflags% /W4 /WX /wd4505 /EHsc /I"%sdk%" /I"%mh%\include" /Fo"%out%\test_mfg.obj" "%src%\tests\test_mfg.cpp" %hooks% /link %lflags% /OUT:"%out%\test_mfg.exe" user32.lib version.lib
if errorlevel 1 exit /b 1
"%out%\test_mfg.exe"
if errorlevel 1 exit /b 1

:: Forwarding proxies: the same object linked behind each System32 export table
python "%src%\generate_proxies.py"
if errorlevel 1 exit /b 1
for %%p in (version winmm dbghelp dinput8 dxgi d3d12) do (
    call :proxy %%p
    if errorlevel 1 exit /b 1
)
python "%src%\tests\test_proxies.py"
if errorlevel 1 exit /b 1

:: Generic release, same convention as dlssg_sm86: utility-class proxies at the
:: root, render-path names in alternatives\. version.dll is dlssg_sm86's own
:: slot, so it is only offered as an alternative here.
set "release=%out%\release"
if exist "%release%" rmdir /s /q "%release%"
mkdir "%release%\alternatives"
for %%p in (winmm dinput8 dbghelp) do copy /y "%out%\proxies\%%p\%%p.dll" "%release%\" >nul
for %%p in (dxgi d3d12 version) do copy /y "%out%\proxies\%%p\%%p.dll" "%release%\alternatives\" >nul
copy /y "%root%config\dlss_mfg_sm86.ini" "%release%\" >nul
copy /y "%root%README.md" "%release%\" >nul

:: games\ue5\ace-combat-8 bootstrap (no CRT, no frame generation logic)
set "ac8=%root%games\ue5\ace-combat-8"
cl %cflags% /c /W3 /GS- /Zl /Fo"%out%\start_protected_game.obj" "%ac8%\bootstrap\start_protected_game.c"
if errorlevel 1 exit /b 1
link %lflags% /SUBSYSTEM:WINDOWS /ENTRY:entry /NODEFAULTLIB /OUT:"%out%\start_protected_game.exe" "%out%\start_protected_game.obj" kernel32.lib user32.lib
if errorlevel 1 exit /b 1
cl %cflags% /c /W3 /GS- /Zl /Fo"%out%\launcher_test.obj" "%ac8%\bootstrap\tests\launcher_test.c"
if errorlevel 1 exit /b 1
link %lflags% /SUBSYSTEM:CONSOLE /ENTRY:testEntry /NODEFAULTLIB /OUT:"%out%\launcher_test.exe" "%out%\launcher_test.obj" kernel32.lib user32.lib
if errorlevel 1 exit /b 1
"%out%\launcher_test.exe"
if errorlevel 1 exit /b 1

:: Refresh the copy-ready binaries in each game's mod\ folder. The INIs there are
:: maintained by hand and are never overwritten.
copy /y "%out%\start_protected_game.exe" "%ac8%\mod\" >nul
copy /y "%out%\proxies\dinput8\dinput8.dll" "%ac8%\mod\Game\Binaries\Win64\" >nul
copy /y "%out%\proxies\winmm\winmm.dll" "%root%games\ue5\ninja-gaiden-2-black\mod\NINJAGAIDEN2BLACK\Binaries\Win64\" >nul
copy /y "%out%\proxies\winmm\winmm.dll" "%root%games\ue5\halo-campaign-evolved\mod\Meteorite\Binaries\Win64\" >nul
copy /y "%out%\proxies\winmm\winmm.dll" "%root%games\ue4\the-first-berserker-khazan\mod\BBQ\Binaries\Win64\" >nul
echo Built %release% and refreshed every game's mod folder; all tests passed.
exit /b 0

:proxy
set "dir=%out%\proxies\%1"
ml64 /nologo /c /Fo"%dir%\proxy_stubs.obj" "%dir%\proxy.asm" >nul
if errorlevel 1 exit /b 1
cl %cflags% /c /W4 /WX /EHsc /I"%dir%" /Fo"%dir%\proxy_forward.obj" "%src%\proxy_forward.cpp" >nul
if errorlevel 1 exit /b 1
link %lflags% /DLL /DEF:"%dir%\proxy.def" /OUT:"%dir%\%1.dll" /IMPLIB:"%dir%\proxy.lib" "%out%\dlss_mfg_sm86.obj" "%dir%\proxy_forward.obj" "%dir%\proxy_stubs.obj" %hooks% kernel32.lib user32.lib version.lib >nul
exit /b %errorlevel%
