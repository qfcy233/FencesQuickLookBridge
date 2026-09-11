#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <sddl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <objbase.h>
#include <psapi.h>
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>
#include <math.h>

#define BRIDGE_VERSION L"3.4.4-native"
#define IDI_APP_ICON 101
#define MAX_PROCESS_IDS 64
#define MAX_MONITORS 16
#define MAX_PORTAL_GROUPS 64
#define MAX_PORTAL_VIEWS 128
#define MAX_PORTAL_NAVIGATION_CACHE 16
#define ROOT_CAPACITY 2048
#define PATH_CAPACITY 32768
#define ITEM_CAPACITY 1024
#define PIPE_TEXT_CAPACITY (PATH_CAPACITY + 128)
#define PIPE_BYTES_CAPACITY (PIPE_TEXT_CAPACITY * 3)
#define MAX_PORTAL_SNAPSHOT_ITEMS 16
#define PORTAL_NAME_CAPACITY 260
#define PORTAL_CACHE_ANCHORS 3
#define TRAY_ICON_ID 1
#define WM_REHOOK_QUICKLOOK (WM_APP + 1)
#define WM_TRAY_ICON (WM_APP + 2)
#define WM_SHOW_TRAY_STATUS (WM_APP + 3)
#define WM_BRIDGE_QUERY (WM_APP + 4)
#define WM_NOTIFY_UNRESPONSIVE_QUICKLOOK (WM_APP + 5)
#define TRAY_COMMAND_REFRESH 1001
#define TRAY_COMMAND_EXIT 1002
#define TRAY_WINDOW_CLASS L"FencesQuickLookBridge.TrayWindow"
#define STATUS_FILE_NAME L"FencesQuickLookBridge.status"
#define QUICKLOOK_RESPONSE_TIMEOUT_MS 400
#define QUICKLOOK_WATCHDOG_TIMER_ID 1
#define QUICKLOOK_WATCHDOG_INTERVAL_MS 2000
#define QUICKLOOK_START_GRACE_MS 20000

#define LVM_FIRST_VALUE 0x1000
#define LVM_GETITEMCOUNT_VALUE (LVM_FIRST_VALUE + 4)
#define LVM_GETNEXTITEM_VALUE (LVM_FIRST_VALUE + 12)
#define LVM_GETITEMTEXTW_VALUE (LVM_FIRST_VALUE + 115)
#define LVNI_FOCUSED_VALUE 0x0001
#define LVNI_SELECTED_VALUE 0x0002

typedef struct MonitorEntry {
    WCHAR device[32];
    RECT bounds;
} MonitorEntry;

typedef struct PortalGroup {
    WCHAR root[ROOT_CAPACITY];
    RECT bounds;
} PortalGroup;

typedef struct PortalView {
    HWND listWindow;
    RECT bounds;
    int groupIndex;
} PortalView;

typedef struct PortalCacheBuild {
    DWORD explorerPids[MAX_PROCESS_IDS];
    int explorerPidCount;
    MonitorEntry monitors[MAX_MONITORS];
    int monitorCount;
    PortalGroup groups[MAX_PORTAL_GROUPS];
    int groupCount;
    PortalView views[MAX_PORTAL_VIEWS];
    int viewCount;
} PortalCacheBuild;

typedef struct PortalListSnapshot {
    int totalItemCount;
    int nameCount;
    int indices[MAX_PORTAL_SNAPSHOT_ITEMS];
    WCHAR names[MAX_PORTAL_SNAPSHOT_ITEMS][PORTAL_NAME_CAPACITY];
} PortalListSnapshot;

typedef struct PortalNavigationCache {
    HWND listWindow;
    WCHAR directory[ROOT_CAPACITY];
    int itemCount;
    int anchorCount;
    int anchorIndices[PORTAL_CACHE_ANCHORS];
    WCHAR anchorNames[PORTAL_CACHE_ANCHORS][PORTAL_NAME_CAPACITY];
} PortalNavigationCache;

typedef struct RemoteListReader {
    HANDLE process;
    BYTE *remote;
    WCHAR *remoteText;
    int textCapacity;
} RemoteListReader;

typedef enum PreviewAction {
    PreviewAction_None = 0,
    PreviewAction_PortalToggle = 1,
    PreviewAction_Close = 2,
    PreviewAction_Exit = 3
} PreviewAction;

typedef struct WindowSearch {
    DWORD *pids;
    int pidCount;
    BOOL visibleOnly;
    HWND result;
} WindowSearch;

static DWORD g_explorerPids[MAX_PROCESS_IDS];
static int g_explorerPidCount;
static DWORD g_quickLookPids[MAX_PROCESS_IDS];
static int g_quickLookPidCount;
static ULONGLONG g_lastQuickLookPidRefresh;

static PortalGroup g_portalGroups[MAX_PORTAL_GROUPS];
static int g_portalGroupCount;
static PortalView g_portalViews[MAX_PORTAL_VIEWS];
static int g_portalViewCount;
static SRWLOCK g_portalCacheLock = SRWLOCK_INIT;
static ULONGLONG g_lastPortalCacheRefresh;

static HHOOK g_keyboardHook;
static HWINEVENTHOOK g_quickLookWinEventHook;
static HWINEVENTHOOK g_selectionWinEventHook;
static HANDLE g_workerEvent;
static HANDLE g_workerThread;
static CRITICAL_SECTION g_actionLock;
static PreviewAction g_action;
static LONG g_actionGeneration;
static WCHAR g_actionPath[PATH_CAPACITY];
static HWND g_actionPortalWindow;
static WCHAR g_pipeName[512];
static DWORD g_mainThreadId;
static DWORD g_hookOrderedQuickLookPid;
static DWORD g_quickLookCoordinationPid;
static BOOL g_quickLookCoordinationPending;
static BOOL g_traceHookOrder;

static ULONGLONG g_spaceDownAt;
static BOOL g_bridgePress;
static PVOID volatile g_activePortalWindow;
static PortalNavigationCache g_portalNavigationCache[MAX_PORTAL_NAVIGATION_CACHE];
static SRWLOCK g_portalNavigationCacheLock = SRWLOCK_INIT;

static HWND g_trayWindow;
static UINT g_taskbarCreatedMessage;
static HICON g_trayIcon;

static volatile LONG g_spacePressCount;
static volatile LONG g_portalToggleCount;
static volatile LONG g_invokeCount;
static volatile LONG g_closeCount;
static volatile LONG g_resolveFailureCount;
static volatile LONG g_unresponsiveNotices;
static DWORD g_lastResolveMilliseconds;
static ULONGLONG g_startedAtTick;
static BOOL g_quickLookWatchdogRunning;
static ULONGLONG g_quickLookWatchdogStartedAt;

static BOOL InitializePipeName(void);
static void RefreshQuickLookPids(void);
static void TraceHookEvent(const WCHAR *eventName, DWORD pid);
static BOOL ReinstallKeyboardHook(HINSTANCE instance, DWORD quickLookPid);
static const WCHAR *FileNamePart(const WCHAR *path);
static BOOL SendPipeMessage(const WCHAR *message, const WCHAR *path);
static BOOL AddTrayIcon(HWND window);
static BOOL IsWindowResponsive(HWND window);
static void WriteStdoutText(const WCHAR *text);
static void StartQuickLookWatchdog(void);
static void StopQuickLookWatchdog(HWND window);
static void ShowUnresponsiveQuickLookNotice(HWND window);

static HICON LoadApplicationIcon(HINSTANCE instance, int width, int height)
{
    HICON icon = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON),
        IMAGE_ICON, width, height, LR_DEFAULTCOLOR | LR_SHARED);
    return icon ? icon : LoadIconW(NULL, IDI_APPLICATION);
}

static BOOL IsPidInList(DWORD pid, const DWORD *pids, int count)
{
    int i;
    for (i = 0; i < count; ++i) if (pids[i] == pid) return TRUE;
    return FALSE;
}

static BOOL ContainsInsensitive(const WCHAR *text, const WCHAR *needle)
{
    size_t needleLength;
    const WCHAR *cursor;
    if (!text || !needle || !*needle) return FALSE;
    needleLength = wcslen(needle);
    for (cursor = text; *cursor; ++cursor)
        if (_wcsnicmp(cursor, needle, needleLength) == 0) return TRUE;
    return FALSE;
}

static const WCHAR *FileNamePart(const WCHAR *path)
{
    const WCHAR *slash;
    if (!path) return L"";
    slash = wcsrchr(path, L'\\');
    return slash ? slash + 1 : path;
}

static BOOL IsExistingPath(const WCHAR *path)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES;
}

static int ReadProcessIds(const WCHAR *processName, DWORD *output, int capacity)
{
    HANDLE snapshot;
    PROCESSENTRY32W entry;
    int count = 0;
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    ZeroMemory(&entry, sizeof(entry));
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, processName) == 0 && count < capacity)
                output[count++] = entry.th32ProcessID;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return count;
}

static BOOL CALLBACK MonitorCallback(HMONITOR monitor, HDC hdc, LPRECT rect, LPARAM data)
{
    PortalCacheBuild *build = (PortalCacheBuild *)data;
    MONITORINFOEXW info;
    MonitorEntry *entry;
    (void)hdc;
    (void)rect;
    if (!build || build->monitorCount >= MAX_MONITORS) return FALSE;
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, (MONITORINFO *)&info)) return TRUE;
    entry = &build->monitors[build->monitorCount++];
    wcsncpy_s(entry->device, 32, info.szDevice, _TRUNCATE);
    entry->bounds = info.rcMonitor;
    return TRUE;
}

static WCHAR *TrimField(WCHAR *value)
{
    WCHAR *end;
    while (*value == L' ' || *value == L'`') ++value;
    end = value + wcslen(value);
    while (end > value && (end[-1] == L' ' || end[-1] == L'`')) --end;
    *end = L'\0';
    return value;
}

static int SplitFields(WCHAR *value, WCHAR **fields, int capacity)
{
    int count = 0;
    WCHAR *cursor = value;
    if (capacity <= 0) return 0;
    fields[count++] = cursor;
    while (*cursor && count < capacity) {
        if (*cursor == L'|') {
            *cursor = L'\0';
            fields[count++] = cursor + 1;
        }
        ++cursor;
    }
    return count;
}

static BOOL DirectoryExists(const WCHAR *path)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static void ReadPortalGroups(PortalCacheBuild *build)
{
    HKEY key;
    DWORD valueIndex = 0;
    if (!build) return;
    build->monitorCount = 0;
    build->groupCount = 0;
    EnumDisplayMonitors(NULL, NULL, MonitorCallback, (LPARAM)build);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Stardock\\Fences\\Groups",
        0, KEY_READ, &key) != ERROR_SUCCESS) return;

    for (;;) {
        WCHAR valueName[512];
        WCHAR data[8192];
        DWORD valueNameLength = 511;
        DWORD dataBytes = sizeof(data) - sizeof(WCHAR);
        DWORD type = 0;
        LONG result;
        WCHAR *fields[32];
        int fieldCount;
        WCHAR *root;
        LONG x, y, width, height, physicalLeft, physicalTop, physicalRight, physicalBottom;
        RECT logicalMonitor;
        int i;
        double scaleX, scaleY;
        PortalGroup *group;

        ZeroMemory(valueName, sizeof(valueName));
        ZeroMemory(data, sizeof(data));
        result = RegEnumValueW(key, valueIndex++, valueName, &valueNameLength,
            NULL, &type, (BYTE *)data, &dataBytes);
        if (result == ERROR_NO_MORE_ITEMS) break;
        if (result != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) continue;
        data[dataBytes / sizeof(WCHAR)] = L'\0';
        fieldCount = SplitFields(data, fields, 32);
        if (fieldCount < 17 || build->groupCount >= MAX_PORTAL_GROUPS) continue;
        for (i = 0; i < fieldCount; ++i) fields[i] = TrimField(fields[i]);
        root = fields[16];
        if (!*root || !DirectoryExists(root)) continue;

        x = wcstol(fields[1], NULL, 10);
        y = wcstol(fields[2], NULL, 10);
        width = wcstol(fields[3], NULL, 10);
        height = wcstol(fields[4], NULL, 10);
        physicalLeft = wcstol(fields[8], NULL, 10);
        physicalTop = wcstol(fields[9], NULL, 10);
        physicalRight = wcstol(fields[10], NULL, 10);
        physicalBottom = wcstol(fields[11], NULL, 10);
        if (physicalRight <= physicalLeft || physicalBottom <= physicalTop) continue;

        SetRect(&logicalMonitor, physicalLeft, physicalTop, physicalRight, physicalBottom);
        for (i = 0; i < build->monitorCount; ++i) {
            if (_wcsicmp(build->monitors[i].device, fields[7]) == 0) {
                logicalMonitor = build->monitors[i].bounds;
                break;
            }
        }
        scaleX = (logicalMonitor.right - logicalMonitor.left)
            / (double)(physicalRight - physicalLeft);
        scaleY = (logicalMonitor.bottom - logicalMonitor.top)
            / (double)(physicalBottom - physicalTop);
        group = &build->groups[build->groupCount++];
        wcsncpy_s(group->root, ROOT_CAPACITY, root, _TRUNCATE);
        group->bounds.left = logicalMonitor.left + (LONG)llround((x - physicalLeft) * scaleX);
        group->bounds.top = logicalMonitor.top + (LONG)llround((y - physicalTop) * scaleY);
        group->bounds.right = group->bounds.left + max(1L, (LONG)llround(width * scaleX));
        group->bounds.bottom = group->bounds.top + max(1L, (LONG)llround(height * scaleY));
    }
    RegCloseKey(key);
}

