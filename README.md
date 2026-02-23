# Xray DB 7z Plugin

## 📜 Description

**Xray DB 7z Plugin** is a 7‑Zip format handler for S.T.A.L.K.E.R. database archives (XDB / 11xx / 2215 / 2945 / 2947 RU & WW). It lets you open, extract, update, delete, and create archives directly in 7‑Zip while preserving scrambling, compression, and userdata.

## ✨ Features

- Read/write support for XDB, 2947RU, 2947WW, 2945, 2215, 1114, 1154 variants
- Auto‑detects format on open; configurable override for new archives
- Honors per‑entry compression vs. store and keeps scrambling rules intact
- Optional userdata embedding; shown read‑only in 7‑Zip as **UserData**
- Duplicate-path control (dedup on by default) and delete support
- Safety checks: offset bounds, CRC verification on read, header size cap

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
2. **Configure & build (x64)**
   ```pwsh
   cmake -S . -B _build/x64 -G "Visual Studio 17 2022" -A x64
   cmake --build _build/x64 --config Release
   ```
   Win32 build: replace `-A x64` with `-A Win32` and use `_build/Win32`.

3. **Result**
   - `_build/x64/bin/Release/XDB_x64.dll`
   - `_build/Win32/bin/Release/XDB_x86.dll`

## 🧭 Usage in 7‑Zip

Choose format **xdb**. Parameters go into 7‑Zip’s **Parameters** box (comma‑separated or space‑separated):

- `compress=auto|true|false` (default `auto`): compress new/modified files based on existing archive mix (`auto`), always (`true`), or never (`false`).
- `dbver=auto|xdb|2947ru|2947ww|2945|2215|11xx` (default `auto`): force a variant for new archives or override detection.
- `keepdups=true|false` (default `false`): keep duplicate paths or keep only the last occurrence.
- `xdb_ud=PATH`: embed userdata from a file (bytes copied verbatim; only written for XDB).

Archive properties shown in 7‑Zip:
- **Userdata**: full userdata text
- **DB-Format**: detected variant

Other behavior:
- Deletes: items marked “anti” in the update callback are removed on save.
- CRC: verified on read when CRC is present.
- Header safety: decompression capped and validated to avoid runaway buffers.

## 📂 Layout

- `CPP/7zip/Archive/Xdb` - plugin sources
- `third_party/7zip` - upstream 7‑Zip sources
- `third_party/xray_re-tools` - legacy helper code

## 📄 License

* Plugin-specific code: MIT (see `LICENSE`)
* Third-party code keeps its own licenses (see `NOTICE`)
