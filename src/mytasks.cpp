/*
 * MyTasks - Lightweight Windows auto-shutdown / scheduled-task service
 * Triggers: login/lock screen, idle timeout, daily schedule, startup delays.
 * Config/log paths: install/CLI -config / -log; default = EXE directory.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <strsafe.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")

static const wchar_t* kServiceName = L"MyTasks";
static const wchar_t* kServiceDisplayName = L"MyTasks Auto Power Service";
static const wchar_t* kConfigFileName = L"config.ini";
static const wchar_t* kLogFileName = L"MyTasks.log";

static SERVICE_STATUS_HANDLE g_StatusHandle = NULL;
static SERVICE_STATUS g_Status = {};
static HANDLE g_StopEvent = NULL;
static LONG g_ActionBusy = 0;
static wchar_t g_ExeDir[MAX_PATH] = {};
static wchar_t g_ConfigPath[MAX_PATH] = {};
static wchar_t g_ConfigDir[MAX_PATH] = {};
static wchar_t g_LogPath[MAX_PATH] = {};

/* Login/lock secure-desktop idle accrual. */
static ULONGLONG g_LoginScreenSinceMs = 0;
static BOOL g_SessionLocked = FALSE;
static BOOL g_IdleActionArmed = TRUE;          /* re-arm after activity resumes */
static BOOL g_LoginScreenActionArmed = TRUE;   /* fire once per visit to login UI */

/* Schedule: last fired local day key YYYYMMDD for each slot index (simple single last stamp). */
static int g_LastScheduleDay = -1;
static int g_LastScheduleHour = -1;
static int g_LastScheduleMinute = -1;

static BOOL IsLoginScreenShowing(void);
static BOOL HasInteractiveUserSession(void);

/* ---------- helpers ---------- */

static void GetExeDirectory(wchar_t* outDir, DWORD cch)
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *slash = L'\0';
    StringCchCopyW(outDir, cch, path);
}

static void GetExePath(wchar_t* outPath, DWORD cch)
{
    GetModuleFileNameW(NULL, outPath, cch);
}

static void BuildPath(wchar_t* out, DWORD cch, const wchar_t* dir, const wchar_t* name)
{
    StringCchPrintfW(out, cch, L"%s\\%s", dir, name);
}

static BOOL IsAbsolutePath(const wchar_t* path)
{
    if (!path || path[0] == L'\0') return FALSE;
    if (path[0] == L'\\' && path[1] == L'\\') return TRUE; /* UNC */
    if (path[0] == L'\\') return TRUE;
    if (path[0] && path[1] == L':') return TRUE;
    return FALSE;
}

static void PathDirName(const wchar_t* path, wchar_t* outDir, DWORD cch)
{
    StringCchCopyW(outDir, cch, path);
    wchar_t* slash = wcsrchr(outDir, L'\\');
    if (slash) *slash = L'\0';
    else StringCchCopyW(outDir, cch, g_ExeDir);
}

static void ExpandAgainstDir(wchar_t* path, DWORD cch, const wchar_t* baseDir)
{
    if (!path[0] || IsAbsolutePath(path))
        return;
    wchar_t abs[MAX_PATH] = {};
    StringCchPrintfW(abs, MAX_PATH, L"%s\\%s", baseDir, path);
    StringCchCopyW(path, cch, abs);
}

static void EnsureParentDirExists(const wchar_t* filePath)
{
    wchar_t dir[MAX_PATH] = {};
    PathDirName(filePath, dir, MAX_PATH);
    if (dir[0] == L'\0')
        return;
    /* SHCreateDirectoryEx-like: create nested dirs */
    wchar_t tmp[MAX_PATH] = {};
    StringCchCopyW(tmp, MAX_PATH, dir);
    size_t len = wcslen(tmp);
    for (size_t i = 0; i < len; ++i) {
        if (tmp[i] != L'\\')
            continue;
        if (i == 0) continue;
        if (i == 2 && tmp[1] == L':') continue; /* skip "C:" */
        tmp[i] = L'\0';
        CreateDirectoryW(tmp, NULL);
        tmp[i] = L'\\';
    }
    CreateDirectoryW(tmp, NULL);
}

static void LogMsg(const wchar_t* fmt, ...)
{
    if (g_LogPath[0] == L'\0')
        BuildPath(g_LogPath, MAX_PATH, g_ExeDir[0] ? g_ExeDir : L".", kLogFileName);

    EnsureParentDirExists(g_LogPath);

    SYSTEMTIME st = {};
    GetLocalTime(&st);

    wchar_t line[1024] = {};
    wchar_t body[900] = {};
    va_list ap;
    va_start(ap, fmt);
    StringCchVPrintfW(body, 900, fmt, ap);
    va_end(ap);

    StringCchPrintfW(line, 1024, L"%04d-%02d-%02d %02d:%02d:%02d  %s\r\n",
                     st.wYear, st.wMonth, st.wDay,
                     st.wHour, st.wMinute, st.wSecond, body);

    HANDLE h = CreateFileW(g_LogPath, FILE_APPEND_DATA, FILE_SHARE_READ,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, NULL, 0, NULL, NULL);
        if (bytes > 1) {
            char* utf8 = (char*)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)bytes);
            if (utf8) {
                WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, bytes, NULL, NULL);
                DWORD written = 0;
                WriteFile(h, utf8, (DWORD)(bytes - 1), &written, NULL);
                HeapFree(GetProcessHeap(), 0, utf8);
            }
        }
        CloseHandle(h);
    }
}

/* Scan entire argv for -config / -log (order-independent with install/test/...). */
static void ScanPathArgs(int argc, wchar_t** argv, wchar_t* configOut, DWORD configCch,
                         wchar_t* logOut, DWORD logCch)
{
    if (!argv) return;
    for (int i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        if ((_wcsicmp(a, L"-config") == 0 || _wcsicmp(a, L"--config") == 0 || _wcsicmp(a, L"/config") == 0) &&
            i + 1 < argc) {
            StringCchCopyW(configOut, configCch, argv[++i]);
            continue;
        }
        if ((_wcsicmp(a, L"-log") == 0 || _wcsicmp(a, L"--log") == 0 || _wcsicmp(a, L"/log") == 0) &&
            i + 1 < argc) {
            StringCchCopyW(logOut, logCch, argv[++i]);
            continue;
        }
    }
}