static int FindMatchingGroup(const PortalCacheBuild *build, const RECT *bounds)
{
    int best = -1;
    LONG bestDifference = LONG_MAX;
    int i;
    if (!build) return -1;
    for (i = 0; i < build->groupCount; ++i) {
        const RECT *group = &build->groups[i].bounds;
        LONG difference = labs(group->left - bounds->left)
            + labs(group->top - bounds->top)
            + labs((group->right - group->left) - (bounds->right - bounds->left))
            + labs((group->bottom - group->top) - (bounds->bottom - bounds->top));
        if (difference <= 160 && difference < bestDifference) {
            best = i;
            bestDifference = difference;
        }
    }
    return best;
}

static void ConsiderPortalWindow(PortalCacheBuild *build, HWND window)
{
    WCHAR className[128];
    RECT bounds;
    int groupIndex;
    PortalView *view;
    if (!build || build->viewCount >= MAX_PORTAL_VIEWS || !IsWindowVisible(window)) return;
    if (!GetClassNameW(window, className, 128) || wcscmp(className, L"SysListView32") != 0) return;
    if (!GetWindowRect(window, &bounds)) return;
    groupIndex = FindMatchingGroup(build, &bounds);
    if (groupIndex < 0) return;
    view = &build->views[build->viewCount++];
    view->listWindow = window;
    view->bounds = bounds;
    view->groupIndex = groupIndex;
}

static BOOL CALLBACK ExplorerChildCallback(HWND window, LPARAM data)
{
    ConsiderPortalWindow((PortalCacheBuild *)data, window);
    return TRUE;
}

static BOOL CALLBACK ExplorerTopCallback(HWND window, LPARAM data)
{
    PortalCacheBuild *build = (PortalCacheBuild *)data;
    DWORD pid = 0;
    if (!build) return FALSE;
    GetWindowThreadProcessId(window, &pid);
    if (!IsPidInList(pid, build->explorerPids, build->explorerPidCount)) return TRUE;
    ConsiderPortalWindow(build, window);
    EnumChildWindows(window, ExplorerChildCallback, (LPARAM)build);
    return TRUE;
}

static void RefreshPortalCache(void)
{
    PortalCacheBuild *build = (PortalCacheBuild *)VirtualAlloc(NULL,
        sizeof(PortalCacheBuild), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!build) return;
    build->explorerPidCount = ReadProcessIds(L"explorer.exe",
        build->explorerPids, MAX_PROCESS_IDS);
    ReadPortalGroups(build);
    EnumWindows(ExplorerTopCallback, (LPARAM)build);

    AcquireSRWLockExclusive(&g_portalCacheLock);
    g_explorerPidCount = build->explorerPidCount;
    if (g_explorerPidCount > 0) CopyMemory(g_explorerPids, build->explorerPids,
        (SIZE_T)g_explorerPidCount * sizeof(DWORD));
    g_portalGroupCount = build->groupCount;
    if (g_portalGroupCount > 0) CopyMemory(g_portalGroups, build->groups,
        (SIZE_T)g_portalGroupCount * sizeof(PortalGroup));
    g_portalViewCount = build->viewCount;
    if (g_portalViewCount > 0) CopyMemory(g_portalViews, build->views,
        (SIZE_T)g_portalViewCount * sizeof(PortalView));
    g_lastPortalCacheRefresh = GetTickCount64();
    ReleaseSRWLockExclusive(&g_portalCacheLock);
    VirtualFree(build, 0, MEM_RELEASE);
}

static BOOL InitializeRemoteListReader(HWND listWindow, int textCapacity,
    RemoteListReader *reader)
{
    const DWORD access = PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE
        | PROCESS_QUERY_LIMITED_INFORMATION;
    DWORD pid = 0;
    SIZE_T textBytes;
    SIZE_T totalBytes;
    if (!listWindow || !reader || textCapacity <= 1) return FALSE;
    ZeroMemory(reader, sizeof(*reader));
    GetWindowThreadProcessId(listWindow, &pid);
    reader->process = OpenProcess(access, FALSE, pid);
    if (!reader->process) return FALSE;
    textBytes = (SIZE_T)textCapacity * sizeof(WCHAR);
    totalBytes = sizeof(LVITEMW) + textBytes;
    reader->remote = (BYTE *)VirtualAllocEx(reader->process, NULL, totalBytes,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!reader->remote) {
        CloseHandle(reader->process);
        ZeroMemory(reader, sizeof(*reader));
        return FALSE;
    }
    reader->remoteText = (WCHAR *)(reader->remote + sizeof(LVITEMW));
    reader->textCapacity = textCapacity;
    return TRUE;
}

static void DisposeRemoteListReader(RemoteListReader *reader)
{
    if (!reader) return;
    if (reader->remote && reader->process)
        VirtualFreeEx(reader->process, reader->remote, 0, MEM_RELEASE);
    if (reader->process) CloseHandle(reader->process);
    ZeroMemory(reader, sizeof(*reader));
}

static BOOL ReadRemoteListItemText(RemoteListReader *reader, HWND listWindow,
    int index, WCHAR *output, int outputCapacity)
{
    LVITEMW item;
    SIZE_T transferred;
    SIZE_T textBytes;
    int readCapacity;
    BOOL success = FALSE;
    if (!reader || !reader->process || !reader->remote || !output
        || outputCapacity <= 1) return FALSE;
    output[0] = L'\0';
    readCapacity = min(outputCapacity, reader->textCapacity);
    textBytes = (SIZE_T)readCapacity * sizeof(WCHAR);
    ZeroMemory(&item, sizeof(item));
    item.iSubItem = 0;
    item.pszText = reader->remoteText;
    item.cchTextMax = readCapacity;
    if (WriteProcessMemory(reader->process, reader->remote, &item,
        sizeof(item), &transferred)) {
        SendMessageW(listWindow, LVM_GETITEMTEXTW_VALUE, (WPARAM)index,
            (LPARAM)reader->remote);
        if (ReadProcessMemory(reader->process, reader->remoteText, output,
            textBytes, &transferred)) {
            output[readCapacity - 1] = L'\0';
            success = output[0] != L'\0';
        }
    }
    return success;
}

static BOOL ReadListItemText(HWND listWindow, int index, WCHAR *output,
    int outputCapacity)
{
    RemoteListReader reader;
    BOOL success;
    if (!InitializeRemoteListReader(listWindow, outputCapacity, &reader)) return FALSE;
    success = ReadRemoteListItemText(&reader, listWindow, index, output,
        outputCapacity);
    DisposeRemoteListReader(&reader);
    return success;
}

static void NameWithoutExtension(const WCHAR *name, WCHAR *output, int capacity)
{
    WCHAR *dot;
    wcsncpy_s(output, capacity, name, _TRUNCATE);
    dot = wcsrchr(output, L'.');
    if (dot && dot != output) *dot = L'\0';
}

static BOOL ResolvePortalItem(const WCHAR *root, const WCHAR *itemText,
    WCHAR *output, int outputCapacity)
{
    WCHAR direct[PATH_CAPACITY];
    WCHAR pattern[PATH_CAPACITY];
    WCHAR match[PATH_CAPACITY];
    WIN32_FIND_DATAW data;
    HANDLE find;
    int matches = 0;
    if (!root || !*root || !itemText || !*itemText) return FALSE;
    if (_snwprintf_s(direct, PATH_CAPACITY, _TRUNCATE, L"%s\\%s", root, itemText) > 0
        && IsExistingPath(direct)) {
        wcsncpy_s(output, outputCapacity, direct, _TRUNCATE);
        return TRUE;
    }
    if (_snwprintf_s(pattern, PATH_CAPACITY, _TRUNCATE, L"%s\\*", root) <= 0) return FALSE;
    find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) return FALSE;
    match[0] = L'\0';
    do {
        WCHAR stem[MAX_PATH];
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        NameWithoutExtension(data.cFileName, stem, MAX_PATH);
        if (_wcsicmp(data.cFileName, itemText) != 0 && _wcsicmp(stem, itemText) != 0) continue;
        ++matches;
        if (matches > 1) break;
        _snwprintf_s(match, PATH_CAPACITY, _TRUNCATE, L"%s\\%s", root, data.cFileName);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    if (matches != 1) return FALSE;
    wcsncpy_s(output, outputCapacity, match, _TRUNCATE);
    return TRUE;
}

static BOOL IsPathWithinRoot(const WCHAR *path, const WCHAR *root)
{
    size_t rootLength;
    if (!path || !root || !*path || !*root) return FALSE;
    rootLength = wcslen(root);
    if (_wcsnicmp(path, root, rootLength) != 0) return FALSE;
    return path[rootLength] == L'\0' || path[rootLength] == L'\\';
}

static BOOL BuildPortalListSnapshot(HWND listWindow, RemoteListReader *reader,
    PortalListSnapshot *snapshot)
{
    int sampleCount;
    int sample;
    if (!listWindow || !reader || !snapshot) return FALSE;
    ZeroMemory(snapshot, sizeof(*snapshot));
    snapshot->totalItemCount = (int)SendMessageW(listWindow,
        LVM_GETITEMCOUNT_VALUE, 0, 0);
    sampleCount = min(snapshot->totalItemCount, MAX_PORTAL_SNAPSHOT_ITEMS);
    for (sample = 0; sample < sampleCount; ++sample) {
        int index = sampleCount == 1 ? 0
            : (sample * (snapshot->totalItemCount - 1)) / (sampleCount - 1);
        WCHAR *name = snapshot->names[snapshot->nameCount];
        if (ReadRemoteListItemText(reader, listWindow, index, name,
            PORTAL_NAME_CAPACITY)) {
            snapshot->indices[snapshot->nameCount] = index;
            ++snapshot->nameCount;
        }
    }
    return snapshot->nameCount > 0;
}

