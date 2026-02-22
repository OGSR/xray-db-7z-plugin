#pragma once

#include "Common/MyBuffer.h"
#include "Common/MyCom.h"
#include "Common/MyString.h"
#include "Common/MyTypes.h"

#include "Archive/IArchive.h"

namespace NArchive {
namespace NXdb {

struct CItem
{
    bool IsDir = false;
    UString Path;
    UInt64 Offset = 0;
    UInt64 Size = 0;
    UInt64 PackSize = 0;
    UInt32 Crc = 0;
};

Z7_CLASS_IMP_CHandler_IInArchive_1(ISetProperties)

private:
    CMyComPtr<IInStream> _stream;
    UInt64 _phySize = 0;
    CObjectVector<CItem> _items;
    bool _includeUserData = false;

    HRESULT Parse();
};

} // namespace NXdb
} // namespace NArchive
