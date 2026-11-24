// archive_dll.cpp — DLL-backed parsers for 7z and RAR.
//
// Native header-walking cannot reliably handle 7z (compressed/encrypted
// headers need a full LZMA decoder) or modern RAR5. ArchView solves this by
// delegating to the archiver's own DLLs; we replicate that proven approach:
//   * 7z  -> TC7Z64.DLL / TC7Z.DLL / 7z.dll  via the IInArchive COM interface
//   * RAR -> UNRAR64.DLL / UNRAR.DLL         via the unrar.dll C API
// If the DLL is missing we fall back to the native parsers (plain-header 7z,
// RAR4 listing) so the plugin still works standalone.
#include "archive.h"
#include "archive_internal.h"
#include "sevenzip_com.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace archdiview {

using namespace szdll;

namespace {

// ---- PROPVARIANT helpers ----------------------------------------------------
bool VarToUInt64(const PROPVARIANT& p, uint64_t* out) {
    switch (p.vt) {
    case VT_UI8:  *out = p.uhVal.QuadPart; return true;
    case VT_I8:   *out = (uint64_t)p.hVal.QuadPart; return true;
    case VT_UI4: case VT_UINT: *out = p.ulVal; return true;
    case VT_I4: case VT_INT:   *out = (uint64_t)p.lVal; return true;
    case VT_UI2: *out = p.uiVal; return true;
    case VT_I2:  *out = (uint64_t)p.iVal; return true;
    case VT_UI1: *out = p.bVal; return true;
    case VT_I1:  *out = (uint64_t)p.cVal; return true;
    case VT_BOOL: *out = (p.boolVal != VARIANT_FALSE) ? 1 : 0; return true;
    default: return false;
    }
}

bool VarToBool(const PROPVARIANT& p, bool* out) {
    if (p.vt == VT_BOOL) { *out = (p.boolVal != VARIANT_FALSE); return true; }
    if (p.vt == VT_EMPTY) { *out = false; return true; }
    return false;
}

// Wide-string property (VT_LPWSTR or VT_BSTR). Returns true if present.
bool VarToWide(const PROPVARIANT& p, std::wstring* out) {
    if (p.vt == VT_LPWSTR && p.pwszVal) { *out = p.pwszVal; return true; }
    if (p.vt == VT_BSTR && p.bstrVal) { *out = p.bstrVal; return true; }
    return false;
}

// Clear a PROPVARIANT safely (frees BSTR etc.).
void ClearVariant(PROPVARIANT& p) { VariantClear(reinterpret_cast<VARIANTARG*>(&p)); }

// ---- COM: input file stream (IInStream + IStreamGetSize) --------------------
class InFileStream : public IInStream7z, public IStreamGetSize7z {
public:
    InFileStream() : ref_(0), h_(INVALID_HANDLE_VALUE) {}
    virtual ~InFileStream() { if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }

    bool Open(const wchar_t* path) {
        h_ = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        return h_ != INVALID_HANDLE_VALUE;
    }

    HANDLE Handle() const { return h_; }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown) { *ppv = static_cast<IUnknown*>(static_cast<IInStream7z*>(this)); }
        else if (riid == kIID_ISequentialInStream) { *ppv = static_cast<ISequentialInStream7z*>(this); }
        else if (riid == kIID_IInStream) { *ppv = static_cast<IInStream7z*>(this); }
        else if (riid == kIID_IStreamGetSize) { *ppv = static_cast<IStreamGetSize7z*>(this); }
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }

    // ISequentialInStream
    HRESULT STDMETHODCALLTYPE Read(void* data, UInt32 size, UInt32* processed) override {
        if (processed) *processed = 0;
        if (h_ == INVALID_HANDLE_VALUE) return E_FAIL;
        DWORD got = 0;
        // Read in chunks to stay under DWORD limits for big reads.
        UInt32 total = 0;
        uint8_t* p = static_cast<uint8_t*>(data);
        while (total < size) {
            DWORD want = (DWORD)((size - total) > 0x40000000u ? 0x40000000u : (size - total));
            if (!ReadFile(h_, p + total, want, &got, nullptr)) {
                if (processed) *processed = total;
                return HRESULT_FROM_WIN32(GetLastError());
            }
            if (got == 0) break; // EOF
            total += got;
        }
        if (processed) *processed = total;
        return S_OK;
    }

    // IInStream
    HRESULT STDMETHODCALLTYPE Seek(Int64 offset, UInt32 origin, UInt64* newPos) override {
        if (h_ == INVALID_HANDLE_VALUE) return E_FAIL;
        LARGE_INTEGER li; li.QuadPart = offset;
        LARGE_INTEGER out{};
        if (!SetFilePointerEx(h_, li, &out, (DWORD)origin))
            return HRESULT_FROM_WIN32(GetLastError());
        if (newPos) *newPos = (uint64_t)out.QuadPart;
        return S_OK;
    }

    // IStreamGetSize
    HRESULT STDMETHODCALLTYPE GetSize(UInt64* size) override {
        if (h_ == INVALID_HANDLE_VALUE) return E_FAIL;
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(h_, &sz)) return HRESULT_FROM_WIN32(GetLastError());
        if (size) *size = (uint64_t)sz.QuadPart;
        return S_OK;
    }

