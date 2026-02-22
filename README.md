Minimal XDB 7‑Zip plugin
========================

This folder is a trimmed copy of the 7‑Zip sources containing only what’s needed to build the XDB format handler.

Prereq
- Fetch upstream 7-Zip sources as a submodule:
  ```
  git submodule add https://github.com/mcmilk/7-Zip.git third_party/7zip
  git submodule update --init --recursive
  ```

Build
1) Open “x64 Native Tools for VS 2022”.
2) Configure & build:
   ```
   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
   cmake --build build --config Release
   ```
3) DLL lands at `build/bin/XDB.dll`; drop into 7-Zip `Formats`.

Visual Studio solution
- Run `generate_vs2022.bat` (requires the submodule). It creates `build-vs/XDB.sln` you can open in Visual Studio.

Contents
- `CPP/7zip/Archive/Xdb`: XDB handler + LZHUF/LZO helpers (your custom code).
- `third_party/7zip`: upstream submodule providing all other 7‑Zip headers/sources pulled in by CMake.

Notes
- The CMake list enumerates every compiled source; if you trim further, update it accordingly.
- This tree intentionally omits all other formats, UI, and bundles to keep the plugin surface obvious.