/* Config/log from CLI args only; otherwise same directory as the EXE. */
static void InitPaths(int argc, wchar_t** argv)
{
    if (g_ExeDir[0] == L'\0')
        GetExeDirectory(g_ExeDir, MAX_PATH);

    wchar_t cliConfig[MAX_PATH] = {};
    wchar_t cliLog[MAX_PATH] = {};
    if (argc > 1 && argv)
        ScanPathArgs(argc, argv, cliConfig, MAX_PATH, cliLog, MAX_PATH);

    if (cliConfig[0])
        StringCchCopyW(g_ConfigPath, MAX_PATH, cliConfig);
    else
        BuildPath(g_ConfigPath, MAX_PATH, g_ExeDir, kConfigFileName);

    ExpandAgainstDir(g_ConfigPath, MAX_PATH, g_ExeDir);
    PathDirName(g_ConfigPath, g_ConfigDir, MAX_PATH);

    if (cliLog[0])
        StringCchCopyW(g_LogPath, MAX_PATH, cliLog);
    else
        BuildPath(g_LogPath, MAX_PATH, g_ExeDir, kLogFileName);

    ExpandAgainstDir(g_LogPath, MAX_PATH, g_ExeDir);
    EnsureParentDirExists(g_LogPath);
}

static void SetServiceState(DWORD state, DWORD accept, DWORD win32Exit, DWORD waitHint)
{
    g_Status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_Status.dwCurrentState = state;
    g_Status.dwControlsAccepted = accept;
    g_Status.dwWin32ExitCode = win32Exit;
    g_Status.dwWaitHint = waitHint;
    g_Status.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : g_Status.dwCheckPoint + 1;
    if (g_StatusHandle)
        SetServiceStatus(g_StatusHandle, &g_Status);
}

static BOOL IniGetBool(const wchar_t* section, const wchar_t* key, BOOL defVal, const wchar_t* ini)
{
    wchar_t buf[32] = {};
    GetPrivateProfileStringW(section, key, defVal ? L"1" : L"0", buf, 32, ini);
    if (_wcsicmp(buf, L"1") == 0 || _wcsicmp(buf, L"true") == 0 || _wcsicmp(buf, L"yes") == 0)
        return TRUE;
    if (_wcsicmp(buf, L"0") == 0 || _wcsicmp(buf, L"false") == 0 || _wcsicmp(buf, L"no") == 0)
        return FALSE;
    return defVal;
}

static UINT IniGetUint(const wchar_t* section, const wchar_t* key, UINT defVal, const wchar_t* ini)
{
    return GetPrivateProfileIntW(section, key, (INT)defVal, ini);
}

static void ExpandRelativePath(wchar_t* path, DWORD cch)
{
    /* Relative paths resolve against the config file directory. */
    const wchar_t* base = g_ConfigDir[0] ? g_ConfigDir : g_ExeDir;
    ExpandAgainstDir(path, cch, base);
}

/* ---------- config ---------- */

struct AppConfig {
    wchar_t scriptPath[MAX_PATH];
    wchar_t scriptArgs[512];
    wchar_t workingDir[MAX_PATH];

    BOOL loginScreenEnabled;
    BOOL loginScreenOnLock;
    BOOL loginScreenOnLogoff;
    BOOL loginScreenOnDisconnect;
    BOOL loginScreenPollDetect;

    BOOL idleEnabled;
    UINT idleMinutes;
    UINT checkIntervalSeconds;
    BOOL countLoginScreen;

    BOOL scheduleEnabled;
    wchar_t scheduleTimes[256]; /* "HH:MM,HH:MM" */
    BOOL scheduleOnlyWhenIdle;
};

static BOOL LoadConfig(AppConfig* cfg)
{
    ZeroMemory(cfg, sizeof(*cfg));

    if (g_ConfigPath[0] == L'\0')
        InitPaths(0, NULL);

    if (GetFileAttributesW(g_ConfigPath) == INVALID_FILE_ATTRIBUTES) {
        LogMsg(L"Config not found: %s", g_ConfigPath);
        return FALSE;
    }

    const wchar_t* iniPath = g_ConfigPath;

    GetPrivateProfileStringW(L"Settings", L"ScriptPath", L"",
                             cfg->scriptPath, MAX_PATH, iniPath);
    GetPrivateProfileStringW(L"Settings", L"ScriptArgs", L"",
                             cfg->scriptArgs, 512, iniPath);
    GetPrivateProfileStringW(L"Settings", L"WorkingDirectory", L"",
                             cfg->workingDir, MAX_PATH, iniPath);

    /* Prefer [LoginScreen]; fall back to legacy [Logoff] */
    {
        wchar_t probe[8] = {};
        GetPrivateProfileStringW(L"LoginScreen", L"Enabled", L"", probe, 8, iniPath);
        if (probe[0] != L'\0') {
            cfg->loginScreenEnabled = IniGetBool(L"LoginScreen", L"Enabled", TRUE, iniPath);
            cfg->loginScreenOnLock = IniGetBool(L"LoginScreen", L"OnLock", TRUE, iniPath);
            cfg->loginScreenOnLogoff = IniGetBool(L"LoginScreen", L"OnLogoff", TRUE, iniPath);
            cfg->loginScreenOnDisconnect = IniGetBool(L"LoginScreen", L"OnDisconnect", TRUE, iniPath);
            cfg->loginScreenPollDetect = IniGetBool(L"LoginScreen", L"PollDetect", TRUE, iniPath);
        } else {
            cfg->loginScreenEnabled = IniGetBool(L"Logoff", L"Enabled", TRUE, iniPath);
            cfg->loginScreenOnLock = TRUE;
            cfg->loginScreenOnLogoff = TRUE;
            cfg->loginScreenOnDisconnect = TRUE;
            cfg->loginScreenPollDetect = TRUE;
        }
    }

    cfg->idleEnabled = IniGetBool(L"Idle", L"Enabled", TRUE, iniPath);
    cfg->idleMinutes = IniGetUint(L"Idle", L"IdleMinutes", 30, iniPath);
    cfg->checkIntervalSeconds = IniGetUint(L"Idle", L"CheckIntervalSeconds", 30, iniPath);
    cfg->countLoginScreen = IniGetBool(L"Idle", L"CountLoginScreen", TRUE, iniPath);

    cfg->scheduleEnabled = IniGetBool(L"Schedule", L"Enabled", FALSE, iniPath);
    GetPrivateProfileStringW(L"Schedule", L"Times", L"",
                             cfg->scheduleTimes, 256, iniPath);
    cfg->scheduleOnlyWhenIdle = IniGetBool(L"Schedule", L"OnlyWhenIdle", FALSE, iniPath);

    if (cfg->checkIntervalSeconds < 5)
        cfg->checkIntervalSeconds = 5;
    if (cfg->checkIntervalSeconds > 3600)
        cfg->checkIntervalSeconds = 3600;
    if (cfg->idleMinutes < 1)
        cfg->idleMinutes = 1;

    if (cfg->scriptPath[0] == L'\0') {
        LogMsg(L"ScriptPath is empty in config.ini");
        return FALSE;
    }

    ExpandRelativePath(cfg->scriptPath, MAX_PATH);
    ExpandRelativePath(cfg->workingDir, MAX_PATH);

    if (cfg->workingDir[0] == L'\0') {
        StringCchCopyW(cfg->workingDir, MAX_PATH, cfg->scriptPath);
        wchar_t* slash = wcsrchr(cfg->workingDir, L'\\');
        if (slash) *slash = L'\0';
        else StringCchCopyW(cfg->workingDir, MAX_PATH, g_ConfigDir[0] ? g_ConfigDir : g_ExeDir);
    }

    return TRUE;
}