static int ScorePortalDirectory(const WCHAR *directory,
    const PortalListSnapshot *snapshot)
{
    WCHAR pattern[PATH_CAPACITY];
    WIN32_FIND_DATAW data;
    HANDLE find;
    uint64_t matchedMask = 0;
    int entryCount = 0;
    int matched = 0;
    int i;
    if (!directory || !snapshot || snapshot->nameCount <= 0) return INT_MIN;
    if (_snwprintf_s(pattern, PATH_CAPACITY, _TRUNCATE,
        L"%s\\*", directory) <= 0) return INT_MIN;
    find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) return INT_MIN;
    do {
        WCHAR stem[MAX_PATH];
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
            continue;
        ++entryCount;
        NameWithoutExtension(data.cFileName, stem, MAX_PATH);
        for (i = 0; i < snapshot->nameCount; ++i) {
            uint64_t bit = ((uint64_t)1) << i;
            if ((matchedMask & bit) != 0) continue;
            if (_wcsicmp(data.cFileName, snapshot->names[i]) == 0
                || _wcsicmp(stem, snapshot->names[i]) == 0) {
                matchedMask |= bit;
                ++matched;
                break;
            }
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return matched * 10000 - abs(entryCount - snapshot->totalItemCount);
}

static BOOL TryGetCachedPortalDirectory(HWND listWindow,
    PortalNavigationCache *cache)
{
    int i;
    BOOL found = FALSE;
    if (!cache) return FALSE;
    ZeroMemory(cache, sizeof(*cache));
    AcquireSRWLockShared(&g_portalNavigationCacheLock);
    for (i = 0; i < MAX_PORTAL_NAVIGATION_CACHE; ++i) {
        if (!g_portalNavigationCache[i].listWindow) break;
        if (g_portalNavigationCache[i].listWindow != listWindow) continue;
        CopyMemory(cache, &g_portalNavigationCache[i], sizeof(*cache));
        found = cache->directory[0] != L'\0';
        break;
    }
    ReleaseSRWLockShared(&g_portalNavigationCacheLock);
    return found;
}

static void SetCachedPortalDirectory(HWND listWindow, const WCHAR *directory,
    const PortalListSnapshot *snapshot)
{
    int i;
    int slot = -1;
    int anchor;
    AcquireSRWLockExclusive(&g_portalNavigationCacheLock);
    for (i = 0; i < MAX_PORTAL_NAVIGATION_CACHE; ++i) {
        if (!g_portalNavigationCache[i].listWindow) {
            slot = i;
            break;
        }
        if (g_portalNavigationCache[i].listWindow == listWindow) {
            slot = i;
            break;
        }
    }
    if (slot < 0) slot = 0;
    ZeroMemory(&g_portalNavigationCache[slot], sizeof(PortalNavigationCache));
    g_portalNavigationCache[slot].listWindow = listWindow;
    wcsncpy_s(g_portalNavigationCache[slot].directory, ROOT_CAPACITY,
        directory, _TRUNCATE);
    if (snapshot) {
        g_portalNavigationCache[slot].itemCount = snapshot->totalItemCount;
        g_portalNavigationCache[slot].anchorCount = min(snapshot->nameCount,
            PORTAL_CACHE_ANCHORS);
        for (anchor = 0; anchor < g_portalNavigationCache[slot].anchorCount;
            ++anchor) {
            int snapshotIndex = g_portalNavigationCache[slot].anchorCount == 1 ? 0
                : (anchor * (snapshot->nameCount - 1))
                    / (g_portalNavigationCache[slot].anchorCount - 1);
            g_portalNavigationCache[slot].anchorIndices[anchor] =
                snapshot->indices[snapshotIndex];
            wcsncpy_s(g_portalNavigationCache[slot].anchorNames[anchor],
                PORTAL_NAME_CAPACITY, snapshot->names[snapshotIndex], _TRUNCATE);
        }
    }
    ReleaseSRWLockExclusive(&g_portalNavigationCacheLock);
}

static BOOL PortalDirectoryMatchesView(HWND listWindow, RemoteListReader *reader,
    const PortalNavigationCache *cache,
    const WCHAR *selectedText, WCHAR *selectedPath, int selectedPathCapacity)
{
    int itemCount;
    int i;
    if (!cache || !ResolvePortalItem(cache->directory, selectedText,
        selectedPath, selectedPathCapacity))
        return FALSE;
    itemCount = (int)SendMessageW(listWindow, LVM_GETITEMCOUNT_VALUE, 0, 0);
    if (itemCount <= 0 || itemCount != cache->itemCount) return FALSE;
    for (i = 0; i < cache->anchorCount; ++i) {
        WCHAR name[PORTAL_NAME_CAPACITY];
        name[0] = L'\0';
        if (!ReadRemoteListItemText(reader, listWindow,
            cache->anchorIndices[i], name, PORTAL_NAME_CAPACITY)
            || wcscmp(name, cache->anchorNames[i]) != 0) return FALSE;
    }
    return TRUE;
}

static BOOL ResolvePortalItemForView(HWND listWindow, const WCHAR *root,
    const WCHAR *itemText, RemoteListReader *reader, WCHAR *output,
    int outputCapacity)
{
    PortalNavigationCache cached;
    PortalListSnapshot snapshot;
    WCHAR bestDirectory[ROOT_CAPACITY];
    WCHAR bestPath[PATH_CAPACITY];
    int bestScore = INT_MIN;
    DWORD bestOrder = 0;
    HKEY key;
    DWORD valueIndex = 0;
    WCHAR lastDirectory[ROOT_CAPACITY];

    if (TryGetCachedPortalDirectory(listWindow, &cached)
        && IsPathWithinRoot(cached.directory, root)
        && PortalDirectoryMatchesView(listWindow, reader, &cached, itemText,
            output, outputCapacity)) return TRUE;

    BuildPortalListSnapshot(listWindow, reader, &snapshot);
    bestDirectory[0] = L'\0';
    bestPath[0] = L'\0';
    lastDirectory[0] = L'\0';

    if (ResolvePortalItem(root, itemText, bestPath, PATH_CAPACITY)) {
        bestScore = ScorePortalDirectory(root, &snapshot);
        wcsncpy_s(bestDirectory, ROOT_CAPACITY, root, _TRUNCATE);
    }

    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Stardock\\Fences\\ViewStates", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        for (;;) {
            WCHAR valueName[PATH_CAPACITY];
            DWORD valueNameLength = PATH_CAPACITY - 1;
            LONG result;
            WCHAR *separator;
            WCHAR resolved[PATH_CAPACITY];
            int score;
            result = RegEnumValueW(key, valueIndex, valueName, &valueNameLength,
                NULL, NULL, NULL, NULL);
            if (result == ERROR_NO_MORE_ITEMS) break;
            ++valueIndex;
            if (result != ERROR_SUCCESS) continue;
            valueName[valueNameLength] = L'\0';
            separator = wcsrchr(valueName, L'|');
            if (!separator) continue;
            *separator = L'\0';
            if (!IsPathWithinRoot(valueName, root)
                || _wcsicmp(valueName, lastDirectory) == 0) continue;
            wcsncpy_s(lastDirectory, ROOT_CAPACITY, valueName, _TRUNCATE);
            if (!DirectoryExists(valueName)
                || !ResolvePortalItem(valueName, itemText, resolved, PATH_CAPACITY)) continue;
            score = ScorePortalDirectory(valueName, &snapshot);
            if (score > bestScore || (score == bestScore && valueIndex >= bestOrder)) {
                bestScore = score;
                bestOrder = valueIndex;
                wcsncpy_s(bestDirectory, ROOT_CAPACITY, valueName, _TRUNCATE);
                wcsncpy_s(bestPath, PATH_CAPACITY, resolved, _TRUNCATE);
            }
        }
        RegCloseKey(key);
    }

    if (!bestPath[0]) return FALSE;
    SetCachedPortalDirectory(listWindow, bestDirectory, &snapshot);
    wcsncpy_s(output, outputCapacity, bestPath, _TRUNCATE);
    return TRUE;
}

static int CopyPortalViews(PortalView *views, int capacity)
{
    int count;
    AcquireSRWLockShared(&g_portalCacheLock);
    count = min(g_portalViewCount, capacity);
    if (count > 0) CopyMemory(views, g_portalViews, (SIZE_T)count * sizeof(PortalView));
    ReleaseSRWLockShared(&g_portalCacheLock);
    return count;
}

static BOOL GetPortalRootForView(HWND listWindow, WCHAR *root, int rootCapacity)
{
    int i;
    BOOL found = FALSE;
    if (!root || rootCapacity <= 1) return FALSE;
    root[0] = L'\0';
    AcquireSRWLockShared(&g_portalCacheLock);
    for (i = 0; i < g_portalViewCount; ++i) {
        int groupIndex;
        if (g_portalViews[i].listWindow != listWindow) continue;
        groupIndex = g_portalViews[i].groupIndex;
        if (groupIndex >= 0 && groupIndex < g_portalGroupCount) {
            wcsncpy_s(root, rootCapacity, g_portalGroups[groupIndex].root, _TRUNCATE);
            found = root[0] != L'\0';
        }
        break;
    }
    ReleaseSRWLockShared(&g_portalCacheLock);
    return found;
}

static int GetPortalViewCount(void)
{
    int count;
    AcquireSRWLockShared(&g_portalCacheLock);
    count = g_portalViewCount;
    ReleaseSRWLockShared(&g_portalCacheLock);
    return count;
}

static BOOL FindCachedPortalViewForWindow(HWND window, PortalView *result)
{
    int i;
    BOOL found = FALSE;
    if (!window) return FALSE;
    AcquireSRWLockShared(&g_portalCacheLock);
    for (i = 0; i < g_portalViewCount; ++i) {
        if (g_portalViews[i].listWindow != window
            && !IsChild(g_portalViews[i].listWindow, window)) continue;
        if (result) *result = g_portalViews[i];
        found = TRUE;
        break;
    }
    ReleaseSRWLockShared(&g_portalCacheLock);
    return found;
}

static BOOL IsPortalListWindowByBounds(HWND window)
{
    WCHAR className[128];
    RECT bounds;
    int i;
    BOOL matches = FALSE;
    if (!window || !IsWindowVisible(window)
        || !GetClassNameW(window, className, 128)
        || wcscmp(className, L"SysListView32") != 0
        || !GetWindowRect(window, &bounds)) return FALSE;
    AcquireSRWLockShared(&g_portalCacheLock);
    for (i = 0; i < g_portalGroupCount; ++i) {
        const RECT *group = &g_portalGroups[i].bounds;
        LONG difference = labs(group->left - bounds.left)
            + labs(group->top - bounds.top)
            + labs((group->right - group->left) - (bounds.right - bounds.left))
            + labs((group->bottom - group->top) - (bounds.bottom - bounds.top));
        if (difference <= 160) {
            matches = TRUE;
            break;
        }
    }
    ReleaseSRWLockShared(&g_portalCacheLock);
    return matches;
}

static BOOL PortalListHasSelection(HWND listWindow)
{
    int selectedIndex;
    if (!listWindow || !IsWindow(listWindow) || !IsWindowVisible(listWindow))
        return FALSE;
    selectedIndex = (int)SendMessageW(listWindow, LVM_GETNEXTITEM_VALUE,
        (WPARAM)-1, LVNI_SELECTED_VALUE);
    if (selectedIndex < 0) selectedIndex = (int)SendMessageW(listWindow,
        LVM_GETNEXTITEM_VALUE, (WPARAM)-1, LVNI_FOCUSED_VALUE);
    return selectedIndex >= 0;
}

static void SetActivePortalFromSelection(HWND eventWindow)
{
    PortalView view;
    if (FindCachedPortalViewForWindow(eventWindow, &view)
        && PortalListHasSelection(view.listWindow)) {
        InterlockedExchangePointer(&g_activePortalWindow, view.listWindow);
    } else {
        InterlockedExchangePointer(&g_activePortalWindow, NULL);
    }
}

static void InitializeActivePortalFromFocus(void)
{
    HWND foreground = GetForegroundWindow();
    DWORD threadId;
    GUITHREADINFO info;
    if (!foreground) return;
    threadId = GetWindowThreadProcessId(foreground, NULL);
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    if (GetGUIThreadInfo(threadId, &info) && info.hwndFocus)
        SetActivePortalFromSelection(info.hwndFocus);
}

static BOOL ReadPortalSelectionFromView(HWND listWindow, const WCHAR *root,
    WCHAR *path, int pathCapacity)
{
    int selectedIndex;
    WCHAR itemText[ITEM_CAPACITY];
    RemoteListReader reader;
    BOOL success;
    if (!listWindow || !root || !*root
        || !IsWindow(listWindow) || !IsWindowVisible(listWindow)) return FALSE;
    selectedIndex = (int)SendMessageW(listWindow, LVM_GETNEXTITEM_VALUE,
        (WPARAM)-1, LVNI_SELECTED_VALUE);
    if (selectedIndex < 0) {
        selectedIndex = (int)SendMessageW(listWindow, LVM_GETNEXTITEM_VALUE,
            (WPARAM)-1, LVNI_FOCUSED_VALUE);
    }
    if (selectedIndex < 0) return FALSE;
    if (!InitializeRemoteListReader(listWindow, ITEM_CAPACITY, &reader)) return FALSE;
    if (!ReadRemoteListItemText(&reader, listWindow, selectedIndex, itemText,
        ITEM_CAPACITY)) {
        DisposeRemoteListReader(&reader);
        return FALSE;
    }
    success = ResolvePortalItemForView(listWindow, root, itemText, &reader,
        path, pathCapacity);
    DisposeRemoteListReader(&reader);
    return success;
}

static BOOL TryGetPortalSelection(WCHAR *path, int pathCapacity)
{
    HWND listWindow = (HWND)InterlockedCompareExchangePointer(
        &g_activePortalWindow, NULL, NULL);
    WCHAR root[ROOT_CAPACITY];
    if (!listWindow || !GetPortalRootForView(listWindow, root, ROOT_CAPACITY))
        return FALSE;
    return ReadPortalSelectionFromView(listWindow, root, path, pathCapacity);
}

static void RequestPortalCacheRefresh(void)
{
    ULONGLONG lastRefresh;
    AcquireSRWLockShared(&g_portalCacheLock);
    lastRefresh = g_lastPortalCacheRefresh;
    ReleaseSRWLockShared(&g_portalCacheLock);
    if (lastRefresh && GetTickCount64() - lastRefresh < 1000) return;
    EnterCriticalSection(&g_actionLock);
    if (g_action == PreviewAction_None) {
        g_action = PreviewAction_PortalToggle;
        ++g_actionGeneration;
        g_actionPortalWindow = NULL;
        g_actionPath[0] = L'\0';
    }
    LeaveCriticalSection(&g_actionLock);
    SetEvent(g_workerEvent);
}

static BOOL TryGetActivePortalView(HWND *listWindow, WCHAR *root,
    int rootCapacity)
{
    HWND active = (HWND)InterlockedCompareExchangePointer(
        &g_activePortalWindow, NULL, NULL);
    int selectedIndex;
    if (!active || !IsWindow(active) || !IsWindowVisible(active)) return FALSE;
    if (!GetPortalRootForView(active, root, rootCapacity)) return FALSE;
    selectedIndex = (int)SendMessageW(active, LVM_GETNEXTITEM_VALUE,
        (WPARAM)-1, LVNI_SELECTED_VALUE);
    if (selectedIndex < 0) selectedIndex = (int)SendMessageW(active,
        LVM_GETNEXTITEM_VALUE, (WPARAM)-1, LVNI_FOCUSED_VALUE);
    if (selectedIndex < 0) return FALSE;
    if (listWindow) *listWindow = active;
    return TRUE;
}

