// update_mode.inc - режим "wood_server.exe update" (автообновление докера). Включается в main.cpp (одна единица трансляции).
//
//   wood_server.exe update --from-stage --stage-dir "<папка>" --addons "<...\Addons>" [--corel-pid N] [--corel-exe путь] [--corel-args строка]
//                          [--work "<папка рендера>"] [--lang ru|en] [--no-ui] [--result файл] [--stage2]
//
// Сети в этом режиме НЕТ: архив (update.zip) и подпись (update.sig) заранее кладёт в --stage-dir докер (скачивает сам Corel).
// Подпись проверяется здесь (папка пользовательская, а установщик идёт от администратора). После успеха и после неверной
// подписи/негодного архива файлы папки удаляются.
//
// Порядок (всё, что может не получиться, - ДО закрытия Corel):
//   1. копия себя в %TEMP% и запуск оттуда (установщик перезапишет bin\wood_server.exe, пока он занят - не выйдет);
//   2. чтение архива и подписи из папки, проверка подписи ключом разработчика (pubkey_release.h);
//   3. распаковка (miniz) в %TEMP%, проверка состава (wood_install.exe, wpanel\js\sys.js);
//   4. остановка сервера просмотра, закрытие Corel (WM_CLOSE - Corel сам спросит про сохранение), ожидание, пока он отпустит DLL;
//   5. тихий установщик (манифест просит администратора - один запрос UAC), результат читаем из --result установщика;
//   6. Corel запускается снова ВСЕГДА (даже если установка откатилась); временные файлы чистит докер (VBA dockerAutoRun).
// Коды: 0 ок; 61 подпись неверна; 62 негодный архив (в том числе нет файлов в папке); 63 Corel не закрылся/отмена; 64 установщик
// не отработал (код и текст - в сообщении); 65 прочее. (Код 60 "скачать не удалось" больше не используется: качает не exe.)
#include <winhttp.h>
#include <commctrl.h>
#include "miniz.h"
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#include "pubkey_release.h"