/* ---------- process launcher ---------- */

static BOOL EndsWithI(const wchar_t* path, const wchar_t* suffix)
{
    size_t n = wcslen(path);
    size_t m = wcslen(suffix);
    if (n < m) return FALSE;
    return _wcsicmp(path + n - m, suffix) == 0;
}

static BOOL BuildCommandLine(const wchar_t* path, const wchar_t* args, wchar_t* cmdLine, DWORD cch)
{
    if (!path || path[0] == L'\0')
        return FALSE;

    if (EndsWithI(path, L".bat") || EndsWithI(path, L".cmd")) {
        if (args && args[0])
            return SUCCEEDED(StringCchPrintfW(cmdLine, cch, L"cmd.exe /c \"\"%s\" %s\"", path, args));
        return SUCCEEDED(StringCchPrintfW(cmdLine, cch, L"cmd.exe /c \"%s\"", path));
    }
    if (EndsWithI(path, L".ps1")) {
        if (args && args[0])
            return SUCCEEDED(StringCchPrintfW(cmdLine, cch,
                L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"%s\" %s", path, args));
        return SUCCEEDED(StringCchPrintfW(cmdLine, cch,
            L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"%s\"", path));
    }
    if (args && args[0])
        return SUCCEEDED(StringCchPrintfW(cmdLine, cch, L"\"%s\" %s", path, args));
    return SUCCEEDED(StringCchPrintfW(cmdLine, cch, L"\"%s\"", path));
}

/*
 * Launch path/args. waitMs: 0 = do not wait; >0 wait up to that many ms for exit logging.
 */
static BOOL LaunchProgram(const wchar_t* path, const wchar_t* args, const wchar_t* workDir,
                          const wchar_t* reason, DWORD waitMs)
{
    if (!path || path[0] == L'\0') {
        LogMsg(L"Launch [%s]: empty path", reason ? reason : L"?");
        return FALSE;
    }
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        LogMsg(L"Launch [%s]: file not found: %s", reason ? reason : L"?", path);
        return FALSE;
    }

    wchar_t cmdLine[1024] = {};
    if (!BuildCommandLine(path, args, cmdLine, 1024)) {
        LogMsg(L"Launch [%s]: failed to build command line", reason ? reason : L"?");
        return FALSE;
    }

    LogMsg(L"Launch [%s]: %s", reason ? reason : L"?", cmdLine);

    wchar_t workBuf[MAX_PATH] = {};
    const wchar_t* cwd = NULL;
    if (workDir && workDir[0]) {
        cwd = workDir;
    } else {
        StringCchCopyW(workBuf, MAX_PATH, path);
        wchar_t* slash = wcsrchr(workBuf, L'\\');
        if (slash) {
            *slash = L'\0';
            cwd = workBuf;
        }
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(
        NULL, cmdLine, NULL, NULL, FALSE, CREATE_NO_WINDOW,
        NULL, cwd, &si, &pi);

    if (!ok) {
        LogMsg(L"Launch [%s]: CreateProcess failed, error=%lu", reason ? reason : L"?", GetLastError());
        return FALSE;
    }

    if (waitMs > 0) {
        DWORD wait = WaitForSingleObject(pi.hProcess, waitMs);
        if (wait == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            LogMsg(L"Launch [%s]: finished, exitCode=%lu", reason ? reason : L"?", code);
        } else {
            LogMsg(L"Launch [%s]: still running after wait", reason ? reason : L"?");
        }
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;
}

struct ActionJob {
    wchar_t reason[64];
};

static void RunShutdownAction(const wchar_t* reason)
{
    if (InterlockedCompareExchange(&g_ActionBusy, 1, 0) != 0) {
        LogMsg(L"Action already running; skip (%s)", reason ? reason : L"?");
        return;
    }

    AppConfig cfg = {};
    if (!LoadConfig(&cfg)) {
        InterlockedExchange(&g_ActionBusy, 0);
        return;
    }

    LaunchProgram(cfg.scriptPath, cfg.scriptArgs, cfg.workingDir, reason, 120000);
    InterlockedExchange(&g_ActionBusy, 0);
}

static DWORD WINAPI ActionThread(LPVOID param)
{
    ActionJob* job = (ActionJob*)param;
    RunShutdownAction(job ? job->reason : L"unknown");
    if (job) HeapFree(GetProcessHeap(), 0, job);
    return 0;
}

static void TriggerActionAsync(const wchar_t* reason)
{
    ActionJob* job = (ActionJob*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ActionJob));
    if (!job) {
        LogMsg(L"HeapAlloc failed for action job");
        return;
    }
    StringCchCopyW(job->reason, 64, reason ? reason : L"unknown");

    HANDLE th = CreateThread(NULL, 0, ActionThread, job, 0, NULL);
    if (th) {
        CloseHandle(th);
    } else {
        LogMsg(L"CreateThread failed, error=%lu", GetLastError());
        HeapFree(GetProcessHeap(), 0, job);
    }
}

/* ---------- startup delayed programs ---------- */

enum { kMaxStartupTasks = 16 };

struct StartupTask {
    UINT delaySeconds;
    wchar_t path[MAX_PATH];
    wchar_t args[512];
    wchar_t workDir[MAX_PATH];
};

static ULONGLONG g_ServiceStartMs = 0;
static StartupTask g_StartupTasks[kMaxStartupTasks];
static int g_StartupTaskCount = 0;
static HANDLE g_StartupThread = NULL;

static int CompareStartupByDelay(const void* a, const void* b)
{
    const StartupTask* ta = (const StartupTask*)a;
    const StartupTask* tb = (const StartupTask*)b;
    if (ta->delaySeconds < tb->delaySeconds) return -1;
    if (ta->delaySeconds > tb->delaySeconds) return 1;
    return 0;
}

