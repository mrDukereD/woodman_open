// wood_server.exe - локальный сервер модуля рендера коробки (замена Node: viewer_server.js + batch.js).
//
//   wood_server.exe render --code <папка кода> --work <рабочая папка> [--port N] [--token T]
//                          [--portfile файл] [--verbose]
//
// Код (viewer.html, src\, node_modules\three\) читается из --code, пользовательские данные
// (config.js, input\, output\, manual.log) - из --work. Слушает ТОЛЬКО 127.0.0.1. Если задан --token,
// запросы без него отклоняются (первый запрос ?k=TOKEN, дальше страница ходит с cookie wk).
// Только Win32 + Winsock2: без std::thread/filesystem и сторонних библиотек (цель - Win7 SP1, x86).
// Выходит сам: нет пингов страницы 20 с, или страницу так и не открыли за 3 мин, или GET /quit.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <bcrypt.h>
#include <winhttp.h>
#include <algorithm>
#include <functional>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "winhttp.lib")

using std::string;
using std::wstring;
using std::vector;

// ---------- настройки ----------
static wstring g_code, g_work;
static string g_token;
static int g_port = 47653;
static bool g_verbose = false;
static bool g_noWindow = false;               // тесты: не открывать окно просмотра
static ULONGLONG g_started = 0;                 // GetTickCount64 на старте
static volatile LONG64 g_lastPing = 0;          // GetTickCount64 последнего /ping (0 - не было)
static double g_startVer = 0;                   // версия кода на момент запуска

// ---------- строки ----------
static string toUtf8(const wstring& w) {
    if (w.empty()) return string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static wstring toWide(const string& s) {
    if (s.empty()) return wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static string lower(string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return s;
}
static bool endsWithI(const string& s, const string& suf) {
    return s.size() >= suf.size() && lower(s.substr(s.size() - suf.size())) == suf;
}
static bool startsWith(const string& s, const string& pre) { return s.compare(0, pre.size(), pre) == 0; }

static string jsonEsc(const string& s) {
    string o;
    for (unsigned char c : s) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c < 0x20) { char b[8]; sprintf_s(b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}

// %XX и + не трогаем (в пути +  - обычный символ); возвращает false на мусоре
static bool pctDecode(const string& in, string& out) {
    out.clear();
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '%') {
            if (i + 2 >= in.size()) return false;
            auto hx = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int a = hx(in[i + 1]), b = hx(in[i + 2]);
            if (a < 0 || b < 0) return false;
            out += (char)(a * 16 + b);
            i += 2;
        } else out += in[i];
    }
    return true;
}

// ---------- файлы ----------
static bool fileInfo(const wstring& p, double* mtimeMs, ULONGLONG* size) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return false;
    if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    ULARGE_INTEGER u; u.LowPart = d.ftLastWriteTime.dwLowDateTime; u.HighPart = d.ftLastWriteTime.dwHighDateTime;
    if (mtimeMs) *mtimeMs = (double)((u.QuadPart - 116444736000000000ULL) / 10000ULL);
    if (size) { ULARGE_INTEGER s; s.LowPart = d.nFileSizeLow; s.HighPart = d.nFileSizeHigh; *size = s.QuadPart; }
    return true;
}
static bool readFile(const wstring& p, string& out, ULONGLONG limit = 512ULL * 1024 * 1024) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || (ULONGLONG)sz.QuadPart > limit) { CloseHandle(h); return false; }
    out.resize((size_t)sz.QuadPart);
    size_t got = 0;
    while (got < out.size()) {
        DWORD n = 0;
        if (!ReadFile(h, &out[got], (DWORD)std::min<size_t>(out.size() - got, 1 << 24), &n, nullptr) || n == 0) break;
        got += n;
    }
    CloseHandle(h);
    out.resize(got);
    return true;
}
static bool writeFile(const wstring& p, const char* data, size_t len, bool append = false) {
    HANDLE h = CreateFileW(p.c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    size_t done = 0;
    while (done < len) {
        DWORD n = 0;
        if (!WriteFile(h, data + done, (DWORD)std::min<size_t>(len - done, 1 << 24), &n, nullptr) || n == 0) { CloseHandle(h); return false; }
        done += n;
    }
    CloseHandle(h);
    return true;
}
static void makeDirs(wstring p) {                 // mkdir -p
    for (size_t i = 1; i < p.size(); i++)
        if (p[i] == L'\\' && p[i - 1] != L':') { wstring q = p.substr(0, i); CreateDirectoryW(q.c_str(), nullptr); }
    CreateDirectoryW(p.c_str(), nullptr);
}
static bool exists(const wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// список имён файлов папки по маске
static vector<wstring> listDir(const wstring& dir, const wstring& mask = L"*") {
    vector<wstring> r;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\" + mask).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return r;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) r.push_back(fd.cFileName); } while (FindNextFileW(h, &fd));
    FindClose(h);
    return r;
}

// ---------- JSON-проверка (только "правильный JSON", значение не нужно) ----------
struct JsonCheck {
    const string& s; size_t i = 0; int depth = 0;
    explicit JsonCheck(const string& str) : s(str) {}
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++; }
    bool lit(const char* w) { size_t n = strlen(w); if (s.compare(i, n, w) != 0) return false; i += n; return true; }
    bool str() {
        if (i >= s.size() || s[i] != '"') return false;
        i++;
        while (i < s.size()) {
            unsigned char c = (unsigned char)s[i++];
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c == '\\') {
                if (i >= s.size()) return false;
                char e = s[i++];
                if (e == 'u') { for (int k = 0; k < 4; k++) { if (i >= s.size() || !isxdigit((unsigned char)s[i])) return false; i++; } }
                else if (!strchr("\"\\/bfnrt", e)) return false;
            }
        }
        return false;
    }
    bool num() {
        size_t st = i;
        if (i < s.size() && s[i] == '-') i++;
        if (i >= s.size() || !isdigit((unsigned char)s[i])) return false;
        if (s[i] == '0') i++; else while (i < s.size() && isdigit((unsigned char)s[i])) i++;
        if (i < s.size() && s[i] == '.') { i++; if (i >= s.size() || !isdigit((unsigned char)s[i])) return false; while (i < s.size() && isdigit((unsigned char)s[i])) i++; }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            i++; if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
            if (i >= s.size() || !isdigit((unsigned char)s[i])) return false;
            while (i < s.size() && isdigit((unsigned char)s[i])) i++;
        }
        return i > st;
    }
    bool val() {
        ws();
        if (i >= s.size() || ++depth > 200) return false;
        bool ok = false;
        char c = s[i];
        if (c == '{') {
            i++; ws();
            if (i < s.size() && s[i] == '}') { i++; ok = true; }
            else {
                ok = true;
                for (;;) {
                    ws(); if (!str()) { ok = false; break; }
                    ws(); if (i >= s.size() || s[i++] != ':') { ok = false; break; }
                    if (!val()) { ok = false; break; }
                    ws(); if (i >= s.size()) { ok = false; break; }
                    if (s[i] == ',') { i++; continue; }
                    if (s[i] == '}') { i++; break; }
                    ok = false; break;
                }
            }
        } else if (c == '[') {
            i++; ws();
            if (i < s.size() && s[i] == ']') { i++; ok = true; }
            else {
                ok = true;
                for (;;) {
                    if (!val()) { ok = false; break; }
                    ws(); if (i >= s.size()) { ok = false; break; }
                    if (s[i] == ',') { i++; continue; }
                    if (s[i] == ']') { i++; break; }
                    ok = false; break;
                }
            }
        } else if (c == '"') ok = str();
        else if (c == 't') ok = lit("true");
        else if (c == 'f') ok = lit("false");
        else if (c == 'n') ok = lit("null");
        else ok = num();
        depth--;
        return ok;
    }
    bool valid() { if (!val()) return false; ws(); return i == s.size(); }
};

// ---------- HTTP ----------
struct Req {
    string method, path, query, body;
    std::map<string, string> h;                    // имена заголовков в нижнем регистре
    string q(const string& key) const {            // значение ?key=...
        size_t p = 0;
        while (p < query.size()) {
            size_t e = query.find('&', p); if (e == string::npos) e = query.size();
            string kv = query.substr(p, e - p); size_t eq = kv.find('=');
            if (kv.substr(0, eq) == key) { string v; pctDecode(eq == string::npos ? "" : kv.substr(eq + 1), v); return v; }
            p = e + 1;
        }
        return string();
    }
};

static thread_local bool t_sendFail = false;       // отправка ответа оборвалась (браузер закрыл соединение): пишется в журнал рендера
static thread_local int t_sendErr = 0;
static bool sendAll(SOCKET s, const char* d, size_t n) {
    while (n > 0) {
        int k = send(s, d, (int)std::min<size_t>(n, 32 << 10), 0);   // кусками: у части машин (фильтры антивируса / файрвола на loopback) большой send рвёт соединение RST
        if (k <= 0) { t_sendFail = true; t_sendErr = WSAGetLastError(); return false; }
        d += k; n -= k;
    }
    return true;
}
static void reply(SOCKET s, int code, const string& body, const char* type = "text/plain; charset=utf-8", const string& extra = string()) {
    const char* txt = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 403 ? "Forbidden" : code == 404 ? "Not Found"
                    : code == 405 ? "Method Not Allowed" : code == 411 ? "Length Required" : code == 413 ? "Payload Too Large" : "Error";
    char head[512];
    int n = sprintf_s(head, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %llu\r\nCache-Control: no-store\r\nConnection: close\r\n",
                      code, txt, type, (unsigned long long)body.size());
    string h(head, n);
    h += extra; h += "\r\n";
    sendAll(s, h.data(), h.size());
    if (!body.empty()) sendAll(s, body.data(), body.size());
}
static const char* kJson = "application/json";
static void replyJson(SOCKET s, int code, const string& body, const string& extra = string()) { reply(s, code, body, kJson, extra); }

static const char* mimeOf(const string& name) {
    string e = lower(name.substr(name.find_last_of('.') == string::npos ? name.size() : name.find_last_of('.')));
    if (e == ".html") return "text/html; charset=utf-8";
    if (e == ".js" || e == ".mjs") return "text/javascript; charset=utf-8";
    if (e == ".svg") return "image/svg+xml";
    if (e == ".json") return kJson;
    if (e == ".css") return "text/css";
    if (e == ".png") return "image/png";
    if (e == ".jpg" || e == ".jpeg") return "image/jpeg";
    if (e == ".webp") return "image/webp";
    if (e == ".txt") return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

// ---------- логика (то, что делал viewer_server.js) ----------
static wstring inDir() { return g_work + L"\\input"; }
// Папка снимков/STL/GLB: по умолчанию work\output; если в config.js outputDir задан абсолютным путём
// (докер: Ctrl на кнопке «Папка output») - берём его. Относительный ('./output') - как раньше.
static wstring outDir() {
    wstring def = g_work + L"\\output";
    string cfg;
    if (!readFile(g_work + L"\\config.js", cfg, 1024 * 1024)) return def;
    size_t k = cfg.find("outputDir");
    if (k == string::npos) return def;
    size_t a = cfg.find_first_of("'\"", k);
    if (a == string::npos) return def;
    size_t b = cfg.find(cfg[a], a + 1);
    if (b == string::npos) return def;
    wstring p = toWide(cfg.substr(a + 1, b - a - 1));
    for (auto& c : p) if (c == L'/') c = L'\\';
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    bool abs = p.size() > 2 && ((p[1] == L':' && p[2] == L'\\') || (p[0] == L'\\' && p[1] == L'\\'));
    return abs ? p : def;
}

static bool matchView(const string& n, long* num) {         // ^[A-Za-z0-9_-]+_view_(\d+)\.png$
    if (!endsWithI(n, ".png") || n.size() < 12) return false;
    string b = n.substr(0, n.size() - 4);
    size_t p = b.rfind("_view_");
    if (p == string::npos || p == 0) return false;
    string d = b.substr(p + 6);
    if (d.empty()) return false;
    for (char c : d) if (c < '0' || c > '9') return false;
    for (size_t i = 0; i < p; i++) { char c = b[i]; if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) return false; }
    if (num) *num = atol(d.c_str());
    return true;
}

