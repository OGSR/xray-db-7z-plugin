#include "StdAfx.h"

#include "7zip/Common/RegisterArc.h"

#include "XdbHandler.h"

namespace NArchive {
namespace NXdb {

static const Byte k_Signature[] = { 0x9A, 0x02, 0x00, 0x00, 0x9B };
static const char k_Exts[] =
    "db db0 db1 db2 db3 db4 db5 db6 db7 db8 db9 "
    "xdb xdb0 xdb1 xdb2 xdb3 xdb4 xdb5 xdb6 xdb7 xdb8 xdb9";

REGISTER_ARC_I(
    "XDB",
    k_Exts,
    NULL,            // AddExt
    0xB0,            // Id
    k_Signature, 0,  // Signature and SignatureSize
    NArcInfoFlags::kFindSignature, // Flags
    NULL)            // IsArc function

} // NXdb
} // NArchive