static void LoadStartupTasks(void)
{
    g_StartupTaskCount = 0;
    ZeroMemory(g_StartupTasks, sizeof(g_StartupTasks));

    if (g_ConfigPath[0] == L'\0')
        InitPaths(0, NULL);
    if (GetFileAttributesW(g_ConfigPath) == INVALID_FILE_ATTRIBUTES)
        return;

    const wchar_t* iniPath = g_ConfigPath;

    if (!IniGetBool(L"Startup", L"Enabled", FALSE, iniPath)) {
        LogMsg(L"Startup tasks disabled");
        return;
    }

    for (int i = 1; i <= kMaxStartupTasks; ++i) {
        wchar_t keyPath[64] = {};
        wchar_t keyDelay[64] = {};
        wchar_t keyArgs[64] = {};
        wchar_t keyWd[64] = {};
        StringCchPrintfW(keyPath, 64, L"Task%d_Path", i);
        StringCchPrintfW(keyDelay, 64, L"Task%d_DelaySeconds", i);
        StringCchPrintfW(keyArgs, 64, L"Task%d_Args", i);
        StringCchPrintfW(keyWd, 64, L"Task%d_WorkingDirectory", i);

        StartupTask task = {};
        GetPrivateProfileStringW(L"Startup", keyPath, L"", task.path, MAX_PATH, iniPath);
        if (task.path[0] == L'\0')
            continue;

        task.delaySeconds = IniGetUint(L"Startup", keyDelay, 0, iniPath);
        GetPrivateProfileStringW(L"Startup", keyArgs, L"", task.args, 512, iniPath);
        GetPrivateProfileStringW(L"Startup", keyWd, L"", task.workDir, MAX_PATH, iniPath);

        ExpandRelativePath(task.path, MAX_PATH);
        ExpandRelativePath(task.workDir, MAX_PATH);

        g_StartupTasks[g_StartupTaskCount++] = task;
        LogMsg(L"Startup task%d: delay=%us path=%s", i, task.delaySeconds, task.path);
    }

    if (g_StartupTaskCount > 1)
        qsort(g_StartupTasks, (size_t)g_StartupTaskCount, sizeof(StartupTask), CompareStartupByDelay);

    LogMsg(L"Startup tasks loaded: %d", g_StartupTaskCount);
}

static DWORD WINAPI StartupSchedulerThread(LPVOID)
{
    for (int i = 0; i < g_StartupTaskCount; ++i) {
        ULONGLONG due = g_ServiceStartMs + (ULONGLONG)g_StartupTasks[i].delaySeconds * 1000ULL;
        ULONGLONG now = GetTickCount64();
        if (due > now) {
            DWORD waitMs = (DWORD)((due - now > 0x7FFFFFFFULL) ? 0x7FFFFFFFUL : (due - now));
            if (WaitForSingleObject(g_StopEvent, waitMs) != WAIT_TIMEOUT)
                return 0;
        }

        if (WaitForSingleObject(g_StopEvent, 0) == WAIT_OBJECT_0)
            return 0;

        wchar_t reason[64] = {};
        StringCchPrintfW(reason, 64, L"startup-%d@%us", i + 1, g_StartupTasks[i].delaySeconds);
        /* Do not wait for process exit so later tasks keep accurate timing. */
        LaunchProgram(g_StartupTasks[i].path, g_StartupTasks[i].args,
                      g_StartupTasks[i].workDir, reason, 0);
    }
    LogMsg(L"Startup scheduler finished all tasks");
    return 0;
}

static void StartStartupScheduler(void)
{
    g_ServiceStartMs = GetTickCount64();
    LoadStartupTasks();
    if (g_StartupTaskCount <= 0)
        return;

    g_StartupThread = CreateThread(NULL, 0, StartupSchedulerThread, NULL, 0, NULL);
    if (!g_StartupThread)
        LogMsg(L"Failed to start startup scheduler, error=%lu", GetLastError());
    else
        LogMsg(L"Startup scheduler started (%d task(s))", g_StartupTaskCount);
}

static void StopStartupScheduler(void)
{
    if (g_StartupThread) {
        WaitForSingleObject(g_StartupThread, 5000);
        CloseHandle(g_StartupThread);
        g_StartupThread = NULL;
    }
}

/* ---------- session / idle ---------- */

static BOOL SessionHasUser(DWORD sessionId)
{
    LPWSTR user = NULL;
    DWORD bytes = 0;
    BOOL ok = FALSE;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId,
                                    WTSUserName, &user, &bytes)) {
        if (user && user[0] != L'\0')
            ok = TRUE;
        WTSFreeMemory(user);
    }
    return ok;
}

static BOOL HasInteractiveUserSession(void)
{
    PWTS_SESSION_INFOW sessions = NULL;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count))
        return FALSE;

    BOOL hasUser = FALSE;
    for (DWORD i = 0; i < count; ++i) {
        if (sessions[i].State == WTSActive || sessions[i].State == WTSConnected) {
            if (SessionHasUser(sessions[i].SessionId)) {
                hasUser = TRUE;
                break;
            }
        }
    }
    WTSFreeMemory(sessions);
    return hasUser;
}

/* Local GetLastInputInfo idle seconds (for reportidle / console). */
static DWORD LocalIdleSeconds(void)
{
    LASTINPUTINFO lii = {};
    lii.cbSize = sizeof(lii);
    if (!GetLastInputInfo(&lii))
        return 0;
    DWORD now = GetTickCount();
    DWORD idleMs = (now >= lii.dwTime) ? (now - lii.dwTime) : 0;
    return idleMs / 1000;
}

/*
 * Query idle seconds inside a user session by launching ourselves as that user:
 *   MyTasks.exe reportidle
 * Exit code = idle seconds (capped).
 */
