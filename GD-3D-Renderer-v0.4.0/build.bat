@echo off
setlocal

rem Defaults for the user's Geode setup; a Developer Command Prompt still
rem provides the MSVC and Windows SDK library environment.
if not defined GEODE_SDK if exist "C:\GeodeSDK\CMakeLists.txt" set "GEODE_SDK=C:\GeodeSDK"
if exist "C:\Program Files\LLVM\bin\clang.exe" set "PATH=C:\Program Files\LLVM\bin;%PATH%"

where geode >nul 2>nul
if errorlevel 1 (
  echo ERROR: Geode CLI was not found in PATH.
  echo Install it with: winget install --exact --id GeodeSDK.GeodeCLI
  pause
  exit /b 1
)
where cmake >nul 2>nul
if errorlevel 1 (
  echo ERROR: CMake was not found in PATH.
  pause
  exit /b 1
)
where clang >nul 2>nul
if errorlevel 1 (
  echo ERROR: Clang was not found in PATH.
  echo Install LLVM and add C:\Program Files\LLVM\bin to PATH.
  pause
  exit /b 1
)
where clang++ >nul 2>nul
if errorlevel 1 (
  echo ERROR: clang++ was not found in PATH.
  pause
  exit /b 1
)
where ninja >nul 2>nul
if errorlevel 1 (
  echo ERROR: Ninja was not found in PATH.
  pause
  exit /b 1
)
if not defined GEODE_SDK (
  echo ERROR: GEODE_SDK is not set and C:\GeodeSDK was not found.
  echo Run: set "GEODE_SDK=C:\GeodeSDK"
  pause
  exit /b 1
)

rem CMake generator caches cannot be reused across generators.
if exist "build\CMakeCache.txt" (
  findstr /C:"CMAKE_GENERATOR:INTERNAL=NMake Makefiles" "build\CMakeCache.txt" >nul
  if not errorlevel 1 (
    echo Removing stale NMake build cache...
    rmdir /s /q "build"
  )
)

echo Building GD 3D Renderer v0.4.0 with Clang + Ninja...
geode build --ninja
if errorlevel 1 (
  echo.
  echo Build failed. Copy the FIRST compiler error and send it back.
) else (
  echo.
  echo Build completed. Check the output above for the .geode package.
)
pause
