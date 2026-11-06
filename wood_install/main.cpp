// Win32 x86 Unicode installer (Windows 7-11 compatible)
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <algorithm>
#include <winhttp.h>

#include "resource.h"

#pragma comment(lib, "Winhttp.lib")

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "Comctl32.lib")

// ---------------- Registry ----------------
static const wchar_t* kCompanyKey = L"Software\\WPanel";
static const wchar_t* kCustomPathsValue = L"CustomPaths";
static const wchar_t* kInstallerLangValue = L"InstallerLang"; // e.g. "ru", "en"

// Language priority and display names
struct LangPriority { const wchar_t* code; int priority; };
static const LangPriority g_langPriorities[] = {
    {L"ru", 1}, {L"en", 2}, {L"by", 3}, {L"uk", 4}, {L"es", 5}, {L"ar", 6},
    {L"bg", 10}, {L"cs", 11}, {L"da", 12}, {L"de", 13}, {L"et", 14}, {L"fi", 15},
    {L"fr", 16}, {L"lt", 17}, {L"lv", 18}, {L"nl", 19}, {L"pl", 20}, {L"pt-br", 21},
    {L"ro", 22}, {L"sv", 23}, {L"id", 30}, {L"ja", 31}, {L"ko", 32}, {L"tr", 33},
    {L"zh", 34}, {L"kz", 40}, {L"uz", 41}
};

// Color brushes for UI
static HBRUSH g_brushBackground = nullptr;   // rgb(230,230,230)
static HBRUSH g_brushInstallBtn = nullptr;   // rgb(255, 200, 120)
static HBRUSH g_brushVideoBtn = nullptr;     // rgb(255, 235, 200)
static HBRUSH g_brushTelegramBtn = nullptr;  // rgb(100, 181, 246)

// Version from wpanel/js/sys.js
static std::wstring g_version = L"unknown";

static std::wstring GetExeDir()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return path;
}

static bool IsRunningElevated()
{
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    bool elevated = false;
    if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &size)) {
        elevated = elevation.TokenIsElevated != 0;
    }
    CloseHandle(hToken);
    return elevated;
}


static bool HasSourceFolder()
{
    std::wstring src = GetExeDir() + L"\\wpanel";
    return PathFileExistsW(src.c_str()) != FALSE;
}


static std::wstring ReadVersionFromSysJs()
{
    std::wstring sysJsPath = GetExeDir() + L"\\wpanel\\js\\sys.js";
    std::wifstream file(sysJsPath);
    if (!file) return L"unknown";
    
    std::wstring line;
    for (int i = 0; i < 10 && std::getline(file, line); i++) {
        // Look for: var version = '3.05.08';
        size_t pos = line.find(L"version");
        if (pos != std::wstring::npos) {
            size_t start = line.find(L"'", pos);
            if (start != std::wstring::npos) {
                size_t end = line.find(L"'", start + 1);
                if (end != std::wstring::npos) {
                    return line.substr(start + 1, end - start - 1);
                }
            }
        }
    }
    return L"unknown";
}

static int GetLangPriority(const std::wstring& code)
{
    for (size_t i = 0; i < _countof(g_langPriorities); i++) {
        if (_wcsicmp(code.c_str(), g_langPriorities[i].code) == 0) {
            return g_langPriorities[i].priority;
        }
    }
    return 999; // default priority for unlisted languages
}

// ---------------- Paths / Logging ----------------

static std::wstring g_logPath;                 // --log: свой файл журнала (по умолчанию рядом с exe)

static void LogLine(const std::wstring& s)
{
    const std::wstring logPath = g_logPath.empty() ? GetExeDir() + L"\\wpanel_installer.log" : g_logPath;
    std::wofstream f(logPath.c_str(), std::ios::app);
    if (!f) return;
    SYSTEMTIME st{}; GetLocalTime(&st);
    f << L"[" << st.wYear << L"-" << st.wMonth << L"-" << st.wDay
        << L" " << st.wHour << L":" << st.wMinute << L":" << st.wSecond << L"] "
        << s << L"\n";
}

static std::wstring GetProgramFiles64Path()
{
    // x86 build, but on x64 OS ProgramW6432 points to "C:\Program Files"
    wchar_t buf[32767]{};
    DWORD n = GetEnvironmentVariableW(L"ProgramW6432", buf, _countof(buf));
    if (n > 0 && n < _countof(buf)) return buf;
    n = GetEnvironmentVariableW(L"ProgramFiles", buf, _countof(buf));
    if (n > 0 && n < _countof(buf)) return buf;
    return L"";
}

static bool EnsureDir(const std::wstring& dir)
{
    if (PathFileExistsW(dir.c_str())) return true;
    int r = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return (r == ERROR_SUCCESS || r == ERROR_FILE_EXISTS || r == ERROR_ALREADY_EXISTS);
}

// Файлы, которые держит Corel (в т. ч. wpanel.gms) или сервер рендера (занятые копировать нельзя). Если файл релиза совпадает с
// установленным побайтно, его не копируем и занятым не считаем: Corel закрывать не нужно.
static bool IsLockableName(const wchar_t* name)
{
    static const wchar_t* kNames[] = { L"proxy.dll", L"proxy32.dll", L"main.dll", L"main32.dll", L"wood_server.exe", L"wpanel.gms" };
    for (const wchar_t* n : kNames) if (_wcsicmp(name, n) == 0) return true;
    return false;
}

static bool FilesIdentical(const std::wstring& a, const std::wstring& b)
{
    HANDLE ha = CreateFileW(a.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (ha == INVALID_HANDLE_VALUE) return false;
    HANDLE hb = CreateFileW(b.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hb == INVALID_HANDLE_VALUE) { CloseHandle(ha); return false; }
    LARGE_INTEGER sa{}, sb{};
    bool same = GetFileSizeEx(ha, &sa) && GetFileSizeEx(hb, &sb) && sa.QuadPart == sb.QuadPart;
    std::vector<char> ba(1 << 16), bb(1 << 16);
    while (same) {
        DWORD na = 0, nb = 0;
        if (!ReadFile(ha, ba.data(), (DWORD)ba.size(), &na, nullptr) || !ReadFile(hb, bb.data(), (DWORD)bb.size(), &nb, nullptr)) { same = false; break; }
        if (na != nb || memcmp(ba.data(), bb.data(), na) != 0) { same = false; break; }
        if (na == 0) break;
    }
    CloseHandle(ha); CloseHandle(hb);
    return same;
}

static bool g_skipIdentical = false;   // установка: одинаковые занятые файлы (IsLockableName) не копируем

// rollback - режим отката: файл с тем же размером и временем записи не трогаем (занятый или нечитаемый
// файл, который установка не успела изменить, не должен ронять откат), сбой одного файла не прерывает остальные.
static bool SameFileStamp(const std::wstring& a, const std::wstring& b)
{
    WIN32_FILE_ATTRIBUTE_DATA x{}, y{};
    if (!GetFileAttributesExW(a.c_str(), GetFileExInfoStandard, &x)) return false;
    if (!GetFileAttributesExW(b.c_str(), GetFileExInfoStandard, &y)) return false;
    return x.nFileSizeHigh == y.nFileSizeHigh && x.nFileSizeLow == y.nFileSizeLow &&
           CompareFileTime(&x.ftLastWriteTime, &y.ftLastWriteTime) == 0;
}

static bool CopyDirRecursive(const std::wstring& src, const std::wstring& dst, bool rollback = false)
{
    if (!PathFileExistsW(src.c_str())) {
        LogLine(L"ERROR: CopyDirRecursive - source missing: " + src);
        return false;
    }
    if (!EnsureDir(dst)) {
        LogLine(L"ERROR: CopyDirRecursive - EnsureDir failed (err " + std::to_wstring(GetLastError()) + L"): " + dst);
        return false;
    }

    std::wstring mask = src + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(mask.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        LogLine(L"ERROR: CopyDirRecursive - FindFirstFile failed (err " + std::to_wstring(GetLastError()) + L"): " + mask);
        return false;
    }

    bool allOk = true;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

        std::wstring s = src + L"\\" + fd.cFileName;
        std::wstring d = dst + L"\\" + fd.cFileName;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!CopyDirRecursive(s, d, rollback)) {
                if (!rollback) { FindClose(h); return false; }
                allOk = false;
            }
        }
        else {
            EnsureDir(dst);
            if (rollback && SameFileStamp(s, d)) continue;
            if (g_skipIdentical && IsLockableName(fd.cFileName) && FilesIdentical(s, d)) { LogLine(L"INFO: unchanged, skipped: " + d); continue; }
            if (!CopyFileW(s.c_str(), d.c_str(), FALSE)) {
                LogLine(L"ERROR: CopyDirRecursive - CopyFile failed (err " + std::to_wstring(GetLastError()) + L"): " + s + L" -> " + d);
                if (!rollback) { FindClose(h); return false; }
                allOk = false;
            }
        }
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return allOk;
}

static void DeleteIfExists(const std::wstring& path)
{
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return;

    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        std::wstring mask = path + L"\\*";
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(mask.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                DeleteIfExists(path + L"\\" + fd.cFileName);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(path.c_str());
    }
    else {
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(path.c_str());
    }
}

static std::wstring GetAppData()
{
    wchar_t path[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, path)))
        return path;
    return L"";
}

// ---------------- Localization ----------------
// Built-in English defaults (fallback)
static std::map<std::wstring, std::wstring> g_en = {
    {L"Title", L"Woodman Docker"},
    {L"FoundVersions", L"Detected CorelDRAW Addons folders:"},
    {L"NotFound", L"(CorelDRAW not found)"},
    {L"InstallButton", L"Install"},
    {L"SaveSettings", L"Backup settings"},
    {L"RestoreSettings", L"Restore from backup..."},
    {L"FullCleanup", L"Full cleanup (with backup)"},
    {L"RestoreDialogTitle", L"Restore from backup"},
    {L"RestoreDialogHint", L"Choose a backup (newest first):"},
    {L"RestoreDialogOk", L"Restore"},
    {L"RestoreDialogCancel", L"Cancel"},
    {L"RestoreConfirm", L"Restore the selected backup? What the backup contains (WPanel files, settings; only settings if the box is ticked) will be erased and replaced with it. The current state is saved as a separate backup first. CorelDRAW must be closed."},
    {L"RestoreDone", L"Restored. Start CorelDRAW."},
    {L"RestoreNoBackups", L"No backups yet. They appear when you install or clean up."},
    {L"RestoreOnlySettings", L"Only settings"},
    {L"BkKindSettings", L"Settings"},
    {L"BkKindUpdate", L"Update"},
    {L"BkKindInstall", L"Install"},
    {L"BkKindCleanup", L"Cleanup"},
    {L"BkKindFailed", L"Failed install"},
    {L"BkKindBeforeRestore", L"Before restore"},
    {L"Exit", L"Exit"},
    {L"SelectFolder", L"Browse..."},
    {L"SelectFolderDialogTitle", L"Select CorelDRAW Addons folder"},
    {L"TargetFolder", L"Target folder:"},
    {L"Language", L"Language:"},
    {L"DisableVBA", L"Disable VBA DelayLoad (recommended)"},
    {L"Note", L"Note: Keep the 'wpanel' folder next to this installer EXE."},
    {L"InstallSuccess", L"Installation completed successfully (v{0})."},
    {L"Error", L"Error:"},
    {L"SettingsSaved", L"Settings backup created."},
    {L"SettingsRestored", L"Settings restored from backup."},
    {L"SettingsNotFound", L"Settings file not found:"},
    {L"BackupNotFound", L"Backup file not found:"},
    {L"CleanupConfirm", L"Remove WPanel from all detected CorelDRAW folders?"},
    {L"CleanupTitle", L"Cleanup"},
    {L"CleanupComplete", L"Full cleanup completed (backup created)."},
    {L"SourceMissing", L"Source folder 'wpanel' was not found next to the installer EXE."},
    {L"InstallDisabled", L"Installation is disabled until the 'wpanel' folder is placed next to the installer."},
    {L"FilesInUse", L"Cannot install: these files are in use by another program:\n{0}\nClose CorelDRAW and run the installation again."},
    {L"FilesInUseRetry", L"These files are in use by another program:\n{0}\nClose CorelDRAW and click Retry."},
    {L"RunCorel", L"Run CorelDRAW"},
    {L"FinishInstall", L"Finish installation"},
    {L"RemoveFolderConfirm", L"Remove folder from the list?\n{0}"},
    {L"RemoveFolderTitle", L"Remove folder"},
    {L"CannotRemoveStandard", L"Only manually added folders can be removed from the list."},
    {L"HintInstall", L"Copy WPanel to the selected CorelDRAW folder and enable the docker."},
    {L"HintBrowse", L"Add your own Addons folder if the needed CorelDRAW version is not in the list."},
    {L"HintBackupSettings", L"Save a backup of WPanel settings (materials, controllers, etc.). It appears in the Restore list."},
    {L"HintRestoreSettings", L"Restore WPanel from a backup (the installer makes them on every install and cleanup). By default everything in the backup is restored; tick Only settings to restore just the settings. CorelDRAW must be closed."},
    {L"HintFullCleanup", L"Remove WPanel and old settings from the selected folder (with a backup). Use before a clean reinstall."},
    {L"HintCorelTweak", L"Tune CorelDRAW for the docker: autosave every 3 minutes, more undo levels, fewer extra dialogs."},
};