struct Svg { string name; double m = 0; bool ok = false; };
static Svg newestSvg() {
    Svg best;
    for (const wstring& w : listDir(inDir())) {
        string n = toUtf8(w);
        if (!endsWithI(n, ".svg")) continue;
        double m;
        if (!fileInfo(inDir() + L"\\" + w, &m, nullptr)) continue;
        if (!best.ok || m > best.m) { best.name = n; best.m = m; best.ok = true; }
    }
    return best;
}
struct Shot { string name; long n; double m; };
static vector<string> shots() {
    vector<Shot> v;
    for (const wstring& w : listDir(outDir())) {
        string n = toUtf8(w); long num;
        if (!matchView(n, &num)) continue;
        double m = 0; fileInfo(outDir() + L"\\" + w, &m, nullptr);
        v.push_back({ n, num, m });
    }
    std::stable_sort(v.begin(), v.end(), [](const Shot& a, const Shot& b) { return a.m != b.m ? a.m < b.m : a.n < b.n; });
    vector<string> r;
    for (auto& s : v) r.push_back(s.name + "?" + std::to_string((long long)s.m));
    return r;
}
static string fmtNum(double d) { char b[40]; sprintf_s(b, "%.0f", d); return b; }
static double codeVer() {
    double mx = 0;
    const wchar_t* files[] = { L"\\viewer.html", L"\\src\\renderer.js", L"\\src\\box_solver.js", L"\\src\\flex.js", L"\\src\\patch_scene.js" };
    for (auto f : files) { double m; if (fileInfo(g_code + f, &m, nullptr)) mx = std::max(mx, m); }
    return mx;
}
static bool safeOutName(const string& n);
static string versionJson() {
    Svg svg = newestSvg();
    double cfg = 0; fileInfo(g_work + L"\\config.js", &cfg, nullptr);
    double pm = 0; fileInfo(inDir() + L"\\patch\\patch_names.json", &pm, nullptr);      // «Передать в 3Д»: файл имён пишется последним, его время - сигнал окну
    string sh = "[";
    auto list = shots();
    for (size_t i = 0; i < list.size(); i++) { if (i) sh += ","; sh += "\"" + jsonEsc(list[i]) + "\""; }
    sh += "]";
    string st = "[";                                             // STL текущей сцены: не старше SVG (старые файлы других сцен в output не считаются); их берёт «Поделиться»
    if (svg.ok) {
        bool first = true;
        for (const wstring& w : listDir(outDir(), L"*.stl")) {
            string n = toUtf8(w); double m = 0;
            if (!safeOutName(n) || !fileInfo(outDir() + L"\\" + w, &m, nullptr) || m < svg.m) continue;
            st += string(first ? "" : ",") + "\"" + jsonEsc(n) + "\""; first = false;
        }
    }
    st += "]";
    string cd = "[";                                             // CDR выделения (box render / shadow box: галочка CDR): тот же отбор, что у STL - не старше SVG
    if (svg.ok) {
        bool first = true;
        for (const wstring& w : listDir(outDir(), L"*.cdr")) {
            string n = toUtf8(w); double m = 0;
            if (!safeOutName(n) || !fileInfo(outDir() + L"\\" + w, &m, nullptr) || m < svg.m) continue;
            cd += string(first ? "" : ",") + "\"" + jsonEsc(n) + "\""; first = false;
        }
    }
    cd += "]";
    string r = "{\"svg\":\"" + (svg.ok ? "input/" + jsonEsc(svg.name) : string()) + "\",\"ver\":\"" +
               (svg.ok ? fmtNum(svg.m) + "_" + fmtNum(cfg) : string()) + "\",\"patch\":\"" + (pm > 0 ? fmtNum(pm) : string()) + "\",\"shots\":" + sh + ",\"stls\":" + st + ",\"cdrs\":" + cd + ",\"code\":" + fmtNum(codeVer()) + "}";
    return r;
}

// картинка текстуры: textures/[тип/]файл.jpg|jpeg|png|webp (не глубже одной подпапки)
static bool isTextureExt(const string& n) { return endsWithI(n, ".jpg") || endsWithI(n, ".jpeg") || endsWithI(n, ".png") || endsWithI(n, ".webp"); }
static bool isTextureName(const string& rel) {
    if (!startsWith(rel, "textures/") || !isTextureExt(rel)) return false;
    size_t a = rel.find('/', 9);                      // после "textures/": либо файл, либо тип/файл
    return a == string::npos || rel.find('/', a + 1) == string::npos;
}

// что можно отдавать: страница, исходники и three.js, config.js, входные SVG, снимки, текстуры
static bool allowed(const string& rel) {
    long n;
    if (rel == "viewer.html" || rel == "batch.html" || rel == "config.js") return true;
    if (startsWith(rel, "src/") || startsWith(rel, "vendor/three/") || startsWith(rel, "vendor/mp4-muxer/")) return true;
    if (startsWith(rel, "input/") && endsWithI(rel, ".svg")) return true;
    if (startsWith(rel, "input/") && isTextureExt(rel)) {                                   // растры из SVG: input/<имя>_Images/<файл> (Corel кладёт их рядом с SVG)
        size_t a = rel.find('/', 6);
        if (a != string::npos && rel.find('/', a + 1) == string::npos) return true;
    }
    if (startsWith(rel, "textures/") && isTextureName(rel)) return true;                 // textures/<тип>/<файл> или textures/<файл>
    // снимки из окна (*_view_N.png) и кадры камер рендера (<имя>_<камера>.png): окно показывает их в ленте; только PNG, только прямо в папке output
    if (startsWith(rel, "output/") && rel.find('/', 7) == string::npos && (matchView(rel.substr(7), &n) || endsWithI(rel, ".png") || endsWithI(rel, ".stl") || endsWithI(rel, ".cdr"))) return true;       // .stl - для «Поделиться» (список в /ping: stls)
    return false;
}
static bool isWorkPath(const string& rel) { return rel == "config.js" || startsWith(rel, "input/") || startsWith(rel, "output/"); }

// безопасный относительный путь из URL: без .., \, :, пустых и "хвостовых" сегментов
static bool safeRel(const string& decoded, string& rel) {
    rel = decoded;
    while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
    if (rel.empty()) return true;
    if (rel.find('\\') != string::npos || rel.find(':') != string::npos || rel.find('\0') != string::npos) return false;
    size_t p = 0;
    while (p <= rel.size()) {
        size_t e = rel.find('/', p); if (e == string::npos) e = rel.size();
        string seg = rel.substr(p, e - p);
        if (seg.empty() || seg == "." || seg == ".." || seg.back() == '.' || seg.back() == ' ') return false;
        p = e + 1;
    }
    return true;
}

static string isoNow() {
    SYSTEMTIME t; GetSystemTime(&t);
    char b[40]; sprintf_s(b, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return b;
}

// ---------- состояние пакетного рендера (batch) ----------
static HANDLE g_doneEvent = nullptr;
static volatile LONG g_pageStarted = 0, g_batchOk = 0, g_wantView = 0, g_viewPort = 47653;
static volatile LONG64 g_lastActivity = 0;
static CRITICAL_SECTION g_logCs;

// строка в консоль (UTF-8) и в render.log рядом с данными пользователя
static ULONGLONG g_batchT0 = 0;                // старт пакетного рендера (GetTickCount64); 0 - не пакетный режим
static bool g_batchNoUi = false;               // --no-ui: без окна прогресса и сообщений (стенд)
static string g_lastErr;                       // последняя ошибка страницы (для сообщения пользователю)
namespace prog { static void set(const wstring& s); }
static void logLine(const string& t0) {
    EnterCriticalSection(&g_logCs);
    string t = t0;
    if (g_batchT0) { char b[32]; sprintf_s(b, "[+%6.2f с] ", (double)(GetTickCount64() - g_batchT0) / 1000.0); t = b + t0; }
    fwrite(t.data(), 1, t.size(), stdout); fputc('\n', stdout); fflush(stdout);
    if (g_batchT0 && t0.compare(0, 7, "WARNING") != 0) prog::set(toWide(t0));      // в окне прогресса - последняя строка, без предупреждений
    string l = t + "\n";
    writeFile(g_work + L"\\render.log", l.data(), l.size(), true);
    LeaveCriticalSection(&g_logCs);
}
static void touchActivity() { InterlockedExchange64(&g_lastActivity, (LONG64)GetTickCount64()); }

// имя выходного файла: без путей и служебных символов, только известные расширения
static bool safeOutName(const string& n) {
    if (n.empty() || n.size() > 200 || n[0] == '.') return false;
    for (unsigned char c : n) if (c < 0x20 || strchr("\\/:*?\"<>|", c)) return false;
    const char* ext[] = { ".png", ".stl", ".cdr", ".glb", ".mp4", ".webm", ".gif", ".svg" };       // .svg - планы деталей схемы сборки (<имя>_step_NN_plan.svg)
    for (auto e : ext) if (endsWithI(n, e)) return true;
    return false;
}
static vector<wstring> listSubDirs(const wstring& dir) {
    vector<wstring> r;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return r;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0)
            r.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return r;
}
// {"types":[{"name":"fanera","files":[{"n":"oak.jpg","m":123}]}]}: тип материала = подпапка textures; файлы прямо в textures - тип "" (m - время файла, сброс кэша)
// Текстуры лежат в двух местах: поставка докера (g_code\textures, читается только) и папка пользователя (g_work\textures, Мои документы).
// Одно имя тип/файл в обеих - побеждает пользовательская (решение 19.10.2026: поставка не копируется, а накладывается).
static wstring textureFullPath(const string& rel) {          // rel = textures/[тип/]файл; пользовательская папка главнее поставки
    wstring w = toWide(rel);
    for (auto& c : w) if (c == L'/') c = L'\\';
    wstring u = g_work + L"\\" + w;
    if (g_code == g_work || exists(u)) return u;
    return g_code + L"\\" + w;
}
static string texturesJson() {
    std::map<string, std::map<string, double>> types;           // тип -> файл -> время; "" - файлы прямо в textures
    auto scan = [&](const wstring& root) {
        auto one = [&](const wstring& dir, const string& type) {
            auto& files = types[type];
            for (const wstring& w : listDir(dir)) {
                string n = toUtf8(w);
                if (!isTextureExt(n) || files.count(n)) continue;
                double m = 0; fileInfo(dir + L"\\" + w, &m, nullptr);
                files[n] = m;
            }
        };
        one(root, "");
        for (const wstring& d : listSubDirs(root)) one(root + L"\\" + d, toUtf8(d));
    };
    scan(g_work + L"\\textures");                              // сначала пользовательские: при совпадении имени остаются они
    if (g_code != g_work) scan(g_code + L"\\textures");
    string r = "{\"types\":[";
    bool firstT = true;
    for (auto& t : types) {
        if (!firstT) r += ",";
        firstT = false;
        r += "{\"name\":\"" + jsonEsc(t.first) + "\",\"files\":[";
        bool first = true;
        for (auto& f : t.second) {
            if (!first) r += ",";
            first = false;
            r += "{\"n\":\"" + jsonEsc(f.first) + "\",\"m\":" + fmtNum(f.second) + "}";
        }
        r += "]}";
    }
    return r + "]}";
}
static vector<string> inputSvgs() {
    vector<string> r;
    for (const wstring& w : listDir(inDir())) { string n = toUtf8(w); if (endsWithI(n, ".svg")) r.push_back(n); }
    return r;
}