private:
    LONG ref_;
    HANDLE h_;
};

// ---- COM: open callback (+ optional password prompt -> we abort) -----------
class OpenCallback : public IArchiveOpenCallback7z, public ICryptoGetTextPassword7z {
public:
    OpenCallback() : ref_(0) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown) { *ppv = static_cast<IUnknown*>(static_cast<IArchiveOpenCallback7z*>(this)); }
        else if (riid == kIID_IArchiveOpenCallback) { *ppv = static_cast<IArchiveOpenCallback7z*>(this); }
        else if (riid == kIID_ICryptoGetTextPassword) { *ppv = static_cast<ICryptoGetTextPassword7z*>(this); }
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE SetTotal(const UInt64*, const UInt64*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64*, const UInt64*) override { return S_OK; }
    // No password support: abort so encrypted archives fail cleanly.
    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR*) override { return E_ABORT; }

private:
    LONG ref_;
};

// ---- Locate + load the 7z DLL ----------------------------------------------
// A bare LoadLibraryW(L"TC7Z64.DLL") does NOT reliably find the DLL inside TC:
// the loader's default search covers the app dir / system dirs / PATH, but TC's
// own folder (where TC7Z64.DLL lives) is only searched if it happens to be on
// PATH. So we build explicit candidate paths and try each in turn.
HMODULE LoadArchiverDll(const wchar_t* dllName) {
    // 1. The directory of the module containing this code (ArchDiView.wlx64) —
    //    works even for portable TC / on-a-stick where %COMMANDER_PATH% is unset.
    HMODULE selfMod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCWSTR)&LoadArchiverDll, &selfMod) && selfMod) {
        wchar_t self[MAX_PATH] = {0};
        if (GetModuleFileNameW(selfMod, self, MAX_PATH)) {
            wchar_t* slash = wcsrchr(self, L'\\');
            if (slash) { *(slash + 1) = L'\0';
                std::wstring p = std::wstring(self) + dllName;
                HMODULE m = LoadLibraryW(p.c_str());
                if (m) return m;
            }
        }
    }
    // 2. %COMMANDER_PATH% (TC sets this to its install folder).
    wchar_t cmdr[MAX_PATH] = {0};
    if (GetEnvironmentVariableW(L"COMMANDER_PATH", cmdr, MAX_PATH) > 0) {
        std::wstring p = std::wstring(cmdr) + L"\\" + dllName;
        HMODULE m = LoadLibraryW(p.c_str());
        if (m) return m;
    }
    // 3. The directory of the host EXE (totalcmd64.exe / totalcmd.exe).
    wchar_t exe[MAX_PATH] = {0};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        wchar_t* slash = wcsrchr(exe, L'\\');
        if (slash) { *(slash + 1) = L'\0';
            std::wstring p = std::wstring(exe) + dllName;
            HMODULE m = LoadLibraryW(p.c_str());
            if (m) return m;
        }
    }
    // 4. Plain name (PATH / system dirs) as a last resort.
    return LoadLibraryW(dllName);
}