static BOOL TryGetAnyPortalSelectionDetails(HWND *listWindow, WCHAR *root,
    int rootCapacity, WCHAR *path, int pathCapacity)
{
    PortalView views[MAX_PORTAL_VIEWS];
    int viewCount;
    int i;
    viewCount = CopyPortalViews(views, MAX_PORTAL_VIEWS);
    if (viewCount == 0) {
        RefreshPortalCache();
        viewCount = CopyPortalViews(views, MAX_PORTAL_VIEWS);
    }
    for (i = 0; i < viewCount; ++i) {
        PortalView *view = &views[i];
        int selectedIndex;
        WCHAR itemText[ITEM_CAPACITY];
        WCHAR currentRoot[ROOT_CAPACITY];
        RemoteListReader reader;
        BOOL success;
        if (!IsWindow(view->listWindow) || !IsWindowVisible(view->listWindow)) continue;
        selectedIndex = (int)SendMessageW(view->listWindow, LVM_GETNEXTITEM_VALUE,
            (WPARAM)-1, LVNI_SELECTED_VALUE);
        if (selectedIndex < 0) {
            selectedIndex = (int)SendMessageW(view->listWindow, LVM_GETNEXTITEM_VALUE,
                (WPARAM)-1, LVNI_FOCUSED_VALUE);
        }
        if (selectedIndex < 0) continue;
        if (!InitializeRemoteListReader(view->listWindow, ITEM_CAPACITY, &reader))
            continue;
        if (!ReadRemoteListItemText(&reader, view->listWindow, selectedIndex,
            itemText, ITEM_CAPACITY)) {
            DisposeRemoteListReader(&reader);
            continue;
        }
        if (!GetPortalRootForView(view->listWindow, currentRoot, ROOT_CAPACITY)) {
            DisposeRemoteListReader(&reader);
            continue;
        }
        success = ResolvePortalItemForView(view->listWindow, currentRoot,
            itemText, &reader, path, pathCapacity);
        DisposeRemoteListReader(&reader);
        if (success) {
            if (listWindow) *listWindow = view->listWindow;
            if (root) wcsncpy_s(root, rootCapacity, currentRoot, _TRUNCATE);
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL TryGetAnyPortalSelection(WCHAR *path, int pathCapacity)
{
    return TryGetAnyPortalSelectionDetails(NULL, NULL, 0, path, pathCapacity);
}

static BOOL IsTextEntryActive(HWND foreground)
{
    DWORD pid = 0;
    DWORD thread = GetWindowThreadProcessId(foreground, &pid);
    GUITHREADINFO info;
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    return GetGUIThreadInfo(thread, &info)
        && (info.flags != 0 || info.hwndCaret != NULL);
}

static BOOL IsNativeSelectionContext(void)
{
    HWND foreground = GetForegroundWindow();
    DWORD pid = 0;
    DWORD explorerPids[MAX_PROCESS_IDS];
    int explorerPidCount;
    WCHAR className[256];
    if (!foreground || IsTextEntryActive(foreground)) return FALSE;
    GetWindowThreadProcessId(foreground, &pid);
    AcquireSRWLockShared(&g_portalCacheLock);
    explorerPidCount = g_explorerPidCount;
    CopyMemory(explorerPids, g_explorerPids, sizeof(explorerPids));
    ReleaseSRWLockShared(&g_portalCacheLock);
    if (IsPidInList(pid, explorerPids, explorerPidCount)) return TRUE;
    className[0] = L'\0';
    GetClassNameW(foreground, className, 256);
    if (ContainsInsensitive(className, L"Fence")
        || ContainsInsensitive(className, L"DesktopDock")) return TRUE;
    if (_wcsicmp(className, L"CabinetWClass") == 0
        || _wcsicmp(className, L"Progman") == 0
        || _wcsicmp(className, L"WorkerW") == 0) {
        RequestPortalCacheRefresh();
        return TRUE;
    }
    return FALSE;
}

static BOOL CALLBACK QuickLookWindowCallback(HWND window, LPARAM data)
{
    WindowSearch *search = (WindowSearch *)data;
    DWORD pid = 0;
    WCHAR className[256];
    WCHAR title[1024];
    GetWindowThreadProcessId(window, &pid);
    if (!IsPidInList(pid, search->pids, search->pidCount)
        || (search->visibleOnly && !IsWindowVisible(window))) return TRUE;
    className[0] = L'\0';
    GetClassNameW(window, className, 256);
    if (wcsncmp(className, L"HwndWrapper[QuickLook.exe",
        wcslen(L"HwndWrapper[QuickLook.exe")) != 0) return TRUE;
    title[0] = L'\0';
    GetWindowTextW(window, title, 1024);
    if (!*title || _wcsicmp(title, L"Hidden Window") == 0) return TRUE;
    search->result = window;
    return FALSE;
}

static void RefreshQuickLookPids(void)
{
    g_quickLookPidCount = ReadProcessIds(L"QuickLook.exe", g_quickLookPids, MAX_PROCESS_IDS);
    g_lastQuickLookPidRefresh = GetTickCount64();
}

static HWND FindQuickLookWindow(BOOL visibleOnly)
{
    WindowSearch search;
    if (g_quickLookPidCount == 0) RefreshQuickLookPids();
    ZeroMemory(&search, sizeof(search));
    search.pids = g_quickLookPids;
    search.pidCount = g_quickLookPidCount;
    search.visibleOnly = visibleOnly;
    EnumWindows(QuickLookWindowCallback, (LPARAM)&search);
    if (!search.result && GetTickCount64() - g_lastQuickLookPidRefresh > 1000) {
        RefreshQuickLookPids();
        search.pids = g_quickLookPids;
        search.pidCount = g_quickLookPidCount;
        EnumWindows(QuickLookWindowCallback, (LPARAM)&search);
    }
    return search.result;
}

static BOOL QuickLookShowsPath(const WCHAR *path)
{
    HWND window = FindQuickLookWindow(TRUE);
    WCHAR title[1024];
    if (!window) return FALSE;
    title[0] = L'\0';
    GetWindowTextW(window, title, 1024);
    return ContainsInsensitive(title, FileNamePart(path));
}

static BOOL InitializePipeName(void)
{
    HANDLE token;
    DWORD bytes = 0;
    TOKEN_USER *user;
    LPWSTR sidText = NULL;
    BOOL success = FALSE;
    if (g_pipeName[0]) return TRUE;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return FALSE;
    GetTokenInformation(token, TokenUser, NULL, 0, &bytes);
    user = (TOKEN_USER *)HeapAlloc(GetProcessHeap(), 0, bytes);
    if (user && GetTokenInformation(token, TokenUser, user, bytes, &bytes)
        && ConvertSidToStringSidW(user->User.Sid, &sidText)) {
        if (_snwprintf_s(g_pipeName, 512, _TRUNCATE,
            L"\\\\.\\pipe\\QuickLook.App.Pipe.%s", sidText) > 0) success = TRUE;
    }
    if (sidText) LocalFree(sidText);
    if (user) HeapFree(GetProcessHeap(), 0, user);
    CloseHandle(token);
    return success;
}

static BOOL SendPipeMessage(const WCHAR *message, const WCHAR *path)
{
    static WCHAR line[PIPE_TEXT_CAPACITY];
    static CHAR bytes[PIPE_BYTES_CAPACITY];
    HANDLE pipe;
    int byteCount;
    DWORD written = 0;
    if (!InitializePipeName()) return FALSE;
    if (!path) path = L"";
    if (_snwprintf_s(line, PIPE_TEXT_CAPACITY, _TRUNCATE,
        L"%s|%s|\n", message, path) <= 0) return FALSE;
    byteCount = WideCharToMultiByte(CP_UTF8, 0, line, -1,
        bytes, PIPE_BYTES_CAPACITY, NULL, NULL);
    if (byteCount <= 1) return FALSE;
    if (!WaitNamedPipeW(g_pipeName, 250)) return FALSE;
    pipe = CreateFileW(g_pipeName, GENERIC_WRITE, 0, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (pipe == INVALID_HANDLE_VALUE) return FALSE;
    if (!WriteFile(pipe, bytes, (DWORD)(byteCount - 1), &written, NULL)) {
        CloseHandle(pipe);
        return FALSE;
    }
    CloseHandle(pipe);
    return written == (DWORD)(byteCount - 1);
}

static BOOL IsQuickLookPipeReady(void)
{
    if (!InitializePipeName()) return FALSE;
    return WaitNamedPipeW(g_pipeName, 0);
}

static void ScheduleQuickLookCoordination(DWORD pid)
{
    BOOL scheduled = FALSE;
    if (!pid || !g_workerEvent) return;
    EnterCriticalSection(&g_actionLock);
    if (pid != g_hookOrderedQuickLookPid && pid != g_quickLookCoordinationPid) {
        g_quickLookCoordinationPid = pid;
        g_quickLookCoordinationPending = TRUE;
        scheduled = TRUE;
    }
    LeaveCriticalSection(&g_actionLock);
    if (!scheduled) return;
    StartQuickLookWatchdog();
    SetEvent(g_workerEvent);
    TraceHookEvent(L"COORDINATE", pid);
}

static void ClearQuickLookCoordination(DWORD pid)
{
    EnterCriticalSection(&g_actionLock);
    if (g_quickLookCoordinationPid == pid) {
        g_quickLookCoordinationPid = 0;
        g_quickLookCoordinationPending = FALSE;
    }
    LeaveCriticalSection(&g_actionLock);
}

static BOOL WaitForQuickLookPipe(DWORD pid)
{
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    ULONGLONG started = GetTickCount64();
    BOOL ready = FALSE;
    while (GetTickCount64() - started < 30000) {
        if (process && WaitForSingleObject(process, 0) == WAIT_OBJECT_0) break;
        if (IsQuickLookPipeReady()) {
            ready = TRUE;
            break;
        }
        Sleep(25);
    }
    if (process) CloseHandle(process);
    return ready;
}

static void CALLBACK QuickLookWinEventCallback(HWINEVENTHOOK hook, DWORD event,
    HWND window, LONG objectId, LONG childId, DWORD eventThread, DWORD eventTime)
{
    WCHAR className[256];
    DWORD pid = 0;
    (void)hook;
    (void)childId;
    (void)eventThread;
    (void)eventTime;
    if (event != EVENT_OBJECT_CREATE || !window || objectId != OBJID_WINDOW) return;
    className[0] = L'\0';
    GetClassNameW(window, className, 256);
    if (wcsncmp(className, L"HwndWrapper[QuickLook.exe",
        wcslen(L"HwndWrapper[QuickLook.exe")) != 0) return;
    GetWindowThreadProcessId(window, &pid);
    if (!pid) return;

    if (pid == g_hookOrderedQuickLookPid) return;
    RefreshQuickLookPids();
    if (IsPidInList(pid, g_quickLookPids, g_quickLookPidCount))
        ScheduleQuickLookCoordination(pid);
}

static void CALLBACK SelectionWinEventCallback(HWINEVENTHOOK hook, DWORD event,
    HWND window, LONG objectId, LONG childId, DWORD eventThread, DWORD eventTime)
{
    PortalView view;
    DWORD pid = 0;
    BOOL explorerWindow;
    WCHAR className[256];
    HWND rootWindow;
    (void)hook;
    (void)childId;
    (void)eventThread;
    (void)eventTime;
    if (!window || objectId != OBJID_CLIENT) return;
    if (event != EVENT_OBJECT_FOCUS && event != EVENT_OBJECT_SELECTION
        && event != EVENT_OBJECT_SELECTIONADD
        && event != EVENT_OBJECT_SELECTIONREMOVE
        && event != EVENT_OBJECT_SELECTIONWITHIN) return;
    if (FindCachedPortalViewForWindow(window, &view)) {
        if (PortalListHasSelection(view.listWindow))
            InterlockedExchangePointer(&g_activePortalWindow, view.listWindow);
        else
            InterlockedExchangePointer(&g_activePortalWindow, NULL);
        return;
    }
    GetWindowThreadProcessId(window, &pid);
    AcquireSRWLockShared(&g_portalCacheLock);
    explorerWindow = IsPidInList(pid, g_explorerPids, g_explorerPidCount);
    ReleaseSRWLockShared(&g_portalCacheLock);
    if (explorerWindow && IsPortalListWindowByBounds(window)) {
        /*
         * Fences recreates the Portal's Explorer list when navigating into a
         * child folder. Rebind that new HWND before classifying its selection;
         * otherwise the still-cached parent HWND makes this look like a native
         * desktop/Explorer selection and Portal Space is incorrectly released.
         */
        RefreshPortalCache();
        if (FindCachedPortalViewForWindow(window, &view)) {
            if (PortalListHasSelection(view.listWindow))
                InterlockedExchangePointer(&g_activePortalWindow, view.listWindow);
            else
                InterlockedExchangePointer(&g_activePortalWindow, NULL);
            return;
        }
    }
    className[0] = L'\0';
    GetClassNameW(window, className, 256);
    rootWindow = GetAncestor(window, GA_ROOT);
    if (!explorerWindow && rootWindow) {
        GetClassNameW(rootWindow, className, 256);
        explorerWindow = ContainsInsensitive(className, L"Fence")
            || ContainsInsensitive(className, L"DesktopDock")
            || _wcsicmp(className, L"CabinetWClass") == 0
            || _wcsicmp(className, L"Progman") == 0
            || _wcsicmp(className, L"WorkerW") == 0
            || _wcsicmp(className, L"SHELLDLL_DefView") == 0;
    }
    if (explorerWindow)
        InterlockedExchangePointer(&g_activePortalWindow, NULL);
}

static BOOL ResolveShortcut(const WCHAR *path, WCHAR *output, int outputCapacity)
{
    IShellLinkW *link = NULL;
    IPersistFile *persist = NULL;
    WIN32_FIND_DATAW data;
    HRESULT result;
    if (_wcsicmp(wcsrchr(path, L'.') ? wcsrchr(path, L'.') : L"", L".lnk") != 0) {
        wcsncpy_s(output, outputCapacity, path, _TRUNCATE);
        return TRUE;
    }
    result = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
        &IID_IShellLinkW, (void **)&link);
    if (FAILED(result)) goto fallback;
    result = IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&persist);
    if (FAILED(result)) goto fallback;
    result = IPersistFile_Load(persist, path, STGM_READ);
    if (FAILED(result)) goto fallback;
    ZeroMemory(&data, sizeof(data));
    result = IShellLinkW_GetPath(link, output, outputCapacity, &data, SLGP_RAWPATH);
    if (SUCCEEDED(result) && IsExistingPath(output)) {
        IPersistFile_Release(persist);
        IShellLinkW_Release(link);
        return TRUE;
    }
fallback:
    if (persist) IPersistFile_Release(persist);
    if (link) IShellLinkW_Release(link);
    wcsncpy_s(output, outputCapacity, path, _TRUNCATE);
    return TRUE;
}

static void ScheduleAction(PreviewAction action, const WCHAR *path)
{
    EnterCriticalSection(&g_actionLock);
    g_action = action;
    ++g_actionGeneration;
    g_actionPortalWindow = NULL;
    if (path) wcsncpy_s(g_actionPath, PATH_CAPACITY, path, _TRUNCATE);
    else g_actionPath[0] = L'\0';
    LeaveCriticalSection(&g_actionLock);
    SetEvent(g_workerEvent);
}

static void SchedulePortalToggle(HWND listWindow, const WCHAR *root)
{
    EnterCriticalSection(&g_actionLock);
    g_action = PreviewAction_PortalToggle;
    ++g_actionGeneration;
    g_actionPortalWindow = listWindow;
    wcsncpy_s(g_actionPath, PATH_CAPACITY, root, _TRUNCATE);
    LeaveCriticalSection(&g_actionLock);
    SetEvent(g_workerEvent);
}

static BOOL IsActionCurrent(LONG generation)
{
    BOOL current;
    EnterCriticalSection(&g_actionLock);
    current = generation == g_actionGeneration;
    LeaveCriticalSection(&g_actionLock);
    return current;
}

static DWORD WINAPI WorkerMain(LPVOID parameter)
{
    WCHAR path[PATH_CAPACITY];
    WCHAR resolved[PATH_CAPACITY];
    (void)parameter;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    for (;;) {
        PreviewAction action;
        LONG generation;
        BOOL coordinate;
        DWORD coordinatePid;
        HWND portalWindow;
        WaitForSingleObject(g_workerEvent, INFINITE);
        EnterCriticalSection(&g_actionLock);
        action = g_action;
        generation = g_actionGeneration;
        wcsncpy_s(path, PATH_CAPACITY, g_actionPath, _TRUNCATE);
        portalWindow = g_actionPortalWindow;
        coordinate = g_quickLookCoordinationPending;
        coordinatePid = g_quickLookCoordinationPid;
        g_quickLookCoordinationPending = FALSE;
        LeaveCriticalSection(&g_actionLock);
        if (action == PreviewAction_Exit) break;
        if (coordinate) {
            if (WaitForQuickLookPipe(coordinatePid)) {
                TraceHookEvent(L"PIPE_READY", coordinatePid);
                if (!PostThreadMessageW(g_mainThreadId, WM_REHOOK_QUICKLOOK,
                    (WPARAM)coordinatePid, 0)) {
                    ClearQuickLookCoordination(coordinatePid);
                    TraceHookEvent(L"REHOOK_POST_FAILED", coordinatePid);
                }
            } else {
                ClearQuickLookCoordination(coordinatePid);
                TraceHookEvent(L"PIPE_WAIT_ENDED", coordinatePid);
            }
            EnterCriticalSection(&g_actionLock);
            action = g_action;
            generation = g_actionGeneration;
            wcsncpy_s(path, PATH_CAPACITY, g_actionPath, _TRUNCATE);
            portalWindow = g_actionPortalWindow;
            LeaveCriticalSection(&g_actionLock);
            if (action == PreviewAction_Exit) break;
        }
        if (action == PreviewAction_PortalToggle) {
            if (!portalWindow || !path[0]) {
                RefreshPortalCache();
            } else {
                WCHAR selectedPath[PATH_CAPACITY];
                ULONGLONG resolveStarted = GetTickCount64();
                selectedPath[0] = L'\0';
                if (ReadPortalSelectionFromView(portalWindow, path,
                    selectedPath, PATH_CAPACITY)) {
                    BOOL closesCurrent;
                    InterlockedExchange((LONG *)&g_lastResolveMilliseconds,
                        (LONG)min(GetTickCount64() - resolveStarted, 0x7FFFFFFF));
                    ResolveShortcut(selectedPath, resolved, PATH_CAPACITY);
                    closesCurrent = QuickLookShowsPath(resolved);
                    if (closesCurrent) {
                        /*
                         * A frozen preview window never processes the Close
                         * message. Detect it on this worker thread (never on the
                         * keyboard hook thread) and tell the user once.
                         */
                        HWND preview = FindQuickLookWindow(TRUE);
                        if (preview && !IsWindowResponsive(preview) && g_trayWindow)
                            PostMessageW(g_trayWindow, WM_NOTIFY_UNRESPONSIVE_QUICKLOOK, 0, 0);
                    }
                    if (IsActionCurrent(generation)) {
                        InterlockedIncrement(&g_portalToggleCount);
                        if (closesCurrent) {
                            InterlockedIncrement(&g_closeCount);
                            SendPipeMessage(L"QuickLook.App.PipeMessages.Close", L"");
                        } else {
                            InterlockedIncrement(&g_invokeCount);
                            if (IsActionCurrent(generation))
                                SendPipeMessage(L"QuickLook.App.PipeMessages.Invoke", resolved);
                        }
                    }
                } else {
                    InterlockedIncrement(&g_resolveFailureCount);
                }
            }
        } else if (action == PreviewAction_Close) {
            if (IsActionCurrent(generation)) {
                InterlockedIncrement(&g_closeCount);
                SendPipeMessage(L"QuickLook.App.PipeMessages.Close", L"");
            }
        }
        EnterCriticalSection(&g_actionLock);
        if (generation == g_actionGeneration) g_action = PreviewAction_None;
        LeaveCriticalSection(&g_actionLock);
    }
    CoUninitialize();
    return 0;
}

static BOOL AnyModifierDown(void)
{
    return (GetAsyncKeyState(VK_SHIFT) & 0x8000)
        || (GetAsyncKeyState(VK_CONTROL) & 0x8000)
        || (GetAsyncKeyState(VK_MENU) & 0x8000)
        || (GetAsyncKeyState(VK_LWIN) & 0x8000)
        || (GetAsyncKeyState(VK_RWIN) & 0x8000);
}

static LRESULT CALLBACK KeyboardCallback(int code, WPARAM message, LPARAM data)
{
    KBDLLHOOKSTRUCT *event = (KBDLLHOOKSTRUCT *)data;
    BOOL isDown;
    BOOL isUp;
    if (code < 0 || !event || event->vkCode != VK_SPACE)
        return CallNextHookEx(g_keyboardHook, code, message, data);
    isDown = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    isUp = message == WM_KEYUP || message == WM_SYSKEYUP;
    if (!isDown && !isUp) return CallNextHookEx(g_keyboardHook, code, message, data);

    if (isDown) {
        HWND portalWindow = NULL;
        WCHAR portalRoot[ROOT_CAPACITY];
        portalRoot[0] = L'\0';
        if (g_spaceDownAt) return g_bridgePress ? 1 : CallNextHookEx(g_keyboardHook, code, message, data);
        g_spaceDownAt = GetTickCount64();
        g_bridgePress = FALSE;
        if (!AnyModifierDown() && IsNativeSelectionContext()
            && TryGetActivePortalView(&portalWindow, portalRoot, ROOT_CAPACITY)) {
            g_bridgePress = TRUE;
            InterlockedIncrement(&g_spacePressCount);
            SchedulePortalToggle(portalWindow, portalRoot);
        }
    } else {
        ULONGLONG held = g_spaceDownAt ? GetTickCount64() - g_spaceDownAt : 0;
        g_spaceDownAt = 0;
        if (!g_bridgePress) return CallNextHookEx(g_keyboardHook, code, message, data);
        g_bridgePress = FALSE;
        if (held >= 750)
            ScheduleAction(PreviewAction_Close, NULL);
        return 1;
    }
    return g_bridgePress ? 1 : CallNextHookEx(g_keyboardHook, code, message, data);
}

static BOOL ReinstallKeyboardHook(HINSTANCE instance, DWORD quickLookPid)
{
    HHOOK replacement;
    HHOOK previous;
    RefreshQuickLookPids();
    if (!IsPidInList(quickLookPid, g_quickLookPids, g_quickLookPidCount)) {
        ClearQuickLookCoordination(quickLookPid);
        TraceHookEvent(L"REHOOK_STALE", quickLookPid);
        return FALSE;
    }
    replacement = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardCallback, instance, 0);
    if (!replacement) {
        ClearQuickLookCoordination(quickLookPid);
        TraceHookEvent(L"REHOOK_FAILED", quickLookPid);
        return FALSE;
    }
    previous = g_keyboardHook;
    g_keyboardHook = replacement;
    g_hookOrderedQuickLookPid = quickLookPid;
    if (previous) UnhookWindowsHookEx(previous);
    ClearQuickLookCoordination(quickLookPid);
    TraceHookEvent(L"REHOOK_OK", quickLookPid);
    return TRUE;
}

static BOOL AddTrayIcon(HWND window)
{
    NOTIFYICONDATAW iconData;
    ZeroMemory(&iconData, sizeof(iconData));
    iconData.cbSize = sizeof(iconData);
    iconData.hWnd = window;
    iconData.uID = TRAY_ICON_ID;
    iconData.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    iconData.uCallbackMessage = WM_TRAY_ICON;
    if (!g_trayIcon) g_trayIcon = LoadApplicationIcon(GetModuleHandleW(NULL),
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    iconData.hIcon = g_trayIcon;
    wcsncpy_s(iconData.szTip, 128,
        L"Fences QuickLook Bridge 3.4.4 - Running", _TRUNCATE);
    return Shell_NotifyIconW(NIM_ADD, &iconData);
}

static void RemoveTrayIcon(HWND window)
{
    NOTIFYICONDATAW iconData;
    ZeroMemory(&iconData, sizeof(iconData));
    iconData.cbSize = sizeof(iconData);
    iconData.hWnd = window;
    iconData.uID = TRAY_ICON_ID;
    Shell_NotifyIconW(NIM_DELETE, &iconData);
}

static void ShowTrayStatus(HWND window)
{
    WCHAR message[512];
    _snwprintf_s(message, 512, _TRUNCATE,
        L"Fences QuickLook Bridge is running.\n\nVersion: 3.4.4\n"
        L"Folder Portals detected: %d\nProcess ID: %lu",
        GetPortalViewCount(), (unsigned long)GetCurrentProcessId());
    MessageBoxW(window, message, L"Fences QuickLook Bridge",
        MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

static void RefreshFromTray(HWND window)
{
    HINSTANCE instance = GetModuleHandleW(NULL);
    RefreshPortalCache();
    RefreshQuickLookPids();
    if (g_quickLookPidCount > 0 && IsQuickLookPipeReady())
        ReinstallKeyboardHook(instance, g_quickLookPids[0]);
    MessageBoxW(window,
        L"Folder Portal mappings and the QuickLook keyboard hook were refreshed.",
        L"Fences QuickLook Bridge", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

static BOOL IsWindowResponsive(HWND window)
{
    DWORD_PTR result = 0;
    if (!window || !IsWindow(window)) return FALSE;
    return SendMessageTimeoutW(window, WM_NULL, 0, 0,
        SMTO_ABORTIFHUNG | SMTO_BLOCK, QUICKLOOK_RESPONSE_TIMEOUT_MS, &result) != 0;
}

static BOOL GetStatusFilePath(WCHAR *path, int capacity)
{
    WCHAR directory[MAX_PATH];
    DWORD length = GetTempPathW(MAX_PATH, directory);
    if (length == 0 || length >= MAX_PATH) return FALSE;
    return _snwprintf_s(path, capacity, _TRUNCATE, L"%s%s",
        directory, STATUS_FILE_NAME) > 0;
}

static void BuildStatusText(WCHAR *output, int capacity)
{
    WCHAR *activeRoot;
    WCHAR *activeItem;
    WCHAR quickLookPids[512];
    WCHAR quickLookTitle[1024];
    HWND active = (HWND)InterlockedCompareExchangePointer(&g_activePortalWindow,
        NULL, NULL);
    HWND quickLook = FindQuickLookWindow(TRUE);
    PROCESS_MEMORY_COUNTERS_EX memory;
    DWORD handles = 0;
    int portalGroups;
    int portalViews;
    int i;
    int offset = 0;
    ULONGLONG uptime = g_startedAtTick ? (GetTickCount64() - g_startedAtTick) / 1000 : 0;

    activeRoot = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        ROOT_CAPACITY * sizeof(WCHAR));
    activeItem = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        ITEM_CAPACITY * sizeof(WCHAR));
    if (!activeRoot || !activeItem) {
        if (activeRoot) HeapFree(GetProcessHeap(), 0, activeRoot);
        if (activeItem) HeapFree(GetProcessHeap(), 0, activeItem);
        wcsncpy_s(output, capacity, L"Fences QuickLook Bridge: status unavailable.\n", _TRUNCATE);
        return;
    }

    if (active && IsWindow(active) && IsWindowVisible(active)) {
        int selectedIndex;
        GetPortalRootForView(active, activeRoot, ROOT_CAPACITY);
        selectedIndex = (int)SendMessageW(active, LVM_GETNEXTITEM_VALUE,
            (WPARAM)-1, LVNI_SELECTED_VALUE);
        if (selectedIndex < 0)
            selectedIndex = (int)SendMessageW(active, LVM_GETNEXTITEM_VALUE,
                (WPARAM)-1, LVNI_FOCUSED_VALUE);
        if (selectedIndex >= 0) {
            RemoteListReader reader;
            if (InitializeRemoteListReader(active, ITEM_CAPACITY, &reader)) {
                ReadRemoteListItemText(&reader, active, selectedIndex, activeItem,
                    ITEM_CAPACITY);
                DisposeRemoteListReader(&reader);
            }
        }
    }

    quickLookPids[0] = L'\0';
    for (i = 0; i < g_quickLookPidCount && offset < 480; ++i) {
        WCHAR entry[32];
        _snwprintf_s(entry, 32, _TRUNCATE, L"%s%lu", i ? L"," : L"",
            (unsigned long)g_quickLookPids[i]);
        wcsncat_s(quickLookPids, 512, entry, _TRUNCATE);
        offset += (int)wcslen(entry);
    }

    ZeroMemory(&memory, sizeof(memory));
    memory.cb = sizeof(memory);
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&memory,
        sizeof(memory));
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    AcquireSRWLockShared(&g_portalCacheLock);
    portalGroups = g_portalGroupCount;
    portalViews = g_portalViewCount;
    ReleaseSRWLockShared(&g_portalCacheLock);
    quickLookTitle[0] = L'\0';
    if (quickLook) GetWindowTextW(quickLook, quickLookTitle, 1024);

    _snwprintf_s(output, capacity, _TRUNCATE,
        L"Fences QuickLook Bridge live status\n"
        L"Version=%s\nProcessId=%lu\nUptimeSeconds=%llu\n"
        L"PortalGroups=%d\nPortalViews=%d\n"
        L"ActivePortal=%p\nActivePortalRoot=%s\nActivePortalItem=%s\n"
        L"LastResolveMs=%lu\nResolveFailures=%ld\n"
        L"QuickLookPids=%s\nQuickLookVisible=%s\nQuickLookResponsive=%s\n"
        L"QuickLookTitle=%s\nQuickLookHookPid=%lu\nQuickLookCoordinationPending=%s\n"
        L"SpacePresses=%ld\nPortalToggles=%ld\nPreviewInvokes=%ld\nPreviewCloses=%ld\n"
        L"UnresponsiveNotices=%ld\n"
        L"WorkingSetMB=%.2f\nPrivateMB=%.2f\nHandles=%lu\n",
        BRIDGE_VERSION, (unsigned long)GetCurrentProcessId(),
        (unsigned long long)uptime,
        portalGroups, portalViews,
        active, activeRoot, activeItem,
        (unsigned long)g_lastResolveMilliseconds, g_resolveFailureCount,
        quickLookPids,
        quickLook ? L"True" : L"False",
        !quickLook ? L"NA" : (IsWindowResponsive(quickLook) ? L"True" : L"False"),
        quickLookTitle,
        (unsigned long)g_hookOrderedQuickLookPid,
        g_quickLookCoordinationPending ? L"True" : L"False",
        g_spacePressCount, g_portalToggleCount, g_invokeCount, g_closeCount,
        g_unresponsiveNotices,
        memory.WorkingSetSize / 1048576.0, memory.PrivateUsage / 1048576.0,
        (unsigned long)handles);

    HeapFree(GetProcessHeap(), 0, activeRoot);
    HeapFree(GetProcessHeap(), 0, activeItem);
}

static BOOL WriteStatusToFile(void)
{
    WCHAR path[MAX_PATH];
    WCHAR *text;
    HANDLE file;
    DWORD written = 0;
    CHAR utf8[16384];
    int byteCount;
    BOOL success;
    if (!GetStatusFilePath(path, MAX_PATH)) return FALSE;
    text = (WCHAR *)VirtualAlloc(NULL, 16384 * sizeof(WCHAR),
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!text) return FALSE;
    BuildStatusText(text, 16384);
    byteCount = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, (int)sizeof(utf8),
        NULL, NULL);
    VirtualFree(text, 0, MEM_RELEASE);
    if (byteCount <= 1) return FALSE;
    file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    success = WriteFile(file, utf8, (DWORD)(byteCount - 1), &written, NULL)
        && written == (DWORD)(byteCount - 1);
    CloseHandle(file);
    return success;
}

static int RunStatusQuery(void)
{
    WCHAR path[MAX_PATH];
    HWND tray;
    DWORD_PTR result = 0;
    HANDLE file;
    CHAR utf8[16384];
    DWORD read = 0;
    int byteCount;
    WCHAR *text;
    if (!GetStatusFilePath(path, MAX_PATH)) return 7;
    DeleteFileW(path);
    tray = FindWindowW(TRAY_WINDOW_CLASS, NULL);
    if (!tray) {
        WriteStdoutText(L"FencesQuickLookBridge is not running.\n");
        return 7;
    }
    if (!SendMessageTimeoutW(tray, WM_BRIDGE_QUERY, 0, 0,
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, &result)) {
        WriteStdoutText(L"FencesQuickLookBridge did not answer the status query.\n");
        return 8;
    }
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        WriteStdoutText(L"FencesQuickLookBridge did not write a status report.\n");
        return 8;
    }
    ReadFile(file, utf8, (DWORD)sizeof(utf8) - 1, &read, NULL);
    CloseHandle(file);
    utf8[read] = '\0';
    byteCount = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (byteCount <= 1) return 8;
    text = (WCHAR *)VirtualAlloc(NULL, (SIZE_T)byteCount * sizeof(WCHAR),
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!text) return 8;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, text, byteCount);
    WriteStdoutText(text);
    VirtualFree(text, 0, MEM_RELEASE);
    return 0;
}