namespace upd {

// ---------- тексты (ru/en) ----------
static bool g_ru = true;
struct Tx { const wchar_t* ru; const wchar_t* en; };
static const Tx T_TITLE   = { L"Обновление Woodman Docker", L"Woodman Docker update" };
static const Tx T_DOWN    = { L"Скачивание обновления...", L"Downloading the update..." };
static const Tx T_SIG     = { L"Проверка подписи...", L"Checking the signature..." };
static const Tx T_UNPACK  = { L"Распаковка...", L"Unpacking..." };
static const Tx T_CLOSE   = { L"Закройте CorelDRAW (он предложит сохранить файлы)...", L"Please close CorelDRAW (it will offer to save your files)..." };
static const Tx T_UNLOAD  = { L"Выгрузка модуля докера (CorelDRAW закрывать не нужно)...", L"Unloading the docker module (CorelDRAW stays open)..." };
static const Tx T_FREE    = { L"Ожидание, пока CorelDRAW отпустит файлы...", L"Waiting for CorelDRAW to release its files..." };
static const Tx T_INST    = { L"Установка обновления...", L"Installing the update..." };
static const Tx T_START   = { L"Запуск CorelDRAW...", L"Starting CorelDRAW..." };
static const Tx Q_CLOSE   = { L"Для обновления нужно закрыть CorelDRAW. Он сам предложит сохранить открытые файлы.\nЗакрыть CorelDRAW сейчас?", L"The update needs CorelDRAW to be closed. It will offer to save your open files.\nClose CorelDRAW now?" };
static const Tx Q_CLOSE_Y = { L"Закрыть CorelDRAW", L"Close CorelDRAW" };
static const Tx Q_CLOSE_N = { L"Отмена", L"Cancel" };
static const Tx Q_DONE_OK = { L"Обновление установлено.", L"The update is installed." };
static const Tx Q_DONE_ER = { L"Обновление не установлено (подробности в журнале).", L"The update was not installed (see the log)." };
static const Tx Q_RUN_Y   = { L"Запустить CorelDRAW", L"Run CorelDRAW" };
static const Tx Q_RUN_N   = { L"Закончить", L"Finish" };
static const Tx E_NOCLOSE = { L"Обновление отложено: CorelDRAW не закрыт. Ничего не изменено.", L"The update was postponed: CorelDRAW was not closed. Nothing was changed." };
static const Tx T_DONE    = { L"Готово.", L"Done." };
static const Tx T_CANCEL  = { L"Отмена", L"Cancel" };
static const Tx E_DOWN    = { L"Не удалось скачать обновление. Проверьте интернет и попробуйте позже.", L"Could not download the update. Check your internet connection and try again." };
static const Tx E_SIG     = { L"Подпись обновления неверна. Установка отменена, ничего не изменено.", L"The update signature is invalid. Installation cancelled, nothing was changed." };
static const Tx E_PKG     = { L"Архив обновления повреждён или неполный. Ничего не изменено.", L"The update archive is damaged or incomplete. Nothing was changed." };
static const Tx E_AV      = { L"Файл обновления не найден: скорее всего, его забрал антивирус. Поищите его в карантине антивируса (wood_server.exe, wood_install.exe), восстановите, добавьте в исключения и повторите. Ничего не изменено.", L"An update file was not found: most likely your antivirus took it. Look for it in the antivirus quarantine (wood_server.exe, wood_install.exe), restore it, add it to the exclusions and try again. Nothing was changed." };
static const Tx E_CLOSE   = { L"CorelDRAW не закрылся. Обновление отменено, ничего не изменено.", L"CorelDRAW did not close. The update was cancelled, nothing was changed." };
static const Tx E_INST    = { L"Установщик не смог обновить докер", L"The installer could not update the docker" };
static const Tx E_ROLLED  = { L"Прежняя версия возвращена.", L"The previous version was restored." };
static const Tx E_UAC     = { L"Установка отменена (нет разрешения администратора).", L"Installation cancelled (administrator permission was not granted)." };
static const wchar_t* tr(const Tx& t) { return g_ru ? t.ru : t.en; }

// Вопрос пользователю с двумя своими подписями кнопок (MessageBox их не умеет: подписи подменяет CBT-хук). Да -> true.
static HHOOK g_askHook = nullptr;
static const wchar_t *g_askYes = L"", *g_askNo = L"";
static LRESULT CALLBACK askHookProc(int code, WPARAM w, LPARAM l)
{
    if (code == HCBT_ACTIVATE) {
        HWND h = (HWND)w;
        SetDlgItemTextW(h, IDYES, g_askYes);
        SetDlgItemTextW(h, IDNO, g_askNo);
        UnhookWindowsHookEx(g_askHook); g_askHook = nullptr;
        return 0;
    }
    return CallNextHookEx(g_askHook, code, w, l);
}
static bool askUser(const wchar_t* text, const wchar_t* yes, const wchar_t* no, UINT icon)
{
    g_askYes = yes; g_askNo = no;
    g_askHook = SetWindowsHookExW(WH_CBT, askHookProc, nullptr, GetCurrentThreadId());
    int r = MessageBoxW(nullptr, text, tr(T_TITLE), MB_YESNO | icon | MB_TOPMOST | MB_SETFOREGROUND);
    if (g_askHook) { UnhookWindowsHookEx(g_askHook); g_askHook = nullptr; }
    return r == IDYES;
}

// ---------- состояние ----------
static HWND g_wnd = nullptr, g_label = nullptr, g_bar = nullptr;
static volatile LONG g_cancel = 0;
static bool g_noUi = false;
static wstring g_resultFile;
static const UINT WM_STATUS = WM_APP + 1, WM_PROGRESS = WM_APP + 2, WM_FINISH = WM_APP + 3;

static wstring tempDirFor(const wchar_t* tag);
// журнал обновления: C:\temp\wood_update.log (папку создаём; нет доступа - %TEMP%). Дописывается, чтобы видеть несколько попыток подряд.
static void ulog(const wstring& s)
{
    static wstring path;
    if (path.empty()) {
        makeDirs(L"C:\\temp");
        path = L"C:\\temp\\wood_update.log";
        if (GetFileAttributesW(L"C:\\temp") == INVALID_FILE_ATTRIBUTES) path = tempDirFor(L"wood_update.log");
    }
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t ts[48];
    swprintf(ts, 48, L"%04d-%02d-%02d %02d:%02d:%02d [%lu] ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId());
    string u = toUtf8(wstring(ts) + s + L"\r\n");
    writeFile(path, u.data(), u.size(), true);
}
static void status(const wstring& s)
{
    ulog(L"status: " + s);
    if (g_noUi || !g_wnd) { string u = toUtf8(s); fwrite(u.data(), 1, u.size(), stdout); fputc('\n', stdout); fflush(stdout); return; }
    PostMessageW(g_wnd, WM_STATUS, 0, (LPARAM)new wstring(s));
}
static void progress(int pct)
{
    if (!g_noUi && g_wnd) PostMessageW(g_wnd, WM_PROGRESS, (WPARAM)pct, 0);
}

// ---------- скачивание (WinHTTP) ----------
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4   // сборка идёт с _WIN32_WINNT=0x0601, в заголовке константы нет
#endif
// Сессия WinHTTP: прокси определяется автоматически (настройки пользователя, WPAD/PAC), Win8.1 и новее.
// На более старой системе флаг не принимается (ERROR_INVALID_PARAMETER) - прежний режим, системный прокси (netsh winhttp).
static HINTERNET openSession(const wchar_t* agent)
{
    HINTERNET s = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    return s;
}

// антивирус сразу после записи держит файл на проверке: пробуем читать до 5 с
static bool readRetry(const wstring& p, string& out, ULONGLONG limit, DWORD* le)
{
    for (int i = 0; i < 10; i++) {
        if (readFile(p, out, limit)) return true;
        *le = GetLastError();
        if (g_cancel || *le == ERROR_FILE_NOT_FOUND || *le == ERROR_PATH_NOT_FOUND) break;
        Sleep(500);
    }
    return false;
}

// ---------- проверка подписи архива (ключ разработчика) ----------
static bool verifyArchive(const string& zipData, const string& sigText)
{
    string t = sigText;
    while (!t.empty() && (unsigned char)t.back() <= ' ') t.pop_back();
    size_t st = 0;
    while (st < t.size() && (unsigned char)t[st] <= ' ') st++;
    string sig;
    if (!b64uDecode(t.substr(st), sig) || sig.size() < 128) return false;
    return verifyRsaSha256Key(kReleasePub, (ULONG)sizeof kReleasePub, zipData.data(), zipData.size(), sig);
}

// ---------- распаковка (miniz), защита от выхода за пределы папки ----------
static bool safeZipPath(wstring& w)
{
    for (auto& c : w) if (c == L'/') c = L'\\';
    if (w.empty() || w[0] == L'\\' || w.find(L':') != wstring::npos) return false;
    size_t p = 0;
    while (p <= w.size()) {
        size_t e = w.find(L'\\', p);
        if (e == wstring::npos) e = w.size();
        wstring seg = w.substr(p, e - p);
        bool last = e >= w.size();
        if (seg == L"." || seg == L".." || (seg.empty() && !last)) return false;
        if (!seg.empty() && (seg.back() == L'.' || seg.back() == L' ')) return false;
        p = e + 1;
    }
    return true;
}
// Время файла остаётся таким, как в архиве (как при ручной распаковке): в архиве (-ts-) только DOS-время = стенные часы
// сборщика (+3), miniz отдаёт его как местное. Проверка обновлений (sys.js, checkUpdate) считает время wpanel.gms минским
// и вычитает 3 часа; если файл получит время установки, у пользователей западнее +3 сразу после установки снова
// будет "доступно обновление".
static void setArchiveTime(const wstring& f, time_t t)
{
    if (t <= 0) return;
    HANDLE h = CreateFileW(f.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    ULONGLONG v = ((ULONGLONG)t + 11644473600ULL) * 10000000ULL;
    FILETIME ft; ft.dwLowDateTime = (DWORD)v; ft.dwHighDateTime = (DWORD)(v >> 32);
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
}
static bool extractZip(const string& zipData, const wstring& dest, string* err)
{
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    if (!mz_zip_reader_init_mem(&z, zipData.data(), zipData.size(), 0)) { *err = "not a zip"; return false; }
    mz_uint n = mz_zip_reader_get_num_files(&z);
    bool ok = true;
    for (mz_uint i = 0; i < n && ok; i++) {
        if (g_cancel) { *err = "cancelled"; ok = false; break; }
        mz_zip_archive_file_stat fs;
        if (!mz_zip_reader_file_stat(&z, i, &fs)) { *err = "bad entry"; ok = false; break; }
        string raw = fs.m_filename;
        wstring name = (fs.m_bit_flag & (1 << 11)) ? toWide(raw) : [&]() {
            int len = MultiByteToWideChar(CP_OEMCP, 0, raw.c_str(), (int)raw.size(), nullptr, 0);
            wstring w(len, L'\0');
            MultiByteToWideChar(CP_OEMCP, 0, raw.c_str(), (int)raw.size(), &w[0], len);
            return w;
        }();
        bool isDir = mz_zip_reader_is_file_a_directory(&z, i) != 0;
        if (isDir && !name.empty() && (name.back() == L'/' || name.back() == L'\\')) name.pop_back();
        if (!safeZipPath(name)) { *err = "unsafe path in archive: " + raw; ok = false; break; }
        wstring out = dest + L"\\" + name;
        if (isDir) { makeDirs(out); continue; }
        if (fs.m_uncomp_size > (200ULL << 20)) { *err = "entry too large: " + raw; ok = false; break; }
        size_t sz = 0;
        void* p = mz_zip_reader_extract_to_heap(&z, i, &sz, 0);
        if (!p && fs.m_uncomp_size != 0) { *err = "extract failed: " + raw; ok = false; break; }
        makeDirs(out.substr(0, out.find_last_of(L'\\')));
        if (!writeFile(out, (const char*)p, sz)) { *err = "cannot write " + toUtf8(out); ok = false; }
        else setArchiveTime(out, fs.m_time);
        if (p) mz_free(p);
        progress(60 + (int)(15 * (i + 1) / n));
    }
    mz_zip_reader_end(&z);
    return ok;
}

// ---------- Corel ----------
static BOOL CALLBACK enumClose(HWND h, LPARAM l)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == (DWORD)l && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == nullptr) PostMessageW(h, WM_CLOSE, 0, 0);
    return TRUE;
}
static bool dllLocked(const wstring& addons)
{
    return !FindLockedFilesUpd(addons + L"\\wpanel").empty();
}
// закрыть Corel и дождаться, пока он отпустит proxy.dll/main.dll; false - не закрылся (таймаут/отмена)
static bool closeCorel(DWORD pid, const wstring& addons, int waitSec)
{
    ULONGLONG t0 = GetTickCount64();
    if (pid) {
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (h) {
            status(tr(T_CLOSE));
            EnumWindows(enumClose, (LPARAM)pid);
            bool gone = false;
            while (!gone) {
                if (WaitForSingleObject(h, 300) == WAIT_OBJECT_0) gone = true;
                else if (g_cancel || GetTickCount64() - t0 > (ULONGLONG)waitSec * 1000) break;
            }
            CloseHandle(h);
            if (!gone) return false;
        }
    }
    status(tr(T_FREE));
    while (dllLocked(addons)) {
        if (g_cancel || GetTickCount64() - t0 > (ULONGLONG)(waitSec + 60) * 1000) return false;
        Sleep(300);
    }
    return true;
}
// ---- обновление без закрытия Corel ----
// proxy.dll почти не меняется. Если proxy (32 и 64) в пакете такой же, как установленный, Corel закрывать не нужно:
// main.dll выгружает сам proxy сразу после запуска обновления (UnloadMain), мы только ждём, пока файл освободится.
// Когда всё поставлено, пишем <work>\update.done (код результата), докер по нему перезагружается или сообщает о сбое.
// Не освободился за 30 с (старая proxy, Corel занят) - прежний путь с закрытием Corel.
static bool sameFileContent(const wstring& a, const wstring& b)
{
    string x, y;
    return readFile(a, x, 256ULL << 20) && readFile(b, y, 256ULL << 20) && x == y;
}
// proxy*.dll пакета совпадают с установленными; сравнить было нечего - считаем, что не совпадают (без доказательства Corel закрываем)
static bool proxySame(const wstring& root, const wstring& addons)
{
    const wchar_t* names[] = { L"proxy.dll", L"proxy32.dll" };
    bool compared = false;
    for (const wchar_t* n : names) {
        wstring s = root + L"\\wpanel\\" + n, d = addons + L"\\wpanel\\" + n;
        if (!exists(s) || !exists(d)) continue;
        if (!sameFileContent(s, d)) return false;
        compared = true;
    }
    return compared;
}
// main.dll/main32.dll пакета отличаются от установленных И сейчас заняты (то есть нужна выгрузка)
static bool mainNeedsUnload(const wstring& root, const wstring& addons)
{
    const wchar_t* names[] = { L"bin\\main.dll", L"bin\\main32.dll" };
    for (const wchar_t* n : names) {
        wstring s = root + L"\\wpanel\\" + n, d = addons + L"\\wpanel\\" + n;
        if (!exists(s) || !exists(d)) continue;
        HANDLE h = CreateFileW(d.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); continue; }
        if (GetLastError() == ERROR_SHARING_VIOLATION && !sameFileContent(s, d)) return true;
    }
    return false;
}
static wstring workFile(const wstring& work, const wchar_t* name)
{
    wstring w = work;
    while (!w.empty() && (w.back() == L'\\' || w.back() == L'/')) w.pop_back();
    return w + L"\\" + name;
}
// ждём до sec секунд, пока proxy выгрузит main.dll; true - освободилась
static bool waitMainFree(const wstring& root, const wstring& addons, int sec)
{
    ULONGLONG t0 = GetTickCount64();
    while (mainNeedsUnload(root, addons)) {
        if (g_cancel || GetTickCount64() - t0 > (ULONGLONG)sec * 1000) return false;
        Sleep(300);
    }
    return true;
}