HMODULE LoadSevenZipDll() {
    // Prefer the architecture-appropriate TC DLL, then a standalone 7z.dll.
#ifdef _WIN64
    HMODULE m = LoadArchiverDll(L"TC7Z64.DLL");
    if (m) return m;
    return LoadArchiverDll(L"7z.dll");
#else
    HMODULE m = LoadArchiverDll(L"TC7Z.DLL");
    if (m) return m;
    return LoadArchiverDll(L"7z.dll");
#endif
}

// Resolve the CreateObject export and build an IInArchive for 7z.
typedef HRESULT(STDMETHODCALLTYPE* CreateObjectFn)(const GUID* clsid,
                                                    const GUID* iid, void** out);

bool Make7zArchive(IInArchive7z** out, HMODULE* dllOut) {
    *out = nullptr;
    HMODULE dll = LoadSevenZipDll();
    if (!dll) return false;
    CreateObjectFn create = (CreateObjectFn)GetProcAddress(dll, "CreateObject");
    if (!create) { FreeLibrary(dll); return false; }
    IInArchive7z* arc = nullptr;
    HRESULT hr = create(&kCLSID_CFormat7z, &kIID_IInArchive, (void**)&arc);
    if (FAILED(hr) || !arc) { FreeLibrary(dll); return false; }
    *out = arc;
    if (dllOut) *dllOut = dll;
    return true;
}

} // namespace

bool TryParseSevenZipDll(const std::wstring& path, ArchiveInfo& info) {
    info = ArchiveInfo{};
    info.format = L"7Z";

    HMODULE dll = nullptr;
    IInArchive7z* arc = nullptr;
    if (!Make7zArchive(&arc, &dll)) {
        info.error = L"7-Zip DLL not available";
        return false; // caller falls back to native parser
    }

    bool ok = false;
    InFileStream* stream = new InFileStream();
    stream->AddRef();
    if (!stream->Open(path.c_str())) {
        info.error = L"Cannot open file";
        stream->Release();
        arc->Release();
        FreeLibrary(dll);
        return false;
    }

    ULARGE_INTEGER fsz{};
    if (GetFileSizeEx(stream->Handle(), reinterpret_cast<PLARGE_INTEGER>(&fsz)))
        info.archiveSize = fsz.QuadPart;

    OpenCallback* cb = new OpenCallback();
    cb->AddRef();

    HRESULT hr = arc->Open(stream, nullptr, cb);
    if (SUCCEEDED(hr)) {
        uint32_t n = 0;
        if (SUCCEEDED(arc->GetNumberOfItems(&n))) {
            if (n == 0) {
                // Valid signature but no entries => truncated/corrupt/fake.
                info.error = L"7-Zip: archive has no entries (truncated or corrupt)";
            } else {
            // Archive-level method (dominant algorithm).
            {
                PROPVARIANT p; PropVariantInit(&p);
                if (SUCCEEDED(arc->GetArchiveProperty(kpidMethod, &p))) {
                    std::wstring m; if (VarToWide(p, &m)) info.algorithm = m;
                    ClearVariant(p);
                }
            }
            bool anyEncrypted = false;
            for (uint32_t i = 0; i < n; ++i) {
                ArchiveEntry e;
                PROPVARIANT p; PropVariantInit(&p);
                if (SUCCEEDED(arc->GetProperty(i, kpidPath, &p))) {
                    VarToWide(p, &e.path); ClearVariant(p);
                }
                if (e.path.empty()) continue; // skip nameless (rare)
                if (SUCCEEDED(arc->GetProperty(i, kpidSize, &p))) {
                    VarToUInt64(p, &e.unpackedSize); ClearVariant(p);
                }
                if (SUCCEEDED(arc->GetProperty(i, kpidPackSize, &p))) {
                    VarToUInt64(p, &e.packedSize); ClearVariant(p);
                }
                bool isDir = false;
                if (SUCCEEDED(arc->GetProperty(i, kpidIsDir, &p))) {
                    VarToBool(p, &isDir); ClearVariant(p);
                }
                e.isDir = isDir;
                if (SUCCEEDED(arc->GetProperty(i, kpidMTime, &p))) {
                    if (p.vt == VT_FILETIME) e.mtime = p.filetime;
                    ClearVariant(p);
                }
                if (!info.algorithm.empty()) e.methodName = info.algorithm;
                info.entries.push_back(e);
                if (e.isDir) info.dirCount++; else info.fileCount++;
                info.totalUnpacked += e.unpackedSize;
                info.totalPacked += e.packedSize;
            }
            // Encrypted flag at archive level.
            {
                PROPVARIANT p; PropVariantInit(&p);
                if (SUCCEEDED(arc->GetArchiveProperty(kpidEncrypted, &p))) {
                    bool enc = false; VarToBool(p, &enc); anyEncrypted = enc; ClearVariant(p);
                }
            }
            if (anyEncrypted && info.algorithm.empty()) info.algorithm = L"Encrypted";
            info.perEntryCompression = true;
            ok = true;
            }
        }
        arc->Close();
    }
    if (!ok && info.error.empty()) info.error = L"7-Zip DLL failed to open archive";

    cb->Release();
    stream->Release();
    arc->Release();
    FreeLibrary(dll);
    return ok;
}