static BOOL QuerySessionIdleSeconds(DWORD sessionId, DWORD* outSeconds)
{
    *outSeconds = 0;

    HANDLE userToken = NULL;
    if (!WTSQueryUserToken(sessionId, &userToken))
        return FALSE;

    HANDLE primary = NULL;
    if (!DuplicateTokenEx(userToken, MAXIMUM_ALLOWED, NULL, SecurityImpersonation,
                          TokenPrimary, &primary)) {
        CloseHandle(userToken);
        return FALSE;
    }
    CloseHandle(userToken);

    wchar_t exePath[MAX_PATH] = {};
    GetExePath(exePath, MAX_PATH);

    wchar_t cmdLine[MAX_PATH + 32] = {};
    StringCchPrintfW(cmdLine, MAX_PATH + 32, L"\"%s\" reportidle", exePath);

    LPVOID env = NULL;
    CreateEnvironmentBlock(&env, primary, FALSE);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.lpDesktop = (LPWSTR)L"winsta0\\default";
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessAsUserW(
        primary,
        NULL,
        cmdLine,
        NULL,
        NULL,
        FALSE,
        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
        env,
        g_ExeDir,
        &si,
        &pi);

    if (env)
        DestroyEnvironmentBlock(env);
    CloseHandle(primary);

    if (!ok)
        return FALSE;

    DWORD wait = WaitForSingleObject(pi.hProcess, 10000);
    DWORD code = 0;
    if (wait == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    *outSeconds = code;
    return TRUE;
}

/*
 * Returns TRUE and idle seconds when measurable.
 * - With interactive users: minimum idle among active sessions with a user.
 * - With no user + CountLoginScreen: seconds since login screen observed.
 */
static BOOL GetEffectiveIdleSeconds(const AppConfig* cfg, DWORD* outSeconds, BOOL* outOnLoginScreen)
{
    *outSeconds = 0;
    *outOnLoginScreen = FALSE;

    PWTS_SESSION_INFOW sessions = NULL;
    DWORD count = 0;
    DWORD minIdle = MAXDWORD;
    BOOL anyUser = FALSE;

    if (WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) {
        for (DWORD i = 0; i < count; ++i) {
            WTS_CONNECTSTATE_CLASS st = sessions[i].State;
            if (st != WTSActive && st != WTSConnected)
                continue;
            if (!SessionHasUser(sessions[i].SessionId))
                continue;

            anyUser = TRUE;
            DWORD idle = 0;
            if (QuerySessionIdleSeconds(sessions[i].SessionId, &idle)) {
                if (idle < minIdle)
                    minIdle = idle;
            }
        }
        WTSFreeMemory(sessions);
    }

    if (anyUser && !IsLoginScreenShowing()) {
        g_LoginScreenSinceMs = 0;
        /* Fallback when WTSQueryUserToken is unavailable (e.g. console idle command). */
        if (minIdle == MAXDWORD)
            minIdle = LocalIdleSeconds();
        *outSeconds = minIdle;
        /* Re-arm idle action once user is active again (idle below ~1 minute). */
        if (*outSeconds < 60)
            g_IdleActionArmed = TRUE;
        return TRUE;
    }

    /* Lock / logoff / switch-user / no desktop: login UI is showing */
    *outOnLoginScreen = TRUE;
    if (!cfg->countLoginScreen)
        return FALSE;

    ULONGLONG now = GetTickCount64();
    if (g_LoginScreenSinceMs == 0)
        g_LoginScreenSinceMs = now;

    DWORD sinceScreen = (DWORD)((now - g_LoginScreenSinceMs) / 1000ULL);
    DWORD inputIdle = (minIdle == MAXDWORD) ? 0 : minIdle;
    *outSeconds = (sinceScreen > inputIdle) ? sinceScreen : inputIdle;
    return TRUE;
}

static void CheckIdleTrigger(const AppConfig* cfg)
{
    if (!cfg->idleEnabled)
        return;

    DWORD idleSec = 0;
    BOOL onLogin = FALSE;
    if (!GetEffectiveIdleSeconds(cfg, &idleSec, &onLogin))
        return;

    UINT needSec = cfg->idleMinutes * 60;
    if (idleSec < needSec) {
        if (idleSec < 60)
            g_IdleActionArmed = TRUE;
        return;
    }

    if (!g_IdleActionArmed) {
        return;
    }

    g_IdleActionArmed = FALSE;
    if (onLogin)
        LogMsg(L"Idle trigger: login screen for %lu sec (threshold %u min)", idleSec, cfg->idleMinutes);
    else
        LogMsg(L"Idle trigger: no input for %lu sec (threshold %u min)", idleSec, cfg->idleMinutes);

    TriggerActionAsync(onLogin ? L"idle-loginscreen" : L"idle");
}

/* ---------- schedule ---------- */

static BOOL ParseHm(const wchar_t* text, int* hour, int* minute)
{
    int h = 0, m = 0;
    if (swscanf(text, L"%d:%d", &h, &m) != 2)
        return FALSE;
    if (h < 0 || h > 23 || m < 0 || m > 59)
        return FALSE;
    *hour = h;
    *minute = m;
    return TRUE;
}

static void CheckScheduleTrigger(const AppConfig* cfg)
{
    if (!cfg->scheduleEnabled)
        return;
    if (cfg->scheduleTimes[0] == L'\0')
        return;

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    int dayKey = st.wYear * 10000 + st.wMonth * 100 + st.wDay;

    wchar_t buf[256] = {};
    StringCchCopyW(buf, 256, cfg->scheduleTimes);

    wchar_t* ctx = NULL;
    for (wchar_t* tok = wcstok_s(buf, L",;", &ctx); tok; tok = wcstok_s(NULL, L",;", &ctx)) {
        while (*tok == L' ' || *tok == L'\t') ++tok;
        wchar_t* end = tok + wcslen(tok);
        while (end > tok && (end[-1] == L' ' || end[-1] == L'\t')) --end;
        *end = L'\0';
        if (*tok == L'\0') continue;

        int h = 0, m = 0;
        if (!ParseHm(tok, &h, &m)) {
            LogMsg(L"Invalid schedule time ignored: %s", tok);
            continue;
        }

        if (st.wHour != h || st.wMinute != m)
            continue;

        if (g_LastScheduleDay == dayKey &&
            g_LastScheduleHour == h &&
            g_LastScheduleMinute == m)
            continue;

        if (cfg->scheduleOnlyWhenIdle) {
            DWORD idleSec = 0;
            BOOL onLogin = FALSE;
            if (!GetEffectiveIdleSeconds(cfg, &idleSec, &onLogin) ||
                idleSec < cfg->idleMinutes * 60) {
                LogMsg(L"Schedule %02d:%02d matched but not idle enough; skip", h, m);
                continue;
            }
        }

        g_LastScheduleDay = dayKey;
        g_LastScheduleHour = h;
        g_LastScheduleMinute = m;

        LogMsg(L"Schedule trigger at %02d:%02d", h, m);
        TriggerActionAsync(L"schedule");
        break; /* one fire per check tick is enough */
    }
}

/* ---------- login / lock screen ---------- */

/*
 * True when the machine is presenting the credential / lock UI (not UAC).
 * Covers: lock, logoff, switch-user, console disconnect, post-failed-shutdown login.
 */
static BOOL IsLoginScreenShowing(void)
{
    if (g_SessionLocked)
        return TRUE;

    DWORD consoleId = WTSGetActiveConsoleSessionId();
    PWTS_SESSION_INFOW sessions = NULL;
    DWORD count = 0;
    BOOL anyActiveUser = FALSE;
    BOOL switchUserUi = FALSE;

    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count))
        return !HasInteractiveUserSession();

    for (DWORD i = 0; i < count; ++i) {
        DWORD id = sessions[i].SessionId;
        WTS_CONNECTSTATE_CLASS st = sessions[i].State;
        if (!SessionHasUser(id))
            continue;

        if (st == WTSActive || st == WTSConnected)
            anyActiveUser = TRUE;

        /* Switch-user: console session still exists but is disconnected. */
        if (consoleId != 0xFFFFFFFF && id == consoleId && st == WTSDisconnected)
            switchUserUi = TRUE;
    }
    WTSFreeMemory(sessions);

    if (!anyActiveUser)
        return TRUE;
    if (switchUserUi)
        return TRUE;
    return FALSE;
}

