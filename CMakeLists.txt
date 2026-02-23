cmake_minimum_required(VERSION 3.16)
project(xdb_plugin LANGUAGES C CXX)

# Standalone XDB format plugin for 7-Zip.
# Builds produce platform-suffixed DLLs:
#   x64  -> cmake -S . -B _build/x64 -G "Visual Studio 17 2022" -A x64
#           cmake --build _build/x64 --config Release
#   Win32-> cmake -S . -B _build/Win32 -G "Visual Studio 17 2022" -A Win32
#           cmake --build _build/Win32 --config Release
# Result: _build/<arch>/bin/Release/XDB_<arch>.dll (drop into 7-Zip/Formats)

set(ROOT ${CMAKE_CURRENT_LIST_DIR})
set(Z7_ROOT ${ROOT}/third_party/7zip)

if (NOT EXISTS "${Z7_ROOT}/CPP/7zip/Archive/IArchive.h")
  message(FATAL_ERROR "third_party/7zip is missing. Run:\n  git submodule update --init --recursive")
endif()

set(SRC_XDB
  ${ROOT}/CPP/7zip/Archive/Xdb/XdbHandler.cpp
  ${ROOT}/CPP/7zip/Archive/Xdb/XdbRegister.cpp
  ${ROOT}/third_party/xray_re-tools/sources/utils/converter/lzo/minilzo.c
  ${ROOT}/third_party/xray_re-tools/sources/xray_re/xr_lzhuf.cxx
  ${ROOT}/third_party/xray_re-tools/sources/xray_re/xr_scrambler.cxx
  ${Z7_ROOT}/CPP/7zip/Archive/ArchiveExports.cpp
  ${Z7_ROOT}/CPP/7zip/Archive/DllExports2.cpp
)

set(SRC_AR_COMMON
  ${Z7_ROOT}/CPP/7zip/Archive/Common/CoderMixer2.cpp
  ${Z7_ROOT}/CPP/7zip/Archive/Common/HandlerOut.cpp
  ${Z7_ROOT}/CPP/7zip/Archive/Common/InStreamWithCRC.cpp
  ${Z7_ROOT}/CPP/7zip/Archive/Common/ItemNameUtils.cpp
  ${Z7_ROOT}/CPP/7zip/Archive/Common/OutStreamWithCRC.cpp
  ${Z7_ROOT}/CPP/7zip/Archive/Common/ParseProperties.cpp
)

set(SRC_7Z_COMMON
  ${Z7_ROOT}/CPP/7zip/Common/CreateCoder.cpp
  ${Z7_ROOT}/CPP/7zip/Common/CWrappers.cpp
  ${Z7_ROOT}/CPP/7zip/Common/InBuffer.cpp
  ${Z7_ROOT}/CPP/7zip/Common/InOutTempBuffer.cpp
  ${Z7_ROOT}/CPP/7zip/Common/FilterCoder.cpp
  ${Z7_ROOT}/CPP/7zip/Common/LimitedStreams.cpp
  ${Z7_ROOT}/CPP/7zip/Common/MethodId.cpp
  ${Z7_ROOT}/CPP/7zip/Common/MethodProps.cpp
  ${Z7_ROOT}/CPP/Common/UTFConvert.cpp
  ${Z7_ROOT}/CPP/7zip/Common/OutBuffer.cpp
  ${Z7_ROOT}/CPP/7zip/Common/ProgressUtils.cpp
  ${Z7_ROOT}/CPP/7zip/Common/PropId.cpp
  ${Z7_ROOT}/CPP/7zip/Common/StreamBinder.cpp
  ${Z7_ROOT}/CPP/7zip/Common/StreamObjects.cpp
  ${Z7_ROOT}/CPP/7zip/Common/StreamUtils.cpp
  ${Z7_ROOT}/CPP/7zip/Common/UniqBlocks.cpp
  ${Z7_ROOT}/CPP/7zip/Common/VirtThread.cpp
)

