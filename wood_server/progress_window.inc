// progress_window.inc - маленькое окно "Рендер коробки..." для пакетного режима (batch). Включается в main.cpp.
// Рендер запускает DLL без консоли, поэтому без окна не видно, что что-то происходит и когда оно закончится.
// Окно живёт в своём потоке: последняя строка журнала, бегущая полоса и время; закрывается само по окончании.
#include <commctrl.h>

namespace prog {

static HWND g_wnd = nullptr, g_label = nullptr, g_time = nullptr;
static HANDLE g_thread = nullptr, g_ready = nullptr;
static ULONGLONG g_t0 = 0;
static bool g_ru = true;
static const UINT WM_SET = WM_APP + 10, WM_END = WM_APP + 11;

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_SET: { wstring* s = (wstring*)l; SetWindowTextW(g_label, s->c_str()); delete s; return 0; }
    case WM_TIMER: {
        ULONGLONG sec = (GetTickCount64() - g_t0) / 1000;
        wchar_t b[64];
        swprintf(b, _countof(b), g_ru ? L"Прошло: %llu с" : L"Elapsed: %llu s", sec);
        SetWindowTextW(g_time, b);
        return 0;
    }
    case WM_CLOSE: return 0;                                   // закрыть вручную нельзя: окно уйдёт само, когда рендер закончится
    case WM_END: DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI thread(LPVOID)
{
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);
    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = proc; wc.hInstance = hi; wc.lpszClassName = L"WoodRenderProgress";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    int W = 460, H = 130;
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, g_ru ? L"Рендер коробки" : L"Box render", WS_CAPTION,
                            (GetSystemMetrics(SM_CXSCREEN) - W) / 2, (GetSystemMetrics(SM_CYSCREEN) - H) / 3, W, H, nullptr, nullptr, hi, nullptr);
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    g_label = CreateWindowExW(0, L"STATIC", g_ru ? L"Запуск..." : L"Starting...", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 16, 14, 420, 18, g_wnd, nullptr, hi, nullptr);
    HWND bar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | 0x08 /*PBS_MARQUEE*/, 16, 40, 420, 14, g_wnd, nullptr, hi, nullptr);
    g_time = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 16, 64, 420, 18, g_wnd, nullptr, hi, nullptr);
    SendMessageW(g_label, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(g_time, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(bar, 0x040A /*PBM_SETMARQUEE*/, TRUE, 40);
    SetTimer(g_wnd, 1, 500, nullptr);
    ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
    SetEvent(g_ready);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    g_wnd = nullptr;
    return 0;
}

static void start(bool ru)
{
    g_ru = ru;
    g_t0 = GetTickCount64();
    g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, thread, nullptr, 0, nullptr);
    if (g_thread) WaitForSingleObject(g_ready, 3000);
}
static void set(const wstring& s)
{
    HWND h = g_wnd;
    if (h) PostMessageW(h, WM_SET, 0, (LPARAM)new wstring(s));
}
static void stop()
{
    HWND h = g_wnd;
    if (h) PostMessageW(h, WM_END, 0, 0);
    if (g_thread) { WaitForSingleObject(g_thread, 2000); CloseHandle(g_thread); g_thread = nullptr; }
}

} // namespace prog