static void ShowUnresponsiveQuickLookNotice(HWND window)
{
    NOTIFYICONDATAW iconData;
    if (InterlockedIncrement(&g_unresponsiveNotices) > 1) return;
    ZeroMemory(&iconData, sizeof(iconData));
    iconData.cbSize = sizeof(iconData);
    iconData.hWnd = window;
    iconData.uID = TRAY_ICON_ID;
    iconData.uFlags = NIF_INFO;
    iconData.dwInfoFlags = NIIF_WARNING;
    iconData.uTimeout = 8000;
    wcsncpy_s(iconData.szInfoTitle, 64, L"QuickLook is not responding", _TRUNCATE);
    wcsncpy_s(iconData.szInfo, 256,
        L"The preview window is frozen. Restart QuickLook, then press Space again.",
        _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &iconData);
}

static void StartQuickLookWatchdog(void)
{
    if (g_quickLookWatchdogRunning || !g_trayWindow) return;
    g_quickLookWatchdogRunning = TRUE;
    g_quickLookWatchdogStartedAt = GetTickCount64();
    SetTimer(g_trayWindow, QUICKLOOK_WATCHDOG_TIMER_ID, QUICKLOOK_WATCHDOG_INTERVAL_MS, NULL);
}

static void StopQuickLookWatchdog(HWND window)
{
    if (!g_quickLookWatchdogRunning) return;
    g_quickLookWatchdogRunning = FALSE;
    KillTimer(window, QUICKLOOK_WATCHDOG_TIMER_ID);
}

