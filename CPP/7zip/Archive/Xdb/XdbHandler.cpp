#include "StdAfx.h"

#include "Common/ComTry.h"
#include "Common/StringConvert.h"
#include "Windows/PropVariant.h"
#include "7zip/Common/ProgressUtils.h"
#include "7zip/Common/RegisterArc.h"
#include "7zip/Common/StreamUtils.h"

#include "7zCrc.h"
#include "XdbHandler.h"
#include "xr_lzhuf.h"

extern "C" {
#include "minilzo.h"
}

#include <algorithm>
#include <string>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <cstdlib>
#include <cstdio>
#include <fstream>

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

enum
{
    kpidUserDataText = kpidUserDefined
};

static const CStatProp kArcProps[] =
{
    { NULL, kpidPhySize, VT_UI8 },
    { "UserData", kpidUserDataText, VT_BSTR }
};

IMP_IInArchive_Props
IMP_IInArchive_ArcProps_WITH_NAME

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

static inline void WriteLE16(std::vector<Byte> &v, UInt16 val)
{
    v.push_back((Byte)(val & 0xFF));
    v.push_back((Byte)((val >> 8) & 0xFF));
}

static inline void WriteLE32(std::vector<Byte> &v, UInt32 val)
{
    v.push_back((Byte)(val & 0xFF));
    v.push_back((Byte)((val >> 8) & 0xFF));
    v.push_back((Byte)((val >> 16) & 0xFF));
    v.push_back((Byte)((val >> 24) & 0xFF));
}

static HRESULT WriteAll(ISequentialOutStream *out, const void *data, size_t size)
{
    const Byte *p = (const Byte*)data;
    size_t remaining = size;
    while (remaining)
    {
        const UInt32 chunk = (UInt32)std::min<size_t>(remaining, 1 << 20);
        UInt32 written = 0;
        RINOK(out->Write(p, chunk, &written));
        if (written != chunk)
            return E_FAIL;
        p += written;
        remaining -= written;
    }
    return S_OK;
}

static HRESULT CopyFromInStream(IInStream *in, UInt64 offset, UInt64 size, ISequentialOutStream *out)
{
    static const size_t kBufSize = 1 << 16;
    Byte buf[kBufSize];
    RINOK(SeekAbs(in, offset));
    UInt64 remaining = size;
    while (remaining)
    {
        const size_t cur = (size_t)std::min<UInt64>(remaining, kBufSize);
        RINOK(ReadExact(in, buf, cur));
        RINOK(WriteAll(out, buf, cur));
        remaining -= cur;
    }
    return S_OK;
}

