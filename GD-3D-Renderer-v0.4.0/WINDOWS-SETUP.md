# Windows build setup

Geode's Windows toolchain uses Clang + Ninja. LLVM does not include the Windows SDK / CRT, so install Visual Studio Build Tools too. Full Visual Studio IDE is not required.

## 1. Install LLVM and Ninja

Open PowerShell and run:

```powershell
winget install --exact --id LLVM.LLVM
winget install --exact --id Ninja-build.Ninja
```

Close and reopen PowerShell after installation. Confirm:

```powershell
clang --version
clang++ --version
ninja --version
cmake --version
geode --version
```

## 2. Install Visual Studio Build Tools

Install **Build Tools for Visual Studio 2022 or newer** from:
https://visualstudio.microsoft.com/downloads/

In the installer, select **Desktop development with C++**. Ensure the MSVC x64/x86 build tools and a Windows 10 or Windows 11 SDK are selected. The full Visual Studio editor is optional.

## 3. Install Geode SDK binaries

Open a new PowerShell window and run:

```powershell
geode sdk install-binaries
```

If `GEODE_SDK` is blank, check the Geode CLI setup / SDK path and reopen the terminal.

## 4. Build this mod

Double-click `build.bat`, or run this in the project folder:

```powershell
geode build --ninja
```

The `--ninja` option avoids the NMake generator/platform mismatch. CMake generator caches are not interchangeable; the included build script automatically removes the old `build` folder only when it detects an NMake cache.