static void MarkDesktopBack(void)
{
    g_SessionLocked = FALSE;
    g_LoginScreenSinceMs = 0;
    g_IdleActionArmed = TRUE;
    g_LoginScreenActionArmed = TRUE;
}

static BOOL CauseAllowed(const AppConfig* cfg, const wchar_t* cause)
{
    if (_wcsicmp(cause, L"lock") == 0)
        return cfg->loginScreenOnLock;
    if (_wcsicmp(cause, L"logoff") == 0)
        return cfg->loginScreenOnLogoff;
    if (_wcsicmp(cause, L"disconnect") == 0)
        return cfg->loginScreenOnDisconnect;
    if (_wcsicmp(cause, L"poll") == 0)
        return cfg->loginScreenPollDetect;
    return TRUE;
}

static void TryFireLoginScreen(const wchar_t* cause, DWORD sessionId, const AppConfig* cfg)
{
    if (!cfg->loginScreenEnabled) {
        LogMsg(L"LoginScreen trigger disabled (cause=%s session=%lu)", cause, sessionId);
        return;
    }
    if (!CauseAllowed(cfg, cause)) {
        LogMsg(L"LoginScreen cause '%s' disabled in config; skip", cause);
        return;
    }

    /*
     * Pure logoff notification: if another Active user remains, this is not
     * "machine at login UI" (e.g. one of several sessions ended).
     * Lock / disconnect / poll use IsLoginScreenShowing instead.
     */
    if (_wcsicmp(cause, L"logoff") == 0 && HasInteractiveUserSession()) {
        LogMsg(L"Logoff event but another interactive session remains; skip");
        return;
    }

    if (_wcsicmp(cause, L"poll") == 0 || _wcsicmp(cause, L"disconnect") == 0) {
        if (!IsLoginScreenShowing()) {
            LogMsg(L"Cause %s but login UI not showing; skip", cause);
            return;
        }
    }

    if (!g_LoginScreenActionArmed) {
        LogMsg(L"Login screen already handled this visit (cause=%s); skip", cause);
        return;
    }

    g_LoginScreenActionArmed = FALSE;
    g_LoginScreenSinceMs = GetTickCount64();
    g_IdleActionArmed = TRUE;
    if (_wcsicmp(cause, L"lock") == 0)
        g_SessionLocked = TRUE;

    LogMsg(L"Login/lock screen detected via %s (session=%lu); starting action",
           cause, sessionId);
    TriggerActionAsync(cause);
}

static void OnLoginScreenSessionEvent(DWORD sessionId, const wchar_t* cause, const AppConfig* cfg)
{
    LogMsg(L"SESSIONCHANGE: %s (session=%lu)", cause, sessionId);
    if (_wcsicmp(cause, L"lock") == 0)
        g_SessionLocked = TRUE;
    TryFireLoginScreen(cause, sessionId, cfg);
}

static void CheckLoginScreenPoll(const AppConfig* cfg)
{
    if (!cfg->loginScreenEnabled || !cfg->loginScreenPollDetect)
        return;

    if (IsLoginScreenShowing()) {
        if (g_LoginScreenSinceMs == 0)
            g_LoginScreenSinceMs = GetTickCount64();
        TryFireLoginScreen(L"poll", WTSGetActiveConsoleSessionId(), cfg);
    } else {
        if (!g_LoginScreenActionArmed)
            LogMsg(L"Desktop usable again; re-arm login-screen trigger");
        MarkDesktopBack();
    }
}

/* ---------- monitor loop ---------- */

static void MonitorLoop(void)
{
    AppConfig cfg = {};
    if (LoadConfig(&cfg)) {
        LogMsg(L"Config loaded: loginScreen=%d(lock=%d,logoff=%d,disconnect=%d,poll=%d) idle=%d(%umin) schedule=%d times=%s interval=%us",
               cfg.loginScreenEnabled ? 1 : 0,
               cfg.loginScreenOnLock ? 1 : 0,
               cfg.loginScreenOnLogoff ? 1 : 0,
               cfg.loginScreenOnDisconnect ? 1 : 0,
               cfg.loginScreenPollDetect ? 1 : 0,
               cfg.idleEnabled ? 1 : 0, cfg.idleMinutes,
               cfg.scheduleEnabled ? 1 : 0,
               cfg.scheduleTimes[0] ? cfg.scheduleTimes : L"(none)",
               cfg.checkIntervalSeconds);
    } else {
        LogMsg(L"Config load failed at start; will retry");
        cfg.checkIntervalSeconds = 30;
    }

    for (;;) {
        DWORD intervalMs = cfg.checkIntervalSeconds * 1000UL;
        DWORD wait = WaitForSingleObject(g_StopEvent, intervalMs);
        if (wait != WAIT_TIMEOUT)
            break;

        if (!LoadConfig(&cfg))
            continue;

        CheckLoginScreenPoll(&cfg);
        CheckIdleTrigger(&cfg);
        CheckScheduleTrigger(&cfg);
    }
}

/* ---------- SCM ---------- */

