Minimal XDB 7‑Zip plugin
========================

This folder is a trimmed copy of the 7‑Zip sources containing only what’s needed to build the XDB format handler.

Prereq
- If the `third_party/7zip` folder is missing, fetch upstream 7-Zip sources as a submodule:
  ```
  git submodule update --init --recursive
  ```

Build
1) Configure & build:
   ```
   cmake -S . -B _build/x64 -G "Visual Studio 17 2022" -A x64
   cmake --build _build/x64 --config Release --target Xdb
   ```
2) DLL lands at `_build/x64/bin/Release/XDB_x64.dll`; drop it into the 7-Zip `Formats` directory.

Contents
- `CPP/7zip/Archive/Xdb`: XDB handler + LZHUF/LZO helpers (your custom code).
- `third_party/7zip`: upstream submodule providing all other 7‑Zip headers/sources pulled in by CMake.

Notes
- The CMake list enumerates every compiled source; if you trim further, update it accordingly.
- This tree intentionally omits all other formats, UI, and bundles to keep the plugin surface obvious.

Usage in 7‑Zip
--------------
Pick format **Xdb** when adding/creating an archive. All per‑format options are passed through the **Parameters** box (7‑Zip doesn’t offer custom UI for plugins).

Supported parameters:
- `compress=auto|true|false` (default `auto`): compression mode for new/modified files. `auto` mirrors the existing archive’s store/compress mix (for a brand‑new archive it defaults to store); `true` always compresses; `false` always stores.
- `x=<level>`: standard 7‑Zip “Compression level” value. `0` forces store, any other level forces compress (alias for `compress=true/false`).
- `keepdups=true|false` (default `false`): `true` keeps duplicate paths, `false` keeps only the last occurrence (dedup on).
- `xdb_ud=<path>`: embed userdata from an external file (bytes are copied verbatim into the userdata chunk).

Viewing userdata:
- Open the archive properties pane in 7‑Zip and read the custom **UserData** property (full text, read‑only)

Other behavior:
- Deletes: marking an item as “anti” in the update callback removes it on save.
- CRC verification: existing entries are checked when read; new data CRC is stored over uncompressed bytes.
- Safety limits: offsets/ranges validated; archive size limited to 32‑bit offsets (format constraint).
- The 7‑Zip compression level dropdown is honored via `x=` as described above.