static std::map<std::wstring, std::wstring> g_inst; // current language dictionary

static std::wstring T(const wchar_t* key)
{
    auto it = g_inst.find(key);
    if (it != g_inst.end()) return it->second;
    auto it2 = g_en.find(key);
    if (it2 != g_en.end()) return it2->second;
    return key;
}

static std::wstring ReadAllBytesAsWide(const std::wstring& path)
{
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return L"";
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    int wlen = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    if (wlen <= 0) return L"";
    
    std::wstring w(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), &w[0], wlen);
    
    return w;
}

static std::wstring UnescapeJsString(std::wstring s)
{
    std::wstring out;
    out.reserve(s.size());

    for (size_t i = 0; i < s.size(); i++) {
        wchar_t c = s[i];
        if (c == L'\\' && i + 1 < s.size()) {
            wchar_t n = s[++i];
            switch (n) {
            case L'n': out.push_back(L'\n'); break;
            case L'r': out.push_back(L'\r'); break;
            case L't': out.push_back(L'\t'); break;
            case L'\\': out.push_back(L'\\'); break;
            case L'"': out.push_back(L'"'); break;
            case L'\'': out.push_back(L'\''); break;
            default: out.push_back(n); break;
            }
        }
        else {
            out.push_back(c);
        }
    }
    return out;
}

// Line-by-line parser for: inst['Key'] = "Value";
static void ParseInstJs(const std::wstring& content, std::map<std::wstring, std::wstring>& dict)
{
    // Split by newlines and parse line by line
    size_t pos = 0;
    int parsedCount = 0;
    
    while (pos < content.size()) {
        // Find end of line
        size_t eol = content.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = content.size();
        
        std::wstring line = content.substr(pos, eol - pos);
        
        // Skip leading whitespace
        size_t start = 0;
        while (start < line.size() && (line[start] == L' ' || line[start] == L'\t' || line[start] == L'\r')) start++;
        
        // Look for inst[' pattern
        if (start + 6 <= line.size() && line.substr(start, 6) == L"inst['") {
            // Find closing ']
            size_t k2 = line.find(L"']", start + 6);
            if (k2 != std::wstring::npos) {
                std::wstring key = line.substr(start + 6, k2 - (start + 6));
                
                // Find = sign after key
                size_t eq = line.find(L"=", k2);
                if (eq != std::wstring::npos) {
                    // Find opening quote
                    size_t q1 = line.find_first_of(L"\"'", eq);
                    if (q1 != std::wstring::npos) {
                        wchar_t quote = line[q1];
                        
                        // Find closing quote
                        size_t q2 = q1 + 1;
                        bool esc = false;
                        while (q2 < line.size()) {
                            if (esc) { esc = false; q2++; continue; }
                            if (line[q2] == L'\\') { esc = true; q2++; continue; }
                            if (line[q2] == quote) break;
                            q2++;
                        }
                        
                        if (q2 < line.size()) {
                            std::wstring raw = line.substr(q1 + 1, q2 - (q1 + 1));
                            dict[key] = UnescapeJsString(raw);
                            parsedCount++;
                        }
                    }
                }
            }
        }
        
        pos = eol + 1;
    }
}

struct LangFile {
    std::wstring code;     // "ru"
    std::wstring path;     // full path to ru.js
};

static std::vector<LangFile> ScanLangFiles()
{
    std::vector<LangFile> out;
    std::wstring dir = GetExeDir() + L"\\wpanel\\langs";
    std::wstring mask = dir + L"\\*.js";

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(mask.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        std::wstring name = fd.cFileName; // e.g. ru.js
        // skip *_old.js (case-insensitive)
        if (name.size() > 7) {
            const wchar_t* tail = name.c_str() + (name.size() - 7);
            if (_wcsicmp(tail, L"_old.js") == 0) continue;
        }

        // code is filename without extension
        wchar_t base[MAX_PATH]{};
        lstrcpynW(base, name.c_str(), MAX_PATH);
        PathRemoveExtensionW(base);

        std::wstring code = base;
        if (code.empty()) continue;

        LangFile lf;
        lf.code = code;
        lf.path = dir + L"\\" + name;
        out.push_back(lf);

    } while (FindNextFileW(h, &fd));

    FindClose(h);

    // Sort by priority
    std::sort(out.begin(), out.end(), [](const LangFile& a, const LangFile& b) {
        int priorA = GetLangPriority(a.code);
        int priorB = GetLangPriority(b.code);
        if (priorA != priorB) return priorA < priorB;
        return _wcsicmp(a.code.c_str(), b.code.c_str()) < 0;
    });

    return out;
}

static std::wstring GetRegistryString(HKEY root, const wchar_t* subkey, const wchar_t* valueName)
{
    HKEY hKey{};
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) return L"";

    DWORD type = 0, cb = 0;
    if (RegQueryValueExW(hKey, valueName, nullptr, &type, nullptr, &cb) != ERROR_SUCCESS || type != REG_SZ || cb < sizeof(wchar_t)) {
        RegCloseKey(hKey);
        return L"";
    }

    std::wstring buf(cb / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(hKey, valueName, nullptr, &type, (LPBYTE)buf.data(), &cb) != ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return L"";
    }
    RegCloseKey(hKey);
    return std::wstring(buf.c_str());
}

static void SetRegistryString(HKEY root, const wchar_t* subkey, const wchar_t* valueName, const std::wstring& value)
{
    HKEY hKey{};
    RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr);
    RegSetValueExW(hKey, valueName, 0, REG_SZ, (const BYTE*)value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
}

static std::wstring GetSystemIsoLang2()
{
    // ISO 639-1, like "ru", "en"
    wchar_t buf[16]{};
    if (GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SISO639LANGNAME, buf, 16) > 0) {
        return buf;
    }
    return L"en";
}