/*
 * QuickLook installs its own low level keyboard hook when it starts. The bridge
 * must be installed after QuickLook so that a Portal Space press reaches the
 * bridge first. A QuickLook window-creation event normally triggers the reorder;
 * the watchdog only runs while the bridge is waiting for a starting QuickLook,
 * and it stops as soon as the order is correct. Idle operation stays
 * event-driven: the watchdog is bounded by QUICKLOOK_START_GRACE_MS.
 */
static void QuickLookWatchdogTick(HWND window)
{
    DWORD pid;
    if (g_quickLookPidCount > 0 && g_quickLookPids[0] == g_hookOrderedQuickLookPid) {
        StopQuickLookWatchdog(window);
        return;
    }
    if (GetTickCount64() - g_quickLookWatchdogStartedAt > QUICKLOOK_START_GRACE_MS) {
        StopQuickLookWatchdog(window);
        return;
    }
    /* WaitNamedPipeW with a zero timeout is cheap and needs no process scan. */
    if (!IsQuickLookPipeReady()) return;
    RefreshQuickLookPids();
    if (g_quickLookPidCount <= 0) return;
    pid = g_quickLookPids[0];
    if (pid == g_hookOrderedQuickLookPid) {
        StopQuickLookWatchdog(window);
        return;
    }
    if (ReinstallKeyboardHook(GetModuleHandleW(NULL), pid))
        StopQuickLookWatchdog(window);
}

static void ShowTrayMenu(HWND window)
{
    HMENU menu = CreatePopupMenu();
    POINT cursor;
    UINT command;
    if (!menu) return;
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Running - version 3.4.4");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, TRAY_COMMAND_REFRESH,
        L"Refresh Folder Portals and QuickLook hook");
    AppendMenuW(menu, MF_STRING, TRAY_COMMAND_EXIT, L"Exit");
    GetCursorPos(&cursor);
    SetForegroundWindow(window);
    command = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        cursor.x, cursor.y, 0, window, NULL);
    DestroyMenu(menu);
    PostMessageW(window, WM_NULL, 0, 0);
    if (command) PostMessageW(window, WM_COMMAND, command, 0);
}

static LRESULT CALLBACK TrayWindowCallback(HWND window, UINT message,
    WPARAM wParam, LPARAM lParam)
{
    (void)wParam;
    if (g_taskbarCreatedMessage && message == g_taskbarCreatedMessage) {
        AddTrayIcon(window);
        return 0;
    }
    switch (message) {
        case WM_TRAY_ICON:
            if (lParam == WM_LBUTTONDBLCLK) ShowTrayStatus(window);
            else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU)
                ShowTrayMenu(window);
            return 0;
        case WM_BRIDGE_QUERY:
            WriteStatusToFile();
            return TRUE;
        case WM_NOTIFY_UNRESPONSIVE_QUICKLOOK:
            ShowUnresponsiveQuickLookNotice(window);
            return 0;
        case WM_TIMER:
            if (wParam == QUICKLOOK_WATCHDOG_TIMER_ID) {
                QuickLookWatchdogTick(window);
                return 0;
            }
            return DefWindowProcW(window, message, wParam, lParam);
        case WM_SHOW_TRAY_STATUS:
            ShowTrayStatus(window);
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == TRAY_COMMAND_REFRESH) RefreshFromTray(window);
            else if (LOWORD(wParam) == TRAY_COMMAND_EXIT) DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            RemoveTrayIcon(window);
            g_trayWindow = NULL;
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
    }
}