// ---------- сохранённые сборки: work\saves\<имя>\ ----------
// В сохранении: input\ целиком (SVG, <имя>_Images, pins.json, materials.json), config.js, файлы output (PNG/STL/GLB), render.log, manual.log.
// Не входят: textures\ (общие, большие), license.dat, viewer.session. Все операции - только внутри work\saves.
static void rmTree(const wstring& dir);
static wstring savesDir() { return g_work + L"\\saves"; }
static bool safeSaveName(const string& n, wstring& w) {
    if (n.empty() || n.size() > 120) return false;
    for (unsigned char c : n) if (c < 0x20 || strchr("\\/:*?\"<>|", c)) return false;
    if (n.front() == ' ' || n.back() == ' ' || n.front() == '.' || n.back() == '.') return false;
    string b = lower(n.substr(0, n.find('.')));
    while (!b.empty() && b.back() == ' ') b.pop_back();
    if (b == "con" || b == "prn" || b == "aux" || b == "nul") return false;
    if (b.size() == 4 && (b.compare(0, 3, "com") == 0 || b.compare(0, 3, "lpt") == 0) && b[3] >= '0' && b[3] <= '9') return false;
    w = toWide(n);
    return !w.empty();
}
static bool copyTree(const wstring& src, const wstring& dst) {            // файлы и подпапки (не ссылки); false - что-то не скопировалось
    bool ok = true;
    makeDirs(dst);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((src + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return true;
    do {
        wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && !copyTree(src + L"\\" + n, dst + L"\\" + n)) ok = false;
        } else if (!CopyFileW((src + L"\\" + n).c_str(), (dst + L"\\" + n).c_str(), FALSE)) ok = false;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}
static bool copyOutputFiles(const wstring& src, const wstring& dst) {      // только файлы рендера (png/stl/glb/...) прямо в папке
    bool ok = true;
    makeDirs(dst);
    for (const wstring& w : listDir(src))
        if (safeOutName(toUtf8(w)) && !CopyFileW((src + L"\\" + w).c_str(), (dst + L"\\" + w).c_str(), FALSE)) ok = false;
    return ok;
}
static void copyIfExists(const wstring& a, const wstring& b) { if (exists(a)) CopyFileW(a.c_str(), b.c_str(), FALSE); }
static void touchNow(const wstring& f) {                                     // новое время файла: окно просмотра по нему видит смену сцены
    HANDLE h = CreateFileW(f.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
}
struct SaveInfo { string name; double m; };
static vector<SaveInfo> listSaves() {
    vector<SaveInfo> r;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((savesDir() + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return r;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        ULARGE_INTEGER u; u.LowPart = fd.ftLastWriteTime.dwLowDateTime; u.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        r.push_back({ toUtf8(fd.cFileName), (double)((u.QuadPart - 116444736000000000ULL) / 10000ULL) });
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::stable_sort(r.begin(), r.end(), [](const SaveInfo& a, const SaveInfo& b) { return a.m > b.m; });
    return r;
}
// миниатюра сохранения: первый кадр камеры (*_Camera_1.png), иначе любая картинка рендера (не снимок из окна)
static wstring saveThumb(const wstring& dir) {
    if (exists(dir + L"\\thumb.png")) return dir + L"\\thumb.png";      // снимок окна на момент сохранения (лежит вне output: в ленту снимков и «Импорт PNG» не попадает)
    wstring out = dir + L"\\output", any;
    for (const wstring& w : listDir(out)) {
        string n = toUtf8(w); long k;
        if (!endsWithI(n, ".png")) continue;
        if (endsWithI(n, "_camera_1.png")) return out + L"\\" + w;
        if (any.empty() && !matchView(n, &k)) any = out + L"\\" + w;
    }
    return any;
}
// POST /saves/<save|load|delete|rename>?name=..[&to=..][&overwrite=1]; ответ {"ok":true} либо {"ok":false,"error":..,"exists":true}
static void savesAction(SOCKET s, const Req& rq, const string& act, const string& extra) {
    auto fail = [&](int code, const string& err, bool ex = false) {
        replyJson(s, code, "{\"ok\":false,\"error\":\"" + jsonEsc(err) + "\"" + (ex ? ",\"exists\":true" : "") + "}", extra);
    };
    wstring wn;
    if (!safeSaveName(rq.q("name"), wn)) return fail(400, "недопустимое имя");
    wstring dir = savesDir() + L"\\" + wn;
    if (act == "save") {
        if (newestSvg().ok == false) return fail(400, "нечего сохранять: в рабочей папке нет сборки");
        bool over = rq.q("overwrite") == "1";
        if (exists(dir) && !over) return fail(409, "сохранение с таким именем уже есть", true);
        makeDirs(savesDir());
        wstring tmpd = savesDir() + L"\\.tmp_save";
        rmTree(tmpd);
        bool ok = copyTree(inDir(), tmpd + L"\\input");
        ok = copyOutputFiles(outDir(), tmpd + L"\\output") && ok;
        copyIfExists(g_work + L"\\config.js", tmpd + L"\\config.js");
        copyIfExists(g_work + L"\\render.log", tmpd + L"\\render.log");
        copyIfExists(g_work + L"\\manual.log", tmpd + L"\\manual.log");
        { const string& b = rq.body; if (b.size() >= 8 && (unsigned char)b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') writeFile(tmpd + L"\\thumb.png", b.data(), b.size()); }     // тело запроса - PNG вида окна для превью
        if (!ok) { rmTree(tmpd); return fail(500, "не удалось скопировать файлы (диск, права?)"); }
        rmTree(dir);
        if (!MoveFileExW(tmpd.c_str(), dir.c_str(), MOVEFILE_REPLACE_EXISTING)) { rmTree(tmpd); return fail(500, "не удалось записать сохранение"); }
        return replyJson(s, 200, "{\"ok\":true}", extra);
    }
    if (!exists(dir)) return fail(404, "сохранения нет");
    if (act == "delete") { rmTree(dir); return replyJson(s, 200, "{\"ok\":true}", extra); }
    if (act == "rename") {
        wstring wt;
        if (!safeSaveName(rq.q("to"), wt)) return fail(400, "недопустимое новое имя");
        wstring to = savesDir() + L"\\" + wt;
        if (exists(to) && lower(rq.q("to")) != lower(rq.q("name"))) return fail(409, "сохранение с таким именем уже есть", true);
        if (!MoveFileW(dir.c_str(), to.c_str())) return fail(500, "не удалось переименовать");
        return replyJson(s, 200, "{\"ok\":true}", extra);
    }
    if (act == "load") {
        if (!exists(dir + L"\\input")) return fail(400, "в сохранении нет сборки");
        rmTree(inDir());
        bool ok = copyTree(dir + L"\\input", inDir());
        copyIfExists(dir + L"\\config.js", g_work + L"\\config.js");
        wstring out = outDir();                                    // уже по config.js из сохранения
        makeDirs(out);
        for (const wstring& w : listDir(out)) if (safeOutName(toUtf8(w))) { SetFileAttributesW((out + L"\\" + w).c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW((out + L"\\" + w).c_str()); }
        ok = copyOutputFiles(dir + L"\\output", out) && ok;
        copyIfExists(dir + L"\\render.log", g_work + L"\\render.log");
        copyIfExists(dir + L"\\manual.log", g_work + L"\\manual.log");
        for (const wstring& w : listDir(inDir())) if (endsWithI(toUtf8(w), ".svg")) touchNow(inDir() + L"\\" + w);
        touchNow(g_work + L"\\config.js");
        if (!ok) return fail(500, "часть файлов не удалось загрузить");
        return replyJson(s, 200, "{\"ok\":true}", extra);
    }
    return fail(404, "неизвестное действие");
}

// ---------- «Поделиться»: ретрансляция окна просмотра в share.php сайта ----------
// Окно шлёт POST /api/share/<действие>, wood_server добавляет почту и серийник и пересылает на сайт; ответ сайта (JSON) отдаётся окну как есть.
// Почту и серийник пишет панель при запуске окна в work\share_auth.txt (строка 1 - почта, строка 2 - серийник; js\sys.js woodRenderStart), в страницу они не попадают.
// Действия: list | renew {slot} | clear {slot} | up_begin {slot} | up_file?id=..&name=.. (тело - сам файл как есть, на сайт уходит multipart) | up_end {id, label} | up_abort {id}.
// Сайт по умолчанию https://mrwoodman.by/share.php; переменная окружения WOOD_SHARE_URL заменяет адрес (стенды: поддельный сайт на 127.0.0.1).
// Нет файла с почтой - {"ok":false,"error":"no_license"}; нет связи с сайтом - 502 и {"ok":false,"error":"net"}.
static bool shareAuthRead(string& email, string& serial) {
    string t;
    email.clear(); serial.clear();
    if (!readFile(g_work + L"\\share_auth.txt", t, 4096)) return false;
    if (t.size() >= 3 && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF) t.erase(0, 3);
    std::vector<string> lines;
    size_t p = 0;
    while (p <= t.size() && lines.size() < 2) {
        size_t e = t.find_first_of("\r\n", p);
        string ln = t.substr(p, e == string::npos ? string::npos : e - p);
        while (!ln.empty() && (ln.back() == ' ' || ln.back() == '\t')) ln.pop_back();
        size_t i = 0; while (i < ln.size() && (ln[i] == ' ' || ln[i] == '\t')) i++;
        ln.erase(0, i);
        if (!ln.empty()) lines.push_back(ln);
        if (e == string::npos) break;
        p = e + 1;
    }
    if (lines.size() < 2) return false;
    email = lines[0]; serial = lines[1];
    return true;
}
static string shareEnc(const string& s) {                       // application/x-www-form-urlencoded
    static const char* hx = "0123456789ABCDEF";
    string o;
    for (unsigned char c : s) {
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += hx[c >> 4]; o += hx[c & 15]; }
    }
    return o;
}
static void shareUtf8Add(string& o, unsigned cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
    else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 63)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
}
// Значение поля плоского JSON ({"slot":3,"id":"..","label":".."}): строка (с разбором \", \\, \n, \uXXXX) или число
static bool shareJsonVal(const string& j, const char* key, string& out) {
    string k = string("\"") + key + "\"";
    size_t p = j.find(k);
    if (p == string::npos) return false;
    p = j.find(':', p + k.size());
    if (p == string::npos) return false;
    for (p++; p < j.size() && (j[p] == ' ' || j[p] == '\t' || j[p] == '\r' || j[p] == '\n'); p++) {}
    out.clear();
    if (p < j.size() && j[p] == '"') {
        for (p++; p < j.size() && j[p] != '"'; p++) {
            char c = j[p];
            if (c != '\\' || p + 1 >= j.size()) { out += c; continue; }
            char d = j[++p];
            if (d == 'n') out += '\n';
            else if (d == 't') out += '\t';
            else if (d == 'r') out += '\r';
            else if (d == 'u' && p + 4 < j.size()) {
                unsigned cp = (unsigned)strtoul(j.substr(p + 1, 4).c_str(), nullptr, 16);
                p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && j.compare(p + 1, 2, "\\u") == 0 && p + 6 < j.size()) {
                    unsigned lo = (unsigned)strtoul(j.substr(p + 3, 4).c_str(), nullptr, 16);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
                }
                shareUtf8Add(out, cp);
            } else out += d;
        }
        return true;
    }
    size_t e = p;
    while (e < j.size() && ((j[e] >= '0' && j[e] <= '9') || j[e] == '-')) e++;
    out = j.substr(p, e - p);
    return !out.empty();
}
// POST на сайт (WinHTTP, TLS 1.2); ответ - тело, до 4 МБ. false - нет связи (err - причина для журнала).
static bool sharePost(const string& ctype, const string& body, string& out, DWORD& status, string& err) {
    wstring url = L"https://mrwoodman.by/share.php";
    wchar_t ev[1024];
    DWORD en = GetEnvironmentVariableW(L"WOOD_SHARE_URL", ev, 1024);
    if (en > 0 && en < 1024) url = ev;
    wchar_t host[256] = {}, path[2048] = {};
    URL_COMPONENTSW uc = {};
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { err = "bad url"; return false; }
    HINTERNET ses = WinHttpOpen(L"woodman-viewer/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { err = "WinHttpOpen"; return false; }
    DWORD protos = 0x00000800 | 0x00000200 | 0x00000080;                 // TLS 1.2 | 1.1 | 1.0 (на Win7 TLS 1.2 надо включать явно)
    WinHttpSetOption(ses, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof protos);
    WinHttpSetTimeouts(ses, 10000, 10000, 120000, 120000);
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"POST", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    bool ok = false;
    wstring hdr = L"Content-Type: " + toWide(ctype);
    if (req && WinHttpSendRequest(req, hdr.c_str(), (DWORD)-1, (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) && WinHttpReceiveResponse(req, nullptr)) {
        DWORD sz = sizeof status;
        status = 0;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        out.clear();
        char buf[16384];
        for (;;) {
            DWORD n = 0;
            if (!WinHttpReadData(req, buf, sizeof buf, &n) || n == 0) break;
            if (out.size() + n > 4u * 1024 * 1024) break;
            out.append(buf, n);
        }
        ok = true;
    } else err = "winhttp error " + std::to_string(GetLastError());
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}
static void shareRelay(SOCKET s, Req& rq, const string& act, const string& extra) {
    if (act != "list" && act != "renew" && act != "clear" && act != "download" && act != "move" && act != "rename" && act != "edit" && act != "cats" && act != "up_begin" && act != "up_file" && act != "up_end" && act != "up_abort")
        return replyJson(s, 404, "{\"ok\":false,\"error\":\"bad_action\"}", extra);
    string email, serial;
    if (!shareAuthRead(email, serial)) return replyJson(s, 200, "{\"ok\":false,\"error\":\"no_license\"}", extra);
    string ctype, body, v;
    if (act == "up_file") {
        string id = rq.q("id"), name = rq.q("name");
        char rb[8];
        BCryptGenRandom(nullptr, (PUCHAR)rb, sizeof rb, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        static const char* hx = "0123456789abcdef";
        string bd = "----woodshare";
        for (int i = 0; i < 8; i++) { bd += hx[((unsigned char)rb[i]) >> 4]; bd += hx[((unsigned char)rb[i]) & 15]; }
        auto field = [&](const char* n, const string& val) { body += "--" + bd + "\r\nContent-Disposition: form-data; name=\"" + n + "\"\r\n\r\n" + val + "\r\n"; };
        field("action", "up_file"); field("email", email); field("serial", serial); field("id", id); field("name", name);
        body += "--" + bd + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"f.bin\"\r\nContent-Type: application/octet-stream\r\n\r\n";
        body += rq.body;
        body += "\r\n--" + bd + "--\r\n";
        ctype = "multipart/form-data; boundary=" + bd;
    } else {
        body = "action=" + shareEnc(act) + "&email=" + shareEnc(email) + "&serial=" + shareEnc(serial);
        if (shareJsonVal(rq.body, "slot", v)) body += "&slot=" + shareEnc(v);
        if (shareJsonVal(rq.body, "id", v)) body += "&id=" + shareEnc(v);
        if (shareJsonVal(rq.body, "label", v)) body += "&label=" + shareEnc(v);   // up_end и rename: подпись слота (UTF-8, на сайте не длиннее 80 знаков)
        if (shareJsonVal(rq.body, "desc", v)) body += "&desc=" + shareEnc(v);     // edit: описание макета (UTF-8, переводы строк как есть; на сайте не длиннее 1000 знаков)
        if (shareJsonVal(rq.body, "dims", v)) body += "&dims=" + shareEnc(v);     // edit: габариты "Ш:Г:В", мм
        if (shareJsonVal(rq.body, "thick", v)) body += "&thick=" + shareEnc(v);   // edit: толщины через ":", мм
        if (shareJsonVal(rq.body, "on", v)) body += "&on=" + shareEnc(v);         // download: галочка «поделиться файлом» (1 / 0)
        if (shareJsonVal(rq.body, "to", v)) body += "&to=" + shareEnc(v);         // move: 0 - в публичные, N - в обычный слот N
        if (shareJsonVal(rq.body, "cats", v)) body += "&cats=" + shareEnc(v);     // cats: разделы галереи, ключи через запятую (пустая строка - снять все)
        if (shareJsonVal(rq.body, "lang", v)) body += "&lang=" + shareEnc(v);     // язык подписей разделов в ответе (catalog): код языка докера, сайт знает ru / en / ar, остальные - en
        ctype = "application/x-www-form-urlencoded";
    }
    string out, err;
    DWORD status = 0;
    if (!sharePost(ctype, body, out, status, err)) {
        if (g_verbose) fprintf(stderr, "share %s: %s\n", act.c_str(), err.c_str());
        return replyJson(s, 502, "{\"ok\":false,\"error\":\"net\"}", extra);
    }
    size_t b = out.find_first_not_of(" \t\r\n");
    if (status != 200 || b == string::npos || out[b] != '{') {                // сайт ответил не JSON (ошибка сервера, заглушка хостинга)
        if (g_verbose) fprintf(stderr, "share %s: http %lu\n", act.c_str(), (unsigned long)status);
        return replyJson(s, 502, "{\"ok\":false,\"error\":\"net\"}", extra);
    }
    return replyJson(s, 200, out, extra);
}

static void handle(SOCKET s, Req& rq) {
    // --- защита: Host и Origin только свои, токен (если задан) ---
    string host = lower(rq.h.count("host") ? rq.h["host"] : "");
    string p = std::to_string(g_port);
    if (host != "127.0.0.1:" + p && host != "localhost:" + p) { if (g_batchT0) logLine("отказ 403 (Host " + host + "): " + rq.path); return reply(s, 403, "bad host"); }
    if (rq.h.count("origin")) {
        string o = lower(rq.h["origin"]);
        if (o != "http://127.0.0.1:" + p && o != "http://localhost:" + p) { if (g_batchT0) logLine("отказ 403 (Origin " + o + "): " + rq.path); return reply(s, 403, "bad origin"); }
    }
    string extra;
    if (!g_token.empty()) {
        bool ok = false;
        if (rq.q("k") == g_token) { ok = true; extra = "Set-Cookie: wk" + p + "=" + g_token + "; Path=/; HttpOnly; SameSite=Strict\r\n"; }
        else if (rq.h.count("cookie")) {
            string c = "; " + rq.h["cookie"] + ";";
            ok = c.find("; wk" + p + "=" + g_token + ";") != string::npos;
        }
        if (!ok) { if (g_batchT0) logLine(string("отказ 403 (нет токена: cookie ") + (rq.h.count("cookie") ? "пришла, но не та" : "не пришла") + "): " + rq.path); return reply(s, 403, "no token"); }
    }

    const string& path = rq.path;
    if (path == "/ping") {
        InterlockedExchange64(&g_lastPing, (LONG64)GetTickCount64());
        return replyJson(s, 200, versionJson(), extra);
    }
    if (path == "/status") {
        LONG64 lp = g_lastPing;
        bool alive = lp && (ULONGLONG)GetTickCount64() - (ULONGLONG)lp < 8000;
        return replyJson(s, 200, string("{\"viewerAlive\":") + (alive ? "true" : "false") + ",\"code\":" + fmtNum(codeVer()) +
                                     ",\"started\":" + fmtNum(g_startVer) + "}", extra);
    }
    if (path == "/quit") {
        replyJson(s, 200, "{\"ok\":true}", extra);
        Sleep(100);
        ExitProcess(0);
    }
    if (path == "/log" && rq.method == "POST") {
        if (rq.body.size() > 1024 * 1024) return reply(s, 413, "too large");
        wstring f = g_work + L"\\manual.log";
        double m; ULONGLONG sz;
        if (fileInfo(f, &m, &sz) && sz > 1.5 * 1024 * 1024) MoveFileExW(f.c_str(), (f + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
        string line = isoNow() + " " + rq.body + "\n";
        if (!writeFile(f, line.data(), line.size(), true)) return replyJson(s, 500, "{\"ok\":false}", extra);
        return replyJson(s, 200, "{\"ok\":true}", extra);
    }
    if (path == "/pins") {
        wstring f = inDir() + L"\\pins.json";
        if (rq.method == "GET") {
            string txt = "{}"; readFile(f, txt, 8 * 1024 * 1024);
            return replyJson(s, 200, txt, extra);
        }
        if (rq.method == "DELETE") { DeleteFileW(f.c_str()); return replyJson(s, 200, "{\"ok\":true}", extra); }
        if (rq.method == "POST" || rq.method == "PUT") {
            if (rq.body.size() > 2 * 1024 * 1024) return reply(s, 413, "too large");
            JsonCheck jc(rq.body);
            if (!jc.valid()) return replyJson(s, 400, "{\"ok\":false,\"error\":\"bad json\"}", extra);
            makeDirs(inDir());
            if (!writeFile(f, rq.body.data(), rq.body.size())) return replyJson(s, 500, "{\"ok\":false}", extra);
            return replyJson(s, 200, "{\"ok\":true}", extra);
        }
        return reply(s, 405, "method");
    }
    // --- сохранённые сборки (saves\<имя>) ---
    if (path == "/saves" && rq.method == "GET") {
        string r = "[";
        auto v = listSaves();
        for (size_t i = 0; i < v.size(); i++) {
            wstring wn = toWide(v[i].name);
            if (i) r += ",";
            r += "{\"name\":\"" + jsonEsc(v[i].name) + "\",\"m\":" + fmtNum(v[i].m) + ",\"thumb\":" + (saveThumb(savesDir() + L"\\" + wn).empty() ? "false" : "true") + "}";
        }
        return replyJson(s, 200, r + "]", extra);
    }
    if (path == "/saves/thumb" && rq.method == "GET") {
        wstring wn;
        if (!safeSaveName(rq.q("name"), wn)) return reply(s, 404, "not found");
        wstring f = saveThumb(savesDir() + L"\\" + wn);
        string data;
        if (f.empty() || !readFile(f, data, 32 * 1024 * 1024)) return reply(s, 404, "not found");
        return reply(s, 200, data, "image/png", extra);
    }
    if (startsWith(path, "/saves/") && rq.method == "POST") {
        if (g_batchT0) return replyJson(s, 409, "{\"ok\":false,\"error\":\"идёт рендер\"}", extra);
        savesAction(s, rq, path.substr(7), extra);
        return;
    }
    if (path == "/api/textures" && rq.method == "GET") return replyJson(s, 200, texturesJson(), extra);
    if (path.compare(0, 11, "/api/share/") == 0 && rq.method == "POST") return shareRelay(s, rq, path.substr(11), extra);       // «Поделиться»: на сайт (share.php)
    if (path == "/corel_ids") {                            // input\corel_ids.json: StaticID фигур Corel (пишет генератор box render при экспорте сборки); POST - окно после «Передать в 3Д» обновляет габариты заменённых фигур
        wstring f = inDir() + L"\\corel_ids.json";
        if (rq.method == "GET") {
            string txt = "{}"; readFile(f, txt, 8 * 1024 * 1024);
            return replyJson(s, 200, txt, extra);
        }
        if (rq.method == "POST" || rq.method == "PUT") {
            if (g_batchT0) return replyJson(s, 409, "{\"ok\":false,\"error\":\"идёт рендер\"}", extra);
            JsonCheck jc(rq.body);
            if (!jc.valid()) return replyJson(s, 400, "{\"ok\":false,\"error\":\"bad json\"}", extra);
            makeDirs(inDir());
            if (!writeFile(f, rq.body.data(), rq.body.size())) return replyJson(s, 500, "{\"ok\":false}", extra);
            return replyJson(s, 200, "{\"ok\":true}", extra);
        }
        return reply(s, 405, "method");
    }
    if (path == "/assemble" && rq.method == "GET") {       // input\assemble.json: тексты слоя assemble и центры их вращения = места стыков (пишет генератор box render при экспорте сборки); файла нет - {}
        string txt = "{}"; readFile(inDir() + L"\\assemble.json", txt, 2 * 1024 * 1024);
        return replyJson(s, 200, txt, extra);
    }
    if (path == "/scene" && rq.method == "POST") {         // «Передать в 3Д»: окно склеило выбранные в Corel фигуры в чертёж и возвращает его на место (самый свежий input\*.svg)
        if (g_batchT0) return replyJson(s, 409, "{\"ok\":false,\"error\":\"идёт рендер\"}", extra);
        if (rq.body.size() < 64 || rq.body.find("<svg") == string::npos || rq.body.find("</svg>") == string::npos) return replyJson(s, 400, "{\"ok\":false,\"error\":\"это не SVG\"}", extra);
        Svg svg = newestSvg();
        if (!svg.ok) return replyJson(s, 404, "{\"ok\":false,\"error\":\"в рабочей папке нет чертежа\"}", extra);
        if (!writeFile(inDir() + L"\\" + toWide(svg.name), rq.body.data(), rq.body.size())) return replyJson(s, 500, "{\"ok\":false}", extra);
        return replyJson(s, 200, "{\"ok\":true,\"file\":\"" + jsonEsc(svg.name) + "\"}", extra);
    }
    if (path == "/patch" && rq.method == "DELETE") {      // патч принят окном: файлы больше не нужны (patch_names.json стёрт - сигнал в /ping погас)
        DeleteFileW((inDir() + L"\\patch\\patch_names.json").c_str());
        DeleteFileW((inDir() + L"\\patch\\patch.svg").c_str());
        return replyJson(s, 200, "{\"ok\":true}", extra);
    }
    if (path == "/patch" && rq.method == "GET") {         // input\patch\patch_names.json: имена и габариты выбранных фигур, переданных кнопкой «Передать в 3Д» (рядом patch.svg - обычный файл input/patch/patch.svg)
        string txt = "{}"; readFile(inDir() + L"\\patch\\patch_names.json", txt, 8 * 1024 * 1024);
        return replyJson(s, 200, txt, extra);
    }
    if (path == "/edits") {                               // input\edits.json: правки узлов фигур Corel из окна (пазы за двигаемой деталью); читает кнопка докера
        wstring f = inDir() + L"\\edits.json";
        if (rq.method == "GET") {
            string txt = "{}"; readFile(f, txt, 8 * 1024 * 1024);
            return replyJson(s, 200, txt, extra);
        }
        if (rq.method == "DELETE") { DeleteFileW(f.c_str()); return replyJson(s, 200, "{\"ok\":true}", extra); }
        if (rq.method == "POST" || rq.method == "PUT") {
            if (rq.body.size() > 2 * 1024 * 1024) return reply(s, 413, "too large");
            JsonCheck jc(rq.body);
            if (!jc.valid()) return replyJson(s, 400, "{\"ok\":false,\"error\":\"bad json\"}", extra);
            makeDirs(inDir());
            if (!writeFile(f, rq.body.data(), rq.body.size())) return replyJson(s, 500, "{\"ok\":false}", extra);
            return replyJson(s, 200, "{\"ok\":true}", extra);
        }
        return reply(s, 405, "method");
    }
    if (path == "/collisions") {                          // input\collisions.json: пятна коллизий из окна (по деталям: имя, цвет, отрезки на листе); читает кнопка докера «Коллизии из 3Д»
        wstring f = inDir() + L"\\collisions.json";
        if (rq.method == "GET") {
            string txt = "{}"; readFile(f, txt, 8 * 1024 * 1024);
            return replyJson(s, 200, txt, extra);
        }
        if (rq.method == "DELETE") { DeleteFileW(f.c_str()); return replyJson(s, 200, "{\"ok\":true}", extra); }
        if (rq.method == "POST" || rq.method == "PUT") {
            if (rq.body.size() > 8 * 1024 * 1024) return reply(s, 413, "too large");
            JsonCheck jc(rq.body);
            if (!jc.valid()) return replyJson(s, 400, "{\"ok\":false,\"error\":\"bad json\"}", extra);
            makeDirs(inDir());
            if (!writeFile(f, rq.body.data(), rq.body.size())) return replyJson(s, 500, "{\"ok\":false}", extra);
            return replyJson(s, 200, "{\"ok\":true}", extra);
        }
        return reply(s, 405, "method");
    }
    if (path == "/materials") {                           // назначение текстур (input\materials.json), как /pins
        wstring f = inDir() + L"\\materials.json";
        if (rq.method == "GET") {
            string txt = "{}"; readFile(f, txt, 8 * 1024 * 1024);
            return replyJson(s, 200, txt, extra);
        }
        if (rq.method == "DELETE") { DeleteFileW(f.c_str()); return replyJson(s, 200, "{\"ok\":true}", extra); }
        if (rq.method == "POST" || rq.method == "PUT") {
            if (rq.body.size() > 2 * 1024 * 1024) return reply(s, 413, "too large");
            JsonCheck jc(rq.body);
            if (!jc.valid()) return replyJson(s, 400, "{\"ok\":false,\"error\":\"bad json\"}", extra);
            makeDirs(inDir());
            if (!writeFile(f, rq.body.data(), rq.body.size())) return replyJson(s, 500, "{\"ok\":false}", extra);
            return replyJson(s, 200, "{\"ok\":true}", extra);
        }
        return reply(s, 405, "method");
    }
    if (path == "/solve") {                               // готовое решение сборки (input\solve.json): считает страница рендера, окно просмотра берёт готовое
        wstring f = inDir() + L"\\solve.json";
        if (rq.method == "GET") {
            string txt = "{}"; readFile(f, txt, 8 * 1024 * 1024);
            return replyJson(s, 200, txt, extra);
        }
        if (rq.method == "DELETE") { DeleteFileW(f.c_str()); return replyJson(s, 200, "{\"ok\":true}", extra); }
        if (rq.method == "POST" || rq.method == "PUT") {
            if (rq.body.size() > 4 * 1024 * 1024) return reply(s, 413, "too large");
            JsonCheck jc(rq.body);
            if (!jc.valid()) return replyJson(s, 400, "{\"ok\":false,\"error\":\"bad json\"}", extra);
            makeDirs(inDir());
            if (!writeFile(f, rq.body.data(), rq.body.size())) return replyJson(s, 500, "{\"ok\":false}", extra);
            return replyJson(s, 200, "{\"ok\":true}", extra);
        }
        return reply(s, 405, "method");
    }
    if (path == "/solvewhy") {                            // журнал решений автосборки (input\solve_why.json): пишет страница рендера рядом с solve.json
        wstring f = inDir() + L"\\solve_why.json";
        if (rq.method == "GET") {
            string txt = "{}"; readFile(f, txt, 8 * 1024 * 1024);
            return replyJson(s, 200, txt, extra);
        }
        if (rq.method == "DELETE") { DeleteFileW(f.c_str()); return replyJson(s, 200, "{\"ok\":true}", extra); }
        if (rq.method == "POST" || rq.method == "PUT") {
            if (rq.body.size() > 2 * 1024 * 1024) return reply(s, 413, "too large");
            JsonCheck jc(rq.body);
            if (!jc.valid()) return replyJson(s, 400, "{\"ok\":false,\"error\":\"bad json\"}", extra);
            makeDirs(inDir());
            if (!writeFile(f, rq.body.data(), rq.body.size())) return replyJson(s, 500, "{\"ok\":false}", extra);
            return replyJson(s, 200, "{\"ok\":true}", extra);
        }
        return reply(s, 405, "method");
    }
    if (path == "/renderstat" && rq.method == "POST") {    // журнал сборок для оптимизации автосборщика: окно зовёт после явного 💾; ?hash=<16 hex>&rev=N&name=..&asm=0|1, тело - JSON {final, a1}. Папки render_stat нет - не пишем ничего (сам её не создаём)
        wstring wk = g_work;                                // журнал лежит рядом с рабочей папкой: <родитель --work>\render_stat
        while (wk.size() > 3 && (wk.back() == L'\\' || wk.back() == L'/')) wk.pop_back();
        size_t cut = wk.find_last_of(L"\\/");
        const wstring root = cut == wstring::npos ? wstring() : wk.substr(0, cut) + L"\\render_stat";
        if (root.empty() || !exists(root)) return replyJson(s, 200, "{\"ok\":true,\"written\":false}", extra);
        if (rq.body.size() > 4 * 1024 * 1024) return reply(s, 413, "too large");
        JsonCheck jc(rq.body);
        if (!jc.valid()) return replyJson(s, 400, "{\"ok\":false,\"error\":\"bad json\"}", extra);
        string hash = rq.q("hash"), rev = rq.q("rev");
        if (hash.size() != 16 || rev.empty() || rev.size() > 6) return replyJson(s, 400, "{\"ok\":false,\"error\":\"bad args\"}", extra);
        for (char c : hash) if (!isxdigit((unsigned char)c)) return replyJson(s, 400, "{\"ok\":false,\"error\":\"bad hash\"}", extra);
        for (char c : rev) if (!isdigit((unsigned char)c)) return replyJson(s, 400, "{\"ok\":false,\"error\":\"bad rev\"}", extra);
        bool isAsm = rq.q("asm") == "1";
        wstring dir = root + L"\\" + toWide(hash);
        makeDirs(dir);
        if (!writeFile(dir + L"\\final.json", rq.body.data(), rq.body.size())) return replyJson(s, 500, "{\"ok\":false}", extra);
        Svg svg = newestSvg();
        if (svg.ok) copyIfExists(inDir() + L"\\" + toWide(svg.name), dir + L"\\scene.svg");
        if (exists(inDir() + L"\\pins.json")) copyIfExists(inDir() + L"\\pins.json", dir + L"\\pins.json"); else DeleteFileW((dir + L"\\pins.json").c_str());
        // auto_<rev>.json = журнал решений (input\solve_why.json) этого же прогона: ключ тот же, что у solve.json, и оканчивается |r<rev>; иначе (старый exe/рендер, чужая сцена, assemble) - без него
        string autoSt = isAsm ? "asm" : "nofile";
        string why;
        if (!isAsm && readFile(inDir() + L"\\solve_why.json", why, 4 * 1024 * 1024)) {
            auto keyOf = [](const string& t) { size_t p = t.find("\"key\":\""); if (p == string::npos) return string(); p += 7; size_t e = t.find('"', p); return e == string::npos ? string() : t.substr(p, e - p); };
            string kw = keyOf(why), ks; string sol; readFile(inDir() + L"\\solve.json", sol, 8 * 1024 * 1024); ks = keyOf(sol);
            string tail = "|r" + rev;
            if (kw.empty() || kw != ks) autoSt = "nokey";
            else if (kw.size() < tail.size() || kw.compare(kw.size() - tail.size(), tail.size(), tail) != 0) autoSt = "rev";
            else if (writeFile(dir + L"\\auto_" + toWide(rev) + L".json", why.data(), why.size())) autoSt = "ok";
            else autoSt = "write";
        }
        if (autoSt == "ok") {                                  // оставить версии решателя: текущую и предыдущую
            vector<int> revs;
            for (const wstring& w : listDir(dir, L"auto_*.json")) { int v = _wtoi(w.c_str() + 5); if (v > 0) revs.push_back(v); }
            std::sort(revs.begin(), revs.end(), std::greater<int>());
            for (size_t i = 2; i < revs.size(); i++) DeleteFileW((dir + L"\\auto_" + std::to_wstring(revs[i]) + L".json").c_str());
        }
        SYSTEMTIME st; GetLocalTime(&st);
        char tb[40]; snprintf(tb, sizeof tb, "%04d-%02d-%02dT%02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        string line = string("{\"t\":\"") + tb + "\",\"hash\":\"" + hash + "\",\"name\":\"" + jsonEsc(rq.q("name")) + "\",\"rev\":" + rev + ",\"asm\":" + (isAsm ? "true" : "false") + ",\"auto\":\"" + autoSt + "\"}\n";
        writeFile(root + L"\\index.jsonl", line.data(), line.size(), true);
        return replyJson(s, 200, "{\"ok\":true,\"written\":true,\"auto\":\"" + autoSt + "\"}", extra);
    }
    if (path == "/save" && rq.method == "POST") {          // снимок из окна просмотра - в output
        const string& b = rq.body;
        if (b.size() < 8 || (unsigned char)b[0] != 0x89 || b[1] != 'P' || b[2] != 'N' || b[3] != 'G')
            return replyJson(s, 400, "{\"ok\":false,\"error\":\"это не PNG\"}", extra);
        Svg svg = newestSvg();
        string base = svg.ok ? svg.name.substr(0, svg.name.size() - 4) : "scene";
        for (auto& c : base) if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) c = '_';
        makeDirs(outDir());
        int n = 1; wstring file;
        do { file = outDir() + L"\\" + toWide(base) + L"_view_" + std::to_wstring(n) + L".png"; n++; } while (exists(file));
        if (!writeFile(file, b.data(), b.size())) return replyJson(s, 500, "{\"ok\":false}", extra);
        string fn = toUtf8(file.substr(file.find_last_of(L'\\') + 1));
        return replyJson(s, 200, "{\"ok\":true,\"file\":\"" + jsonEsc(fn) + "\"}", extra);
    }
    if (path == "/video" && rq.method == "POST") {         // ролик из окна просмотра (запись видео) - в output; MP4: в байтах 4..7 "ftyp"
        const string& b = rq.body;
        if (b.size() < 16 || b.compare(4, 4, "ftyp") != 0)
            return replyJson(s, 400, "{\"ok\":false,\"error\":\"это не MP4\"}", extra);
        Svg svg = newestSvg();
        string base = svg.ok ? svg.name.substr(0, svg.name.size() - 4) : "scene";
        for (auto& c : base) if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) c = '_';
        makeDirs(outDir());
        int n = 1; wstring file;
        do { file = outDir() + L"\\" + toWide(base) + L"_video_" + std::to_wstring(n) + L".mp4"; n++; } while (exists(file));
        if (!writeFile(file, b.data(), b.size())) return replyJson(s, 500, "{\"ok\":false}", extra);
        string fn = toUtf8(file.substr(file.find_last_of(L'\\') + 1));
        return replyJson(s, 200, "{\"ok\":true,\"file\":\"" + jsonEsc(fn) + "\"}", extra);
    }
    if (path == "/openout" && rq.method == "POST") {       // кнопка «папка» панели видео: открыть output в проводнике
        makeDirs(outDir());
        INT_PTR sh = (INT_PTR)ShellExecuteW(nullptr, L"open", outDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return replyJson(s, 200, sh > 32 ? "{\"ok\":true}" : "{\"ok\":false}", extra);
    }
    // --- пакетный рендер: страница batch.html сама считает и шлёт результаты ---
    if (path == "/api/inputs" && rq.method == "GET") {
        string r = "[";
        auto v = inputSvgs();
        for (size_t i = 0; i < v.size(); i++) { if (i) r += ","; r += "\"" + jsonEsc(v[i]) + "\""; }
        return replyJson(s, 200, r + "]", extra);
    }
    if (path == "/batchlog" && rq.method == "POST") {
        InterlockedExchange(&g_pageStarted, 1); touchActivity();
        if (rq.body.compare(0, 6, "ERROR:") == 0 || rq.body.compare(0, 11, "[pageerror]") == 0) g_lastErr = rq.body.substr(0, 600);
        logLine(rq.body);
        return replyJson(s, 200, "{\"ok\":true}", extra);
    }
    if (path == "/file" && rq.method == "POST") {
        string name = rq.q("name");
        if (!safeOutName(name)) return replyJson(s, 400, "{\"ok\":false,\"error\":\"bad name\"}", extra);
        makeDirs(outDir());
        touchActivity();
        if (!writeFile(outDir() + L"\\" + toWide(name), rq.body.data(), rq.body.size())) return replyJson(s, 500, "{\"ok\":false}", extra);
        return replyJson(s, 200, "{\"ok\":true}", extra);
    }
    if (path == "/done" && rq.method == "POST") {
        InterlockedExchange(&g_pageStarted, 1);
        InterlockedExchange(&g_batchOk, rq.q("ok") == "1" ? 1 : 0);
        InterlockedExchange(&g_wantView, rq.q("view") == "1" ? 1 : 0);
        int vp = atoi(rq.q("port").c_str());
        if (vp > 0 && vp < 65536) InterlockedExchange(&g_viewPort, vp);
        replyJson(s, 200, "{\"ok\":true}", extra);
        if (g_doneEvent) SetEvent(g_doneEvent);
        return;
    }
    if (rq.method != "GET") return reply(s, 405, "method");

    // --- статические файлы ---
    string dec, rel;
    if (!pctDecode(path, dec) || !safeRel(dec, rel)) return reply(s, 404, "not found");
    if (rel.empty()) rel = "viewer.html";
    if (g_batchT0 && rel == "batch.html") { logLine("браузер запросил batch.html"); logLine("браузер: " + (rq.h.count("user-agent") ? rq.h["user-agent"] : string("?"))); }
    // файлы кода страницы (модули, три.js, конфиг) - в журнал рендера: при сбое загрузки видно, на каком файле и с каким ответом (присланные пользователями журналы)
    const bool logFile = g_batchT0 && (startsWith(rel, "src/") || startsWith(rel, "vendor/") || rel == "config.js" || rel == "batch.html");
    if (!allowed(rel)) { if (g_batchT0) logLine("отказ 404 (путь не разрешён): " + rel); return reply(s, 404, "not found"); }
    wstring full = (isWorkPath(rel) ? g_work : g_code) + L"\\" + toWide(rel);
    if (startsWith(rel, "textures/")) full = textureFullPath(rel);
    if (startsWith(rel, "output/")) full = outDir() + L"\\" + toWide(rel.substr(7));        // папка снимков может быть назначена в config.js
    for (auto& c : full) if (c == L'/') c = L'\\';
    string data;
    if (!readFile(full, data)) { if (g_batchT0) logLine("отказ 404 (файла нет или не читается): " + rel); return reply(s, 404, "not found"); }
    if (logFile) logLine("файл " + rel + ": " + std::to_string(data.size()) + " байт");
    if (rel == "viewer.html") {                // страница узнаёт версию кода сервера и перезагружается, если она сменилась
        const string tag = "<script type=\"module\">";
        size_t at = data.find(tag);
        if (at != string::npos) data.insert(at, "<script>window.__CODE__ = " + fmtNum(codeVer()) + ";</script>\n");
    }
    t_sendFail = false;
    reply(s, 200, data, mimeOf(rel), extra);
    if (logFile && t_sendFail) logLine("ОБРЫВ при отправке " + rel + " (код сокета " + std::to_string(t_sendErr) + "): браузер закрыл соединение раньше, чем получил файл целиком");
}

// ---------- соединение ----------
static void serve(SOCKET s) {
    DWORD to = 30000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to, sizeof to);
    string buf; char tmp[16384];
    size_t hend = string::npos;
    while (hend == string::npos) {
        int n = recv(s, tmp, sizeof tmp, 0);
        if (n <= 0) return;
        buf.append(tmp, n);
        hend = buf.find("\r\n\r\n");
        if (hend == string::npos && buf.size() > 65536) return reply(s, 400, "headers too large");
    }
    Req rq;
    string head = buf.substr(0, hend);
    size_t le = head.find("\r\n");
    string line = head.substr(0, le);
    size_t sp1 = line.find(' '), sp2 = line.rfind(' ');
    if (sp1 == string::npos || sp2 == sp1) return reply(s, 400, "bad request");
    rq.method = line.substr(0, sp1);
    string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    size_t qm = target.find('?');
    rq.path = target.substr(0, qm);
    if (qm != string::npos) rq.query = target.substr(qm + 1);
    size_t pos = le == string::npos ? head.size() : le + 2;
    while (pos < head.size()) {
        size_t e = head.find("\r\n", pos); if (e == string::npos) e = head.size();
        string h = head.substr(pos, e - pos); size_t c = h.find(':');
        if (c != string::npos) {
            string v = h.substr(c + 1);
            while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(0, 1);
            rq.h[lower(h.substr(0, c))] = v;
        }
        pos = e + 2;
    }
    if (rq.h.count("transfer-encoding")) return reply(s, 411, "chunked not supported");
    unsigned long long clen = rq.h.count("content-length") ? strtoull(rq.h["content-length"].c_str(), nullptr, 10) : 0;
    unsigned long long limit = (rq.path == "/save" || rq.path == "/file" || rq.path == "/video") ? 200ULL * 1024 * 1024 : (rq.path == "/api/share/up_file" || rq.path == "/scene" ? 64ULL * 1024 * 1024 : (rq.path == "/solve" || rq.path == "/collisions" ? 8ULL * 1024 * 1024 : 2ULL * 1024 * 1024));
    if (clen > limit) return reply(s, 413, "too large");
    rq.body = buf.substr(hend + 4);
    while (rq.body.size() < clen) {
        int n = recv(s, tmp, sizeof tmp, 0);
        if (n <= 0) return;
        rq.body.append(tmp, n);
    }
    rq.body.resize((size_t)clen);
    handle(s, rq);
    if (g_verbose) { printf("%s %s\n", rq.method.c_str(), rq.path.c_str()); fflush(stdout); }
}
static DWORD WINAPI connThread(LPVOID p) {
    SOCKET s = (SOCKET)(uintptr_t)p;
    serve(s);
    shutdown(s, SD_SEND);                          // дослать ответ, затем вычитать недочитанное тело:
    DWORD to = 1000;                               // иначе закрытие с непрочитанными данными даёт RST и клиент не видит ответ
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);
    char junk[16384]; size_t left = 8u << 20;
    while (left > 0) { int n = recv(s, junk, sizeof junk, 0); if (n <= 0) break; left -= std::min<size_t>(left, (size_t)n); }
    closesocket(s);
    return 0;
}

// запущен ли CorelDRAW (CorelDRW.exe)
static bool corelRunning() {
    HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (h == INVALID_HANDLE_VALUE) return true;                 // не смогли посмотреть - не выходим
    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    bool found = false;
    for (BOOL ok = Process32FirstW(h, &pe); ok && !found; ok = Process32NextW(h, &pe)) found = _wcsicmp(pe.szExeFile, L"CorelDRW.exe") == 0;
    CloseHandle(h);
    return found;
}

// Corel был и закрылся - выходим; окно закрыли - сервер живёт дальше (картинку ищут, возвращаются на страницу).
// Если Corel так и не увидели (запуск вне Corel, стенды): прежнее правило - нет пингов 20 с или окно не открыли за 3 мин
static DWORD WINAPI idleThread(LPVOID) {
    bool seenCorel = false;
    int gone = 0;
    for (;;) {
        Sleep(5000);
        if (corelRunning()) { seenCorel = true; gone = 0; continue; }
        if (seenCorel) { if (++gone >= 2) ExitProcess(0); continue; }   // 2 проверки подряд (~10 с) - не реагировать на мигание процесса
        ULONGLONG now = GetTickCount64();
        LONG64 lp = g_lastPing;
        if (lp ? now - (ULONGLONG)lp > 20000 : now - g_started > 180000) ExitProcess(0);
    }
}

// ---------- сервер: сокет и приём соединений ----------
static SOCKET g_ls = INVALID_SOCKET;
static DWORD WINAPI acceptThread(LPVOID) {
    for (;;) {
        SOCKET c = accept(g_ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) { Sleep(10); continue; }
        HANDLE h = CreateThread(nullptr, 0, connThread, (LPVOID)(uintptr_t)c, 0, nullptr);
        if (h) CloseHandle(h); else closesocket(c);
    }
}
static bool startServer(const wstring& portfile) {
    g_ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_ls == INVALID_SOCKET) return false;
    BOOL excl = TRUE;                                      // чужой процесс не сядет на тот же порт
    setsockopt(g_ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl, sizeof excl);
    sockaddr_in sa = {};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)g_port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_ls, (sockaddr*)&sa, sizeof sa) != 0 || listen(g_ls, 64) != 0) { fprintf(stderr, "порт %d занят\n", g_port); return false; }
    if (g_port == 0) { int len = sizeof sa; getsockname(g_ls, (sockaddr*)&sa, &len); g_port = ntohs(sa.sin_port); }
    printf("PORT=%d\n", g_port); fflush(stdout);
    if (!portfile.empty()) { string t = std::to_string(g_port); writeFile(portfile, t.data(), t.size()); }
    CreateThread(nullptr, 0, acceptThread, nullptr, 0, nullptr);
    return true;
}

// ---------- токен ----------
static string randomToken() {
    typedef BOOLEAN (WINAPI* RtlGenRandomFn)(PVOID, ULONG);
    unsigned char b[16] = {};
    HMODULE m = LoadLibraryW(L"advapi32.dll");
    RtlGenRandomFn f = m ? (RtlGenRandomFn)GetProcAddress(m, "SystemFunction036") : nullptr;
    if (!f || !f(b, sizeof b)) { LARGE_INTEGER q; QueryPerformanceCounter(&q); for (int i = 0; i < 16; i++) b[i] = (unsigned char)(q.QuadPart >> (i % 8) * 8) ^ (unsigned char)(GetTickCount() + i * 31); }
    char h[40];
    for (int i = 0; i < 16; i++) sprintf_s(h + i * 2, 3, "%02x", b[i]);
    return string(h, 32);
}

// ---------- браузер ----------
static wstring envVar(const wchar_t* n) {
    wchar_t b[2048];
    DWORD k = GetEnvironmentVariableW(n, b, 2048);
    return (k > 0 && k < 2048) ? wstring(b, k) : wstring();
}
static int readBrowserTxt(wstring* path);
// порядок: browser.txt (рабочая папка, рядом с exe, папка кода), CHROME_PATH, Chrome, Edge, Chromium
static vector<wstring> findBrowsers() {
    vector<wstring> r;
    auto add = [&](const wstring& p) { if (!p.empty() && exists(p) && std::find(r.begin(), r.end(), p) == r.end()) r.push_back(p); };
    wstring bp;
    if (readBrowserTxt(&bp) == 2) add(bp);
    add(envVar(L"CHROME_PATH"));
    const wchar_t* rel[] = { L"\\Google\\Chrome\\Application\\chrome.exe", L"\\Microsoft\\Edge\\Application\\msedge.exe", L"\\Chromium\\Application\\chrome.exe" };
    wstring bases[] = { envVar(L"ProgramFiles"), envVar(L"ProgramFiles(x86)"), envVar(L"LOCALAPPDATA") };
    for (auto rl : rel) for (auto& b : bases) if (!b.empty()) add(b + rl);
    return r;
}
static wstring edgeOrChrome() {
    wstring bases[] = { envVar(L"ProgramFiles(x86)"), envVar(L"ProgramFiles"), envVar(L"LOCALAPPDATA") };
    const wchar_t* rel[] = { L"\\Microsoft\\Edge\\Application\\msedge.exe", L"\\Google\\Chrome\\Application\\chrome.exe" };
    for (auto rl : rel) for (auto& b : bases) if (!b.empty() && exists(b + rl)) return b + rl;
    return wstring();
}
// ---------- браузер окна просмотра: browser.txt (рядом с exe или в папке кода рендера) ----------
// нет файла или он пуст - браузер Windows по умолчанию (как щелчок по ссылке; обновление файл может удалить); в файле путь - этот браузер.
// путь неверен или запуск не удался - браузер по умолчанию, дальше Edge / Chrome, дальше ShellExecute (так же, если браузер по умолчанию не открылся)
#pragma comment(lib, "advapi32.lib")
static int readBrowserTxt(wstring* path) {                // 0 - файла нет, 1 - пустой, 2 - в нём путь
    wchar_t mb[MAX_PATH * 2]; DWORD mn = GetModuleFileNameW(nullptr, mb, MAX_PATH * 2);
    wstring me(mb, mn);
    wstring dirs[] = { g_work, me.substr(0, me.find_last_of(L'\\')), g_code };
    for (auto& d : dirs) {
        string txt;
        if (d.empty() || !readFile(d + L"\\browser.txt", txt, 4096)) continue;
        if (txt.size() >= 3 && (unsigned char)txt[0] == 0xEF && (unsigned char)txt[1] == 0xBB && (unsigned char)txt[2] == 0xBF) txt.erase(0, 3);
        size_t e = txt.find_first_of("\r\n"); if (e != string::npos) txt.resize(e);
        while (!txt.empty() && (txt.back() == ' ' || txt.back() == '\t')) txt.pop_back();
        size_t b = txt.find_first_not_of(" \t"); txt = b == string::npos ? string() : txt.substr(b);
        if (txt.size() >= 2 && txt.front() == '"' && txt.back() == '"') txt = txt.substr(1, txt.size() - 2);
        if (txt.empty()) return 1;
        *path = toWide(txt);
        return 2;
    }
    return 0;
}
static bool regString(HKEY root, const wstring& key, const wchar_t* name, wstring& out) {
    HKEY k;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    wchar_t b[2048]; DWORD sz = sizeof b - sizeof(wchar_t), ty = 0;
    LONG r = RegQueryValueExW(k, name, nullptr, &ty, (LPBYTE)b, &sz);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || (ty != REG_SZ && ty != REG_EXPAND_SZ)) return false;
    out.assign(b, sz / sizeof(wchar_t));
    while (!out.empty() && out.back() == 0) out.pop_back();
    if (ty == REG_EXPAND_SZ) { wchar_t x[2048]; DWORD n = ExpandEnvironmentStringsW(out.c_str(), x, 2048); if (n > 0 && n <= 2048) out.assign(x, n - 1); }
    return !out.empty();
}
static wstring defaultBrowser() {                         // exe браузера по умолчанию (по https, затем http); пусто - не нашли
    for (const wchar_t* proto : { L"https", L"http" }) {
        wstring prog, cmd;
        if (!regString(HKEY_CURRENT_USER, wstring(L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\") + proto + L"\\UserChoice", L"ProgId", prog)) continue;
        if (!regString(HKEY_CLASSES_ROOT, prog + L"\\shell\\open\\command", nullptr, cmd)) continue;
        size_t s = 0, e;
        if (cmd[0] == L'"') { s = 1; e = cmd.find(L'"', 1); }
        else { e = cmd.find(L".exe"); if (e != wstring::npos) e += 4; }
        if (e == wstring::npos || e <= s) continue;
        wstring p = cmd.substr(s, e - s);
        if (exists(p)) return p;
    }
    return wstring();
}
// --app (окно без вкладок) надёжно держат Edge, Chrome и близкие; у Opera он открывает новый экземпляр с пустым окном (проверено 26.10.2026), Firefox его не знает.
// Остальным отдаём адрес как есть: браузер по умолчанию - через ShellExecute (как щелчок по ссылке), свой путь из browser.txt - обычным аргументом; оба открывают вкладку в уже запущенном браузере
static bool launchWindow(const wstring& br, const wstring& url, bool isDefault) {
    wstring low = br.substr(br.find_last_of(L'\\') + 1); for (auto& c : low) c = (wchar_t)towlower(c);
    bool app = low.find(L"msedge") != wstring::npos || low.find(L"chrome") != wstring::npos || low.find(L"brave") != wstring::npos || low.find(L"vivaldi") != wstring::npos;
    if (!app && isDefault) {
        logLine("окно просмотра: " + toUtf8(br) + " - браузер по умолчанию, открываю адрес вкладкой " + toUtf8(url.substr(0, url.find(L'?'))));
        INT_PTR sh = (INT_PTR)ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (sh > 32) return true;
        logLine("окно просмотра: ShellExecute не открыл адрес, код " + std::to_string((long long)sh));
        return false;
    }
    logLine("окно просмотра: " + toUtf8(br) + (app ? " --app=" : " ") + toUtf8(url.substr(0, url.find(L'?'))));   // --app: отдельное окно без вкладок и адресной строки; повторный запуск с тем же адресом поднимает его
    STARTUPINFOW si = {}; si.cb = sizeof si; PROCESS_INFORMATION pi = {};
    wstring cmd = L"\"" + br + L"\" " + (app ? L"--app=\"" : L"\"") + url + L"\"" + (app ? L" --window-size=1100,820 --window-position=120,90" : L"");
    vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return true; }
    logLine("окно просмотра: запустить браузер не удалось, код Windows " + std::to_string((unsigned long)GetLastError()));
    return false;
}
// ---------- диагностика окружения (render.log): по этим строкам видно систему, видеокарту и браузер пользователя Windows 7 (22.10.2026) ----------
#pragma comment(lib, "version.lib")
static string fileVersion(const wstring& p) {            // "109.0.5414.120"; пусто - файла нет или у него нет ресурса версии
    DWORD h = 0, n = GetFileVersionInfoSizeW(p.c_str(), &h);
    if (!n) return string();
    vector<char> buf(n);
    if (!GetFileVersionInfoW(p.c_str(), 0, n, buf.data())) return string();
    VS_FIXEDFILEINFO* fi = nullptr; UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\", (void**)&fi, &len) || !fi) return string();
    char b[64];
    sprintf_s(b, "%u.%u.%u.%u", (unsigned)HIWORD(fi->dwFileVersionMS), (unsigned)LOWORD(fi->dwFileVersionMS), (unsigned)HIWORD(fi->dwFileVersionLS), (unsigned)LOWORD(fi->dwFileVersionLS));
    return b;
}
static string exitCodeText(DWORD ec) {                   // код выхода и в шестнадцатеричном виде: 0xC0000135 - нет библиотеки, 0xC0000142 - ошибка запуска, 0xC0000005 - сбой
    char b[48];
    sprintf_s(b, "%ld (0x%08lX)", (long)(int)ec, (unsigned long)ec);
    return b;
}
static void logEnvOnce() {
    static bool done = false;
    if (done) return;
    done = true;
    typedef LONG (WINAPI* RtlGetVersionFn)(OSVERSIONINFOW*);
    OSVERSIONINFOW vi = {}; vi.dwOSVersionInfoSize = sizeof vi;
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionFn rgv = nt ? (RtlGetVersionFn)GetProcAddress(nt, "RtlGetVersion") : nullptr;
    if (rgv) rgv(&vi);
    SYSTEM_INFO si = {}; GetNativeSystemInfo(&si);
    MEMORYSTATUSEX ms = {}; ms.dwLength = sizeof ms; GlobalMemoryStatusEx(&ms);
    const char* arch = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x64" : (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL ? "x86" : "?");
    char b[640];
    sprintf_s(b, "окружение: Windows %lu.%lu.%lu %s, процессор %s, ядер %lu, SSE3 %s, память %llu МБ (свободно %llu), wood_server %d-bit от %s %s",
              vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber, toUtf8(wstring(vi.szCSDVersion)).c_str(), arch, si.dwNumberOfProcessors,
              IsProcessorFeaturePresent(13) ? "да" : "нет", ms.ullTotalPhys / (1024 * 1024), ms.ullAvailPhys / (1024 * 1024), (int)(sizeof(void*) * 8), __DATE__, __TIME__);
    logLine(b);
    wchar_t me[MAX_PATH * 2]; DWORD mn = GetModuleFileNameW(nullptr, me, MAX_PATH * 2);
    sprintf_s(b, "окружение: язык интерфейса 0x%04X, exe %s", (unsigned)GetUserDefaultUILanguage(), toUtf8(wstring(me, mn)).c_str());
    logLine(b);
    logLine("окружение: код " + toUtf8(g_code) + ", данные " + toUtf8(g_work) + ", TEMP " + toUtf8(envVar(L"TEMP")));
    wchar_t sysd[MAX_PATH]; UINT sl = GetSystemDirectoryW(sysd, MAX_PATH);
    if (sl) {                                            // Windows 7: WebGL 2 в Chrome через DirectX 11, ему нужно обновление KB2670838 (новые d3d11.dll и dxgi.dll)
        const wchar_t* dlls[] = { L"d3d11.dll", L"dxgi.dll", L"d3d9.dll", L"d3dcompiler_47.dll", L"opengl32.dll", L"dwmapi.dll" };
        string dl;
        for (auto d : dlls) { string v = fileVersion(wstring(sysd, sl) + L"\\" + d); dl += toUtf8(d) + " " + (v.empty() ? string("нет") : v) + "; "; }
        logLine("окружение: системные библиотеки: " + dl);
    }
    for (DWORD i = 0; i < 8; i++) {
        DISPLAY_DEVICEW dd = {}; dd.cb = sizeof dd;
        if (!EnumDisplayDevicesW(nullptr, i, &dd, 0)) break;
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        logLine("окружение: видеоадаптер " + toUtf8(wstring(dd.DeviceString)) + " [" + toUtf8(wstring(dd.DeviceID)) + "]" + ((dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? " основной" : ""));
    }
    auto br = findBrowsers();
    if (br.empty()) logLine("окружение: браузер Chrome / Edge не найден");
    for (auto& p : br) { string v = fileVersion(p); logLine("окружение: браузер " + toUtf8(p) + ", версия " + (v.empty() ? string("?") : v)); }
}
// журнал самого браузера (--enable-logging пишет chrome_debug.log в профиль, профиль после запуска удаляется): строки про ошибки, видеокарту и WebGL - в render.log
static void logBrowserLog(const wstring& prof) {
    string t;
    if (!readFile(prof + L"\\chrome_debug.log", t, 4 * 1024 * 1024)) { logLine("журнал браузера: chrome_debug.log не создан"); return; }
    size_t pos = 0; int shown = 0, total = 0;
    while (pos < t.size()) {
        size_t e = t.find('\n', pos);
        if (e == string::npos) e = t.size();
        string ln = t.substr(pos, e - pos);
        pos = e + 1;
        while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ')) ln.pop_back();
        string low = ln;
        for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
        bool hit = low.find(":error") != string::npos || low.find("fatal") != string::npos || low.find("gpu") != string::npos || low.find("angle") != string::npos ||
                   low.find("webgl") != string::npos || low.find("swiftshader") != string::npos || low.find("d3d") != string::npos || low.find("dxgi") != string::npos || low.find("crash") != string::npos;
        if (!hit) continue;
        total++;
        if (shown < 40) { logLine("браузер: " + ln.substr(0, 320)); shown++; }
    }
    if (!total) logLine("журнал браузера: строк про ошибки и видеокарту нет (" + std::to_string(t.size()) + " байт)");
    else if (total > shown) logLine("журнал браузера: скрыто строк: " + std::to_string(total - shown));
}
static void rmTree(const wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            wstring p = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) rmTree(p); else RemoveDirectoryW(p.c_str()); }
            else { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(p.c_str()); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}
// процесс в job-объекте: закрытие job убивает браузер со всеми дочерними процессами
static bool spawnJob(const wstring& cmdLine, DWORD flags, HANDLE* hp, HANDLE* hjob) {
    STARTUPINFOW si = {}; si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    vector<wchar_t> buf(cmdLine.begin(), cmdLine.end()); buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, flags | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) return false;
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;   // BREAKAWAY_OK: песочница Chrome создаёт GPU-процесс с CREATE_BREAKAWAY_FROM_JOB, иначе на Win7 (job не вложенные) "GPU process launch failed: error_code=18"
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
        AssignProcessToJobObject(job, pi.hProcess);        // не вышло (уже в чужом job на старой ОС) - живём без него
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    *hp = pi.hProcess; *hjob = job;
    return true;
}

// ---------- маленький HTTP-клиент (для проверки уже запущенного сервера просмотра) ----------
static int httpGet(int port, const string& path, string* body) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    DWORD to = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to, sizeof to);
    sockaddr_in sa = {};
    sa.sin_family = AF_INET; sa.sin_port = htons((u_short)port); sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (sockaddr*)&sa, sizeof sa) != 0) { closesocket(s); return 0; }
    string rq = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nConnection: close\r\n\r\n";
    sendAll(s, rq.data(), rq.size());
    string resp; char tmp[8192];
    for (;;) { int n = recv(s, tmp, sizeof tmp, 0); if (n <= 0) break; resp.append(tmp, n); }
    closesocket(s);
    if (resp.size() < 12 || resp.compare(0, 5, "HTTP/") != 0) return 0;
    int code = atoi(resp.c_str() + 9);
    size_t he = resp.find("\r\n\r\n");
    if (body) *body = he == string::npos ? string() : resp.substr(he + 4);
    return code;
}
static double jsonNum(const string& j, const char* key) {
    size_t p = j.find(string("\"") + key + "\":");
    return p == string::npos ? -1 : atof(j.c_str() + p + strlen(key) + 3);
}