static DWORD WINAPI ServiceCtrlHandlerEx(DWORD control, DWORD eventType,
                                         LPVOID eventData, LPVOID /*context*/)
{
    switch (control) {
    case SERVICE_CONTROL_STOP:
        LogMsg(L"Stop requested");
        SetServiceState(SERVICE_STOP_PENDING, 0, NO_ERROR, 3000);
        if (g_StopEvent)
            SetEvent(g_StopEvent);
        return NO_ERROR;

    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;

    case SERVICE_CONTROL_SESSIONCHANGE: {
        DWORD sessionId = 0;
        if (eventData)
            sessionId = ((WTSSESSION_NOTIFICATION*)eventData)->dwSessionId;

        if (eventType == WTS_SESSION_LOCK ||
            eventType == WTS_SESSION_LOGOFF ||
            eventType == WTS_CONSOLE_DISCONNECT ||
            eventType == WTS_REMOTE_DISCONNECT) {
            AppConfig cfg = {};
            if (!LoadConfig(&cfg)) {
                cfg.loginScreenEnabled = TRUE;
                cfg.loginScreenOnLock = TRUE;
                cfg.loginScreenOnLogoff = TRUE;
                cfg.loginScreenOnDisconnect = TRUE;
                cfg.loginScreenPollDetect = TRUE;
            }
            const wchar_t* cause = L"logoff";
            if (eventType == WTS_SESSION_LOCK)
                cause = L"lock";
            else if (eventType == WTS_CONSOLE_DISCONNECT || eventType == WTS_REMOTE_DISCONNECT)
                cause = L"disconnect";
            OnLoginScreenSessionEvent(sessionId, cause, &cfg);
        } else if (eventType == WTS_SESSION_UNLOCK ||
                   eventType == WTS_SESSION_LOGON ||
                   eventType == WTS_CONSOLE_CONNECT ||
                   eventType == WTS_REMOTE_CONNECT) {
            LogMsg(L"SESSIONCHANGE: desktop returning (event=%lu session=%lu)",
                   eventType, sessionId);
            MarkDesktopBack();
        }
        return NO_ERROR;
    }

    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static void WINAPI ServiceMain(DWORD argc, LPWSTR* argv)
{
    GetExeDirectory(g_ExeDir, MAX_PATH);
    InitPaths((int)argc, argv);

    g_StatusHandle = RegisterServiceCtrlHandlerExW(kServiceName, ServiceCtrlHandlerEx, NULL);
    if (!g_StatusHandle)
        return;

    g_StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    SetServiceState(SERVICE_START_PENDING, 0, NO_ERROR, 3000);

    LogMsg(L"Service starting (login/lock / idle / schedule / startup tasks)");
    LogMsg(L"ConfigPath=%s", g_ConfigPath);
    LogMsg(L"LogPath=%s", g_LogPath);

    SetServiceState(SERVICE_RUNNING,
                    SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SESSIONCHANGE,
                    NO_ERROR, 0);

    if (!HasInteractiveUserSession())
        g_LoginScreenSinceMs = GetTickCount64();

    StartStartupScheduler();
    MonitorLoop();
    StopStartupScheduler();

    LogMsg(L"Service stopped");
    SetServiceState(SERVICE_STOPPED, 0, NO_ERROR, 0);

    if (g_StopEvent) {
        CloseHandle(g_StopEvent);
        g_StopEvent = NULL;
    }
}

/* ---------- install / uninstall / tools ---------- */

static BOOL IsElevated(void)
{
    BOOL elevated = FALSE;
    HANDLE token = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elev = {};
        DWORD size = 0;
        if (GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &size))
            elevated = elev.TokenIsElevated;
        CloseHandle(token);
    }
    return elevated;
}

static int InstallService(int argc, wchar_t** argv)
{
    if (!IsElevated()) {
        wprintf(L"Please run as Administrator to install.\n");
        return 1;
    }

    GetExeDirectory(g_ExeDir, MAX_PATH);
    InitPaths(argc, argv);

    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);

    wchar_t cliConfig[MAX_PATH] = {};
    wchar_t cliLog[MAX_PATH] = {};
    ScanPathArgs(argc, argv, cliConfig, MAX_PATH, cliLog, MAX_PATH);
    ExpandAgainstDir(cliConfig, MAX_PATH, g_ExeDir);
    ExpandAgainstDir(cliLog, MAX_PATH, g_ExeDir);

    /* binPath keeps -config/-log so the service process finds custom paths. */
    wchar_t binPath[2048] = {};
    StringCchPrintfW(binPath, 2048, L"\"%s\"", exePath);
    if (cliConfig[0]) {
        wchar_t more[MAX_PATH + 32] = {};
        StringCchPrintfW(more, MAX_PATH + 32, L" -config \"%s\"", cliConfig);
        StringCchCatW(binPath, 2048, more);
    }
    if (cliLog[0]) {
        wchar_t more[MAX_PATH + 32] = {};
        StringCchPrintfW(more, MAX_PATH + 32, L" -log \"%s\"", cliLog);
        StringCchCatW(binPath, 2048, more);
    }

    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        wprintf(L"OpenSCManager failed: %lu\n", GetLastError());
        return 1;
    }

    SC_HANDLE svc = CreateServiceW(
        scm,
        kServiceName,
        kServiceDisplayName,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        binPath,
        NULL, NULL, NULL,
        NULL,   /* LocalSystem */
        NULL);

    if (!svc) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_EXISTS) {
            wprintf(L"Service already exists. Updating binPath / start type...\n");
            svc = OpenServiceW(scm, kServiceName, SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_QUERY_STATUS);
            if (!svc) {
                wprintf(L"OpenService failed: %lu\n", GetLastError());
                CloseServiceHandle(scm);
                return 1;
            }
            if (!ChangeServiceConfigW(svc, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                                      SERVICE_ERROR_NORMAL, binPath, NULL, NULL, NULL,
                                      NULL, NULL, kServiceDisplayName)) {
                wprintf(L"ChangeServiceConfig failed: %lu\n", GetLastError());
                CloseServiceHandle(svc);
                CloseServiceHandle(scm);
                return 1;
            }
        } else {
            wprintf(L"CreateService failed: %lu\n", err);
            CloseServiceHandle(scm);
            return 1;
        }
    }

    SERVICE_DESCRIPTIONW desc = {};
    desc.lpDescription = (LPWSTR)L"Auto shutdown / delayed startup programs. Settings in config.ini.";
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

    SC_ACTION actions[3] = {
        { SC_ACTION_RESTART, 5000 },
        { SC_ACTION_RESTART, 5000 },
        { SC_ACTION_RESTART, 5000 }
    };
    SERVICE_FAILURE_ACTIONSW fa = {};
    fa.dwResetPeriod = 86400;
    fa.cActions = 3;
    fa.lpsaActions = actions;
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

    StartServiceW(svc, 0, NULL);

    wprintf(L"Installed and started as LocalSystem / Automatic.\n");
    wprintf(L"ConfigPath: %ls\n", g_ConfigPath);
    wprintf(L"LogPath:    %ls\n", g_LogPath);
    wprintf(L"Specify paths at install: MyTasks.exe install -config PATH -log PATH\n");

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

