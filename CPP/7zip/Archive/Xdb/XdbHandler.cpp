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
#include "xr_scrambler.h"

extern "C" {
#include "minilzo.h"
}

#include <algorithm>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <cstring>
#include <stdexcept>

// Minimal hook so xr_lzhuf links; throw on fatal so we can catch.
namespace xray_re {
void die(const char* message, const char* file, unsigned line) {
    throw std::runtime_error(std::string("xr_lzhuf die: ") + file + ":" + std::to_string(line) + ": " + message);
}
} // namespace xray_re

namespace NArchive {
namespace NXdb {

using DBVersion = CHandler::DBVersion;

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
    kpidUserDataText = kpidUserDefined,
    kpidDbFormat = kpidUserDefined + 1
};

static const CStatProp kArcProps[] =
{
    { "Userdata", kpidUserDataText, VT_BSTR },
    { "DB-Format", kpidDbFormat, VT_BSTR }
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

static void NormalizePath(UString &path)
{
    path.MakeLower_Ascii();
    path.Replace(L'/', L'\\');
    TrimTrailingSlashes(path);
}

static void AddItemDedup(CObjectVector<CItem> &items, const CItem &item, bool dedup,
                         std::unordered_map<std::wstring, unsigned> &lastByPath)
{
    if (!dedup)
    {
        items.Add(item);
        return;
    }

    UString norm = item.Path;
    norm.MakeLower_Ascii();
    norm.Replace(L'/', L'\\');
    const std::wstring key(norm);
    auto it = lastByPath.find(key);
    if (it == lastByPath.end())
    {
        lastByPath.emplace(key, (unsigned)items.Size());
        items.Add(item);
    }
    else
    {
        items[(int)it->second] = item;
    }
}

// LZHUF helper with hard limit and exception safety
static bool DecompressLzhuf(const std::vector<Byte> &in, std::vector<Byte> &out, UInt64 maxOut)
{
    size_t outSize = 0;
    uint8_t *outPtr = nullptr;
    try
    {
        xray_re::xr_lzhuf::decompress(outPtr, outSize,
            reinterpret_cast<const uint8_t*>(in.data()), in.size());
    }
    catch (...)
    {
        if (outPtr)
            free(outPtr);
        return false;
    }

    if (!outPtr || outSize == 0 || outSize > maxOut)
    {
        if (outPtr)
            free(outPtr);
        return false;
    }

    out.assign(outPtr, outPtr + outSize);
    free(outPtr);
    return true;
}

static bool ParseV2947(const Byte *start, const Byte *end, bool dedup, CObjectVector<CItem> &items)
{
    std::unordered_map<std::wstring, unsigned> lastByPath;
    const Byte *p = start;
    while (p < end)
    {
        if ((size_t)(end - p) < 2 + 4 + 4 + 4 + 4)
            return false;

        UInt16 nameField = ReadLE16(p); p += 2;
        if (nameField < 16)
            return false;

        UInt32 nameLen = (UInt32)nameField - 16;
        UInt32 sizeReal = ReadLE32(p); p += 4;
        UInt32 sizeComp = ReadLE32(p); p += 4;
        UInt32 crc = ReadLE32(p); p += 4;

        if ((size_t)(end - p) < nameLen + 4)
            return false;

        std::string nameA((const char*)p, (size_t)nameLen);
        p += nameLen;

        UInt32 offset = ReadLE32(p); p += 4;

        std::replace(nameA.begin(), nameA.end(), '\\', '/');

        CItem it;
        it.IsDir = (offset == 0);
        it.Path = MultiByteToUnicodeString(nameA.c_str(), CP_ACP);
        TrimTrailingSlashes(it.Path);
        if (it.Path.IsEmpty())
            continue;

        it.Offset = offset;
        it.Size = sizeReal;
        it.PackSize = sizeComp;
        it.Crc = crc;
        it.UseLzhuf = false;

        AddItemDedup(items, it, dedup, lastByPath);
    }
    return !items.IsEmpty();
}

static bool ParseV2945(const Byte *start, const Byte *end, bool dedup, CObjectVector<CItem> &items)
{
    std::unordered_map<std::wstring, unsigned> lastByPath;
    const Byte *p = start;
    while (p < end)
    {
        const char *nz = (const char*)memchr(p, 0, end - p);
        if (!nz)
            return false;

        size_t nameLen = (size_t)(nz - (const char*)p);
        std::string nameA((const char*)p, nameLen);
        p = (const Byte*)(nz + 1);

        if ((size_t)(end - p) < 4 + 4 + 4 + 4)
            return false;

        UInt32 crc = ReadLE32(p); p += 4;
        UInt32 offset = ReadLE32(p); p += 4;
        UInt32 sizeReal = ReadLE32(p); p += 4;
        UInt32 sizeComp = ReadLE32(p); p += 4;

        std::replace(nameA.begin(), nameA.end(), '\\', '/');

        CItem it;
        it.IsDir = (offset == 0);
        it.Path = MultiByteToUnicodeString(nameA.c_str(), CP_ACP);
        TrimTrailingSlashes(it.Path);
        if (it.Path.IsEmpty())
            continue;

        it.Offset = offset;
        it.Size = sizeReal;
        it.PackSize = sizeComp;
        it.Crc = crc;
        it.UseLzhuf = false;

        AddItemDedup(items, it, dedup, lastByPath);
    }
    return !items.IsEmpty();
}

static bool ParseV2215(const Byte *start, const Byte *end, bool dedup, CObjectVector<CItem> &items)
{
    std::unordered_map<std::wstring, unsigned> lastByPath;
    const Byte *p = start;
    while (p < end)
    {
        const char *nz = (const char*)memchr(p, 0, end - p);
        if (!nz)
            return false;

        size_t nameLen = (size_t)(nz - (const char*)p);
        std::string nameA((const char*)p, nameLen);
        p = (const Byte*)(nz + 1);

        if ((size_t)(end - p) < 4 + 4 + 4)
            return false;

        UInt32 offset = ReadLE32(p); p += 4;
        UInt32 sizeReal = ReadLE32(p); p += 4;
        UInt32 sizeComp = ReadLE32(p); p += 4;

        std::replace(nameA.begin(), nameA.end(), '\\', '/');

        CItem it;
        it.IsDir = (offset == 0);
        it.Path = MultiByteToUnicodeString(nameA.c_str(), CP_ACP);
        TrimTrailingSlashes(it.Path);
        if (it.Path.IsEmpty())
            continue;

        it.Offset = offset;
        it.Size = sizeReal;
        it.PackSize = sizeComp;
        it.Crc = 0;
        it.UseLzhuf = false;

        AddItemDedup(items, it, dedup, lastByPath);
    }
    return !items.IsEmpty();
}

static bool ParseV11XX(const Byte *start, const Byte *end, bool dedup, CObjectVector<CItem> &items)
{
    std::unordered_map<std::wstring, unsigned> lastByPath;
    const Byte *p = start;
    while (p < end)
    {
        const char *nz = (const char*)memchr(p, 0, end - p);
        if (!nz)
            return false;

        size_t nameLen = (size_t)(nz - (const char*)p);
        std::string nameA((const char*)p, nameLen);
        p = (const Byte*)(nz + 1);

        if ((size_t)(end - p) < 4 + 4 + 4)
            return false;

        UInt32 uncompressedFlag = ReadLE32(p); p += 4;
        UInt32 offset = ReadLE32(p); p += 4;
        UInt32 sizeField = ReadLE32(p); p += 4;

        std::replace(nameA.begin(), nameA.end(), '\\', '/');

        CItem it;
        it.IsDir = (offset == 0);
        it.Path = MultiByteToUnicodeString(nameA.c_str(), CP_ACP);
        TrimTrailingSlashes(it.Path);
        if (it.Path.IsEmpty())
            continue;

        it.Offset = offset;
        it.PackSize = sizeField;
        it.Size = (uncompressedFlag != 0) ? sizeField : 0; // real size unknown when compressed
        it.Crc = 0;
        it.UseLzhuf = (uncompressedFlag == 0);

        AddItemDedup(items, it, dedup, lastByPath);
    }
    return !items.IsEmpty();
}

static bool TryParseHeader(const std::vector<Byte> &payload, bool compressed,
                           DBVersion candidate, UInt64 headerMax, bool dedup,
                           CObjectVector<CItem> &outItems, DBVersion &detected)
{
    std::vector<Byte> plain;
    if (compressed)
    {
        if (payload.size() < 4)
            return false;

        const UInt32 expected = ReadLE32(payload.data());
        if (expected > headerMax)
            return false;

        if (expected == 0)
        {
            // Empty headers are valid for empty archives.
            plain.clear();
        }
        else
        {
            if (!DecompressLzhuf(payload, plain, headerMax))
                return false;
        }
    }
    else
    {
        plain = payload;
    }

    const Byte *start = plain.data();
    const Byte *end = start + plain.size();

    CObjectVector<CItem> parsed;

    if (ParseV2947(start, end, dedup, parsed))
    {
        outItems = parsed;
        detected = candidate;
        return true;
    }

    parsed.Clear();
    if (ParseV2945(start, end, dedup, parsed))
    {
        outItems = parsed;
        detected = DBVersion::V2945;
        return true;
    }

    parsed.Clear();
    if (ParseV2215(start, end, dedup, parsed))
    {
        outItems = parsed;
        detected = DBVersion::V2215;
        return true;
    }

    parsed.Clear();
    if (ParseV11XX(start, end, dedup, parsed))
    {
        outItems = parsed;
        detected = DBVersion::V11XX;
        return true;
    }

    if (plain.empty())
    {
        outItems.Clear();
        detected = candidate;
        return true;
    }

    return false;
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

Z7_COM7F_IMF(CHandler::Open(IInStream* stream, const UInt64*, IArchiveOpenCallback* callback))
{
    COM_TRY_BEGIN
    CMyComPtr<IArchiveOpenVolumeCallback> volumeCallback;
    callback->QueryInterface(IID_IArchiveOpenVolumeCallback, (void**)&volumeCallback);

    if (volumeCallback)
    {
        PROPVARIANT prop;
        PropVariantInit(&prop);
        if (volumeCallback->GetProperty(kpidExtension, &prop) == S_OK && prop.vt == VT_BSTR)
        {
            UString ext = prop.bstrVal;
            if (ext.Find(L"db") != 0 && ext.Find(L"xdb") != 0) 
                return S_FALSE;
        }
        PropVariantClear(&prop);
    }

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

    _items.Clear();
    _userData.clear();
    _hasUserData = false;
    _dbVersion = DBVersion::UNKNOWN;

    UInt64 dataPos = 0, dataSize = 0, hdrPos = 0, hdrSize = 0;
    UInt64 userPos = 0, userSize = 0;
    UInt32 hdrIdRaw = 0;

    const HRESULT fr = FindChunks(_stream, _phySize,
                                  dataPos, dataSize,
                                  hdrPos, hdrSize, hdrIdRaw,
                                  userPos, userSize);
    if (fr != S_OK)
        return S_FALSE;

    // Read header chunk payload (compressed or not)
    std::vector<Byte> hdrComp((size_t)hdrSize);
    RINOK(SeekAbs(_stream, hdrPos));
    RINOK(ReadExact(_stream, hdrComp.data(), hdrComp.size()));

    const bool hdrCompressed = (hdrIdRaw & kChunkCompressedFlag) != 0;
    // Allow small archives whose unpacked header can be larger than total archive size,
    // but still keep a hard safety cap for malformed inputs.
    UInt64 kHeaderMax = _phySize;
    if (kHeaderMax < (1ull << 26)) // 64 MiB minimum cap
        kHeaderMax = (1ull << 26);
    if (kHeaderMax > (1ull << 28)) // 256 MiB hard ceiling
        kHeaderMax = (1ull << 28);

    auto attempt_order = [&](const std::vector<DBVersion> &order) -> bool
    {
        for (DBVersion v : order)
        {
            std::vector<Byte> payload;
            bool compressed = hdrCompressed;

            if (v == DBVersion::V2947RU || v == DBVersion::V2947WW)
            {
                if (!hdrCompressed)
                    continue;

                payload.resize(hdrComp.size());
                xray_re::xr_scrambler scr(v == DBVersion::V2947RU
                    ? xray_re::xr_scrambler::CC_RU
                    : xray_re::xr_scrambler::CC_WW);
                scr.decrypt(payload.data(), hdrComp.data(), hdrComp.size());

                if (payload.size() < 4)
                    continue;

                const UInt32 expected = ReadLE32(payload.data());
                if (expected > kHeaderMax)
                    continue;

                compressed = true;
            }
            else
            {
                payload = hdrComp;
                compressed = hdrCompressed;
            }

            CObjectVector<CItem> parsed;
            DBVersion detected = DBVersion::UNKNOWN;
            if (TryParseHeader(payload, compressed, v, kHeaderMax, _dedupPaths, parsed, detected))
            {
                _items = parsed;
                _dbVersion = detected;
                return true;
            }
        }
        return false;
    };

    bool parsedOk = false;

    if (_dbForce != DBVersion::UNKNOWN)
    {
        parsedOk = attempt_order({_dbForce});
    }
    else
    {
        if (hdrCompressed)
            parsedOk = attempt_order({DBVersion::V2947WW, DBVersion::V2947RU, DBVersion::XDB, DBVersion::V2945, DBVersion::V2215, DBVersion::V11XX});
        else
            parsedOk = attempt_order({DBVersion::XDB, DBVersion::V2945, DBVersion::V2215, DBVersion::V11XX});
    }

    if (!parsedOk)
        return S_FALSE;

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
    _autoCompressDefault = (compressed == 0 && stored == 0) || (compressed > stored);

    if (userPos && userSize)
    {
        _userData.resize((size_t)userSize);
        RINOK(SeekAbs(_stream, userPos));
        RINOK(ReadExact(_stream, _userData.data(), _userData.size()));
        _hasUserData = true;
    }

    return S_OK;
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
        case kpidPath:
        {
            UString path = it.Path;
            // 7-Zip agent rename logic compares OS-style paths.
            // Returning native separators avoids false "Parameter is incorrect" errors.
            path.Replace(L'/', WCHAR_PATH_SEPARATOR);
            prop = path;
            break;
        }
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

    const bool needDecompress = (it.Size != it.PackSize) || it.UseLzhuf;

    if (needDecompress)
    {
        if (_dbVersion == DBVersion::V11XX && it.UseLzhuf)
        {
                uint8_t *outPtr = nullptr;
                size_t outSize = 0;
                xray_re::xr_lzhuf::decompress(outPtr, outSize,
                    reinterpret_cast<const uint8_t*>(packed.data()), packed.size());
                if (!outPtr || outSize == 0)
                {
                    if (outPtr) free(outPtr);
                    res = NArchive::NExtract::NOperationResult::kDataError;
                }
                else
                {
                    unpacked.assign(outPtr, outPtr + outSize);
                    free(outPtr);
                    dataPtr = unpacked.data();
                    dataSize = unpacked.size();
                }
            }
            else
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
        case kpidDbFormat:
        {
            const wchar_t *fmt = L"unknown";
            switch (_dbVersion)
            {
                case DBVersion::XDB:      fmt = L"xdb"; break;
                case DBVersion::V2947RU:  fmt = L"2947ru"; break;
                case DBVersion::V2947WW:  fmt = L"2947ww"; break;
                case DBVersion::V2945:    fmt = L"2945"; break;
                case DBVersion::V2215:    fmt = L"2215"; break;
                case DBVersion::V11XX:    fmt = L"11xx"; break;
                default: break;
            }
            prop = fmt;
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

static bool PropToUString(const PROPVARIANT &v, UString &res)
{
    res.Empty();
    switch (v.vt)
    {
        case VT_EMPTY:
            return true;
        case VT_BSTR:
            if (v.bstrVal)
                res = v.bstrVal;
            return true;
        case VT_BOOL:
            res = (v.boolVal != VARIANT_FALSE) ? L"true" : L"false";
            return true;
        case VT_UI1:
        {
            char tmp[16];
            std::snprintf(tmp, sizeof(tmp), "%u", (unsigned)v.bVal);
            res = MultiByteToUnicodeString(tmp, CP_ACP);
            return true;
        }
        case VT_UI2:
        {
            char tmp[16];
            std::snprintf(tmp, sizeof(tmp), "%u", (unsigned)v.uiVal);
            res = MultiByteToUnicodeString(tmp, CP_ACP);
            return true;
        }
        case VT_UI4:
        {
            char tmp[16];
            std::snprintf(tmp, sizeof(tmp), "%u", (unsigned)v.ulVal);
            res = MultiByteToUnicodeString(tmp, CP_ACP);
            return true;
        }
        case VT_I4:
        {
            char tmp[16];
            std::snprintf(tmp, sizeof(tmp), "%d", (int)v.lVal);
            res = MultiByteToUnicodeString(tmp, CP_ACP);
            return true;
        }
        default:
            return false;
    }
}

static void TrimWs(UString &s)
{
    while (s.Len() != 0 && (s[0] == L' ' || s[0] == L'\t'))
        s.Delete(0, 1);
    while (s.Len() != 0 && (s.Back() == L' ' || s.Back() == L'\t'))
        s.DeleteBack();
}

static void StripOuterQuotes(UString &s)
{
    for (;;)
    {
        if (s.Len() < 2)
            return;
        const wchar_t front = s[0];
        const wchar_t back = s.Back();
        if ((front == L'"' && back == L'"') || (front == L'\'' && back == L'\''))
        {
            s.Delete(0, 1);
            s.DeleteBack();
            continue;
        }
        return;
    }
}

static bool ReadFileToBytes_Wide(const UString &path, std::vector<Byte> &data)
{
    data.clear();

#if defined(_WIN32)
    FILE *f = nullptr;
#if defined(_MSC_VER)
    if (_wfopen_s(&f, path.Ptr(), L"rb") != 0 || f == nullptr)
        return false;
#else
    f = _wfopen(path.Ptr(), L"rb");
    if (f == nullptr)
        return false;
#endif

    if (_fseeki64(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return false;
    }

    const __int64 len64 = _ftelli64(f);
    if (len64 < 0)
    {
        fclose(f);
        return false;
    }

    if (_fseeki64(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        return false;
    }

    data.resize((size_t)len64);
    if (len64 > 0)
    {
        const size_t need = (size_t)len64;
        const size_t got = fread(data.data(), 1, need, f);
        if (got != need)
        {
            fclose(f);
            data.clear();
            return false;
        }
    }

    fclose(f);
    return true;
#else
    AString pathA = UnicodeStringToMultiByte(path, CP_UTF8);
    std::ifstream f(pathA.Ptr(), std::ios::binary);
    if (!f)
        return false;
    f.seekg(0, std::ios::end);
    std::streamoff len = f.tellg();
    if (len < 0)
        return false;
    data.resize((size_t)len);
    f.seekg(0, std::ios::beg);
    if (len > 0)
        f.read((char*)data.data(), len);
    return true;
#endif
}

static bool LoadUserDataFile(const UString &rawPath, std::vector<Byte> &dataOut)
{
    UString path = rawPath;
    TrimWs(path);
    StripOuterQuotes(path);
    TrimWs(path);
    if (path.IsEmpty())
        return false;
    return ReadFileToBytes_Wide(path, dataOut);
}

static bool BuildRawPropertyToken(const wchar_t *name, const PROPVARIANT &val, UString &token)
{
    token = name;
    if (val.vt == VT_EMPTY)
        return true;

    UString value;
    if (!PropToUString(val, value))
        return false;
    token += L"=";
    token += value;
    return true;
}

static std::vector<Byte> MakeGeneratedXdbUserData()
{
    static const char kDefaultUserData[] =
        "[header]\n"
        "auto_load = true\n"
        "level_name = stalker\n"
        "level_ver = 1.0\n"
        "entry_point = $fs_root$\\gamedata\\\n"
        "creator = \"Modder\"\n"
        "link = \"https://github.com/Tosox/xray-db-7z-plugin\"\n";

    const Byte *begin = reinterpret_cast<const Byte*>(kDefaultUserData);
    return std::vector<Byte>(begin, begin + sizeof(kDefaultUserData) - 1);
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
        if (name.IsEqualTo_Ascii_NoCase("dbver"))
        {
            if (val.vt != VT_BSTR || val.bstrVal == nullptr)
                return E_INVALIDARG;
            UString v(val.bstrVal);
            v.MakeLower_Ascii();
            if (v.IsEqualTo_Ascii_NoCase("auto"))
                _dbForce = DBVersion::UNKNOWN;
            else if (v.IsEqualTo_Ascii_NoCase("xdb"))
                _dbForce = DBVersion::XDB;
            else if (v.IsEqualTo_Ascii_NoCase("2947ru"))
                _dbForce = DBVersion::V2947RU;
            else if (v.IsEqualTo_Ascii_NoCase("2947ww"))
                _dbForce = DBVersion::V2947WW;
            else if (v.IsEqualTo_Ascii_NoCase("2945"))
                _dbForce = DBVersion::V2945;
            else if (v.IsEqualTo_Ascii_NoCase("2215"))
                _dbForce = DBVersion::V2215;
            else if (v.IsEqualTo_Ascii_NoCase("11xx"))
                _dbForce = DBVersion::V11XX; // covers 1114/1154-style
            else
                return E_INVALIDARG;
            continue;
        }
        if (name.IsEqualTo_Ascii_NoCase("xdb_ud"))
        {
            _userData.clear();
            _hasUserData = false;

            UString pathValue;
            if (!PropToUString(val, pathValue))
                return E_INVALIDARG;

            UString mergedPath = pathValue;
            UInt32 consumedTo = i;
            bool loaded = LoadUserDataFile(mergedPath, _userData);

            // GUI parameter parsing splits by spaces without quote awareness.
            // Re-join following tokens only for quoted xdb_ud values.
            if (!loaded)
            {
                UString trimmed = mergedPath;
                TrimWs(trimmed);

                wchar_t quote = 0;
                if (!trimmed.IsEmpty() && (trimmed[0] == L'"' || trimmed[0] == L'\''))
                    quote = trimmed[0];

                bool quoteClosed = false;
                if (quote != 0)
                {
                    if (trimmed.Len() >= 2 && trimmed.Back() == quote)
                        quoteClosed = true;

                    for (UInt32 j = i + 1; j < numProps && !quoteClosed; j++)
                    {
                        UString token;
                        if (!BuildRawPropertyToken(names[j], values[j], token))
                            return E_INVALIDARG;
                        mergedPath += L" ";
                        mergedPath += token;
                        consumedTo = j;
                        if (!token.IsEmpty() && token.Back() == quote)
                            quoteClosed = true;
                    }

                    loaded = LoadUserDataFile(mergedPath, _userData);
                }
            }

            if (!loaded)
                return E_INVALIDARG;

            _hasUserData = true;
            i = consumedTo;
            continue;
        }
        if (name.IsEqualTo_Ascii_NoCase("x"))
        {
            // Ignore value, use "compress" instead
            continue;
        }
        if (name.IsEqualTo_Ascii_NoCase("gen_xdb_ud"))
        {
            bool b = false;
            if (!PropToBool(val, b))
                return E_INVALIDARG;
            _genXdbUserData = b;
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

    DBVersion effVer = (_dbForce != DBVersion::UNKNOWN) ? _dbForce
                        : (_dbVersion != DBVersion::UNKNOWN ? _dbVersion : DBVersion::XDB);
    if (effVer == DBVersion::UNKNOWN)
        effVer = DBVersion::XDB;
    _dbVersion = effVer;

    struct OutItem
    {
        bool IsDir = false;
        bool UseLzhuf = false;
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

            // Some 7-Zip update paths expose renamed entries via kpidName.
            HRESULT hPath = cb->GetProperty(i, kpidPath, &prop);
            if (hPath == S_OK && prop.vt == VT_BSTR && prop.bstrVal != nullptr)
            {
                oi.Path = prop.bstrVal;
            }
            else if (hPath == S_OK && prop.vt == VT_EMPTY && src)
            {
                oi.Path = src->Path;
            }
            else
            {
                prop.Clear();
                const HRESULT hName = cb->GetProperty(i, kpidName, &prop);
                if (hName == S_OK && prop.vt == VT_BSTR && prop.bstrVal != nullptr)
                    oi.Path = prop.bstrVal;
                else if (src)
                    oi.Path = src->Path;
                else
                    return E_INVALIDARG;
            }
            prop.Clear();

            const HRESULT hIsDir = cb->GetProperty(i, kpidIsDir, &prop);
            if (hIsDir == S_OK && prop.vt == VT_BOOL)
                oi.IsDir = (prop.boolVal != VARIANT_FALSE);
            else if (src)
                oi.IsDir = src->IsDir;
            else if (hIsDir == S_OK && prop.vt == VT_EMPTY)
                oi.IsDir = false;
            else if (hIsDir != S_OK)
                oi.IsDir = false;
            else
                return E_INVALIDARG;

            prop.Clear();

            // Check anti/delete flag if provided
            const HRESULT hAnti = cb->GetProperty(i, kpidIsAnti, &prop);
            if (hAnti == S_OK && prop.vt == VT_BOOL && prop.boolVal != VARIANT_FALSE)
                continue; // delete: skip adding to outItems
            if (hAnti == S_OK && prop.vt != VT_EMPTY && prop.vt != VT_BOOL)
                return E_INVALIDARG;
        }
        else
        {
            oi.Path = src->Path;
            oi.IsDir = src->IsDir;
            oi.UseLzhuf = src->UseLzhuf;
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
                if (effVer == DBVersion::V11XX)
                {
                    uint8_t *compPtr = nullptr;
                    size_t compSize = 0;
                    xray_re::xr_lzhuf::compress(compPtr, compSize,
                        reinterpret_cast<const uint8_t*>(oi.Data.data()),
                        oi.Data.size());
                    if (!compPtr || compSize == 0 || compSize >= oi.Data.size())
                    {
                        if (compPtr) free(compPtr);
                        oi.PackSize = (UInt64)oi.Data.size(); // store
                        oi.UseLzhuf = false;
                    }
                    else
                    {
                        oi.PackSize = (UInt64)compSize;
                        oi.Data.assign(compPtr, compPtr + compSize);
                        oi.UseLzhuf = true;
                        free(compPtr);
                    }
                }
                else
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
                        oi.UseLzhuf = false;
                    }
                    else
                    {
                        oi.PackSize = (UInt64)oi.Data.size(); // store
                        oi.UseLzhuf = false;
                    }
                }
            }
            else
            {
                oi.PackSize = (UInt64)oi.Data.size(); // store
                oi.UseLzhuf = false;
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
            oi.UseLzhuf = src->UseLzhuf;
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
        if (oi.IsDir)
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

    if (effVer == DBVersion::V2945)
    {
        for (unsigned i = 0; i < outItems.Size(); i++)
        {
            const OutItem &oi = outItems[i];
            UString pathU = oi.Path;
            pathU.MakeLower_Ascii();
            pathU.Replace(L'/', L'\\');
            AString pathA = UnicodeStringToMultiByte(pathU, CP_ACP);
            hdrPlain.insert(hdrPlain.end(), (const Byte*)pathA.Ptr(), (const Byte*)pathA.Ptr() + pathA.Len());
            hdrPlain.push_back(0); // null terminator
            WriteLE32(hdrPlain, oi.Crc);
            WriteLE32(hdrPlain, (UInt32)oi.Offset);
            WriteLE32(hdrPlain, (UInt32)oi.Size);
            WriteLE32(hdrPlain, (UInt32)oi.PackSize);
        }
    }
    else if (effVer == DBVersion::V2215)
    {
        for (unsigned i = 0; i < outItems.Size(); i++)
        {
            const OutItem &oi = outItems[i];
            UString pathU = oi.Path;
            pathU.MakeLower_Ascii();
            pathU.Replace(L'/', L'\\');
            AString pathA = UnicodeStringToMultiByte(pathU, CP_ACP);
            hdrPlain.insert(hdrPlain.end(), (const Byte*)pathA.Ptr(), (const Byte*)pathA.Ptr() + pathA.Len());
            hdrPlain.push_back(0); // null terminator
            WriteLE32(hdrPlain, (UInt32)oi.Offset);
            WriteLE32(hdrPlain, (UInt32)oi.Size);
            WriteLE32(hdrPlain, (UInt32)oi.PackSize);
        }
    }
    else if (effVer == DBVersion::V11XX)
    {
        for (unsigned i = 0; i < outItems.Size(); i++)
        {
            const OutItem &oi = outItems[i];
            UString pathU = oi.Path;
            pathU.MakeLower_Ascii();
            pathU.Replace(L'/', L'\\');
            AString pathA = UnicodeStringToMultiByte(pathU, CP_ACP);
            hdrPlain.insert(hdrPlain.end(), (const Byte*)pathA.Ptr(), (const Byte*)pathA.Ptr() + pathA.Len());
            hdrPlain.push_back(0); // null terminator
            UInt32 uncompressedFlag = (oi.PackSize == oi.Size) ? 1u : 0u;
            WriteLE32(hdrPlain, uncompressedFlag);
            WriteLE32(hdrPlain, (UInt32)oi.Offset);
            WriteLE32(hdrPlain, (UInt32)oi.PackSize);
        }
    }
    else // Xdb / 2947
    {
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

    // Optional scramble for 2947 RU/WW
    if (effVer == DBVersion::V2947RU || effVer == DBVersion::V2947WW)
    {
        xray_re::xr_scrambler scr(effVer == DBVersion::V2947RU
            ? xray_re::xr_scrambler::CC_RU
            : xray_re::xr_scrambler::CC_WW);
        std::vector<Byte> tmp(hdrComp.size());
        scr.encrypt(tmp.data(), hdrComp.data(), hdrComp.size());
        hdrComp.swap(tmp);
    }

    std::vector<Byte> generatedUserData;
    const std::vector<Byte> *userDataToWrite = nullptr;
    if (effVer == DBVersion::XDB)
    {
        if (_hasUserData && !_userData.empty())
            userDataToWrite = &_userData;
        else if (_genXdbUserData)
        {
            generatedUserData = MakeGeneratedXdbUserData();
            if (!generatedUserData.empty())
                userDataToWrite = &generatedUserData;
        }
    }

    // ensure total archive size fits 32-bit offsets (include header chunk header, and optional userdata)
    UInt64 totalSize = dataChunkStart + dataChunkSize + 8ULL + hdrComp.size();
    if (userDataToWrite)
        totalSize += 8ULL + userDataToWrite->size();
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

    // append userdata chunk if present and format supports it (XDB only)
    if (userDataToWrite)
    {
        const UInt32 userChunkId = kDbChunkUserData;
        const UInt32 userSize32 = (UInt32)userDataToWrite->size();
        RINOK(WriteAll(outStream, &userChunkId, sizeof(UInt32)));
        RINOK(WriteAll(outStream, &userSize32, sizeof(UInt32)));
        RINOK(WriteAll(outStream, userDataToWrite->data(), userDataToWrite->size()));
    }

    if (effVer == DBVersion::XDB)
    {
        if (userDataToWrite)
        {
            _userData = *userDataToWrite;
            _hasUserData = true;
        }
        else
        {
            _userData.clear();
            _hasUserData = false;
        }
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
    if (userDataToWrite)
        _phySize += 8 + userDataToWrite->size();

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