static void stopViewerServer(const wstring& work)
{
    if (work.empty()) return;
    wstring save = g_work;
    g_work = work;
    int port = 0; string token;
    if (readSession(&port, &token)) httpGet(port, "/quit?k=" + token, nullptr);
    g_work = save;
}

// ---------- установщик ----------
static bool runInstaller(const wstring& pkg, const wstring& addons, const wstring& tmp, const wstring& extra, int* code, wstring* text)
{
    wstring result = tmp + L"\\installer_result.txt";
    DeleteFileW(result.c_str());
    wstring args = L"--silent --kind update --target \"" + addons + L"\" --result \"" + result + L"\" --log \"" + tmp + L"\\installer.log\" --wait-locked 30" +
                   (extra.empty() ? L"" : L" " + extra);
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    wstring exe = pkg + L"\\wood_install.exe";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.lpDirectory = pkg.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) { *code = (GetLastError() == ERROR_CANCELLED) ? -1 : -2; return false; }
    WaitForSingleObject(sei.hProcess, 30 * 60 * 1000);
    DWORD ec = 99;
    GetExitCodeProcess(sei.hProcess, &ec);
    CloseHandle(sei.hProcess);
    *code = (int)ec;
    string r;
    if (readFile(result, r, 65536)) *text = toWide(r);
    return ec == 0;
}