// ---------------- Custom paths in HKCU\Software\WPanel ----------------
static std::vector<std::wstring> LoadCustomPaths()
{
    std::vector<std::wstring> out;
    HKEY hKey{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kCompanyKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return out;

    DWORD type = 0, cb = 0;
    if (RegQueryValueExW(hKey, kCustomPathsValue, nullptr, &type, nullptr, &cb) == ERROR_SUCCESS &&
        type == REG_SZ && cb > sizeof(wchar_t))
    {
        std::wstring buf(cb / sizeof(wchar_t), L'\0');
        if (RegQueryValueExW(hKey, kCustomPathsValue, nullptr, &type, (LPBYTE)buf.data(), &cb) == ERROR_SUCCESS) {
            std::wstring s = buf.c_str();
            size_t pos = 0;
            while (true) {
                size_t next = s.find(L'*', pos);
                std::wstring token = (next == std::wstring::npos) ? s.substr(pos) : s.substr(pos, next - pos);
                if (!token.empty()) out.push_back(token);
                if (next == std::wstring::npos) break;
                pos = next + 1;
            }
        }
    }
    RegCloseKey(hKey);
    return out;
}

static void SaveCustomPathAppendUnique(const std::wstring& path)
{
    std::vector<std::wstring> current = LoadCustomPaths();
    for (auto& p : current) {
        if (_wcsicmp(p.c_str(), path.c_str()) == 0) return;
    }
    current.push_back(path);

    std::wstring joined;
    for (size_t i = 0; i < current.size(); i++) {
        if (i) joined += L"*";
        joined += current[i];
    }

    HKEY hKey{};
    RegCreateKeyExW(HKEY_CURRENT_USER, kCompanyKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr);
    RegSetValueExW(hKey, kCustomPathsValue, 0, REG_SZ, (const BYTE*)joined.c_str(),
        (DWORD)((joined.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
}

static void RemoveCustomPath(const std::wstring& path)
{
    std::vector<std::wstring> current = LoadCustomPaths();
    current.erase(std::remove_if(current.begin(), current.end(), [&](const std::wstring& p) {
        return _wcsicmp(p.c_str(), path.c_str()) == 0;
        }), current.end());

    HKEY hKey{};
    if (current.empty()) {
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kCompanyKey, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
            RegDeleteValueW(hKey, kCustomPathsValue);
            RegCloseKey(hKey);
        }
        return;
    }

    std::wstring joined;
    for (size_t i = 0; i < current.size(); i++) {
        if (i) joined += L"*";
        joined += current[i];
    }

    RegCreateKeyExW(HKEY_CURRENT_USER, kCompanyKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr);
    RegSetValueExW(hKey, kCustomPathsValue, 0, REG_SZ, (const BYTE*)joined.c_str(),
        (DWORD)((joined.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
}

// ---------------- VBA DelayLoad tweak ----------------
static bool DisableVBADelay(int corelVer)
{
    wchar_t subkey[512]{};
    wsprintfW(subkey, L"Software\\Corel\\CorelDRAW\\%d.0\\Draw\\Application Preferences\\VBA", corelVer);

    HKEY hKey{};
    LONG regCreateResult = RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr);
    if (regCreateResult != ERROR_SUCCESS) {
        LogLine(L"ERROR: DisableVBADelay - Failed to create registry key for v" + std::to_wstring(corelVer) + 
                L" (error: " + std::to_wstring(regCreateResult) + L")");
        return false;
    }

    const wchar_t* value = L"0";
    LONG r = RegSetValueExW(hKey, L"DelayLoadVBA", 0, REG_SZ, (const BYTE*)value, (DWORD)(2 * sizeof(wchar_t)));
    RegCloseKey(hKey);
    
    if (r == ERROR_SUCCESS) {
        LogLine(L"SUCCESS: DisableVBADelay - DelayLoadVBA = 0 for CorelDRAW v" + std::to_wstring(corelVer));
    } else {
        LogLine(L"ERROR: DisableVBADelay - Failed to set registry value for v" + std::to_wstring(corelVer) + 
                L" (error: " + std::to_wstring(r) + L")");
    }
    
    return (r == ERROR_SUCCESS);
}

// ---------------- IE11 browser emulation for CorelDRW.exe ----------------
// The docker is an embedded IE (MSHTML WebBrowser ActiveX), and wpanel.html has
// no <!DOCTYPE>, so without a forced mode IE renders it in quirks. 0x2AF9 (11001)
// = IE11 forced edge mode regardless of DOCTYPE, matching IEEmulateMode="11001"
// in wpanel/AppUI.xslt. FEATURE_BROWSER_EMULATION keys the value by the process
// executable name; CorelDRAW's is CorelDRW.exe. HKCU wins over HKLM and needs no
// admin rights.
static const wchar_t* kBrowserEmulationKey =
    L"Software\\Microsoft\\Internet Explorer\\Main\\FeatureControl\\FEATURE_BROWSER_EMULATION";
static const wchar_t* kCorelExeValue = L"CorelDRW.exe";
static const DWORD    kIE11ForcedMode = 0x2AF9; // 11001

static bool SetBrowserEmulation()
{
    HKEY hKey{};
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, kBrowserEmulationKey, 0, nullptr, 0,
        KEY_SET_VALUE, nullptr, &hKey, nullptr);
    if (rc != ERROR_SUCCESS) {
        LogLine(L"ERROR: SetBrowserEmulation - RegCreateKeyEx failed (error: " + std::to_wstring(rc) + L")");
        return false;
    }

    DWORD mode = kIE11ForcedMode;
    LONG r = RegSetValueExW(hKey, kCorelExeValue, 0, REG_DWORD, (const BYTE*)&mode, sizeof(mode));
    RegCloseKey(hKey);

    if (r == ERROR_SUCCESS) {
        LogLine(L"SUCCESS: SetBrowserEmulation - CorelDRW.exe = 11001 (IE11 forced edge mode)");
    } else {
        LogLine(L"ERROR: SetBrowserEmulation - RegSetValueEx failed (error: " + std::to_wstring(r) + L")");
    }
    return (r == ERROR_SUCCESS);
}

static void RemoveBrowserEmulation()
{
    HKEY hKey{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kBrowserEmulationKey, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        LogLine(L"INFO: RemoveBrowserEmulation - FEATURE_BROWSER_EMULATION key not present.");
        return;
    }
    LONG r = RegDeleteValueW(hKey, kCorelExeValue);
    RegCloseKey(hKey);
    if (r == ERROR_SUCCESS)
        LogLine(L"SUCCESS: RemoveBrowserEmulation - CorelDRW.exe value deleted.");
    else if (r == ERROR_FILE_NOT_FOUND)
        LogLine(L"INFO: RemoveBrowserEmulation - CorelDRW.exe value was not set.");
    else
        LogLine(L"ERROR: RemoveBrowserEmulation - RegDeleteValue failed (error: " + std::to_wstring(r) + L")");
}

// ---------------- Detect Corel Addons folders ----------------
struct FoundPath {
    std::wstring label;       // e.g. "2024 (v25) Programs64"
    int corelVer;             // 25 or 0 if unknown
    std::wstring addonsPath;  // ...\Addons
    bool isStandard;          // from known list (true) vs from saved custom paths (false)
};

static void AddIfExists(std::vector<FoundPath>& out,
    const std::wstring& label, int ver, const std::wstring& path, bool isStandard)
{
    if (!PathFileExistsW(path.c_str())) return;
    for (auto& x : out) {
        if (_wcsicmp(x.addonsPath.c_str(), path.c_str()) == 0) return;
    }
    out.push_back({ label, ver, path, isStandard });
}

static std::vector<FoundPath> DetectCorelAddons()
{
    const std::wstring PF = GetProgramFiles64Path();
    auto P = [&](const std::wstring& tail) { return PF + tail; };

    struct Check { const wchar_t* year; int ver; const wchar_t* base; };
    const Check bases[] = {
        {L"2026", 27, L"\\Corel\\CorelDRAW Graphics Suite\\27"},
        {L"2026", 27, L"\\Corel\\CorelDRAW Technical Suite\\27"},
        {L"2025", 26, L"\\Corel\\CorelDRAW Graphics Suite\\26"},
        {L"2025", 26, L"\\Corel\\CorelDRAW Technical Suite\\26"},
        {L"2024", 25, L"\\Corel\\CorelDRAW Graphics Suite\\25"},
        {L"2024", 25, L"\\Corel\\CorelDRAW Technical Suite\\25"},
        {L"2022", 24, L"\\Corel\\CorelDRAW Graphics Suite 2022"},
        {L"2022", 24, L"\\Corel\\CorelDRAW Technical Suite 2022"},
        {L"2021", 23, L"\\Corel\\CorelDRAW Graphics Suite 2021"},
        {L"2021", 23, L"\\Corel\\CorelDRAW Technical Suite 2021"},
        {L"2020", 22, L"\\Corel\\CorelDRAW Graphics Suite 2020"},
        {L"2020", 22, L"\\Corel\\CorelDRAW Technical Suite 2020"},
        {L"2019", 21, L"\\Corel\\CorelDRAW Graphics Suite 2019"},
        {L"2019", 21, L"\\Corel\\CorelDRAW Technical Suite 2019"},
        {L"2018", 20, L"\\Corel\\CorelDRAW Graphics Suite 2018"},
        {L"2018", 20, L"\\Corel\\CorelDRAW Technical Suite 2018"},
        {L"2017", 19, L"\\Corel\\CorelDRAW Graphics Suite 2017"},
        {L"2017", 19, L"\\Corel\\CorelDRAW Technical Suite 2017"},
        {L"X8",   18, L"\\Corel\\CorelDRAW Graphics Suite X8"},
        {L"X7",   17, L"\\Corel\\CorelDRAW Graphics Suite X7"},
    };

    std::vector<FoundPath> out;

    for (auto& b : bases) {
        // Programs64\Addons
        {
            std::wstring tail = std::wstring(b.base) + L"\\Programs64\\Addons";
            wchar_t buf[128]{};
            wsprintfW(buf, L"%s (v%d)", b.year, b.ver);
            AddIfExists(out, buf, b.ver, P(tail), true);
        }
        // Programs\Addons
        {
            std::wstring tail = std::wstring(b.base) + L"\\Programs\\Addons";
            wchar_t buf[128]{};
            wsprintfW(buf, L"%s (v%d)", b.year, b.ver);
            AddIfExists(out, buf, b.ver, P(tail), true);
        }
    }

    // Saved custom paths
    for (auto& cp : LoadCustomPaths()) {
        AddIfExists(out, L"Custom", 0, cp, false);
    }

    return out;
}

// ---------------- UI Helpers ----------------
static std::wstring BrowseForFolder(HWND owner)
{
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = T(L"SelectFolderDialogTitle").c_str();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return L"";

    wchar_t path[MAX_PATH]{};
    std::wstring result;
    if (SHGetPathFromIDListW(pidl, path)) result = path;

    CoTaskMemFree(pidl);
    return result;
}

static void SetDlgText(HWND hDlg, int id, const std::wstring& text)
{
    SetDlgItemTextW(hDlg, id, text.c_str());
}

static std::wstring GetDlgText(HWND hDlg, int id)
{
    wchar_t buf[2048]{};
    GetDlgItemTextW(hDlg, id, buf, 2048);
    return buf;
}

// Итоговое окно установки с двумя кнопками: "Да" -> "Запустить CorelDRAW", "Нет" -> "Закончить установку"
// (подписи подменяет CBT-хук: стандартный MessageBox их не умеет, а манифеста comctl32 v6 для TaskDialog нет).
static HHOOK g_btnHook = nullptr;
static LRESULT CALLBACK BtnCaptionHook(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HCBT_ACTIVATE) {
        HWND h = (HWND)wParam;
        SetDlgItemTextW(h, IDYES, T(L"RunCorel").c_str());
        SetDlgItemTextW(h, IDNO, T(L"FinishInstall").c_str());
        // Кнопки MessageBox рассчитаны на короткие подписи: растягиваем под текст и расширяем окно.
        HWND bY = GetDlgItem(h, IDYES), bN = GetDlgItem(h, IDNO);
        if (bY && bN) {
            HDC dc = GetDC(bY);
            HFONT old = (HFONT)SelectObject(dc, (HFONT)SendMessageW(bY, WM_GETFONT, 0, 0));
            SIZE a{}, b{};
            std::wstring ty = T(L"RunCorel"), tn = T(L"FinishInstall");
            GetTextExtentPoint32W(dc, ty.c_str(), (int)ty.size(), &a);
            GetTextExtentPoint32W(dc, tn.c_str(), (int)tn.size(), &b);
            SelectObject(dc, old); ReleaseDC(bY, dc);
            RECT ry, rn, rw;
            GetWindowRect(bY, &ry); GetWindowRect(bN, &rn); GetWindowRect(h, &rw);
            POINT py{ry.left, ry.top}, pn{rn.left, rn.top};
            ScreenToClient(h, &py); ScreenToClient(h, &pn);
            int bh = ry.bottom - ry.top, gap = 8, margin = 12;
            int bw = std::max<int>(std::max<int>(a.cx, b.cx) + 32, ry.right - ry.left);
            int need = 2 * bw + gap + 2 * margin;
            RECT rc; GetClientRect(h, &rc);
            int cw = rc.right - rc.left;
            if (need > cw) {
                SetWindowPos(h, nullptr, rw.left - (need - cw) / 2, rw.top, (rw.right - rw.left) + (need - cw), rw.bottom - rw.top, SWP_NOZORDER);
                cw = need;
            }
            int x0 = (cw - (2 * bw + gap)) / 2;
            SetWindowPos(bY, nullptr, x0, py.y, bw, bh, SWP_NOZORDER);
            SetWindowPos(bN, nullptr, x0 + bw + gap, pn.y, bw, bh, SWP_NOZORDER);
        }
        UnhookWindowsHookEx(g_btnHook); g_btnHook = nullptr;
        return 0;
    }
    return CallNextHookEx(g_btnHook, code, wParam, lParam);
}
// Corel запускаем через explorer.exe: установщик работает от администратора, а Corel от администратора не нужен
// (другой профиль, не работает перетаскивание файлов).
static void LaunchCorelFor(const std::wstring& addonsPath)
{
    size_t cut = addonsPath.find_last_of(L'\\');
    if (cut == std::wstring::npos) return;
    const std::wstring exe = addonsPath.substr(0, cut) + L"\\CorelDRW.exe";
    if (!PathFileExistsW(exe.c_str())) { LogLine(L"INFO: CorelDRW.exe not found: " + exe); return; }
    ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"\"" + exe + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}
static void ShowDoneAskRun(HWND hDlg, const std::wstring& text, const std::wstring& addonsPath)
{
    size_t cut = addonsPath.find_last_of(L'\\');
    const bool haveExe = cut != std::wstring::npos && PathFileExistsW((addonsPath.substr(0, cut) + L"\\CorelDRW.exe").c_str());
    if (!haveExe) { MessageBoxW(hDlg, text.c_str(), T(L"Title").c_str(), MB_OK | MB_ICONINFORMATION); return; }
    g_btnHook = SetWindowsHookExW(WH_CBT, BtnCaptionHook, nullptr, GetCurrentThreadId());
    int r = MessageBoxW(hDlg, text.c_str(), T(L"Title").c_str(), MB_YESNO | MB_ICONINFORMATION);
    if (g_btnHook) { UnhookWindowsHookEx(g_btnHook); g_btnHook = nullptr; }
    if (r == IDYES) LaunchCorelFor(addonsPath);
}

static void ShowMsg(HWND hDlg, const std::wstring& text, UINT icon = MB_ICONINFORMATION)
{
    MessageBoxW(hDlg, text.c_str(), T(L"Title").c_str(), MB_OK | icon);
}

static std::wstring MakeTimestampFolderName()
{
    SYSTEMTIME st{}; GetLocalTime(&st);
    wchar_t buf[64]{};
    swprintf(buf, _countof(buf), L"%04d%02d%02d_%02d%02d%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}



// ---------------- Globals ----------------
static std::vector<FoundPath> g_found;

static std::vector<LangFile> g_langFiles;
static int g_currentLangIndex = -1; // index in g_langFiles, -1 => built-in English
static bool g_hasEnFile = false;    // true if en.js file exists in loaded languages



// ---------------- Settings backup/restore ----------------
static std::wstring g_settingsDirOverride;     // --settings-dir (стенд): вместо %APPDATA%\\wpanel

static std::wstring SettingsPath()
{
    if (!g_settingsDirOverride.empty()) return g_settingsDirOverride;
    std::wstring ad = GetAppData();
    if (ad.empty()) return L"";
    return ad + L"\\wpanel";
}

static void UpdateWindowTitle(HWND hDlg)
{
    std::wstring title = T(L"Title") + L" v" + g_version + L" - Installation";
    SetWindowTextW(hDlg, title.c_str());
}

// ---------------- Tooltips ----------------
static HWND g_hwndTip = nullptr;

static void SetTooltip(HWND hDlg, int ctrlId, const std::wstring& text)
{
    if (!g_hwndTip || text.empty()) return;
    HWND hCtrl = GetDlgItem(hDlg, ctrlId);
    if (!hCtrl) return;

    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.hwnd = hDlg;
    ti.uId = (UINT_PTR)hCtrl;

    SendMessageW(g_hwndTip, TTM_DELTOOLW, 0, (LPARAM)&ti);

    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.lpszText = const_cast<LPWSTR>(text.c_str());
    SendMessageW(g_hwndTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

static void ApplyTooltips(HWND hDlg)
{
    SetTooltip(hDlg, IDC_BTN_INSTALL, T(L"HintInstall"));
    SetTooltip(hDlg, IDC_BTN_BROWSE, T(L"HintBrowse"));
    SetTooltip(hDlg, IDC_BTN_BACKUP_SETTINGS, T(L"HintBackupSettings"));
    SetTooltip(hDlg, IDC_BTN_RESTORE_SETTINGS, T(L"HintRestoreSettings"));
    SetTooltip(hDlg, IDC_BTN_FULL_CLEANUP, T(L"HintFullCleanup"));
    SetTooltip(hDlg, IDC_BTN_COREL_TWEAK, T(L"HintCorelTweak"));
}

static void ApplyUiText(HWND hDlg)
{
    UpdateWindowTitle(hDlg);

    SetDlgItemTextW(hDlg, IDC_STATIC_FOUND, T(L"FoundVersions").c_str());
    SetDlgItemTextW(hDlg, IDC_BTN_INSTALL, T(L"InstallButton").c_str());
    SetDlgItemTextW(hDlg, IDC_BTN_BROWSE, T(L"SelectFolder").c_str());

    SetDlgItemTextW(hDlg, IDC_BTN_BACKUP_SETTINGS, T(L"SaveSettings").c_str());
    SetDlgItemTextW(hDlg, IDC_BTN_RESTORE_SETTINGS, T(L"RestoreSettings").c_str());
    SetDlgItemTextW(hDlg, IDC_BTN_FULL_CLEANUP, T(L"FullCleanup").c_str());
    SetDlgItemTextW(hDlg, IDC_BTN_EXIT, T(L"Exit").c_str());

    SetDlgItemTextW(hDlg, IDC_STATIC_LANG, T(L"Language").c_str());

    // Right side
    SetDlgItemTextW(hDlg, IDC_STATIC_INSTR_TITLE, T(L"Instructions").c_str());
    std::wstring instr = T(L"InstructionText");
    // Ensure proper line endings for EDITTEXT (convert \n to \r\n)
    std::wstring formatted;
    for (size_t i = 0; i < instr.size(); i++) {
        if (instr[i] == L'\n' && (i == 0 || instr[i-1] != L'\r')) {
            formatted += L"\r\n";
        } else {
            formatted += instr[i];
        }
    }
    SetDlgItemTextW(hDlg, IDC_EDIT_INSTRUCTION, formatted.c_str());

    SetDlgItemTextW(hDlg, IDC_BTN_VIDEO, T(L"VideoInstruction").c_str()); // или "Video"

    ApplyTooltips(hDlg);
}


// ---------------- Detect list ----------------
static void RefreshDetectedList(HWND hDlg)
{
    g_found = DetectCorelAddons();

    HWND hList = GetDlgItem(hDlg, IDC_LIST_PATHS);
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);

    for (auto& it : g_found) {
        std::wstring line = it.label + L"  ->  " + it.addonsPath;
        SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)line.c_str());
    }

    if (!g_found.empty()) {
        SendMessageW(hList, LB_SETCURSEL, 0, 0);
        SetDlgText(hDlg, IDC_EDIT_TARGET, g_found[0].addonsPath);
    }
}

// ---------------- Language load/select ----------------
static bool LoadLanguageByIndex(int idx)
{
    g_inst = g_en; // start from English fallback

    if (idx < 0 || idx >= (int)g_langFiles.size()) {
        g_currentLangIndex = -1;
        return true;
    }

    std::wstring content = ReadAllBytesAsWide(g_langFiles[idx].path);
    if (content.empty()) {
        LogLine(L"ERROR: Content is empty for " + g_langFiles[idx].path);
        return false;
    }
    
    std::map<std::wstring, std::wstring> dict;
    ParseInstJs(content, dict);
    for (auto& kv : dict) g_inst[kv.first] = kv.second;

    //LogLine(L"Language loaded: " + g_langFiles[idx].code + L" (" + std::to_wstring(dict.size()) + L" keys)");

    g_currentLangIndex = idx;
    return true;
}

static int FindLangIndexByCode(const std::wstring& code)
{
    for (int i = 0; i < (int)g_langFiles.size(); i++) {
        if (_wcsicmp(g_langFiles[i].code.c_str(), code.c_str()) == 0) return i;
    }
    return -1;
}

static std::wstring LoadSavedLangCode()
{
    return GetRegistryString(HKEY_CURRENT_USER, kCompanyKey, kInstallerLangValue);
}

static void SaveLangCode(const std::wstring& code)
{
    SetRegistryString(HKEY_CURRENT_USER, kCompanyKey, kInstallerLangValue, code);
}

static void FillLanguageCombo(HWND hDlg)
{
    HWND hCmb = GetDlgItem(hDlg, IDC_CMB_LANG);
    SendMessageW(hCmb, CB_RESETCONTENT, 0, 0);

    // Check if en.js file exists in loaded languages
    g_hasEnFile = false;
    for (int i = 0; i < (int)g_langFiles.size(); i++) {
        if (_wcsicmp(g_langFiles[i].code.c_str(), L"en") == 0) {
            g_hasEnFile = true;
            break;
        }
    }

    // If no en.js file, add built-in English
    if (!g_hasEnFile) {
        LoadLanguageByIndex(-1); // Load built-in English
        std::wstring enName = T(L"Name");
        if (enName == L"Name") enName = L"English"; // Fallback if Name key not found
        
        // Item 0: English (built-in)
        SendMessageW(hCmb, CB_ADDSTRING, 0, (LPARAM)enName.c_str());
        SendMessageW(hCmb, CB_SETITEMDATA, 0, (LPARAM)-1);
    }

    for (int i = 0; i < (int)g_langFiles.size(); i++) {
        // Load language temporarily to get inst['Name']
        LoadLanguageByIndex(i);
        std::wstring label = T(L"Name");
        if (label == L"Name" || label.empty()) {
            label = g_langFiles[i].code; // Fallback to language code
        }
        std::wstring display = label + L" (" + g_langFiles[i].code + L")";
        SendMessageW(hCmb, CB_ADDSTRING, 0, (LPARAM)display.c_str());
        // store index in item data
        SendMessageW(hCmb, CB_SETITEMDATA, (g_hasEnFile ? i : i + 1), (LPARAM)i);
    }
}

static void SelectInitialLanguage(HWND hDlg)
{
    // Priority:
    // 1) saved user choice
    // 2) system ISO language (ru/en/...)
    // 3) English fallback

    std::wstring saved = LoadSavedLangCode();
    std::wstring sys = GetSystemIsoLang2();

    int targetIdx = -1;

    if (!saved.empty()) {
        targetIdx = FindLangIndexByCode(saved);
        if (targetIdx >= 0) LogLine(L"Lang from registry: " + saved);
    }

    if (targetIdx < 0) {
        targetIdx = FindLangIndexByCode(sys);
        if (targetIdx >= 0) LogLine(L"Lang from system: " + sys);
    }

    // apply
    HWND hCmb = GetDlgItem(hDlg, IDC_CMB_LANG);
    if (targetIdx >= 0) {
        LoadLanguageByIndex(targetIdx);
        // If en.js exists, language items start at 0. Otherwise at 1 (after built-in English)
        int comboIdx = g_hasEnFile ? targetIdx : targetIdx + 1;
        SendMessageW(hCmb, CB_SETCURSEL, comboIdx, 0);
    }
    else {
        LoadLanguageByIndex(-1);
        SendMessageW(hCmb, CB_SETCURSEL, 0, 0);
    }
}

// ---------------- Corel Tweaks ----------------
static bool SetCorelRegistryValue(const wchar_t* path, const wchar_t* valueName, const wchar_t* value)
{
    HKEY hKey{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        LogLine(L"ERROR: ApplyCorelTweaks - Cannot open key: " + std::wstring(path));
        return false;
    }

    LONG r = RegSetValueExW(hKey, valueName, 0, REG_SZ, (const BYTE*)value, (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);

    if (r == ERROR_SUCCESS) {
        LogLine(L"SUCCESS: Set " + std::wstring(path) + L" \\ " + std::wstring(valueName) + L" = " + std::wstring(value));
    } else {
        LogLine(L"ERROR: Failed to set " + std::wstring(valueName) + L" (error: " + std::to_wstring(r) + L")");
    }

    return (r == ERROR_SUCCESS);
}

static void ApplyCorelTweaks(HWND hDlg, int corelVer)
{
    // Version-independent: the docker's IE11 emulation is keyed by exe name.
    SetBrowserEmulation();

    if (corelVer <= 0) {
        ShowMsg(hDlg, L"Cannot apply tweaks: CorelDRAW version is unknown.", MB_ICONWARNING);
        LogLine(L"INFO: ApplyCorelTweaks - Version unknown, only browser emulation applied.");
        return;
    }

    wchar_t basePath[512]{};
    wsprintfW(basePath, L"Software\\Corel\\CorelDRAW\\%d.0\\Draw\\Application Preferences", corelVer);

    LogLine(L"INFO: ApplyCorelTweaks - Applying tweaks for CorelDRAW v" + std::to_wstring(corelVer));

    // Display settings
    {
        wchar_t path[512]{};
        wsprintfW(path, L"%s\\Options - Display", basePath);
        SetCorelRegistryValue(path, L"OutlineSelectedObjects", L"1");
    }

    // Advanced settings
    {
        wchar_t path[512]{};
        wsprintfW(path, L"%s\\Options - Advanced", basePath);
        SetCorelRegistryValue(path, L"AutoBackupMins", L"3");
        SetCorelRegistryValue(path, L"EmbedFonts", L"0");
        SetCorelRegistryValue(path, L"MakeBackupOnSave", L"0");
    }

    // New Documents Settings Dialog
    {
        wchar_t path[512]{};
        wsprintfW(path, L"%s\\New Documents Settings Dialog", basePath);
        SetCorelRegistryValue(path, L"ShowNewDocumentDialog", L"0");
    }

    // General settings
    {
        wchar_t path[512]{};
        wsprintfW(path, L"%s\\Options - General", basePath);
        SetCorelRegistryValue(path, L"UndoLevels", L"333");
    }

    // Tools settings
    {
        wchar_t path[512]{};
        wsprintfW(path, L"%s\\Options - Tools", basePath);
        SetCorelRegistryValue(path, L"NodeSizePreset", L"1");
    }

    LogLine(L"INFO: ApplyCorelTweaks - Completed for v" + std::to_wstring(corelVer));
    ShowMsg(hDlg, L"Corel tweaks applied successfully!", MB_ICONINFORMATION);
}

// ---------------- Install / Cleanup ----------------
// A DLL loaded by CorelDRAW cannot be opened for writing (sharing violation).
// srcRoot - папка wpanel релиза: одинаковые с ним файлы занятыми не считаются (их не копируют).
static std::wstring FindLockedFiles(const std::wstring& dstRoot, const std::wstring& srcRoot = L"")
{
    // wood_server.exe - сервер рендера: пока он работает, копирование падает на нём посреди установки
    static const wchar_t* kFiles[] = { L"proxy.dll", L"proxy32.dll", L"bin\\main.dll", L"bin\\main32.dll", L"bin\\wood_server.exe", L"wpanel.gms" };
    std::wstring locked;
    for (const wchar_t* rel : kFiles) {
        const std::wstring p = dstRoot + L"\\" + rel;
        if (!PathFileExistsW(p.c_str())) continue;
        if (!srcRoot.empty() && FilesIdentical(srcRoot + L"\\" + rel, p)) continue;
        HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); continue; }
        DWORD err = GetLastError();
        LogLine(L"INFO: FindLockedFiles - cannot open for write (err " + std::to_wstring(err) + L"): " + p);
        if (err == ERROR_SHARING_VIOLATION) locked += L"  " + std::wstring(rel) + L"\n";
    }
    return locked;
}

// Сервер просмотра рендера (bin\wood_server.exe) закрывается командой /quit: порт и токен лежат в
// <Документы>\woodman\render\viewer.session ("порт токен"). Ждём, пока он отпустит exe (до 3 с).
static void StopViewerServer(const std::wstring& dstRoot)
{
    wchar_t docs[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs))) return;
    std::wstring f = std::wstring(docs) + L"\\woodman\\render\\viewer.session";
    HANDLE h = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[512]{}; DWORD n = 0;
    ReadFile(h, buf, sizeof buf - 1, &n, nullptr);
    CloseHandle(h);
    std::string t(buf, n);
    int port = atoi(t.c_str());
    size_t sp = t.find(' ');
    if (port <= 0 || sp == std::string::npos) return;
    std::string tk = t.substr(sp + 1);
    while (!tk.empty() && (tk.back() == '\r' || tk.back() == '\n' || tk.back() == ' ')) tk.pop_back();
    if (tk.empty() || tk.size() > 128) return;
    std::wstring path = L"/quit?k=" + std::wstring(tk.begin(), tk.end());

    HINTERNET s = WinHttpOpen(L"wood_install", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
    if (!s) return;
    WinHttpSetTimeouts(s, 2000, 2000, 2000, 2000);
    if (HINTERNET c = WinHttpConnect(s, L"127.0.0.1", (INTERNET_PORT)port, 0)) {
        if (HINTERNET r = WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, nullptr, nullptr, 0)) {
            if (WinHttpSendRequest(r, nullptr, 0, nullptr, 0, 0, 0)) WinHttpReceiveResponse(r, nullptr);
            WinHttpCloseHandle(r);
            LogLine(L"INFO: viewer server asked to quit (port " + std::to_wstring(port) + L")");
        }
        WinHttpCloseHandle(c);
    }
    WinHttpCloseHandle(s);
    const std::wstring exe = dstRoot + L"\\bin\\wood_server.exe";
    for (int i = 0; i < 10 && PathFileExistsW(exe.c_str()); i++) {
        HANDLE w = CreateFileW(exe.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (w != INVALID_HANDLE_VALUE) { CloseHandle(w); break; }
        Sleep(300);
    }
}

// ---------------- Backups (одна схема для установки, очистки и восстановления) ----------------
// Корень: %APPDATA%\wpanel_backups (НЕ внутри %APPDATA%\wpanel: "Полная очистка" удаляет ту папку целиком).
// Копия: <yyyymmdd_hhmmss>_<тип>\ { manifest.txt, wpanel\ (копия Addons\wpanel), settings\ (копия %APPDATA%\wpanel, если снималась) }.
// Типы: update (тихая установка), install (кнопка "Установить"), cleanup, before_restore (текущее перед восстановлением).
// Автоматические (update, install) хранятся в числе последних N, остальные сами не удаляются.
static std::wstring g_backupRootOverride;   // тесты и --backup-root

static std::wstring BackupsRoot()
{
    if (!g_backupRootOverride.empty()) return g_backupRootOverride;
    std::wstring ad = GetAppData();
    return ad.empty() ? L"" : ad + L"\\wpanel_backups";
}

static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static bool WriteTextFileUtf8(const std::wstring& path, const std::wstring& text)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string u = WideToUtf8(text);
    DWORD n = 0;
    bool ok = WriteFile(h, u.data(), (DWORD)u.size(), &n, nullptr) && n == u.size();
    CloseHandle(h);
    return ok;
}
static std::wstring ReadTextFileUtf8(const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::string s;
    char buf[4096]; DWORD n = 0;
    while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n > 0) s.append(buf, n);
    CloseHandle(h);
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s.erase(0, 3);
    return Utf8ToWide(s);
}