set(SRC_COMMON
  ${Z7_ROOT}/CPP/Common/CRC.cpp
  ${Z7_ROOT}/CPP/Common/CrcReg.cpp
  ${Z7_ROOT}/CPP/Common/IntToString.cpp
  ${Z7_ROOT}/CPP/Common/LzFindPrepare.cpp
  ${Z7_ROOT}/CPP/Common/NewHandler.cpp
  ${Z7_ROOT}/CPP/Common/MyString.cpp
  ${Z7_ROOT}/CPP/Common/StringConvert.cpp
  ${Z7_ROOT}/CPP/Common/StringToInt.cpp
  ${Z7_ROOT}/CPP/Common/MyVector.cpp
  ${Z7_ROOT}/CPP/Common/Wildcard.cpp
  ${Z7_ROOT}/CPP/Common/StdInStream.cpp
  ${Z7_ROOT}/CPP/Common/StdOutStream.cpp
)

set(SRC_WINDOWS
  ${Z7_ROOT}/CPP/Windows/FileDir.cpp
  ${Z7_ROOT}/CPP/Windows/FileFind.cpp
  ${Z7_ROOT}/CPP/Windows/FileIO.cpp
  ${Z7_ROOT}/CPP/Windows/FileName.cpp
  ${Z7_ROOT}/CPP/Windows/PropVariant.cpp
  ${Z7_ROOT}/CPP/Windows/Synchronization.cpp
  ${Z7_ROOT}/CPP/Windows/System.cpp
  ${Z7_ROOT}/CPP/Windows/TimeUtils.cpp
)

set(SRC_COMPRESS
  ${Z7_ROOT}/CPP/7zip/Compress/CodecExports.cpp
  ${Z7_ROOT}/CPP/7zip/Compress/CopyCoder.cpp
  ${Z7_ROOT}/CPP/7zip/Compress/CopyRegister.cpp
)

set(SRC_C
  ${Z7_ROOT}/C/Alloc.c
  ${Z7_ROOT}/C/CpuArch.c
  ${Z7_ROOT}/C/LzFind.c
  ${Z7_ROOT}/C/7zCrc.c
  ${Z7_ROOT}/C/7zCrcOpt.c
  ${Z7_ROOT}/C/Threads.c
)

add_library(Xdb SHARED
  ${SRC_XDB}
  ${SRC_AR_COMMON}
  ${SRC_7Z_COMMON}
  ${SRC_COMMON}
  ${SRC_WINDOWS}
  ${SRC_COMPRESS}
  ${SRC_C}
)

target_include_directories(Xdb PRIVATE
  ${ROOT}/CPP/7zip/Archive/Xdb
  ${ROOT}/third_party/xray_re-tools/sources/utils/converter/lzo
  ${ROOT}/third_party/xray_re-tools/sources/xray_re
  ${Z7_ROOT}/CPP
  ${Z7_ROOT}/CPP/include
  ${Z7_ROOT}/CPP/Common
  ${Z7_ROOT}/CPP/7zip
  ${Z7_ROOT}/CPP/7zip/Archive
  ${Z7_ROOT}/CPP/7zip/Common
  ${Z7_ROOT}/CPP/Windows
  ${Z7_ROOT}/CPP/Windows/Control
  ${Z7_ROOT}/C
)

target_compile_definitions(Xdb PRIVATE
  _WIN32_WINNT=0x0601
  UNICODE
  _UNICODE
  _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
  target_compile_options(Xdb PRIVATE /W3 /EHsc)
endif()

set_target_properties(Xdb PROPERTIES
  OUTPUT_NAME "XDB_$<IF:$<EQUAL:${CMAKE_SIZEOF_VOID_P},8>,x64,x86>"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Ensure required 7-Zip exports are present
target_link_options(Xdb PRIVATE "/DEF:${Z7_ROOT}/CPP/7zip/Archive/Archive2.def")