static wstring tempDirFor(const wchar_t* tag)
{
    wchar_t t[MAX_PATH * 2];
    DWORD n = GetTempPathW(MAX_PATH * 2, t);
    wstring base(t, n);
    while (!base.empty() && base.back() == L'\\') base.pop_back();
    return base + L"\\" + tag;
}

struct Args {
    wstring addons, corelExe, corelArgs, work, resultFile, installerExtra, stageDir;
    DWORD corelPid = 0;
    bool stage2 = false, keepTemp = false;
    int closeTimeout = 600;            // сек. ждать закрытия Corel (стенд сокращает)
    int unloadTimeout = 30;            // сек. ждать, пока докер выгрузит main.dll
};

static void writeResult(int code, const wstring& msg)
{
    if (g_resultFile.empty()) return;
    string u = toUtf8(code == 0 ? wstring(L"OK") : L"ERR " + std::to_wstring(code) + L" " + msg);
    writeFile(g_resultFile, u.data(), u.size());
}

// ---------- фоновая загрузка в папку (stage) ----------
static void clearStage(const wstring& dir)
{
    // meta.txt первым: без него панель считает папку пустой, даже если остальное не успело удалиться
    const wchar_t* names[] = { L"meta.txt", L"update.zip", L"update.sig", L"update.zip.part" };
    for (const wchar_t* n : names) DeleteFileW((dir + L"\\" + n).c_str());
    RemoveDirectoryW(dir.c_str());
}