// ---------- окно просмотра (бывший openViewer из batch.js) ----------
static wstring selfPath() { wchar_t b[MAX_PATH * 2]; DWORD n = GetModuleFileNameW(nullptr, b, MAX_PATH * 2); return wstring(b, n); }

static void openWindow(const wstring& url) {
    if (g_noWindow) return;
    logEnvOnce();
    wstring want;
    int mode = readBrowserTxt(&want);
    if (mode == 2) {
        if (exists(want)) { if (launchWindow(want, url, false)) return; }
        else logLine("окно просмотра: browser.txt - браузер не найден (" + toUtf8(want) + "), беру браузер по умолчанию");
    }
    {                                                       // нет файла, пустой или путь не сработал - браузер по умолчанию
        wstring def = defaultBrowser();
        if (def.empty()) logLine("окно просмотра: браузер по умолчанию не определён");
        else if (launchWindow(def, url, true)) return;
    }
    wstring br = edgeOrChrome();
    if (br.empty()) logLine("окно просмотра: Chrome / Edge не найден, открываю в браузере по умолчанию");
    else if (launchWindow(br, url, false)) return;
    INT_PTR sh = (INT_PTR)ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (sh <= 32) logLine("окно просмотра: ShellExecute не открыл адрес, код " + std::to_string((long long)sh));
}
static bool readSession(int* port, string* token) {          // work\viewer.session: порт и токен живого сервера просмотра
    string t;
    if (!readFile(g_work + L"\\viewer.session", t, 4096)) return false;
    int p = atoi(t.c_str());
    size_t sp = t.find(' ');
    if (p <= 0 || sp == string::npos) return false;
    string tk = t.substr(sp + 1);
    while (!tk.empty() && (tk.back() == '\r' || tk.back() == '\n' || tk.back() == ' ')) tk.pop_back();
    *port = p; *token = tk;
    return true;
}
static int openViewer(int port) {
    string token, body;
    int sp; string tk;
    if (readSession(&sp, &tk) && sp == port) token = tk;
    int st = token.empty() ? 0 : httpGet(port, "/status?k=" + token, &body);
    if (st == 200 && jsonNum(body, "started") != codeVer()) {   // сервер остался от прошлой версии кода - просим уйти
        httpGet(port, "/quit?k=" + token, nullptr);
        for (int i = 0; i < 15 && st == 200; i++) { Sleep(200); st = httpGet(port, "/status?k=" + token, nullptr); }
        logLine("Viewer: старый сервер заменён новым.");
        st = 0;
    }
    if (st != 200) {
        token = randomToken();
        string line = std::to_string(port) + " " + token;
        writeFile(g_work + L"\\viewer.session", line.data(), line.size());
        wstring cmd = L"\"" + selfPath() + L"\" serve --code \"" + g_code + L"\" --work \"" + g_work + L"\" --port " + std::to_wstring(port) + L" --token " + toWide(token);
        HANDLE hp, hj;
        bool ok = spawnJob(cmd, CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB, &hp, &hj);
        if (!ok) ok = spawnJob(cmd, CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, &hp, &hj);
        if (!ok) { logLine("Viewer: не удалось запустить сервер."); return 1; }
        if (hj) { JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {}; SetInformationJobObject(hj, JobObjectExtendedLimitInformation, &li, sizeof li); }   // сервер должен пережить нас
        if (hj) CloseHandle(hj);
        DWORD exitCode = STILL_ACTIVE;
        bool alive = true;
        int waited = 0;
        for (int i = 0; i < 75 && st != 200; i++) {                  // до 15 с: на слабой машине serve поднимается долго
            Sleep(200); waited += 200;
            st = httpGet(port, "/status?k=" + token, nullptr);
            if (st != 200 && WaitForSingleObject(hp, 0) == WAIT_OBJECT_0) { alive = false; GetExitCodeProcess(hp, &exitCode); break; }   // serve уже умер - ждать нечего
        }
        CloseHandle(hp);
        if (st != 200) {
            logLine("Viewer: сервер не запустился (порт " + std::to_string(port) + " занят?). Ждали " + std::to_string(waited) + " мс, /status=" + std::to_string(st) +
                    ", процесс serve " + (alive ? string("жив, но не отвечает") : "завершился, код " + std::to_string((unsigned long)exitCode)) + ".");
            return 1;
        }
        if (waited > 5000) logLine("Viewer: сервер поднялся за " + std::to_string(waited) + " мс.");
    }
    wstring url = L"http://127.0.0.1:" + std::to_wstring(port) + L"/viewer.html?k=" + toWide(token);
    openWindow(url);
    logLine("Viewer: http://127.0.0.1:" + std::to_string(port) + "/viewer.html");
    return 0;
}