// ============================================================================
// RAR via unrar.dll (works for RAR4 + RAR5, no header-walking).
// ============================================================================
namespace {

typedef HANDLE(PASCAL* RarOpenArchiveExFn)(void* ArchiveData);
typedef int   (PASCAL* RarCloseArchiveFn)(HANDLE hArcData);
typedef int   (PASCAL* RarReadHeaderExFn)(HANDLE hArcData, void* HeaderData);
typedef int   (PASCAL* RarProcessFileWFn)(HANDLE hArcData, int Operation,
                                          wchar_t* DestPath, wchar_t* DestName);

#pragma pack(push, 1)
struct RarHeaderDataEx {
    char         ArcName[1024];
    wchar_t      ArcNameW[1024];
    char         FileName[1024];
    wchar_t      FileNameW[1024];
    unsigned int Flags;
    unsigned int PackSize;
    unsigned int PackSizeHigh;
    unsigned int UnpSize;
    unsigned int UnpSizeHigh;
    unsigned int HostOS;
    unsigned int FileCRC;
    unsigned int FileTime;
    unsigned int UnpVer;
    unsigned int Method;
    unsigned int FileAttr;
    char*        CmtBuf;
    unsigned int CmtBufSize;
    unsigned int CmtSize;
    unsigned int CmtState;
    unsigned int DictSize;
    unsigned int HashType;
    char         Hash[32];
    unsigned int RedirType;
    wchar_t*     RedirName;
    unsigned int RedirNameSize;
    unsigned int DirTarget;
    unsigned int MtimeLow;
    unsigned int MtimeHigh;
    unsigned int CtimeLow;
    unsigned int CtimeHigh;
    unsigned int AtimeLow;
    unsigned int AtimeHigh;
    unsigned int Reserved[988];
};

struct RarOpenArchiveDataEx {
    char*         ArcName;
    wchar_t*      ArcNameW;
    unsigned int  OpenMode;
    unsigned int  OpenResult;
    char*         CmtBuf;
    unsigned int  CmtBufSize;
    unsigned int  CmtSize;
    unsigned int  CmtState;
    unsigned int  Flags;
    void*         Callback;
    long          UserData;
    unsigned int  OpFlags;
    wchar_t*      CmtBufW;
    unsigned int  Reserved[25];
};
#pragma pack(pop)

const unsigned int RAR_OM_LIST     = 0;
const int          RAR_SKIP        = 0;
const int          ERAR_END_ARCHIVE = 10;
const int          ERAR_BAD_ARCHIVE = 13;
const unsigned int RHDF_DIRECTORY  = 0x20;

HMODULE LoadUnrarDll() {
#ifdef _WIN64
    return LoadArchiverDll(L"UNRAR64.DLL");
#else
    return LoadArchiverDll(L"UNRAR.DLL");
#endif
}

// Map RAR method nibble to a friendly label.
std::wstring RarMethodName(unsigned int method) {
    if (method == 0) return L"Store";
    wchar_t buf[16];
    swprintf(buf, 16, L"m%u", method);
    return buf;
}

} // namespace