// основная работа (в рабочем потоке или синхронно при --no-ui); возвращает код выхода
static int doUpdate(const Args& a, wstring* errText)
{
    wstring tmp = tempDirFor((L"wood_update_" + std::to_wstring(GetCurrentProcessId())).c_str());
    makeDirs(tmp);
    wstring zipPath = a.stageDir + L"\\update.zip", sigPath = a.stageDir + L"\\update.sig", pkg = tmp + L"\\pkg";     // архив и подпись уже скачала панель (Corel)
    wstring root = pkg;                                     // папка с wood_install.exe: корень распаковки или единственная вложенная ("woodman docker")
    string err;
    int rc = 0;
    bool keepCorel = false;                                  // Corel остаётся открытым (proxy не менялся)

    ulog(L"=== update start: stage=" + a.stageDir + L" addons=" + a.addons + L" corelPid=" + std::to_wstring(a.corelPid) + L" corelExe=" + a.corelExe + L" tmp=" + tmp);
    progress(50);                                             // архив уже скачан

    {
        status(tr(T_SIG)); progress(52);
        string zipData, sigText;
        DWORD le = 0;
        if (!readRetry(zipPath, zipData, 512ULL * 1024 * 1024, &le)) { *errText = wstring(tr(E_AV)) + L" (read update.zip, " + std::to_wstring(le) + L")"; rc = 62; goto done; }
        if (!readRetry(sigPath, sigText, 65536, &le)) { *errText = wstring(tr(E_AV)) + L" (read update.sig, " + std::to_wstring(le) + L")"; rc = 62; goto done; }
        ulog(L"downloaded: zip " + std::to_wstring(zipData.size()) + L" bytes, sig " + std::to_wstring(sigText.size()) + L" bytes");
        if (!verifyArchive(zipData, sigText)) { *errText = tr(E_SIG); rc = 61; goto done; }
        ulog(L"signature OK");

        status(tr(T_UNPACK)); progress(60);
        makeDirs(pkg);
        if (!extractZip(zipData, pkg, &err)) { *errText = wstring(tr(E_PKG)) + L" (" + toWide(err) + L")"; rc = 62; goto done; }
        // архив собран вместе с папкой ("woodman docker/wood_install.exe"): корень пакета - единственная вложенная папка
        if (!exists(pkg + L"\\wood_install.exe")) {
            WIN32_FIND_DATAW fd;
            HANDLE fh = FindFirstFileW((pkg + L"\\*").c_str(), &fd);
            if (fh != INVALID_HANDLE_VALUE) {
                do {
                    wstring nm = fd.cFileName;
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && nm != L"." && nm != L".." && exists(pkg + L"\\" + nm + L"\\wood_install.exe")) { root = pkg + L"\\" + nm; break; }
                } while (FindNextFileW(fh, &fd));
                FindClose(fh);
            }
        }
        ulog(L"unpacked, package root: " + root);
        // файл только что записан и пропал - обычно антивирус унёс его в карантин (wood_install.exe без подписи); ждём до 5 с и говорим, какой именно
        const wchar_t* need[2] = { L"\\wood_install.exe", L"\\wpanel\\js\\sys.js" };
        for (int k = 0; k < 2; k++) {
            for (int i = 0; i < 10 && !exists(root + need[k]); i++) Sleep(500);
            if (!exists(root + need[k])) { *errText = wstring(tr(E_AV)) + L" (missing " + (need[k] + 1) + L")"; rc = 62; goto done; }
        }
    }

    // --- дальше трогаем систему: сервер просмотра, Corel, установка ---
    progress(78);
    stopViewerServer(a.work);
    if (a.corelPid && !a.work.empty() && proxySame(root, a.addons)) {
        if (mainNeedsUnload(root, a.addons)) {
            status(tr(T_UNLOAD));
            keepCorel = waitMainFree(root, a.addons, a.unloadTimeout);
            ulog(keepCorel ? L"main.dll unloaded by proxy, Corel stays open" : L"main.dll was not unloaded, closing Corel");
        } else { keepCorel = true; ulog(L"proxy/main unchanged or free, Corel stays open"); }
    }
    if (!keepCorel && !g_noUi && a.corelPid) {                // Corel закрываем только с согласия пользователя (--no-ui: стенды, как раньше)
        HANDLE hc = OpenProcess(SYNCHRONIZE, FALSE, a.corelPid);
        if (hc) {
            bool alive = WaitForSingleObject(hc, 0) == WAIT_TIMEOUT;
            CloseHandle(hc);
            if (alive && !askUser(tr(Q_CLOSE), tr(Q_CLOSE_Y), tr(Q_CLOSE_N), MB_ICONQUESTION)) { *errText = tr(E_NOCLOSE); rc = 63; goto done; }
        }
    }
    if (!keepCorel && !closeCorel(a.corelPid, a.addons, a.closeTimeout)) { *errText = tr(E_CLOSE); rc = 63; goto done; }

    {
        status(tr(T_INST)); progress(85);
        int ic = 0; wstring itext;
        bool ok = runInstaller(root, a.addons, tmp, a.installerExtra, &ic, &itext);
        ulog(L"installer: code " + std::to_wstring(ic) + L" ok=" + (ok ? L"1" : L"0") + L" " + itext);
        if (!ok) {
            if (ic == -1) *errText = tr(E_UAC);
            else {
                *errText = wstring(tr(E_INST)) + L" (" + std::to_wstring(ic) + L"): " + itext;
                if (ic == 6) *errText += L" " + wstring(tr(E_ROLLED));
            }
            rc = 64;
        }
    }

