@echo off
REM ==========================================================================
REM  build.bat  --  one-command, self-contained Windows build for the
REM                 line-scanner pipeline. Bootstraps the whole toolchain
REM                 (MSVC + a pinned local CMake) and builds. On a bare
REM                 Windows box this is the ONLY command you need.
REM
REM  Incremental by design -- nothing is re-fetched or rebuilt needlessly:
REM    * MSVC is installed only if absent (detected via vswhere).
REM    * CMake 3.22.1 is downloaded to .\tools only on the first run.
REM    * yaml-cpp is fetched + compiled into .\build\_deps only on the first
REM      configure; later runs reuse it and recompile ONLY your changed files.
REM    * The script never deletes .\build, which is what makes the above work.
REM      (To force a clean rebuild, delete the build\ folder yourself.)
REM
REM  What it does, in order:
REM    1. Ensures the MSVC x64 toolset exists: locates it via vswhere and, if
REM       missing, downloads the VS Build Tools bootstrapper and installs the
REM       "Desktop development with C++" workload (one time, UAC-elevated).
REM       Then imports its environment (cl.exe, headers, libs).
REM    2. Installs (once) the exact CMake that ships with Ubuntu 22.04 (3.22.1)
REM       into .\tools. The shared CMakeLists.txt pins yaml-cpp 0.8.0, whose
REM       cmake_minimum_required(VERSION 3.4) is rejected by CMake 4.0+; 3.22.1
REM       (like Ubuntu) accepts it unchanged, so CMakeLists stays identical
REM       across Linux and Windows.
REM    3. Configures (first run only) and builds Release with MSVC via the
REM       Visual Studio generator. MSVC (cl.exe) is the compiler; the generator
REM       is just the build driver that invokes it.
REM
REM  Usage:  run  build.bat  from any shell (double-click works too).
REM  Output: build\Release\line_scanner.exe
REM ==========================================================================
setlocal
set "ROOT=%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

REM --- 1. Ensure the MSVC x64 toolset exists, installing it if necessary -----
call :find_vs
if defined VSINSTALL goto :import_msvc

echo [build] No Visual C++ toolset detected.
echo [build] Installing VS Build Tools with the "Desktop development with C++"
echo [build] workload -- one time. A UAC prompt will appear; click Yes. This
echo [build] downloads several GB and may take a few minutes.
set "BOOTSTRAP=%TEMP%\vs_BuildTools.exe"
powershell -NoProfile -ExecutionPolicy Bypass -Command "try { $ProgressPreference='SilentlyContinue'; [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -Uri 'https://aka.ms/vs/17/release/vs_BuildTools.exe' -OutFile '%BOOTSTRAP%' } catch { Write-Error $_; exit 1 }" || goto :fail_vs_dl
powershell -NoProfile -ExecutionPolicy Bypass -Command "try { $p=Start-Process -FilePath '%BOOTSTRAP%' -ArgumentList '--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive --wait --norestart' -Verb RunAs -Wait -PassThru; exit $p.ExitCode } catch { Write-Error $_; exit 1 }"
del "%BOOTSTRAP%" >nul 2>&1
call :find_vs
if not defined VSINSTALL goto :fail_vs_install

:import_msvc
if not exist "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" goto :fail_vs
echo [build] Importing MSVC x64 environment from:
echo         %VSINSTALL%
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat"
where cl >nul 2>&1 || goto :fail_vs

REM --- 2. Install (once) the exact CMake that ships with Ubuntu 22.04 ---------
set "CMAKE_VER=3.22.1"
set "TOOLS=%ROOT%tools"
set "CMAKE_HOME=%TOOLS%\cmake-%CMAKE_VER%-windows-x86_64"
set "CMAKE=%CMAKE_HOME%\bin\cmake.exe"
set "ZIP=%TOOLS%\cmake-%CMAKE_VER%-windows-x86_64.zip"
set "URL=https://github.com/Kitware/CMake/releases/download/v%CMAKE_VER%/cmake-%CMAKE_VER%-windows-x86_64.zip"
if exist "%CMAKE%" (
    echo [build] Using cached CMake %CMAKE_VER% from tools\ ^(no download needed^).
) else (
    echo [build] Installing CMake %CMAKE_VER% into tools\ ^(one time^)...
    if not exist "%TOOLS%" mkdir "%TOOLS%"
    powershell -NoProfile -ExecutionPolicy Bypass -Command "try { $ProgressPreference='SilentlyContinue'; [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -Uri '%URL%' -OutFile '%ZIP%' } catch { Write-Error $_; exit 1 }" || goto :fail_dl
    powershell -NoProfile -ExecutionPolicy Bypass -Command "try { $ProgressPreference='SilentlyContinue'; Expand-Archive -Force -Path '%ZIP%' -DestinationPath '%TOOLS%' } catch { Write-Error $_; exit 1 }" || goto :fail_zip
    del "%ZIP%" >nul 2>&1
)
"%CMAKE%" --version | findstr /i "version" || goto :fail_cmake

REM --- 3. Configure (first run only) + incremental Release build --------------
if exist "%ROOT%build\CMakeCache.txt" (
    echo [build] Reusing existing build\ configuration; deps are cached, incremental build.
) else (
    echo [build] First configure: fetching + building yaml-cpp once...
    "%CMAKE%" -S "%ROOT%." -B "%ROOT%build" -A x64 || goto :fail_cfg
)
echo [build] Building Release ^(only changed files recompile^)...
"%CMAKE%" --build "%ROOT%build" --config Release || goto :fail_build

echo.
echo [build] SUCCESS.
echo [build]   Executable: %ROOT%build\Release\line_scanner.exe
echo.
echo [build]   Run it (cmd):
echo             "%ROOT%build\Release\line_scanner.exe" "%ROOT%pipeline.yaml"
echo.
echo [build]   Run it (PowerShell -- note the leading ^&):
echo             ^& "%ROOT%build\Release\line_scanner.exe" "%ROOT%pipeline.yaml"
endlocal
exit /b 0

REM ---- subroutine: locate a VS install that has the x64 C++ toolset ---------
:find_vs
set "VSINSTALL="
if not exist "%VSWHERE%" goto :eof
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
goto :eof

REM ---- error handlers -------------------------------------------------------
:fail_vs
echo [build] ERROR: the MSVC environment could not be initialized.
exit /b 1
:fail_vs_dl
echo [build] ERROR: could not download the VS Build Tools installer.
echo         Check internet/proxy, or install VS 2022 / Build Tools with the
echo         "Desktop development with C++" workload manually, then re-run.
exit /b 1
:fail_vs_install
echo [build] ERROR: MSVC still not detected after the installer ran.
echo         The UAC prompt may have been declined, or the install failed.
echo         Re-run build.bat, or install the "Desktop development with C++"
echo         workload manually via the Visual Studio Installer.
exit /b 1
:fail_dl
echo [build] ERROR: could not download CMake %CMAKE_VER% from:
echo         %URL%
echo         Check your internet connection / proxy and re-run.
exit /b 1
:fail_zip
echo [build] ERROR: could not extract %ZIP%
exit /b 1
:fail_cmake
echo [build] ERROR: the pinned cmake.exe failed to run at %CMAKE%
exit /b 1
:fail_cfg
echo [build] ERROR: CMake configure failed (see output above).
exit /b 1
:fail_build
echo [build] ERROR: build failed (see compiler output above).
exit /b 1