static HWND CreateTrayWindow(HINSTANCE instance)
{
    WNDCLASSEXW windowClass;
    ZeroMemory(&windowClass, sizeof(windowClass));
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = TrayWindowCallback;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadApplicationIcon(instance,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    windowClass.hIconSm = LoadApplicationIcon(instance,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    windowClass.lpszClassName = TRAY_WINDOW_CLASS;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return NULL;
    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        TRAY_WINDOW_CLASS, L"Fences QuickLook Bridge", WS_OVERLAPPED,
        0, 0, 0, 0, NULL, NULL, instance, NULL);
}

static void WriteStdoutText(const WCHAR *text)
{
    static CHAR bytes[32768];
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    int byteCount;
    DWORD written;
    if (!output || output == INVALID_HANDLE_VALUE || !text) return;
    byteCount = WideCharToMultiByte(CP_UTF8, 0, text, -1,
        bytes, (int)sizeof(bytes), NULL, NULL);
    if (byteCount > 1) WriteFile(output, bytes, (DWORD)(byteCount - 1), &written, NULL);
}

static int RunPortalDump(void)
{
    PortalView views[MAX_PORTAL_VIEWS];
    int viewCount;
    int i;
    RefreshPortalCache();
    viewCount = CopyPortalViews(views, MAX_PORTAL_VIEWS);
    for (i = 0; i < viewCount; ++i) {
        PortalView *view = &views[i];
        WCHAR root[ROOT_CAPACITY];
        WCHAR line[4096];
        int itemCount;
        int selectedIndex;
        int focusedIndex;
        int itemIndex;
        HWND current;
        int level;
        root[0] = L'\0';
        GetPortalRootForView(view->listWindow, root, ROOT_CAPACITY);
        itemCount = (int)SendMessageW(view->listWindow, LVM_GETITEMCOUNT_VALUE, 0, 0);
        selectedIndex = (int)SendMessageW(view->listWindow, LVM_GETNEXTITEM_VALUE,
            (WPARAM)-1, LVNI_SELECTED_VALUE);
        focusedIndex = (int)SendMessageW(view->listWindow, LVM_GETNEXTITEM_VALUE,
            (WPARAM)-1, LVNI_FOCUSED_VALUE);
        _snwprintf_s(line, 4096, _TRUNCATE,
            L"PORTAL[%d] hwnd=%p root=%s items=%d selected=%d focused=%d "
            L"bounds=%ld,%ld,%ld,%ld\n",
            i, view->listWindow, root, itemCount, selectedIndex, focusedIndex,
            view->bounds.left, view->bounds.top, view->bounds.right, view->bounds.bottom);
        WriteStdoutText(line);

        current = view->listWindow;
        for (level = 0; current && level < 10; ++level) {
            WCHAR className[256];
            WCHAR title[1024];
            RECT bounds;
            DWORD pid = 0;
            className[0] = L'\0';
            title[0] = L'\0';
            ZeroMemory(&bounds, sizeof(bounds));
            GetClassNameW(current, className, 256);
            GetWindowTextW(current, title, 1024);
            GetWindowRect(current, &bounds);
            GetWindowThreadProcessId(current, &pid);
            _snwprintf_s(line, 4096, _TRUNCATE,
                L"  WINDOW[%d] hwnd=%p pid=%lu class=%s title=%s bounds=%ld,%ld,%ld,%ld\n",
                level, current, (unsigned long)pid, className, title,
                bounds.left, bounds.top, bounds.right, bounds.bottom);
            WriteStdoutText(line);
            current = GetParent(current);
        }

        for (itemIndex = 0; itemIndex < itemCount && itemIndex < 128; ++itemIndex) {
            WCHAR itemText[ITEM_CAPACITY];
            itemText[0] = L'\0';
            ReadListItemText(view->listWindow, itemIndex, itemText, ITEM_CAPACITY);
            _snwprintf_s(line, 4096, _TRUNCATE,
                L"  ITEM[%d]%s%s text=%s\n", itemIndex,
                itemIndex == selectedIndex ? L" selected" : L"",
                itemIndex == focusedIndex ? L" focused" : L"", itemText);
            WriteStdoutText(line);
        }
    }
    return 0;
}

static int RunNestedResolverTest(const WCHAR *root, const WCHAR *currentDirectory,
    const WCHAR *selectedText)
{
    INITCOMMONCONTROLSEX controls;
    HWND listWindow;
    WCHAR pattern[PATH_CAPACITY];
    WIN32_FIND_DATAW data;
    HANDLE find;
    int itemIndex = 0;
    WCHAR resolved[PATH_CAPACITY];
    WCHAR expected[PATH_CAPACITY];
    WCHAR output[PATH_CAPACITY * 2 + 256];
    ULONGLONG started;
    DWORD elapsed;
    BOOL success;
    RemoteListReader reader;
    ZeroMemory(&controls, sizeof(controls));
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&controls);
    listWindow = CreateWindowExW(0, WC_LISTVIEWW, L"NestedResolverTest",
        WS_POPUP | LVS_ICON, 0, 0, 400, 300, NULL, NULL,
        GetModuleHandleW(NULL), NULL);
    if (!listWindow) return 40;
    if (_snwprintf_s(pattern, PATH_CAPACITY, _TRUNCATE,
        L"%s\\*", currentDirectory) <= 0) {
        DestroyWindow(listWindow);
        return 41;
    }
    find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) {
        DestroyWindow(listWindow);
        return 42;
    }
    do {
        LVITEMW item;
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
            continue;
        ZeroMemory(&item, sizeof(item));
        item.mask = LVIF_TEXT;
        item.iItem = itemIndex++;
        item.pszText = data.cFileName;
        SendMessageW(listWindow, LVM_INSERTITEMW, 0, (LPARAM)&item);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    resolved[0] = L'\0';
    expected[0] = L'\0';
    ResolvePortalItem(currentDirectory, selectedText, expected, PATH_CAPACITY);
    started = GetTickCount64();
    success = InitializeRemoteListReader(listWindow, ITEM_CAPACITY, &reader)
        && ResolvePortalItemForView(listWindow, root, selectedText, &reader,
            resolved, PATH_CAPACITY);
    if (reader.process) DisposeRemoteListReader(&reader);
    elapsed = (DWORD)(GetTickCount64() - started);
    _snwprintf_s(output, PATH_CAPACITY * 2 + 256, _TRUNCATE,
        L"NESTED_RESOLVER_TEST_%s ElapsedMs=%lu Expected=%s Resolved=%s\n",
        success && expected[0] && _wcsicmp(expected, resolved) == 0 ? L"OK" : L"FAIL",
        (unsigned long)elapsed, expected, resolved);
    WriteStdoutText(output);
    DestroyWindow(listWindow);
    return success && expected[0] && _wcsicmp(expected, resolved) == 0 ? 0 : 43;
}

static void TraceHookEvent(const WCHAR *eventName, DWORD pid)
{
    WCHAR line[256];
    if (!g_traceHookOrder) return;
    _snwprintf_s(line, 256, _TRUNCATE, L"HOOK_ORDER %s PID=%lu Tick=%llu\n",
        eventName, (unsigned long)pid, (unsigned long long)GetTickCount64());
    WriteStdoutText(line);
}

static BOOL WaitForQuickLookTitle(const WCHAR *path, DWORD timeoutMilliseconds)
{
    ULONGLONG started = GetTickCount64();
    while (GetTickCount64() - started < timeoutMilliseconds) {
        if (QuickLookShowsPath(path)) return TRUE;
        Sleep(2);
    }
    return QuickLookShowsPath(path);
}

static BOOL WaitForQuickLookHidden(DWORD timeoutMilliseconds)
{
    ULONGLONG started = GetTickCount64();
    while (GetTickCount64() - started < timeoutMilliseconds) {
        if (!FindQuickLookWindow(TRUE)) return TRUE;
        Sleep(2);
    }
    return !FindQuickLookWindow(TRUE);
}

static int CompareDouble(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static int RunNativeFlickerTest(const WCHAR *firstPath, const WCHAR *secondPath)
{
    double latencies[50];
    LARGE_INTEGER frequency;
    int i;
    double total = 0;
    WCHAR output[1024];
    if (!IsExistingPath(firstPath) || !IsExistingPath(secondPath)) return 20;
    RefreshQuickLookPids();
    SendPipeMessage(L"QuickLook.App.PipeMessages.Close", L"");
    if (!WaitForQuickLookHidden(3000)) {
        HWND stuck = FindQuickLookWindow(TRUE);
        _snwprintf_s(output, 1024, _TRUNCATE,
            L"NATIVE_FLICKER_TEST_SKIPPED Reason=QuickLookDidNotClose Responsive=%s\n",
            stuck && IsWindowResponsive(stuck) ? L"True" : L"False");
        WriteStdoutText(output);
        return 25;
    }
    if (!SendPipeMessage(L"QuickLook.App.PipeMessages.Invoke", firstPath)
        || !WaitForQuickLookTitle(firstPath, 5000)) return 21;
    QueryPerformanceFrequency(&frequency);
    for (i = 0; i < 50; ++i) {
        const WCHAR *target = (i % 2 == 0) ? secondPath : firstPath;
        LARGE_INTEGER started, ended;
        QueryPerformanceCounter(&started);
        if (!SendPipeMessage(L"QuickLook.App.PipeMessages.Invoke", target)
            || !WaitForQuickLookTitle(target, 5000)) return 22;
        QueryPerformanceCounter(&ended);
        latencies[i] = (ended.QuadPart - started.QuadPart) * 1000.0 / frequency.QuadPart;
        total += latencies[i];
    }
    for (i = 0; i < 30; ++i) {
        SendPipeMessage(L"QuickLook.App.PipeMessages.Invoke",
            (i % 2 == 0) ? firstPath : secondPath);
        Sleep(3);
    }
    SendPipeMessage(L"QuickLook.App.PipeMessages.Invoke", secondPath);
    if (!WaitForQuickLookTitle(secondPath, 5000)) return 23;
    qsort(latencies, 50, sizeof(double), CompareDouble);
    SendPipeMessage(L"QuickLook.App.PipeMessages.Close", L"");
    if (!WaitForQuickLookHidden(3000)) {
        _snwprintf_s(output, 1024, _TRUNCATE,
            L"NATIVE_FLICKER_TEST_SKIPPED Reason=QuickLookStayedOpenAfterClose\n");
        WriteStdoutText(output);
        return 24;
    }
    _snwprintf_s(output, 1024, _TRUNCATE,
        L"NATIVE_FLICKER_TEST_OK SteadySwitches=50 BurstRequests=31 "
        L"AvgTitleMs=%.1f P95TitleMs=%.1f MaxTitleMs=%.1f\n",
        total / 50.0, latencies[47], latencies[49]);
    WriteStdoutText(output);
    return 0;
}

static int RunPortalWorkerTest(void)
{
    HWND listWindow = NULL;
    WCHAR root[ROOT_CAPACITY];
    WCHAR path[PATH_CAPACITY];
    WCHAR output[PATH_CAPACITY + 256];
    ULONGLONG started;
    DWORD elapsed = 0;
    BOOL success = FALSE;
    int result = 0;

    root[0] = L'\0';
    path[0] = L'\0';
    InitializeCriticalSection(&g_actionLock);
    g_workerEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_workerEvent) {
        DeleteCriticalSection(&g_actionLock);
        return 1;
    }
    InitializePipeName();
    RefreshPortalCache();
    if (!TryGetAnyPortalSelectionDetails(&listWindow, root, ROOT_CAPACITY,
        path, PATH_CAPACITY)) {
        result = 4;
        goto cleanup;
    }

    SendPipeMessage(L"QuickLook.App.PipeMessages.Close", L"");
    started = GetTickCount64();
    while (FindQuickLookWindow(TRUE) && GetTickCount64() - started < 2000) Sleep(10);

    g_workerThread = CreateThread(NULL, 0, WorkerMain, NULL, 0, NULL);
    if (!g_workerThread) {
        result = 1;
        goto cleanup;
    }
    started = GetTickCount64();
    SchedulePortalToggle(listWindow, root);
    while (GetTickCount64() - started < 3000) {
        if (QuickLookShowsPath(path)) {
            success = TRUE;
            elapsed = (DWORD)(GetTickCount64() - started);
            break;
        }
        Sleep(5);
    }
    SendPipeMessage(L"QuickLook.App.PipeMessages.Close", L"");
    ScheduleAction(PreviewAction_Exit, NULL);
    WaitForSingleObject(g_workerThread, 1000);
    CloseHandle(g_workerThread);
    g_workerThread = NULL;
    _snwprintf_s(output, PATH_CAPACITY + 256, _TRUNCATE,
        L"PORTAL_WORKER_TEST_%s TitleMs=%lu Path=%s\n",
        success ? L"OK" : L"FAIL", (unsigned long)elapsed, path);
    WriteStdoutText(output);
    if (!success) result = 5;

cleanup:
    if (g_workerThread) {
        ScheduleAction(PreviewAction_Exit, NULL);
        WaitForSingleObject(g_workerThread, 1000);
        CloseHandle(g_workerThread);
        g_workerThread = NULL;
    }
    CloseHandle(g_workerEvent);
    g_workerEvent = NULL;
    DeleteCriticalSection(&g_actionLock);
    return result;
}