done:
    ulog(L"=== update end: rc=" + std::to_wstring(rc) + (errText->empty() ? L"" : L" error: " + *errText));
    {
        string il;
        if (readFile(tmp + L"\\installer.log", il, 65536)) ulog(L"installer.log:\r\n" + toWide(il));
    }
    // скачанное: поставилось - больше не нужно; подпись/архив негодные - не оставлять, следующий раз скачается заново
    if (rc == 0 || rc == 61 || rc == 62) clearStage(a.stageDir);
    if (!a.work.empty()) {                                   // итог для докера (живой или перезапущенный Corel): 0 - поставлено
        string c = std::to_string(rc);
        writeFile(workFile(a.work, L"update.done"), c.data(), c.size());
    }
    if (!keepCorel && !a.corelExe.empty() && (rc == 0 || rc >= 64) &&      // после установки (в т.ч. при откате) Corel запускаем только по кнопке (--no-ui: стенды, сразу)
        (g_noUi || askUser(rc == 0 ? tr(Q_DONE_OK) : tr(Q_DONE_ER), tr(Q_RUN_Y), tr(Q_RUN_N), rc == 0 ? MB_ICONINFORMATION : MB_ICONWARNING))) {
        status(tr(T_START));
        ShellExecuteW(nullptr, L"open", a.corelExe.c_str(), a.corelArgs.empty() ? nullptr : a.corelArgs.c_str(), nullptr, SW_SHOWNORMAL);
    }
    progress(100);
    // Временные папки wood_update_* в %TEMP% здесь не удаляем (антивирусы принимают самоудаление за признак вредоноса):
    // их чистит докер при запуске (VBA dockerAutoRun). --keep-temp принимается для совместимости.
    return rc;
}

// ---------- окно ----------
struct Work { Args a; int rc = 0; wstring err; };
static Work* g_work2 = nullptr;

