#include "StdAfx.h"

#include "7zip/Common/RegisterArc.h"

#include "XdbHandler.h"

namespace NArchive {
namespace NXdb {

static const Byte k_Signature[] = { 0x9A, 0x02, 0x00, 0x00, 0x9B };
static const char k_Exts[] = "db0 db1 db2 db3 db4 db5 db6 db7 db8 db9";

REGISTER_ARC_IO(
    "xdb",
    k_Exts,
    NULL,            // AddExt
    0xB0,            // Id
    k_Signature, 0,  // Signature and SignatureSize
    NArcInfoFlags::kFindSignature, // Flags
    0,               // Time flags
    NULL)            // IsArc function

} // NXdb
} // NArchive