// ---------- base64url и проверка подписи RSA-SHA256 (подпись архива обновления, update_mode.inc) ----------
static bool b64uDecode(const string& in, string& out) {
    out.clear();
    unsigned int acc = 0; int bits = 0;
    for (char c : in) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '-' || c == '+') v = 62;
        else if (c == '_' || c == '/') v = 63;
        else if (c == '=') break;
        else return false;
        acc = (acc << 6) | (unsigned)v; bits += 6;
        if (bits >= 8) { bits -= 8; out += (char)((acc >> bits) & 0xFF); }
    }
    return true;
}
static bool verifyRsaSha256Key(const unsigned char* blob, ULONG blobLen, const void* data, size_t dataLen, const string& sig) {
    BCRYPT_ALG_HANDLE hRsa = nullptr, hSha = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    BCRYPT_HASH_HANDLE hh = nullptr;
    bool ok = false;
    UCHAR digest[32];
    if (BCryptOpenAlgorithmProvider(&hRsa, BCRYPT_RSA_ALGORITHM, nullptr, 0) == 0 &&
        BCryptImportKeyPair(hRsa, nullptr, BCRYPT_RSAPUBLIC_BLOB, &hKey, (PUCHAR)blob, blobLen, 0) == 0 &&
        BCryptOpenAlgorithmProvider(&hSha, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(hSha, &hh, nullptr, 0, nullptr, 0, 0) == 0 &&
        BCryptHashData(hh, (PUCHAR)data, (ULONG)dataLen, 0) == 0 &&
        BCryptFinishHash(hh, digest, sizeof digest, 0) == 0) {
        BCRYPT_PKCS1_PADDING_INFO pad = { BCRYPT_SHA256_ALGORITHM };
        ok = BCryptVerifySignature(hKey, &pad, digest, sizeof digest, (PUCHAR)sig.data(), (ULONG)sig.size(), BCRYPT_PAD_PKCS1) == 0;
    }
    if (hh) BCryptDestroyHash(hh);
    if (hKey) BCryptDestroyKey(hKey);
    if (hSha) BCryptCloseAlgorithmProvider(hSha, 0);
    if (hRsa) BCryptCloseAlgorithmProvider(hRsa, 0);
    return ok;
}
// занятые DLL докера (Corel открыт): их нельзя открыть на запись; пустая строка - свободно (как FindLockedFiles установщика)
static wstring FindLockedFilesUpd(const wstring& wpanelRoot) {
    static const wchar_t* kFiles[] = { L"proxy.dll", L"proxy32.dll", L"bin\\main.dll", L"bin\\main32.dll" };
    wstring locked;
    for (const wchar_t* rel : kFiles) {
        wstring p = wpanelRoot + L"\\" + rel;
        if (!exists(p)) continue;
        HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); continue; }
        if (GetLastError() == ERROR_SHARING_VIOLATION) locked += wstring(L"  ") + rel + L"\n";
    }
    return locked;
}