// версия докера из sys.js (первые строки: var version = '3.10.23';)
static std::wstring ReadVersionFromFile(const std::wstring& sysJsPath)
{
    std::wifstream file(sysJsPath);
    if (!file) return L"";
    std::wstring line;
    for (int i = 0; i < 10 && std::getline(file, line); i++) {
        size_t pos = line.find(L"version");
        if (pos == std::wstring::npos) continue;
        size_t start = line.find(L"'", pos);
        if (start == std::wstring::npos) continue;
        size_t end = line.find(L"'", start + 1);
        if (end != std::wstring::npos) return line.substr(start + 1, end - start - 1);
    }
    return L"";
}

struct BackupInfo {
    std::wstring dir, name, kind, date, oldVersion, newVersion, addons, label;
    bool hasWpanel = false, hasSettings = false;
};

static std::map<std::wstring, std::wstring> ParseKv(const std::wstring& text)
{
    std::map<std::wstring, std::wstring> kv;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find(L'\n', p);
        if (e == std::wstring::npos) e = text.size();
        std::wstring line = text.substr(p, e - p);
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        size_t eq = line.find(L'=');
        if (eq != std::wstring::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
        p = e + 1;
    }
    return kv;
}

static bool LoadBackupInfo(const std::wstring& dir, BackupInfo& bi)
{
    bi = BackupInfo();
    bi.dir = dir;
    bi.name = dir.substr(dir.find_last_of(L'\\') + 1);
    bi.hasWpanel = PathFileExistsW((dir + L"\\wpanel").c_str()) != FALSE;
    bi.hasSettings = PathFileExistsW((dir + L"\\settings").c_str()) != FALSE;
    if (!bi.hasWpanel && !bi.hasSettings) return false;
    auto kv = ParseKv(ReadTextFileUtf8(dir + L"\\manifest.txt"));
    bi.kind = kv[L"kind"]; bi.date = kv[L"date"]; bi.oldVersion = kv[L"old_version"];
    bi.newVersion = kv[L"new_version"]; bi.addons = kv[L"addons"]; bi.label = kv[L"label"];
    return true;
}

