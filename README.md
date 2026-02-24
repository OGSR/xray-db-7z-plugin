# Xray DB 7z Plugin
[![](https://img.shields.io/github/downloads/Tosox/xray-db-7z-plugin/total?label=Downloads&style=for-the-badge&color=green)](https://github.com/Tosox/xray-db-7z-plugin/releases)

## 📜 Description

**Xray DB 7z Plugin** lets you work with S.T.A.L.K.E.R. game archives in 7-Zip like normal archives. You can browse files, edit content, and build new mod archives without extra tools.

## 📋 Features

- Open, extract, and create S.T.A.L.K.E.R. DB archives directly with 7-Zip
- Supports XDB, 2947WW, 2947RU, 2945, 2215, 1114 and 1154 archives
- Edit archives in place (add, replace, rename, delete files and folders)
- Keeps game-compatible archive behavior (compression, scrambling, userdata handling)
- Auto-detects archive format and uses sensible defaults for new archives
- Optional userdata from file, with automatic fallback userdata for new XDB archives

## 📦 Installation

1) Download the latest release zip from GitHub
2) Pick the DLL that matches your 7‑Zip install:  
   - `XDB_x64.dll` for 64‑bit 7‑Zip (most installs)
   - `XDB_x86.dll` only if you explicitly run 32‑bit 7‑Zip
3) Copy the DLL into your 7‑Zip `Formats` directory (e.g., `C:\Program Files\7-Zip\Formats`). Create the `Formats` folder if it isn’t there.  

## 🛠 Build

1. **Fetch submodules**
   ```pwsh
   git submodule update --init --recursive
   ```
2. **Configure & build**
   ```pwsh
   cmake -S . -B _build/x64 -G "Visual Studio 17 2022" -A x64
   cmake --build _build/x64 --config Release
   ```
   * Win32 build: replace `-A x64` with `-A Win32` and use `_build/Win32`.

   * Tested supported generators:
     - `Visual Studio 17 2022`
     - `Visual Studio 18 2026`

3. **Result**
   - `_build/x64/bin/Release/XDB_x64.dll`
   - `_build/Win32/bin/Release/XDB_x86.dll`

## 🧭 Usage in 7‑Zip

Parameters:
- `compress=auto|true|false` (default `auto`)
  - Existing archive: `auto` follows the archive's current mix of compressed vs stored files
  - New archive: `auto` defaults to `true`
- `dbver=auto|xdb|2947ww|2947ru|2945|2215|11xx` (default `auto`)
  - Existing archive: `auto` keeps the detected format
  - New archive: `auto` defaults to `xdb`
- `xdb_ud=PATH`: embed userdata from a file (bytes copied verbatim; only written for XDB)
  - If the path contains spaces, quote it with either `"` or `'`
- `gen_xdb_ud=true|false` (default `true`): if no `xdb_ud` is provided and the selected format resolves to `xdb`, a fallback userdata block is generated automatically
  - If `xdb_ud` is provided, it is always used instead of the generated fallback
- `keepdups=true|false` (default `false`): keep duplicate paths or keep only the last occurrence (used for incorrect archives)

Game format notes:
- Most S.T.A.L.K.E.R. games use `xdb`
- **Shadow of Chernobyl** uses `2947ww` (World Wide) or `2947ru` (Russian release)

Examples:
- Add to `Parameters` field: `compress=true xdb_ud="D:\Mods\my mod\userdata.ltx"`
- Add to `Parameters` field: `compress=false dbver=2947ww xdb_ud='D:\Mods\my mod\userdata.ltx'`
- `7z.exe`: `7z a mymod.db0 .\gamedata\* -txdb -mdbver=xdb`
- `7z.exe`: `7z a mymod.db0 .\gamedata\* -txdb -mdbver=xdb -mgen_xdb_ud=false`

Archive properties shown in 7‑Zip:
- **Userdata**: full userdata text
- **DB-Format**: detected variant

## 📂 Layout

- `CPP/7zip/Archive/Xdb` - plugin sources
- `third_party/7zip` - upstream 7‑Zip sources
- `third_party/xray_re-tools` - legacy helper code

## 📄 License

* Plugin-specific code: MIT (see `LICENSE`)
* Third-party code keeps its own licenses (see `NOTICE`)
