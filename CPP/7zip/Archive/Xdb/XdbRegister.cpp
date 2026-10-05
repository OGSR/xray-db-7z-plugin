#include "StdAfx.h"

#include "7zip/Common/RegisterArc.h"

#include "XdbHandler.h"

namespace NArchive {
namespace NXdb {

constexpr Byte k_Signature[]{0}; //unused

REGISTER_ARC_IO(
    "xdb",
    "",
    nullptr,         // AddExt
    0xB0,            // Id
    k_Signature, 0,  // Signature and SignatureSize
    0,               // Flags
    0,               // Time flags
    nullptr)         // IsArc function

} // NXdb
} // NArchive