// все копии корня, новые первыми (имя папки начинается с даты, поэтому порядок по имени = по времени)
static std::vector<BackupInfo> ListBackups()
{
    std::vector<BackupInfo> r;
    std::wstring root = BackupsRoot();
    if (root.empty()) return r;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return r;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        BackupInfo bi;
        if (LoadBackupInfo(root + L"\\" + fd.cFileName, bi)) r.push_back(bi);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(r.begin(), r.end(), [](const BackupInfo& a, const BackupInfo& b) { return a.name > b.name; });
    return r;
}

static bool IsAutoBackupKind(const std::wstring& k) { return k == L"update" || k == L"install"; }

// Копии удачных установок (update, install) и неудачных (failed) считаются раздельно: серия сбойных попыток
// не вытесняет копии, снятые перед удачными установками.
static const int kKeepFailed = 15;

static void PruneAutoBackups(int keep)
{
    if (keep < 1) return;
    int seen = 0, seenFailed = 0;
    for (const BackupInfo& b : ListBackups()) {
        bool failed = b.kind == L"failed";
        if (!failed && !IsAutoBackupKind(b.kind)) continue;
        if (++(failed ? seenFailed : seen) > (failed ? kKeepFailed : keep)) { LogLine(L"Backup prune: " + b.dir); DeleteIfExists(b.dir); }
    }
}

// Установка не удалась: копию переименовываем в <дата>_failed (kind=failed в манифесте).
static void MarkBackupFailed(std::wstring& dir)
{
    const size_t us = dir.find_last_of(L'_');
    const size_t sl = dir.find_last_of(L'\\');
    if (us == std::wstring::npos || sl == std::wstring::npos || us < sl) return;
    std::wstring nd = dir.substr(0, us) + L"_failed";
    for (int i = 2; PathFileExistsW(nd.c_str()) && i < 100; i++) nd = dir.substr(0, us) + L"_failed_" + std::to_wstring(i);
    if (!MoveFileW(dir.c_str(), nd.c_str())) return;
    std::wstring m = ReadTextFileUtf8(nd + L"\\manifest.txt");
    size_t p = m.find(L"kind=");
    size_t e = p == std::wstring::npos ? p : m.find(L'\n', p);
    if (p != std::wstring::npos && e != std::wstring::npos) {
        m.replace(p, e - p, L"kind=failed");
        WriteTextFileUtf8(nd + L"\\manifest.txt", m);
    }
    dir = nd;
}