bool TryParseRarDll(const std::wstring& path, ArchiveInfo& info) {
    info = ArchiveInfo{};
    info.format = L"RAR";

    HMODULE dll = LoadUnrarDll();
    if (!dll) {
        info.error = L"unrar DLL not available";
        return false; // caller falls back to native parser
    }
    RarOpenArchiveExFn OpenArchive = (RarOpenArchiveExFn)GetProcAddress(dll, "RAROpenArchiveEx");
    RarReadHeaderExFn  ReadHeader  = (RarReadHeaderExFn)GetProcAddress(dll, "RARReadHeaderEx");
    RarProcessFileWFn  ProcessFile = (RarProcessFileWFn)GetProcAddress(dll, "RARProcessFileW");
    RarCloseArchiveFn  CloseArchive = (RarCloseArchiveFn)GetProcAddress(dll, "RARCloseArchive");
    if (!OpenArchive || !ReadHeader || !ProcessFile || !CloseArchive) {
        FreeLibrary(dll);
        info.error = L"unrar DLL exports missing";
        return false;
    }

    // Get archive size via a plain open.
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz{};
            if (GetFileSizeEx(h, &sz)) info.archiveSize = (uint64_t)sz.QuadPart;
            CloseHandle(h);
        }
    }

    RarOpenArchiveDataEx od{};
    std::vector<wchar_t> nameBuf(path.begin(), path.end());
    nameBuf.push_back(L'\0');
    od.ArcNameW = nameBuf.data();
    // Also provide the ANSI name: some unrar builds only consult ArcName.
    std::vector<char> ansiBuf(path.size() + 1, '\0');
    WideCharToMultiByte(CP_ACP, 0, path.c_str(), -1,
                        ansiBuf.data(), (int)ansiBuf.size(), nullptr, nullptr);
    od.ArcName = ansiBuf.data();
    od.OpenMode = RAR_OM_LIST;

    HANDLE hArc = OpenArchive(&od);
    if (!hArc || od.OpenResult != 0) {
        if (hArc) CloseArchive(hArc);
        FreeLibrary(dll);
        info.error = L"unrar: cannot open archive (corrupt or unsupported)";
        return false;
    }

    bool ok = false;
    bool solid = (od.Flags & 0x0008) != 0; // ROADF_SOLID
    while (true) {
        RarHeaderDataEx hd{};
        int r = ReadHeader(hArc, &hd);
        if (r != 0) {
            if (r == ERAR_END_ARCHIVE || r == ERAR_BAD_ARCHIVE) ok = true;
            break;
        }
        ArchiveEntry e;
        e.path = hd.FileNameW ? std::wstring(hd.FileNameW) : std::wstring();
        if (e.path.empty()) e.path = internal::decodeAscii(hd.FileName, 1024); // ANSI fallback
        e.isDir = (hd.Flags & RHDF_DIRECTORY) != 0;
        e.packedSize = (uint64_t)hd.PackSize | ((uint64_t)hd.PackSizeHigh << 32);
        e.unpackedSize = (uint64_t)hd.UnpSize | ((uint64_t)hd.UnpSizeHigh << 32);
        // Prefer precise NTFS time if present, else DOS time.
        if (hd.MtimeLow || hd.MtimeHigh) {
            e.mtime.dwLowDateTime = hd.MtimeLow;
            e.mtime.dwHighDateTime = hd.MtimeHigh;
        } else {
            e.mtime = internal::dosTimeToFT((uint16_t)(hd.FileTime >> 16),
                                            (uint16_t)(hd.FileTime & 0xFFFF));
        }
        e.methodName = RarMethodName(hd.Method);
        e.method = hd.Method;
        info.entries.push_back(e);
        if (e.isDir) info.dirCount++; else info.fileCount++;
        info.totalUnpacked += e.unpackedSize;
        info.totalPacked += e.packedSize;

        if (ProcessFile(hArc, RAR_SKIP, nullptr, nullptr) != 0) break;
    }
    CloseArchive(hArc);
    FreeLibrary(dll);

    if (!ok && (info.fileCount || info.dirCount)) ok = true; // partial but usable
    if (ok) {
        // Dominant algorithm label.
        if (solid) info.algorithm = L"RAR (solid)";
        else if (info.fileCount || info.dirCount) info.algorithm = L"RAR";
        info.perEntryCompression = true;
    } else if (info.error.empty()) {
        info.error = L"unrar: no entries or read failure";
    }
    return ok;
}

} // namespace archdiview
