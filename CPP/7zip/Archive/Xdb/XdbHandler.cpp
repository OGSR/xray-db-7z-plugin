#include "StdAfx.h"

#include "Common/ComTry.h"
#include "Common/StringConvert.h"
#include "Windows/PropVariant.h"
#include "7zip/Common/ProgressUtils.h"
#include "7zip/Common/RegisterArc.h"
#include "7zip/Common/StreamUtils.h"

#include "XdbHandler.h"
#include "xr_lzhuf.h"

extern "C" {
#include "minilzo.h"
}

#include <algorithm>
#include <string>
#include <sstream>
#include <vector>
#include <cstdlib>
#include <cstdio>

// Minimal hook so xr_lzhuf links; aborts on fatal.
namespace xray_re {
void die(const char* message, const char* file, unsigned line) {
    std::printf("xr_lzhuf die: %s:%u: %s\n", file, line, message);
    std::abort();
}
} // namespace xray_re

namespace NArchive {
namespace NXdb {

static const UInt32 kChunkCompressedFlag = 0x80000000u;
static const UInt32 kChunkIdMask         = ~kChunkCompressedFlag;

static const UInt32 kDbChunkData      = 0x00000000; // DB_CHUNK_DATA
static const UInt32 kDbChunkUserData  = 0x0000029A; // DB_CHUNK_HEADER (userdata)
static const UInt32 kDbChunkHeader    = 0x00000001; // DB_CHUNK_HEADER

static const Byte kProps[] =
{
    kpidPath,
    kpidSize,
    kpidPackSize,
    kpidCRC,
    kpidIsDir
};

static const Byte kArcProps[] =
{
    kpidPhySize
};

IMP_IInArchive_Props
IMP_IInArchive_ArcProps

static inline UInt16 ReadLE16(const Byte *p)
{
    return (UInt16)p[0] | ((UInt16)p[1] << 8);
}

static inline UInt32 ReadLE32(const Byte *p)
{
    return (UInt32)p[0]
        | ((UInt32)p[1] << 8)
        | ((UInt32)p[2] << 16)
        | ((UInt32)p[3] << 24);
}

static HRESULT SeekAbs(IInStream *s, UInt64 pos)
{
    UInt64 newPos = 0;
    return s->Seek((Int64)pos, STREAM_SEEK_SET, &newPos);
}

static HRESULT GetSize(IInStream *s, UInt64 &sizeOut)
{
    UInt64 cur = 0, end = 0;
    RINOK(s->Seek(0, STREAM_SEEK_CUR, &cur));
    RINOK(s->Seek(0, STREAM_SEEK_END, &end));
    RINOK(s->Seek((Int64)cur, STREAM_SEEK_SET, &cur));
    sizeOut = end;
    return S_OK;
}

static HRESULT ReadExact(IInStream *s, void *buf, size_t size)
{
    Byte *p = (Byte*)buf;
    size_t remaining = size;
    while (remaining)
    {
        UInt32 processed = 0;
        RINOK(s->Read(p, (UInt32)remaining, &processed));
        if (processed == 0)
            return S_FALSE;
        p += processed;
        remaining -= processed;
    }
    return S_OK;
}

// For signature probing: walk all chunks and ensure data+header exist and header is plausible.
static HRESULT FindChunks(IInStream *s, UInt64 phySize,
                          UInt64 &dataChunkPos, UInt64 &dataChunkSize,
                          UInt64 &hdrChunkPos,  UInt64 &hdrChunkSize,  UInt32 &hdrChunkIdRaw,
                          UInt64 &userChunkPos, UInt64 &userChunkSize)
{
    dataChunkPos = dataChunkSize = 0;
    hdrChunkPos  = hdrChunkSize  = 0;
    hdrChunkIdRaw = 0;
    userChunkPos = userChunkSize = 0;

    UInt64 pos = 0;
    while (pos + 8 <= phySize)
    {
        Byte hdr[8];
        RINOK(SeekAbs(s, pos));
        RINOK(ReadExact(s, hdr, 8));

        const UInt32 chunkIdRaw = ReadLE32(hdr);
        const UInt32 chunkSize  = ReadLE32(hdr + 4);

        const UInt64 dataPos = pos + 8;
        const UInt64 nextPos = dataPos + (UInt64)chunkSize;
        if (nextPos > phySize)
            return S_FALSE;

        const UInt32 chunkId = (chunkIdRaw & kChunkIdMask);

        if (chunkId == kDbChunkData)
        {
            dataChunkPos = dataPos;
            dataChunkSize = chunkSize;
        }
        else if (chunkId == kDbChunkHeader)
        {
            hdrChunkPos = dataPos;
            hdrChunkSize = chunkSize;
            hdrChunkIdRaw = chunkIdRaw;
        }
        else if (chunkId == kDbChunkUserData)
        {
            userChunkPos = dataPos;
            userChunkSize = chunkSize;
        }

        pos = nextPos;

        // Only return success when *real* header + data found:
        if (dataChunkPos && hdrChunkPos)
            return S_OK;
    }

    return S_FALSE;
}

Z7_COM7F_IMF(CHandler::Open(IInStream *stream, const UInt64 *, IArchiveOpenCallback *))
{
    COM_TRY_BEGIN
    _stream = stream;
    _items.Clear();
    RINOK(GetSize(_stream, _phySize));
    return Parse();
    COM_TRY_END
}

Z7_COM7F_IMF(CHandler::Close())
{
    _items.Clear();
    _stream.Release();
    _phySize = 0;
    return S_OK;
}

HRESULT CHandler::Parse()
{
    if (!_stream)
        return E_FAIL;

    UInt64 dataPos = 0, dataSize = 0, hdrPos = 0, hdrSize = 0;
    UInt64 userPos = 0, userSize = 0;
    UInt32 hdrIdRaw = 0;

    const HRESULT fr = FindChunks(_stream, _phySize,
                                  dataPos, dataSize,
                                  hdrPos, hdrSize, hdrIdRaw,
                                  userPos, userSize);
    if (fr != S_OK)
        return S_FALSE;

    // Read header chunk payload
    std::vector<Byte> hdrComp((size_t)hdrSize);
    RINOK(SeekAbs(_stream, hdrPos));
    RINOK(ReadExact(_stream, hdrComp.data(), hdrComp.size()));

    // Decompress header if needed (LZHUF)
    std::vector<Byte> hdrPlain;
    const bool hdrCompressed = (hdrIdRaw & kChunkCompressedFlag) != 0;

    if (hdrCompressed)
    {
        size_t outSize = 0;
        uint8_t *outPtr = nullptr;
        xray_re::xr_lzhuf::decompress(outPtr, outSize,
            reinterpret_cast<const uint8_t*>(hdrComp.data()), hdrComp.size());
        if (!outPtr || outSize == 0)
            return S_FALSE;
        hdrPlain.assign(outPtr, outPtr + outSize);
        free(outPtr); // allocated via malloc/realloc inside xr_lzhuf
    }
    else
    {
        hdrPlain.swap(hdrComp);
    }

    _items.Clear();

    const Byte *p = hdrPlain.data();
    const Byte *end = p + hdrPlain.size();

    while (p < end)
    {
        // Need at least u16 + 4*u32 (min) before name
        if ((size_t)(end - p) < 2 + 4 + 4 + 4 + 4)
            break;

        const UInt16 nameSizeField = ReadLE16(p); p += 2;
        if (nameSizeField < 16)
            return S_FALSE;

        const UInt32 nameLen = (UInt32)nameSizeField - 16;

        const UInt32 sizeReal = ReadLE32(p); p += 4;
        const UInt32 sizeComp = ReadLE32(p); p += 4;
        const UInt32 crc      = ReadLE32(p); p += 4;

        if ((size_t)(end - p) < (size_t)nameLen + 4)
            return S_FALSE;

        std::string nameA((const char*)p, (size_t)nameLen);
        p += nameLen;

        const UInt32 offsetAbs = ReadLE32(p); p += 4;

        std::replace(nameA.begin(), nameA.end(), '\\', '/');

        CItem item;
        item.IsDir = (offsetAbs == 0);
        item.Path = MultiByteToUnicodeString(nameA.c_str(), CP_ACP); // XRay paths are typically ANSI/ASCII
        item.Offset = offsetAbs;                                     // IMPORTANT: absolute offset
        item.Size = sizeReal;
        item.PackSize = sizeComp;
        item.Crc = crc;

        // Some headers may include empty entries; optionally skip them:
        if (item.Path.IsEmpty())
            continue;

        _items.Add(item);
    }

    // Expose userdata chunk as a stored file if present
    if (_includeUserData && userPos && userSize)
    {
        CItem userItem;
        userItem.IsDir = false;
        userItem.Path = L"_userdata.ltx";
        userItem.Offset = userPos;
        userItem.Size = userSize;
        userItem.PackSize = userSize;
        userItem.Crc = 0;
        _items.Add(userItem);
    }

    return _items.IsEmpty() ? S_FALSE : S_OK;
}

Z7_COM7F_IMF(CHandler::GetNumberOfItems(UInt32 *numItems))
{
    *numItems = (UInt32)_items.Size();
    return S_OK;
}

Z7_COM7F_IMF(CHandler::GetProperty(UInt32 index, PROPID propID, PROPVARIANT *value))
{
    COM_TRY_BEGIN
    NWindows::NCOM::CPropVariant prop;
    const CItem &it = _items[(int)index];

    switch (propID)
    {
        case kpidPath:      prop = it.Path; break;
        case kpidIsDir:     prop = it.IsDir; break;
        case kpidSize:      if (!it.IsDir) prop = (UInt64)it.Size; break;
        case kpidPackSize:  if (!it.IsDir) prop = (UInt64)it.PackSize; break;
        case kpidCRC:       if (!it.IsDir) prop = (UInt32)it.Crc; break;
        default: break;
    }

    prop.Detach(value);
    return S_OK;
    COM_TRY_END
}

Z7_COM7F_IMF(CHandler::Extract(const UInt32 *indices, UInt32 numItems, Int32 testMode, IArchiveExtractCallback *ecb))
{
    COM_TRY_BEGIN
    if (!_stream)
        return E_FAIL;

    // init LZO once
    static bool lzoInit = false;
    if (!lzoInit)
    {
        if (lzo_init() != LZO_E_OK)
            return E_FAIL;
        lzoInit = true;
    }

    for (UInt32 i = 0; i < numItems; i++)
    {
        const UInt32 index = indices ? indices[i] : i;
        const CItem &it = _items[(int)index];

        Int32 askMode = testMode ? NArchive::NExtract::NAskMode::kTest : NArchive::NExtract::NAskMode::kExtract;
        UInt32 res = 0;
        CMyComPtr<ISequentialOutStream> outStream;
        RINOK(ecb->GetStream(index, &outStream, askMode));
        if (it.IsDir)
        {
            RINOK(ecb->PrepareOperation(askMode));
            RINOK(ecb->SetOperationResult(NArchive::NExtract::NOperationResult::kOK));
            continue;
        }

        RINOK(ecb->PrepareOperation(askMode));

        // Read packed bytes at absolute offset (as db-converter does: data_full + offset)
        std::vector<Byte> packed((size_t)it.PackSize);
        RINOK(SeekAbs(_stream, it.Offset));
        RINOK(ReadExact(_stream, packed.data(), (size_t)it.PackSize));

        if (!testMode && outStream)
        {
            if (it.Size == it.PackSize)
            {
                // stored
                UInt32 processed = 0;
                RINOK(outStream->Write(packed.data(), (UInt32)packed.size(), &processed));
                if (processed != packed.size())
                    res = NArchive::NExtract::NOperationResult::kDataError;
            }
            else
            {
                // LZO1X
                std::vector<Byte> unpacked((size_t)it.Size);
                lzo_uint outLen = (lzo_uint)unpacked.size();
                const int rc = lzo1x_decompress_safe(
                    packed.data(), (lzo_uint)packed.size(),
                    unpacked.data(), &outLen,
                    nullptr);

                if (rc != LZO_E_OK || (UInt64)outLen != it.Size)
                {
                    res = NArchive::NExtract::NOperationResult::kDataError;
                }
                else
                {
                    UInt32 processed = 0;
                    RINOK(outStream->Write(unpacked.data(), (UInt32)unpacked.size(), &processed));
                    if (processed != unpacked.size())
                        res = NArchive::NExtract::NOperationResult::kDataError;
                }
            }
        }

        RINOK(ecb->SetOperationResult(res == 0 ? NArchive::NExtract::NOperationResult::kOK : res));
    }

    return S_OK;
    COM_TRY_END
}

Z7_COM7F_IMF(CHandler::GetArchiveProperty(PROPID propID, PROPVARIANT *value))
{
    COM_TRY_BEGIN
    NWindows::NCOM::CPropVariant prop;
    switch (propID)
    {
        case kpidPhySize: prop = _phySize; break;
        default: break;
    }
    prop.Detach(value);
    return S_OK;
    COM_TRY_END
}

static bool PropToBool(const PROPVARIANT &v, bool &res)
{
    switch (v.vt)
    {
        case VT_BOOL:
            res = (v.boolVal != VARIANT_FALSE); return true;
        case VT_UI1: res = (v.bVal != 0); return true;
        case VT_UI2: res = (v.uiVal != 0); return true;
        case VT_UI4: res = (v.ulVal != 0); return true;
        case VT_I4:  res = (v.lVal != 0); return true;
        case VT_BSTR:
        {
            UString s(v.bstrVal);
            s.MakeLower_Ascii();
            if (s.IsEqualTo_Ascii_NoCase("true"))
                { res = true; return true; }
            if (s.IsEqualTo_Ascii_NoCase("false"))
                { res = false; return true; }
            return false;
        }
        default:
            return false;
    }
}

Z7_COM7F_IMF(CHandler::SetProperties(const wchar_t * const *names, const PROPVARIANT *values, UInt32 numProps))
{
    COM_TRY_BEGIN

    for (UInt32 i = 0; i < numProps; i++)
    {
        UString name = names[i];
        name.MakeLower_Ascii();
        const PROPVARIANT &val = values[i];

        if (name.IsEqualTo_Ascii_NoCase("userdata"))
        {
            bool b = true;
            if (!PropToBool(val, b))
                return E_INVALIDARG;
            _includeUserData = b;
            continue;
        }

        return E_INVALIDARG; // unknown property
    }

    return S_OK;
    COM_TRY_END
}

} // NXdb
} // NArchive