// 0 - копия снята (outDir), 1 - копировать нечего (первая установка), -1 - ошибка
static int MakeBackup(const std::wstring& kind, const std::wstring& addonsPath, bool withSettings,
                      const std::wstring& label, const std::wstring& newVersion, std::wstring* outDir, bool withWpanel = true)
{
    if (outDir) outDir->clear();
    const std::wstring wp = addonsPath + L"\\wpanel";
    const std::wstring st = SettingsPath();
    const bool haveWp = withWpanel && PathFileExistsW(wp.c_str()) != FALSE;
    const bool haveSt = withSettings && !st.empty() && PathFileExistsW(st.c_str());
    if (!haveWp && !haveSt) return 1;

    std::wstring root = BackupsRoot();
    if (root.empty()) return -1;
    SYSTEMTIME t{}; GetLocalTime(&t);
    wchar_t date[64]{};
    swprintf(date, _countof(date), L"%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::wstring base = root + L"\\" + MakeTimestampFolderName() + L"_" + kind;
    std::wstring dir = base;
    for (int i = 2; PathFileExistsW(dir.c_str()) && i < 100; i++) dir = base + L"_" + std::to_wstring(i);
    if (!EnsureDir(dir)) { LogLine(L"ERROR: backup - cannot create " + dir); return -1; }

    bool ok = true;
    if (haveWp) ok = CopyDirRecursive(wp, dir + L"\\wpanel");
    if (ok && haveSt) ok = CopyDirRecursive(st, dir + L"\\settings");
    if (ok) {
        std::wstring m = L"kind=" + kind + L"\ndate=" + date + L"\nold_version=" + ReadVersionFromFile(wp + L"\\js\\sys.js") +
            L"\nnew_version=" + newVersion + L"\naddons=" + addonsPath + L"\nlabel=" + label + L"\n";
        ok = WriteTextFileUtf8(dir + L"\\manifest.txt", m);
    }
    if (!ok) { LogLine(L"ERROR: backup failed, removing " + dir); DeleteIfExists(dir); return -1; }
    LogLine(L"Backup created: " + dir);
    if (outDir) *outDir = dir;
    return 0;
}

// Восстановление ЧИСТОЕ: Addons\wpanel (и settings, если они есть в копии) очищаются и копируются из копии.
// Перед этим текущее состояние само сохраняется копией before_restore. Коды: 0 ок, 4 DLL заняты (Corel открыт),
// 5 не вышла копия текущего, 8 ошибка копирования, 9 копия негодная.
static int RestoreBackup(const std::wstring& backupDir, const std::wstring& addonsPath, bool onlySettings, std::wstring* err)
{
    BackupInfo bi;
    if (!LoadBackupInfo(backupDir, bi)) { if (err) *err = L"backup not found or empty"; return 9; }
    const bool doWp = bi.hasWpanel && !onlySettings;
    const bool doSt = bi.hasSettings;
    if (!doWp && !doSt) { if (err) *err = L"nothing to restore (no settings in this backup)"; return 9; }
    const std::wstring wp = addonsPath + L"\\wpanel";
    // Занятые DLL = Corel открыт: он держит файлы докера и перезапишет настройки при закрытии.
    if (!addonsPath.empty()) {
        std::wstring locked = FindLockedFiles(wp);
        if (!locked.empty()) { if (err) *err = L"files in use:\n" + locked; return 4; }
    }
    std::wstring pre;
    int b = MakeBackup(L"before_restore", addonsPath, doSt, bi.label, L"", &pre, doWp);
    if (b < 0) { if (err) *err = L"cannot back up the current state"; return 5; }

    if (doWp) {
        DeleteIfExists(wp);
        if (PathFileExistsW(wp.c_str())) { if (err) *err = L"cannot clear " + wp; return 8; }
        if (!CopyDirRecursive(bi.dir + L"\\wpanel", wp)) { if (err) *err = L"copy failed: wpanel"; return 8; }
    }
    if (doSt) {
        const std::wstring st = SettingsPath();
        if (!st.empty()) {
            DeleteIfExists(st);
            if (!CopyDirRecursive(bi.dir + L"\\settings", st)) { if (err) *err = L"copy failed: settings"; return 8; }
        }
    }
    LogLine(L"Restored from " + backupDir + (pre.empty() ? L"" : L" (current saved to " + pre + L")"));
    return 0;
}

// ---------------- Установка ----------------
struct InstallOpts {
    std::wstring kind = L"install";   // тип копии: update (тихая) / install (кнопка)
    int keep = 5;                     // сколько копий удачных установок хранить (неудачных - kKeepFailed)
    bool registry = true;             // DisableVBADelay + режим IE11 (стенд отключает)
    int waitLockedSec = 0;            // ждать освобождения proxy.dll/main.dll до N секунд
    bool prompt = true;               // файл занят: окно "закройте Corel и нажмите Повторить" (стенды: --no-prompt, сразу код 4)
};

static int FindCorelVerFor(const std::wstring& addons)
{
    for (auto& f : DetectCorelAddons())
        if (_wcsicmp(f.addonsPath.c_str(), addons.c_str()) == 0) return f.corelVer;
    return 0;
}
static std::wstring FindLabelFor(const std::wstring& addons)
{
    for (auto& f : DetectCorelAddons())
        if (_wcsicmp(f.addonsPath.c_str(), addons.c_str()) == 0) return f.label;
    return L"";
}

// Откат: удаляет в dst файлы и пустые папки, которые есть в релизе (src), но которых не было в копии (bak).
// Всё, чего нет в релизе (личные файлы пользователя), не трогает.
static void RemoveReleaseOnlyFiles(const std::wstring& src, const std::wstring& bak, const std::wstring& dst)
{
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((src + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        const std::wstring s = src + L"\\" + fd.cFileName, b = bak + L"\\" + fd.cFileName, d = dst + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            RemoveReleaseOnlyFiles(s, b, d);
            if (!PathFileExistsW(b.c_str())) RemoveDirectoryW(d.c_str());   // только пустую
        }
        else if (!PathFileExistsW(b.c_str())) {
            SetFileAttributesW(d.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(d.c_str());
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Копия, копирование поверх (слияние, как было), откат при сбое. Коды: 0 ок; 3 нет папки wpanel рядом с установщиком;
// 4 DLL заняты (Corel открыт; список в lockedList); 5 копию снять не удалось; 6 сбой копирования, откат выполнен;
// 7 сбой копирования и откат не вышел.
static int InstallCore(const std::wstring& targetAddons, const InstallOpts& o, std::wstring* lockedList, std::wstring* backupDirOut)
{
    if (!HasSourceFolder()) return 3;
    const std::wstring exeDir = GetExeDir();
    const std::wstring src = exeDir + L"\\wpanel";
    const std::wstring dstRoot = targetAddons + L"\\wpanel";
    LogLine(L"Installing to: " + dstRoot);

    StopViewerServer(dstRoot);

    std::wstring locked;
    const DWORD t0 = GetTickCount();
    for (;;) {
        locked = FindLockedFiles(dstRoot, src);
        if (locked.empty()) break;
        if ((int)(GetTickCount() - t0) < o.waitLockedSec * 1000) { Sleep(500); continue; }
        if (!o.prompt) break;
        std::wstring msg = T(L"FilesInUseRetry");
        size_t p = msg.find(L"{0}");
        if (p != std::wstring::npos) msg.replace(p, 3, locked);
        if (MessageBoxW(nullptr, msg.c_str(), T(L"Title").c_str(), MB_RETRYCANCEL | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND) != IDRETRY) break;
    }
    if (!locked.empty()) {
        LogLine(L"ERROR: Files in use, install aborted:\n" + locked);
        if (lockedList) *lockedList = locked;
        return 4;
    }

    std::wstring bdir;
    int b = MakeBackup(o.kind, targetAddons, false, FindLabelFor(targetAddons), ReadVersionFromFile(src + L"\\js\\sys.js"), &bdir);
    if (b < 0) return 5;
    if (backupDirOut) *backupDirOut = bdir;

    g_skipIdentical = true;
    const bool copied = EnsureDir(targetAddons) && CopyDirRecursive(src, dstRoot);
    g_skipIdentical = false;
    if (!copied) {
        LogLine(L"ERROR: Copy failed.");
        if (b == 0) {
            // Папку НЕ стираем целиком: в ней личные файлы пользователя (свои папки в gbx), а занятый файл
            // не удалится и не перезапишется. Откат: убираем только то, что принёс релиз и чего не было в копии,
            // затем возвращаем файлы из копии поверх.
            RemoveReleaseOnlyFiles(src, bdir + L"\\wpanel", dstRoot);
            const bool rolled = CopyDirRecursive(bdir + L"\\wpanel", dstRoot, true);
            MarkBackupFailed(bdir);
            if (backupDirOut) *backupDirOut = bdir;
            PruneAutoBackups(o.keep);
            if (rolled) { LogLine(L"Rolled back from " + bdir); return 6; }
            LogLine(L"ERROR: Rollback failed (copy: " + bdir + L").");
            return 7;
        }
        DeleteIfExists(dstRoot);                       // первая установка: откатываться не к чему, недокопированное убираем
        return 6;
    }

    // cleanup like PS
    DeleteIfExists(dstRoot + L"\\gbx\\box simple.js");
    DeleteIfExists(dstRoot + L"\\gbx\\box simple");

    if (o.registry) {
        int ver = FindCorelVerFor(targetAddons);
        if (ver >= 17) DisableVBADelay(ver);
        else if (ver > 0) LogLine(L"INFO: CorelDRAW v" + std::to_wstring(ver) + L" does not support VBA delay disable (requires v17+).");
        else LogLine(L"INFO: CorelDRAW version unknown - VBA delay disable skipped.");
        // Force IE11 edge mode for CorelDRW.exe so the docker (embedded IE) renders correctly.
        SetBrowserEmulation();
    }
    PruneAutoBackups(o.keep);
    return 0;
}

static bool DoInstall(HWND hDlg)
{
    if (!HasSourceFolder()) {
        ShowMsg(hDlg, T(L"SourceMissing") + std::wstring(L"\n") + T(L"InstallDisabled"), MB_ICONERROR);
        EnableWindow(GetDlgItem(hDlg, IDC_BTN_INSTALL), FALSE);
        return false;
    }

    std::wstring targetAddons = GetDlgText(hDlg, IDC_EDIT_TARGET);
    if (targetAddons.empty()) {
        ShowMsg(hDlg, T(L"Error") + L" " + T(L"SelectCorel"), MB_ICONWARNING); // if key exists in js
        return false;
    }

    InstallOpts o;
    o.kind = L"install";
    std::wstring locked;
    int rc = InstallCore(targetAddons, o, &locked, nullptr);
    if (rc == 4) {
        std::wstring msg = T(L"FilesInUse");
        size_t p = msg.find(L"{0}");
        if (p != std::wstring::npos) msg.replace(p, 3, locked);
        ShowMsg(hDlg, msg, MB_ICONWARNING);
        return false;
    }
    if (rc != 0) {
        std::wstring what = rc == 5 ? L"Cannot create backup." : rc == 6 ? L"Copy failed (previous version restored)." : L"Copy failed.";
        ShowMsg(hDlg, std::wstring(T(L"Error")) + L" " + what, MB_ICONERROR);
        return false;
    }

    std::wstring successMsg = T(L"InstallSuccess");
    size_t pos = successMsg.find(L"{0}");
    if (pos != std::wstring::npos) {
        successMsg.replace(pos, 3, g_version);
    }
    ShowDoneAskRun(hDlg, successMsg, targetAddons);
    RefreshDetectedList(hDlg);
    return true;
}

static bool DoFullCleanup(HWND hDlg)
{
    HWND hList = GetDlgItem(hDlg, IDC_LIST_PATHS);
    int sel = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR || sel < 0 || sel >= (int)g_found.size()) {
        ShowMsg(hDlg, T(L"SelectCorel"), MB_ICONWARNING);
        return false;
    }

    int res = MessageBoxW(hDlg, T(L"CleanupConfirm").c_str(), T(L"CleanupTitle").c_str(), MB_YESNO | MB_ICONQUESTION);
    if (res != IDYES) return false;

    auto& it = g_found[sel];
    const std::wstring wp = it.addonsPath + L"\\wpanel";

    // Копия по общей схеме (wpanel выбранного Corel + все настройки пользователя): её же потом
    // возвращает кнопка "Восстановить из копии". Не вышла копия - ничего не удаляем.
    std::wstring bdir;
    int b = MakeBackup(L"cleanup", it.addonsPath, true, it.label, L"", &bdir);
    if (b < 0) {
        LogLine(L"ERROR: Cleanup - backup failed, nothing deleted");
        ShowMsg(hDlg, std::wstring(T(L"Error")) + L" " + std::wstring(L"Cannot create backup folder."), MB_ICONERROR);
        return false;
    }

    // Delete wpanel from selected CorelDRAW only
    if (PathFileExistsW(wp.c_str())) DeleteIfExists(wp);

    // Delete %APPDATA%\wpanel (настройки)
    {
        std::wstring dir = SettingsPath();
        if (!dir.empty() && PathFileExistsW(dir.c_str())) DeleteIfExists(dir);
    }

    // Remove the forced IE11 emulation for CorelDRW.exe
    RemoveBrowserEmulation();

    ShowMsg(hDlg, T(L"CleanupComplete"), MB_ICONINFORMATION);
    RefreshDetectedList(hDlg);
    return true;
}

// ---------------- Тихий режим (без окон) ----------------
//   wood_install.exe --silent --target "<...\Addons>" [--backup-root D] [--keep N] [--no-registry] [--wait-locked сек]
//                    [--kind update] [--result файл] [--log файл] [--settings-dir D]
//   wood_install.exe --restore "<папка копии>" --target "<...\Addons>" [--result файл] ...
// Коды: 0 ок; 2 неверные аргументы; 3 нет wpanel рядом с установщиком; 4 DLL заняты; 5 копия не вышла;
// 6 сбой копирования, откат выполнен; 7 сбой, откат не вышел; 8 сбой восстановления; 9 копия негодная.
// --result: файл с одной строкой "OK" или "ERR <код> <текст>" (читает updater).
static int RunHeadless(int argc, wchar_t** argv)
{
    std::wstring target, restoreDir, resultFile;
    bool onlySettings = false;
    InstallOpts o;
    o.kind = L"update";
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        auto next = [&](std::wstring& dst) { if (i + 1 < argc) dst = argv[++i]; };
        std::wstring v;
        if (a == L"--target") next(target);
        else if (a == L"--restore") next(restoreDir);
        else if (a == L"--backup-root") next(g_backupRootOverride);
        else if (a == L"--keep") { next(v); o.keep = _wtoi(v.c_str()); }
        else if (a == L"--no-registry") o.registry = false;
        else if (a == L"--only-settings") onlySettings = true;
        else if (a == L"--wait-locked") { next(v); o.waitLockedSec = _wtoi(v.c_str()); }
        else if (a == L"--no-prompt") o.prompt = false;
        else if (a == L"--kind") next(o.kind);
        else if (a == L"--result") next(resultFile);
        else if (a == L"--log") next(g_logPath);
        else if (a == L"--settings-dir") next(g_settingsDirOverride);
    }
    auto finish = [&](int code, const std::wstring& msg) {
        if (!resultFile.empty()) WriteTextFileUtf8(resultFile, code == 0 ? L"OK" : L"ERR " + std::to_wstring(code) + L" " + msg);
        LogLine(L"Headless exit " + std::to_wstring(code) + (msg.empty() ? L"" : L": " + msg));
        return code;
    };
    LogLine(L"--- Start (headless) ---");
    if (target.empty() && !(onlySettings && !restoreDir.empty())) return finish(2, L"--target is required");
    while (target.size() > 3 && (target.back() == L'\\' || target.back() == L'/')) target.pop_back();

    if (!restoreDir.empty()) {
        std::wstring err;
        int rc = RestoreBackup(restoreDir, target, onlySettings, &err);
        return finish(rc, err);
    }
    g_version = ReadVersionFromSysJs();
    std::wstring locked;
    int rc = InstallCore(target, o, &locked, nullptr);
    return finish(rc, rc == 4 ? L"files in use: " + locked : L"");
}

static void DeleteSelectedCustomPath(HWND hDlg, int sel)
{
    if (sel == LB_ERR || sel < 0 || sel >= (int)g_found.size()) return;

    const FoundPath& item = g_found[sel];
    if (item.isStandard) {
        ShowMsg(hDlg, T(L"CannotRemoveStandard"), MB_ICONWARNING);
        return;
    }

    std::wstring msg = T(L"RemoveFolderConfirm");
    size_t pos = msg.find(L"{0}");
    if (pos != std::wstring::npos) msg.replace(pos, 3, item.addonsPath);

    int res = MessageBoxW(hDlg, msg.c_str(), T(L"RemoveFolderTitle").c_str(), MB_YESNO | MB_ICONQUESTION);
    if (res != IDYES) return;

    RemoveCustomPath(item.addonsPath);
    RefreshDetectedList(hDlg);
}

// ---------------- Восстановление из копии (окно выбора) ----------------
static std::vector<BackupInfo> g_restoreList;     // все копии
static std::vector<int> g_restoreShown;           // индексы показанных в списке (фильтр "только настройки")
static bool g_restoreOnlySettings = false;        // отметка в окне на момент закрытия

static std::wstring BackupKindLabel(const std::wstring& k)
{
    if (k == L"update") return T(L"BkKindUpdate");
    if (k == L"install") return T(L"BkKindInstall");
    if (k == L"cleanup") return T(L"BkKindCleanup");
    if (k == L"failed") return T(L"BkKindFailed");
    if (k == L"settings") return T(L"BkKindSettings");
    if (k == L"before_restore") return T(L"BkKindBeforeRestore");
    return k;
}

static void FillRestoreList(HWND h)
{
    const bool only = IsDlgButtonChecked(h, IDC_CHK_ONLY_SETTINGS) == BST_CHECKED;
    HWND lb = GetDlgItem(h, IDC_LIST_BACKUPS);
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    g_restoreShown.clear();
    for (size_t i = 0; i < g_restoreList.size(); i++) {
        const BackupInfo& b = g_restoreList[i];
        if (only && !b.hasSettings) continue;
        std::wstring line = (b.date.empty() ? b.name : b.date) + L"   " + BackupKindLabel(b.kind);
        if (!b.oldVersion.empty() || !b.newVersion.empty())
            line += L"   " + (b.oldVersion.empty() ? L"?" : b.oldVersion) + (b.newVersion.empty() ? L"" : L" -> " + b.newVersion);
        if (!b.label.empty()) line += L"   " + b.label;
        line += b.hasWpanel && b.hasSettings ? L"   [wpanel + settings]" : b.hasSettings ? L"   [settings]" : L"   [wpanel]";
        SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)line.c_str());
        g_restoreShown.push_back((int)i);
    }
    if (!g_restoreShown.empty()) SendMessageW(lb, LB_SETCURSEL, 0, 0);
}

static INT_PTR CALLBACK RestoreDlgProc(HWND h, UINT msg, WPARAM wp, LPARAM)
{
    switch (msg) {
    case WM_INITDIALOG:
        SetWindowTextW(h, T(L"RestoreDialogTitle").c_str());
        SetDlgItemTextW(h, IDC_STATIC_BACKUPS, T(L"RestoreDialogHint").c_str());
        SetDlgItemTextW(h, IDC_CHK_ONLY_SETTINGS, T(L"RestoreOnlySettings").c_str());
        SetDlgItemTextW(h, IDOK, T(L"RestoreDialogOk").c_str());
        SetDlgItemTextW(h, IDCANCEL, T(L"RestoreDialogCancel").c_str());
        CheckDlgButton(h, IDC_CHK_ONLY_SETTINGS, BST_UNCHECKED);     // по умолчанию восстанавливается всё
        FillRestoreList(h);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_CHK_ONLY_SETTINGS) { FillRestoreList(h); return TRUE; }
        if (LOWORD(wp) == IDOK || (LOWORD(wp) == IDC_LIST_BACKUPS && HIWORD(wp) == LBN_DBLCLK)) {
            int s = (int)SendDlgItemMessageW(h, IDC_LIST_BACKUPS, LB_GETCURSEL, 0, 0);
            if (s == LB_ERR || s < 0 || s >= (int)g_restoreShown.size()) return TRUE;
            g_restoreOnlySettings = IsDlgButtonChecked(h, IDC_CHK_ONLY_SETTINGS) == BST_CHECKED;
            EndDialog(h, g_restoreShown[s] + 1);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(h, 0); return TRUE; }
        break;
    }
    return FALSE;
}

