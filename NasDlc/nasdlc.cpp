// NasDlc.cpp  (version 13.3)
// DashLaunch plugin: load DLC (00000002) and title updates (000B0000) from the NAS.
//
// How it works: the plugin patches the kernel file imports of XAM.
//  - Folder open:  if XAM opens ...\Content\<ID>\<TitleID>\<type>\ on the HDD,
//                  the plugin also opens the same folder on the NAS.
//                  If the HDD folder does not exist, XAM gets the NAS folder.
//  - Listing:      after the last HDD entry, the listing continues with the NAS
//                  entries. Names that are on the HDD too are skipped.
//  - Files:        if a package or title update is not on the HDD, the open
//                  goes to the NAS. Only opens of existing files are
//                  redirected, never the creation of new files.
//  - Close:        the extra NAS folder handle is closed with the HDD handle.
//  - Cache:        the start of each NAS package stays in memory, so that the
//                  many small header reads of XAM need few NAS requests.
//                  Reads without an offset (current position) always go to
//                  the NAS, because XAM sets that position with a seek.
//  - Prefetch:     optional (Speed.Prefetch). Worker threads list each NAS DLC
//                  folder and fill the cache of each package before XAM
//                  opens it.
//
// All hooks pass every parameter through as a raw 64-bit value.
// (Version 6 showed that a type conversion breaks NtQueryDirectoryFile.)
//
// Settings: NasDlc.ini in the root of the first USB drive (optional).
// Log:      NasDlc.log in the root of the first USB drive (FAT32).
//
// Build: Title (.xex) + /DLL /ENTRY:"_DllMainCRTStartup" /ALIGN:128,4096
//        + NasDlc.xml (sysdll, base 0x91E00000).

#include <xtl.h>
#include <xkelib.h>          // TODO: use the exact main header name of your xkelib
#include <ppcintrinsics.h>   // __mftb (time base, 50 MHz)
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#define PLUGIN_NAME   "NasDlc v13.3"

// ---------------------------------------------------------------------------
// Fixed values
// ---------------------------------------------------------------------------

#define INI_PATH           "UsbX:\\NasDlc.ini"
#define LOG_PATH           "UsbX:\\NasDlc.log"
#define POLL_INTERVAL_MS   5
#define FLUSH_EVERY_POLLS  100        // 0.5 s
#define NAS_CHECK_MS       2000       // Check for the NAS every 2 s until connected
#define MAX_ORDINAL        1024
#define CFG_PATH_LEN       128

#ifndef FILE_DIRECTORY_FILE
#define FILE_DIRECTORY_FILE 0x00000001
#endif
#define ST_NO_MORE_FILES      ((NTSTATUS)0x80000006)
#define ST_NAME_NOT_FOUND     ((NTSTATUS)0xC0000034)
#define ST_PATH_NOT_FOUND     ((NTSTATUS)0xC000003A)
#define ST_NO_SUCH_FILE       ((NTSTATUS)0xC000000F)
#define ST_ACCESS_DENIED      ((NTSTATUS)0xC0000022)
// Status field of IO_STATUS_BLOCK (offset 0). xkelib uses a different member name.
#define IOSB_STATUS(p)  (*(NTSTATUS*)(p))

#ifndef STATUS_END_OF_FILE
#define STATUS_END_OF_FILE    ((NTSTATUS)0xC0000011)
#endif
#ifndef STATUS_PENDING
#define STATUS_PENDING        ((NTSTATUS)0x00000103)
#endif

// Write-type access bits: FILE_WRITE_DATA, FILE_APPEND_DATA, FILE_WRITE_EA,
// FILE_WRITE_ATTRIBUTES, DELETE, WRITE_DAC, WRITE_OWNER, GENERIC_ALL, GENERIC_WRITE.
// 0012019F & ~WRITE_ACCESS_MASK = 00120089 (the read-only retry of XAM).
#define WRITE_ACCESS_MASK   0x500D0116
#define TB_PER_US           50          // Time base: 50 MHz

typedef unsigned __int64 U64;
#define PTR(x)    ((void*)(DWORD)(x))
#define RAW(p)    ((U64)(DWORD)(p))
#define STATUS(r) ((NTSTATUS)(DWORD)(r))

// ---------------------------------------------------------------------------
// Settings (NasDlc.ini)
// ---------------------------------------------------------------------------

typedef struct _CONFIG {
    char hddContent[CFG_PATH_LEN];   // HDD content root, as XAM sees it
    char nasContent[CFG_PATH_LEN];   // NAS content root, as XAM sees it
    BOOL dlc;                        // Redirect DLC (00000002)
    BOOL tu;                         // Redirect title updates (000B0000)
    BOOL notifyStart;                // Notify: plugin active, NAS connected
    BOOL notifyFound;                // Notify: DLC or title update from the NAS
    int  logLevel;                   // 0 = off, 1 = important lines, 2 = all details
    BOOL tuTrace;                    // Log title update searches (diagnostic)
    BOOL contentTrace;               // Log all XAM accesses to \Content\ paths (diagnostic)
    BOOL cache;                      // Header cache on/off
    BOOL cacheVerify;                // Diagnostic: compare cache data with NAS data
    BOOL prefetch;                   // Fill the package caches with worker threads
    int  pfThreads;                  // Number of prefetch worker threads (1 to 4)
} CONFIG;

static CONFIG g_Cfg;
static int    g_IniState;            // -1 = no file, 0 = not read, 1 = read
static int    g_IniKeys, g_IniUnknown;

#define TYPE_DLC  0
#define TYPE_TU   1
static const char* g_Types[] = { "00000002", "000B0000" };
#define TYPE_COUNT 2

static BOOL TypeEnabled(int t) { return t == TYPE_DLC ? g_Cfg.dlc : g_Cfg.tu; }

static void ConfigDefaults()
{
    strcpy(g_Cfg.hddContent, "\\Device\\Harddisk0\\Partition1\\Content\\");
    strcpy(g_Cfg.nasContent, "\\Network\\Smb\\DOCKER\\XBOXSMB\\Content\\");
    g_Cfg.dlc         = TRUE;
    g_Cfg.tu          = TRUE;
    g_Cfg.notifyStart = TRUE;
    g_Cfg.notifyFound = TRUE;
    g_Cfg.logLevel    = 1;
    g_Cfg.tuTrace     = FALSE;
    g_Cfg.contentTrace = FALSE;
    g_Cfg.cache       = TRUE;
    g_Cfg.cacheVerify = FALSE;
    g_Cfg.prefetch    = FALSE;
    g_Cfg.pfThreads   = 2;
}

