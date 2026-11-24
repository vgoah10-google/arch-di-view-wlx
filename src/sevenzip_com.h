// sevenzip_com.h — Minimal, self-contained 7-Zip IInArchive COM declarations.
//
// We deliberately do NOT include the upstream 7z SDK headers (IStream.h /
// IArchive.h / MyCom.h) because MyCom.h depends on ATL. Instead we declare only
// the handful of interfaces the listing path needs, with the exact vtable order
// and GUIDs the 7-Zip DLL expects. This keeps the plugin free of any external
// dependency beyond the archiver DLL itself.
//
// GUIDs follow the 7z convention (see IDecl.h):
//   Data1=0x23170F69, Data2=0x40C1, Data3=0x278A,
//   Data4 = { 0,0,0,(groupId),0,(subId),0,0 }
#ifndef ARCHDIVIEW_SEVENZIP_COM_H
#define ARCHDIVIEW_SEVENZIP_COM_H

#include <windows.h>
#include <unknwn.h>
#include <propidl.h>   // PROPVARIANT, PROPID
#include <oleauto.h>   // BSTR, SysFreeString / SysAllocString

#include <stdint.h>

namespace szdll {

typedef uint8_t  Byte;
typedef int32_t  Int32;
typedef int64_t  Int64;
typedef uint32_t UInt32;
typedef uint64_t UInt64;

// ---- Interface GUIDs (groupId/subId per the 7z SDK) ------------------------
// {23170F69-40C1-278A-00 00 00 03 00 01 00 00}
static const GUID kIID_ISequentialInStream =
    {0x23170F69, 0x40C1, 0x278A, {0x00, 0x00, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00}};
// {23170F69-40C1-278A-00 00 00 03 00 03 00 00}
static const GUID kIID_IInStream =
    {0x23170F69, 0x40C1, 0x278A, {0x00, 0x00, 0x00, 0x03, 0x00, 0x03, 0x00, 0x00}};
// {23170F69-40C1-278A-00 00 00 03 00 06 00 00}
static const GUID kIID_IStreamGetSize =
    {0x23170F69, 0x40C1, 0x278A, {0x00, 0x00, 0x00, 0x03, 0x00, 0x06, 0x00, 0x00}};
// {23170F69-40C1-278A-00 00 00 06 00 10 00 00}
static const GUID kIID_IArchiveOpenCallback =
    {0x23170F69, 0x40C1, 0x278A, {0x00, 0x00, 0x00, 0x06, 0x00, 0x10, 0x00, 0x00}};
// {23170F69-40C1-278A-00 00 00 05 00 10 00 00}
static const GUID kIID_ICryptoGetTextPassword =
    {0x23170F69, 0x40C1, 0x278A, {0x00, 0x00, 0x00, 0x05, 0x00, 0x10, 0x00, 0x00}};
// {23170F69-40C1-278A-00 00 00 06 00 60 00 00}
static const GUID kIID_IInArchive =
    {0x23170F69, 0x40C1, 0x278A, {0x00, 0x00, 0x00, 0x06, 0x00, 0x60, 0x00, 0x00}};
// CLSID_CFormat7z — the 7z archive-handler COM class.
static const GUID kCLSID_CFormat7z =
    {0x23170F69, 0x40C1, 0x278A, {0x10, 0x00, 0x00, 0x01, 0x10, 0x07, 0x00, 0x00}};

// ---- Property ids (subset of the 7z PropID.h enum) -------------------------
enum {
    kpidNoProperty = 0,
    kpidMainSubfile,      // 1
    kpidHandlerItemIndex, // 2
    kpidPath,             // 3
    kpidName,             // 4
    kpidExtension,        // 5
    kpidIsDir,            // 6
    kpidSize,             // 7
    kpidPackSize,         // 8
    kpidAttrib,           // 9
    kpidCTime,            // 10
    kpidATime,            // 11
    kpidMTime,            // 12
    kpidSolid,            // 13
    kpidCommented,        // 14
    kpidEncrypted,        // 15
    kpidSplitBefore,      // 16
    kpidSplitAfter,       // 17
    kpidDictionarySize,   // 18
    kpidCRC,              // 19
    kpidType,             // 20
    kpidIsAnti,           // 21
    kpidMethod,           // 22
};

// ---- Stream interfaces (vtable order must match the 7z DLL) ----------------
struct ISequentialInStream7z : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Read(void* data, UInt32 size,
                                           UInt32* processedSize) = 0;
};

struct IInStream7z : public ISequentialInStream7z {
    virtual HRESULT STDMETHODCALLTYPE Seek(Int64 offset, UInt32 seekOrigin,
                                           UInt64* newPosition) = 0;
};

struct IStreamGetSize7z : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetSize(UInt64* size) = 0;
};

// ---- Open callback + password ---------------------------------------------
struct IArchiveOpenCallback7z : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetTotal(const UInt64* files,
                                               const UInt64* bytes) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64* files,
                                                   const UInt64* bytes) = 0;
};

struct ICryptoGetTextPassword7z : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR* password) = 0;
};

// ---- The archive reader (IInArchive) ---------------------------------------
struct IInArchive7z : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Open(IInStream7z* stream,
                                           const UInt64* maxCheckStartPosition,
                                           IArchiveOpenCallback7z* openCallback) = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNumberOfItems(UInt32* numItems) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProperty(UInt32 index, PROPID propID,
                                                  PROPVARIANT* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE Extract(const UInt32* indices, UInt32 numItems,
                                             Int32 testMode, void* extractCallback) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetArchiveProperty(PROPID propID,
                                                         PROPVARIANT* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNumberOfProperties(UInt32* numProps) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyInfo(UInt32 index, BSTR* name,
                                                      PROPID* propID, VARTYPE* varType) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNumberOfArchiveProperties(UInt32* numProps) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetArchivePropertyInfo(UInt32 index, BSTR* name,
                                                             PROPID* propID, VARTYPE* varType) = 0;
};

} // namespace szdll

#endif // ARCHDIVIEW_SEVENZIP_COM_H