static int RunPortalRebindTest(void)
{
    PortalView views[MAX_PORTAL_VIEWS];
    HWND actual = NULL;
    HWND rebound;
    WCHAR root[ROOT_CAPACITY];
    WCHAR output[ROOT_CAPACITY + 256];
    int viewCount;
    int i;
    BOOL success;
    RefreshPortalCache();
    viewCount = CopyPortalViews(views, MAX_PORTAL_VIEWS);
    for (i = 0; i < viewCount; ++i) {
        if (PortalListHasSelection(views[i].listWindow)) {
            actual = views[i].listWindow;
            break;
        }
    }
    if (!actual) return 44;
    AcquireSRWLockExclusive(&g_portalCacheLock);
    for (i = 0; i < g_portalViewCount; ++i) {
        if (g_portalViews[i].listWindow == actual) {
            g_portalViews[i].listWindow = (HWND)(UINT_PTR)1;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_portalCacheLock);
    InterlockedExchangePointer(&g_activePortalWindow, (PVOID)(UINT_PTR)1);
    SelectionWinEventCallback(NULL, EVENT_OBJECT_SELECTION, actual,
        OBJID_CLIENT, 1, 0, 0);
    rebound = (HWND)InterlockedCompareExchangePointer(&g_activePortalWindow,
        NULL, NULL);
    root[0] = L'\0';
    success = rebound == actual
        && GetPortalRootForView(actual, root, ROOT_CAPACITY)
        && PortalListHasSelection(actual);
    _snwprintf_s(output, ROOT_CAPACITY + 256, _TRUNCATE,
        L"PORTAL_REBIND_TEST_%s Rebound=%p Expected=%p Root=%s\n",
        success ? L"OK" : L"FAIL", rebound, actual, root);
    WriteStdoutText(output);
    return success ? 0 : 45;
}

static int RunCommandLine(int argc, WCHAR **argv)
{
    if (argc < 2) return -1;
    if (_wcsicmp(argv[1], L"--version") == 0) {
        WriteStdoutText(BRIDGE_VERSION L"\n");
        return 0;
    }
    if (_wcsicmp(argv[1], L"--invoke") == 0 && argc > 2)
        return SendPipeMessage(L"QuickLook.App.PipeMessages.Invoke", argv[2]) ? 0 : 2;
    if (_wcsicmp(argv[1], L"--close") == 0)
        return SendPipeMessage(L"QuickLook.App.PipeMessages.Close", L"") ? 0 : 2;
    if (_wcsicmp(argv[1], L"--portal-selection") == 0) {
        WCHAR path[PATH_CAPACITY];
        WCHAR output[PATH_CAPACITY + 4];
        path[0] = L'\0';
        RefreshPortalCache();
        InitializeActivePortalFromFocus();
        if (!TryGetPortalSelection(path, PATH_CAPACITY))
            TryGetAnyPortalSelection(path, PATH_CAPACITY);
        if (path[0]) {
            _snwprintf_s(output, PATH_CAPACITY + 4, _TRUNCATE, L"%s\n", path);
            WriteStdoutText(output);
        }
        return path[0] ? 0 : 4;
    }
    if (_wcsicmp(argv[1], L"--status") == 0)
        return RunStatusQuery();
    if (_wcsicmp(argv[1], L"--quicklook-state") == 0) {
        WCHAR title[1024];
        WCHAR output[1200];
        HWND quickLook;
        RefreshQuickLookPids();
        quickLook = FindQuickLookWindow(TRUE);
        title[0] = L'\0';
        if (quickLook) GetWindowTextW(quickLook, title, 1024);
        _snwprintf_s(output, 1200, _TRUNCATE, L"Visible=%s Title=%s\n",
            quickLook ? L"True" : L"False", title);
        WriteStdoutText(output);
        return quickLook ? 0 : 1;
    }
    if (_wcsicmp(argv[1], L"--portal-worker-test") == 0)
        return RunPortalWorkerTest();
    if (_wcsicmp(argv[1], L"--portal-rebind-test") == 0)
        return RunPortalRebindTest();
    if (_wcsicmp(argv[1], L"--portal-dump") == 0)
        return RunPortalDump();
    if (_wcsicmp(argv[1], L"--nested-resolver-test") == 0 && argc > 4)
        return RunNestedResolverTest(argv[2], argv[3], argv[4]);
    if (_wcsicmp(argv[1], L"--tray-status") == 0) {
        HWND tray = FindWindowW(TRAY_WINDOW_CLASS, NULL);
        NOTIFYICONIDENTIFIER identifier;
        RECT bounds;
        HRESULT result;
        WCHAR output[512];
        ZeroMemory(&identifier, sizeof(identifier));
        ZeroMemory(&bounds, sizeof(bounds));
        identifier.cbSize = sizeof(identifier);
        identifier.hWnd = tray;
        identifier.uID = TRAY_ICON_ID;
        result = tray ? Shell_NotifyIconGetRect(&identifier, &bounds) : E_FAIL;
        _snwprintf_s(output, 512, _TRUNCATE,
            L"TRAY_STATUS Window=%s Icon=%s Bounds=%ld,%ld,%ld,%ld\n",
            tray ? L"True" : L"False", SUCCEEDED(result) ? L"True" : L"False",
            bounds.left, bounds.top, bounds.right, bounds.bottom);
        WriteStdoutText(output);
        return tray && SUCCEEDED(result) ? 0 : 6;
    }
    if (_wcsicmp(argv[1], L"--diagnose") == 0) {
        WCHAR path[PATH_CAPACITY];
        WCHAR anyPath[PATH_CAPACITY];
        WCHAR title[1024];
        HWND quickLook;
        PROCESS_MEMORY_COUNTERS_EX memory;
        DWORD handles = 0;
        int portalGroupCount;
        int portalViewCount;
        WCHAR output[8192];
        path[0] = L'\0';
        anyPath[0] = L'\0';
        title[0] = L'\0';
        ZeroMemory(&memory, sizeof(memory));
        memory.cb = sizeof(memory);
        RefreshPortalCache();
        RefreshQuickLookPids();
        quickLook = FindQuickLookWindow(TRUE);
        if (quickLook) GetWindowTextW(quickLook, title, 1024);
        TryGetPortalSelection(path, PATH_CAPACITY);
        TryGetAnyPortalSelection(anyPath, PATH_CAPACITY);
        AcquireSRWLockShared(&g_portalCacheLock);
        portalGroupCount = g_portalGroupCount;
        portalViewCount = g_portalViewCount;
        ReleaseSRWLockShared(&g_portalCacheLock);
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&memory, sizeof(memory));
        GetProcessHandleCount(GetCurrentProcess(), &handles);
        _snwprintf_s(output, 8192, _TRUNCATE,
            L"Fences QuickLook Bridge native diagnostics\n"
            L"Version=%s\nPortalGroups=%d\nPortalViews=%d\nPortalSelection=%s\n"
            L"PortalAnySelection=%s\n"
            L"QuickLookVisible=%s\nQuickLookTitle=%s\n"
            L"WorkingSetMB=%.2f\nPrivateMB=%.2f\nHandles=%lu\n",
            BRIDGE_VERSION, portalGroupCount, portalViewCount, path, anyPath,
            quickLook ? L"True" : L"False", title,
            memory.WorkingSetSize / 1048576.0,
            memory.PrivateUsage / 1048576.0,
            (unsigned long)handles);
        WriteStdoutText(output);
        return 0;
    }
    if (_wcsicmp(argv[1], L"--benchmark-hook") == 0) {
        const int iterations = 100000;
        LARGE_INTEGER frequency, start, end;
        int nativeContexts = 0;
        int portalHits = 0;
        int i;
        HWND listWindow;
        WCHAR root[ROOT_CAPACITY];
        WCHAR output[1024];
        double milliseconds;
        RefreshPortalCache();
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
        for (i = 0; i < iterations; ++i) {
            if (!IsNativeSelectionContext()) continue;
            ++nativeContexts;
            if (TryGetActivePortalView(&listWindow, root, ROOT_CAPACITY))
                ++portalHits;
        }
        QueryPerformanceCounter(&end);
        milliseconds = (end.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart;
        _snwprintf_s(output, 1024, _TRUNCATE,
            L"HOOK_BENCHMARK Iterations=%d NativeContexts=%d ActivePortalHits=%d "
            L"TotalMs=%.1f AverageUs=%.2f\n",
            iterations, nativeContexts, portalHits,
            milliseconds, milliseconds * 1000.0 / iterations);
        WriteStdoutText(output);
        return 0;
    }
    if (_wcsicmp(argv[1], L"--flicker-test") == 0 && argc > 3)
        return RunNativeFlickerTest(argv[2], argv[3]);
    return -1;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR commandLine, int showCommand)
{
    HANDLE mutex;
    int argc = 0;
    WCHAR **argv;
    int commandResult;
    BOOL traceHookOrder;
    BOOL serviceMode;
    int exitCode = 0;
    MSG message;
    (void)previous;
    (void)commandLine;
    (void)showCommand;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    traceHookOrder = argc > 1
        && _wcsicmp(argv[1], L"--trace-hook-order-service") == 0;
    serviceMode = traceHookOrder;
    g_traceHookOrder = traceHookOrder;
    commandResult = serviceMode ? -1 : RunCommandLine(argc, argv);
    if (argv) LocalFree(argv);
    if (commandResult >= 0) return commandResult;
    mutex = CreateMutexW(NULL, TRUE, L"Local\\FencesQuickLookBridge.SingleInstance");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existingTray = FindWindowW(TRAY_WINDOW_CLASS, NULL);
        if (existingTray) PostMessageW(existingTray, WM_SHOW_TRAY_STATUS, 0, 0);
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    InitializeCriticalSection(&g_actionLock);
    g_startedAtTick = GetTickCount64();
    g_workerEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_workerEvent) {
        DeleteCriticalSection(&g_actionLock);
        CloseHandle(mutex);
        return 1;
    }
    InitializePipeName();
    RefreshPortalCache();
    RefreshQuickLookPids();
    g_mainThreadId = GetCurrentThreadId();
    g_workerThread = CreateThread(NULL, 0, WorkerMain, NULL, 0, NULL);
    if (!g_workerThread) {
        CloseHandle(g_workerEvent);
        DeleteCriticalSection(&g_actionLock);
        CloseHandle(mutex);
        return 1;
    }
    g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardCallback, instance, 0);
    if (!g_keyboardHook) {
        ScheduleAction(PreviewAction_Exit, NULL);
        WaitForSingleObject(g_workerThread, 1000);
        CloseHandle(g_workerThread);
        CloseHandle(g_workerEvent);
        DeleteCriticalSection(&g_actionLock);
        CloseHandle(mutex);
        return 1;
    }
    g_quickLookWinEventHook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE,
        NULL, QuickLookWinEventCallback, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!g_quickLookWinEventHook) TraceHookEvent(L"WINEVENT_FAILED", 0);
    g_selectionWinEventHook = SetWinEventHook(EVENT_OBJECT_FOCUS,
        EVENT_OBJECT_SELECTIONWITHIN, NULL, SelectionWinEventCallback, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    InitializeActivePortalFromFocus();

    RefreshQuickLookPids();
    if (g_quickLookPidCount > 0) {
        DWORD quickLookPid = g_quickLookPids[0];
        if (IsQuickLookPipeReady())
            ReinstallKeyboardHook(instance, quickLookPid);
        else
            ScheduleQuickLookCoordination(quickLookPid);
    } else {
        TraceHookEvent(L"WAITING_FOR_QUICKLOOK", 0);
    }

    g_trayWindow = CreateTrayWindow(instance);
    if (!g_trayWindow || !AddTrayIcon(g_trayWindow)) {
        exitCode = 1;
        if (g_trayWindow) DestroyWindow(g_trayWindow);
        else PostQuitMessage(0);
    } else if (g_hookOrderedQuickLookPid == 0) {
        StartQuickLookWatchdog();
    }

    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        if (message.message == WM_REHOOK_QUICKLOOK) {
            ReinstallKeyboardHook(instance, (DWORD)message.wParam);
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (g_trayWindow) DestroyWindow(g_trayWindow);
    if (g_selectionWinEventHook) UnhookWinEvent(g_selectionWinEventHook);
    if (g_quickLookWinEventHook) UnhookWinEvent(g_quickLookWinEventHook);
    UnhookWindowsHookEx(g_keyboardHook);
    ScheduleAction(PreviewAction_Exit, NULL);
    WaitForSingleObject(g_workerThread, 1000);
    CloseHandle(g_workerThread);
    CloseHandle(g_workerEvent);
    DeleteCriticalSection(&g_actionLock);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return exitCode;
}