static char* Trim(char* s)
{
    while (*s == ' ' || *s == '\t') s++;
    char* e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

static BOOL ParseBool(const char* v, BOOL def)
{
    if (!_stricmp(v, "1") || !_stricmp(v, "true")  || !_stricmp(v, "yes") || !_stricmp(v, "on"))  return TRUE;
    if (!_stricmp(v, "0") || !_stricmp(v, "false") || !_stricmp(v, "no")  || !_stricmp(v, "off")) return FALSE;
    return def;
}

// Device paths only ("\Device\...", "\Network\..."). Adds the final backslash.
static void SetPath(char* dst, const char* v)
{
    size_t n = strlen(v);
    if (n < 2 || v[0] != '\\' || n + 2 > CFG_PATH_LEN) return;
    strcpy(dst, v);
    if (dst[n - 1] != '\\') { dst[n] = '\\'; dst[n + 1] = 0; }
}

static void LoadConfig()
{
    ConfigDefaults();
    HANDLE h = CreateFile(INI_PATH, GENERIC_READ, FILE_SHARE_READ, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { g_IniState = -1; return; }

    static char text[8192];
    DWORD got = 0;
    ReadFile(h, text, sizeof(text) - 1, &got, NULL);
    CloseHandle(h);
    text[got] = 0;
    g_IniState = 1;

    char  section[32] = "";
    char* line = text;
    if ((BYTE)line[0] == 0xEF && (BYTE)line[1] == 0xBB && (BYTE)line[2] == 0xBF) line += 3;   // UTF-8 BOM

    while (line && *line) {
        char* next = strchr(line, '\n');
        if (next) *next++ = 0;
        char* s = Trim(line);
        line = next;
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') {
            char* e = strchr(s, ']');
            if (e) { *e = 0; strncpy(section, s + 1, sizeof(section) - 1); section[sizeof(section) - 1] = 0; }
            continue;
        }
        char* eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char* key = Trim(s);
        char* val = Trim(eq + 1);
        char  full[80];
        _snprintf(full, sizeof(full) - 1, "%s.%s", section, key);
        full[sizeof(full) - 1] = 0;
        g_IniKeys++;

        if      (!_stricmp(full, "Paths.HddContent"))    SetPath(g_Cfg.hddContent, val);
        else if (!_stricmp(full, "Paths.NasContent"))    SetPath(g_Cfg.nasContent, val);
        else if (!_stricmp(full, "Content.Dlc"))         g_Cfg.dlc         = ParseBool(val, g_Cfg.dlc);
        else if (!_stricmp(full, "Content.TitleUpdates")) g_Cfg.tu         = ParseBool(val, g_Cfg.tu);
        else if (!_stricmp(full, "Notify.Start"))        g_Cfg.notifyStart = ParseBool(val, g_Cfg.notifyStart);
        else if (!_stricmp(full, "Notify.Found"))        g_Cfg.notifyFound = ParseBool(val, g_Cfg.notifyFound);
        else if (!_stricmp(full, "Log.Level"))           { int v = atoi(val); if (v >= 0 && v <= 2) g_Cfg.logLevel = v; }
        else if (!_stricmp(full, "Log.TuTrace"))         g_Cfg.tuTrace     = ParseBool(val, g_Cfg.tuTrace);
        else if (!_stricmp(full, "Log.ContentTrace"))    g_Cfg.contentTrace = ParseBool(val, g_Cfg.contentTrace);
        else if (!_stricmp(full, "Cache.Enabled"))       g_Cfg.cache       = ParseBool(val, g_Cfg.cache);
        else if (!_stricmp(full, "Cache.Verify"))        g_Cfg.cacheVerify = ParseBool(val, g_Cfg.cacheVerify);
        else if (!_stricmp(full, "Speed.Prefetch"))      g_Cfg.prefetch    = ParseBool(val, g_Cfg.prefetch);
        else if (!_stricmp(full, "Speed.PrefetchThreads")) { int v = atoi(val); if (v >= 1 && v <= 4) g_Cfg.pfThreads = v; }
        else g_IniUnknown++;
    }
}

// ---------------------------------------------------------------------------
// Buffered log (the hooks only copy text; the watch thread writes it)
// ---------------------------------------------------------------------------

#define LOG_BUF_SIZE 0x8000

static CRITICAL_SECTION g_LogLock;
static CRITICAL_SECTION g_FileLock;     // Only one writer of the log file at a time
static DWORD            g_StartTick;
static LONG             g_MountStatus;
static char             g_LogBuf[LOG_BUF_SIZE];
static char             g_LogOut[LOG_BUF_SIZE];
static DWORD            g_LogLen;
static DWORD            g_LogDropped;

static void MountUsbDrive()
{
    STRING link, device;
    RtlInitAnsiString(&link,   "\\System??\\UsbX:");
    RtlInitAnsiString(&device, "\\Device\\Mass0");
    g_MountStatus = ObCreateSymbolicLink(&link, &device);
}

// Log level 1: important lines.
static void Log(const char* fmt, ...)
{
    if (g_Cfg.logLevel < 1) return;
    char buf[512];
    int  len = _snprintf(buf, sizeof(buf) - 3, "[%7u] ", GetTickCount() - g_StartTick);
    va_list args;
    va_start(args, fmt);
    _vsnprintf(buf + len, sizeof(buf) - 3 - len, fmt, args);
    va_end(args);
    buf[sizeof(buf) - 3] = 0;
    strcat(buf, "\r\n");
    DWORD n = (DWORD)strlen(buf);

    EnterCriticalSection(&g_LogLock);
    if (g_LogLen + n <= LOG_BUF_SIZE) { memcpy(g_LogBuf + g_LogLen, buf, n); g_LogLen += n; }
    else g_LogDropped++;
    LeaveCriticalSection(&g_LogLock);
}

// Log level 2: all details.
#define LogV(...) do { if (g_Cfg.logLevel >= 2) Log(__VA_ARGS__); } while (0)

static void FlushLog()
{
    EnterCriticalSection(&g_FileLock);
    DWORD len, dropped;
    EnterCriticalSection(&g_LogLock);
    len = g_LogLen; dropped = g_LogDropped;
    if (len) memcpy(g_LogOut, g_LogBuf, len);
    g_LogLen = 0; g_LogDropped = 0;
    LeaveCriticalSection(&g_LogLock);
    if (!len && !dropped) { LeaveCriticalSection(&g_FileLock); return; }

    HANDLE h = CreateFile(LOG_PATH, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { LeaveCriticalSection(&g_FileLock); return; }
    SetFilePointer(h, 0, NULL, FILE_END);
    DWORD written;
    if (len) WriteFile(h, g_LogOut, len, &written, NULL);
    if (dropped) {
        char msg[64];
        sprintf(msg, "[log buffer full: %u lines dropped]\r\n", dropped);
        WriteFile(h, msg, (DWORD)strlen(msg), &written, NULL);
    }
    CloseHandle(h);
    LeaveCriticalSection(&g_FileLock);
}

// ---------------------------------------------------------------------------
// Notifications (sent only from the watch thread, never from a hook)
// ---------------------------------------------------------------------------

typedef VOID (*PFN_XNotifyQueueUI)(DWORD, DWORD, DWORD, LPCWSTR, PVOID);

static void Notify(const char* text)
{
    WCHAR w[128];
    int i = 0;
    for (; i < 127 && text[i]; i++) w[i] = (WCHAR)(BYTE)text[i];
    w[i] = 0;
    HANDLE xam = NULL; PVOID addr = NULL;
    if (XexGetModuleHandle("xam.xex", &xam) == 0 &&
        XexGetProcedureAddress(xam, xamExp_XNotifyQueueUI, &addr) == 0 && addr)
        ((PFN_XNotifyQueueUI)addr)(14, 0xFF, 2, w, NULL);
}

// Events from the hooks, for the watch thread.
static CRITICAL_SECTION g_EventLock;
static int              g_PendingDlc;        // NAS DLC count of the last listing
static char             g_PendingTu[48];     // Name of a title update opened on the NAS

static void ReportDlc(int count)
{
    EnterCriticalSection(&g_EventLock);
    if (count > g_PendingDlc) g_PendingDlc = count;
    LeaveCriticalSection(&g_EventLock);
}

static void ReportTu(const char* path)
{
    const char* n = strrchr(path, '\\');
    n = n ? n + 1 : path;
    EnterCriticalSection(&g_EventLock);
    strncpy(g_PendingTu, n, sizeof(g_PendingTu) - 1);
    g_PendingTu[sizeof(g_PendingTu) - 1] = 0;
    LeaveCriticalSection(&g_EventLock);
}

// ---------------------------------------------------------------------------
// Path mapping
// ---------------------------------------------------------------------------

static BOOL StartsWithI(const char* s, const char* p)
{
    for (; *p; s++, p++)
        if (!*s || ((*s | 0x20) != (*p | 0x20))) return FALSE;
    return TRUE;
}

static BOOL ContainsI(const char* s, const char* sub)
{
    for (; *s; s++) if (StartsWithI(s, sub)) return TRUE;
    return FALSE;
}

static BOOL IsHex(char c)
{
    return (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'f');
}

// "<HddContent><16 hex>\<8 hex>\<type>[\...]" -> "<NasContent><16 hex>\<8 hex>\<type>[\...]"
static BOOL MapToNas(const char* in, char* out, int max, int* type)
{
    if (!StartsWithI(in, g_Cfg.hddContent)) return FALSE;
    const char* r = in + strlen(g_Cfg.hddContent);
    for (int i = 0; i < 16; i++) if (!IsHex(r[i])) return FALSE;
    if (r[16] != '\\') return FALSE;
    for (int i = 17; i < 25; i++) if (!IsHex(r[i])) return FALSE;
    if (r[25] != '\\') return FALSE;
    int t = -1;
    for (int i = 0; i < TYPE_COUNT; i++)
        if (StartsWithI(r + 26, g_Types[i])) { t = i; break; }
    if (t < 0 || !TypeEnabled(t)) return FALSE;
    char c = r[26 + 8];
    if (c != 0 && c != '\\') return FALSE;
    int n = _snprintf(out, max, "%s%s", g_Cfg.nasContent, r);
    if (n < 0 || n >= max) return FALSE;
    *type = t;
    return TRUE;
}

// Diagnostic: log each XAM file access that looks like a title update.
static void TraceTu(const char* op, U64 oaRaw, U64 access, U64 r)
{
    if (!g_Cfg.tuTrace) return;
    POBJECT_ATTRIBUTES oa = (POBJECT_ATTRIBUTES)PTR(oaRaw);
    if (!oa || !MmIsAddressValid(oa)) return;
    STRING* s = oa->ObjectName;
    if (!s || !MmIsAddressValid(s) || !s->Buffer || !MmIsAddressValid(s->Buffer)) return;
    char p[300];
    DWORD n = s->Length < sizeof(p) - 1 ? s->Length : sizeof(p) - 1;
    memcpy(p, s->Buffer, n);
    p[n] = 0;
    if (!ContainsI(p, "000B0000") && !ContainsI(p, "\\tu0") && !ContainsI(p, "\\tu_") &&
        !ContainsI(p, "TitleUpdate"))
        return;
    Log("TU %s %s%s access %08X = %08X", op,
        oa->RootDirectory ? "(relative to handle) " : "", p, (DWORD)access, (DWORD)r);
}

// Diagnostic: log each XAM access to a path that contains "\Content\", on all devices.
static void TraceContent(const char* op, U64 oaRaw, U64 r)
{
    if (!g_Cfg.contentTrace) return;
    POBJECT_ATTRIBUTES oa = (POBJECT_ATTRIBUTES)PTR(oaRaw);
    if (!oa || !MmIsAddressValid(oa)) return;
    STRING* s = oa->ObjectName;
    if (!s || !MmIsAddressValid(s) || !s->Buffer || !MmIsAddressValid(s->Buffer)) return;
    char p[300];
    DWORD n = s->Length < sizeof(p) - 1 ? s->Length : sizeof(p) - 1;
    memcpy(p, s->Buffer, n);
    p[n] = 0;
    if (!ContainsI(p, "\\Content\\")) return;
    Log("CT %s %s%s = %08X", op, oa->RootDirectory ? "(relative) " : "", p, (DWORD)r);
}

typedef struct _NASPATH {
    OBJECT_ATTRIBUTES oa;
    STRING            name;
    int               type;
    char              buf[300];
} NASPATH;

// Build the NAS object attributes for an HDD content path. FALSE = no mapping.
static BOOL BuildNas(POBJECT_ATTRIBUTES src, NASPATH* np)
{
    if (!src || !MmIsAddressValid(src) || src->RootDirectory) return FALSE;
    STRING* s = src->ObjectName;
    if (!s || !MmIsAddressValid(s) || !s->Buffer || !MmIsAddressValid(s->Buffer)) return FALSE;

    char in[300];
    if (s->Length >= sizeof(in)) return FALSE;
    memcpy(in, s->Buffer, s->Length);
    in[s->Length] = 0;

    if (!MapToNas(in, np->buf, sizeof(np->buf), &np->type)) return FALSE;
    RtlInitAnsiString(&np->name, np->buf);
    np->oa = *src;
    np->oa.ObjectName = &np->name;
    np->oa.RootDirectory = NULL;
    return TRUE;
}

static BOOL IsNotFound(NTSTATUS st)
{
    return st == ST_NAME_NOT_FOUND || st == ST_PATH_NOT_FOUND || st == ST_NO_SUCH_FILE;
}

// ---------------------------------------------------------------------------
// Folder state (merge of HDD and NAS listings)
// ---------------------------------------------------------------------------

#define MAX_DIRS   16
#define MAX_SEEN   64
#define NAME_LEN   48

typedef struct _DIRSTATE {
    HANDLE h;          // Handle that XAM uses
    HANDLE nas;        // Extra NAS handle (merge), or NULL if h itself is on the NAS
    int    type;       // TYPE_DLC or TYPE_TU
    int    phase;      // 0 = HDD entries, 1 = NAS entries
    int    nasCount;   // NAS entries given to XAM in this scan
    DWORD  nasStart;   // Tick of the first NAS entry in this scan
    char   nasPath[300]; // NAS folder path (for the list of known NAS files)
    int    nseen;
    char   seen[MAX_SEEN][NAME_LEN];
} DIRSTATE;

static CRITICAL_SECTION g_DirLock;
static DIRSTATE         g_Dirs[MAX_DIRS];

static void DirAdd(HANDLE h, HANDLE nas, int phase, int type, const char* nasPath)
{
    EnterCriticalSection(&g_DirLock);
    for (int i = 0; i < MAX_DIRS; i++) {
        if (!g_Dirs[i].h) {
            g_Dirs[i].h = h; g_Dirs[i].nas = nas; g_Dirs[i].phase = phase;
            g_Dirs[i].type = type; g_Dirs[i].nasCount = 0; g_Dirs[i].nseen = 0;
            g_Dirs[i].nasStart = 0;
            strncpy(g_Dirs[i].nasPath, nasPath, sizeof(g_Dirs[i].nasPath) - 1);
            g_Dirs[i].nasPath[sizeof(g_Dirs[i].nasPath) - 1] = 0;
            LeaveCriticalSection(&g_DirLock);
            return;
        }
    }
    LeaveCriticalSection(&g_DirLock);
    Log("WARNING: folder table full");
}

static DIRSTATE* DirFind(HANDLE h)
{
    DIRSTATE* d = NULL;
    EnterCriticalSection(&g_DirLock);
    for (int i = 0; i < MAX_DIRS; i++) if (h && g_Dirs[i].h == h) { d = &g_Dirs[i]; break; }
    LeaveCriticalSection(&g_DirLock);
    return d;
}

// Remove the entry. Returns the extra NAS handle to close, or NULL.
static HANDLE DirRemove(HANDLE h)
{
    HANDLE nas = NULL;
    EnterCriticalSection(&g_DirLock);
    for (int i = 0; i < MAX_DIRS; i++) {
        if (h && g_Dirs[i].h == h) {
            nas = g_Dirs[i].nas;
            g_Dirs[i].h = NULL; g_Dirs[i].nas = NULL;
            break;
        }
    }
    LeaveCriticalSection(&g_DirLock);
    return nas;
}

// FILE_DIRECTORY_INFORMATION: FileNameLength at 0x3C, FileName at 0x40.
// Returns FALSE if the buffer holds more than one entry or is not readable.
static BOOL SingleEntryName(PVOID info, char* out)
{
    if (!info || !MmIsAddressValid(info)) return FALSE;
    BYTE* e = (BYTE*)info;
    if (*(DWORD*)(e + 0x00) != 0) return FALSE;        // More than one entry
    DWORD n = *(DWORD*)(e + 0x3C);
    if (n >= NAME_LEN) n = NAME_LEN - 1;
    memcpy(out, e + 0x40, n);
    out[n] = 0;
    return TRUE;
}

static BOOL IsJunk(const char* name)
{
    return !strcmp(name, ".") || !strcmp(name, "..") ||
           !strncmp(name, "._", 2) || !_stricmp(name, ".DS_Store");
}

static BOOL WasSeen(DIRSTATE* d, const char* name)
{
    for (int i = 0; i < d->nseen; i++) if (!_stricmp(d->seen[i], name)) return TRUE;
    return FALSE;
}

// ---------------------------------------------------------------------------
// Known NAS files: files that the plugin saw in a NAS listing or opened on the
// NAS. Only for these files, a read/write open is refused at once (the share
// is read-only). For all other files, the NAS gives the real status.
// ---------------------------------------------------------------------------

#define MAX_KNOWN 64

static char g_Known[MAX_KNOWN][300];
static int  g_KnownNext;

static BOOL KnownHas(const char* path)
{
    BOOL found = FALSE;
    EnterCriticalSection(&g_DirLock);
    for (int i = 0; i < MAX_KNOWN; i++)
        if (g_Known[i][0] && !_stricmp(g_Known[i], path)) { found = TRUE; break; }
    LeaveCriticalSection(&g_DirLock);
    return found;
}

static void KnownAdd(const char* path)
{
    if (KnownHas(path)) return;
    EnterCriticalSection(&g_DirLock);
    strncpy(g_Known[g_KnownNext], path, sizeof(g_Known[0]) - 1);
    g_Known[g_KnownNext][sizeof(g_Known[0]) - 1] = 0;
    g_KnownNext = (g_KnownNext + 1) % MAX_KNOWN;     // Ring: the oldest entry goes
    LeaveCriticalSection(&g_DirLock);
}

static void KnownAddEntry(const char* folder, const char* name)
{
    char full[300];
    size_t n = strlen(folder);
    const char* sep = (n && folder[n - 1] == '\\') ? "" : "\\";
    if (_snprintf(full, sizeof(full) - 1, "%s%s%s", folder, sep, name) < 0) return;
    full[sizeof(full) - 1] = 0;
    KnownAdd(full);
}

// ---------------------------------------------------------------------------
// NAS package files: statistics and header cache
// ---------------------------------------------------------------------------
// Each NAS request costs one 16.6 ms tick while a title runs, whatever its
// size. XAM reads the package header in many small pieces and opens each
// package 3 times. The cache keeps the start of each package in memory
// (key = NAS path), so that XAM gets these bytes over the network only once.

#define MAX_FILES      32
#define MAX_CACHE      16
#define CACHE_STAGE1   0x1000      // First fill: enough for the header check
#define CACHE_MAX      0x10000     // Largest cache window
#define CACHE_IDLE_MS  20000       // Free an entry after 20 s without use

// Adaptive second fill: the highest end of the reads in this title (rounded up
// to 4 KB). The first package fills CACHE_MAX; the next packages fill only
// up to this value. 0 = not known yet.
static volatile DWORD g_FillHint;
#define ROUND4K(x) (((x) + 0xFFF) & ~0xFFF)

typedef struct _CACHEENT {
    char  path[300];    // NAS path of the package
    BYTE* data;         // CACHE_MAX bytes, allocated on the first fill
    DWORD have;         // Valid bytes, from offset 0
    BOOL  eof;          // The file ends inside the cache
    BOOL  busy;         // A fill or copy is in progress
    DWORD gen;          // 0 = free entry
    DWORD lastUse;
    BOOL  prefetched;   // A prefetch worker filled this entry
    BOOL  opened;       // XAM opened this package (FileAdd)
    DWORD pfTick;       // Tick of the prefetch fill
} CACHEENT;

typedef struct _NASFILE {
    HANDLE h;
    int    cache;       // Index in g_Cache, or -1
    DWORD  gen;         // Generation of that cache entry
    DWORD  reads, hits, netReads, maxEnd, mismatches;
    U64    readTicks;   // Time base ticks spent in network reads
    U64    openTick;
    char   name[12];
} NASFILE;

static CACHEENT      g_Cache[MAX_CACHE];
static DWORD         g_CacheGen;
static NASFILE       g_Files[MAX_FILES];
static volatile LONG g_FileCount;
static volatile LONG g_PfUsed;          // XAM opens of prefetched packages (this title)

// Find or make the cache entry for a NAS path. The caller holds g_DirLock.
// A prefetched entry that XAM did not open yet is replaced only if no other
// entry can go.
static int CacheSlot(const char* path)
{
    int freeSlot = -1, oldest = -1, oldestPf = -1;
    for (int i = 0; i < MAX_CACHE; i++) {
        CACHEENT* e = &g_Cache[i];
        if (e->gen && !_stricmp(e->path, path)) return i;
        if (!e->gen) { if (freeSlot < 0) freeSlot = i; continue; }
        if (e->busy) continue;
        if (e->prefetched && !e->opened) {
            if (oldestPf < 0 || e->lastUse < g_Cache[oldestPf].lastUse) oldestPf = i;
        } else if (oldest < 0 || e->lastUse < g_Cache[oldest].lastUse) {
            oldest = i;
        }
    }
    int i = freeSlot >= 0 ? freeSlot : oldest >= 0 ? oldest : oldestPf;
    if (i < 0) return -1;
    CACHEENT* e = &g_Cache[i];
    if (e->data) free(e->data);
    memset(e, 0, sizeof(CACHEENT));
    strncpy(e->path, path, sizeof(e->path) - 1);
    e->gen = ++g_CacheGen;
    e->lastUse = GetTickCount();
    return i;
}

// Free idle entries (or all entries). Entries in use stay.
static void CacheTrim(BOOL all)
{
    DWORD now = GetTickCount();
    int freed = 0;
    EnterCriticalSection(&g_DirLock);
    for (int i = 0; i < MAX_CACHE; i++) {
        CACHEENT* e = &g_Cache[i];
        if (!e->gen || e->busy) continue;
        if (!all && now - e->lastUse < CACHE_IDLE_MS) continue;
        if (e->data) free(e->data);
        memset(e, 0, sizeof(CACHEENT));
        freed++;
    }
    LeaveCriticalSection(&g_DirLock);
    if (freed) LogV("cache: %d entries freed", freed);
}

static void FileAdd(HANDLE h, const char* path)
{
    const char* n = strrchr(path, '\\');
    n = n ? n + 1 : path;
    EnterCriticalSection(&g_DirLock);
    for (int i = 0; i < MAX_FILES; i++) {
        NASFILE* f = &g_Files[i];
        if (f->h) continue;
        memset(f, 0, sizeof(NASFILE));
        f->h = h;
        f->openTick = __mftb();
        strncpy(f->name, n, 8);
        f->cache = CacheSlot(path);
        f->gen = f->cache >= 0 ? g_Cache[f->cache].gen : 0;
        if (f->cache >= 0) {
            CACHEENT* e = &g_Cache[f->cache];
            if (e->prefetched && !e->opened) g_PfUsed++;
            e->opened = TRUE;
        }
        g_FileCount++;
        break;
    }
    LeaveCriticalSection(&g_DirLock);
}

static NASFILE* FileFind(HANDLE h)
{
    if (!g_FileCount || !h) return NULL;
    NASFILE* f = NULL;
    EnterCriticalSection(&g_DirLock);
    for (int i = 0; i < MAX_FILES; i++) if (g_Files[i].h == h) { f = &g_Files[i]; break; }
    LeaveCriticalSection(&g_DirLock);
    return f;
}

static void FileClose(HANDLE h)
{
    if (!g_FileCount || !h) return;
    NASFILE copy;
    BOOL found = FALSE;
    EnterCriticalSection(&g_DirLock);
    for (int i = 0; i < MAX_FILES; i++) {
        if (g_Files[i].h == h) {
            copy = g_Files[i];
            g_Files[i].h = NULL;
            g_FileCount--;
            found = TRUE;
            break;
        }
    }
    LeaveCriticalSection(&g_DirLock);
    if (!found) return;
    LogV("close %s: %u reads (%u in cache), %u mismatches, %u network reads, %u ms network, "
         "max end %X, open for %u ms",
         copy.name, copy.reads, copy.hits, copy.mismatches, copy.netReads,
         (DWORD)(copy.readTicks / (TB_PER_US * 1000)), copy.maxEnd,
         (DWORD)((__mftb() - copy.openTick) / (TB_PER_US * 1000)));
}

// ---------------------------------------------------------------------------
// Original kernel functions (all parameters raw 64-bit)
// ---------------------------------------------------------------------------

typedef U64 (*PFN1)(U64);
typedef U64 (*PFN2)(U64, U64);
typedef U64 (*PFN6)(U64, U64, U64, U64, U64, U64);
typedef U64 (*PFN8)(U64, U64, U64, U64, U64, U64, U64, U64);
typedef U64 (*PFN9)(U64, U64, U64, U64, U64, U64, U64, U64, U64);
typedef U64 (*PFN10)(U64, U64, U64, U64, U64, U64, U64, U64, U64, U64);

static PFN9  g_pNtCreateFile;
static PFN6  g_pNtOpenFile;
static PFN10 g_pNtQueryDirectoryFile;
static PFN2  g_pNtQueryFullAttributesFile;
static PFN1  g_pNtClose;
static PFN8  g_pNtReadFile;

static volatile BOOL g_NasReadOnly;     // TRUE after the first write open was denied

// ---------------------------------------------------------------------------
// Parallel prefetch (Speed.Prefetch, off by default)
// ---------------------------------------------------------------------------
// When XAM opens a NAS DLC folder, a worker lists the same folder with its own
// handle. Then the workers open each package read-only and fill the first
// CACHE_STAGE1 bytes of its cache entry, before XAM opens the package. XAM
// then gets its header reads from memory.
//  - Workers are system threads. They use only the original kernel functions
//    and close their own handles.
//  - The hooks only add a folder to the queue. They never wait for a worker.
//    If a cache entry is busy, XAM reads from the NAS as before.
//  - A worker stays at most PF_LEAD packages in front of XAM, so that the
//    16 cache entries are not replaced before XAM uses them.
//  - At each title change, the queue is cancelled.

#define PF_MAX_THREADS  4
#define PF_MAX_FOLDERS  8
#define PF_QUEUE        128
#define PF_LEAD         8            // Prefetched packages that XAM did not open yet
#define PF_STALE_MS     3000         // After this time, such a package does not count
#define PF_FILE_ACCESS  0x00120089   // Read-only (the same access as the read-only retry of XAM)
#define PF_DIR_ACCESS   0x00100001   // SYNCHRONIZE | FILE_LIST_DIRECTORY
#define PF_SHARE_ALL    7            // Share read, write, delete: never lock out XAM
#define PF_FILE_OPTS    0x60         // FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE
#define PF_DIR_OPTS     0x21         // FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT
#define ST_SHARING_VIOLATION ((NTSTATUS)0xC0000043)

typedef struct _PFJOB {
    DWORD gen;                 // g_PfGen when the job was queued
    int   folder;              // Index in g_PfFolders
    char  name[NAME_LEN];      // Package name. Empty = list the folder
} PFJOB;

static CRITICAL_SECTION g_PfLock;
static DWORD   g_PfGen;                           // Changes at each title change
static char    g_PfFolders[PF_MAX_FOLDERS][300];  // NAS folders of this title
static int     g_PfFolderCount;
static PFJOB   g_PfQueue[PF_QUEUE];
static int     g_PfHead, g_PfTail;                // Jobs in the queue: tail - head
static char    g_PfActive[PF_MAX_THREADS][300];   // Package that each worker has open

// Statistics of one batch (from the first job to an empty queue). Under g_PfLock.
static DWORD   g_PfStart;
static int     g_PfQueued, g_PfDone, g_PfFilled, g_PfSkipped, g_PfFailed;
static U64     g_PfOpenTicks, g_PfReadTicks;
static volatile LONG g_PfInFlight, g_PfMaxInFlight;   // Parallel network requests

static void PfNetBegin()
{
    LONG n = InterlockedIncrement(&g_PfInFlight);
    LONG m;
    while (n > (m = g_PfMaxInFlight))
        if (InterlockedCompareExchange(&g_PfMaxInFlight, n, m) == m) break;
}

static void PfNetEnd() { InterlockedDecrement(&g_PfInFlight); }

// The caller holds g_PfLock.
static BOOL PfPushLocked(const PFJOB* j)
{
    if (g_PfTail - g_PfHead >= PF_QUEUE) return FALSE;
    g_PfQueue[g_PfTail % PF_QUEUE] = *j;
    g_PfTail++;
    if (g_PfQueued == g_PfDone) {                     // New batch
        g_PfStart = GetTickCount();
        g_PfFilled = g_PfSkipped = g_PfFailed = 0;
        g_PfOpenTicks = g_PfReadTicks = 0;
        g_PfMaxInFlight = 0;
    }
    g_PfQueued++;
    return TRUE;
}

// Get the next job and a copy of its folder path. FALSE = queue empty.
static BOOL PfTake(PFJOB* j, char* folder)
{
    BOOL ok = FALSE;
    EnterCriticalSection(&g_PfLock);
    while (g_PfHead < g_PfTail) {
        *j = g_PfQueue[g_PfHead % PF_QUEUE];
        g_PfHead++;
        if (j->gen != g_PfGen || j->folder >= g_PfFolderCount) { g_PfDone++; continue; }
        strcpy(folder, g_PfFolders[j->folder]);
        ok = TRUE;
        break;
    }
    LeaveCriticalSection(&g_PfLock);
    return ok;
}

static BOOL PfCancelled(DWORD gen) { return gen != g_PfGen; }

// result: 1 = filled, 0 = skipped (cancelled or already in the cache), -1 = failed,
//         2 = folder listed (not counted)
static void PfJobDone(DWORD gen, int result, U64 openTicks, U64 readTicks)
{
    BOOL last = FALSE;
    int  filled = 0, skipped = 0, failed = 0, maxPar = 0;
    DWORD ms = 0, openUs = 0, readUs = 0;
    EnterCriticalSection(&g_PfLock);
    g_PfDone++;
    if (gen == g_PfGen) {
        if (result == 1)      g_PfFilled++;
        else if (result == 0) g_PfSkipped++;
        else if (result < 0)  g_PfFailed++;
        g_PfOpenTicks += openTicks;
        g_PfReadTicks += readTicks;
    }
    if (g_PfDone == g_PfQueued && g_PfStart) {
        last    = TRUE;
        filled  = g_PfFilled; skipped = g_PfSkipped; failed = g_PfFailed;
        ms      = GetTickCount() - g_PfStart;
        if (g_PfFilled) {
            openUs = (DWORD)(g_PfOpenTicks / g_PfFilled / TB_PER_US);
            readUs = (DWORD)(g_PfReadTicks / g_PfFilled / TB_PER_US);
        }
        maxPar  = (int)g_PfMaxInFlight;
        g_PfStart = 0;
    }
    LeaveCriticalSection(&g_PfLock);
    if (last)
        Log("prefetch done: %d filled, %d skipped, %d failed in %u ms, "
            "average open %u us, average read %u us, max %d parallel requests",
            filled, skipped, failed, ms, openUs, readUs, maxPar);
}

// Hook side: queue a NAS DLC folder (once per title).
static void PrefetchFolder(const char* path)
{
    if (!g_Cfg.prefetch) return;
    BOOL added = FALSE, full = FALSE;
    EnterCriticalSection(&g_PfLock);
    int i;
    for (i = 0; i < g_PfFolderCount; i++) if (!_stricmp(g_PfFolders[i], path)) break;
    if (i == g_PfFolderCount) {
        if (i < PF_MAX_FOLDERS) {
            strncpy(g_PfFolders[i], path, sizeof(g_PfFolders[i]) - 1);
            g_PfFolders[i][sizeof(g_PfFolders[i]) - 1] = 0;
            g_PfFolderCount++;
            PFJOB j;
            j.gen = g_PfGen; j.folder = i; j.name[0] = 0;
            added = PfPushLocked(&j);
            full  = !added;
        } else {
            full = TRUE;
        }
    }
    LeaveCriticalSection(&g_PfLock);
    if (added) LogV("prefetch: folder queued %s", path);
    if (full)  Log("prefetch: folder table or queue full, %s not prefetched", path);
}

// Hook side: if a worker has this package open, wait until it closes it
// (max. 500 ms). TRUE = a worker had it open.
static BOOL PrefetchWaitPath(const char* path)
{
    if (!g_Cfg.prefetch) return FALSE;
    BOOL was = FALSE;
    for (int n = 0; n < 100; n++) {
        BOOL active = FALSE;
        EnterCriticalSection(&g_PfLock);
        for (int i = 0; i < PF_MAX_THREADS; i++)
            if (g_PfActive[i][0] && !_stricmp(g_PfActive[i], path)) { active = TRUE; break; }
        LeaveCriticalSection(&g_PfLock);
        if (!active) break;
        was = TRUE;
        Sleep(5);
    }
    return was;
}

static void PfSetActive(int id, const char* path)
{
    EnterCriticalSection(&g_PfLock);
    if (path) strcpy(g_PfActive[id], path); else g_PfActive[id][0] = 0;
    LeaveCriticalSection(&g_PfLock);
}

static U64 PfOpen(HANDLE* ph, const char* path, DWORD access, DWORD opts)
{
    STRING            name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK   io;
    RtlInitAnsiString(&name, path);
    oa.RootDirectory = NULL;
    oa.ObjectName    = &name;
    oa.Attributes    = 0x40;                            // OBJ_CASE_INSENSITIVE
    *ph = NULL;
    return g_pNtOpenFile(RAW(ph), access, RAW(&oa), RAW(&io), PF_SHARE_ALL, opts);
}

// Worker: list a NAS folder and queue each package.
static void PfListFolder(int id, const PFJOB* job, const char* folder)
{
    DWORD  t0 = GetTickCount();
    HANDLE h;
    U64 r = PfOpen(&h, folder, PF_DIR_ACCESS, PF_DIR_OPTS);
    if (!NT_SUCCESS(STATUS(r)) || !h) {
        Log("prefetch: folder open %s = %08X", folder, (DWORD)r);
        PfJobDone(job->gen, -1, 0, 0);
        return;
    }

    DWORD  buf[0x200];                                  // 2 KB, DWORD aligned
    STRING mask;
    RtlInitAnsiString(&mask, "*");
    int files = 0, dropped = 0, requests = 0;
    for (int guard = 0; guard < 512 && !PfCancelled(job->gen); guard++) {
        IO_STATUS_BLOCK io;
        io.Information = 0;
        PfNetBegin();
        r = g_pNtQueryDirectoryFile(RAW(h), 0, 0, 0, RAW(&io), RAW(buf), sizeof(buf),
                                    guard ? 0 : RAW(&mask), 0, 0);
        if (STATUS(r) == STATUS_PENDING) {
            NtWaitForSingleObjectEx(h, 0, FALSE, NULL);
            r = (U64)(DWORD)IOSB_STATUS(&io);
        }
        PfNetEnd();
        requests++;
        if (!NT_SUCCESS(STATUS(r))) break;              // Includes "no more files"

        // One or more FILE_DIRECTORY_INFORMATION entries:
        // NextEntryOffset at 0x00, FileAttributes at 0x38, FileNameLength at 0x3C, FileName at 0x40.
        DWORD used = (DWORD)io.Information;
        if (used > sizeof(buf)) used = sizeof(buf);
        DWORD pos = 0;
        while (pos + 0x40 <= used) {
            BYTE* e    = (BYTE*)buf + pos;
            DWORD next = *(DWORD*)(e + 0x00);
            DWORD attr = *(DWORD*)(e + 0x38);
            DWORD n    = *(DWORD*)(e + 0x3C);
            if (n < NAME_LEN && pos + 0x40 + n <= used && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                PFJOB fj;
                memcpy(fj.name, e + 0x40, n);
                fj.name[n] = 0;
                if (fj.name[0] && !IsJunk(fj.name)) {
                    fj.gen = job->gen; fj.folder = job->folder;
                    EnterCriticalSection(&g_PfLock);
                    BOOL ok = PfPushLocked(&fj);
                    LeaveCriticalSection(&g_PfLock);
                    if (ok) files++; else dropped++;
                }
            }
            if (!next) break;
            pos += next;
        }
    }
    g_pNtClose(RAW(h));
    Log("prefetch: worker %d listed %d packages in %u ms (%d requests, last %08X)%s, %s",
        id, files, GetTickCount() - t0, requests, (DWORD)r,
        dropped ? ", QUEUE FULL" : "", folder);
    PfJobDone(job->gen, 2, 0, 0);
}

// Worker: open one package and fill the start of its cache entry.
static void PfFillFile(int id, const PFJOB* job, const char* folder)
{
    char path[300];
    size_t fl = strlen(folder);
    const char* sep = (fl && folder[fl - 1] == '\\') ? "" : "\\";
    if (_snprintf(path, sizeof(path) - 1, "%s%s%s", folder, sep, job->name) < 0) {
        PfJobDone(job->gen, -1, 0, 0);
        return;
    }
    path[sizeof(path) - 1] = 0;

    // Wait while XAM is PF_LEAD packages behind. Stop if the cache has the data.
    for (;;) {
        if (PfCancelled(job->gen)) { PfJobDone(job->gen, 0, 0, 0); return; }
        BOOL  have    = FALSE;
        int   pending = 0;
        DWORD now     = GetTickCount();
        EnterCriticalSection(&g_DirLock);
        for (int i = 0; i < MAX_CACHE; i++) {
            CACHEENT* e = &g_Cache[i];
            if (!e->gen) continue;
            if (!_stricmp(e->path, path) && (e->have >= CACHE_STAGE1 || e->eof)) have = TRUE;
            if (e->prefetched && !e->opened && now - e->pfTick < PF_STALE_MS) pending++;
        }
        LeaveCriticalSection(&g_DirLock);
        if (have) { PfJobDone(job->gen, 0, 0, 0); return; }
        if (pending < PF_LEAD) break;
        Sleep(5);
    }

    U64 t0 = __mftb();
    PfSetActive(id, path);
    HANDLE h;
    PfNetBegin();
    U64 r = PfOpen(&h, path, PF_FILE_ACCESS, PF_FILE_OPTS);
    PfNetEnd();
    U64 openTicks = __mftb() - t0;
    if (!NT_SUCCESS(STATUS(r)) || !h) {
        PfSetActive(id, NULL);
        Log("prefetch: worker %d open %s = %08X", id, job->name, (DWORD)r);
        PfJobDone(job->gen, -1, openTicks, 0);
        return;
    }

    // Claim the cache entry. If it is busy (XAM fills it now) or full, stop.
    CACHEENT* e = NULL;
    EnterCriticalSection(&g_DirLock);
    int slot = PfCancelled(job->gen) ? -1 : CacheSlot(path);
    if (slot >= 0) {
        CACHEENT* c = &g_Cache[slot];
        if (!c->busy && c->have < CACHE_STAGE1 && !c->eof) { c->busy = TRUE; e = c; }
    }
    LeaveCriticalSection(&g_DirLock);

    int   result    = 0;
    U64   readTicks = 0;
    DWORD got       = 0;
    if (e) {
        if (!e->data) e->data = (BYTE*)malloc(CACHE_MAX);
        if (e->data) {
            IO_STATUS_BLOCK io;
            LARGE_INTEGER   o;
            o.QuadPart = e->have;
            io.Information = 0;
            U64 t1 = __mftb();
            PfNetBegin();
            r = g_pNtReadFile(RAW(h), 0, 0, 0, RAW(&io),
                              RAW(e->data + e->have), CACHE_STAGE1 - e->have, RAW(&o));
            if (STATUS(r) == STATUS_PENDING) {
                NtWaitForSingleObjectEx(h, 0, FALSE, NULL);
                r = (U64)(DWORD)IOSB_STATUS(&io);
            }
            PfNetEnd();
            readTicks = __mftb() - t1;
            if (NT_SUCCESS(STATUS(r))) {
                got = (DWORD)io.Information;
                e->have += got;
                if (e->have < CACHE_STAGE1) e->eof = TRUE;
                result = 1;
            } else if (STATUS(r) == STATUS_END_OF_FILE) {
                e->eof = TRUE;
                result = 1;
            } else {
                result = -1;
            }
        } else {
            result = -1;
        }
        EnterCriticalSection(&g_DirLock);
        if (result > 0) { e->prefetched = TRUE; e->pfTick = GetTickCount(); }
        e->lastUse = GetTickCount();
        e->busy = FALSE;
        LeaveCriticalSection(&g_DirLock);
    }
    g_pNtClose(RAW(h));
    PfSetActive(id, NULL);

    LogV("prefetch: worker %d %s: open %u us, read = %08X, %X bytes, %u us%s",
         id, job->name, (DWORD)(openTicks / TB_PER_US), (DWORD)r, got,
         (DWORD)(readTicks / TB_PER_US), e ? "" : " (cache entry not free)");
    PfJobDone(job->gen, result, openTicks, readTicks);
}

static DWORD WINAPI PrefetchThread(LPVOID param)
{
    int   id = (int)(DWORD)param;
    PFJOB job;
    char  folder[300];
    for (;;) {
        if (!PfTake(&job, folder)) { Sleep(10); continue; }
        if (!job.name[0]) PfListFolder(id, &job, folder);
        else              PfFillFile(id, &job, folder);
    }
    return 0;
}

// Watch thread: cancel all jobs at a title change.
static void PrefetchReset()
{
    if (!g_Cfg.prefetch) return;
    EnterCriticalSection(&g_PfLock);
    g_PfGen++;
    g_PfDone += g_PfTail - g_PfHead;                    // Dropped jobs count as done
    g_PfHead = g_PfTail = 0;
    g_PfFolderCount = 0;
    LeaveCriticalSection(&g_PfLock);
    g_PfUsed = 0;
}

static void PrefetchStart()
{
    if (!g_Cfg.prefetch) return;
    int started = 0;
    for (int i = 0; i < g_Cfg.pfThreads && i < PF_MAX_THREADS; i++) {
        HANDLE thread = NULL;
        DWORD  threadId = 0;
        ExCreateThread(&thread, 0, &threadId, (PVOID)XapiThreadStartup,
                       (LPTHREAD_START_ROUTINE)PrefetchThread, (LPVOID)(DWORD)i,
                       0x2 | CREATE_SUSPENDED);         // 0x2 = system thread
        if (!thread) continue;
        XSetThreadProcessor(thread, (i & 1) ? 4 : 5);
        ResumeThread(thread);
        CloseHandle(thread);
        started++;
    }
    Log("prefetch: %d of %d worker threads started", started, g_Cfg.pfThreads);
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

// NtOpenFile(ph, access, oa, iosb, share, options)
static U64 Hook_NtOpenFile(U64 ph, U64 access, U64 oa, U64 iosb, U64 share, U64 opts)
{
    U64 r = g_pNtOpenFile(ph, access, oa, iosb, share, opts);
    TraceTu("open", oa, access, r);
    TraceContent("open", oa, r);

    NASPATH np;
    if (!BuildNas((POBJECT_ATTRIBUTES)PTR(oa), &np)) return r;
    BOOL isDir = ((DWORD)opts & FILE_DIRECTORY_FILE) != 0;
    NTSTATUS st = STATUS(r);
    LogV("HDD %s %s = %08X", isDir ? "folder" : "open",
         np.buf + strlen(g_Cfg.nasContent), (DWORD)st);

    if (NT_SUCCESS(st)) {
        if (!isDir) return r;                           // File is on the HDD
        // HDD folder exists: open the NAS folder too, for the merge.
        HANDLE hNas = NULL;
        IO_STATUS_BLOCK io;
        U64 r2 = g_pNtOpenFile(RAW(&hNas), access, RAW(&np.oa), RAW(&io), share, opts);
        if (NT_SUCCESS(STATUS(r2)) && hNas) {
            DirAdd(*(HANDLE*)PTR(ph), hNas, 0, np.type, np.buf);
            LogV("merge folder %s", np.buf);
            if (np.type == TYPE_DLC) PrefetchFolder(np.buf);
        } else {
            LogV("merge folder %s = %08X (no NAS folder)", np.buf, (DWORD)r2);
        }
        return r;
    }

    if (!IsNotFound(st)) return r;

    // Not on the HDD: try the NAS.
    // The NAS share is read-only. After the first denied write open, a write
    // open of a known NAS file fails at once, without a network round trip.
    // Unknown files go to the NAS, so that XAM gets the real status.
    BOOL wantsWrite = ((DWORD)access & WRITE_ACCESS_MASK) != 0;
    if (!isDir && wantsWrite && g_NasReadOnly && KnownHas(np.buf))
        return (U64)(DWORD)ST_ACCESS_DENIED;             // XAM then retries read-only

    U64 t0 = __mftb();
    U64 r2 = g_pNtOpenFile(ph, access, RAW(&np.oa), iosb, share, opts);
    if (STATUS(r2) == ST_SHARING_VIOLATION && !isDir && PrefetchWaitPath(np.buf)) {
        // A prefetch worker had the package open. It is closed now: try again.
        r2 = g_pNtOpenFile(ph, access, RAW(&np.oa), iosb, share, opts);
        Log("prefetch: sharing conflict on %s, second open = %08X", np.buf, (DWORD)r2);
    }
    DWORD us = (DWORD)((__mftb() - t0) / TB_PER_US);
    LogV("NAS %s %s access %08X = %08X (%u us)",
         isDir ? "folder" : "open", np.buf, (DWORD)access, (DWORD)r2, us);

    if (!NT_SUCCESS(STATUS(r2))) {
        // Access denied: give this status to XAM, so that XAM retries read-only.
        // Other errors: keep the HDD status.
        if (STATUS(r2) == ST_ACCESS_DENIED) {
            if (wantsWrite) g_NasReadOnly = TRUE;
            return r2;
        }
        return r;
    }
    HANDLE h = *(HANDLE*)PTR(ph);
    if (isDir) {
        DirAdd(h, NULL, 1, np.type, np.buf);
        if (np.type == TYPE_DLC) PrefetchFolder(np.buf);
    } else {
        KnownAdd(np.buf);
        FileAdd(h, np.buf);
        if (np.type == TYPE_TU) {
            Log("title update opened on the NAS: %s", np.buf);
            ReportTu(np.buf);
        }
    }
    return r2;
}

// NtCreateFile(ph, access, oa, iosb, alloc, attributes, share, disposition, options)
static U64 Hook_NtCreateFile(U64 ph, U64 access, U64 oa, U64 iosb, U64 alloc,
                             U64 attr, U64 share, U64 disp, U64 opts)
{
    U64 r = g_pNtCreateFile(ph, access, oa, iosb, alloc, attr, share, disp, opts);
    TraceTu("create", oa, access, r);
    TraceContent("create", oa, r);
    if (!IsNotFound(STATUS(r))) return r;
    // Redirect only the open of an existing file (FILE_OPEN = 1).
    // The creation of new files and folders always stays on the HDD.
    if ((DWORD)disp != 1) return r;

    NASPATH np;
    if (!BuildNas((POBJECT_ATTRIBUTES)PTR(oa), &np)) return r;

    U64 r2 = g_pNtCreateFile(ph, access, RAW(&np.oa), iosb, alloc, attr, share, disp, opts);
    if (STATUS(r2) == ST_SHARING_VIOLATION && !((DWORD)opts & FILE_DIRECTORY_FILE) &&
        PrefetchWaitPath(np.buf)) {
        r2 = g_pNtCreateFile(ph, access, RAW(&np.oa), iosb, alloc, attr, share, disp, opts);
        Log("prefetch: sharing conflict on %s, second create = %08X", np.buf, (DWORD)r2);
    }
    LogV("NAS create %s access %08X = %08X", np.buf, (DWORD)access, (DWORD)r2);
    if (!NT_SUCCESS(STATUS(r2)))
        return STATUS(r2) == ST_ACCESS_DENIED ? r2 : r;
    if ((DWORD)opts & FILE_DIRECTORY_FILE) {
        DirAdd(*(HANDLE*)PTR(ph), NULL, 1, np.type, np.buf);
        if (np.type == TYPE_DLC) PrefetchFolder(np.buf);
    } else {
        KnownAdd(np.buf);
        FileAdd(*(HANDLE*)PTR(ph), np.buf);
        if (np.type == TYPE_TU) {
            Log("title update opened on the NAS: %s", np.buf);
            ReportTu(np.buf);
        }
    }
    return r2;
}

// NtQueryFullAttributesFile(oa, info)
static U64 Hook_NtQueryFullAttributesFile(U64 oa, U64 info)
{
    U64 r = g_pNtQueryFullAttributesFile(oa, info);
    TraceTu("attributes", oa, 0, r);
    TraceContent("attributes", oa, r);
    if (!IsNotFound(STATUS(r))) return r;

    NASPATH np;
    if (!BuildNas((POBJECT_ATTRIBUTES)PTR(oa), &np)) return r;

    U64 r2 = g_pNtQueryFullAttributesFile(RAW(&np.oa), info);
    LogV("NAS attributes %s = %08X", np.buf, (DWORD)r2);
    return NT_SUCCESS(STATUS(r2)) ? r2 : r;
}

// NtQueryDirectoryFile: 10 raw values. Value 8 = file name mask (NULL on continuation).
static U64 Hook_NtQueryDirectoryFile(U64 h, U64 ev, U64 apc, U64 ctx, U64 iosb,
                                     U64 info, U64 len, U64 mask, U64 a9, U64 a10)
{
    DIRSTATE* d = DirFind((HANDLE)PTR(h));
    if (!d || (DWORD)ev || (DWORD)apc)                  // Not ours, or asynchronous
        return g_pNtQueryDirectoryFile(h, ev, apc, ctx, iosb, info, len, mask, a9, a10);

    // A mask means a new scan: start again (merge: with the HDD entries).
    if ((DWORD)mask) {
        if (d->nas) { d->phase = 0; d->nseen = 0; }
        d->nasCount = 0;
    }

    for (int guard = 0; guard < 512; guard++) {
        char name[NAME_LEN];

        if (d->phase == 0) {
            U64 r = g_pNtQueryDirectoryFile(h, ev, apc, ctx, iosb, info, len, mask, a9, a10);
            NTSTATUS st = STATUS(r);
            if (st == ST_NO_MORE_FILES && d->nas) {
                d->phase = 1;                            // Continue with the NAS folder
                mask = 0;
                continue;
            }
            if (NT_SUCCESS(st) && SingleEntryName(PTR(info), name) && d->nseen < MAX_SEEN)
                strcpy(d->seen[d->nseen++], name);
            return r;
        }

        // Phase 1: NAS entries (from the extra handle, or from h itself).
        U64 hq = d->nas ? RAW(d->nas) : h;
        U64 r = g_pNtQueryDirectoryFile(hq, ev, apc, ctx, iosb, info, len, mask, a9, a10);
        mask = 0;
        if (STATUS(r) == ST_NO_MORE_FILES && d->nasCount > 0) {
            Log("NAS listing done: %d %s entries in %u ms, %d from the prefetch", d->nasCount,
                d->type == TYPE_DLC ? "DLC" : "title update",
                GetTickCount() - d->nasStart, (int)g_PfUsed);
            if (d->type == TYPE_DLC) ReportDlc(d->nasCount);
            d->nasCount = 0;                             // Report each scan once
        }
        if (!NT_SUCCESS(STATUS(r))) return r;           // Includes "no more files"
        if (!SingleEntryName(PTR(info), name)) return r;
        if (IsJunk(name) || WasSeen(d, name)) continue;  // Skip, get the next entry
        if (d->nasCount == 0) d->nasStart = GetTickCount();
        d->nasCount++;
        KnownAddEntry(d->nasPath, name);
        LogV("NAS entry %s", name);
        return r;
    }
    return (U64)(DWORD)ST_NO_MORE_FILES;
}

// Serve a read from the cache. Fills the cache from the NAS when necessary.
// TRUE = the read is done (dst and *got are set).
static BOOL CacheRead(NASFILE* f, U64 start, DWORD n, BYTE* dst, DWORD* got)
{
    if (!g_Cfg.cache) return FALSE;
    if (n == 0 || start >= CACHE_MAX || start + n > CACHE_MAX) return FALSE;

    CACHEENT* e = NULL;
    EnterCriticalSection(&g_DirLock);
    if (f->cache >= 0 && g_Cache[f->cache].gen == f->gen && !g_Cache[f->cache].busy) {
        e = &g_Cache[f->cache];
        e->busy = TRUE;
    }
    LeaveCriticalSection(&g_DirLock);
    if (!e) return FALSE;

    DWORD end = (DWORD)start + n;
    if (end > e->have && !e->eof) {
        if (!e->data) e->data = (BYTE*)malloc(CACHE_MAX);
        if (e->data) {
            DWORD want;
            if (end <= CACHE_STAGE1) {
                want = CACHE_STAGE1;
            } else {
                DWORD hint = g_FillHint;
                want = hint > CACHE_STAGE1 ? hint : CACHE_MAX;
                if (want < ROUND4K(end)) want = ROUND4K(end);
                if (want > CACHE_MAX)    want = CACHE_MAX;
            }
            IO_STATUS_BLOCK io;
            LARGE_INTEGER   o;
            o.QuadPart = e->have;
            io.Information = 0;
            U64 t0 = __mftb();
            U64 r  = g_pNtReadFile(RAW(f->h), 0, 0, 0, RAW(&io),
                                   RAW(e->data + e->have), want - e->have, RAW(&o));
            if (STATUS(r) == STATUS_PENDING) {
                NtWaitForSingleObjectEx(f->h, 0, FALSE, NULL);
                r = (U64)(DWORD)IOSB_STATUS(&io);
            }
            U64 dt = __mftb() - t0;
            f->netReads++;
            f->readTicks += dt;
            LogV("cache %s fill %X-%X = %08X, %u bytes (%u us)", f->name, e->have, want,
                 (DWORD)r, NT_SUCCESS(STATUS(r)) ? (DWORD)io.Information : 0,
                 (DWORD)(dt / TB_PER_US));
            if (NT_SUCCESS(STATUS(r))) {
                e->have += (DWORD)io.Information;
                if (e->have < want) e->eof = TRUE;
            } else if (STATUS(r) == STATUS_END_OF_FILE) {
                e->eof = TRUE;
            }
        }
    }

    BOOL ok = FALSE;
    if (e->data) {
        if (end <= e->have) {
            memcpy(dst, e->data + start, n);
            *got = n;
            ok = TRUE;
        } else if (e->eof && start < e->have) {          // Short read at end of file
            *got = e->have - (DWORD)start;
            memcpy(dst, e->data + start, *got);
            ok = TRUE;
        }
    }
    e->lastUse = GetTickCount();
    EnterCriticalSection(&g_DirLock);
    e->busy = FALSE;
    LeaveCriticalSection(&g_DirLock);
    return ok;
}

// Raise the fill hint after a read, so that the next packages fill enough.
static void UpdateFillHint(U64 start, DWORD n)
{
    if (start + n <= CACHE_MAX && ROUND4K((DWORD)(start + n)) > g_FillHint)
        g_FillHint = ROUND4K((DWORD)(start + n));
}

// NtReadFile(h, event, apc, apcContext, iosb, buffer, length, byteOffset)
static U64 Hook_NtReadFile(U64 h, U64 ev, U64 apc, U64 ctx, U64 iosb,
                           U64 buf, U64 len, U64 off)
{
    NASFILE* f = FileFind((HANDLE)PTR(h));
    if (!f) return g_pNtReadFile(h, ev, apc, ctx, iosb, buf, len, off);

    LARGE_INTEGER* po = (LARGE_INTEGER*)PTR(off);
    if (!po || !MmIsAddressValid(po)) {
        // Read from the current file position. XAM moves this position itself
        // (seek), so only the kernel knows it: always read from the NAS.
        U64 t0 = __mftb();
        U64 r  = g_pNtReadFile(h, ev, apc, ctx, iosb, buf, len, off);
        f->readTicks += __mftb() - t0;
        f->reads++;
        f->netReads++;
        return r;
    }

    U64   start = (U64)po->QuadPart;
    DWORD n     = (DWORD)len;
    BOOL  sync  = !(DWORD)ev && !(DWORD)apc;
    IO_STATUS_BLOCK* io = (IO_STATUS_BLOCK*)PTR(iosb);
    BOOL  ioOk  = io && MmIsAddressValid(io);
    BOOL  bufOk = (DWORD)buf && MmIsAddressValid(PTR(buf));

    f->reads++;
    if (start + n > f->maxEnd) f->maxEnd = (DWORD)(start + n);

    if (g_Cfg.cacheVerify && sync && ioOk && bufOk) {
        // Diagnostic mode: the caller gets the real NAS data.
        // The cache data is only compared with it.
        BYTE* tmp = (BYTE*)malloc(n ? n : 1);
        DWORD got = 0;
        BOOL  hit = tmp && CacheRead(f, start, n, tmp, &got);

        U64 t0 = __mftb();
        U64 r  = g_pNtReadFile(h, ev, apc, ctx, iosb, buf, len, off);
        f->readTicks += __mftb() - t0;
        f->netReads++;
        DWORD info = NT_SUCCESS(STATUS(r)) ? (DWORD)io->Information : 0;

        if (hit) {
            f->hits++;
            BYTE* real = (BYTE*)PTR(buf);
            DWORD cmp  = info < got ? info : got;
            DWORD diff = 0xFFFFFFFF;
            for (DWORD i = 0; i < cmp; i++) if (real[i] != tmp[i]) { diff = i; break; }
            if (!NT_SUCCESS(STATUS(r)) || info != got || diff != 0xFFFFFFFF) {
                f->mismatches++;
                Log("MISMATCH %s off %X len %X: NAS %08X %X bytes, cache %X bytes, first difference at +%X",
                    f->name, (DWORD)start, n, (DWORD)r, info, got, diff);
            }
        }
        if (tmp) free(tmp);
        UpdateFillHint(start, n);
        return r;
    }

    if (sync && ioOk && bufOk) {
        DWORD got = 0;
        if (CacheRead(f, start, n, (BYTE*)PTR(buf), &got)) {
            IOSB_STATUS(io) = STATUS_SUCCESS;
            io->Information = got;
            f->hits++;
            UpdateFillHint(start, n);
            return (U64)(DWORD)STATUS_SUCCESS;
        }
    }

    U64 t0 = __mftb();
    U64 r  = g_pNtReadFile(h, ev, apc, ctx, iosb, buf, len, off);
    f->readTicks += __mftb() - t0;
    f->netReads++;
    UpdateFillHint(start, n);
    return r;
}

// NtClose(h)
static U64 Hook_NtClose(U64 h)
{
    HANDLE nas = DirRemove((HANDLE)PTR(h));
    if (nas) g_pNtClose(RAW(nas));
    FileClose((HANDLE)PTR(h));
    return g_pNtClose(h);
}

// ---------------------------------------------------------------------------
// Hook table (kernel functions imported by XAM)
// ---------------------------------------------------------------------------

typedef struct _HOOK {
    DWORD       Ordinal;
    const char* Name;
    PVOID       Hook;
    PVOID*      Original;
} HOOK;

static HOOK g_Hooks[] = {
    { kernelExp_NtCreateFile,              "NtCreateFile",              (PVOID)Hook_NtCreateFile,              (PVOID*)&g_pNtCreateFile },
    { kernelExp_NtOpenFile,                "NtOpenFile",                (PVOID)Hook_NtOpenFile,                (PVOID*)&g_pNtOpenFile },
    { kernelExp_NtQueryDirectoryFile,      "NtQueryDirectoryFile",      (PVOID)Hook_NtQueryDirectoryFile,      (PVOID*)&g_pNtQueryDirectoryFile },
    { kernelExp_NtQueryFullAttributesFile, "NtQueryFullAttributesFile", (PVOID)Hook_NtQueryFullAttributesFile, (PVOID*)&g_pNtQueryFullAttributesFile },
    { kernelExp_NtClose,                   "NtClose",                   (PVOID)Hook_NtClose,                   (PVOID*)&g_pNtClose },
    { kernelExp_NtReadFile,                "NtReadFile",                (PVOID)Hook_NtReadFile,                (PVOID*)&g_pNtReadFile },
};
#define HOOK_COUNT (sizeof(g_Hooks) / sizeof(g_Hooks[0]))

static DWORD  g_KrnlAddr[MAX_ORDINAL];
static HANDLE g_XamHandle;

static BOOL CacheExports()
{
    HANDLE krnl = NULL;
    if (XexGetModuleHandle("xboxkrnl.exe", &krnl) != 0 || !krnl) return FALSE;
    if (XexGetModuleHandle("xam.xex", &g_XamHandle) != 0 || !g_XamHandle) return FALSE;
    for (DWORD ord = 1; ord < MAX_ORDINAL; ord++) {
        PVOID addr = NULL;
        if (XexGetProcedureAddress(krnl, ord, &addr) == 0) g_KrnlAddr[ord] = (DWORD)addr;
    }
    for (DWORD i = 0; i < HOOK_COUNT; i++) {
        *g_Hooks[i].Original = (PVOID)g_KrnlAddr[g_Hooks[i].Ordinal];
        if (!*g_Hooks[i].Original) { Log("Cannot resolve %s", g_Hooks[i].Name); return FALSE; }
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// Safe import patch (from v6d)
// ---------------------------------------------------------------------------

typedef struct _NDT_IMPORT_DESC { DWORD Size; DWORD NameTableSize; DWORD ModuleCount; } NDT_IMPORT_DESC;
typedef struct _NDT_IMPORT_TABLE {
    DWORD TableSize; BYTE NextImportDigest[20]; DWORD ModuleNumber; DWORD Version[2];
    BYTE Unused; BYTE ModuleIndex; WORD ImportCount;
} NDT_IMPORT_TABLE;

#pragma section(".text")
__declspec(allocate(".text")) static const DWORD g_FlushInsnCode[] = {
    0x7C00186C,   // dcbst 0,r3
    0x7C0004AC,   // sync
    0x7C001FAC,   // icbi  0,r3
    0x7C0004AC,   // sync
    0x4C00012C,   // isync
    0x4E800020,   // blr
};
typedef void (*PFN_FlushInsn)(const void* p);
#define FlushInsn(p) ((PFN_FlushInsn)(void*)g_FlushInsnCode)(p)

static void WriteInsn(DWORD* p, DWORD v) { *p = v; FlushInsn(p); }

static void WriteStub(DWORD* stub, DWORD dest)
{
    DWORD lis = 0x3D600000 | ((dest >> 16) & 0xFFFF);   // lis   r11, hi
    DWORD ori = 0x616B0000 | (dest & 0xFFFF);           // ori   r11, r11, lo
    if (stub[0] == lis && stub[1] == ori) return;
    WriteInsn(&stub[0], 0x48000000);                    // b .   (callers wait)
    Sleep(2);
    WriteInsn(&stub[1], ori);
    WriteInsn(&stub[2], 0x7D6903A6);                    // mtctr r11
    WriteInsn(&stub[3], 0x4E800420);                    // bctr
    WriteInsn(&stub[0], lis);
}

static HOOK* FindHook(DWORD addr)
{
    for (DWORD i = 0; i < HOOK_COUNT; i++)
        if (addr && (DWORD)*g_Hooks[i].Original == addr) return &g_Hooks[i];
    return NULL;
}

static int PatchXamImports()
{
    PLDR_DATA_TABLE_ENTRY ldr = (PLDR_DATA_TABLE_ENTRY)g_XamHandle;
    NDT_IMPORT_DESC* desc =
        (NDT_IMPORT_DESC*)RtlImageXexHeaderField(ldr->XexHeaderBase, 0x000103FF);
    if (!desc) return 0;

    BYTE* p = (BYTE*)(desc + 1) + desc->NameTableSize;
    int hooked = 0;
    for (DWORD m = 0; m < desc->ModuleCount; m++) {
        NDT_IMPORT_TABLE* table = (NDT_IMPORT_TABLE*)p;
        DWORD* entries = (DWORD*)(table + 1);
        for (DWORD y = 0; y + 1 < table->ImportCount; y++) {
            DWORD* slot = (DWORD*)entries[y];
            if (!MmIsAddressValid(slot)) continue;
            HOOK* hook = FindHook(*slot);
            if (!hook) continue;
            *slot = (DWORD)hook->Hook;
            FlushInsn(slot);
            WriteStub((DWORD*)entries[y + 1], (DWORD)hook->Hook);
            hooked++;
            Log("hooked XAM import %s", hook->Name);
        }
        p += table->TableSize;
    }
    return hooked;
}

// ---------------------------------------------------------------------------
// Watch thread
// ---------------------------------------------------------------------------

// TRUE if the NAS content folder can be opened (ConnectX is connected).
static BOOL NasReachable()
{
    STRING            name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK   io;
    HANDLE            h = NULL;
    RtlInitAnsiString(&name, g_Cfg.nasContent);
    oa.RootDirectory = NULL;
    oa.ObjectName    = &name;
    oa.Attributes    = 0x40;                            // OBJ_CASE_INSENSITIVE
    U64 r = g_pNtOpenFile(RAW(&h), 0x00100001, RAW(&oa), RAW(&io),
                          3 /* share read, write */, 0x21 /* directory, synchronous */);
    if (NT_SUCCESS(STATUS(r)) && h) { g_pNtClose(RAW(h)); return TRUE; }
    return FALSE;
}

static DWORD WINAPI WatchThread(LPVOID)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    int hooked = PatchXamImports();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    Log("XAM imports patched: %d hooks (expected %d)", hooked, (int)HOOK_COUNT);
    if (hooked != (int)HOOK_COUNT)   Notify(PLUGIN_NAME ": hook count wrong");
    else if (g_Cfg.notifyStart)      Notify(PLUGIN_NAME " active");
    PrefetchStart();
    FlushLog();

    DWORD lastTitle = 0xFFFFFFFF, polls = 0;
    DWORD lastNasCheck = GetTickCount() - NAS_CHECK_MS;
    BOOL  nasConnected = FALSE;
    BOOL  dlcNotified  = FALSE;     // One DLC notification per title

    for (;;) {
        DWORD now = GetTickCount();

        // Title change: free the cache, reset the per-title state.
        DWORD tid = XamGetCurrentTitleId();
        if (tid != lastTitle) {
            lastTitle = tid;
            Log("Title changed: %08X", tid);
            PrefetchReset();
            CacheTrim(TRUE);
            g_FillHint  = 0;
            dlcNotified = FALSE;
        }

        // NAS connection (checked until the first success).
        if (!nasConnected && now - lastNasCheck >= NAS_CHECK_MS) {
            lastNasCheck = now;
            if (NasReachable()) {
                nasConnected = TRUE;
                Log("NAS connected: %s", g_Cfg.nasContent);
                if (g_Cfg.notifyStart) Notify("NAS connected: DLC and title updates ready");
            }
        }

        // Events from the hooks.
        int  dlc;
        char tu[48];
        EnterCriticalSection(&g_EventLock);
        dlc = g_PendingDlc;      g_PendingDlc  = 0;
        strcpy(tu, g_PendingTu); g_PendingTu[0] = 0;
        LeaveCriticalSection(&g_EventLock);

        if (g_Cfg.notifyFound) {
            char msg[96];
            if (dlc > 0 && !dlcNotified) {
                _snprintf(msg, sizeof(msg) - 1, "NAS: %d DLC package%s found", dlc, dlc == 1 ? "" : "s");
                msg[sizeof(msg) - 1] = 0;
                Notify(msg);
                dlcNotified = TRUE;
            }
            if (tu[0]) Notify("NAS: title update loaded");
        }

        if (++polls >= FLUSH_EVERY_POLLS) {
            polls = 0;
            CacheTrim(FALSE);                             // Free idle entries
            FlushLog();
        }
        Sleep(POLL_INTERVAL_MS);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

BOOL APIENTRY DllMain(HANDLE hModule, DWORD reason, LPVOID reserved)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;

    g_StartTick = GetTickCount();
    InitializeCriticalSection(&g_LogLock);
    InitializeCriticalSection(&g_FileLock);
    InitializeCriticalSection(&g_DirLock);
    InitializeCriticalSection(&g_EventLock);
    InitializeCriticalSection(&g_PfLock);
    MountUsbDrive();
    LoadConfig();

    Log("==== " PLUGIN_NAME " loaded, USB mount %08X ====", g_MountStatus);
    if (g_IniState < 0) Log("No NasDlc.ini found: default settings");
    else                Log("NasDlc.ini read: %d settings, %d unknown", g_IniKeys, g_IniUnknown);
    Log("HDD %s -> NAS %s", g_Cfg.hddContent, g_Cfg.nasContent);
    if (g_Cfg.prefetch && !g_Cfg.cache) {
        Log("Speed.Prefetch needs Cache.Enabled = 1: prefetch off");
        g_Cfg.prefetch = FALSE;
    }
    Log("DLC %d, title updates %d, notify start %d / found %d, log %d, TU trace %d, content trace %d, cache %d, verify %d, prefetch %d (%d threads)",
        g_Cfg.dlc, g_Cfg.tu, g_Cfg.notifyStart, g_Cfg.notifyFound,
        g_Cfg.logLevel, g_Cfg.tuTrace, g_Cfg.contentTrace, g_Cfg.cache, g_Cfg.cacheVerify,
        g_Cfg.prefetch, g_Cfg.pfThreads);

    if (!CacheExports()) {
        Log("Cannot read exports. Plugin inactive.");
        FlushLog();
        return TRUE;
    }

    HANDLE thread = NULL;
    DWORD  threadId = 0;
    ExCreateThread(&thread, 0, &threadId, (PVOID)XapiThreadStartup,
                   (LPTHREAD_START_ROUTINE)WatchThread, NULL,
                   0x2 | CREATE_SUSPENDED);   // 0x2 = system thread
    if (thread) {
        XSetThreadProcessor(thread, 4);
        ResumeThread(thread);
        CloseHandle(thread);
    } else {
        Log("Cannot create watch thread.");
    }
    FlushLog();
    return TRUE;
}