// Занятые DLL у всех найденных Corel: настройки общие (%APPDATA%\wpanel), и запущенный докер перезапишет их при закрытии.
static std::wstring FindLockedAnyCorel()
{
    std::wstring all;
    for (auto& f : g_found) {
        std::wstring l = FindLockedFiles(f.addonsPath + L"\\wpanel");
        if (!l.empty()) all += f.label + L":\n" + l;
    }
    return all;
}

static void ShowFilesInUse(HWND hDlg, const std::wstring& list)
{
    std::wstring msg = T(L"FilesInUse");
    size_t p = msg.find(L"{0}");
    if (p != std::wstring::npos) msg.replace(p, 3, list);
    ShowMsg(hDlg, msg, MB_ICONWARNING);
}

// Кнопка "Восстановить настройки": список копий, по умолчанию восстанавливается всё (wpanel + настройки, что есть в копии),
// с отметкой "Только настройки" - только они. Corel должен быть закрыт: проверяем занятость DLL до любых действий.
static void DoRestoreFromBackup(HWND hDlg)
{
    g_restoreList = ListBackups();
    if (g_restoreList.empty()) { ShowMsg(hDlg, T(L"RestoreNoBackups"), MB_ICONINFORMATION); return; }
    g_restoreOnlySettings = false;
    INT_PTR r = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_RESTORE), hDlg, RestoreDlgProc, 0);
    if (r <= 0 || r > (INT_PTR)g_restoreList.size()) return;
    const BackupInfo& b = g_restoreList[(size_t)r - 1];
    const bool onlySettings = g_restoreOnlySettings;
    const bool touchesWpanel = b.hasWpanel && !onlySettings;

    int sel = (int)SendDlgItemMessageW(hDlg, IDC_LIST_PATHS, LB_GETCURSEL, 0, 0);
    const bool haveSel = !(sel == LB_ERR || sel < 0 || sel >= (int)g_found.size());
    if (touchesWpanel && !haveSel) { ShowMsg(hDlg, T(L"SelectCorel"), MB_ICONWARNING); return; }

    std::wstring locked = FindLockedAnyCorel();
    if (!locked.empty()) { ShowFilesInUse(hDlg, locked); return; }

    if (MessageBoxW(hDlg, T(L"RestoreConfirm").c_str(), T(L"RestoreDialogTitle").c_str(), MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    std::wstring err;
    int rc = RestoreBackup(b.dir, haveSel ? g_found[sel].addonsPath : L"", onlySettings, &err);
    if (rc == 0) { ShowMsg(hDlg, T(L"RestoreDone"), MB_ICONINFORMATION); RefreshDetectedList(hDlg); return; }
    if (rc == 4) {
        size_t nl = err.find(L'\n');
        ShowFilesInUse(hDlg, nl == std::wstring::npos ? err : err.substr(nl + 1));
        return;
    }
    ShowMsg(hDlg, std::wstring(T(L"Error")) + L" " + err, MB_ICONERROR);
}

// ---------------- Dialog ----------------
static INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_INITDIALOG:
    {
        // Read version from wpanel/js/sys.js
        g_version = ReadVersionFromSysJs();
        
        // Create color brushes
        g_brushBackground = CreateSolidBrush(RGB(230, 230, 230));
        g_brushInstallBtn = CreateSolidBrush(RGB(255, 200, 120));
        g_brushVideoBtn = CreateSolidBrush(RGB(255, 235, 200));
        g_brushTelegramBtn = CreateSolidBrush(RGB(100, 181, 246));
        
        // Center window on screen
        RECT rcWindow, rcScreen;
        GetWindowRect(hDlg, &rcWindow);
        GetWindowRect(GetDesktopWindow(), &rcScreen);
        int x = (rcScreen.right - rcScreen.left - (rcWindow.right - rcWindow.left)) / 2;
        int y = (rcScreen.bottom - rcScreen.top - (rcWindow.bottom - rcWindow.top)) / 2;
        SetWindowPos(hDlg, HWND_TOP, x, y, 0, 0, SWP_NOSIZE);
        
        // Set dialog background color
        SetDlgItemTextW(hDlg, 0, L""); // Placeholder

        // Tooltips for buttons
        g_hwndTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            hDlg, nullptr, (HINSTANCE)GetWindowLongPtrW(hDlg, GWLP_HINSTANCE), nullptr);

        // scan languages
        g_langFiles = ScanLangFiles();
        FillLanguageCombo(hDlg);

        // choose initial language and load dictionary
        SelectInitialLanguage(hDlg);

        // apply to UI
        ApplyUiText(hDlg);

        if (!HasSourceFolder()) {
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_INSTALL), FALSE);

            ShowMsg(hDlg, T(L"SourceMissing") + std::wstring(L"\n") + T(L"InstallDisabled"), MB_ICONWARNING);
        }

        // other init
        CheckDlgButton(hDlg, IDC_CHK_DISABLE_VBA, BST_CHECKED);
        RefreshDetectedList(hDlg);

        return TRUE;
    }

    case WM_CTLCOLORBTN:
    {
        HDC hdc = (HDC)wParam;
        HWND hwndCtrl = (HWND)lParam;
        int ctrlId = GetDlgCtrlID(hwndCtrl);
        
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(0, 0, 0));
        
        if (ctrlId == IDC_BTN_INSTALL) {
            SetBkColor(hdc, RGB(255, 200, 120));
            return (INT_PTR)g_brushInstallBtn;
        }
        else if (ctrlId == IDC_BTN_VIDEO) {
            SetBkColor(hdc, RGB(255, 235, 200));
            return (INT_PTR)g_brushVideoBtn;
        }
        else if (ctrlId == IDC_BTN_TELEGRAM) {
            SetBkColor(hdc, RGB(100, 181, 246));
            SetTextColor(hdc, RGB(255, 255, 255));
            return (INT_PTR)g_brushTelegramBtn;
        }
        else {
            SetBkColor(hdc, RGB(230, 230, 230));
            return (INT_PTR)g_brushBackground;
        }
    }

    case WM_CTLCOLORDLG:
    {
        HDC hdc = (HDC)wParam;
        SetBkColor(hdc, RGB(230, 230, 230));
        return (INT_PTR)g_brushBackground;
    }

    case WM_DRAWITEM:
    {
        LPDRAWITEMSTRUCT lpDIS = (LPDRAWITEMSTRUCT)lParam;
        if (lpDIS->CtlType == ODT_BUTTON) {
            HDC hdc = lpDIS->hDC;
            COLORREF bgColor = RGB(230, 230, 230);
            COLORREF textColor = RGB(0, 0, 0);
            
            int ctrlId = GetDlgCtrlID(lpDIS->hwndItem);
            if (ctrlId == IDC_BTN_INSTALL) {
                bgColor = RGB(255, 200, 120);
            }
            else if (ctrlId == IDC_BTN_VIDEO) {
                bgColor = RGB(255, 235, 200);
            }
            else if (ctrlId == IDC_BTN_TELEGRAM) {
                bgColor = RGB(100, 181, 246);
                textColor = RGB(255, 255, 255);
            }
            
            // Fill button background with color
            HBRUSH hBrush = CreateSolidBrush(bgColor);
            FillRect(hdc, &lpDIS->rcItem, hBrush);
            DeleteObject(hBrush);
            
            // Draw button border with 3D effect
            HPEN hPenDark = CreatePen(PS_SOLID, 1, RGB(100, 100, 100));
            HPEN hPenLight = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
            HPEN hPenOld = (HPEN)SelectObject(hdc, hPenLight);
            
            // Top and left edges (light)
            MoveToEx(hdc, lpDIS->rcItem.left, lpDIS->rcItem.bottom - 1, nullptr);
            LineTo(hdc, lpDIS->rcItem.left, lpDIS->rcItem.top);
            LineTo(hdc, lpDIS->rcItem.right, lpDIS->rcItem.top);
            
            // Bottom and right edges (dark)
            SelectObject(hdc, hPenDark);
            MoveToEx(hdc, lpDIS->rcItem.right - 1, lpDIS->rcItem.top, nullptr);
            LineTo(hdc, lpDIS->rcItem.right - 1, lpDIS->rcItem.bottom - 1);
            LineTo(hdc, lpDIS->rcItem.left, lpDIS->rcItem.bottom - 1);
            
            SelectObject(hdc, hPenOld);
            DeleteObject(hPenDark);
            DeleteObject(hPenLight);
            
            // Draw focus rect if focused
            if (lpDIS->itemState & ODS_FOCUS) {
                RECT rcFocus = lpDIS->rcItem;
                InflateRect(&rcFocus, -2, -2);
                DrawFocusRect(hdc, &rcFocus);
            }
            
            // Draw text
            RECT rcText = lpDIS->rcItem;
            InflateRect(&rcText, -2, -2);
            SetTextColor(hdc, textColor);
            SetBkMode(hdc, TRANSPARENT);
            
            wchar_t szText[256];
            GetWindowTextW(lpDIS->hwndItem, szText, sizeof(szText) / sizeof(szText[0]));
            DrawTextW(hdc, szText, -1, &rcText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            
            return TRUE;
        }
        return FALSE;
    }

    case WM_VKEYTOITEM:
        if ((HWND)lParam == GetDlgItem(hDlg, IDC_LIST_PATHS) && LOWORD(wParam) == VK_DELETE) {
            DeleteSelectedCustomPath(hDlg, HIWORD(wParam));
            SetWindowLongPtrW(hDlg, DWLP_MSGRESULT, (LONG_PTR)-2);
            return TRUE;
        }
        SetWindowLongPtrW(hDlg, DWLP_MSGRESULT, (LONG_PTR)-1);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {

        case IDC_CMB_LANG:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                HWND hCmb = GetDlgItem(hDlg, IDC_CMB_LANG);
                int sel = (int)SendMessageW(hCmb, CB_GETCURSEL, 0, 0);

                if (!g_hasEnFile && sel == 0) {
                    LoadLanguageByIndex(-1);
                    SaveLangCode(L"en");
                }
                else {
                    int idx = (int)SendMessageW(hCmb, CB_GETITEMDATA, sel, 0);
                    if (idx >= 0 && idx < (int)g_langFiles.size()) {
                        if (LoadLanguageByIndex(idx)) {
                            SaveLangCode(g_langFiles[idx].code);
                        }
                    }
                }

                ApplyUiText(hDlg); // <-- вот это must-have для “мгновенно”
            }
            return TRUE;

        case IDC_LIST_PATHS:
            if (HIWORD(wParam) == LBN_SELCHANGE) {
                int sel = (int)SendDlgItemMessageW(hDlg, IDC_LIST_PATHS, LB_GETCURSEL, 0, 0);
                if (sel != LB_ERR && sel >= 0 && sel < (int)g_found.size()) {
                    SetDlgText(hDlg, IDC_EDIT_TARGET, g_found[sel].addonsPath);
                }
            }
            return TRUE;

        case IDC_BTN_BROWSE: {
            std::wstring picked = BrowseForFolder(hDlg);
            if (!picked.empty()) {
                int idx = -1;
                for (size_t i = 0; i < g_found.size(); i++) {
                    if (_wcsicmp(g_found[i].addonsPath.c_str(), picked.c_str()) == 0) { idx = (int)i; break; }
                }
                if (idx < 0) {
                    SaveCustomPathAppendUnique(picked);
                    LogLine(L"Added custom path: " + picked);
                    RefreshDetectedList(hDlg);
                    for (size_t i = 0; i < g_found.size(); i++) {
                        if (_wcsicmp(g_found[i].addonsPath.c_str(), picked.c_str()) == 0) { idx = (int)i; break; }
                    }
                }
                if (idx >= 0) {
                    SendDlgItemMessageW(hDlg, IDC_LIST_PATHS, LB_SETCURSEL, (WPARAM)idx, 0);
                    SetDlgText(hDlg, IDC_EDIT_TARGET, g_found[idx].addonsPath);
                }
            }
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_INSTALL), HasSourceFolder() ? TRUE : FALSE);
            return TRUE;
        }

        case IDC_BTN_INSTALL:
            DoInstall(hDlg);
            return TRUE;

        case IDC_BTN_BACKUP_SETTINGS:
            // копия только настроек - в той же схеме, что и остальные (видна в списке восстановления)
            if (MakeBackup(L"settings", L"", true, L"", L"", nullptr, false) == 0)
                ShowMsg(hDlg, T(L"SettingsSaved"), MB_ICONINFORMATION);
            else
                ShowMsg(hDlg, T(L"SettingsNotFound") + L" " + SettingsPath(), MB_ICONWARNING);
            return TRUE;

        case IDC_BTN_RESTORE_SETTINGS:
            DoRestoreFromBackup(hDlg);
            return TRUE;

        case IDC_BTN_FULL_CLEANUP:
            DoFullCleanup(hDlg);
            return TRUE;

        case IDC_BTN_VIDEO:
        {
            std::wstring url = T(L"Video");
            if (!url.empty()) ShellExecuteW(hDlg, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;
        }
        case IDC_BTN_TELEGRAM:
        {
            std::wstring url = T(L"Telegram");
            if (!url.empty()) ShellExecuteW(hDlg, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;
        }

        case IDC_BTN_COREL_TWEAK:
        {
            HWND hList = GetDlgItem(hDlg, IDC_LIST_PATHS);
            int sel = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
            if (sel == LB_ERR || sel < 0 || sel >= (int)g_found.size()) {
                ShowMsg(hDlg, L"Please select a CorelDRAW version first.", MB_ICONWARNING);
                return TRUE;
            }
            
            int ver = g_found[sel].corelVer;
            ApplyCorelTweaks(hDlg, ver);
            return TRUE;
        }

        case IDC_BTN_EXIT:
        case IDCANCEL:
            EndDialog(hDlg, 0);
            return TRUE;
        }
        break;

        case WM_DESTROY:
            if (g_brushBackground) DeleteObject(g_brushBackground);
            if (g_brushInstallBtn) DeleteObject(g_brushInstallBtn);
            if (g_brushVideoBtn) DeleteObject(g_brushVideoBtn);
            if (g_brushTelegramBtn) DeleteObject(g_brushTelegramBtn);
            g_hwndTip = nullptr;
            return FALSE;
    }
    return FALSE;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int)
{
    {
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        bool headless = false;
        for (int i = 1; argv && i < argc; i++)
            if (wcscmp(argv[i], L"--silent") == 0 || wcscmp(argv[i], L"--restore") == 0) headless = true;
        if (headless) return RunHeadless(argc, argv);
    }
    LogLine(L"--- Start ---");
    LogLine(std::wstring(L"Elevated: ") + (IsRunningElevated() ? L"yes" : L"no"));
    g_inst = g_en;

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);

    return (int)DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_MAIN), nullptr, DlgProc, 0);
}
