#pragma once

#include "Common/MyBuffer.h"
#include "Common/MyCom.h"
#include "Common/MyString.h"
#include "Common/MyTypes.h"

#include "Archive/IArchive.h"
#include <vector>

namespace NArchive {
namespace NXdb {

struct CItem
{
    bool IsDir = false;
    bool UseLzhuf = false; // for legacy variants (11XX)
    UString Path;
    UInt64 Offset = 0;
    UInt64 Size = 0;
    UInt64 PackSize = 0;
    UInt32 Crc = 0;
};

Z7_CLASS_IMP_CHandler_IInArchive_2(ISetProperties, IOutArchive)

public:
    enum class DBVersion
    {
        UNKNOWN = 0,
        V11XX,
        V2215,
        V2945,
        V2947RU,
        V2947WW,
        XDB
    };

private:
    CMyComPtr<IInStream> _stream;
    UInt64 _phySize = 0;
    CObjectVector<CItem> _items;
    bool _dedupPaths = true;
    enum class ECompressMode { Auto, Always, Never } _compressMode = ECompressMode::Auto;
    bool _autoCompressDefault = true; // fallback for new/empty archives (auto mode)
    bool _genXdbUserData = true;
    bool _hasUserData = false;
    std::vector<Byte> _userData;
    DBVersion _dbVersion = DBVersion::UNKNOWN;
    DBVersion _dbForce = DBVersion::UNKNOWN; // user override via dbver property

    HRESULT Parse();
};

} // namespace NXdb
} // namespace NArchive