static DWORD WINAPI workThread(LPVOID)
{
    g_work2->rc = doUpdate(g_work2->a, &g_work2->err);
    PostMessageW(g_wnd, WM_FINISH, 0, 0);
    return 0;
}
static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_STATUS: { wstring* s = (wstring*)l; SetWindowTextW(g_label, s->c_str()); delete s; return 0; }
    case WM_PROGRESS: SendMessageW(g_bar, PBM_SETPOS, w, 0); return 0;
    case WM_FINISH:
        if (g_work2->rc != 0 && !g_work2->err.empty())
            MessageBoxW(h, g_work2->err.c_str(), tr(T_TITLE), MB_OK | (g_work2->rc == 63 ? MB_ICONWARNING : MB_ICONERROR));
        else SetWindowTextW(g_label, tr(T_DONE));
        DestroyWindow(h);
        return 0;
    case WM_COMMAND:
        if (LOWORD(w) == IDCANCEL || LOWORD(w) == 1001) { InterlockedExchange(&g_cancel, 1); EnableWindow(GetDlgItem(h, 1001), FALSE); }
        return 0;
    case WM_CLOSE: InterlockedExchange(&g_cancel, 1); return 0;      // закрыть окно нельзя, пока идёт работа: только "Отмена"
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static int runWithWindow(Work& w)
{
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);
    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndProc; wc.hInstance = hi; wc.lpszClassName = L"WoodUpdateWnd";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    int W = 440, H = 150;
    g_wnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, tr(T_TITLE), WS_CAPTION | WS_SYSMENU, (GetSystemMetrics(SM_CXSCREEN) - W) / 2, (GetSystemMetrics(SM_CYSCREEN) - H) / 2, W, H, nullptr, nullptr, hi, nullptr);
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    g_label = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 16, 14, 400, 34, g_wnd, nullptr, hi, nullptr);
    g_bar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE, 16, 54, 400, 16, g_wnd, nullptr, hi, nullptr);
    HWND btn = CreateWindowExW(0, L"BUTTON", tr(T_CANCEL), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 326, 80, 90, 24, g_wnd, (HMENU)1001, hi, nullptr);
    SendMessageW(g_label, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(btn, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(g_bar, PBM_SETRANGE32, 0, 100);
    ShowWindow(g_wnd, SW_SHOW);
    g_work2 = &w;
    HANDLE th = CreateThread(nullptr, 0, workThread, nullptr, 0, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (th) { WaitForSingleObject(th, 5000); CloseHandle(th); }
    return w.rc;
}

// ---------- вход ----------
static int run(int argc, wchar_t** argv)
{
    Args a;
    wstring lang = L"ru";
    bool noUi = false;
    for (int i = 2; i < argc; i++) {
        wstring k = argv[i], v;
        auto next = [&](wstring& dst) { if (i + 1 < argc) dst = argv[++i]; };
        if (k == L"--addons") next(a.addons);
        else if (k == L"--corel-exe") next(a.corelExe);
        else if (k == L"--corel-args") next(a.corelArgs);
        else if (k == L"--corel-pid") { next(v); a.corelPid = (DWORD)_wtoi(v.c_str()); }
        else if (k == L"--work") next(a.work);
        else if (k == L"--lang") next(lang);
        else if (k == L"--result") next(g_resultFile);
        else if (k == L"--no-ui") noUi = true;
        else if (k == L"--stage2") a.stage2 = true;
        else if (k == L"--keep-temp") a.keepTemp = true;
        else if (k == L"--from-stage") { }                      // единственный режим; ключ оставлен: его по-прежнему передаёт main.dll
        else if (k == L"--stage-dir") next(a.stageDir);
    }
    g_ru = !(lang.size() >= 2 && (lang.compare(0, 2, L"en") == 0 || lang.compare(0, 2, L"de") == 0 || lang.compare(0, 2, L"es") == 0 || lang.compare(0, 2, L"pt") == 0 || lang.compare(0, 2, L"tr") == 0 || lang.compare(0, 2, L"zh") == 0 || lang.compare(0, 2, L"ar") == 0));
    g_noUi = noUi;
    if (a.stageDir.empty() || a.addons.empty()) { fprintf(stderr, "update: не хватает аргументов (--stage-dir, --addons); архив и подпись скачивает панель (Corel) в stage-dir\n"); return 2; }
    while (a.addons.size() > 3 && (a.addons.back() == L'\\' || a.addons.back() == L'/')) a.addons.pop_back();
    while (a.stageDir.size() > 3 && (a.stageDir.back() == L'\\' || a.stageDir.back() == L'/')) a.stageDir.pop_back();

    if (!a.stage2) {
        // этап 1: уходим во временную копию (установщик заменит bin\wood_server.exe), и сразу возвращаемся
        wstring tdir = tempDirFor((L"wood_update_run_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount())).c_str());
        makeDirs(tdir);
        wstring copy = tdir + L"\\wood_server.exe";
        if (!CopyFileW(selfPath().c_str(), copy.c_str(), FALSE)) { fprintf(stderr, "update: не удалось скопировать себя в temp\n"); return 65; }
        wstring cmd = L"\"" + copy + L"\" update --stage2";
        for (int i = 2; i < argc; i++) cmd += L" \"" + wstring(argv[i]) + L"\"";
        vector<wchar_t> b(cmd.begin(), cmd.end()); b.push_back(0);
        STARTUPINFOW si = {}; si.cb = sizeof si; PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(nullptr, b.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, tdir.c_str(), &si, &pi)) return 65;
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return 0;
    }

    Work w;
    w.a = a;
    int rc;
    if (noUi) { rc = doUpdate(a, &w.err); if (rc && !w.err.empty()) status(w.err); }
    else { rc = runWithWindow(w); w.rc = rc; }
    writeResult(rc, w.err);
    return rc;
}

// ---------- api: запросы докера к сайту ----------
// Действия JSON-API (www\panel_api.php, протокол v1). Список закрытый: exe не умеет ходить никуда, кроме панельного API.
static bool apiAllowed(const string& a)
{
    static const char* ok[] = {"update_check", "key", "update_link", "update_get", "stat", "ack", "news", "release", nullptr};
    for (int i = 0; ok[i]; i++) if (a == ok[i]) return true;
    return false;
}

// Класс сбоя связи по коду WinHTTP: докер различает "нет связи" (net), "не разрешился адрес" (dns) и т. п.
static const char* apiNetClass(DWORD e)
{
    if (e == ERROR_WINHTTP_TIMEOUT) return "timeout";
    if (e == ERROR_WINHTTP_NAME_NOT_RESOLVED) return "dns";
    if (e == ERROR_WINHTTP_CANNOT_CONNECT || e == ERROR_WINHTTP_CONNECTION_ERROR) return "connect";
    if (e == ERROR_WINHTTP_SECURE_FAILURE || e == ERROR_WINHTTP_SECURE_CERT_DATE_INVALID || e == ERROR_WINHTTP_SECURE_CERT_CN_INVALID ||
        e == ERROR_WINHTTP_SECURE_INVALID_CA || e == ERROR_WINHTTP_SECURE_CERT_REV_FAILED || e == ERROR_WINHTTP_SECURE_INVALID_CERT) return "tls";
    return "err";
}

// POST JSON, тело ответа до 4 МБ. false - связи нет (lastErr - код WinHTTP).
static bool apiPost(const wstring& url, const string& body, int timeoutSec, string& resp, DWORD& status, DWORD& lastErr)
{
    wchar_t host[256] = {}, path[2048] = {};
    URL_COMPONENTSW uc = {};
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2047;
    lastErr = 0;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { lastErr = GetLastError(); return false; }
    HINTERNET ses = openSession(L"woodman-api/1.0");
    if (!ses) { lastErr = GetLastError(); return false; }
    DWORD protos = 0x00000800 | 0x00000200 | 0x00000080;           // TLS 1.2 | 1.1 | 1.0 (на Win7 TLS 1.2 надо включать явно)
    WinHttpSetOption(ses, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof protos);
    int ms = timeoutSec * 1000;
    WinHttpSetTimeouts(ses, 8000, 8000, ms, ms);
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"POST", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    bool ok = false;
    const wchar_t* hdr = L"Content-Type: application/json; charset=utf-8\r\nAccept: application/json";
    if (req && WinHttpSendRequest(req, hdr, (DWORD)-1, (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) && WinHttpReceiveResponse(req, nullptr)) {
        DWORD sz = sizeof status;
        status = 0;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        resp.clear();
        char buf[16384];
        ok = true;
        for (;;) {
            DWORD n = 0;
            if (!WinHttpReadData(req, buf, sizeof buf, &n)) { lastErr = GetLastError(); ok = false; break; }
            if (n == 0) break;
            if (resp.size() + n > 4u * 1024 * 1024) break;
            resp.append(buf, n);
        }
    } else lastErr = GetLastError();
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

// Файл ответа появляется целиком: пишется во временный и переименовывается, докер ждёт именно его появления.
static bool apiWriteOut(const wstring& out, const string& env)
{
    wstring tmp = out + L".tmp";
    DeleteFileW(tmp.c_str());
    if (!writeFile(tmp, env.data(), env.size())) return false;
    if (!MoveFileExW(tmp.c_str(), out.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(tmp.c_str()); return false; }
    return true;
}

// Уборка: файлы запросов докера %TEMP%\wood_api_* старше часа (докер перезагрузили посреди запроса). В них почта и серийник.
static void apiSweepTemp()
{
    wchar_t dir[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n > MAX_PATH) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((wstring(dir) + L"wood_api_*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER nu; nu.LowPart = now.dwLowDateTime; nu.HighPart = now.dwHighDateTime;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ULARGE_INTEGER fu; fu.LowPart = fd.ftLastWriteTime.dwLowDateTime; fu.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        if (nu.QuadPart > fu.QuadPart && nu.QuadPart - fu.QuadPart > 36000000000ULL) DeleteFileW((wstring(dir) + fd.cFileName).c_str());   // 1 час в 100 нс
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// api --in запрос.json --out ответ.json [--timeout сек]: один запрос докера к сайту. Идёт отсюда, а не из Corel: у Corel выход в интернет
// может быть закрыт, у wood_server - нет. Адрес зашит (https://mrwoodman.by/panel_api.php), действие - из apiAllowed и первым полем запроса
// ({"a":"stat",...}). Подписка/проверка почты - дело сервера, запрос идёт как есть.
// Ответ - конверт: {"net":"ok","http":200,"body":{...ответ сайта...}} (ответ не JSON-объект - "text":"..."), либо
// {"net":"timeout|dns|connect|tls|err","http":0,"detail":"..."}. Код выхода: 0 - конверт записан (в том числе при сбое связи), 1 - файл
// не записался, 2 - аргументы или действие не из списка
// (если --out известен, в нём конверт net=fail). Окон нет.
static int api(int argc, wchar_t** argv)
{
    wstring in, out;
    int timeoutSec = 20;
    for (int i = 2; i < argc; i++) {
        wstring k = argv[i];
        if (k == L"--in" && i + 1 < argc) in = argv[++i];
        else if (k == L"--out" && i + 1 < argc) out = argv[++i];
        else if (k == L"--timeout" && i + 1 < argc) timeoutSec = _wtoi(argv[++i]);
    }
    if (in.empty() || out.empty()) { fprintf(stderr, "api: нужны --in и --out\n"); return 2; }
    if (timeoutSec < 3) timeoutSec = 3;
    if (timeoutSec > 120) timeoutSec = 120;

    apiSweepTemp();
    // Докер запускает exe скрытым и без ожидания, готовность определяет по появлению файла ответа: он пишется всегда, в том числе при отказе.
    auto refuse = [&](const char* why) {
        string e = string("{\"net\":\"fail\",\"http\":0,\"detail\":\"") + why + "\"}";
        apiWriteOut(out, e);
        return 2;
    };

    string body;
    if (!readFile(in, body, 1u << 20)) { fprintf(stderr, "api: не прочитан %s\n", toUtf8(in).c_str()); return refuse("read"); }
    if (body.size() >= 3 && (unsigned char)body[0] == 0xEF && (unsigned char)body[1] == 0xBB && (unsigned char)body[2] == 0xBF) body.erase(0, 3);   // BOM от ADODB.Stream
    const string head = "{\"a\":\"";
    string act;
    if (body.compare(0, head.size(), head) == 0)
        for (size_t p = head.size(); p < body.size() && body[p] != '"' && act.size() < 32; p++) act += body[p];
    if (!apiAllowed(act)) { fprintf(stderr, "api: действие не разрешено\n"); return refuse("action"); }

    wstring url = L"https://mrwoodman.by/panel_api.php";

    string resp, env;
    DWORD status = 0, le = 0;
    if (!apiPost(url, body, timeoutSec, resp, status, le)) {
        env = string("{\"net\":\"") + apiNetClass(le) + "\",\"http\":0,\"detail\":\"winhttp " + std::to_string(le) + "\"}";
    } else {
        size_t b = resp.find_first_not_of(" \t\r\n"), e = resp.find_last_not_of(" \t\r\n");
        string t = (b == string::npos) ? string() : resp.substr(b, e - b + 1);
        bool obj = t.size() >= 2 && t.front() == '{' && t.back() == '}';
        env = "{\"net\":\"ok\",\"http\":" + std::to_string(status) + (obj ? ",\"body\":" + t : ",\"text\":\"" + jsonEsc(t.substr(0, 2000)) + "\"") + "}";
    }
    return apiWriteOut(out, env) ? 0 : 1;
}

} // namespace upd