static inline void TrimTrailingSlashes(UString &path)
{
    while (path.Len() && (path.Back() == L'/' || path.Back() == L'\\'))
        path.DeleteBack();
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

        if (chunkId == kDbChunkData && dataChunkPos == 0)
        {
            dataChunkPos = dataPos;
            dataChunkSize = chunkSize;
        }
        else if (chunkId == kDbChunkHeader && hdrChunkPos == 0)
        {
            hdrChunkPos = dataPos;
            hdrChunkSize = chunkSize;
            hdrChunkIdRaw = chunkIdRaw;
        }
        else if (chunkId == kDbChunkUserData && userChunkPos == 0)
        {
            userChunkPos = dataPos;
            userChunkSize = chunkSize;
        }

        pos = nextPos;
    }

    return (dataChunkPos && hdrChunkPos) ? S_OK : S_FALSE;
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
    _userData.clear();
    _hasUserData = false;
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
        {
            if (outPtr)
                free(outPtr);
            return S_FALSE;
        }
        hdrPlain.assign(outPtr, outPtr + outSize);
        free(outPtr); // allocated via malloc/realloc inside xr_lzhuf
    }
    else
    {
        hdrPlain.swap(hdrComp);
    }

    _items.Clear();
    _userData.clear();
    _hasUserData = false;

    const Byte *p = hdrPlain.data();
    const Byte *end = p + hdrPlain.size();

    std::unordered_map<std::wstring, unsigned> lastByPath; // used only if dedup enabled

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
        TrimTrailingSlashes(item.Path);
        item.Offset = offsetAbs;                                     // IMPORTANT: absolute offset
        item.Size = sizeReal;
        item.PackSize = sizeComp;
        item.Crc = crc;

        // Some headers may include empty entries; optionally skip them:
        if (item.Path.IsEmpty())
            continue;

        if (_dedupPaths)
        {
            UString norm = item.Path;
            norm.MakeLower_Ascii();
            norm.Replace(L'/', L'\\');
            const std::wstring key(norm);
            auto it = lastByPath.find(key);
            if (it == lastByPath.end())
            {
                lastByPath.emplace(key, (unsigned)_items.Size());
                _items.Add(item);
            }
            else
            {
                _items[(int)it->second] = item; // replace with later occurrence (last wins)
            }
        }
        else
        {
            _items.Add(item); // keep duplicates as-is
        }
    }

    // derive default compress policy for auto mode
    unsigned stored = 0, compressed = 0;
    for (unsigned i = 0; i < _items.Size(); i++)
    {
        const CItem &it = _items[i];
        if (it.IsDir)
            continue;
        if (it.Size == it.PackSize)
            stored++;
        else
            compressed++;
    }
    _autoCompressDefault = (compressed > stored);

    if (userPos && userSize)
    {
        _userData.resize((size_t)userSize);
        RINOK(SeekAbs(_stream, userPos));
        RINOK(ReadExact(_stream, _userData.data(), _userData.size()));
        _hasUserData = true;
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

    const UInt32 totalItems = (UInt32)_items.Size();

    // treat nullptr or "-1" as "extract everything"
    if (numItems == (UInt32)-1 || indices == nullptr)
        numItems = totalItems;

    for (UInt32 i = 0; i < numItems; i++)
    {
        const UInt32 index = indices ? indices[i] : i;
        if (index >= totalItems)
            return E_INVALIDARG;

        const CItem &it = _items[(int)index];

        Int32 askMode = testMode ? NArchive::NExtract::NAskMode::kTest : NArchive::NExtract::NAskMode::kExtract;
        UInt32 res = NArchive::NExtract::NOperationResult::kOK;
        CMyComPtr<ISequentialOutStream> outStream;
        RINOK(ecb->GetStream(index, &outStream, askMode));
        if (it.IsDir)
        {
            RINOK(ecb->PrepareOperation(askMode));
            RINOK(ecb->SetOperationResult(NArchive::NExtract::NOperationResult::kOK));
            continue;
        }

        RINOK(ecb->PrepareOperation(askMode));

        const UInt64 endOffset = it.Offset + it.PackSize;
        if (endOffset < it.Offset || endOffset > _phySize)
        {
            RINOK(ecb->SetOperationResult(NArchive::NExtract::NOperationResult::kDataError));
            continue;
        }

        // Read packed bytes at absolute offset (as db-converter does: data_full + offset)
        std::vector<Byte> packed((size_t)it.PackSize);
        RINOK(SeekAbs(_stream, it.Offset));
        RINOK(ReadExact(_stream, packed.data(), packed.size()));

        const Byte *dataPtr = packed.data();
        size_t dataSize = packed.size();
        std::vector<Byte> unpacked;

        if (it.Size != it.PackSize)
        {
            // LZO1X
            unpacked.resize((size_t)it.Size);
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
                dataPtr = unpacked.data();
                dataSize = (size_t)outLen;
            }
        }

        if (res == NArchive::NExtract::NOperationResult::kOK && it.Crc != 0)
        {
            static bool crcInit = []() -> bool { CrcGenerateTable(); return true; }();
            (void)crcInit;
            const UInt32 crc = CrcCalc(dataPtr, dataSize);
            if (crc != it.Crc)
                res = NArchive::NExtract::NOperationResult::kCRCError;
        }

        if (!testMode && outStream && res == NArchive::NExtract::NOperationResult::kOK)
        {
            UInt32 processed = 0;
            RINOK(outStream->Write(dataPtr, (UInt32)dataSize, &processed));
            if (processed != dataSize)
                res = NArchive::NExtract::NOperationResult::kDataError;
        }

        RINOK(ecb->SetOperationResult(res));
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
        case kpidUserDataText:
        {
            if (_hasUserData && !_userData.empty())
            {
                std::string chunk((const char*)_userData.data(), _userData.size());
                prop = MultiByteToUnicodeString(chunk.c_str(), CP_ACP);
            }
            break;
        }
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

static bool PropToUInt32(const PROPVARIANT &v, UInt32 &res)
{
    switch (v.vt)
    {
        case VT_UI1: res = v.bVal; return true;
        case VT_UI2: res = v.uiVal; return true;
        case VT_UI4: res = v.ulVal; return true;
        case VT_I4:
            if (v.lVal < 0) return false;
            res = (UInt32)v.lVal;
            return true;
        case VT_BSTR:
        {
            const wchar_t *s = v.bstrVal;
            if (!s || !*s) return false;
            wchar_t *end = nullptr;
            unsigned long val = wcstoul(s, &end, 10);
            if (end && *end == 0)
            {
                res = (UInt32)val;
                return true;
            }
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

        if (name.IsEqualTo_Ascii_NoCase("keepdups"))
        {
            bool b = false;
            if (!PropToBool(val, b))
                return E_INVALIDARG;
            _dedupPaths = !b;
            continue;
        }
        if (name.IsEqualTo_Ascii_NoCase("xdb_ud"))
        {
            _userData.clear();
            _hasUserData = false;

            if (val.vt != VT_BSTR || val.bstrVal == nullptr)
                return E_INVALIDARG;

            AString pathA = UnicodeStringToMultiByte(UString(val.bstrVal), CP_ACP);
            std::ifstream f(pathA.Ptr(), std::ios::binary);
            if (!f)
                return E_INVALIDARG;
            f.seekg(0, std::ios::end);
            std::streamoff len = f.tellg();
            if (len < 0)
                return E_INVALIDARG;
            _userData.resize((size_t)len);
            f.seekg(0, std::ios::beg);
            if (len > 0)
                f.read((char*)_userData.data(), len);
            _hasUserData = true;
            continue;
        }
        if (name.IsEqualTo_Ascii_NoCase("x"))
        {
            // Ignore value, use "compress" instead
            continue;
        }
        if (name.IsEqualTo_Ascii_NoCase("compress"))
        {
            bool b;
            if (PropToBool(val, b))
            {
                _compressMode = b ? ECompressMode::Always : ECompressMode::Never;
                continue;
            }

            if (val.vt == VT_BSTR)
            {
                UString v(val.bstrVal);
                v.MakeLower_Ascii();
                if (v.IsEqualTo_Ascii_NoCase("auto"))
                    { _compressMode = ECompressMode::Auto; continue; }
            }

            return E_INVALIDARG;
            continue;
        }
        return E_INVALIDARG; // unknown property
    }

    return S_OK;
    COM_TRY_END
}

Z7_COM7F_IMF(CHandler::UpdateItems(ISequentialOutStream *outStream, UInt32 numItems, IArchiveUpdateCallback *cb))
{
    COM_TRY_BEGIN

    if (!outStream)
        return E_FAIL;

    struct OutItem
    {
        bool IsDir = false;
        UString Path;
        UInt64 Size = 0;
        UInt64 PackSize = 0;
        UInt32 Crc = 0;
        UInt64 Offset = 0;
        bool FromOld = false;
        UInt32 SrcIndex = 0;
        UInt64 SrcOffset = 0;
        UInt64 SrcPackSize = 0;
        std::vector<Byte> Data;
    };

    CObjectVector<OutItem> outItems;

    UInt64 dataTotal = 0;

    for (UInt32 i = 0; i < numItems; i++)
    {
        Int32 newData = 0, newProps = 0;
        UInt32 indexInArc = (UInt32)(Int32)-1;
        RINOK(cb->GetUpdateItemInfo(i, &newData, &newProps, &indexInArc));

        OutItem oi;

        const bool hasSrc = ((Int32)indexInArc != -1);
        if (hasSrc && indexInArc >= (UInt32)_items.Size())
            return E_INVALIDARG;

        const CItem *src = hasSrc ? &_items[(int)indexInArc] : nullptr;

        if (newProps || !src)
        {
            NWindows::NCOM::CPropVariant prop;
            RINOK(cb->GetProperty(i, kpidPath, &prop));
            if (prop.vt != VT_BSTR)
                return E_INVALIDARG;
            oi.Path = prop.bstrVal;
            prop.Clear();

            RINOK(cb->GetProperty(i, kpidIsDir, &prop));
            oi.IsDir = (prop.vt == VT_BOOL && prop.boolVal != VARIANT_FALSE);

            // Check anti/delete flag if provided
            if (cb->GetProperty(i, kpidIsAnti, &prop) == S_OK && prop.vt == VT_BOOL && prop.boolVal != VARIANT_FALSE)
                continue; // delete: skip adding to outItems
        }
        else
        {
            oi.Path = src->Path;
            oi.IsDir = src->IsDir;
        }

        TrimTrailingSlashes(oi.Path);

        if (oi.Path.IsEmpty())
            return E_INVALIDARG;

        if (oi.IsDir)
        {
            oi.Size = oi.PackSize = 0;
            oi.Crc = 0;
            oi.Offset = 0;
        }
        else if (newData)
        {
            CMyComPtr<ISequentialInStream> inStream;
            RINOK(cb->GetStream(i, &inStream));
            if (!inStream)
                return E_FAIL;

            std::vector<Byte> buf;
            const size_t kBuf = 1 << 16;
            Byte tmp[kBuf];
            for (;;)
            {
                UInt32 processed = 0;
                RINOK(inStream->Read(tmp, (UInt32)kBuf, &processed));
                if (processed == 0)
                    break;
                buf.insert(buf.end(), tmp, tmp + processed);
            }
            oi.Data.swap(buf);
            oi.Size = (UInt64)oi.Data.size();

            // CRC is calculated over UNpacked data (to match our extract verification)
            static bool crcInit = []() -> bool { CrcGenerateTable(); return true; }();
            (void)crcInit;
            oi.Crc = oi.Data.empty() ? 0 : CrcCalc(oi.Data.data(), oi.Data.size());

            bool compressIt = false;
            switch (_compressMode)
            {
                case ECompressMode::Always: compressIt = true; break;
                case ECompressMode::Never:  compressIt = false; break;
                case ECompressMode::Auto:   compressIt = _autoCompressDefault; break;
            }

            if (compressIt && !oi.Data.empty())
            {
                static bool lzoInit = false;
                if (!lzoInit)
                {
                    if (lzo_init() != LZO_E_OK)
                        return E_FAIL;
                    lzoInit = true;
                }

                // worst-case output buffer as per minilzo docs
                std::vector<Byte> comp(oi.Data.size() + oi.Data.size() / 16 + 64 + 3);
                lzo_uint outLen = 0;
                std::vector<Byte> wrk(LZO1X_1_MEM_COMPRESS);
                const int rc = lzo1x_1_compress(
                    oi.Data.data(), (lzo_uint)oi.Data.size(),
                    comp.data(), &outLen,
                    wrk.data());

                if (rc == LZO_E_OK && outLen > 0 && outLen < oi.Data.size())
                {
                    comp.resize(outLen);
                    oi.PackSize = outLen;
                    oi.Data.swap(comp);
                }
                else
                {
                    oi.PackSize = (UInt64)oi.Data.size(); // store
                }
            }
            else
            {
                oi.PackSize = (UInt64)oi.Data.size(); // store
            }
        }
        else if (src)
        {
            oi.Size = src->Size;
            oi.PackSize = src->PackSize;
            oi.Crc = src->Crc;
            oi.FromOld = true;
            oi.SrcIndex = indexInArc;
            oi.SrcOffset = src->Offset;
            oi.SrcPackSize = src->PackSize;
        }
        else
        {
            return E_INVALIDARG;
        }

        if (oi.Size > 0xFFFFFFFFu || oi.PackSize > 0xFFFFFFFFu)
            return E_FAIL;

        if (!oi.IsDir)
        {
            dataTotal += oi.PackSize;
            if (dataTotal > 0xFFFFFFFFull)
                return E_FAIL;
        }

        outItems.Add(oi);
    }

    // Optional deduplicate by path (keep last occurrence)
    if (_dedupPaths)
    {
        std::unordered_map<std::wstring, unsigned> lastByPath;
        CObjectVector<OutItem> deduped;
        for (unsigned i = 0; i < outItems.Size(); i++)
        {
            // normalize to match header: lowercase ASCII and backslashes
            UString norm = outItems[i].Path;
            norm.MakeLower_Ascii();
            norm.Replace(L'/', L'\\');
            const std::wstring key(norm);
            auto it = lastByPath.find(key);
            if (it == lastByPath.end())
            {
                lastByPath.emplace(key, (unsigned)deduped.Size());
                deduped.Add(outItems[i]);
            }
            else
            {
                deduped[(int)it->second] = outItems[i];
            }
        }
        outItems = deduped;
    }

    const UInt64 dataChunkStart = 8;
    UInt64 cursor = dataChunkStart;
    for (unsigned i = 0; i < outItems.Size(); i++)
    {
        OutItem &oi = outItems[i];
        if (oi.IsDir || oi.PackSize == 0)
        {
            oi.Offset = 0;
            continue;
        }
        oi.Offset = cursor;
        cursor += oi.PackSize;
        if (oi.Offset > 0xFFFFFFFFu || cursor > 0x100000000ull)
            return E_FAIL;
    }
    const UInt32 dataChunkSize = (UInt32)(cursor - dataChunkStart);

    std::vector<Byte> hdrPlain;
    hdrPlain.reserve(outItems.Size() * 32);

    for (unsigned i = 0; i < outItems.Size(); i++)
    {
        const OutItem &oi = outItems[i];
        UString pathU = oi.Path;
        pathU.MakeLower_Ascii();
        pathU.Replace(L'/', L'\\');
        AString pathA = UnicodeStringToMultiByte(pathU, CP_ACP);
        const UInt32 nameLen = (UInt32)pathA.Len();
        if (nameLen > 0xFFFF - 16)
            return E_FAIL;

        WriteLE16(hdrPlain, (UInt16)(nameLen + 16));
        WriteLE32(hdrPlain, (UInt32)oi.Size);
        WriteLE32(hdrPlain, (UInt32)oi.PackSize);
        WriteLE32(hdrPlain, oi.Crc);
        hdrPlain.insert(hdrPlain.end(), (const Byte*)pathA.Ptr(), (const Byte*)pathA.Ptr() + nameLen);
        WriteLE32(hdrPlain, (UInt32)oi.Offset);
    }

    uint8_t *hdrCompPtr = nullptr;
    size_t hdrCompSize = 0;
    xray_re::xr_lzhuf::compress(hdrCompPtr, hdrCompSize,
        hdrPlain.empty() ? nullptr : reinterpret_cast<const uint8_t*>(hdrPlain.data()),
        hdrPlain.size());
    if (!hdrCompPtr || hdrCompSize == 0)
        return E_FAIL;
    std::vector<Byte> hdrComp(hdrCompPtr, hdrCompPtr + hdrCompSize);
    free(hdrCompPtr);

    // ensure total archive size fits 32-bit offsets (include header chunk header, and optional userdata)
    UInt64 totalSize = dataChunkStart + dataChunkSize + 8ULL + hdrComp.size();
    if (_hasUserData && !_userData.empty())
        totalSize += 8ULL + _userData.size();
    if (totalSize > 0xFFFFFFFFull)
        return E_FAIL;

    const UInt32 dataChunkId = kDbChunkData;
    RINOK(WriteAll(outStream, &dataChunkId, sizeof(UInt32)));
    RINOK(WriteAll(outStream, &dataChunkSize, sizeof(UInt32)));

    for (unsigned i = 0; i < outItems.Size(); i++)
    {
        const OutItem &oi = outItems[i];
        if (oi.IsDir || oi.PackSize == 0)
            continue;

        if (oi.FromOld)
        {
            if (!_stream)
                return E_FAIL;
            RINOK(CopyFromInStream(_stream, oi.SrcOffset, oi.SrcPackSize, outStream));
        }
        else
        {
            if (!oi.Data.empty())
                RINOK(WriteAll(outStream, oi.Data.data(), oi.Data.size()));
        }
    }

    const UInt32 hdrChunkId = kDbChunkHeader | kChunkCompressedFlag;
    const UInt32 hdrSize32 = (UInt32)hdrComp.size();
    RINOK(WriteAll(outStream, &hdrChunkId, sizeof(UInt32)));
    RINOK(WriteAll(outStream, &hdrSize32, sizeof(UInt32)));
    if (!hdrComp.empty())
        RINOK(WriteAll(outStream, hdrComp.data(), hdrComp.size()));

    // append userdata chunk if present
    if (_hasUserData && !_userData.empty())
    {
        const UInt32 userChunkId = kDbChunkUserData;
        const UInt32 userSize32 = (UInt32)_userData.size();
        RINOK(WriteAll(outStream, &userChunkId, sizeof(UInt32)));
        RINOK(WriteAll(outStream, &userSize32, sizeof(UInt32)));
        RINOK(WriteAll(outStream, _userData.data(), _userData.size()));
    }

    _items.Clear();
    for (unsigned i = 0; i < outItems.Size(); i++)
    {
        const OutItem &oi = outItems[i];
        CItem it;
        it.IsDir = oi.IsDir;
        it.Path = oi.Path;
        it.Offset = oi.Offset;
        it.Size = oi.Size;
        it.PackSize = oi.PackSize;
        it.Crc = oi.Crc;
        _items.Add(it);
    }
    _phySize = dataChunkStart + dataChunkSize + 8 + hdrComp.size();
    if (_hasUserData && !_userData.empty())
        _phySize += 8 + _userData.size();

    return cb->SetOperationResult(NArchive::NUpdate::NOperationResult::kOK);
    COM_TRY_END
}

Z7_COM7F_IMF(CHandler::GetFileTimeType(UInt32 *type))
{
    *type = NFileTimeType::kNotDefined;
    return S_OK;
}

} // NXdb
} // NArchive