static int UninstallService(void)
{
    if (!IsElevated()) {
        wprintf(L"Please run as Administrator to uninstall.\n");
        return 1;
    }

    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) {
        wprintf(L"OpenSCManager failed: %lu\n", GetLastError());
        return 1;
    }

    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (!svc) {
        wprintf(L"OpenService failed: %lu\n", GetLastError());
        CloseServiceHandle(scm);
        return 1;
    }

    SERVICE_STATUS st = {};
    ControlService(svc, SERVICE_CONTROL_STOP, &st);
    Sleep(1000);
    if (!DeleteService(svc)) {
        wprintf(L"DeleteService failed: %lu\n", GetLastError());
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return 1;
    }

    wprintf(L"Service uninstalled.\n");
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

static int ConsoleTest(int argc, wchar_t** argv)
{
    GetExeDirectory(g_ExeDir, MAX_PATH);
    InitPaths(argc, argv);
    LogMsg(L"Console test: running shutdown action once");
    wprintf(L"ConfigPath: %ls\n", g_ConfigPath);
    wprintf(L"LogPath:    %ls\n", g_LogPath);
    wprintf(L"Running configured action once (console test)...\n");
    wprintf(L"WARNING: if ScriptPath is shutdown.exe, the PC may shut down.\n");
    RunShutdownAction(L"test");
    wprintf(L"Done. See log file above.\n");
    return 0;
}

static int ConsoleIdle(int argc, wchar_t** argv)
{
    GetExeDirectory(g_ExeDir, MAX_PATH);
    InitPaths(argc, argv);
    AppConfig cfg = {};
    if (!LoadConfig(&cfg)) {
        wprintf(L"Failed to load config: %ls\n", g_ConfigPath);
        return 1;
    }
    wprintf(L"ConfigPath: %ls\n", g_ConfigPath);
    wprintf(L"LogPath:    %ls\n", g_LogPath);

    DWORD idleSec = 0;
    BOOL onLogin = FALSE;
    BOOL ok = GetEffectiveIdleSeconds(&cfg, &idleSec, &onLogin);

    wprintf(L"Local session GetLastInputInfo idle: %lu sec\n", LocalIdleSeconds());
    if (!ok) {
        wprintf(L"Effective idle: unavailable (no measurable session)\n");
        return 0;
    }
    wprintf(L"Effective idle: %lu sec (%lu min)  loginScreen=%d  threshold=%u min\n",
            idleSec, idleSec / 60, onLogin ? 1 : 0, cfg.idleMinutes);
    wprintf(L"Schedule enabled=%d times=%ls\n",
            cfg.scheduleEnabled ? 1 : 0,
            cfg.scheduleTimes[0] ? cfg.scheduleTimes : L"(none)");
    return 0;
}

/* Exit code = idle seconds for CreateProcessAsUser helper. */
static int ReportIdleMain(void)
{
    DWORD sec = LocalIdleSeconds();
    if (sec > 86400UL)
        sec = 86400UL;
    return (int)sec;
}

static int ConsoleStartup(int argc, wchar_t** argv)
{
    GetExeDirectory(g_ExeDir, MAX_PATH);
    InitPaths(argc, argv);
    LoadStartupTasks();

    wprintf(L"ConfigPath: %ls\n", g_ConfigPath);
    wprintf(L"LogPath:    %ls\n", g_LogPath);
    wprintf(L"Startup enabled tasks: %d (Task1..Task%d)\n", g_StartupTaskCount, kMaxStartupTasks);
    for (int i = 0; i < g_StartupTaskCount; ++i) {
        wprintf(L"  #%d  delay=%u s  path=%ls  args=%ls\n",
                i + 1,
                g_StartupTasks[i].delaySeconds,
                g_StartupTasks[i].path,
                g_StartupTasks[i].args[0] ? g_StartupTasks[i].args : L"(none)");
    }

    if (argc >= 3 && _wcsicmp(argv[2], L"now") == 0) {
        if (g_StartupTaskCount <= 0) {
            wprintf(L"No tasks to run.\n");
            return 0;
        }
        wprintf(L"Launching all startup tasks immediately (delays ignored)...\n");
        for (int i = 0; i < g_StartupTaskCount; ++i) {
            wchar_t reason[64] = {};
            StringCchPrintfW(reason, 64, L"startup-now-%d", i + 1);
            LaunchProgram(g_StartupTasks[i].path, g_StartupTasks[i].args,
                          g_StartupTasks[i].workDir, reason, 30000);
        }
        wprintf(L"Done. See log file.\n");
    } else {
        wprintf(L"Tip: MyTasks.exe startup now  - run all tasks immediately\n");
    }
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc >= 2) {
        if (_wcsicmp(argv[1], L"install") == 0)
            return InstallService(argc, argv);
        if (_wcsicmp(argv[1], L"uninstall") == 0)
            return UninstallService();
        if (_wcsicmp(argv[1], L"test") == 0)
            return ConsoleTest(argc, argv);
        if (_wcsicmp(argv[1], L"idle") == 0)
            return ConsoleIdle(argc, argv);
        if (_wcsicmp(argv[1], L"startup") == 0)
            return ConsoleStartup(argc, argv);
        if (_wcsicmp(argv[1], L"reportidle") == 0)
            return ReportIdleMain();
        if (_wcsicmp(argv[1], L"paths") == 0) {
            GetExeDirectory(g_ExeDir, MAX_PATH);
            InitPaths(argc, argv);
            wprintf(L"ExeDir:     %ls\n", g_ExeDir);
            wprintf(L"ConfigPath: %ls\n", g_ConfigPath);
            wprintf(L"ConfigDir:  %ls\n", g_ConfigDir);
            wprintf(L"LogPath:    %ls\n", g_LogPath);
            return 0;
        }
        wprintf(L"Usage:\n");
        wprintf(L"  MyTasks.exe install [-config PATH] [-log PATH]\n");
        wprintf(L"  MyTasks.exe uninstall|test|idle|startup|paths\n");
        wprintf(L"  Default config/log: same directory as MyTasks.exe\n");
        return 1;
    }

    SERVICE_TABLE_ENTRYW table[] = {
        { (LPWSTR)kServiceName, ServiceMain },
        { NULL, NULL }
    };

    if (!StartServiceCtrlDispatcherW(table)) {
        DWORD err = GetLastError();
        if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            wprintf(L"Not started by SCM. Use: MyTasks.exe install|uninstall|test|idle|startup|paths\n");
        }
        return 1;
    }
    return 0;
}