#include "progress_window.inc"

// ---------- пакетный рендер ----------
// флаг из config.js вида `ключ: true` (докер пишет его при каждом рендере)
static bool cfgFlag(const char* key) {
    string cfg;
    if (!readFile(g_work + L"\\config.js", cfg, 1024 * 1024)) return false;
    size_t k = cfg.find(key);
    if (k == string::npos) return false;
    size_t c = cfg.find(':', k);
    if (c == string::npos) return false;
    c++;
    while (c < cfg.size() && (cfg[c] == ' ' || cfg[c] == '\t')) c++;
    return cfg.compare(c, 4, "true") == 0;
}

static int runBatch() {
    // appendLog: true (докер: рендер с зажатым Shift) - журнал не очищается, а дополняется: можно показать несколько сборок подряд
    bool appendLog = cfgFlag("appendLog");
    string hdr = string(appendLog ? "\n" : "") + "== " + isoNow() + " batch\n";
    writeFile(g_work + L"\\render.log", hdr.data(), hdr.size(), appendLog);
    g_batchT0 = GetTickCount64();
    logEnvOnce();
    makeDirs(inDir()); makeDirs(outDir());
    if (inputSvgs().empty()) { logLine("No SVG files found in input/"); return 0; }
    const LANGID ul = PRIMARYLANGID(GetUserDefaultUILanguage());
    const bool ru = ul == LANG_RUSSIAN || ul == LANG_UKRAINIAN || ul == LANG_BELARUSIAN || ul == LANG_KAZAK;
    const bool showUi = !g_batchNoUi && !g_noWindow;
    if (showUi) { prog::start(ru); prog::set(ru ? L"Запуск браузера..." : L"Starting the browser..."); }

    auto browsers = findBrowsers();
    if (browsers.empty()) {
        logLine("Не найден браузер для рендера. Установите Google Chrome или Microsoft Edge (на Windows 7 - Chrome 109) и запустите рендер снова.");
        return 20;
    }
    g_doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_token = randomToken();
    g_port = 0;
    if (!startServer(wstring())) return 1;
    wstring url = L"http://127.0.0.1:" + std::to_wstring(g_port) + L"/batch.html?k=" + toWide(g_token);

    const wchar_t* variants[] = { L"--headless=new", L"--headless", L"--headless=new --use-angle=swiftshader --enable-unsafe-swiftshader" };
    int result = 21;
    for (const wstring& br : browsers) {
        for (auto flags : variants) {
            wstring prof = envVar(L"TEMP") + L"\\wood_server_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount());
            wstring cmd = L"\"" + br + L"\" " + flags + L" --user-data-dir=\"" + prof + L"\" --no-first-run --no-default-browser-check --disable-extensions"
                          // как у puppeteer: без фоновых служб браузера и без понижения приоритета скрытой страницы (иначе расчёт
                          // идёт медленнее, пока рядом работает Corel)
                          L" --disable-background-networking --disable-component-update --disable-sync --disable-default-apps --disable-breakpad"
                          L" --disable-client-side-phishing-detection --disable-hang-monitor --disable-popup-blocking --disable-prompt-on-repost"
                          L" --disable-background-timer-throttling --disable-backgrounding-occluded-windows --disable-renderer-backgrounding"
                          L" --disable-ipc-flooding-protection --metrics-recording-only --password-store=basic"
                          L" --disable-features=Translate,MediaRouter,OptimizationHints,AcceptCHFrame"
                          L" --enable-logging --v=0 --window-size=1280,900 --hide-scrollbars --mute-audio \"" + url + L"\"";
            InterlockedExchange(&g_pageStarted, 0);
            touchActivity();
            HANDLE hp = nullptr, hj = nullptr;
            if (!spawnJob(cmd, CREATE_NO_WINDOW | ABOVE_NORMAL_PRIORITY_CLASS, &hp, &hj)) { logLine("Не удалось запустить " + toUtf8(br)); continue; }
            logLine("браузер запущен: " + toUtf8(br) + " " + toUtf8(flags));
            ULONGLONG t0 = GetTickCount64();
            int state = 0;                                    // 1 - готово, 2 - браузер упал/не стартовал, 3 - завис
            bool stubGone = false;                            // запущенный процесс вышел с кодом 0, а страница ещё грузится: у Chrome / Edge бывает процесс-заглушка (перезапуск себя, политика, повышенные права) - настоящий браузер живёт дальше
            for (;;) {
                HANDLE hs[2] = { g_doneEvent, hp };
                DWORD w = WaitForMultipleObjects(stubGone ? 1 : 2, hs, FALSE, 250);
                if (w == WAIT_OBJECT_0) { state = 1; break; }
                if (w == WAIT_OBJECT_0 + 1) {
                    DWORD ec = STILL_ACTIVE; GetExitCodeProcess(hp, &ec);
                    if (WaitForSingleObject(g_doneEvent, 500) == WAIT_OBJECT_0) { state = 1; break; }
                    if (ec == 0 && !g_pageStarted) { stubGone = true; logLine("запущенный процесс браузера вышел с кодом 0, жду страницу (возможно, браузер перезапустил себя)"); touchActivity(); continue; }
                    state = 2; break;
                }
                ULONGLONG now = GetTickCount64();
                if (stubGone && !g_pageStarted && now - (ULONGLONG)g_lastActivity > 15000) { state = 2; break; }     // за заглушкой никто не пришёл
                if (!g_pageStarted && now - t0 > 40000) { state = 2; break; }
                if (g_pageStarted && now - (ULONGLONG)g_lastActivity > 180000) { state = 3; break; }
            }
            { DWORD bec = STILL_ACTIVE; GetExitCodeProcess(hp, &bec);
              logLine(state == 1 ? "страница сообщила: готово" : "ожидание закончилось без результата (код выхода браузера " + (bec == STILL_ACTIVE ? std::string("работает") : exitCodeText(bec)) + ")"); }
            if (hj) CloseHandle(hj); else TerminateProcess(hp, 0);     // KILL_ON_JOB_CLOSE
            WaitForSingleObject(hp, 3000);
            CloseHandle(hp);
            logBrowserLog(prof);
            for (int i = 0; i < 10; i++) { rmTree(prof); if (!exists(prof)) break; Sleep(200); }
            logLine("браузер закрыт, профиль удалён");
            if (state == 1) {
                // страница не смогла загрузить свои модули (ошибка import, журналы пользователей 21.10.2026: Chrome 155 отдаёт «Failed to fetch dynamically imported module» сразу после загрузки файлов):
                // это сбой запуска именно в этом браузере / с этими флагами, а не ошибка рендера - пробуем следующий вариант и следующий браузер (Edge), как при зависании
                if (!g_batchOk && g_lastErr.find("Failed to fetch dynamically imported module") != string::npos) {
                    logLine("Браузер не смог загрузить модули страницы: " + toUtf8(br) + " " + toUtf8(flags) + ", пробую другой вариант.");
                    ResetEvent(g_doneEvent);
                    g_lastErr.clear();
                    InterlockedExchange(&g_batchOk, 0);
                    InterlockedExchange(&g_pageStarted, 0);
                    continue;
                }
                result = g_batchOk ? 0 : 1; goto finished;
            }
            if (state == 3) { logLine("Рендер завис: нет ответа от страницы 3 минуты."); result = 22; goto finished; }
            if (g_pageStarted) { logLine("Браузер закрылся до окончания рендера."); result = 21; goto finished; }
            logLine("Страница не запустилась: " + toUtf8(br) + " " + toUtf8(flags) + ", пробую другой вариант.");
        }
    }
    logLine("Не удалось запустить браузер для рендера. Установите свежий Google Chrome или Microsoft Edge (на Windows 7 - Chrome 109).");
finished:
    prog::stop();
    if (result != 0 && showUi) {
        wstring m;
        if (result == 20) m = ru ? L"Не найден браузер для рендера. Установите Google Chrome или Microsoft Edge (на Windows 7 - Chrome 109) и запустите рендер снова."
                                 : L"No browser found for rendering. Install Google Chrome or Microsoft Edge (Chrome 109 on Windows 7) and try again.";
        else if (result == 22) m = ru ? L"Рендер завис: страница не отвечала 3 минуты." : L"The render stopped responding for 3 minutes.";
        else if (result == 21) m = ru ? L"Браузер не запустился или закрылся до конца рендера. Установите свежий Google Chrome или Microsoft Edge." : L"The browser did not start or closed before the render finished. Install a current Google Chrome or Microsoft Edge.";
        else m = ru ? L"Рендер завершился с ошибкой." : L"The render finished with an error.";
        if (!g_lastErr.empty()) m += L"\n\n" + toWide(g_lastErr);
        m += (ru ? L"\n\nЖурнал: " : L"\n\nLog: ") + g_work + L"\\render.log";
        MessageBoxW(nullptr, m.c_str(), ru ? L"Рендер коробки" : L"Box render", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
    }
    if (result == 0 && g_wantView) result = openViewer((int)g_viewPort);
    return result;
}

#include "update_mode.inc"

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    InitializeCriticalSection(&g_logCs);
    wstring cmd = argc > 1 ? argv[1] : L"";
    if (cmd == L"render") cmd = L"serve";
    if (cmd == L"update") {                  // автообновление докера: лицензия рендера не нужна, защита - подпись архива
        WSADATA wsaU;
        WSAStartup(MAKEWORD(2, 2), &wsaU);
        return upd::run(argc, argv);
    }
    if (cmd == L"api") {                     // запросы докера к сайту (panel_api.php): сам Corel в сеть не ходит
        return upd::api(argc, argv);
    }
    if (cmd != L"serve" && cmd != L"batch" && cmd != L"view") {
        fprintf(stderr, "wood_server.exe serve|batch|view|update [--code <dir>] [--work <dir>] [--port N] [--token T] [--portfile f] [--verbose]\n"
                        "  serve - сервер (порт, токен); batch - пакетный рендер из work\\input; view - открыть окно просмотра\n");
        return 2;
    }
    wstring portfile;
    int viewPort = 47653;
    for (int i = 2; i < argc; i++) {
        wstring a = argv[i], v;
        auto next = [&](wstring& dst) { if (i + 1 < argc) dst = argv[++i]; };
        if (a == L"--code") next(g_code);
        else if (a == L"--work") next(g_work);
        else if (a == L"--port") { next(v); g_port = _wtoi(v.c_str()); }
        else if (a == L"--viewer-port") { next(v); viewPort = _wtoi(v.c_str()); }
        else if (a == L"--token") { next(v); g_token = toUtf8(v); }
        else if (a == L"--portfile") next(portfile);
        else if (a == L"--verbose") g_verbose = true;
        else if (a == L"--nowindow") g_noWindow = true;
        else if (a == L"--no-ui") g_batchNoUi = true;
        // --serial от прежней main.dll не нужен (подписку проверяет только DLL при запуске) - неизвестные аргументы пропускаются
    }
    if (g_code.empty()) g_code = envVar(L"WOOD_RENDER_CODE");   // разработка: код рендера не в докере (Repo\render)
    if (g_code.empty()) {                                  // докер: wpanel\bin\wood_server.exe, код рендера - wpanel\render
        wstring me = selfPath();
        wchar_t full[MAX_PATH * 2];
        wstring cand = me.substr(0, me.find_last_of(L'\\')) + L"\\..\\render";
        DWORD n = GetFullPathNameW(cand.c_str(), MAX_PATH * 2, full, nullptr);
        if (n > 0 && n < MAX_PATH * 2) g_code = wstring(full, n);
    }
    if (g_code.empty()) { fprintf(stderr, "нет --code\n"); return 2; }
    if (g_work.empty()) g_work = g_code;
    auto trimSlash = [](wstring& w) { while (w.size() > 3 && (w.back() == L'\\' || w.back() == L'/')) w.pop_back(); };
    trimSlash(g_code); trimSlash(g_work);
    if (!exists(g_code + L"\\viewer.html") || !exists(g_code + L"\\src\\renderer.js")) {
        fprintf(stderr, "Не найден код рендера (папка render докера): %s\n", toUtf8(g_code).c_str());
        return 50;
    }
    g_started = GetTickCount64();
    g_startVer = codeVer();

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;

    if (cmd == L"batch") return runBatch();
    if (cmd == L"view") {
        if (inputSvgs().empty()) { logLine("Viewer: нет прежнего рендера (папка input пуста). Сделайте «Экспорт SVG + рендер»."); return 0; }
        return openViewer(viewPort);
    }
    if (!startServer(portfile)) return 1;
    CreateThread(nullptr, 0, idleThread, nullptr, 0, nullptr);
    Sleep(INFINITE);
    return 0;
}
