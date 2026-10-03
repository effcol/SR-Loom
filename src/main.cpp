// SR Weaver — tray app that weaves a side-by-side stereo source onto a
// Simulated Reality display. Milestone 1: tray icon + a static SBS test image
// woven into a window that can toggle between fullscreen and windowed.

#include "Common.h"
#include "Renderer.h"
#include "Present12.h"
#include "SRWeaver.h"
#include "TrayIcon.h"
#include "UpdateChecker.h"
#include "Capture.h"
#include "CaptureDXGI.h"
#include "Converter.h"
#include "RegionWeave.h"
#include "ScreenAnalysis.h"
#include "GpuTrack.h"
#include <map>
#include <deque>
#include <future>
#include <execution>   // parallel region tracking
#include <unordered_map>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>
#include <type_traits>
#include "Detector.h"
#include "Gui.h"
#include "Settings.h"
#include "Profiles.h"
#include "MediaProfiles.h"
#include "VideoSource.h"
#include "KatangaSource.h"
#include "LFPReader.h"
#include "LFPRenderer.h"
#include "OpenTrackBridge.h"
#include "../third_party/one_euro_filter.h"
#include "resource.h"

#include <shellscalingapi.h>
#include <dwmapi.h>
#include <dbghelp.h>
#include <imm.h>
#include <timeapi.h>   // timeBeginPeriod
#include <avrt.h>      // AvSetMmThreadCharacteristics (MMCSS)
#pragma comment(lib, "avrt.lib")
#include <tuple>
#include <commdlg.h>
#include <windowsx.h>
#include <shellapi.h>      // DragAcceptFiles, DragQueryFile, DragFinish
#include <shlobj.h>        // SHGetKnownFolderPath, SHCreateDirectoryExW (NPClient extract)
#include <cmath>
#include <vector>
#include <algorithm>
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")   // timeBeginPeriod
#pragma comment(lib, "dbghelp.lib")  // stall sampler: function names for the render thread's stack
#pragma comment(lib, "imm32.lib")    // ImmDisableIME
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "advapi32.lib")  // RegOpenKeyEx / RegCreateKeyEx / RegSetValueEx

using namespace srw;

namespace {
    // Lookup an embedded RCDATA resource (e.g. NPClient64.dll) by id ->
    // pointer + size. Module-owned memory, valid for the process lifetime.
    // Duplicated from Gui.cpp's anonymous-namespace helper to avoid
    // making EmbeddedResource a public srw:: symbol just for one call site.
    const void* LookupEmbeddedResource(int id, size_t& size)
    {
        size = 0;
        HMODULE mod = GetModuleHandleA(nullptr);
        HRSRC res = FindResourceA(mod, MAKEINTRESOURCEA(id), (LPCSTR)RT_RCDATA);
        if (!res) return nullptr;
        HGLOBAL h = LoadResource(mod, res);
        if (!h) return nullptr;
        size = SizeofResource(mod, res);
        return LockResource(h);
    }
}

// --- Undocumented NtQuerySystemInformation plumbing -----------------------
// Used to identify the Katanga publishing process via shared-kernel-object
// handle matching. Same technique Process Explorer / Handle.exe use. The
// struct layout has been stable since Vista; the SystemExtendedHandle-
// Information class returns 64-bit-safe PIDs/handles on Win64.
extern "C" {
    typedef LONG NTSTATUS;
    #ifndef NT_SUCCESS
    #define NT_SUCCESS(x) (((NTSTATUS)(x)) >= 0)
    #endif
    #ifndef STATUS_INFO_LENGTH_MISMATCH
    #define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)
    #endif
    typedef struct _SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX {
        PVOID     Object;
        ULONG_PTR UniqueProcessId;
        ULONG_PTR HandleValue;
        ULONG     GrantedAccess;
        USHORT    CreatorBackTraceIndex;
        USHORT    ObjectTypeIndex;
        ULONG     HandleAttributes;
        ULONG     Reserved;
    } SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX;
    typedef struct _SYSTEM_HANDLE_INFORMATION_EX {
        ULONG_PTR                          NumberOfHandles;
        ULONG_PTR                          Reserved;
        SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX  Handles[1];
    } SYSTEM_HANDLE_INFORMATION_EX;
}
static constexpr int kSystemExtendedHandleInformation = 64;

namespace
{
    constexpr char  kWindowClass[] = "SRWeaverWindow";
    constexpr char  kWindowTitle[] = "SR Loom";
    constexpr int   kHotkeyToggle    = 1;   // Ctrl+Alt+W : enable/disable weaving
    constexpr int   kHotkeyMode      = 2;   // Ctrl+Alt+F : fullscreen/windowed
    constexpr int   kHotkeyCapture   = 3;   // Ctrl+Alt+C : make active window 3D
    constexpr int   kHotkeyDetect    = 5;   // Ctrl+Alt+D : auto-detect stereo format
    constexpr int   kHotkeyCalibrate = 6;   // Ctrl+Alt+R : recenter head tracking
    constexpr int   kHotkeyAutoRegion = 7;  // Ctrl+Alt+A : Auto Stereo -- toggle region under cursor
    constexpr int   kHotkeyAutoRegionDbg = 8; // Ctrl+Alt+Shift+A : same, and save what the finder saw (debug)
    constexpr UINT  kRenderTimer   = 1;   // drives rendering during modal move/resize

    // ---- Render thread / UI thread ---------------------------------------
    // The UI thread (WinMain's) owns every window and pumps their messages; the
    // render thread runs the loop body (logic + RenderFrame) and owns none. A
    // thread that owns windows runs other programs' hooks inside PeekMessage,
    // and one of those now and then blocks it for 16-30 ms: with the render
    // loop on that thread, a frame or two was lost every 10-20 s in every mode.
    //
    // The two never run SR Loom's code at the same time: one lock (g_appLock)
    // is held by whichever is running it. The render thread lets go of it
    // while it waits for the next refresh (most of every frame); a window
    // procedure takes it for as long as it handles a message. The system's
    // waits inside PeekMessage happen outside it, so they hold up no frame.
    //
    // Windows are only ever changed from the UI thread: the Win32 calls that
    // do so are replaced below by wrappers of the same name, which run the call
    // on the UI thread (UiCall) when the render thread makes it.
    // Settings RenderThread = 0: everything on one thread, as before.
    std::recursive_timed_mutex g_appLock;
    std::atomic<bool>    g_threaded{ false };       // the render thread is running
    DWORD                g_uiThreadId = 0;
    DWORD                g_renderThreadId = 0;
    HWND                 g_uiCallWnd = nullptr;     // (message-only; UiCall's target)
    constexpr UINT       kUiCallMsg = WM_USER + 1;
    thread_local int     t_appLockDepth = 0;        // this thread's holds of g_appLock

    // (Only the render thread hands its window calls over: the UI thread makes
    // them itself, and so do helper threads with windows of their own -- the
    // tray menu's.)
    inline bool OnRenderThread() { return g_threaded.load(std::memory_order_relaxed) && GetCurrentThreadId() == g_renderThreadId; }

    // Hold the lock for a scope (window procedures, the loop body).
    struct AppLock
    {
        bool held = false;
        AppLock()
        {
            if (!g_threaded.load(std::memory_order_relaxed)) return;
            if (g_appLock.try_lock()) { held = true; ++t_appLockDepth; return; }
            // A message SENT by another thread: the sender may be the render
            // thread itself, blocked inside a window call until this returns
            // while it holds the lock. Waited for a while; then the message is
            // handled without it (the render thread, blocked, runs nothing).
            const bool sent = InSendMessage() != FALSE;
            const ULONGLONG t0 = GetTickCount64();
            for (;;)
            {
                if (g_appLock.try_lock()) { held = true; ++t_appLockDepth; return; }
                if (sent && GetTickCount64() - t0 > 250)
                {
                    static std::atomic<int> s_logged{ 0 };
                    if (s_logged.fetch_add(1) < 5)
                        Log("AppLock: a sent window message waited 250 ms for the render thread -- handled without the lock (is a window call of its not going through UiCall?)");
                    return;
                }
                // (Posted message: let messages sent to this thread in meanwhile --
                // the render thread's own, if it is waiting on one.)
                if (!sent) { MSG m; PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE); }
                if (g_appLock.try_lock_for(std::chrono::milliseconds(2))) { held = true; ++t_appLockDepth; return; }
            }
        }
        ~AppLock() { if (held) { --t_appLockDepth; g_appLock.unlock(); } }
        AppLock(const AppLock&) = delete;
        AppLock& operator=(const AppLock&) = delete;
    };

    // Let go of the lock for a scope: the render loop's waits, and while the
    // UI thread does something for it. (Only this thread's own outermost hold.)
    struct AppUnlock
    {
        bool did = false;
        AppUnlock() { if (g_threaded.load(std::memory_order_relaxed) && t_appLockDepth == 1) { did = true; --t_appLockDepth; g_appLock.unlock(); } }
        ~AppUnlock() { if (did) { g_appLock.lock(); ++t_appLockDepth; } }
        AppUnlock(const AppUnlock&) = delete;
        AppUnlock& operator=(const AppUnlock&) = delete;
    };

    LRESULT CALLBACK UiCallWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        if (m == kUiCallMsg)
        {
            AppLock lock;
            (*reinterpret_cast<std::function<void()>*>(l))();
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }

    // Run f on the UI thread and return what it returns. On the UI thread (or
    // with no render thread) that's a plain call; from the render thread the
    // lock is let go while the UI thread runs it.
    template <typename F>
    auto UiCall(F&& f) -> decltype(f())
    {
        if (!OnRenderThread() || !g_uiCallWnd) return f();
        if constexpr (std::is_void_v<decltype(f())>)
        {
            std::function<void()> thunk = [&] { f(); };
            AppUnlock unlock;
            SendMessageW(g_uiCallWnd, kUiCallMsg, 0, reinterpret_cast<LPARAM>(&thunk));
        }
        else
        {
            decltype(f()) result{};
            std::function<void()> thunk = [&] { result = f(); };
            { AppUnlock unlock; SendMessageW(g_uiCallWnd, kUiCallMsg, 0, reinterpret_cast<LPARAM>(&thunk)); }
            return result;
        }
    }

    // The window-changing Win32 calls, by way of the UI thread. (Each real one
    // sends messages to the window's own thread and waits for it; made from
    // the render thread while it holds the lock, that thread could not answer.)
    inline BOOL Ui_SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT fl)
    { return UiCall([&] { return ::SetWindowPos(h, after, x, y, cx, cy, fl); }); }
    inline BOOL Ui_ShowWindow(HWND h, int cmd) { return UiCall([&] { return ::ShowWindow(h, cmd); }); }
    inline LONG_PTR Ui_SetWindowLongPtrA(HWND h, int idx, LONG_PTR v) { return UiCall([&] { return ::SetWindowLongPtrA(h, idx, v); }); }
    inline LONG_PTR Ui_SetWindowLongPtrW(HWND h, int idx, LONG_PTR v) { return UiCall([&] { return ::SetWindowLongPtrW(h, idx, v); }); }
    inline int  Ui_SetWindowRgn(HWND h, HRGN r, BOOL redraw) { return UiCall([&] { return ::SetWindowRgn(h, r, redraw); }); }
    inline BOOL Ui_DestroyWindow(HWND h) { return UiCall([&] { return ::DestroyWindow(h); }); }
    inline HWND Ui_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int hgt,
                                   HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
    { return UiCall([&] { return ::CreateWindowExA(ex, cls, name, style, x, y, w, hgt, parent, menu, inst, param); }); }
    inline HWND Ui_CreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int w, int hgt,
                                   HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
    { return UiCall([&] { return ::CreateWindowExW(ex, cls, name, style, x, y, w, hgt, parent, menu, inst, param); }); }
    inline UINT_PTR Ui_SetTimer(HWND h, UINT_PTR id, UINT ms, TIMERPROC proc) { return UiCall([&] { return ::SetTimer(h, id, ms, proc); }); }
    inline BOOL Ui_KillTimer(HWND h, UINT_PTR id) { return UiCall([&] { return ::KillTimer(h, id); }); }
    inline BOOL Ui_SetForegroundWindow(HWND h) { return UiCall([&] { return ::SetForegroundWindow(h); }); }
    // (Mouse capture belongs to a thread: the windows' one.)
    inline HWND Ui_GetCapture() { return UiCall([] { return ::GetCapture(); }); }
    inline HWND Ui_SetCapture(HWND h) { return UiCall([&] { return ::SetCapture(h); }); }
    inline BOOL Ui_ReleaseCapture() { return UiCall([] { return ::ReleaseCapture(); }); }
#define SetWindowPos        Ui_SetWindowPos
#define ShowWindow          Ui_ShowWindow
#define SetWindowLongPtrA   Ui_SetWindowLongPtrA
#define SetWindowLongPtrW   Ui_SetWindowLongPtrW
#define SetWindowRgn        Ui_SetWindowRgn
#define DestroyWindow       Ui_DestroyWindow
#define CreateWindowExA     Ui_CreateWindowExA
#define CreateWindowExW     Ui_CreateWindowExW
#define SetTimer            Ui_SetTimer
#define KillTimer           Ui_KillTimer
#define SetForegroundWindow Ui_SetForegroundWindow
#define GetCapture          Ui_GetCapture
#define SetCapture          Ui_SetCapture
#define ReleaseCapture      Ui_ReleaseCapture

    // Walk a suspended thread's stack from its context (x64 unwind data).
    // Plain function: __try can't share a frame with C++ objects to unwind.
    // A bad stack (mid-prologue, JIT code) just ends the walk early.
    int WalkStack(CONTEXT ctx, DWORD64* pcs, int maxPcs)
    {
        int n = 0;
        __try
        {
            while (n < maxPcs && ctx.Rip)
            {
                pcs[n++] = ctx.Rip;
                DWORD64 base = 0;
                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
                if (!fn)   // leaf function: the return address is on top of the stack
                {
                    ctx.Rip = *(const DWORD64*)ctx.Rsp;
                    ctx.Rsp += 8;
                    continue;
                }
                PVOID handlerData = nullptr;
                DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &handlerData, &frame, nullptr);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return n;
    }

    // Stall sampler: the render thread marks where it is (Mark); when it has
    // sat on one mark for 15 ms+, a watchdog thread suspends it for a moment,
    // copies its call stack and logs it by function name. That names what a
    // stall is WAITING in (the graphics driver, the window manager, a lock),
    // which timing the sections alone can't. Only while weaving (Arm).
    class StallSampler
    {
    public:
        void Start()
        {
            if (m_thread.joinable()) return;
            m_tid = GetCurrentThreadId();   // (called on the render thread)
            m_run = true;
            m_thread = std::thread([this] { Loop(); });
        }
        void Stop()
        {
            m_run = false;
            if (m_thread.joinable()) m_thread.join();
        }
        void Arm(bool on) { m_armed = on; }
        // `what` must be a string literal (or otherwise live forever).
        void Mark(const char* what)
        {
            // (Only the watched thread's marks: the UI thread has work of its own.)
            if (m_tid && GetCurrentThreadId() != m_tid) return;
            LARGE_INTEGER q{}; QueryPerformanceCounter(&q);
            m_what = what;
            m_since = q.QuadPart;
            m_seq.fetch_add(1);
        }
        const char* Current() const { return m_what.load(); }

    private:
        void Loop()
        {
            LARGE_INTEGER f{}; QueryPerformanceFrequency(&f);
            const double toMs = 1000.0 / (double)f.QuadPart;
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, m_tid);
            if (!th) return;
            unsigned lastSeq = 0;
            int samplesThisMark = 0;
            while (m_run)
            {
                Sleep(3);
                if (!m_armed) continue;
                const unsigned seq = m_seq.load();
                const char* what = m_what.load();
                LARGE_INTEGER q{}; QueryPerformanceCounter(&q);
                const double ms = (double)(q.QuadPart - m_since.load()) * toMs;
                if (seq != lastSeq) { lastSeq = seq; samplesThisMark = 0; }
                // One sample at 15 ms and more at 40 / 100 ms for a long stall
                // (the second shows whether it's still in the same wait).
                static const double kAt[] = { 15.0, 40.0, 100.0 };
                if (samplesThisMark >= 3 || ms < kAt[samplesThisMark] || !what) continue;
                ++samplesThisMark;

                DWORD64 pcs[48];
                int n = 0;
                if (SuspendThread(th) != (DWORD)-1)
                {
                    // Nothing that could take a lock the render thread holds
                    // (heap, log queue) until it's resumed.
                    CONTEXT ctx{};
                    ctx.ContextFlags = CONTEXT_FULL;
                    if (GetThreadContext(th, &ctx) && m_seq.load() == seq)
                        n = WalkStack(ctx, pcs, 48);
                    ResumeThread(th);
                }
                if (n > 0) LogStack(what, ms, pcs, n);
            }
            CloseHandle(th);
        }

        void LogStack(const char* what, double ms, const DWORD64* pcs, int n)
        {
            HANDLE proc = GetCurrentProcess();
            if (!m_symInit)
            {
                SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
                m_symInit = SymInitialize(proc, nullptr, TRUE) != FALSE;
            }
            else
                SymRefreshModuleList(proc);   // (DLLs loaded since)
            std::string line;
            int shown = 0;
            for (int i = 0; i < n && shown < 14; ++i)
            {
                char mod[MAX_PATH] = "?";
                HMODULE hm = nullptr;
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCSTR)pcs[i], &hm) && hm)
                {
                    GetModuleFileNameA(hm, mod, MAX_PATH);
                    if (char* s = strrchr(mod, '\\')) memmove(mod, s + 1, strlen(s));
                    if (char* d = strrchr(mod, '.')) *d = '\0';
                }
                char name[160] = "";
                if (m_symInit)
                {
                    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 128] = {};
                    SYMBOL_INFO* si = reinterpret_cast<SYMBOL_INFO*>(buf);
                    si->SizeOfStruct = sizeof(SYMBOL_INFO);
                    si->MaxNameLen = 127;
                    DWORD64 disp = 0;
                    if (SymFromAddr(GetCurrentProcess(), pcs[i], &disp, si))
                        _snprintf_s(name, _TRUNCATE, "%s", si->Name);
                }
                char frame[320];
                if (name[0]) _snprintf_s(frame, _TRUNCATE, "%s!%s", mod, name);
                else         _snprintf_s(frame, _TRUNCATE, "%s+0x%llX", mod,
                                         (unsigned long long)(hm ? pcs[i] - (DWORD64)hm : pcs[i]));
                if (!line.empty()) line += " < ";
                line += frame;
                ++shown;
            }
            Log("Stall: in \"%s\" for %.0f ms so far, waiting in: %s", what, ms, line.c_str());
        }

        std::thread              m_thread;
        std::atomic<bool>        m_run{ false }, m_armed{ false };
        std::atomic<const char*> m_what{ nullptr };
        std::atomic<long long>   m_since{ 0 };
        std::atomic<unsigned>    m_seq{ 0 };
        DWORD                    m_tid = 0;
        bool                     m_symInit = false;
    };
    StallSampler g_stall;

    // Hitch watchdog: time a section of the render thread and log it when it
    // takes too long (a frame of ~6 ms at 160 Hz; 20 ms+ is a visible hitch).
    // Also marks the section for the stall sampler.
    struct HitchWatch
    {
        const char* what;
        const char* outer = g_stall.Current();
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        double cpu0 = ThreadCpuMs();
        explicit HitchWatch(const char* w) : what(w) { g_stall.Mark(w); }
        ~HitchWatch()
        {
            g_stall.Mark(outer);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (ms > 20.0) Log("Hitch: %s took %.1f ms (running %.1f ms of it)", what, ms, ThreadCpuMs() - cpu0);
        }
    };

    // GPU timestamps around each loop's work, for the frame profile: how long
    // the GPU spends on our part (capture copy, analysis, tracking, crops,
    // conversions), the SR weave, and the mask + present. A ring of query
    // sets, read back a few frames later without ever stalling.
    struct GpuFrameTimer
    {
        // Timestamps, in frame order. Any not reached in a frame (e.g. the
        // Auto Stereo ones in other modes) are stamped at End (zero length).
        // (kPresent: up to the present. kEnd: what ran after it -- a conversion
        // put off until the frame was on its way, see deferConvert.)
        enum Mark { kStart, kCapture, kAnalysis, kTracking, kOurs, kWeave, kMask, kPresent, kEnd, kCount };
        static constexpr int kRing = 6;
        struct Set { ID3D11Query* disjoint = nullptr; ID3D11Query* ts[kCount] = {}; bool pending = false; };
        Set                  sets[kRing];
        int                  cur = 0;
        bool                 active = false;   // this frame is being timed
        bool                 marked[kCount] = {};
        ID3D11DeviceContext* ctx = nullptr;
        double               seg[kCount] = {}; // seg[i] = ts[i] - ts[i-1], summed
        double               lastConvMs = 0.0; // the latest frame's conversion (after the capture, up to the weave)
        unsigned             lastConvSerial = 0;
        int                  n = 0;

        bool Init(ID3D11Device* dev, ID3D11DeviceContext* c)
        {
            ctx = c;
            D3D11_QUERY_DESC dj{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
            D3D11_QUERY_DESC ts{ D3D11_QUERY_TIMESTAMP, 0 };
            for (Set& s : sets)
            {
                if (FAILED(dev->CreateQuery(&dj, &s.disjoint))) return false;
                for (auto& q : s.ts) if (FAILED(dev->CreateQuery(&ts, &q))) return false;
            }
            return true;
        }
        void Begin()
        {
            Collect();
            if (active) { ctx->End(sets[cur].disjoint); active = false; }   // last frame bailed out early: drop it
            Set& s = sets[cur];
            active = ctx && s.disjoint && !s.pending;
            if (!active) return;
            for (bool& m : marked) m = false;
            ctx->Begin(s.disjoint);
            Stamp(kStart);
        }
        void Stamp(int i)
        {
            if (!active || marked[i]) return;
            // Earlier marks this frame skipped get the same moment (zero length).
            for (int k = 0; k < i; ++k) if (!marked[k]) { ctx->End(sets[cur].ts[k]); marked[k] = true; }
            ctx->End(sets[cur].ts[i]);
            marked[i] = true;
        }
        void End()
        {
            if (!active) return;
            Stamp(kEnd);
            ctx->End(sets[cur].disjoint);
            sets[cur].pending = true;
            cur = (cur + 1) % kRing;
            active = false;
        }
        void Collect()
        {
            for (Set& s : sets)
            {
                if (!s.pending) continue;
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
                if (ctx->GetData(s.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
                UINT64 t[kCount] = {};
                bool ok = true;
                for (int i = 0; i < kCount && ok; ++i)
                    ok = ctx->GetData(s.ts[i], &t[i], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
                if (!ok) continue;
                s.pending = false;
                if (dj.Disjoint || dj.Frequency == 0) continue;
                const double k = 1000.0 / (double)dj.Frequency;
                for (int i = 1; i < kCount; ++i) seg[i] += (double)(t[i] - t[i - 1]) * k;
                lastConvMs = (double)(t[kOurs] - t[kCapture]) * k;
                ++lastConvSerial;
                ++n;
            }
        }
        double Avg(int i) const { return n ? seg[i] / n : 0.0; }
        void Reset() { for (double& v : seg) v = 0; n = 0; }
    };

    struct AppState
    {
        HWND       hwnd          = nullptr;
        Renderer     renderer;
        SRWeaver     weaver;
        TrayIcon     tray;
        Capture      capture;        // WGC: default; only API that can do per-window + window-exclusion
        CaptureDXGI  captureDxgi;    // Output Duplication fallback for exclusive-fullscreen sources
        Converter    converter;
        // Auto Stereo (region weaving). While autoStereo is on, the weave
        // window covers the SR display (Fullscreen mode, click-through) but
        // is clipped to the regions in regionWeaver; each region is converted
        // with its own format into a composite that the weaver weaves.
        // Stage 1 (foundation): regions are added by hand with Ctrl+Alt+A
        // (the window under the cursor, in the current format) and follow
        // that window. Detection of image regions / 3D formats comes next.
        RegionWeaver regionWeaver;
        bool         autoStereo      = false;
        bool         regionsDirty    = false;   // region set/rects changed -> rebuild + re-clip
        bool         autoStereoLensOn = false;  // lens hint state while in Auto Stereo
        // Screen analysis (half-res luma readback) + per-region content
        // trackers. The analyser only runs while a pick is pending or a
        // content-tracked region exists.
        ScreenAnalyzer               analyzer;
        LumaImage                    analysisImg;
        std::map<int, RegionTracker> regionTrackers;   // region id -> tracker
        // Ctrl+Alt+A "pick": find the image under this point on the next
        // analysed frame (falls back to the whole window if none / timeout).
        bool         pickPending   = false;
        POINT        pickPoint{};
        HWND         pickWindow    = nullptr;
        DWORD        pickStartMs   = 0;
        StereoFormat pickFormat    = StereoFormat::HalfSBS;
        bool         pickDebug     = false;   // Ctrl+Alt+Shift+A: also save what the finder saw
        // Per-region window bookkeeping: the windows above each region's
        // host (cut out of the weave -- never weave over a window that's in
        // front), and when to try re-growing an image a window was covering.
        std::map<int, std::vector<RECT>> regionOcc;     // region id -> screen rects in front of it
        std::map<int, uint64_t>          regionOccSig;  // region id -> layout signature
        std::map<int, long long>         regionOccArea; // region id -> covered area (px)
        std::map<int, DWORD>             regrowAt;      // content region id -> when to try
        // A picture found while a window covered part of its viewport stays cut
        // until a re-grow finds the rest: the covered area when it was found (or
        // last checked), and when the covering last shrank below that -- while
        // less is covered, re-grows are retried until one, run on a frame from
        // after the change, has had a look.
        std::map<int, long long>         regionFoundOcc;
        std::map<int, DWORD>             regionUncoveredAt;
        // Content regions found cut off by their viewport's edge (a picture half
        // scrolled into view): which edges (1 top, 2 bottom, 4 left, 8 right)
        // and how far in they'd moved at the last re-grow try (analysis px).
        struct Truncation { int edges = 0; int lastGap = 0; };
        std::map<int, Truncation>        regionTrunc;
        std::map<int, int>               reacqMisses;   // re-acquire results rejected in a row
        std::map<int, DWORD>             reacqFirstMiss; // ... when the first of them was (only dropped after a while)
        std::map<int, DWORD>             scrolledAt;    // content region id -> when it last moved (tick)
        std::map<int, DWORD>             regrowTriedAt; // ... and when a re-grow was last tried
        std::map<int, POINT>             regionViewPos; // content region id -> its viewport's top-left at the last track (analysis px)
        // Background image-rectangle searches per content region (see
        // LaunchAsyncFind / PollAsyncFinds).
        struct AsyncFind
        {
            bool regrow = false;
            RECT old{};
            std::shared_ptr<LumaImage> img;
            std::future<std::pair<bool, RECT>> fut;
            DWORD startedAt = 0;   // (GetTickCount)
        };
        std::map<int, AsyncFind> asyncFinds;
        std::map<int, std::deque<std::pair<int64_t, RECT>>> regionAnchorHist;   // region id -> (QPC 100 ns, anchor screen rect)
        // Auto-detect (the panel's "Auto Stereo" button): a background
        // scanner looks for stereo images on the SR display every
        // kScanIntervalMs and weaves them itself. A find is only woven when
        // the next scan agrees (same place, same format); woven auto regions
        // are re-checked each scan and dropped after 3 misses. Everything
        // stays in memory; nothing is saved.
        bool                         autoDetect     = false;
        // Stereo 3D Input = "Automatic Detection" (see UpdateInputChoice):
        // Auto Stereo on this display in Fullscreen, a whole-picture layout
        // check on any other source. Starts as the pinned default.
        bool                         autoInput      = true;
        int                          defaultInput   = 0;       // the pinned default (Settings::ReadDefaultInput)
        size_t                       inputCtxKey    = 0;       // source/mode it was last applied to
        DWORD                        wholeDetectUntil = 0;     // whole-picture check: keep trying until (tick)
        DWORD                        wholeDetectNext  = 0;     // ... next attempt (tick)
        // Automatic Detection + a fullscreen window on the SR display (see
        // UpdateFullscreenAuto): when its whole picture is 3D, the display is
        // woven in that layout until it leaves fullscreen.
        HWND                         fsAutoWindow    = nullptr;  // fullscreen window woven whole right now
        // Automatic Detection + a chosen Window: Auto Stereo, but only the 3D
        // pictures inside this (top-level) window.
        HWND                         autoScopeWindow = nullptr;
        bool                         analysisDeferred = false;   // Auto Stereo's CPU analysis runs after this frame's present
        bool                         overlayRgnValid = false;    // the window-overlay's rounded-corner shape is set (UpdateOverlayTracking)
        bool                         eyeOrderDetect = false;     // Settings::ReadEyeOrderDetect (polled)
        bool                         autoEyeSwap = false;        // whole-display weave (fullscreen 3D): detected with the eyes swapped
        // A layout picked by hand (or found for a whole picture): its eye order,
        // checked every 1.5 s in the background (UpdateManualEyeOrder).
        bool                         manualEyeSwap = false;
        int                          manualEyeVotes = 0;          // consecutive verdicts disagreeing with manualEyeSwap
        int                          manualEyePhase = 0;          // 0 idle, 1 frame submitted, 2 judging
        DWORD                        manualEyeNext = 0;
        size_t                       manualEyeKey = 0;
        std::future<EyeOrder>        manualEyeJob;
        // Anaglyph picked by hand (or a whole fullscreen one), Recovered Colour:
        // what was the picture under it? Black-and-white: decoded as Mono;
        // one colour (sepia, a duotone): with its tint (mode 5); colour:
        // recovered. Checked every 1.5 s in the background (UpdateManualAnaColour).
        // One picture box's verdict (source uv; kind 1 black-and-white, 2 one colour).
        struct AnaBoxVerdict { float u0, v0, u1, v1; int kind; std::shared_ptr<const AnaTint> tint; DWORD seen = 0;
                               float insetU = 0, insetV = 0; };   // (past the padding: where the picture itself starts)
        // kind: the whole frame's (when no box was found); boxes: each picture's.
        struct AnaVerdict { int kind = -1; std::shared_ptr<const AnaTint> tint; int boxes = 0, grey = 0, tinted = 0, colour = 0;
                            std::vector<AnaBoxVerdict> special; std::vector<AnaBoxVerdict> coloured;
                            std::vector<std::string> diag; };   // (per box, for the log)
        int                          manualAnaKind = 0;           // 0 colour, 1 black-and-white, 2 one colour
        std::shared_ptr<const AnaTint> manualAnaTint;             // (kind 2)
        std::vector<AnaBoxVerdict>   manualAnaBoxes;              // the black-and-white / one-colour pictures on a page of several
        int                          manualAnaVotes = 0;          // consecutive verdicts disagreeing with manualAnaKind
        int                          manualAnaPhase = 0;          // 0 idle, 3 frame wanted (RenderFrame hands it over), 1 frame submitted, 2 judging
        bool                         manualAnaSubmitWanted = false;
        DWORD                        manualAnaNext = 0;
        size_t                       manualAnaKey = 0;
        DWORD                        manualAnaKeyAt = 0;          // when this picture was first seen
        bool                         manualAnaLogged = false;     // this picture's first verdict is in the log
        bool                         manualAnaStale = false;      // the running job is for an earlier picture
        std::future<AnaVerdict>      manualAnaJob;
        RECT                         autoGlass{};                // Looking Glass + Auto Stereo: glass last woven (frame px)
        bool                         autoInputCropped = false;   // ... weave input is the glass crop (vs the whole composite)
        HWND                         fsCheckedWindow = nullptr;  // fullscreen window being / already checked
        DWORD                        fsCheckUntil = 0, fsNextCheck = 0, fsNextPoll = 0;
        struct FsVerdict { HWND window = nullptr; bool is3D = false; StereoFormat fmt{}; AnaglyphKind ana; EyeOrder eye; std::string diag; };
        std::future<FsVerdict>       fsJob;                      // the whole-picture check, off the render thread
        StereoScanner                scanner;
        DWORD                        lastScanMs     = 0;
        bool                         scanWantColour = false;   // next analysed frame carries colour
        bool                         screenChangedSinceScan = true;
        std::vector<ScanHit>         scanPending;              // found once, awaiting confirmation
        std::map<int, int>           autoMisses;               // auto region id -> scans not seen as 3D
        std::map<int, RegionTracker> suppressed;               // auto finds the user removed
        std::vector<HWND>            suppressedWindows;        // whole-window finds the user removed
        int                          nextSuppressId = 1;
        size_t                       lastScanLogKey = 0;       // log a scan only when its result changes
        bool                         analysisWaitNewest = false; // this frame: wait for its own analysis
        double                       autoTimeAnalysisMs = 0;   // frame profile (below)
        double                       autoTimeReadbackMs = 0;   // ... of which: reading the analysis image back from the GPU
        bool                         paceOnCapture = false;    // this loop is paced by capture frames, not DwmFlush
        bool                         deferHeavyConvert = false;// Recovered Colour converted after the present, woven next loop (Settings DeferRecovered)
        bool                         hdrActive = false;        // the 16-bit float chain is in use (StartSRSession)
        bool                         hdrDisplay = false;       // Windows has HDR on for the SR display (as last looked)
        bool                         weaverLatencyAuto = false; // (Settings WeaverLatency 1)
        double                       latencyEma = 0.0;
        int                          actApplied = 0;           // (the anti-crosstalk setting last given to the weaver)
        bool                         actTouched = false;       // (it has been changed from the display's default this run)
        bool                         lfOn = false, lfSlantSet = false;   // light field (Settings LightField...)
        float                        lfPitch = 0.0f, lfSlant = 0.0f, lfOffset = 0.0f, lfOffsetNow = 0.0f;
        int                          lfFollow = 0;             // (Settings LfFollow: the viewing distance taken from the camera)
        bool                         lfCentre = false;         // (Settings LfCentre: the views kept aimed at the tracked viewer)
        float                        lfFanCm = 11.6f;          // (how wide the lens' fan of views is where they are aimed)
        float                        rgbdStrength = 50.0f, rgbdFocus = 50.0f, rgbdLookX = 0.0f, rgbdLookY = 0.0f;   // RGB + depth (Settings Rgbd*)
        int                          rgbdFlags = 4;            // (1 depth on the left, 2 black near, 4 the side found automatically)
        bool                         rgbdLook = true;
        float                        lfSpread = 0.3f;          // (Settings LfSpread: the part of the Quilt's views the fan shows)
        bool                         lfGeoValid = false;       // (the light field's lens geometry, from the SDK, for the place below)
        float                        lfGeoPitch = 0.0f, lfGeoSlant = 0.0f, lfGeoPhase = 0.0f, lfGeoX = 0.0f, lfGeoY = 0.0f, lfGeoZ = 0.0f;
        UINT                         lfGeoW = 0;
        float                        lfDistanceCm = 60.0f;     // (Settings LfDistance: where the fans are aimed)
        bool                         lfPattern = false;        // (Settings LfPattern: the alignment pattern)
        bool                         asyncConvert = true;      // DX12: Recovered Colour converted apart from the weave (Settings AsyncConvert)
        bool                         asyncMode = false;        // (the weave reads the presenter's own picture: conversion apart from the weave)
        double                       asyncWaitMs = 1.2;        // (how long the weave waits for a conversion under way: Settings AsyncWaitUs)
        int                          asyncBands = 4;           // (the conversion sent to the GPU in pieces: Settings AsyncBands)
        bool                         asyncSlow = false;        // (conversions are slow at the moment: apart from the weave)
        double                       asyncEnterMs = 3.5;       // (a conversion this long is slow: Settings AsyncEnterUs)
        int                          asyncSlowRun = 0;         // (slow conversions in a row)
        unsigned                     asyncSeenSerial = 0;
        ULONGLONG                    asyncSlowAt = 0;          // (when one last took over 2.5 ms)
        ID3D11ShaderResourceView*    deferSrc = nullptr;       // ... this loop's source, until then
        int                          deferW = 0, deferH = 0;
        // GPU scroll tracking (DirectComposition presenter only): content
        // pictures are positioned on the GPU each frame; see GpuTrack.h.
        GpuTracker                   gpuTracker;
        bool                         gpuTracking = false;
        // Frame profile, logged every 5 s while weaving: where each loop's
        // time goes (for tuning latency / dropped frames), in every mode.
        struct FrameProfile
        {
            std::chrono::steady_clock::time_point t0{}, tWait{};
            double wait = 0, work = 0, weave = 0, present = 0, total = 0, analysis = 0, readback = 0, worst = 0;
            double gui = 0, outside = 0;                     // panel drawing; all time outside the weave
            double compWait = 0, vblankWait = 0;             // pace-wait's parts: the compositor taking the last frame, the vertical blank
            int    late = 0;                                  // loops longer than 1.5 refreshes (a refresh shown twice: judder)
            int    lateComp = 0, lateBlank = 0, lateWork = 0, lateOther = 0, lateCapture = 0, lateCap = 0, lateOutside = 0, latePresent = 0;   // ... what took the time in each
            double lastCaptureWaitMs = 0;                     // this loop's wait for a new capture frame
            std::chrono::steady_clock::time_point prevPresent{};  // the last present (for late frames)
            std::chrono::steady_clock::time_point lastEnd{}; // end of the previous weave
            int    loops = 0, frames = 0;
            DWORD  last = 0;
        } prof;
        GpuFrameTimer                gpuTimer;   // GPU side of the frame profile
        int                          lateLatchingApplied = -1;   // SR weaver late latching as set (-1 unknown)
        bool                         diagSkipWeave = false;      // diagnostics: no SR weave call
        bool                         perfLog = false;            // frame / GPU timing lines (Settings::ReadPerfLog)
        Detector     detector;
        Gui          gui;
        VideoSource  video;          // active video file source (mp4/mov/etc), if any
        KatangaSource katanga;       // shared-texture receiver for Bo3b Katanga (Geo-11 etc.)
        LFPRenderer  lfpRenderer;    // per-frame plenoptic SBS render (head-tracked aperture)
        OpenTrackBridge openTrack;   // SR head-pose -> OneEuro -> OpenTrack UDP. Off by default.
        std::string  openTrackExePath; // detected opentrack.exe path (empty if not installed)
        // NaturalPoint registry says where NPClient.dll/NPClient64.dll live.
        // When set + both DLLs exist, OpenTrack's NPClient stack is usable
        // and SR Loom enables TrackIR output by writing the same
        // FT_SharedMem the DLL reads from. Empty when not detected; GUI
        // greys the TrackIR checkbox + shows "(Please install OpenTrack)".
        std::string  npClientDir;
        // Auto-enable policy: head-tracking outputs come on at launch (unless
        // "Head Tracking On Startup" is off) and are opportunistically re-enabled
        // on weave-start if the launch attempt failed. They stay on across
        // weave-stop. openTrackUserDisabled records "the user turned all outputs
        // off" (GUI / tray toggles, or startup-off) and suppresses that
        // re-enable; it clears only when the user turns an output back on.
        bool         openTrackUserDisabled = false;
        // Per-game profile auto-apply (NTM-style). The list is loaded from
        // %LOCALAPPDATA%\SRLoom\profiles.ini at startup and re-saved on edit.
        // m_lastProfile is the last name applied -- used to debounce so we
        // don't re-apply on every WM_APP_FOREGROUND_CHANGED.
        std::vector<Profile>  profiles;
        bool                  profilesAutoApply = true;
        std::string           lastAppliedProfile;
        // HWND-based debounce: re-applying the SAME running window is a
        // no-op, but a fresh HWND (e.g. closing and re-launching the game)
        // triggers the apply path even though the profile name matches
        // what we just applied. lastAppliedProfile alone wasn't enough --
        // user closed+relaunched a game, the new HWND matched the saved
        // profile, but the name-debounce blocked the apply.
        HWND                  lastAppliedHwnd = nullptr;
        // Profile name + window title of the last AUTO apply (foreground
        // hook / poll). Separate from lastAppliedProfile, which the manual
        // tray/GUI apply also sets: a manual pick must not make the poll
        // think "different profile" and re-apply over it. The title lets
        // format=auto profiles re-detect when the same window's title
        // changes (VLC moving to the next playlist file).
        std::string           lastAutoAppliedProfile;
        std::string           lastAutoAppliedTitle;
        // Currently-tracked "fullscreen-condition profile" HWND. Set by
        // ApplyProfile when it applies a fullscreenOnly profile to a window
        // (and cleared when it applies anything else). The periodic poll
        // watches this HWND; when it stops being fullscreen (or is
        // destroyed) while we're still weaving it, we auto-disable the
        // weave. Cleared on disable.
        HWND                  activeFullscreenProfileHwnd = nullptr;
        // Taskbar cut-out currently applied to the weave window's region (in
        // screen coords), so the 250ms poll only calls SetWindowRgn when the
        // taskbar actually shows / hides / moves. See UpdateTaskbarCutout.
        // Source-switch hand-off (weave-a-window <-> fullscreen etc.). A new
        // capture session takes a few frames to deliver its first frame.
        // Restyling/resizing the weave window straight away meant those
        // frames showed the OLD image in the NEW geometry (the flicker).
        // While captureWarmup is set, ApplyMode is deferred (the window
        // keeps its old shape, showing the last good frame); the first new
        // frame -- or a 250ms timeout -- applies the mode and renders the new
        // source in one step.
        bool                  captureWarmup = false;
        DWORD                 captureWarmupStartMs = 0;
        bool                  modeApplyDeferred = false;
        // Holes currently cut out of the weave window's region (screen coords):
        // the taskbar plus any system-UI window above the weave (Start, toasts,
        // Game Bar, Alt+Tab...). taskbarCutActive = any cut-out is applied.
        bool                  taskbarCutActive = false;
        std::vector<RECT>     weaveCuts;
        std::vector<int>      weaveCutRad;          // corner radius per hole (rounded windows)
        RECT                  taskbarCutWindow{};   // our window rect the cuts were made for
        // System-UI rects recently seen above the weave, with the time last
        // seen. A hole lingers ~200ms after its window goes: the capture runs
        // a frame or two behind, so closing the hole instantly would weave a
        // ghost of e.g. the Start menu's last position (the "trail").
        struct LingerCut { RECT r; DWORD t; int rad; };
        std::vector<LingerCut> recentUiCuts;
        // 2D windows / windows in front of Auto Stereo pictures: where they were
        // recently (QPC 100 ns timestamps), to cut them where the displayed
        // capture still shows them (see UpdateTaskbarCutout).
        // (owner: the region id a cut belongs to -- a window in front of that
        // picture only; none / -1: a cut in everything.)
        struct TimedCuts { int64_t t = 0; std::vector<RECT> r; std::vector<int> rad; std::vector<int> owner; };
        std::map<HWND, std::deque<TimedCuts>> keepHistory;
        std::deque<TimedCuts>  occHistory;
        DWORD                 lastFullscreenPollMs = 0;
        // WinEventHook handle for EVENT_SYSTEM_FOREGROUND. Posts back to
        // the main window via WM_APP_FOREGROUND_CHANGED with the new HWND
        // in lParam; the handler walks profiles, matches, applies.
        HWINEVENTHOOK         fgHook = nullptr;
        // Last foreground window that was NOT one of our own. Updated by
        // the foreground hook every time the user focuses an external app.
        // Used by "Save current as profile" because at click-time the
        // GUI panel itself is the foreground -- without this cache we'd
        // capture SR Loom's own exe instead of the game the user just
        // alt-tabbed away from.
        HWND                  lastExternalForeground = nullptr;
        // Currently-loaded image / video file path (empty when source is
        // not media). Set by LoadTestImage; cleared on source change away.
        // Used by the media-profile system to (a) look up + apply remembered
        // settings when a file is opened and (b) auto-save settings back to
        // the profile when the user tweaks format / swap / convergence /
        // anaglyph while the file is the live source. Kept in its own
        // INI (media_profiles.ini) separate from the game profiles list
        // so opened-photo entries don't pollute the GUI profiles dropdown.
        std::string           currentMediaPath;
        // Last-saved hash of the stereo state for the current media file
        // (format + swap + convergence + anaglyph + pulfrich + frame pack).
        // The render loop compares the live hash each tick: change ->
        // schedule a save, throttled by lastMediaSaveMs so dragging the
        // convergence slider doesn't hammer the INI file.
        uint64_t              lastMediaStateHash = 0;
        DWORD                 lastMediaSaveMs    = 0;
                                     // Bound when format==Katanga; the render loop
                                     // arms/engages based on receiver state. Game-exit
                                     // detection lives in KatangaSource itself (it
                                     // re-opens the named mapping each poll, so when
                                     // the publishing process dies the kernel object
                                     // is reclaimed and the next open fails).
        // Main window of the Katanga-publishing game, discovered via
        // NT-API handle-table lookup at reception start. Used to pin SR
        // Loom's overlay specifically ABOVE the game in Z each frame, so
        // games that self-set HWND_TOPMOST on activation can't pop above
        // our weave. Null if discovery failed or no game is publishing.
        HWND       katangaPublisherWnd = nullptr;
        unsigned   katangaGeneration   = 0;   // last KatangaSource::Generation() bound to the weaver
        // Katanga auto-receive (Settings::ReadKatangaAutoReceive). While
        // katangaAuto is set, SR Loom switched itself into Katanga because a
        // sender appeared; the saved state is restored when it stops.
        bool         katangaAuto            = false;
        DWORD        katangaAutoStartMs     = 0;
        DWORD        katangaAutoLastPollMs  = 0;
        bool         katangaAutoBlocked     = false;   // see PollKatangaAutoReceive
        StereoFormat katangaPrevFormat      = StereoFormat::HalfSBS;
        OutputMode   katangaPrevMode        = OutputMode::Fullscreen;
        bool         katangaPrevWeaving     = false;
        RECT         katangaPlacedRect{};     // where the Katanga weave was last placed

        // VR180 / VR360 viewer state (used by the VR converter shader path).
        // yaw / pitch in RADIANS; zoom in [0.2 .. 3.0] (1 = ~90° horizontal
        // FOV, higher = zoomed in). headLook = head-position drives a small
        // additional yaw shift so leaning to the side reveals a bit more of
        // the panorama. vrDrag tracks an in-progress mouse drag-to-look.
        // vrMouseDown / vrMoved distinguish click from drag: on LBUTTONUP
        // without enough movement, fall through to the normal click handler
        // so the user can still toggle the test-image overlays.
        float        vrYaw          = 0.0f;
        float        vrPitch        = 0.0f;
        float        vrZoom         = 1.0f;
        // Default OFF for v1.6 -- the SR head-tracker step rate (~60Hz
        // raw poses) shows up as visible per-frame jitter at the 165Hz
        // render rate that no amount of LP/Accela filtering can fully
        // hide without adding lag. Mouse-drag look-around works great;
        // users can opt in to head-look via the GUI toggle and accept
        // the trade. v1.7 plan: replace GetHeadPose() with a late-
        // latched predicted pose from the SDK.
        bool         vrHeadLook     = false;
        bool         vrMouseDown    = false;
        bool         vrMoved        = false;
        int          vrDragStartX   = 0;
        int          vrDragStartY   = 0;
        int          vrDragLastX    = 0;
        int          vrDragLastY    = 0;
        // Two-stage VR head-tracking filter, one per axis:
        //   1. OneEuro adaptive low-pass smooths the SR-tracker stream.
        //      Run at a fixed 60 Hz constructor freq -- we don't pass a
        //      timestamp at call time because GetTickCount has 15 ms
        //      granularity on Windows, which makes OneEuro's auto-freq
        //      estimate (1/dt) wildly unstable at the 165 Hz render
        //      rate (some frames see dt=0, others dt=15ms).
        //   2. Accela velocity-gain on top with deadzone=0 -- soft
        //      damping of tiny residual inputs via the gain curve
        //      (near-zero gain at small normalised values), no hard
        //      threshold that would cause "still still SNAP" stair-
        //      stepping. Real head turns hit the steep part of the
        //      curve and snap instantly.
        OneEuroFilter vrYawOneEuro    { 60.0f, 1.0f, 0.3f };
        OneEuroFilter vrPitchOneEuro  { 60.0f, 1.0f, 0.3f };
        struct AccelaAxis { double lastOutput = 0.0; DWORD lastTickMs = 0; bool init = false; };
        AccelaAxis    vrYawAccela;
        AccelaAxis    vrPitchAccela;
        bool          vrFilterInit    = false;
        float        convergence    = 0.0f;   // GUI convergence slider (-1..1)
        // Light-field parallax-scale slider value (in mm of head-lean
        // needed to drive the aperture sample to its edge). Lower =
        // more sensitive, higher = closer to the physical camera baseline.
        // Default 30 mm matches the v2 launch behaviour; minimum tracks
        // the actual aperture radius (1:1 physical) and maximum lets the
        // user crank parallax further (eg 10 mm head lean for huge effect).
        float        lfpHeadLeanMm  = 30.0f;
        bool         weavingEnabled = true;
        OutputMode   mode           = OutputMode::Fullscreen;
        SourceKind   source         = SourceKind::CaptureMonitor;  // default: weave the screen (fullscreen SBS)
        StereoFormat format         = StereoFormat::HalfSBS;  // default: most on-screen SBS content is half-width
        bool         swapEyes       = false;
        int          anaglyphCombo  = 0;   // 0..5 colour combination, 6 Custom (kAnaComboCustom: the colours below)
        float        anaCustomL[3]  = { 1, 0, 0 };   // the Custom pair's left filter colour (Settings AnaCustomLeft)
        float        anaCustomR[3]  = { 0, 1, 1 };   // ... the right's
        float        anaSaved[8][6] = {};             // saved Custom pairs (Settings AnaSaved*)
        int          anaSavedCount  = 0;
        // The Custom pair's eyedropper: 1 / 2 picking the left / right colour from the
        // picture (the weave window takes the next click: EyedropStart / EyedropEnd).
        int          eyedrop        = 0;
        bool         eyedropClicked = false;
        POINT        eyedropPt      = {};             // (the click, weave-window client px)
        LONG_PTR     eyedropOldEx   = 0;
        ULONGLONG    eyedropSince   = 0;
        int          anaglyphMode   = 4;   // shader mode value (4 = Recovered colour, the default)
        PulfrichMode pulfrichMode   = PulfrichMode::TimeDelay;
        int          pulfrichDelay  = 1;   // delay frames (time-delay mode)
        int          pulfrichNd     = 1;   // ND level index (default Medium)
        int          framePackMode  = 0;   // FramePackPresets index (0 = 1080p)
        // Quilt source (Looking Glass): cols x rows grid; view indices pick the L/R pair.
        // Defaults are the centre pair of an 8x6 quilt; auto-set on file load from a
        // "_qsCxR_" filename token, falling back to these.
        int          quiltCols      = 8;
        int          quiltRows      = 6;
        int          quiltLeftIdx   = 23;  // centre - 1 (0-based) for 8x6 = 48 views
        int          quiltRightIdx  = 24;  // centre
        // Head-tracking smoothing state for Quilt view selection. EMA on the
        // raw head.x position takes the high-frequency tracker noise out before
        // it ever drives the view choice -- so a still head stays put. View
        // BLENDING (below) replaces the old hysteresis-snap: instead of locking
        // to one integer view, we cross-fade between the two nearest views in
        // the shader using the fractional position, which is what Looking Glass
        // does between physical lenticular columns -- no jagged step.
        double       headXEMA          = 0.0;   // legacy head-centre EMA (fallback)
        bool         headXEMAInit      = false;
        double       leftEyeXEMA       = 0.0;   // smoothed left-eye-X from getPredictedEyePositions
        double       rightEyeXEMA      = 0.0;
        bool         eyesEMAInit       = false;
        float        quiltLeftBlend    = 0.0f;   // L pane: blend between quiltLeftIdx and the next view
        float        quiltRightBlend   = 0.0f;   // R pane: blend between quiltRightIdx and the next view
        double       srMmPerPx         = 0.0;    // SR display physical-to-pixel scale (mm/px), 0 = unknown
        std::string  lastTestImagePath;          // last image fed to SetStereoImageFromFile
        HWND       sourceWindow   = nullptr; // tracked window in WindowOverlay mode
        HWND       lastForeground = nullptr; // last real foreground window (for "make active window 3D")
        bool       loupeInteractive = false; // looking glass: currently grabbable (not click-through)
        bool       loupeDragging  = false;   // looking glass: in a move/resize loop
        struct { bool active = false; int hit = 0; POINT start{}; RECT startRect{}; } loupeDrag;   // our own move/resize
        bool       loupeActive    = false;   // looking glass shown (keep its position across re-applies)
        // Last Looking Glass window rect, saved whenever we leave it (e.g. to
        // Fullscreen) so coming back restores the exact position + size.
        RECT       loupeSavedRect{};
        bool       loupeHasSaved  = false;
        bool       captureRebind  = false;   // re-register SRV on next frame
        // Weaving off: when to let the SR session go (0 = nothing pending). It is
        // kept that long so switching weaving straight back on is instant.
        ULONGLONG  srStopAtMs     = 0;
        int        weaverChoiceSeen = -1;   // (Settings WeaverChoice last acted on; -1 not read yet)
        ULONGLONG  displayChangedAtMs = 0;  // (WM_DISPLAYCHANGE: when; HandleDisplayChange once it settles)
        double     paceHz = 0.0;            // (the refresh rate the renderer paces for: the SR display's current one)
        RECT       srDisplayRect  = { 0, 0, 1920, 1080 };  // filled from SR SDK
        HMONITOR   sourceMonitor  = nullptr; // monitor being captured (passthrough / display picker)
        bool       foreignDisplay = false;   // capturing a NON-SR display: weave the whole frame (no crop)
        // Exclusive-fullscreen fallback state. WGC stops delivering frames when the
        // captured monitor hosts a true exclusive-fullscreen app; we switch to DXGI
        // Output Duplication for those, then back to WGC when fullscreen ends. Only
        // engaged for foreign-display monitor capture (DXGI on the SR display itself
        // would capture our own overlay -> feedback loop).
        bool       dxgiActive     = false;
        int        wgcStuck       = 0;     // consecutive iterations with no new WGC frame
        int        dxgiCheckCtr   = 0;     // polls the foreground-fullscreen state every ~30 ticks

        // Update checker: URL of the latest GitHub release stashed when the
        // worker thread finds one newer than kAppVersion. Used to open the
        // release page when the user clicks the toast balloon.
        std::string pendingUpdateUrl;
        std::string pendingUpdateTag;
    };

    AppState* g_app = nullptr;

    // Determine where to place the output window: the SR display if known,
    // otherwise the primary monitor.
    RECT ResolveTargetRect(SRWeaver& weaver)
    {
        RECT rc{};
        if (weaver.GetSRDisplayRect(rc))
            return rc;

        rc.left   = 0;
        rc.top    = 0;
        rc.right  = GetSystemMetrics(SM_CXSCREEN);
        rc.bottom = GetSystemMetrics(SM_CYSCREEN);
        return rc;
    }

    // Heuristic: is the current foreground window covering the entirety of `target`?
    // Used to decide whether to keep the DXGI Output Duplication fallback engaged.
    // Catches both true exclusive fullscreen and borderless-fullscreen; we don't
    // actually need to distinguish them -- DXGI captures both equally well, and the
    // signal that we should swap BACK to WGC is "the foreground stopped being
    // fullscreen-sized on this monitor".
    bool ForegroundCoversMonitor(HMONITOR target)
    {
        if (!target) return false;
        HWND fg = GetForegroundWindow();
        if (!fg) return false;
        if (MonitorFromWindow(fg, MONITOR_DEFAULTTONULL) != target) return false;
        MONITORINFO mi{}; mi.cbSize = sizeof(mi);
        if (!GetMonitorInfo(target, &mi)) return false;
        RECT wr{};
        if (!GetWindowRect(fg, &wr)) return false;
        const LONG tol = 2;   // a few px of slop for borderless windows that don't quite hit the edge
        return wr.left  <= mi.rcMonitor.left  + tol &&
               wr.top   <= mi.rcMonitor.top   + tol &&
               wr.right >= mi.rcMonitor.right - tol &&
               wr.bottom >= mi.rcMonitor.bottom - tol;
    }

    // Apply fullscreen (borderless on the SR display) or windowed styling.
    // The VISIBLE window rectangle. GetWindowRect includes ~7px of invisible
    // resize border on Win10/11, which made the in-place overlay sit slightly
    // larger than the window; the DWM extended frame bounds exclude it.
    RECT VisibleWindowRect(HWND hwnd)
    {
        RECT r{};
        if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
            GetWindowRect(hwnd, &r);
        return r;
    }

    // Click-through (layered+transparent) is wanted whenever the weave overlays
    // live content the user interacts with beneath it.
    bool WantsClickThrough(const AppState& app)
    {
        switch (app.mode)
        {
        case OutputMode::WindowOverlay: return true;
        case OutputMode::LookingGlass:  return !app.loupeInteractive;
        case OutputMode::Fullscreen:
            // Layered fullscreen for passthrough AND for Katanga: both put SR
            // Loom over the entire SR display, both want the bit-blt swap
            // chain. Switching between them then never recreates the swap
            // chain -- which is critical because swap-chain recreation
            // leaves the LeiaSR weaver in a state where its output never
            // reaches the panel (root cause of the post-Katanga black bug).
            return app.source == SourceKind::CaptureMonitor
                || app.format == StereoFormat::Katanga;
        default:                        return false;
        }
    }

    // ----- Fullscreen-controls overlay --------------------------------------
    // A tiny topmost layered popup with three custom-drawn buttons (minimise /
    // switch to windowed / close) that appears in the top-right of the SR
    // display whenever the user clicks inside the fullscreen weave. Auto-hides
    // after a few seconds of inactivity. Painted with GDI on its own thread of
    // messages so it doesn't interfere with the weave path.
    // Warm-dark palette matched to Gui.cpp's "Warm Dark" theme so the floating
    // overlays sit visually with the rest of the app (Reeder-ish: warm tones,
    // low-contrast surfaces, rounded corners). Shared by both overlays.
    constexpr COLORREF kFsBg       = RGB(28, 27, 25);   // window bg
    constexpr COLORREF kFsBtnBg    = RGB(40, 38, 34);   // button surface
    constexpr COLORREF kFsBtnHover = RGB(60, 56, 50);   // button hover
    constexpr COLORREF kFsCloseHot = RGB(210, 105, 74); // warm accent (close hover)
    constexpr COLORREF kFsText     = RGB(232, 228, 220);
    constexpr COLORREF kFsDim      = RGB(160, 152, 140);

    constexpr char  kFsCtrlClass[] = "SRLoomFsControls";
    constexpr int   kFsBtnCount    = 3;          // [0] minimise, [1] switch to Windowed, [2] close
    constexpr int   kFsBtnW        = 64;
    constexpr int   kFsBtnH        = 52;
    constexpr int   kFsCtrlPad     = 10;
    constexpr int   kFsCtrlGap     = 8;
    constexpr int   kFsCtrlRadius  = 10;
    constexpr int   kFsCtrlW       = kFsBtnW * kFsBtnCount + kFsCtrlGap + kFsCtrlPad * 2;
    constexpr int   kFsCtrlH       = kFsBtnH + kFsCtrlPad * 2;
    constexpr UINT  kFsCtrlTimerId = 7;
    constexpr DWORD kFsCtrlHideMs  = 3500;

    // Rounded-rect button paint. No border; hover differentiates by fill.
    void FsPaintRoundButton(HDC dc, const RECT& r, COLORREF fill, int radius)
    {
        HBRUSH fb = CreateSolidBrush(fill);
        HBRUSH oldB = (HBRUSH)SelectObject(dc, fb);
        HPEN   pen = CreatePen(PS_NULL, 0, 0);
        HPEN   oldP = (HPEN)SelectObject(dc, pen);
        RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, radius, radius);
        SelectObject(dc, oldB); DeleteObject(fb);
        SelectObject(dc, oldP); DeleteObject(pen);
    }

    HWND  g_fsCtrl          = nullptr;
    int   g_fsCtrlHover     = -1;
    DWORD g_fsCtrlLastUse   = 0;
    bool  g_fsCtrlTracking  = false;

    RECT FsBtnRect(int i)
    {
        RECT r{};
        r.left   = kFsCtrlPad + i * (kFsBtnW + kFsCtrlGap);
        r.top    = kFsCtrlPad;
        r.right  = r.left + kFsBtnW;
        r.bottom = r.top  + kFsBtnH;
        return r;
    }
    int FsHit(int x, int y)
    {
        for (int i = 0; i < kFsBtnCount; ++i)
        {
            RECT r = FsBtnRect(i);
            if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i;
        }
        return -1;
    }

    // Draw the standard Windows title-bar glyphs (minimise, close) using GDI
    // primitives instead of font codepoints — the codepoints depend on which
    // version of Segoe Fluent / MDL2 Assets is installed and aren't reliable.
    // Lines / rects render crisply and look identical to native chrome.
    void DrawFsGlyph(HDC dc, int which, const RECT& r, COLORREF colour)
    {
        const int cx = (r.left + r.right)  / 2;
        const int cy = (r.top  + r.bottom) / 2;
        const int s  = 9;     // half-icon size (~18x18 footprint, large + chunky)
        HPEN pen     = CreatePen(PS_SOLID, 2, colour);   // thicker stroke for visibility
        HPEN oldPen  = (HPEN)SelectObject(dc, pen);
        switch (which)
        {
        case 0:   // minimise: horizontal bar, centred
            MoveToEx(dc, cx - s, cy, nullptr); LineTo(dc, cx + s + 1, cy);
            break;
        case 1:   // restore / switch to windowed: empty rectangle outline
        {
            HBRUSH hollow = (HBRUSH)GetStockObject(NULL_BRUSH);
            HBRUSH oldB   = (HBRUSH)SelectObject(dc, hollow);
            Rectangle(dc, cx - s, cy - s, cx + s + 1, cy + s + 1);
            SelectObject(dc, oldB);
            break;
        }
        case 2:   // close: two diagonal lines forming X
            MoveToEx(dc, cx - s, cy - s, nullptr); LineTo(dc, cx + s + 1, cy + s + 1);
            MoveToEx(dc, cx + s, cy - s, nullptr); LineTo(dc, cx - s - 1, cy + s + 1);
            break;
        }
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }

    LRESULT CALLBACK FsCtrlProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        AppLock appLock;   // (see g_appLock)
        switch (msg)
        {
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT cr; GetClientRect(hwnd, &cr);
            HBRUSH bg = CreateSolidBrush(kFsBg);
            FillRect(dc, &cr, bg);
            DeleteObject(bg);
            for (int i = 0; i < kFsBtnCount; ++i)
            {
                RECT r = FsBtnRect(i);
                COLORREF fill = kFsBtnBg;
                if (i == g_fsCtrlHover)
                    fill = (i == 2) ? kFsCloseHot : kFsBtnHover;   // close is index 2 now
                FsPaintRoundButton(dc, r, fill, kFsCtrlRadius);
                DrawFsGlyph(dc, i, r, kFsText);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE:
        {
            if (!g_fsCtrlTracking)
            {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                g_fsCtrlTracking = true;
            }
            const int hit = FsHit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (hit != g_fsCtrlHover) { g_fsCtrlHover = hit; InvalidateRect(hwnd, nullptr, FALSE); }
            g_fsCtrlLastUse = GetTickCount();
            return 0;
        }
        case WM_MOUSELEAVE:
            g_fsCtrlTracking = false;
            if (g_fsCtrlHover != -1) { g_fsCtrlHover = -1; InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;
        case WM_LBUTTONDOWN:
        {
            const int hit = FsHit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            g_fsCtrlLastUse = GetTickCount();
            if (hit >= 0 && g_app && g_app->hwnd)
                PostMessageA(g_app->hwnd, WM_APP_FS_BUTTON, (WPARAM)hit, 0);
            return 0;
        }
        case WM_TIMER:
            if (wp == kFsCtrlTimerId)
            {
                // Auto-hide when the cursor has been away from the controls for
                // a few seconds; keep visible while it's hovering a button.
                if (g_fsCtrlHover == -1 && (GetTickCount() - g_fsCtrlLastUse) > kFsCtrlHideMs)
                    ShowWindow(hwnd, SW_HIDE);
            }
            return 0;
        case WM_NCHITTEST: return HTCLIENT;
        }
        return DefWindowProcA(hwnd, msg, wp, lp);
    }

    void EnsureFsCtrlWindow(HINSTANCE inst)
    {
        if (g_fsCtrl) return;
        WNDCLASSA wc{};
        wc.lpfnWndProc   = FsCtrlProc;
        wc.hInstance     = inst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kFsCtrlClass;
        RegisterClassA(&wc);
        g_fsCtrl = CreateWindowExA(
            WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            kFsCtrlClass, "", WS_POPUP,
            0, 0, kFsCtrlW, kFsCtrlH, nullptr, nullptr, inst, nullptr);
        if (g_fsCtrl)
        {
            SetLayeredWindowAttributes(g_fsCtrl, 0, 225, LWA_ALPHA);
            SetTimer(g_fsCtrl, kFsCtrlTimerId, 250, nullptr);
        }
    }

    // Position helper: top-left or top-right corner of the SR Loom main window's
    // CLIENT area. Anchoring to the window (not the SR display rect) means the
    // overlays sit correctly in BOTH fullscreen and windowed modes, and follow
    // the window when the user drags / resizes it.
    void OverlayCornerScreenPos(HWND mainHwnd, int overlayW, bool leftCorner,
                                int margin, int& outX, int& outY)
    {
        RECT cr; GetClientRect(mainHwnd, &cr);
        POINT tl{ cr.left, cr.top }, tr{ cr.right, cr.top };
        ClientToScreen(mainHwnd, &tl);
        ClientToScreen(mainHwnd, &tr);
        outY = tl.y + margin;
        outX = leftCorner ? (tl.x + margin) : (tr.x - overlayW - margin);
    }

    void ShowFsCtrlOverlay(const AppState& app)
    {
        if (!g_fsCtrl || !app.hwnd) return;
        int x = 0, y = 0;
        OverlayCornerScreenPos(app.hwnd, kFsCtrlW, /*leftCorner=*/false, 12, x, y);
        SetWindowPos(g_fsCtrl, HWND_TOPMOST, x, y, kFsCtrlW, kFsCtrlH,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        HRGN rgn = CreateRoundRectRgn(0, 0, kFsCtrlW + 1, kFsCtrlH + 1,
                                      kFsCtrlRadius * 2, kFsCtrlRadius * 2);
        SetWindowRgn(g_fsCtrl, rgn, TRUE);
        g_fsCtrlLastUse = GetTickCount();
    }

    void HideFsCtrlOverlay()
    {
        if (g_fsCtrl) ShowWindow(g_fsCtrl, SW_HIDE);
    }

    // ===== Top-left settings overlay ========================================
    // Inline controls for picking an image, format, and (when format == Quilt)
    // the quilt grid cols/rows. Same visual style as the right-corner overlay,
    // but each button opens a native popup menu via TrackPopupMenu — cheap to
    // implement and looks identical to standard Windows menus.
    constexpr char kFsSetClass[]   = "SRLoomFsSettings";
    constexpr int  kFsSetBtnH      = 52;
    constexpr int  kFsSetPad       = 10;
    constexpr int  kFsSetGap       = 8;
    constexpr int  kFsSetRadius    = 10;     // rounded-corner radius
    constexpr UINT kFsSetTimerId   = 8;
    constexpr int  kFsSetBtnLoad    = 0;
    constexpr int  kFsSetBtnFormat  = 1;
    constexpr int  kFsSetBtnCols    = 2;
    constexpr int  kFsSetBtnRows    = 3;
    constexpr int  kFsSetBtnAuto    = 4;
    constexpr int  kFsSetBtnLfpLean = 5;   // LightField: Head-lean preset picker
    constexpr int  kFsSetWLoad      = 180;
    constexpr int  kFsSetWFormat    = 250;
    constexpr int  kFsSetWCols      = 140;
    constexpr int  kFsSetWRows      = 140;
    constexpr int  kFsSetWAuto      = 130;
    constexpr int  kFsSetWLfpLean   = 160;
    constexpr int  kQuiltColsMax   = 12;
    constexpr int  kQuiltRowsMax   = 9;

    HWND  g_fsSet         = nullptr;
    int   g_fsSetHover    = -1;
    DWORD g_fsSetLastUse  = 0;
    bool  g_fsSetTracking = false;
    int   g_fsSetBtnCount = 2;
    RECT  g_fsSetRects[6] = {};
    int   g_fsSetCalcW    = 0;
    int   g_fsSetCalcH    = 0;
    // Light-field "head-lean" slider drag state (lives in the FS overlay).
    bool  g_fsSetSliderDrag    = false;
    constexpr float kLfpLeanMinMm = 1.0f;     // floor; clamped to aperture radius too
    constexpr float kLfpLeanMaxMm = 200.0f;   // user request: bumped from 100 to 200

    // Map a layout slot index (0..g_fsSetBtnCount) to the LOGICAL button
    // id (kFsSetBtn*). Slot 0 + 1 are always Load + Format. Beyond that
    // the meaning depends on the active format -- Quilt fills slots 2..4
    // with Cols / Rows / Auto, LightField fills slot 2 with the Head-lean
    // preset picker.
    int FsSetSlotId(int slot)
    {
        if (slot < 2) return slot;   // Load / Format
        const StereoFormat fmt = g_app ? g_app->format : StereoFormat::FullSBS;
        if (fmt == StereoFormat::LightField) return kFsSetBtnLfpLean;
        return slot;                  // Quilt: 2=Cols, 3=Rows, 4=Auto
    }

    int FsSetBtnW(int slot)
    {
        switch (FsSetSlotId(slot)) {
        case kFsSetBtnLoad:    return kFsSetWLoad;
        case kFsSetBtnFormat:  return kFsSetWFormat;
        case kFsSetBtnCols:    return kFsSetWCols;
        case kFsSetBtnRows:    return kFsSetWRows;
        case kFsSetBtnAuto:    return kFsSetWAuto;
        case kFsSetBtnLfpLean: return kFsSetWLfpLean;
        }
        return 60;
    }
    const RECT& FsSetBtnRect(int slot) { return g_fsSetRects[slot]; }

    // Lay out the buttons into rows. If everything fits on one row inside the
    // given max width, that's used; otherwise buttons wrap to additional rows
    // so the overlay stays inside the window even when the window is narrow.
    void FsSetComputeLayout(int btnCount, int maxWidth)
    {
        int x = kFsSetPad;
        int y = kFsSetPad;
        int rowMaxRight = kFsSetPad;
        int maxRightAll = kFsSetPad;
        for (int i = 0; i < btnCount; ++i)
        {
            const int w = FsSetBtnW(i);
            // Wrap when this button wouldn't fit on the current row.
            if (i > 0 && (x + w + kFsSetPad) > maxWidth)
            {
                if (rowMaxRight > maxRightAll) maxRightAll = rowMaxRight;
                x = kFsSetPad;
                y += kFsSetBtnH + kFsSetGap;
                rowMaxRight = kFsSetPad;
            }
            RECT& r = g_fsSetRects[i];
            r.left   = x;
            r.right  = x + w;
            r.top    = y;
            r.bottom = y + kFsSetBtnH;
            x += w + kFsSetGap;
            if (r.right > rowMaxRight) rowMaxRight = r.right;
        }
        if (rowMaxRight > maxRightAll) maxRightAll = rowMaxRight;
        g_fsSetCalcW = maxRightAll + kFsSetPad;
        g_fsSetCalcH = y + kFsSetBtnH + kFsSetPad;
    }

    int FsSetHit(int x, int y)
    {
        for (int i = 0; i < g_fsSetBtnCount; ++i)
        {
            RECT r = FsSetBtnRect(i);
            if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i;
        }
        return -1;
    }

    const char* FsCurrentFormatLabel(StereoFormat f)
    {
        int n = 0; const StereoFormatEntry* l = StereoFormatList(n);
        for (int i = 0; i < n; ++i) if (l[i].fmt == f) return l[i].label;
        return "?";
    }

    // Open a popup menu for picking a stereo format. Sent as a WM_COMMAND to
    // the main window so it routes through the existing tray-cmd handler
    // (which also calls EnsureWeavingFormatOnly to apply the change live).
    void FsOpenFormatMenu()
    {
        if (!g_app) return;
        HMENU menu = CreatePopupMenu();
        int n = 0; const StereoFormatEntry* l = StereoFormatList(n);
        for (int i = 0; i < n; ++i)
            AppendMenuA(menu, MF_STRING | (g_app->format == l[i].fmt ? MF_CHECKED : 0),
                        ID_TRAY_FMT_BASE + (UINT)i, l[i].label);
        POINT pt; GetCursorPos(&pt);
        SetForegroundWindow(g_app->hwnd);
        TrackPopupMenu(menu, TPM_LEFTBUTTON, pt.x, pt.y, 0, g_app->hwnd, nullptr);
        DestroyMenu(menu);
    }

    // Generic integer-picker popup for Quilt cols/rows. Returns the chosen
    // value (1..maxVal) or 0 if dismissed. Updates the corresponding app state.
    void FsOpenIntMenu(int currentVal, int maxVal, const char* unit,
                       int* outAppField)
    {
        if (!g_app || !outAppField) return;
        HMENU menu = CreatePopupMenu();
        for (int v = 1; v <= maxVal; ++v)
        {
            char buf[16]; sprintf_s(buf, "%d %s", v, unit);
            AppendMenuA(menu, MF_STRING | (currentVal == v ? MF_CHECKED : 0),
                        (UINT)v, buf);
        }
        POINT pt; GetCursorPos(&pt);
        SetForegroundWindow(g_app->hwnd);
        int picked = TrackPopupMenu(menu, TPM_LEFTBUTTON | TPM_RETURNCMD,
                                    pt.x, pt.y, 0, g_app->hwnd, nullptr);
        DestroyMenu(menu);
        if (picked >= 1 && picked <= maxVal)
        {
            *outAppField = picked;
            // Re-centre the view pair so the new grid has a sensible default.
            const int total = g_app->quiltCols * g_app->quiltRows;
            g_app->quiltLeftIdx  = total / 2 - 1; if (g_app->quiltLeftIdx  < 0) g_app->quiltLeftIdx  = 0;
            g_app->quiltRightIdx = total / 2;     if (g_app->quiltRightIdx >= total) g_app->quiltRightIdx = total - 1;
            g_app->quiltLeftBlend = g_app->quiltRightBlend = 0.0f;
            g_app->captureRebind = true;
            if (g_fsSet) InvalidateRect(g_fsSet, nullptr, FALSE);
        }
    }

    void OpenLoadTestImageDialog(AppState& app);   // fwd: defined below near other source helpers
    void AutoDetectQuiltOnCurrent(AppState& app);  // fwd: defined below near LoadTestImage

    LRESULT CALLBACK FsSetProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        AppLock appLock;   // (see g_appLock)
        switch (msg)
        {
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT cr; GetClientRect(hwnd, &cr);
            HBRUSH bg = CreateSolidBrush(kFsBg);
            FillRect(dc, &cr, bg);
            DeleteObject(bg);

            HFONT font = CreateFontW(22, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH, L"Segoe UI");
            HFONT oldFont = (HFONT)SelectObject(dc, font);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, kFsText);

            const StereoFormat fmt = g_app ? g_app->format : StereoFormat::FullSBS;
            const int cols = g_app ? g_app->quiltCols : 8;
            const int rows = g_app ? g_app->quiltRows : 6;

            for (int i = 0; i < g_fsSetBtnCount; ++i)
            {
                RECT r = FsSetBtnRect(i);
                FsPaintRoundButton(dc, r, i == g_fsSetHover ? kFsBtnHover : kFsBtnBg, kFsSetRadius);

                const int id = FsSetSlotId(i);

                // LightField "head-lean" slider: draw a horizontal track
                // + handle on top of the rounded background, with the
                // current value labelled.
                if (id == kFsSetBtnLfpLean)
                {
                    const float val = g_app ? g_app->lfpHeadLeanMm : 30.0f;
                    const float minV = kLfpLeanMinMm;
                    const float maxV = kLfpLeanMaxMm;
                    const float t    = (val - minV) / (maxV - minV);
                    const float tClamp = t < 0 ? 0 : (t > 1 ? 1 : t);
                    // Track: thin horizontal rect in the lower 1/3 of the button.
                    const int trackInset = 14;
                    const int trackY = (r.top + r.bottom) / 2 + 6;
                    RECT trackR{ r.left + trackInset, trackY - 2,
                                 r.right - trackInset, trackY + 2 };
                    HBRUSH trackBr = CreateSolidBrush(kFsDim);
                    FillRect(dc, &trackR, trackBr);
                    DeleteObject(trackBr);
                    // Handle: filled accent circle at the value position.
                    const int handleR = 7;
                    const int handleX = trackR.left + (int)(tClamp * (trackR.right - trackR.left));
                    HBRUSH hb = CreateSolidBrush(kFsCloseHot);
                    HPEN hp = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
                    HBRUSH ob = (HBRUSH)SelectObject(dc, hb);
                    Ellipse(dc, handleX - handleR, trackY - handleR,
                                handleX + handleR + 1, trackY + handleR + 1);
                    SelectObject(dc, ob);
                    SelectObject(dc, hp);
                    DeleteObject(hb);
                    // Label above the track.
                    wchar_t lbl[40]; swprintf_s(lbl, L"Lean: %.0f mm", val);
                    RECT tr{ r.left + 16, r.top, r.right - 16, trackY - 6 };
                    DrawTextW(dc, lbl, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    continue;   // skip generic button-text rendering
                }

                wchar_t label[64];
                switch (id)
                {
                case kFsSetBtnLoad:    wcscpy_s(label, L"Load Media..."); break;
                case kFsSetBtnFormat:  swprintf_s(label, L"%hs", FsCurrentFormatLabel(fmt)); break;
                case kFsSetBtnCols:    swprintf_s(label, L"%d cols", cols); break;
                case kFsSetBtnRows:    swprintf_s(label, L"%d rows", rows); break;
                case kFsSetBtnAuto:    wcscpy_s(label, L"Auto-detect"); break;
                default:               label[0] = 0;
                }
                const bool hasChevron = (id == kFsSetBtnFormat || id == kFsSetBtnCols
                                       || id == kFsSetBtnRows);
                RECT tr = r;
                tr.left  += 16;
                tr.right -= hasChevron ? 26 : 16;
                DrawTextW(dc, label, -1, &tr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                // Right-aligned dropdown chevron on menu-opening buttons.
                if (hasChevron)
                {
                    HPEN pen = CreatePen(PS_SOLID, 2, kFsDim);
                    HPEN op  = (HPEN)SelectObject(dc, pen);
                    const int ax = r.right - 16;
                    const int ay = (r.top + r.bottom) / 2;
                    MoveToEx(dc, ax - 6, ay - 3, nullptr); LineTo(dc, ax,     ay + 3);
                    MoveToEx(dc, ax,     ay + 3, nullptr); LineTo(dc, ax + 7, ay - 3);
                    SelectObject(dc, op);
                    DeleteObject(pen);
                }
            }
            SelectObject(dc, oldFont);
            DeleteObject(font);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE:
        {
            if (!g_fsSetTracking)
            {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                g_fsSetTracking = true;
            }
            const int hit = FsSetHit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (hit != g_fsSetHover) { g_fsSetHover = hit; InvalidateRect(hwnd, nullptr, FALSE); }
            g_fsSetLastUse = GetTickCount();
            // Drag the LFP lean slider if started.
            if (g_fsSetSliderDrag && g_app)
            {
                // Find the slider slot (LfpLean) -- it owns the drag.
                for (int i = 0; i < g_fsSetBtnCount; ++i)
                {
                    if (FsSetSlotId(i) != kFsSetBtnLfpLean) continue;
                    const RECT r = FsSetBtnRect(i);
                    const int trackInset = 14;
                    const int trackL = r.left + trackInset;
                    const int trackR = r.right - trackInset;
                    const int x = GET_X_LPARAM(lp);
                    const float t = (float)(x - trackL) / (float)(trackR - trackL);
                    const float tClamp = t < 0 ? 0 : (t > 1 ? 1 : t);
                    g_app->lfpHeadLeanMm = kLfpLeanMinMm + tClamp * (kLfpLeanMaxMm - kLfpLeanMinMm);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
                }
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_fsSetTracking = false;
            if (g_fsSetHover != -1) { g_fsSetHover = -1; InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;
        case WM_LBUTTONDOWN:
        {
            const int hit = FsSetHit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            g_fsSetLastUse = GetTickCount();
            if (!g_app || hit < 0) return 0;
            const int id = FsSetSlotId(hit);
            switch (id)
            {
            case kFsSetBtnLoad:   OpenLoadTestImageDialog(*g_app); break;
            case kFsSetBtnFormat: FsOpenFormatMenu(); break;
            case kFsSetBtnCols:   FsOpenIntMenu(g_app->quiltCols, kQuiltColsMax, "cols", &g_app->quiltCols); break;
            case kFsSetBtnRows:   FsOpenIntMenu(g_app->quiltRows, kQuiltRowsMax, "rows", &g_app->quiltRows); break;
            case kFsSetBtnAuto:
                AutoDetectQuiltOnCurrent(*g_app);
                InvalidateRect(hwnd, nullptr, FALSE);
                break;
            case kFsSetBtnLfpLean:
            {
                // Begin dragging the slider; jump to the clicked position.
                g_fsSetSliderDrag = true;
                SetCapture(hwnd);
                const RECT r = FsSetBtnRect(hit);
                const int trackInset = 14;
                const int trackL = r.left + trackInset;
                const int trackR = r.right - trackInset;
                const int x = GET_X_LPARAM(lp);
                const float t = (float)(x - trackL) / (float)(trackR - trackL);
                const float tClamp = t < 0 ? 0 : (t > 1 ? 1 : t);
                g_app->lfpHeadLeanMm = kLfpLeanMinMm + tClamp * (kLfpLeanMaxMm - kLfpLeanMinMm);
                InvalidateRect(hwnd, nullptr, FALSE);
                break;
            }
            }
            return 0;
        }
        case WM_LBUTTONUP:
            if (g_fsSetSliderDrag)
            {
                g_fsSetSliderDrag = false;
                ReleaseCapture();
            }
            return 0;
        case WM_TIMER:
            if (wp == kFsSetTimerId &&
                g_fsSetHover == -1 && (GetTickCount() - g_fsSetLastUse) > kFsCtrlHideMs)
                ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_NCHITTEST: return HTCLIENT;
        }
        return DefWindowProcA(hwnd, msg, wp, lp);
    }

    void EnsureFsSetWindow(HINSTANCE inst)
    {
        if (g_fsSet) return;
        WNDCLASSA wc{};
        wc.lpfnWndProc   = FsSetProc;
        wc.hInstance     = inst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kFsSetClass;
        RegisterClassA(&wc);
        g_fsSet = CreateWindowExA(
            WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            kFsSetClass, "", WS_POPUP,
            // Initial size is placeholder; Show recomputes the layout from
            // current button-count and window-width every time it's surfaced.
            0, 0, 400, kFsSetBtnH + kFsSetPad * 2, nullptr, nullptr, inst, nullptr);
        if (g_fsSet)
        {
            SetLayeredWindowAttributes(g_fsSet, 0, 225, LWA_ALPHA);
            SetTimer(g_fsSet, kFsSetTimerId, 250, nullptr);
        }
    }

    void ShowFsSetOverlay(const AppState& app)
    {
        if (!g_fsSet || !app.hwnd) return;
        // Quilt gets Cols / Rows / Auto-detect; LightField gets a Head-lean
        // preset picker; everything else just Load + Format.
        if      (app.format == StereoFormat::Quilt)       g_fsSetBtnCount = 5;
        else if (app.format == StereoFormat::LightField)  g_fsSetBtnCount = 3;
        else                                              g_fsSetBtnCount = 2;
        // Layout limit: try to fit inside the window's client width minus margins,
        // wrap to additional rows when too wide. Falls back to a single row if
        // the window's somehow ridiculously narrow.
        RECT cr; GetClientRect(app.hwnd, &cr);
        const int clientW   = cr.right - cr.left;
        const int sideSlack = 24;     // margin from window edge on each side
        int maxW = clientW - sideSlack * 2;
        if (maxW < 200) maxW = 1024;  // window too small to constrain -- just lay out single row
        FsSetComputeLayout(g_fsSetBtnCount, maxW);
        const int w = g_fsSetCalcW;
        const int h = g_fsSetCalcH;
        int x = 0, y = 0;
        OverlayCornerScreenPos(app.hwnd, w, /*leftCorner=*/true, 12, x, y);
        SetWindowPos(g_fsSet, HWND_TOPMOST, x, y, w, h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        // Rounded outer window region to match the buttons' rounded corners.
        HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, kFsSetRadius * 2, kFsSetRadius * 2);
        SetWindowRgn(g_fsSet, rgn, TRUE);
        InvalidateRect(g_fsSet, nullptr, FALSE);
        g_fsSetLastUse = GetTickCount();
    }

    void HideFsSetOverlay() { if (g_fsSet) ShowWindow(g_fsSet, SW_HIDE); }

    // ===== Video transport overlay ==========================================
    // Bottom-of-window strip with play/pause + progress bar + time text. Shows
    // when a video source is loaded; click the play glyph to toggle, click the
    // bar to seek.
    constexpr char kFsVidClass[]   = "SRLoomVideoCtrls";
    constexpr int  kFsVidH         = 64;
    constexpr int  kFsVidPadX      = 14;
    constexpr int  kFsVidPadY      = 10;
    constexpr int  kFsVidBtnW      = 48;
    constexpr int  kFsVidBtnH      = 44;
    constexpr int  kFsVidGap       = 14;
    constexpr int  kFsVidTimeW     = 140;          // room for "12:34 / 56:78"
    constexpr int  kFsVidBarH      = 12;
    constexpr int  kFsVidRadius    = 12;
    constexpr UINT kFsVidTimerId   = 9;
    constexpr DWORD kFsVidHideMs   = 3500;

    HWND  g_fsVid          = nullptr;
    bool  g_fsVidBtnHover  = false;
    bool  g_fsVidBarHover  = false;
    DWORD g_fsVidLastUse   = 0;
    bool  g_fsVidTracking  = false;

    RECT FsVidBtnRect(int clientW)
    {
        (void)clientW;
        RECT r{};
        r.left   = kFsVidPadX;
        r.top    = (kFsVidH - kFsVidBtnH) / 2;
        r.right  = r.left + kFsVidBtnW;
        r.bottom = r.top  + kFsVidBtnH;
        return r;
    }
    RECT FsVidBarRect(int clientW)
    {
        RECT r{};
        r.left   = kFsVidPadX + kFsVidBtnW + kFsVidGap;
        r.right  = clientW - kFsVidPadX - kFsVidTimeW - kFsVidGap;
        r.top    = (kFsVidH - kFsVidBarH) / 2;
        r.bottom = r.top + kFsVidBarH;
        if (r.right < r.left + 40) r.right = r.left + 40;
        return r;
    }
    RECT FsVidTimeRect(int clientW)
    {
        RECT r{};
        r.left   = clientW - kFsVidPadX - kFsVidTimeW;
        r.right  = clientW - kFsVidPadX;
        r.top    = 0;
        r.bottom = kFsVidH;
        return r;
    }

    void FsFormatHmsMmSs(long long hns, char* buf, size_t cap)
    {
        if (hns < 0) hns = 0;
        const long long totalSec = hns / 10000000LL;
        const int mm = (int)(totalSec / 60);
        const int ss = (int)(totalSec % 60);
        _snprintf_s(buf, cap, _TRUNCATE, "%d:%02d", mm, ss);
    }

    LRESULT CALLBACK FsVidProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        AppLock appLock;   // (see g_appLock)
        switch (msg)
        {
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT cr; GetClientRect(hwnd, &cr);
            HBRUSH bg = CreateSolidBrush(kFsBg);
            FillRect(dc, &cr, bg);
            DeleteObject(bg);
            const int W = cr.right;

            const RECT btn  = FsVidBtnRect(W);
            const RECT bar  = FsVidBarRect(W);
            const RECT time = FsVidTimeRect(W);

            // Play/pause button.
            FsPaintRoundButton(dc, btn, g_fsVidBtnHover ? kFsBtnHover : kFsBtnBg, 8);
            const int cx = (btn.left + btn.right)  / 2;
            const int cy = (btn.top  + btn.bottom) / 2;
            const bool playing = (g_app && g_app->video.IsOpen() && !g_app->video.IsPaused());
            HBRUSH glyphBrush = CreateSolidBrush(kFsText);
            HBRUSH oldB = (HBRUSH)SelectObject(dc, glyphBrush);
            HPEN nopen = (HPEN)GetStockObject(NULL_PEN);
            HPEN oldP = (HPEN)SelectObject(dc, nopen);
            if (playing)
            {
                // two vertical bars (pause)
                RECT a{ cx - 9, cy - 11, cx - 3, cy + 11 };
                RECT b{ cx + 3, cy - 11, cx + 9, cy + 11 };
                FillRect(dc, &a, glyphBrush);
                FillRect(dc, &b, glyphBrush);
            }
            else
            {
                // triangle (play)
                POINT t[3] = { {cx - 7, cy - 11}, {cx + 10, cy}, {cx - 7, cy + 11} };
                Polygon(dc, t, 3);
            }
            SelectObject(dc, oldB);
            SelectObject(dc, oldP);
            DeleteObject(glyphBrush);

            // Progress bar: dim track + accent fill up to current position.
            const long long dur = (g_app && g_app->video.IsOpen()) ? g_app->video.DurationHns() : 0;
            const long long pos = (g_app && g_app->video.IsOpen()) ? g_app->video.PositionHns() : 0;
            const double frac = (dur > 0) ? std::min(1.0, (double)pos / (double)dur) : 0.0;
            HBRUSH track = CreateSolidBrush(kFsBtnBg);
            FillRect(dc, &bar, track);
            DeleteObject(track);
            RECT fill = bar;
            fill.right = bar.left + (LONG)((bar.right - bar.left) * frac + 0.5);
            HBRUSH fillB = CreateSolidBrush(kFsCloseHot);   // warm accent
            FillRect(dc, &fill, fillB);
            DeleteObject(fillB);

            // Time text.
            char posStr[16], durStr[16];
            FsFormatHmsMmSs(pos, posStr, sizeof(posStr));
            FsFormatHmsMmSs(dur, durStr, sizeof(durStr));
            char both[40];
            _snprintf_s(both, _TRUNCATE, "%s / %s", posStr, durStr);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, kFsText);
            HFONT font = CreateFontW(20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH, L"Segoe UI");
            HFONT oldFont = (HFONT)SelectObject(dc, font);
            wchar_t wboth[40];
            MultiByteToWideChar(CP_UTF8, 0, both, -1, wboth, 40);
            RECT timeMut = time;
            DrawTextW(dc, wboth, -1, &timeMut, DT_VCENTER | DT_SINGLELINE | DT_CENTER);
            SelectObject(dc, oldFont);
            DeleteObject(font);

            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE:
        {
            if (!g_fsVidTracking)
            {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                g_fsVidTracking = true;
            }
            RECT cr; GetClientRect(hwnd, &cr);
            const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            const RECT btn = FsVidBtnRect(cr.right);
            const RECT bar = FsVidBarRect(cr.right);
            const bool bh  = (x >= btn.left && x < btn.right && y >= btn.top && y < btn.bottom);
            const bool ph  = (x >= bar.left && x < bar.right && y >= bar.top - 6 && y < bar.bottom + 6);
            if (bh != g_fsVidBtnHover || ph != g_fsVidBarHover)
            {
                g_fsVidBtnHover = bh;
                g_fsVidBarHover = ph;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            g_fsVidLastUse = GetTickCount();
            return 0;
        }
        case WM_MOUSELEAVE:
            g_fsVidTracking = false;
            if (g_fsVidBtnHover || g_fsVidBarHover)
            {
                g_fsVidBtnHover = g_fsVidBarHover = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN:
        {
            if (!g_app || !g_app->video.IsOpen()) return 0;
            RECT cr; GetClientRect(hwnd, &cr);
            const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            const RECT btn = FsVidBtnRect(cr.right);
            const RECT bar = FsVidBarRect(cr.right);
            g_fsVidLastUse = GetTickCount();
            if (x >= btn.left && x < btn.right && y >= btn.top && y < btn.bottom)
            {
                g_app->video.TogglePause();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            // Seek if click lands on (or very near) the bar.
            if (y >= bar.top - 6 && y < bar.bottom + 6 && x >= bar.left && x < bar.right)
            {
                const double frac = (double)(x - bar.left) / (double)(bar.right - bar.left);
                const long long dur = g_app->video.DurationHns();
                if (dur > 0) g_app->video.Seek((long long)(frac * (double)dur));
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_TIMER:
            if (wp == kFsVidTimerId)
            {
                if (!g_fsVidBtnHover && !g_fsVidBarHover &&
                    (GetTickCount() - g_fsVidLastUse) > kFsVidHideMs)
                    ShowWindow(hwnd, SW_HIDE);
                else
                    InvalidateRect(hwnd, nullptr, FALSE);   // tick the progress bar
            }
            return 0;
        case WM_NCHITTEST: return HTCLIENT;
        }
        return DefWindowProcA(hwnd, msg, wp, lp);
    }

    void EnsureFsVidWindow(HINSTANCE inst)
    {
        if (g_fsVid) return;
        WNDCLASSA wc{};
        wc.lpfnWndProc   = FsVidProc;
        wc.hInstance     = inst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kFsVidClass;
        RegisterClassA(&wc);
        g_fsVid = CreateWindowExA(
            WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            kFsVidClass, "", WS_POPUP,
            0, 0, 400, kFsVidH, nullptr, nullptr, inst, nullptr);
        if (g_fsVid)
        {
            SetLayeredWindowAttributes(g_fsVid, 0, 225, LWA_ALPHA);
            // Tick at ~10 Hz so the progress bar visibly advances during playback.
            SetTimer(g_fsVid, kFsVidTimerId, 100, nullptr);
        }
    }

    void ShowFsVidOverlay(const AppState& app)
    {
        if (!g_fsVid || !app.hwnd || !app.video.IsOpen()) return;
        RECT cr; GetClientRect(app.hwnd, &cr);
        POINT bl{ cr.left, cr.bottom }, br{ cr.right, cr.bottom };
        ClientToScreen(app.hwnd, &bl);
        ClientToScreen(app.hwnd, &br);
        const int clientW = br.x - bl.x;
        const int sideMargin = 20;
        int w = clientW - sideMargin * 2;
        if (w < 220) w = 220;
        if (w > 800) w = 800;
        const int x = bl.x + (clientW - w) / 2;
        const int y = bl.y - kFsVidH - 16;
        SetWindowPos(g_fsVid, HWND_TOPMOST, x, y, w, kFsVidH,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, kFsVidH + 1,
                                      kFsVidRadius * 2, kFsVidRadius * 2);
        SetWindowRgn(g_fsVid, rgn, TRUE);
        InvalidateRect(g_fsVid, nullptr, FALSE);
        g_fsVidLastUse = GetTickCount();
    }

    void HideFsVidOverlay() { if (g_fsVid) ShowWindow(g_fsVid, SW_HIDE); }

    // Re-anchor visible overlays after the main window moves / resizes.
    void RepositionOverlays(const AppState& app)
    {
        if (g_fsCtrl && IsWindowVisible(g_fsCtrl)) ShowFsCtrlOverlay(app);
        if (g_fsSet  && IsWindowVisible(g_fsSet))  ShowFsSetOverlay(app);
        if (g_fsVid  && IsWindowVisible(g_fsVid))  ShowFsVidOverlay(app);
    }

    // ------------------------------------------------------------------------

    // Per-eye content aspect for the current source + format. Used to size
    // the Windowed / Looking Glass window so it matches the 3D content shape
    // (no letterbox bars inside). FullSBS crops to centre 50% vertical so
    // per-eye is w/2 over h/2; HalfSBS per-eye is w/2 over h; TAB per-eye
    // is w over h/2; etc.
    double ContentAspect(const AppState& app)
    {
        double aspect = 16.0 / 9.0;
        int sw = 0, sh = 0;
        if (app.source == SourceKind::TestImage)
        {
            if (app.format == StereoFormat::LightField && app.lfpRenderer.HasData())
            {
                sw = app.lfpRenderer.OutputPerEyeWidth();
                sh = app.lfpRenderer.OutputHeight();
            }
            else if (app.video.IsOpen()) { sw = app.video.Width(); sh = app.video.Height(); }
            else                         { sw = app.weaver.SourceWidth(); sh = app.weaver.SourceHeight(); }
        }
        else
        {
            // Live capture: source dims come from the active capture.
            if (app.dxgiActive)              { sw = app.captureDxgi.Width(); sh = app.captureDxgi.Height(); }
            else if (app.capture.IsActive()) { sw = app.capture.Width();     sh = app.capture.Height(); }
        }
        if (sw <= 0 || sh <= 0) return aspect;
        double aw = (double)sw, ah = (double)sh;
        switch (app.format)
        {
        case StereoFormat::Quilt:
            if (app.quiltCols > 0 && app.quiltRows > 0) {
                aw /= app.quiltCols; ah /= app.quiltRows;
            }
            break;
        case StereoFormat::FullSBS:
            aw *= 0.5; ah *= 0.5; break;
        case StereoFormat::HalfSBS:           aw *= 0.5; break;
        case StereoFormat::FullTAB:
        case StereoFormat::HalfTAB:
        case StereoFormat::RowInterleaved:    ah *= 0.5; break;
        case StereoFormat::ColumnInterleaved: aw *= 0.5; break;
        default: break;
        }
        if (aw > 0 && ah > 0) aspect = aw / ah;
        return aspect;
    }

    // Looking Glass format change: re-fit the window to the new content
    // aspect WITHOUT moving it. Keeps the window's top-left corner and
    // height exactly where the user put them and only changes the width
    // (clamped to the SR display). A plain resize -- no restyle, no
    // swap-chain or z-order churn -- so the loupe doesn't jump or flicker.
    RECT FitRectToAspectKeepingPos(const RECT& r, double aspect, const RECT& d);   // fwd decl

    void RefitLoupeKeepingPosition(AppState& app)
    {
        RECT wr{};
        if (!GetWindowRect(app.hwnd, &wr)) return;
        const RECT nr = FitRectToAspectKeepingPos(wr, ContentAspect(app), app.srDisplayRect);
        const int w = nr.right - nr.left, h = nr.bottom - nr.top;
        if (w == wr.right - wr.left && h == wr.bottom - wr.top) return;
        SetWindowPos(app.hwnd, nullptr, 0, 0, w, h,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void UpdateTaskbarCutout(AppState& app, bool force);   // fwd decl (defined below)
    RECT FrameToScreen(const AppState& app, const RECT& f); // fwd decl (Auto Stereo, below)
    bool IsKeep2DWindow(const AppState& app, HWND h);       // fwd decl (below)
    bool VisibleFrameRect(HWND h, RECT& r);                 // fwd decl (below)
    void SetAutoDetect(AppState& app, bool on);             // fwd decl (Auto Stereo, below)
    void UpdateInputChoice(AppState& app);                  // fwd decl (Stereo 3D Input, below)
    const char* AnaComboName(int combo);                   // fwd decl (Auto Stereo, below)
    void EndLoupeOwnDrag(AppState& app);                    // fwd decl (below)
    void ApplyMode(AppState& app);                         // fwd decl (defined below)
    void EndAutoStereo(AppState& app, const char* why);    // fwd decl (defined below)
    void RemoveAutoRegion(AppState& app, int id, const char* why);   // fwd decl (below)

    // Native title bar (Looking Glass / Windowed) in Windows' light/dark app
    // mode, with ONE fixed caption colour for active and inactive. Without a
    // fixed colour it spawned in the bright "active" caption and flashed
    // until it lost focus. 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (Win10 20H1+),
    // 34/35/36 = border / caption / text colour (Win11; ignored on Win10).
    // Re-applied on WM_SETTINGCHANGE so a live theme switch is picked up.
    void ApplyCaptionTheme(HWND hwnd)
    {
        const bool     light   = Settings::ReadSystemUsesLightTheme();
        const BOOL     dark    = light ? FALSE : TRUE;
        const COLORREF caption = light ? RGB(243, 243, 243) : RGB(32, 32, 32);
        const COLORREF text    = light ? RGB(28, 28, 28)    : RGB(230, 230, 230);
        DwmSetWindowAttribute(hwnd, 20, &dark,    sizeof(dark));
        DwmSetWindowAttribute(hwnd, 34, &caption, sizeof(caption));
        DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption));
        DwmSetWindowAttribute(hwnd, 36, &text,    sizeof(text));
    }

    // Keep a rect's top-left and height, change its width to `aspect`,
    // clamped to the SR display. Used to fit the Looking Glass to an
    // image/video's shape without moving it.
    RECT FitRectToAspectKeepingPos(const RECT& r, double aspect, const RECT& d)
    {
        const int dw = d.right - d.left, dh = d.bottom - d.top;
        int h = r.bottom - r.top;
        int w = (int)(h * aspect + 0.5);
        const int maxW = (dw > 200) ? dw - 100 : 1280;
        const int maxH = (dh > 200) ? dh - 100 : 720;
        if (w > maxW) { w = maxW; h = (int)(w / aspect + 0.5); }
        if (h > maxH) { h = maxH; w = (int)(h * aspect + 0.5); }
        if (w < 320) w = 320;
        if (h < 240) h = 240;
        return { r.left, r.top, r.left + w, r.top + h };
    }

    // A capture source switch while the weave is on screen: hold the current
    // window (shape + last frame) until the new session's first frame. See
    // AppState::captureWarmup.
    void BeginCaptureWarmup(AppState& app)
    {
        app.captureWarmup        = true;
        app.captureWarmupStartMs = GetTickCount();
    }

    // Called by RenderFrame once the new capture delivered a frame (or the
    // warm-up timed out): apply the deferred mode now, in the same frame the
    // new source is first drawn.
    void EndCaptureWarmup(AppState& app)
    {
        app.captureWarmup = false;
        if (!app.modeApplyDeferred) return;
        app.modeApplyDeferred = false;
        ApplyMode(app);
    }

    void ApplyMode(AppState& app)
    {
        app.overlayRgnValid = false;   // (restyling may reset the window's shape)
        // Mid source-switch: keep the old window until the new capture's
        // first frame arrives (see captureWarmup). RenderFrame applies it.
        if (app.captureWarmup)
        {
            app.modeApplyDeferred = true;
            return;
        }
        // Auto Stereo lives in the Fullscreen weave window or the Looking Glass
        // (just the pictures under the glass); any other mode ends it.
        if (app.autoStereo && app.mode != OutputMode::Fullscreen && app.mode != OutputMode::LookingGlass)
            EndAutoStereo(app, "display mode changed");
        if (app.mode != OutputMode::LookingGlass)
        {
            // Leaving the Looking Glass: the window still has the loupe's
            // geometry here (we haven't restyled yet), so remember it -- the
            // next Looking Glass entry comes back exactly where it was.
            if (app.loupeActive)
            {
                RECT r{};
                if (GetWindowRect(app.hwnd, &r) && r.right > r.left && r.bottom > r.top)
                {
                    app.loupeSavedRect = r;
                    app.loupeHasSaved  = true;
                }
            }
            app.loupeActive = false;
        }
        // Right (min / windowed / close) overlay is ONLY for fullscreen test-image
        // viewing; in windowed mode the native title-bar chrome already does
        // those three actions. Left (settings) overlay shows in both -- it's
        // for image/format/quilt controls, which are useful regardless of mode.
        const bool isFsTestImage = (app.source == SourceKind::TestImage &&
                                    app.mode   == OutputMode::Fullscreen);
        const bool isTestImage   = (app.source == SourceKind::TestImage);
        if (!isFsTestImage) HideFsCtrlOverlay();
        if (!isTestImage)   HideFsSetOverlay();
        if (!isTestImage || !app.video.IsOpen()) HideFsVidOverlay();

        const RECT& d = app.srDisplayRect;
        const int dw = d.right - d.left;
        const int dh = d.bottom - d.top;
        const HWND hwnd = app.hwnd;
        const bool ct = WantsClickThrough(app);

        // If an LFP is loaded, the renderer's RT aspect depends on mode:
        // Fullscreen wants SR-display aspect (so the renderer pillarboxes
        // and we get aspect-correct fullscreen with side bars); other
        // modes want CONTENT aspect (so the window can fit tightly with
        // no bars). Looking-glass + Window-overlay also use content
        // since they're framed by other windows / overlays.
        if (app.lfpRenderer.HasData())
        {
            const float displayAspect = (dh > 0) ? (float)dw / (float)dh : 0.0f;
            const float targetAspect  = (app.mode == OutputMode::Fullscreen)
                                         ? displayAspect : 0.0f;   // 0 = content
            app.lfpRenderer.SetTargetAspect(targetAspect);
        }

        DWORD    style   = WS_POPUP;
        LONG_PTR exStyle = 0;
        RECT     rect    { d.left, d.top, d.left + dw, d.top + dh };
        HWND     zorder  = HWND_TOP;

        switch (app.mode)
        {
        case OutputMode::Fullscreen:
            style   = WS_POPUP;
            exStyle = ct ? (WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE) : 0;
            zorder  = ct ? HWND_TOPMOST : HWND_TOP;
            break;

        case OutputMode::WindowOverlay:
            style   = WS_POPUP;
            // NOT topmost: the overlay is pinned directly above the source window each
            // frame (UpdateOverlayTracking), so windows you alt-tab to can occlude the
            // weave. HWND_NOTOPMOST drops it out of the topmost band on entry.
            exStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
            zorder  = HWND_NOTOPMOST;
            rect    = { d.left, d.top, d.left + 1280, d.top + 720 };
            if (app.sourceWindow && IsWindow(app.sourceWindow))
                rect = VisibleWindowRect(app.sourceWindow);
            break;

        case OutputMode::LookingGlass:
        case OutputMode::Windowed:
        default:
        {
            const bool isLG = (app.mode == OutputMode::LookingGlass);
            style   = WS_OVERLAPPEDWINDOW;
            zorder  = isLG ? HWND_TOPMOST : HWND_NOTOPMOST;
            exStyle = isLG ? (WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT) : 0;

            // LG: preserve user-set size after first entry; only size on
            // initial open. Windowed: always re-size to content shape on
            // mode change.
            // Re-entering the Looking Glass (e.g. back from Fullscreen): use
            // the remembered rect if it's still on the SR display. For an
            // image/video, fit the width to the (possibly new) content
            // without moving it; in passthrough the shape is the user's.
            RECT onScreen{};
            if (isLG && !app.loupeActive && app.loupeHasSaved &&
                IntersectRect(&onScreen, &app.loupeSavedRect, &app.srDisplayRect))
            {
                rect = app.loupeSavedRect;
                if (app.source != SourceKind::CaptureMonitor)
                    rect = FitRectToAspectKeepingPos(rect, ContentAspect(app), app.srDisplayRect);
                app.loupeActive = true;
            }
            else if (isLG && app.loupeActive)
            {
                GetWindowRect(hwnd, &rect);
            }
            else
            {
                const double aspect = ContentAspect(app);
                int h = isLG ? 600 : 720;
                int w = (int)(h * aspect + 0.5);
                const int maxW = (dw > 200) ? dw - 100 : 1280;
                const int maxH = (dh > 200) ? dh - 100 : 720;
                if (w > maxW) { w = maxW; h = (int)(w / aspect + 0.5); }
                if (h > maxH) { h = maxH; w = (int)(h * aspect + 0.5); }
                if (w < 320)  { w = 320; }
                if (h < 240)  { h = 240; }
                const int x = d.left + (dw - w) / 2;
                const int y = d.top  + (dh - h) / 2;
                rect = { x, y, x + w, y + h };
                if (isLG) app.loupeActive = true;
            }
            break;
        }
        }

        // Going layered: switch to the bit-blt swap chain BEFORE the window
        // becomes layered. A flip-model chain can't present to a layered
        // window, so doing it after (the old order) guaranteed a blank frame
        // on e.g. test-image Fullscreen -> Looking Glass. Going non-layered
        // keeps the old order (style first, then the flip chain below),
        // since flip can't be created on a still-layered window. No-op when
        // the model is already right (passthrough Fullscreen <-> Looking
        // Glass are both layered and never recreate the chain).
        if (ct) app.renderer.SetLayered(true);
        // Katanga armed (selected, but no sender frames yet): keep the weave
        // window HIDDEN. Showing it here (the WS_VISIBLE style + the
        // SWP_SHOWWINDOW below) put an empty, black, full-display window up
        // -- the long-standing "Katanga turns the screen black" bug. The
        // render loop shows it when the first frame arrives.
        const bool katangaArmed = (app.format == StereoFormat::Katanga &&
                                   !app.katanga.IsReceiving());
        // (DirectComposition: the window was created without a redirection
        // bitmap; keep that flag through every restyle.)
        if (app.renderer.IsDComp()) exStyle |= WS_EX_NOREDIRECTIONBITMAP;
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle);
        SetWindowLongPtr(hwnd, GWL_STYLE, style | (katangaArmed ? 0 : WS_VISIBLE));
        // Looking Glass / Windowed use the native title bar: theme it BEFORE
        // the window is shown in its new style (see ApplyCaptionTheme).
        if (style & WS_CAPTION) ApplyCaptionTheme(hwnd);
        else
        {
            // Frameless modes (Fullscreen / overlay): no Win11 window border
            // at all -- otherwise DWM can draw its default light/accent 1px
            // outline around the weave. 34 = DWMWA_BORDER_COLOR,
            // 0xFFFFFFFE = DWMWA_COLOR_NONE (Win11; ignored on Win10).
            const COLORREF none = 0xFFFFFFFE;
            DwmSetWindowAttribute(hwnd, 34, &none, sizeof(none));
        }
        if (exStyle & WS_EX_LAYERED)
            SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);  // fully opaque
        // WindowOverlay applies a rounded-rect region per-frame in
        // UpdateOverlayTracking to match the source window's DWM corners; in
        // every other mode the overlay is a plain rectangle, so clear any
        // stale region left over from a prior WindowOverlay session.
        if (app.mode != OutputMode::WindowOverlay)
            SetWindowRgn(hwnd, nullptr, FALSE);
        app.renderer.SetVisibleAll();
        app.taskbarCutActive = false;   // region cleared; recomputed at the end
        UINT flags = SWP_FRAMECHANGED | (katangaArmed ? SWP_HIDEWINDOW : SWP_SHOWWINDOW);
        // The Looking Glass never needs focus when it appears (it's click-
        // through; grabbing its chrome still activates it normally), and
        // activating it would steal focus from the app the user is in.
        if ((exStyle & WS_EX_NOACTIVATE) || app.mode == OutputMode::LookingGlass)
            flags |= SWP_NOACTIVATE;
        SetWindowPos(hwnd, zorder, rect.left, rect.top,
                     rect.right - rect.left, rect.bottom - rect.top, flags);

        // Match the swap-chain model to the window: flip (low-latency) when not
        // click-through, bit-blt when layered (flip can't render on layered windows).
        app.renderer.SetLayered(ct);
        // Re-cut the taskbar hole for the new window rect (region was cleared above).
        UpdateTaskbarCutout(app, true);
    }

    // The looking glass passes clicks through the glass, but becomes grabbable when
    // the cursor is over its chrome (title bar / resize edges) or during a move/
    // resize — then returns to click-through. The cursor is polled directly so this
    // works even while the window is click-through.
    void EndLoupeOwnDrag(AppState& app)
    {
        if (!app.loupeDrag.active) return;
        app.loupeDrag.active = false;
        app.loupeDragging = false;
        if (GetCapture() == app.hwnd) ReleaseCapture();
    }

    // Our own Looking Glass move / resize (see WM_NCLBUTTONDOWN): each frame,
    // put the window where the mouse has taken it. Runs in the render loop,
    // so the glass keeps weaving at full rate while it's dragged.
    void UpdateLoupeOwnDrag(AppState& app)
    {
        if (!app.loupeDrag.active) return;
        if (app.mode != OutputMode::LookingGlass || !(GetAsyncKeyState(VK_LBUTTON) & 0x8000))
        {
            EndLoupeOwnDrag(app);
            return;
        }
        POINT pt{};
        GetCursorPos(&pt);
        const int dx = pt.x - app.loupeDrag.start.x, dy = pt.y - app.loupeDrag.start.y;
        RECT r = app.loupeDrag.startRect;
        const int hit = app.loupeDrag.hit;
        constexpr int kMin = 200;   // (as WM_GETMINMAXINFO)
        if (hit == HTCAPTION) OffsetRect(&r, dx, dy);
        else
        {
            if (hit == HTLEFT  || hit == HTTOPLEFT    || hit == HTBOTTOMLEFT)  r.left   = (std::min)(r.left + dx, r.right - kMin);
            if (hit == HTRIGHT || hit == HTTOPRIGHT   || hit == HTBOTTOMRIGHT) r.right  = (std::max)(r.right + dx, r.left + kMin);
            if (hit == HTTOP   || hit == HTTOPLEFT    || hit == HTTOPRIGHT)    r.top    = (std::min)(r.top + dy, r.bottom - kMin);
            if (hit == HTBOTTOM|| hit == HTBOTTOMLEFT || hit == HTBOTTOMRIGHT) r.bottom = (std::max)(r.bottom + dy, r.top + kMin);
        }
        RECT cur{};
        GetWindowRect(app.hwnd, &cur);
        if (!EqualRect(&cur, &r))
            SetWindowPos(app.hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void UpdateLoupeInteractivity(AppState& app)
    {
        if (app.mode != OutputMode::LookingGlass || app.modeApplyDeferred) return;

        bool interactive;
        if (app.loupeDragging)
        {
            interactive = true;   // stay grabbable for the whole move/resize
        }
        else
        {
            POINT pt{};
            GetCursorPos(&pt);
            RECT wr{}, cr{};
            GetWindowRect(app.hwnd, &wr);
            GetClientRect(app.hwnd, &cr);
            POINT tl{ cr.left, cr.top }, br{ cr.right, cr.bottom };
            ClientToScreen(app.hwnd, &tl);
            ClientToScreen(app.hwnd, &br);
            const bool inWindow = pt.x >= wr.left && pt.x < wr.right && pt.y >= wr.top && pt.y < wr.bottom;
            const bool inGlass  = pt.x >= tl.x   && pt.x < br.x     && pt.y >= tl.y   && pt.y < br.y;
            interactive = inWindow && !inGlass;   // over the chrome
        }

        if (interactive == app.loupeInteractive) return;
        app.loupeInteractive = interactive;

        LONG_PTR ex = WS_EX_TOPMOST | WS_EX_LAYERED | (interactive ? 0 : WS_EX_TRANSPARENT);
        if (app.renderer.IsDComp()) ex |= WS_EX_NOREDIRECTIONBITMAP;
        SetWindowLongPtr(app.hwnd, GWL_EXSTYLE, ex);
        SetLayeredWindowAttributes(app.hwnd, 0, 255, LWA_ALPHA);
        SetWindowPos(app.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_NOACTIVATE);
    }

    constexpr ULONGLONG kSrKeepAliveMs = 5000;   // (SetWeaving: the SR session outlives weaving by this)
    void PaceForSrRefresh(AppState& app, bool now = false);   // fwd decl (defined below)
    void SetWeaving(AppState& app, bool enable);   // fwd decl (defined below)
    void ChangeFormat(AppState& app, StereoFormat newFmt);  // fwd decl
    void EnsureWeavingFormatOnly(AppState& app);   // fwd decl
    void EnsureWeaving(AppState& app);             // fwd decl
    void UseWindow(AppState& app, HWND target);    // fwd decl

    // Foreground/process helpers for the per-game profile system. Both
    // return "" on failure (no exception, no abort) so the foreground
    // handler can safely no-op when it can't read a window's identity.
    std::string ForegroundExeBaseName(HWND fg)
    {
        if (!fg) return {};
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        if (pid == 0) return {};
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!proc) return {};
        char path[MAX_PATH] = {};
        DWORD len = MAX_PATH;
        const BOOL ok = QueryFullProcessImageNameA(proc, 0, path, &len);
        CloseHandle(proc);
        if (!ok || len == 0) return {};
        const char* slash = strrchr(path, '\\');
        return slash ? std::string(slash + 1) : std::string(path);
    }

    std::string WindowTitle(HWND fg)
    {
        if (!fg) return {};
        char buf[512] = {};
        if (GetWindowTextA(fg, buf, (int)sizeof(buf)) <= 0) return {};
        return std::string(buf);
    }

    // True if `h` currently covers its entire monitor (fullscreen). Used
    // by the fullscreenOnly profile condition: VLC / MPC / an image viewer
    // going fullscreen should trigger the weave; going back to windowed
    // should turn it off. Compares against the FULL monitor rect (not the
    // work area) since fullscreen apps cover the taskbar too. Small 2-px
    // tolerance for the window manager's invisible resize border on some
    // borderless-fullscreen setups.
    bool IsWindowFullscreen(HWND h)
    {
        if (!h || !::IsWindow(h) || !::IsWindowVisible(h)) return false;
        if (::IsIconic(h)) return false;
        RECT wr{};
        if (!::GetWindowRect(h, &wr)) return false;
        HMONITOR mon = ::MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        if (!::GetMonitorInfo(mon, &mi)) return false;
        return wr.left   <= mi.rcMonitor.left   + 2 &&
               wr.top    <= mi.rcMonitor.top    + 2 &&
               wr.right  >= mi.rcMonitor.right  - 2 &&
               wr.bottom >= mi.rcMonitor.bottom - 2;
    }

    // Apply a profile's stereo settings to the running state. Used by
    // both the auto-apply path (foreground match) and the manual
    // "click a profile in the tray menu" path.
    //
    // captureHwnd: when the foreground hook fires the auto-apply path, we
    // pass the game's window so SR Loom re-targets the capture at the
    // game (mode -> WindowOverlay, source -> CaptureWindow). Without this
    // a freshly-launched game would just see the format flip but the
    // weave would still be pointed at whatever it was pointed at before
    // (probably "Monitor"), so the user wouldn't see anything. The manual
    // tray "apply" path passes nullptr -- it doesn't know which window
    // the user "meant", so it just flips the format and lets the user
    // pick the source themselves.
    void ApplyProfile(AppState& app, const Profile& p, HWND captureHwnd = nullptr,
                      const std::string& currentTitle = std::string())
    {
        // If the profile has format=auto, resolve the effective format
        // from the current window title. Recognises tokens like HSBS /
        // HTAB / _2x1 / MVC / anaglyph / etc. (see Profiles::DetectFormatFromTitle).
        // Falls back to defaultFormat if nothing recognisable.
        StereoFormat effFormat = p.format;
        if (p.useAutoFormat)
        {
            bool detected = false;
            StereoFormat f = Profiles::DetectFormatFromTitle(currentTitle, detected);
            effFormat = detected ? f : p.defaultFormat;
            Log("Profile auto-format: title='%s' -> %s (detected=%d, default=%s)",
                currentTitle.c_str(), Profiles::FormatToString(effFormat),
                (int)detected, Profiles::FormatToString(p.defaultFormat));
        }
        Log("Profile apply: '%s' (format=%s swap=%d conv=%.2f hwnd=%p HT=%d)",
            p.name.c_str(), Profiles::FormatToString(effFormat),
            (int)p.swapEyes, (double)p.convergence, (void*)captureHwnd,
            (int)p.includeHeadTracking);
        // Format + format-specific sub-options. Set the sub-options BEFORE
        // ChangeFormat so the freshly-applied format reads the right
        // anaglyph combo / decode mode / pulfrich timing / FP preset on
        // its first frame.
        app.swapEyes       = p.swapEyes;
        app.convergence    = p.convergence;
        app.anaglyphCombo  = p.anaglyphCombo;
        app.anaglyphMode   = p.anaglyphMode;
        app.pulfrichMode   = (PulfrichMode)p.pulfrichMode;
        app.pulfrichDelay  = p.pulfrichDelay;
        app.pulfrichNd     = p.pulfrichNd;
        app.framePackMode  = p.framePackMode;
        if (p.quiltCols > 0) app.quiltCols = p.quiltCols;
        if (p.quiltRows > 0) app.quiltRows = p.quiltRows;
        if (p.quiltLeftIdx  >= 0) app.quiltLeftIdx  = p.quiltLeftIdx;
        if (p.quiltRightIdx >= 0) app.quiltRightIdx = p.quiltRightIdx;
        app.autoInput = false;   // (the profile says which input)
        if (app.autoStereo) EndAutoStereo(app, "profile applied");
        ChangeFormat(app, effFormat);
        if (captureHwnd)
        {
            UseWindow(app, captureHwnd);   // sets mode = WindowOverlay
            EnsureWeaving(app);             // full ensure -- starts weave if off
        }
        else
        {
            EnsureWeavingFormatOnly(app);
        }
        // Optional head-tracking apply. Only if the profile opted in --
        // most users won't want a per-game HT override and don't want
        // their global HT state stomped on every alt-tab.
        if (p.includeHeadTracking)
        {
            auto cfg = app.openTrack.GetConfig();
            cfg.outputMode  = p.htOutputMode;
            cfg.invertX     = p.htInvertX;
            cfg.invertY     = p.htInvertY;
            cfg.invertZ     = p.htInvertZ;
            cfg.invertYaw   = p.htInvertYaw;
            cfg.invertPitch = p.htInvertPitch;
            cfg.invertRoll  = p.htInvertRoll;
            app.openTrack.SetConfig(cfg);
            app.openTrack.SetOutputs(p.htOpenTrack, p.htFreeTrack, p.htTrackIR);
        }
        app.lastAppliedProfile = p.name;
        // Only a fullscreenOnly profile bound to a real window gets the
        // "turn off when it leaves fullscreen" tracking. Anything else
        // (another profile, a manual apply) replaces it, so a stale handle
        // can't later switch off an unrelated weave.
        app.activeFullscreenProfileHwnd =
            (p.fullscreenOnly && captureHwnd) ? captureHwnd : nullptr;
    }

    // True for any window owned by SR Loom's own process (GUI panel,
    // weave window, fullscreen-control overlay, tray menus).
    bool IsOwnProcessWindow(HWND h)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        return pid == GetCurrentProcessId();
    }

    // Called when WinEventHook reports a foreground-window change AND
    // also polled from the main loop every ~250ms so that entering/exiting
    // fullscreen without focus change (VLC F11, browser F11) and title
    // changes (VLC's next playlist file) are still picked up. Walks the
    // profile list, applies the first match.
    void HandleForegroundChanged(AppState& app, HWND fg)
    {
        if (!app.profilesAutoApply || app.profiles.empty()) return;
        if (!fg || IsOwnProcessWindow(fg)) return;
        const std::string exe = ForegroundExeBaseName(fg);
        const std::string title = WindowTitle(fg);
        if (exe.empty() && title.empty()) return;
        for (const auto& p : app.profiles)
        {
            if (!Profiles::Matches(p, exe, title)) continue;
            // fullscreenOnly gate: only apply when the target window is
            // actually presenting fullscreen. `continue`, not `return`, so
            // a later profile for the same app (e.g. a windowed variant)
            // still gets its chance. The periodic poll keeps re-invoking
            // us, so entering fullscreen later triggers the apply.
            if (p.fullscreenOnly && !IsWindowFullscreen(fg)) continue;
            // Debounce on HWND + profile: same window + same profile = no-op,
            // but a new HWND (relaunch, different instance) or a different
            // profile for the same window (windowed -> fullscreen variant)
            // re-applies.
            if (fg != app.lastAppliedHwnd || p.name != app.lastAutoAppliedProfile)
            {
                ApplyProfile(app, p, fg, title);
                app.lastAppliedHwnd        = fg;
                app.lastAutoAppliedProfile = p.name;
                app.lastAutoAppliedTitle   = title;
            }
            else if (p.useAutoFormat && title != app.lastAutoAppliedTitle)
            {
                // Same window + profile, new title: a media player moved on
                // to the next file. Re-detect the format only -- a full
                // re-apply would stomp the user's convergence/swap tweaks.
                // ChangeFormat doesn't turn weaving on, so a user who paused
                // the weave stays paused (the new format is ready for when
                // they resume).
                app.lastAutoAppliedTitle = title;
                bool detected = false;
                const StereoFormat f = Profiles::DetectFormatFromTitle(title, detected);
                const StereoFormat eff = detected ? f : p.defaultFormat;
                if (eff != app.format)
                {
                    Log("Profile auto-format: title changed '%s' -> %s (detected=%d)",
                        title.c_str(), Profiles::FormatToString(eff), (int)detected);
                    ChangeFormat(app, eff);
                }
            }
            return;
        }
    }

    // Periodic tick for fullscreen-condition profiles. Handles two events
    // the WinEventHook doesn't cover: (a) exiting fullscreen without focus
    // change -> disable the weave; (b) entering fullscreen without focus
    // change -> apply the profile. Throttled to 250ms so this is cheap
    // even in the tight render loop.
    void PollProfileFullscreenState(AppState& app)
    {
        const DWORD now = GetTickCount();
        if (app.lastFullscreenPollMs != 0 && (now - app.lastFullscreenPollMs) < 250)
            return;
        app.lastFullscreenPollMs = now;

        // (a) Currently-active fullscreen-condition profile: if its window
        // stops being fullscreen (or dies), turn the weave off. We do NOT
        // touch format / other settings -- just disable, so re-entering
        // fullscreen picks the same profile up cleanly. Only if we're still
        // weaving that window: if the user has since pointed the weave at
        // something else, just drop the tracking.
        if (app.activeFullscreenProfileHwnd)
        {
            HWND h = app.activeFullscreenProfileHwnd;
            if (!::IsWindow(h) || !IsWindowFullscreen(h))
            {
                const bool stillOurs = app.weavingEnabled &&
                                       app.source == SourceKind::CaptureWindow &&
                                       app.sourceWindow == h;
                Log("Profile: fullscreen exited (hwnd=%p)%s", (void*)h,
                    stillOurs ? ", disabling weave" : "");
                if (stillOurs) SetWeaving(app, false);
                app.activeFullscreenProfileHwnd = nullptr;
                app.lastAppliedHwnd = nullptr;
                app.lastAppliedProfile.clear();
                app.lastAutoAppliedProfile.clear();
            }
        }

        // (b) Foreground window may have entered fullscreen, or changed
        // title, since we last checked. HandleForegroundChanged is
        // internally guarded (own windows, no match, not fullscreen yet,
        // already applied), so calling it every tick is safe + cheap.
        if (app.profilesAutoApply && !app.profiles.empty())
            HandleForegroundChanged(app, ::GetForegroundWindow());
    }

    // --- Weave window vs. taskbar / fullscreen apps ----------------------
    //
    // Fullscreen passthrough and the Looking Glass sit in the topmost band so
    // nothing covers the weave. Two refinements on top of that:
    //   - the SR display's taskbar is cut OUT of the weave window whenever it's
    //     showing, so the real 2D taskbar is visible (un-woven) through the
    //     hole while SR Loom stays on top of everything else;
    //   - a fullscreen app/game that pushes itself above us (many set
    //     HWND_TOPMOST on activation) gets put back underneath.

    HMONITOR SrMonitor(const AppState& app);   // fwd decl (defined below)

    bool IsTopmostWeaveMode(const AppState& app)
    {
        return app.weavingEnabled && app.hwnd && IsWindowVisible(app.hwnd) &&
               (app.mode == OutputMode::Fullscreen || app.mode == OutputMode::LookingGlass) &&
               app.format != StereoFormat::Katanga;   // Katanga pins itself above the game
    }

    bool IsCloaked(HWND h)
    {
        DWORD cloaked = 0;
        return SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
               cloaked != 0;
    }

    // Windows shell UI (desktop, Start, search, task view, flyouts) --
    // allowed above the weave, and never treated as a fullscreen app.
    bool IsShellWindowUncached(HWND h);

    // IsShellWindow runs every frame from UpdateTaskbarCutout on the
    // foreground window, and resolving a process name needs OpenProcess --
    // so remember the answer for the last window asked about.
    bool IsShellWindow(HWND h)
    {
        static HWND s_hwnd  = nullptr;
        static bool s_shell = false;
        if (h != s_hwnd || !IsWindow(h))
        {
            s_hwnd  = h;
            s_shell = IsShellWindowUncached(h);
        }
        return s_shell;
    }

    bool IsShellWindowUncached(HWND h)
    {
        char cls[64] = {};
        GetClassNameA(h, cls, (int)sizeof(cls));
        if (!strcmp(cls, "Progman") || !strcmp(cls, "WorkerW") ||
            !strcmp(cls, "Shell_TrayWnd") || !strcmp(cls, "Shell_SecondaryTrayWnd"))
            return true;
        const std::string exe = ForegroundExeBaseName(h);
        static const char* kShellExes[] = {
            "explorer.exe", "ShellExperienceHost.exe", "StartMenuExperienceHost.exe",
            "SearchHost.exe", "SearchApp.exe", "ShellHost.exe",
        };
        for (const char* s : kShellExes)
            if (_stricmp(exe.c_str(), s) == 0) return true;
        return false;
    }

    // The SR display's taskbar (primary "Shell_TrayWnd" or a per-monitor
    // "Shell_SecondaryTrayWnd"), or nullptr if that monitor has none.
    // The SR display's taskbar. Remembered: finding it (FindWindow) goes
    // through the window manager and stalled the render loop at times --
    // looked up again only when it's gone / moved, or every 2 s.
    HWND SrTaskbar(HMONITOR srMon)
    {
        static HWND s_tb = nullptr;
        static HMONITOR s_mon = nullptr;
        static DWORD s_at = 0;
        const DWORD now = GetTickCount();
        if (s_mon == srMon && now - s_at < 2000 &&
            (!s_tb || (IsWindow(s_tb) && MonitorFromWindow(s_tb, MONITOR_DEFAULTTONULL) == srMon)))
            return s_tb;
        s_mon = srMon;
        s_at  = now;
        s_tb  = nullptr;
        HWND tb = FindWindowA("Shell_TrayWnd", nullptr);
        if (tb && IsWindowVisible(tb) && MonitorFromWindow(tb, MONITOR_DEFAULTTONULL) == srMon)
            return s_tb = tb;
        for (HWND s = FindWindowExA(nullptr, nullptr, "Shell_SecondaryTrayWnd", nullptr); s;
             s = FindWindowExA(nullptr, s, "Shell_SecondaryTrayWnd", nullptr))
        {
            if (IsWindowVisible(s) && MonitorFromWindow(s, MONITOR_DEFAULTTONULL) == srMon)
                return s_tb = s;
        }
        return nullptr;
    }

    // z-order band of a window (undocumented user32!GetWindowBand, present on
    // Win8+). 1 = ZBID_DESKTOP (every normal app, SR Loom included); anything
    // higher is system UI that always stacks above us: Start, toasts / Action
    // Center, volume OSD, Alt+Tab, Win+V / emoji, Game Bar. Returns 0 if the
    // API is unavailable or fails.
    DWORD WindowBand(HWND h)
    {
        using GetWindowBandFn = BOOL (WINAPI*)(HWND, DWORD*);
        static GetWindowBandFn fn = reinterpret_cast<GetWindowBandFn>(
            GetProcAddress(GetModuleHandleA("user32.dll"), "GetWindowBand"));
        DWORD band = 0;
        if (!fn || !fn(h, &band)) return 0;
        return band;
    }

    // Cut holes in the weave window's region so things that must stay 2D show
    // through un-woven, with no woven "ghost" of them underneath:
    //   - the SR display's taskbar, while it's showing (not auto-hidden, and
    //     no fullscreen app/game in front of it);
    //   - every visible system-UI window above the weave (band > desktop, see
    //     WindowBand). They're drawn above us anyway, but the capture also
    //     contains them, so their woven copy showed through translucent parts
    //     and trailed behind as they moved/closed. Their holes linger ~200ms
    //     after they go, to outlast the capture's frame or two of lag.
    // Runs every frame; only calls SetWindowRgn when the hole set changes,
    // unless force (after ApplyMode cleared the region). Never touches
    // WindowOverlay's rounded region: taskbarCutActive is only ever set in
    // the topmost modes.
    // Windows 11 rounds window corners (8 px at 100% scaling); maximised /
    // fullscreen windows are square.
    int WindowCornerRadius(HWND h)
    {
        if (IsZoomed(h) || IsWindowFullscreen(h)) return 0;
        const UINT dpi = GetDpiForWindow(h);
        return (int)(8 * (dpi ? dpi : 96) / 96);
    }

    // What a system pop-up actually shows: shell flyouts (e.g. the tray
    // overflow) are big transparent windows around a smaller content child,
    // so use the union of their visible children when that's smaller.
    RECT PopupContentRect(HWND h, const RECT& frame)
    {
        struct Ctx { RECT u; bool any; } ctx{ {}, false };
        EnumChildWindows(h, [](HWND c, LPARAM lp) -> BOOL {
            auto* x = reinterpret_cast<Ctx*>(lp);
            if (!IsWindowVisible(c) || GetParent(c) != GetAncestor(c, GA_PARENT)) return TRUE;
            RECT r{};
            if (!GetWindowRect(c, &r) || IsRectEmpty(&r)) return TRUE;
            if (!x->any) { x->u = r; x->any = true; } else UnionRect(&x->u, &x->u, &r);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
        RECT out = frame;
        if (ctx.any && IntersectRect(&out, &ctx.u, &frame)) return out;
        return frame;
    }

    // Window watcher: a background thread that keeps a fresh snapshot of every
    // visible top-level window in z-order (top first), with what the cut-outs
    // need worked out per window. The render loop only reads the latest
    // snapshot: walking the window list itself (and asking which process each
    // window belongs to) occasionally stalled it for tens of ms.
    struct WinInfo
    {
        HWND     h = nullptr;
        RECT     frame{};            // visible frame (no invisible resize border)
        RECT     content{};          // what a pop-up actually shows (== frame for normal windows)
        DWORD    band = 0;           // z-band (see WindowBand)
        LONG_PTR ex = 0;             // extended style
        int      radius = 0;         // rounded corners (see WindowCornerRadius)
        bool     own = false;        // one of SR Loom's windows
        bool     gui = false;        // SR Loom's panel
        bool     shell = false;      // Windows shell UI
        bool     trayWnd = false;    // a taskbar
        bool     desktop = false;    // Progman / WorkerW (the list stops here)
        bool     keep2D = false;     // always stays 2D (panel, terminals)
        bool     fullscreenOnSr = false;
    };
    struct WinSnapshot
    {
        std::vector<WinInfo> z;
        int64_t              t100 = 0;   // QPC time taken, 100 ns units
    };

    class WindowWatcher
    {
    public:
        ~WindowWatcher() { Stop(); }
        void Start()
        {
            if (m_thread.joinable()) return;
            m_run = true;
            m_thread = std::thread([this] { Loop(); });
        }
        void Stop()
        {
            m_run = false;
            if (m_thread.joinable()) m_thread.join();
        }
        // Only works while something needs it (a topmost weave mode).
        void SetActive(bool on) { m_active = on; }
        void SetContext(HWND gui, HMONITOR srMon) { m_gui = gui; m_srMon = srMon; }
        std::shared_ptr<const WinSnapshot> Latest()
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            return m_latest;
        }

    private:
        // Rebuilds when Windows reports a top-level window moving, showing,
        // hiding, changing z-order or (un)cloaking -- the events arrive on
        // this thread's message queue -- and every 30 ms regardless (anything
        // the events miss). It used to rebuild every ~1 ms whether anything
        // had changed or not: hundreds of window-manager queries a second,
        // competing with the render thread for the window manager's lock.
        void Loop()
        {
            std::vector<HWINEVENTHOOK> hooks;
            DWORD lastBuild = 0;
            while (m_run)
            {
                if (!m_active)
                {
                    if (!hooks.empty()) { for (HWINEVENTHOOK h : hooks) UnhookWinEvent(h); hooks.clear(); }
                    Sleep(20);
                    continue;
                }
                if (hooks.empty())
                {
                    s_dirty = true;
                    const DWORD fl = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNTHREAD;
                    const std::pair<DWORD, DWORD> ranges[] = {
                        { EVENT_OBJECT_DESTROY, EVENT_OBJECT_REORDER },   // destroy, show, hide, reorder
                        { EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE },
                        { EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED },
                        { EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND },
                        { EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND },
                        { EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND },
                    };
                    for (const auto& r : ranges)
                        if (HWINEVENTHOOK h = SetWinEventHook(r.first, r.second, nullptr, &OnWinEvent, 0, 0, fl))
                            hooks.push_back(h);
                }
                // Deliver the hook callbacks (they only set s_dirty).
                MSG msg;
                while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessage(&msg);
                const DWORD now = GetTickCount();
                // (A burst of window events -- a menu fading in, a page redrawing
                // under it -- rebuilt the list for every one, each rebuild asking
                // the compositor about every window: at most one every 10 ms.)
                if ((s_dirty && now - lastBuild >= 10) || now - lastBuild >= 30)
                {
                    s_dirty = false;
                    lastBuild = now;
                    auto snap = std::make_shared<WinSnapshot>();
                    Build(*snap);
                    std::lock_guard<std::mutex> lk(m_mutex);
                    m_latest = std::move(snap);
                }
                // Wake on the next event (or in time for the periodic rebuild).
                const DWORD since = GetTickCount() - lastBuild;
                const DWORD wait = s_dirty ? (since >= 10 ? 0 : 10 - since) : 30;   // (an event waiting: as soon as 10 ms are up)
                MsgWaitForMultipleObjects(0, nullptr, FALSE, wait, QS_ALLINPUT);
            }
            for (HWINEVENTHOOK h : hooks) UnhookWinEvent(h);
        }

        static void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD, HWND h, LONG idObject, LONG idChild, DWORD, DWORD)
        {
            // Top-level windows only (not the cursor, carets or controls).
            if (!h || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
            if (GetAncestor(h, GA_PARENT) != GetDesktopWindow()) return;
            s_dirty = true;
        }
        static inline std::atomic<bool> s_dirty{ true };

        void Build(WinSnapshot& s)
        {
            LARGE_INTEGER qpc{}, qpf{};
            QueryPerformanceCounter(&qpc);
            QueryPerformanceFrequency(&qpf);
            s.t100 = (int64_t)((double)qpc.QuadPart * 1.0e7 / (double)qpf.QuadPart);
            const HWND gui = m_gui.load();
            const HMONITOR srMon = m_srMon.load();
            s.z.reserve(128);
            for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT))
            {
                if (!IsWindowVisible(h) || IsIconic(h) || IsCloaked(h)) continue;
                WinInfo w;
                w.h = h;
                Cached& c = CacheFor(h);
                w.desktop = c.desktop;
                if (w.desktop) { s.z.push_back(w); break; }   // the desktop: nothing below matters
                if (!VisibleFrameRect(h, w.frame)) continue;
                w.ex      = GetWindowLongPtr(h, GWL_EXSTYLE);
                w.band    = WindowBand(h);
                w.own     = c.own;
                w.gui     = (h == gui);
                w.shell   = c.shell;
                w.trayWnd = c.trayWnd;
                w.keep2D  = w.gui || c.terminal;
                w.radius  = WindowCornerRadius(h);
                w.content = (w.band > 1) ? PopupContentRect(h, w.frame) : w.frame;
                w.fullscreenOnSr = srMon && MonitorFromWindow(h, MONITOR_DEFAULTTONULL) == srMon &&
                                   IsWindowFullscreen(h);
                s.z.push_back(w);
            }
            // Forget windows that no longer exist (now and then).
            if (++m_sweep % 500 == 0)
                for (auto it = m_cache.begin(); it != m_cache.end();)
                    it = IsWindow(it->first) ? std::next(it) : m_cache.erase(it);
        }

        // Per-window facts that never change: class and owning process.
        struct Cached { bool desktop = false, trayWnd = false, terminal = false, own = false, shell = false; };
        Cached& CacheFor(HWND h)
        {
            auto it = m_cache.find(h);
            if (it != m_cache.end()) return it->second;
            Cached c;
            char cls[64] = {};
            GetClassNameA(h, cls, (int)sizeof(cls));
            c.desktop  = !strcmp(cls, "Progman") || !strcmp(cls, "WorkerW");
            c.trayWnd  = !strcmp(cls, "Shell_TrayWnd") || !strcmp(cls, "Shell_SecondaryTrayWnd");
            c.terminal = !strcmp(cls, "CASCADIA_HOSTING_WINDOW_CLASS") || !strcmp(cls, "ConsoleWindowClass") ||
                         !strcmp(cls, "mintty");
            c.own      = IsOwnProcessWindow(h);
            c.shell    = IsShellWindowUncached(h);
            return m_cache.emplace(h, c).first->second;
        }

        std::thread                        m_thread;
        std::atomic<bool>                  m_run{ false }, m_active{ false };
        std::atomic<HWND>                  m_gui{ nullptr };
        std::atomic<HMONITOR>              m_srMon{ nullptr };
        std::mutex                         m_mutex;
        std::shared_ptr<const WinSnapshot> m_latest;
        std::unordered_map<HWND, Cached>   m_cache;
        unsigned                           m_sweep = 0;
    };

    WindowWatcher g_winWatch;   // (one per process; started in WinMain)

    void UpdateTaskbarCutout(AppState& app, bool force)
    {
        auto clearCut = [&]() {
            app.recentUiCuts.clear();
            app.keepHistory.clear();
            app.occHistory.clear();
            if (!app.taskbarCutActive) return;
            if (app.renderer.IsDComp()) app.renderer.SetVisibleAll();
            else { SetWindowRgn(app.hwnd, nullptr, TRUE); app.overlayRgnValid = false; }
            app.taskbarCutActive = false;
            app.weaveCuts.clear();
            app.weaveCutRad.clear();
        };
        if (!IsTopmostWeaveMode(app)) { clearCut(); g_winWatch.SetActive(false); return; }
        // The window list comes from the background watcher (fresh within
        // ~2 ms); nothing below walks the windows itself.
        g_winWatch.SetContext(app.gui.Hwnd(), SrMonitor(app));
        g_winWatch.SetActive(true);
        const std::shared_ptr<const WinSnapshot> snap = g_winWatch.Latest();
        if (!snap) return;   // (first moments: no snapshot yet)

        RECT wr{};
        if (!GetWindowRect(app.hwnd, &wr)) return;
        const HMONITOR mon = SrMonitor(app);
        // Holes (screen coords) and their corner radii.
        std::vector<RECT> cuts;
        std::vector<int>  cutRad;
        auto addCut = [&](const RECT& r, int rad) { cuts.push_back(r); cutRad.push_back(rad); };
        RECT cut{};

        // Taskbar. (Not over the media viewer in Fullscreen: a picture or video
        // of SR Loom's own fills the screen, as any player's does -- there is
        // no desktop under it for the taskbar to belong to.)
        const bool ownMediaFullscreen = app.source == SourceKind::TestImage && app.mode == OutputMode::Fullscreen;
        HWND tb = ownMediaFullscreen ? nullptr : SrTaskbar(mon);
        RECT tr{};
        if (tb && GetWindowRect(tb, &tr))
        {
            // A fullscreen app above the taskbar hides it -- weave all of it.
            // Judged by z-order, not focus: clicking into a window on another
            // display leaves the fullscreen app still covering the taskbar.
            bool fsInFront = false;
            std::vector<RECT> overTaskbar;   // (windows in front of it: they're woven, not cut)
            for (const WinInfo& w : snap->z)
            {
                if (w.h == tb || w.desktop) break;
                if (w.own || w.shell) continue;
                // (Click-through overlays and tool windows aren't apps covering it.)
                if (w.ex & (WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW)) continue;
                if (w.fullscreenOnSr) { fsInFront = true; break; }
                RECT o{};
                if (IntersectRect(&o, &w.frame, &tr)) overTaskbar.push_back(o);
            }
            // Auto-hide leaves only a sliver on-screen; the intersection with
            // our window handles that (and a Looking Glass not over it).
            if (!fsInFront && IntersectRect(&cut, &tr, &wr))
            {
                if (overTaskbar.empty()) addCut(cut, 0);
                else
                {
                    // A window in front of the taskbar (dragged over it, or
                    // always-on-top) is 3D there: cut only the taskbar's
                    // visible pieces.
                    HRGN vis = CreateRectRgnIndirect(&cut);
                    for (const RECT& a : overTaskbar)
                    {
                        HRGN ar = CreateRectRgnIndirect(&a);
                        CombineRgn(vis, vis, ar, RGN_DIFF);
                        DeleteObject(ar);
                    }
                    if (const DWORD bytes = GetRegionData(vis, 0, nullptr))
                    {
                        std::vector<char> buf(bytes);
                        auto* rd = reinterpret_cast<RGNDATA*>(buf.data());
                        if (GetRegionData(vis, bytes, rd))
                        {
                            const RECT* rs = reinterpret_cast<const RECT*>(rd->Buffer);
                            for (DWORD i = 0; i < rd->rdh.nCount; ++i) addCut(rs[i], 0);
                        }
                    }
                    DeleteObject(vis);
                }
            }
        }

        // Holes linger a little after their window moves on or goes away:
        // the capture runs a frame or two behind, so closing a hole at once
        // would weave a ghost of the window's last position (the "trail").
        const DWORD now = GetTickCount();
        auto linger = [&](std::vector<AppState::LingerCut>& list, const RECT& r, int rad) {
            for (auto& c : list)
                if (EqualRect(&c.r, &r)) { c.t = now; c.rad = rad; return; }
            list.push_back({ r, now, rad });
        };
        auto expire = [&](std::vector<AppState::LingerCut>& list, DWORD ms) {
            list.erase(std::remove_if(list.begin(), list.end(),
                                      [&](const AppState::LingerCut& c) { return now - c.t > ms; }),
                       list.end());
        };
        // Where things were when the displayed capture was taken: a short
        // history of hole sets with (QPC, 100 ns) timestamps, looked up at the
        // capture frame's own timestamp.
        using TimedCuts = AppState::TimedCuts;
        LARGE_INTEGER qpc{}, qpf{};
        QueryPerformanceCounter(&qpc);
        QueryPerformanceFrequency(&qpf);
        const int64_t now100 = (int64_t)((double)qpc.QuadPart * 1.0e7 / (double)qpf.QuadPart);
        const int64_t capT   = app.capture.IsActive() ? app.capture.LastFrameTime100ns() : 0;
        auto recordHist = [&](std::deque<TimedCuts>& h, TimedCuts cur) {
            cur.t = snap->t100;   // (when the window list was taken)
            h.push_back(std::move(cur));
            while (h.size() > 2 && h[1].t < now100 - 5'000'000) h.pop_front();   // keep ~0.5 s
        };
        auto atCapture = [&](const std::deque<TimedCuts>& h) -> const TimedCuts* {
            if (h.empty()) return nullptr;
            if (capT <= 0) return h.size() >= 2 ? &h[h.size() - 2] : &h.back();   // unknown: one frame back
            const TimedCuts* best = &h.front();
            for (const TimedCuts& e : h) { if (e.t <= capT) best = &e; else break; }
            return best;
        };

        // System UI above us (higher z-band). Walk up the z-order from our
        // window; everything in a higher band is before us in the list.
        for (const WinInfo& w : snap->z)
        {
            if (w.h == app.hwnd || w.desktop) break;   // (everything above our window in z-order)
            if (w.own || w.band <= 1) continue;          // desktop band (or unknown): not system UI
            // (w.content: what it shows -- pop-ups like the tray overflow have
            // wide invisible shadow / resize margins that would show as a 2D ring.)
            if (!IntersectRect(&cut, &w.content, &wr)) continue;
            linger(app.recentUiCuts, cut, w.radius);
        }
        expire(app.recentUiCuts, 200);
        for (const auto& c : app.recentUiCuts) addCut(c.r, c.rad);

        // Windows that always stay 2D (our panel, terminals): cut out the
        // part of each that's actually visible (not under another window),
        // with its rounded corners -- where it is NOW (the live window shows
        // through) and where it was when the displayed capture was taken
        // (the captured picture still shows it there; left woven, that's a
        // ghost trailing a moving window). Nothing more: no extra trail.
        {
            std::map<HWND, TimedCuts> seen;
            std::vector<RECT> above;
            for (const WinInfo& w : snap->z)
            {
                if (w.desktop) break;   // the desktop
                if (w.h == app.hwnd || (w.ex & WS_EX_TRANSPARENT)) continue;
                const HWND h = w.h;
                if (!IntersectRect(&cut, &w.frame, &wr)) continue;
                if (w.keep2D)
                {
                    TimedCuts& tc = seen[h];
                    bool covered = false;
                    for (const RECT& a : above) { RECT i{}; if (IntersectRect(&i, &a, &cut)) { covered = true; break; } }
                    if (!covered) { tc.r.push_back(cut); tc.rad.push_back(w.radius); }   // whole, rounded
                    else
                    {
                        // Partly under other windows: just the visible pieces.
                        HRGN vis = CreateRectRgnIndirect(&cut);
                        for (const RECT& a : above)
                        {
                            HRGN ar = CreateRectRgnIndirect(&a);
                            CombineRgn(vis, vis, ar, RGN_DIFF);
                            DeleteObject(ar);
                        }
                        if (const DWORD bytes = GetRegionData(vis, 0, nullptr))
                        {
                            std::vector<char> buf(bytes);
                            auto* rd = reinterpret_cast<RGNDATA*>(buf.data());
                            if (GetRegionData(vis, bytes, rd))
                            {
                                const RECT* rs = reinterpret_cast<const RECT*>(rd->Buffer);
                                for (DWORD i = 0; i < rd->rdh.nCount; ++i) { tc.r.push_back(rs[i]); tc.rad.push_back(0); }
                            }
                        }
                        DeleteObject(vis);
                    }
                }
                above.push_back(cut);
            }
            // Windows gone since last time get an empty entry (they may still
            // be in the displayed capture for a moment).
            for (auto& kv : app.keepHistory)
                if (!seen.count(kv.first)) seen[kv.first];
            for (auto& kv : seen) recordHist(app.keepHistory[kv.first], std::move(kv.second));
            for (auto it = app.keepHistory.begin(); it != app.keepHistory.end();)
            {
                const TimedCuts* nowE = &it->second.back();
                const TimedCuts* capE = atCapture(it->second);
                for (const TimedCuts* e : { nowE, capE })
                    if (e) for (size_t i = 0; i < e->r.size(); ++i) addCut(e->r[i], e->rad[i]);
                // Gone, and no longer in the capture either: forget it.
                if (nowE->r.empty() && (!capE || capE->r.empty())) it = app.keepHistory.erase(it);
                else ++it;
            }
        }
        // (Duplicates -- a window that hasn't moved -- are harmless but
        // would defeat the "nothing changed" check: drop them.)
        for (size_t i = 0; i < cuts.size(); ++i)
            for (size_t j = cuts.size(); j-- > i + 1;)
                if (EqualRect(&cuts[i], &cuts[j]) && cutRad[i] == cutRad[j])
                {
                    cuts.erase(cuts.begin() + j);
                    cutRad.erase(cutRad.begin() + j);
                }

        // Auto Stereo always manages the region (its base is the region set,
        // not the whole window), even with no system-UI holes to cut.
        if (cuts.empty() && !app.autoStereo) { clearCut(); return; }

        auto sameCuts = [&]() {
            if (cuts.size() != app.weaveCuts.size()) return false;
            for (size_t i = 0; i < cuts.size(); ++i)
                if (!EqualRect(&cuts[i], &app.weaveCuts[i]) || cutRad[i] != app.weaveCutRad[i]) return false;
            return true;
        };
        if (!force && app.taskbarCutActive && sameCuts() && EqualRect(&wr, &app.taskbarCutWindow) &&
            !(app.autoStereo && app.renderer.IsDComp()))   // (Auto Stereo mask: every frame, it's cheap)
            return;

        auto commit = [&]() {
            app.taskbarCutActive = true;
            app.weaveCuts        = std::move(cuts);
            app.weaveCutRad      = std::move(cutRad);
            app.taskbarCutWindow = wr;
        };

        // DirectComposition: the see-through mask, drawn into the next frame
        // together with the picture (in sync; no window reshaping). Coords
        // are the back buffer's (client area).
        if (app.renderer.IsDComp())
        {
            POINT co{ 0, 0 };
            ClientToScreen(app.hwnd, &co);
            auto toClient = [&](RECT s) { OffsetRect(&s, -co.x, -co.y); return s; };
            std::vector<Renderer::MaskCut> excl;
            for (size_t i = 0; i < cuts.size(); ++i) excl.push_back({ toClient(cuts[i]), cutRad[i] });
            if (app.autoStereo)
            {
                // Per picture, moved on the GPU by its tracked offset in the
                // same frame as the picture, minus windows in front of it.
                std::vector<Renderer::MaskTracked> tracked;
                TimedCuts occNow;
                for (const WeaveRegion& r : app.regionWeaver.Regions())
                {
                    Renderer::MaskTracked mt;
                    RECT fr = (r.gpuSlot < 0 && !IsRectEmpty(&r.vis)) ? r.vis : r.frame;
                    OffsetRect(&fr, r.lead.x, r.lead.y);
                    mt.rect  = toClient(FrameToScreen(app, fr));
                    mt.clip  = IsRectEmpty(&r.clip) ? RECT{ -100000, -100000, 100000, 100000 }
                                                    : toClient(FrameToScreen(app, r.clip));
                    mt.slot  = r.gpuSlot;
                    mt.scale = (float)ScreenAnalyzer::Scale();
                    tracked.push_back(mt);
                    auto oc = app.regionOcc.find(r.id);
                    if (oc != app.regionOcc.end())
                        for (const RECT& o : oc->second) { occNow.r.push_back(o); occNow.rad.push_back(0); occNow.owner.push_back(r.id); }
                }
                // Windows in front of pictures: cut where they are now AND
                // where they were when the displayed capture was taken (the
                // captured picture still shows them there).
                recordHist(app.occHistory, std::move(occNow));
                const TimedCuts* occNowE = &app.occHistory.back();
                // Each such cut hides only its own picture (the renderer's
                // owner = that picture's index in `tracked`): the window in
                // front of a browser's pictures may hold a picture of its own.
                std::map<int, int> trackedIndex;
                {
                    int ti = 0;
                    for (const WeaveRegion& r : app.regionWeaver.Regions()) trackedIndex[r.id] = ti++;
                }
                for (const TimedCuts* e : { occNowE, atCapture(app.occHistory) })
                    if (e) for (size_t i = 0; i < e->r.size(); ++i)
                    {
                        const int id = i < e->owner.size() ? e->owner[i] : -1;
                        auto ti = trackedIndex.find(id);
                        if (id >= 0 && ti == trackedIndex.end()) continue;   // (that picture is gone)
                        excl.push_back({ toClient(e->r[i]), e->rad[i], id >= 0 ? ti->second : -1 });
                    }
                app.renderer.SetVisibleTracked(tracked, excl, app.gpuTracking ? app.gpuTracker.ResultsSRV() : nullptr);
            }
            else
                app.renderer.SetVisibleAllExcept(excl);
            commit();
            return;
        }

        // Classic presenter: reshape the window. Base shape: the whole weave
        // window normally; in Auto Stereo, just the woven regions (empty =
        // nothing shown). The holes are then subtracted from either.
        HRGN rgn = nullptr;
        if (app.autoStereo)
        {
            rgn = CreateRectRgn(0, 0, 0, 0);
            for (const WeaveRegion& r : app.regionWeaver.Regions())
            {
                // Just the visible part of the image (not what's scrolled
                // under a toolbar / out of its window), where it's drawn
                // (shifted by its scroll lead, kept inside its viewport) ...
                RECT fv = IsRectEmpty(&r.vis) ? r.frame : r.vis;
                OffsetRect(&fv, r.lead.x, r.lead.y);
                if (!IsRectEmpty(&r.clip) && !IntersectRect(&fv, &fv, &r.clip)) continue;
                RECT s = FrameToScreen(app, fv);
                if (IsRectEmpty(&s)) continue;
                HRGN rr = CreateRectRgn(s.left - wr.left, s.top - wr.top,
                                        s.right - wr.left, s.bottom - wr.top);
                // ... minus windows in front of its window.
                auto oc = app.regionOcc.find(r.id);
                if (oc != app.regionOcc.end())
                    for (const RECT& o : oc->second)
                    {
                        HRGN hole = CreateRectRgn(o.left - wr.left, o.top - wr.top,
                                                  o.right - wr.left, o.bottom - wr.top);
                        CombineRgn(rr, rr, hole, RGN_DIFF);
                        DeleteObject(hole);
                    }
                CombineRgn(rgn, rgn, rr, RGN_OR);
                DeleteObject(rr);
            }
        }
        else
            rgn = CreateRectRgn(0, 0, wr.right - wr.left, wr.bottom - wr.top);
        for (size_t i = 0; i < cuts.size(); ++i)
        {
            const RECT& c = cuts[i];
            const int d = 2 * cutRad[i];
            HRGN hole = d > 0 ? CreateRoundRectRgn(c.left - wr.left, c.top - wr.top,
                                                   c.right - wr.left + 1, c.bottom - wr.top + 1, d, d)
                              : CreateRectRgn(c.left - wr.left, c.top - wr.top,
                                              c.right - wr.left, c.bottom - wr.top);
            CombineRgn(rgn, rgn, hole, RGN_DIFF);
            DeleteObject(hole);
        }
        SetWindowRgn(app.hwnd, rgn, TRUE);   // the system owns rgn from here
        app.overlayRgnValid = false;
        commit();
    }

    // Put the weave back on top if a fullscreen app/game on the SR display
    // has pushed itself above it. Only fullscreen-sized, non-shell windows
    // count, so Start, flyouts, tray menus, tooltips and SR Loom's own panel
    // can still appear above the weave as normal.
    void KeepWeaveAboveFullscreenApps(AppState& app)
    {
        if (!IsTopmostWeaveMode(app)) return;
        const HMONITOR mon = SrMonitor(app);
        for (HWND h = GetWindow(app.hwnd, GW_HWNDPREV); h; h = GetWindow(h, GW_HWNDPREV))
        {
            if (!IsWindowVisible(h) || IsCloaked(h) || IsOwnProcessWindow(h)) continue;
            if (MonitorFromWindow(h, MONITOR_DEFAULTTONULL) != mon || !IsWindowFullscreen(h)) continue;
            if (IsShellWindow(h)) continue;
            // Full-screen TRANSPARENT overlays are not games: NVIDIA's overlay
            // (class CEF-OSC-WIDGET, NVIDIA Share.exe / NVIDIA Overlay.exe --
            // permanently up, WS_EX_LAYERED | NOACTIVATE | TOOLWINDOW),
            // Discord's 2025+ game-glued overlay, and similar. They're per-
            // pixel transparent, so sitting below them is harmless, and their
            // widgets / FPS counters then stay 2D. Pushing above them every
            // 250ms would just ping-pong the z-order forever. Real game
            // windows don't use click-through / tool-window layered styles.
            {
                const LONG_PTR ex = GetWindowLongPtr(h, GWL_EXSTYLE);
                if (ex & WS_EX_TRANSPARENT) continue;
                if ((ex & WS_EX_LAYERED) && (ex & (WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW))) continue;
                char cls[64] = {};
                GetClassNameA(h, cls, (int)sizeof(cls));
                if (!strcmp(cls, "CEF-OSC-WIDGET")) continue;
            }
            static DWORD s_lastLogMs = 0;
            if (GetTickCount() - s_lastLogMs > 5000)
            {
                s_lastLogMs = GetTickCount();
                Log("Weave: fullscreen window %p ('%s') went above the weave -- re-asserting topmost",
                    (void*)h, ForegroundExeBaseName(h).c_str());
            }
            SetWindowPos(app.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            return;
        }
    }

    // "Save current window as profile" tray action. Captures the
    // foreground window's exe + title and the current SR Loom stereo
    // settings; persists. If a profile with the same name (= exe by
    // default, or window title if empty) already exists, overwrites it.
    void SaveCurrentAsProfile(AppState& app)
    {
        // Prefer the cached "last non-self foreground" -- when the user
        // clicks the GUI button, the SR Loom panel itself is the live
        // foreground, so GetForegroundWindow() would return our own
        // window and we'd save "SRLoom.exe" as the profile. The hook
        // cache holds the last external window the user focused.
        HWND fg = app.lastExternalForeground;
        if (!fg || !IsWindow(fg))
        {
            HWND cur = GetForegroundWindow();
            if (cur && cur != app.hwnd) fg = cur;
        }
        if (!fg || fg == app.hwnd) { Log("Profiles: no external window to save"); return; }
        const std::string exe = ForegroundExeBaseName(fg);
        const std::string title = WindowTitle(fg);
        if (exe.empty() && title.empty()) return;
        Profile p;
        // Prefer the window title as the profile's human-readable name
        // (matches what shows up in Windows' alt-tab); fall back to the
        // exe if the title is empty or has weird chars.
        p.name = !title.empty() ? title : exe;
        p.exe          = exe;          // match by exe; usually enough
        p.title        = "";           // empty title pattern = any title for that exe
        p.format       = app.format;
        p.swapEyes     = app.swapEyes;
        p.convergence  = app.convergence;
        // Capture every format-specific sub-option so re-applying the
        // profile later restores anaglyph/pulfrich/frame-pack detail
        // (e.g. red/cyan vs green/magenta, recovered vs filtered decode).
        p.anaglyphCombo  = app.anaglyphCombo;
        p.anaglyphMode   = app.anaglyphMode;
        p.pulfrichMode   = (int)app.pulfrichMode;
        p.pulfrichDelay  = app.pulfrichDelay;
        p.pulfrichNd     = app.pulfrichNd;
        p.framePackMode  = app.framePackMode;
        p.quiltCols      = app.quiltCols;
        p.quiltRows      = app.quiltRows;
        p.quiltLeftIdx   = app.quiltLeftIdx;
        p.quiltRightIdx  = app.quiltRightIdx;
        // Head-tracking snapshot: stored but only applied later when the
        // user explicitly flags includeHeadTracking on this profile
        // (per-row toggle in the PROFILES list). Default false so a fresh
        // profile leaves the global HT state alone.
        p.includeHeadTracking = false;
        p.htOpenTrack   = app.openTrack.IsOpenTrackEnabled();
        p.htFreeTrack   = app.openTrack.IsFreeTrackEnabled();
        p.htTrackIR     = app.openTrack.IsTrackIREnabled();
        const auto cfg  = app.openTrack.GetConfig();
        p.htOutputMode  = cfg.outputMode;
        p.htInvertX     = cfg.invertX;
        p.htInvertY     = cfg.invertY;
        p.htInvertZ     = cfg.invertZ;
        p.htInvertYaw   = cfg.invertYaw;
        p.htInvertPitch = cfg.invertPitch;
        p.htInvertRoll  = cfg.invertRoll;
        // Replace-by-name so re-saving updates instead of duplicating.
        for (auto& existing : app.profiles)
        {
            if (existing.name == p.name) { existing = p; Profiles::Save(app.profiles); return; }
        }
        app.profiles.push_back(p);
        Profiles::Save(app.profiles);
        Log("Profiles: saved '%s' (%s)", p.name.c_str(), exe.c_str());
    }

    // Build a hash of the user-visible stereo state. Used by the media-
    // profile auto-save path to detect change with no need to mirror an
    // independent "last state" struct -- one cheap uint compare per frame.
    uint64_t HashStereoState(const AppState& app)
    {
        uint64_t h = 1469598103934665603ull;   // FNV-1a 64 seed
        auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
        mix((uint64_t)(int)app.format);
        mix((uint64_t)(app.swapEyes ? 1 : 0));
        mix((uint64_t)(int)(app.convergence * 1000.0f));
        mix((uint64_t)app.anaglyphCombo);
        mix((uint64_t)app.anaglyphMode);
        mix((uint64_t)(int)app.pulfrichMode);
        mix((uint64_t)app.pulfrichDelay);
        mix((uint64_t)app.pulfrichNd);
        mix((uint64_t)app.framePackMode);
        // Quilt grid + view picks -- without these in the hash, quilt
        // tweaks (cols/rows from filename token override, or the user
        // dragging the L/R view sliders) wouldn't trigger a save.
        mix((uint64_t)app.quiltCols);
        mix((uint64_t)app.quiltRows);
        mix((uint64_t)app.quiltLeftIdx);
        mix((uint64_t)app.quiltRightIdx);
        return h;
    }

    // Build a Profile snapshot of the live stereo state. Used by both
    // the explicit "save now" path (source-change-away, shutdown) and
    // the throttled auto-save below.
    Profile SnapshotStereoState(const AppState& app)
    {
        Profile p;
        p.format        = app.format;
        p.swapEyes      = app.swapEyes;
        p.convergence   = app.convergence;
        p.anaglyphCombo = app.anaglyphCombo;
        p.anaglyphMode  = app.anaglyphMode;
        p.pulfrichMode  = (int)app.pulfrichMode;
        p.pulfrichDelay = app.pulfrichDelay;
        p.pulfrichNd    = app.pulfrichNd;
        p.framePackMode = app.framePackMode;
        p.quiltCols     = app.quiltCols;
        p.quiltRows     = app.quiltRows;
        p.quiltLeftIdx  = app.quiltLeftIdx;
        p.quiltRightIdx = app.quiltRightIdx;
        return p;
    }

    // Force-flush any unsaved media-profile state right now. Called on
    // source-change-away (before clearing currentMediaPath) and on app
    // shutdown so the user's last tweaks aren't lost just because they
    // didn't sit on the slider for 250ms.
    void FlushMediaProfileIfDirty(AppState& app)
    {
        if (app.currentMediaPath.empty()) return;
        const uint64_t h = HashStereoState(app);
        if (h == app.lastMediaStateHash) return;
        MediaProfiles::SaveFor(app.currentMediaPath, SnapshotStereoState(app));
        app.lastMediaStateHash = h;
        app.lastMediaSaveMs    = GetTickCount();
    }

    // Called from the render loop. When a media file is the source,
    // detect any stereo-state change vs the last save and (throttled)
    // write the new state to media_profiles.ini for this file. Throttle
    // avoids hammering the INI on slider drags -- a settled value lands
    // within ~quarter of a second of the user releasing the mouse.
    void AutoSaveMediaProfileIfDirty(AppState& app)
    {
        if (app.currentMediaPath.empty()) return;
        if (app.source != SourceKind::TestImage)
        {
            // Source changed away from media (user picked Monitor / a
            // window / etc). FLUSH any unsaved state before clearing
            // tracking -- without this, last-second tweaks made just
            // before switching source were lost.
            FlushMediaProfileIfDirty(app);
            app.currentMediaPath.clear();
            app.lastMediaStateHash = 0;
            return;
        }
        const uint64_t h = HashStereoState(app);
        if (h == app.lastMediaStateHash) return;
        const DWORD now = GetTickCount();
        if (app.lastMediaSaveMs != 0 && (now - app.lastMediaSaveMs) < 250) return;
        MediaProfiles::SaveFor(app.currentMediaPath, SnapshotStereoState(app));
        app.lastMediaStateHash = h;
        app.lastMediaSaveMs    = now;
    }

    // SetWinEventHook callback. Hook runs out-of-context; Win32 forbids
    // non-trivial work here. We post to the main thread, which handles
    // the actual profile match + apply in WM_APP_FOREGROUND_CHANGED.
    void CALLBACK ForegroundEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                      LONG idObject, LONG idChild,
                                      DWORD, DWORD)
    {
        if (!g_app || !hwnd) return;
        if (event != EVENT_SYSTEM_FOREGROUND) return;
        if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
        // Skip our own windows so toggling our panel doesn't fire.
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId()) return;
        // Cache the latest non-self foreground so "Save current as profile"
        // can fetch the right window even when our panel is the live
        // foreground at button-click time. Atomic-safe enough at HWND
        // granularity -- it's a pointer-sized store on x64.
        g_app->lastExternalForeground = hwnd;
        PostMessage(g_app->hwnd, WM_APP_FOREGROUND_CHANGED, 0, (LPARAM)hwnd);
    }

    // Install / uninstall the global EVENT_SYSTEM_FOREGROUND hook based on
    // app.profilesAutoApply. Idempotent. Called once at startup and again
    // whenever the user flips "Enable Profiles". The hook has a non-zero
    // system cost (per-process delivery thread + DWM bookkeeping) so we
    // skip it entirely when auto-apply is off.
    void RefreshForegroundHook(AppState& app)
    {
        const bool wantHook = app.profilesAutoApply;
        if (wantHook && !app.fgHook)
        {
            app.fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                         nullptr, ForegroundEventProc, 0, 0,
                                         WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
            Log("RefreshForegroundHook: installed hook=%p", (void*)app.fgHook);
        }
        else if (!wantHook && app.fgHook)
        {
            UnhookWinEvent(app.fgHook);
            app.fgHook = nullptr;
            Log("RefreshForegroundHook: removed hook");
        }
    }

    // Keep the overlay aligned with the tracked source window each frame.
    void UpdateOverlayTracking(AppState& app)
    {
        if (app.mode != OutputMode::WindowOverlay || app.modeApplyDeferred)
            return;   // (deferred: window still has the previous mode's shape)

        HWND src = app.sourceWindow;
        if (!src || !IsWindow(src))
        {
            // The captured window has closed. Don't keep weaving its last frame —
            // tear the overlay down and go idle (turns the lens off too), so the
            // stale 3D image disappears without having to quit SR Loom.
            app.capture.Stop();
            app.sourceWindow = nullptr;
            app.source = SourceKind::CaptureMonitor;   // sane default for the next enable
            SetWeaving(app, false);
            return;
        }

        if (IsIconic(src) || !IsWindowVisible(src))
        {
            if (IsWindowVisible(app.hwnd)) ShowWindow(app.hwnd, SW_HIDE);
            return;
        }

        RECT r = VisibleWindowRect(src), cur{};
        GetWindowRect(app.hwnd, &cur);
        const bool rectChanged = !EqualRect(&r, &cur);

        // Pin the overlay DIRECTLY ABOVE the source in the z-order (not global
        // topmost), so windows the user alt-tabs to occlude the weave. Only re-pin
        // when the source has risen above us (e.g. the game was just clicked/focused),
        // not every frame — that avoids constant z-order churn and flicker.
        const HWND above = GetWindow(src, GW_HWNDPREV);   // window directly above src
        const bool zBad  = (above != app.hwnd);           // overlay isn't sitting above src

        if (rectChanged || zBad)
        {
            UINT flags = SWP_NOACTIVATE | SWP_SHOWWINDOW;   // WM_SIZE resizes the swap chain
            HWND insertAfter = HWND_TOP;
            if (zBad)
                insertAfter = above ? above : HWND_TOP;     // slot just above src
            else
                flags |= SWP_NOZORDER;                      // z is fine; only move/resize
            SetWindowPos(app.hwnd, insertAfter, r.left, r.top,
                         r.right - r.left, r.bottom - r.top, flags);
        }
        if (!IsWindowVisible(app.hwnd))
            ShowWindow(app.hwnd, SW_SHOWNOACTIVATE);

        // Match the source window's Windows-11 rounded corners. WGC captures
        // the source as a rectangular pixel grid (the OS only rounds the
        // display shape via DWM compositing, not the bitmap), so our opaque
        // overlay shows the captured corner pixels as solid black squares
        // by default. SetWindowRgn clips our overlay to a rounded-rect so
        // the desktop shows through there -- matches what the user expects.
        // Recompute only when the source's rect / corner-style changes; the
        // OS reuses an identical region as a no-op.
        DWM_WINDOW_CORNER_PREFERENCE srcCornerPref = DWMWCP_DEFAULT;
        DwmGetWindowAttribute(src, DWMWA_WINDOW_CORNER_PREFERENCE,
                              &srcCornerPref, sizeof(srcCornerPref));
        const bool wantRound = (srcCornerPref != DWMWCP_DONOTROUND);
        const int  w         = r.right - r.left;
        const int  h         = r.bottom - r.top;
        // (Only when the shape changes: SetWindowRgn waits on the compositor,
        // 15+ ms at times -- it was being called every frame.)
        static int s_rgnW = -1, s_rgnH = -1, s_rgnPref = -1;
        static UINT s_rgnDpi = 0;
        const UINT srcDpi = GetDpiForWindow(src);
        if (w == s_rgnW && h == s_rgnH && (int)srcCornerPref == s_rgnPref && srcDpi == s_rgnDpi &&
            app.overlayRgnValid)
            return;
        s_rgnW = w; s_rgnH = h; s_rgnPref = (int)srcCornerPref; s_rgnDpi = srcDpi;
        app.overlayRgnValid = true;
        if (wantRound && w > 0 && h > 0)
        {
            // Win11 default rounded radius is 8 DIPs; ROUNDSMALL is 4 DIPs.
            // Scale by the source window's DPI so the overlay matches what
            // DWM actually drew, not what 100% scaling would have looked like.
            const UINT dpi    = GetDpiForWindow(src);
            const int  radDip = (srcCornerPref == DWMWCP_ROUNDSMALL) ? 4 : 8;
            const int  rad    = radDip * (int)dpi / 96;
            // CreateRoundRectRgn's ellipse args are *diameters*, not radii.
            HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, rad * 2, rad * 2);
            // SetWindowRgn takes ownership of rgn -- don't DeleteObject.
            SetWindowRgn(app.hwnd, rgn, FALSE);
        }
        else
        {
            // Source has rounding disabled (DWMWCP_DONOTROUND) -- clear our
            // region so the overlay is a plain rectangle again.
            SetWindowRgn(app.hwnd, nullptr, FALSE);
        }
    }

    void UsePassthrough(AppState& app);   // fwd decl: default weave is screen passthrough

    // Velocity-aware Accela filter (one axis). Stage 2 of the VR head-
    // tracking chain (stage 1 = OneEuro). Piecewise gain curve takes
    // raw deltas to a follow-rate -- small inputs ride a near-zero gain
    // (soft damping of tracker noise), real head turns hit the steep
    // part and snap. We DON'T use Accela's deadzone here because the
    // hard "no movement below threshold" cutoff caused visible jumps
    // when input crossed the boundary. The gain curve's own first
    // segment provides the damping smoothly.
    double AccelaApply(AppState::AccelaAxis& a, double raw, double threshold)
    {
        static const struct { double x, y; } kGains[] = {
            { 0.0, 0.0 }, { 0.5, 0.4  }, { 1.0, 1.5   }, { 1.5, 8.0   },
            { 2.5, 35.0 }, { 5.0, 100.0 }, { 8.0, 200.0 }, { 9.0, 300.0 }
        };
        constexpr int kN = (int)(sizeof(kGains) / sizeof(kGains[0]));

        const DWORD now = GetTickCount();
        if (!a.init) { a.init = true; a.lastOutput = raw; a.lastTickMs = now; return raw; }
        double dt = (now - a.lastTickMs) * 0.001;
        a.lastTickMs = now;
        if (dt < 1e-5) dt = 1e-5;
        if (dt > 0.25) dt = 0.25;

        const double delta    = raw - a.lastOutput;
        const double absDelta = fabs(delta);
        const double normalized = absDelta / (threshold > 1e-9 ? threshold : 1e-9);

        double gain = kGains[kN - 1].y;
        if (normalized <= kGains[0].x) gain = kGains[0].y;
        else
            for (int i = 1; i < kN; ++i)
                if (normalized <= kGains[i].x)
                {
                    const double t = (normalized - kGains[i - 1].x) / (kGains[i].x - kGains[i - 1].x);
                    gain = kGains[i - 1].y + (kGains[i].y - kGains[i - 1].y) * t;
                    break;
                }

        double alpha = (absDelta > 1e-9) ? (dt * gain / absDelta) : 0.0;
        if (alpha > 1.0) alpha = 1.0;
        if (alpha < 0.0) alpha = 0.0;
        a.lastOutput += alpha * delta;
        return a.lastOutput;
    }

    // Starts the SR session with the weaver chosen in Settings. The DX12 choice
    // first swaps the presenter for its Direct3D 12 one (same window, same
    // pacing) and has the converter make its output shareable, so the weave
    // reads it without a copy; when that presenter or its weaver can't be
    // made, the Direct3D 11 presenter and weaver as before.
    // HDR: is the 16-bit float chain (capture, conversion, output) to be used now?
    // Settings HdrOutput: 1 (default) when Windows has HDR switched on for the SR
    // display, 2 always, 0 never. Only for the layouts that pass the picture's
    // values straight through (side-by-side, top-and-bottom, interleaved,
    // checkerboard): brighter-than-white picture is kept. The others (the
    // anaglyph modes and the rest) work on 0..1 values and would come out
    // capped at 80 nits, dimmer than the desktop -- they stay on the 8-bit chain,
    // which Windows shows at its own SDR brightness. Not with Automatic
    // detection or Auto Stereo, which read the screen as 8-bit.
    bool HdrWanted(AppState& app, bool displayHdr)
    {
        const int set = Settings::ReadHdrOutput();
        if (!(set == 2 || (set == 1 && displayHdr)) || !app.renderer.IsDComp()) return false;
        if (app.autoInput || app.autoStereo) return false;
        switch (app.format)
        {
        case StereoFormat::FullSBS: case StereoFormat::HalfSBS: case StereoFormat::FullTAB: case StereoFormat::HalfTAB:
        case StereoFormat::RowInterleaved: case StereoFormat::ColumnInterleaved: case StereoFormat::Checkerboard:
            return true;
        default:
            return false;
        }
    }

    // The untracked light field needs no weaver and no tracking: the session is
    // then the lens alone, and the eye-tracking camera stays off. (With Distance
    // From Camera the tracker is wanted, so the ordinary session.)
    bool LensOnlyWanted(AppState& app)
    {
        return app.lfOn && (app.format == StereoFormat::Quilt || app.format == StereoFormat::RGBD) && !app.lfFollow && !app.lfCentre;
    }

    bool StartSRSession(AppState& app)
    {
        if (app.weaver.HasWeaver()) return true;
        const bool lensOnly = LensOnlyWanted(app);
        app.weaver.SetLensOnly(lensOnly);
        const bool want12 = !lensOnly && Settings::ReadWeaverChoice() >= 4 && app.renderer.IsDComp();   // (4 the modern Direct3D 12 weaver; lens only draws with Direct3D 11)
        float hdrNits = 0.0f;
        const bool hdrOn = app.renderer.DisplayIsHdr(&hdrNits);
        const bool hdrOut = HdrWanted(app, hdrOn);
        app.hdrDisplay = hdrOn; app.hdrActive = hdrOut;
        const bool have12 = want12 && app.renderer.SetDX12(true, hdrOut);
        {
            Log("SR display: HDR %s in Windows (peak %.0f nits as reported)%s", hdrOn ? "ON" : "off", hdrNits,
                hdrOut ? " -- 16-bit float output chosen (Settings HdrOutput)" : "");
        }
        if (!have12 && app.renderer.IsDX12()) app.renderer.SetDX12(false);
        if (!have12) app.renderer.SetHdrOutput(hdrOut);   // (the Direct3D 11 presenter's)
        app.weaver.SetPresenter12(have12 ? app.renderer.DX12() : nullptr);
        const bool ok = app.weaver.StartSR(app.renderer.Context(), app.hwnd);
        if (have12 && !app.weaver.IsDX12())
        {
            app.weaver.SetPresenter12(nullptr);
            app.renderer.SetDX12(false);
        }
        if (want12) Log("Weaver: DX12 chosen -- %s", app.weaver.IsDX12() ? "Direct3D 12 presenter and weaver" : "not available, using Direct3D 11");
        app.converter.SetShareableOutput(app.weaver.IsDX12());
        app.regionWeaver.SetShareable(app.weaver.IsDX12());
        // (HDR: the capture, the conversion and the output all 16-bit float.)
        app.capture.SetHdr(hdrOut);
        app.converter.SetHdr(hdrOut);
        app.captureRebind = true;   // (the weaver needs its input again)
        return ok;
    }

    void SetWeaving(AppState& app, bool enable)
    {
        app.weavingEnabled = enable;
        // Bump process priority while weaving so Windows' scheduler is less likely
        // to deschedule the render loop mid-frame -- tighter frame pacing, fewer
        // hitches when other apps (browser tab, antivirus etc.) spike. Restore to
        // normal when weaving off so we're not hogging cycles while idle in the tray.
        SetPriorityClass(GetCurrentProcess(),
                         enable ? HIGH_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
        if (enable)
        {
            app.srStopAtMs = 0;   // (a session kept alive after weaving stopped is used as it is)
            // Katanga arm (format = Katanga, no game publishing yet): leave the
            // SR session alive but ask SwitchableLensHint to disable the lens.
            // No StartSR/StopSR churn, no swap-chain recreation risk (that was
            // the original source of the post-Katanga black bug). The SR
            // session is always kept alive while weavingEnabled.
            const bool katangaArmed = (app.format == StereoFormat::Katanga
                                      && !app.katanga.IsReceiving());
            if (!app.weaver.HasWeaver())
            {
                StartSRSession(app);
                // (The SR display may have been moved -- e.g. the Windows display
                // arrangement changed -- since start-up asked.)
                RECT rc{};
                if (app.weaver.GetSRDisplayRect(rc) && !EqualRect(&rc, &app.srDisplayRect))
                {
                    Log("SR display moved: (%ld,%ld)-(%ld,%ld)", rc.left, rc.top, rc.right, rc.bottom);
                    app.srDisplayRect = rc;
                    app.renderer.SetVBlankMonitor(SrMonitor(app));
                    PaceForSrRefresh(app, true);
                }
                // (For the log only. Weaving below the source's own resolution
                // looked blurry -- every eye is kept at full resolution.)
                int vw = 0, vh = 0;
                if (app.weaver.GetRecommendedViewsSize(vw, vh))
                    Log("SR runtime: recommended views texture %dx%d", vw, vh);
            }
            if (katangaArmed) app.weaver.LensDisable();
            else              app.weaver.LensEnable();
            // Default action: weave the screen (fullscreen SBS passthrough). If the
            // chosen source is the monitor but capture isn't running yet, start it.
            if (app.source == SourceKind::CaptureMonitor && !app.capture.IsActive())
                UsePassthrough(app);
            // Bind the Katanga receiver if we're (re-)enabling weaving with
            // format == Katanga. Idempotent if already begun.
            if (app.format == StereoFormat::Katanga && !app.katanga.IsActive())
                app.katanga.Begin(app.renderer.Device());
            app.captureRebind = true;   // re-register the converter output
            ApplyMode(app);
            ShowWindow(app.hwnd, katangaArmed ? SW_HIDE : SW_SHOW);
            // Opportunistic Enable on weave-start, in case the initial
            // Enable at app launch failed (e.g. SR Platform service was
            // still spinning up). Skipped if the user explicitly disabled
            // tracking via the GUI toggle. Bridge owns its own SRContext,
            // so this neither creates nor depends on the weaver context.
            // We re-apply the launch-time output set (OT + FT, plus TIR if
            // NPClient was detected) so a startup-failed TrackIR can come
            // online once SR Platform is up.
            if (!app.openTrackUserDisabled && !app.openTrack.IsEnabled())
                app.openTrack.SetOutputs(true, true, !app.npClientDir.empty());
            UpdateInputChoice(app);   // Automatic Detection applies from the first frame
        }
        else
        {
            // OpenTrack stays running across weave-stop: tracking is an
            // always-on background service, the weave is the on-demand
            // foreground. The user's GUI toggle is the only thing that
            // turns the bridge off.
            EndAutoStereo(app, "weaving turned off");
            // Hide, and lens off now. The SR session (camera, weaver) is let go
            // a few seconds later (see kSrKeepAliveMs): tearing it down and
            // making it again blocked for 60-240 ms each way, so switching
            // weaving off and straight back on is instant this way.
            ShowWindow(app.hwnd, SW_HIDE);
            HideFsCtrlOverlay();
            app.weaver.LensDisable();
            app.srStopAtMs = GetTickCount64() + kSrKeepAliveMs;
        }
        app.tray.SetTooltip(enable
            ? "SR Loom — weaving\nLeft-click for panel"
            : "SR Loom — paused (SR off)\nLeft-click for panel");
    }

    // Selecting a source or format turns weaving ON if it isn't already (so the 3D
    // comes on automatically); if it already is, re-apply the current mode AND
    // restart SR if it's been torn down (e.g. switching back out of Katanga arm,
    // where the SR session was stopped while no game was publishing).
    void EnsureWeaving(AppState& app)
    {
        if (!app.weavingEnabled) { SetWeaving(app, true); return; }
        const bool katangaArmed = (app.format == StereoFormat::Katanga
                                  && !app.katanga.IsReceiving());
        if (!app.weaver.HasWeaver())
        {
            StartSRSession(app);
            app.captureRebind = true;
        }
        // Lens hint matches format state: lens off during Katanga arm
        // (cooperative -- the lens still actually engages if any other SR
        // app has it on); lens on for any active weave format.
        if (katangaArmed) app.weaver.LensDisable();
        else              app.weaver.LensEnable();
        ApplyMode(app);
        if (katangaArmed) ShowWindow(app.hwnd, SW_HIDE);
    }

    // Like EnsureWeaving, but for a stereo-format / decode-option change: it must NOT
    // re-apply the window mode. ApplyMode resizes + re-styles the window and toggles the
    // swap-chain model — in Looking Glass that shrinks the loupe and stalls the weave on
    // every format change. The new format is picked up via captureRebind in RenderFrame,
    // so we only need to switch weaving on if it was off.
    void EnsureWeavingFormatOnly(AppState& app)
    {
        if (!app.weavingEnabled) SetWeaving(app, true);
    }

    // Centralised format setter -- handles Katanga's lifecycle (it's the only
    // format that owns external state: the shared-texture receiver, plus an
    // arm-mode lens-off until a game starts publishing). All other formats
    // are a plain pixel-interpretation change in the converter; nothing to
    // tear down. Always sets captureRebind so the next frame re-registers
    // the SRV at the new format. Re-applies the window mode iff Katanga is
    // transitioning in or out -- WantsClickThrough's Fullscreen branch keys
    // off format==Katanga, so the swap-chain mode (flip vs bit-blt) must be
    // refreshed on those edges.
    void ChangeFormat(AppState& app, StereoFormat newFmt)
    {
        // Picking any other format while auto-receiving Katanga means the
        // user has taken over: don't auto-restore their old setup later.
        if (app.katangaAuto && newFmt != StereoFormat::Katanga)
            app.katangaAuto = false;
        const StereoFormat oldFmt = app.format;
        const bool katangaEdge = (oldFmt == StereoFormat::Katanga)
                              != (newFmt == StereoFormat::Katanga);
        app.format = newFmt;
        app.captureRebind = true;
        // Looking Glass format change. In passthrough (weaving the screen
        // beneath it) the loupe's content IS whatever is under the window,
        // so the format doesn't change its shape: leave it exactly as the
        // user placed and sized it. For an image/video file the content has
        // its own aspect (FullSBS 16:9 per-eye vs HalfSBS 8:9), so re-fit
        // the size -- in place (same top-left + height), never re-centred --
        // or the content ends up pillarboxed / squished.
        if (oldFmt != newFmt && app.mode == OutputMode::LookingGlass)
        {
            if (!app.loupeActive)                              ApplyMode(app);
            else if (app.source != SourceKind::CaptureMonitor) RefitLoupeKeepingPosition(app);
        }
        if (newFmt == StereoFormat::Katanga && oldFmt != StereoFormat::Katanga)
        {
            // Entering Katanga: start the receiver, arm-mode (lens off, our
            // window hidden) until a game publishes -- the render loop's
            // Katanga branch flips them back on at the first received frame.
            app.katanga.Begin(app.renderer.Device());
            app.weaver.LensDisable();
            ShowWindow(app.hwnd, SW_HIDE);
        }
        else if (oldFmt == StereoFormat::Katanga && newFmt != StereoFormat::Katanga)
        {
            // Leaving Katanga: stop the receiver, lens back on, show our
            // window. The format change below will trigger the converter
            // path with whichever Source is still bound.
            app.katanga.End();
            app.katangaPublisherWnd = nullptr;
            app.weaver.LensEnable();
            ShowWindow(app.hwnd, SW_SHOW);
            // Drop topmost (set by the render-loop receive transition).
            SetWindowPos(app.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        if (katangaEdge) ApplyMode(app);   // refresh layered/swap-chain state

        // LightField on-load means an LFP file is bound to the LFPRenderer.
        // If the user picks a different format while in LightField, the
        // user's effectively saying "stop using the loaded LFP" -- so
        // release the GPU resources for it. (Selecting LightField without
        // a loaded LFP is a no-op until the user loads one.)
        if (oldFmt == StereoFormat::LightField && newFmt != StereoFormat::LightField)
            app.lfpRenderer.Unload();
    }

    // Record the most recent "real" foreground window (not ours, the shell, or a
    // menu), so "Make active window 3D" can target it even though clicking the tray
    // menu changes the foreground. Called each loop iteration.
    void UpdateLastForeground(AppState& app)
    {
        HWND fg = GetForegroundWindow();
        if (!fg || fg == app.hwnd || !IsWindowVisible(fg))
            return;
        char cls[64] = {};
        GetClassNameA(fg, cls, (int)sizeof(cls));
        if (strcmp(cls, "SRWeaverWindow") == 0 || strcmp(cls, "SRWeaverGuiWindow") == 0 ||
            strcmp(cls, "Shell_TrayWnd") == 0 || strcmp(cls, "#32768") == 0)   // ours / shell / popup menu
            return;
        app.lastForeground = fg;
    }

    // --- Source selection -------------------------------------------------

    // VR180/VR360 and LightField only make sense for static / file-loaded
    // sources (equirect images / 360° video, Lytro plenoptic photos);
    // they're meaningless on a live screen-capture source. Called from
    // each capture-source setter to drop the user back to Half SBS if
    // they were on one of these formats when switching to a live source.
    // Also tears down the LFP renderer so its stale RT doesn't keep
    // shadowing the captured frames via the `if (lfpRenderer.HasData())`
    // branch in the main render loop.
    void DemoteVRFormatForLiveCapture(AppState& app)
    {
        if (IsVRFormat(app.format) || app.format == StereoFormat::LightField)
        {
            if (app.format == StereoFormat::LightField)
                app.lfpRenderer.Unload();
            ChangeFormat(app, StereoFormat::HalfSBS);
        }
    }

    // Capture a window and present it as an in-place 3D overlay tracking that window.
    void UseWindow(AppState& app, HWND target)
    {
        // Automatic Detection, and the window is on the SR display: Auto
        // Stereo limited to it -- its 3D pictures woven, the rest 2D -- rather
        // than weaving the whole window in one layout.
        if (app.autoInput && target && IsWindow(target) &&
            MonitorFromWindow(target, MONITOR_DEFAULTTONULL) == SrMonitor(app))
        {
            const HWND root = GetAncestor(target, GA_ROOT);
            if (root != app.autoScopeWindow)
            {
                app.autoScopeWindow = root;
                Log("Automatic Detection: 3D pictures in '%s' only", WindowTitle(root).c_str());
                std::vector<int> other;   // (found in other windows: not wanted now)
                for (const WeaveRegion& r : app.regionWeaver.Regions())
                    if (r.autoDetected && r.host != root) other.push_back(r.id);
                for (int id : other) RemoveAutoRegion(app, id, "outside the chosen window");
                app.scanPending.clear();
            }
            UsePassthrough(app);   // (the SR display's capture: Auto Stereo looks at the screen)
            const bool modeChanged = app.mode != OutputMode::Fullscreen;
            app.mode = OutputMode::Fullscreen;
            if (!app.weavingEnabled) SetWeaving(app, true);   // (applies Automatic Detection)
            else
            {
                // (From the Looking Glass: the window becomes the full-screen
                // weave again -- it kept the glass's shape before.)
                if (modeChanged) { app.captureRebind = true; ApplyMode(app); }
                UpdateInputChoice(app);
            }
            if (IsWindow(target)) SetForegroundWindow(target);
            return;
        }
        app.autoScopeWindow = nullptr;
        DemoteVRFormatForLiveCapture(app);
        app.capture.SetCaptureCursor(false);   // overlay sits on the source; real cursor shows through
        const bool wasWeaving = app.weavingEnabled;
        if (target && app.capture.StartWindow(target))
        {
            if (wasWeaving) BeginCaptureWarmup(app);
            app.source       = SourceKind::CaptureWindow;
            app.sourceWindow = target;
            app.captureRebind = true;   // re-register the SRV once frames arrive
            app.mode = OutputMode::WindowOverlay;
            EnsureWeaving(app);
            // Hand input focus back to the captured window. The overlay is NOACTIVATE
            // (it never takes focus), but the panel/menu we were clicked from did — so
            // without this the game sits in the background and ignores controller/key
            // input. We're the current foreground process here, so the OS lets us set
            // it. The overlay stays topmost, so the 3D remains visible over the window.
            if (IsWindow(target))
                SetForegroundWindow(target);
        }
    }

    // Make the active window 3D (tray item / Ctrl+Alt+C). GetForegroundWindow() works
    // for the hotkey, but the tray menu steals focus, so fall back to the last real
    // foreground window we tracked -> targets the user's window, not the test image.
    void CaptureForeground(AppState& app)
    {
        HWND fg = GetForegroundWindow();
        char cls[64] = {};
        if (fg) GetClassNameA(fg, cls, (int)sizeof(cls));
        const bool ours = !fg || fg == app.hwnd || !IsWindow(fg) ||
                          strcmp(cls, "SRWeaverWindow") == 0 || strcmp(cls, "SRWeaverGuiWindow") == 0 ||
                          strcmp(cls, "Shell_TrayWnd") == 0 || strcmp(cls, "#32768") == 0;
        if (ours)
            fg = app.lastForeground;   // we (GUI/menu) had focus; use the window active before us
        Log("CaptureForeground: target=%p app.hwnd=%p", (void*)fg, (void*)app.hwnd);
        if (fg && fg != app.hwnd && IsWindow(fg))
            UseWindow(app, fg);
    }

    // Start a passthrough capture of the SR display. What gets woven is the
    // region of that monitor beneath our (capture-excluded) window — full screen
    // in Fullscreen, or just the viewer's area in Windowed/LookingGlass.
    // The monitor the SR display itself lives on (the "this display" passthrough target).
    HMONITOR SrMonitor(const AppState& app)
    {
        const RECT& d = app.srDisplayRect;
        POINT center{ (d.left + d.right) / 2, (d.top + d.bottom) / 2 };
        return MonitorFromPoint(center, MONITOR_DEFAULTTOPRIMARY);
    }

    // Physical-to-pixel scale (mm per pixel, horizontal axis) for a given
    // monitor. Asks the OS via GetDeviceCaps(HORZSIZE / HORZRES). Returns 0
    // if it can't be determined; callers should treat 0 as "skip features
    // that need it" (head-position window-anchoring, primarily).
    double MonitorMmPerPx(HMONITOR mon)
    {
        if (!mon) return 0.0;
        MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(mon, &mi)) return 0.0;
        HDC hdc = CreateDCW(nullptr, mi.szDevice, nullptr, nullptr);
        if (!hdc) return 0.0;
        const int physWmm  = GetDeviceCaps(hdc, HORZSIZE);
        const int pixelW   = GetDeviceCaps(hdc, HORZRES);
        DeleteDC(hdc);
        if (physWmm <= 0 || pixelW <= 0) return 0.0;
        return (double)physWmm / (double)pixelW;
    }

    // Maximum refresh rate the given monitor reports support for, in Hz. Iterates
    // every display mode the OS lists and picks the highest dmDisplayFrequency.
    // Returns 0 on failure (e.g. monitor disconnected). Per-model: Samsung Odyssey
    // 3D reports 160-165 Hz, Acer SpatialLabs View Pro 27 reports 160 Hz, future
    // higher-refresh panels will scale automatically.
    double MaxMonitorRefreshHz(HMONITOR mon)
    {
        if (!mon) return 0.0;
        MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(mon, &mi)) return 0.0;
        double best = 0.0;
        DEVMODEW dm{}; dm.dmSize = sizeof(dm);
        for (DWORD i = 0; EnumDisplaySettingsW(mi.szDevice, i, &dm); ++i)
        {
            if ((double)dm.dmDisplayFrequency > best)
                best = (double)dm.dmDisplayFrequency;
        }
        return best;
    }

    void UsePassthrough(AppState& app)
    {
        DemoteVRFormatForLiveCapture(app);
        app.sourceWindow = nullptr;
        app.capture.SetCaptureCursor(false);   // same screen as the real cursor → don't double it
        HMONITOR mon = SrMonitor(app);
        // Already capturing the SR display? Keep the running session.
        // StartMonitor tears the WGC session down and rebuilds it, which
        // drops frames for a moment -- visible as a flicker every time the
        // panel's Fullscreen / Looking Glass buttons (or a profile) re-pick
        // the source that's already live.
        if (app.source == SourceKind::CaptureMonitor && !app.foreignDisplay &&
            app.sourceMonitor == mon && app.capture.IsActive())
            return;
        if (app.capture.StartMonitor(mon))
        {
            if (app.weavingEnabled) BeginCaptureWarmup(app);
            app.source = SourceKind::CaptureMonitor;
            app.sourceMonitor = mon;
            app.foreignDisplay = false;   // this IS the SR display → crop to the viewer region
            app.captureRebind = true;
        }
    }

    // stb_image symbols live in SRWeaver.cpp (STB_IMAGE_IMPLEMENTATION is set
    // there). Forward-declare just the two functions we need so we can load
    // pixel data here for image-content quilt-grid auto-detection.
    extern "C" unsigned char* stbi_load(char const* filename, int* x, int* y,
                                        int* channels_in_file, int desired_channels);
    extern "C" unsigned char* stbi_load_from_memory(unsigned char const* buffer,
                                                     int len, int* x, int* y,
                                                     int* channels_in_file,
                                                     int desired_channels);
    typedef unsigned char stbi_uc;
    extern "C" void stbi_image_free(void* retval_from_stbi_load);

    // Image-content quilt-grid auto-detect. Tries the canonical LG grids and
    // picks the one that best matches "looks like a tiled set of horizontally-
    // shifted views". Two-stage filter:
    //   (1) Strict-ish divisibility -- real LG quilts have w%cols and h%rows
    //       within a few px of zero. Reject loose fits.
    //   (2) Score = mean SAD(horizontally-adjacent cells) / mean SAD(diagonally-
    //       opposite cells). For the correct grid, horizontal-adjacent cells
    //       are almost the same scene (small parallax) and opposite-corner
    //       cells are different views (larger diff) -- ratio is small.
    //       For wrong grids, the cells span unrelated content and the ratio
    //       is close to 1. Normalising by the diagonal SAD divides out global
    //       image contrast, so the heuristic survives low- and high-contrast
    //       photos equally well.
    bool AutoDetectQuiltGrid(const char* path, int& outCols, int& outRows)
    {
        int w = 0, h = 0, ch = 0;
        unsigned char* pixels = stbi_load(path, &w, &h, &ch, 4);
        if (!pixels) return false;

        struct Cand { int c, r; };
        const Cand cands[] = {
            {8, 6},  {5, 9},  {4, 8},  {11, 8}, {11, 6},
            {13, 7}, {7, 5},  {10, 6}, {12, 9}, {6, 6},
            {9, 5},  {6, 8},  {4, 6},  {3, 4},  {10, 7},
            {12, 8}, {9, 6},  {7, 6},  {8, 5},  {6, 4},
            {5, 7},  {7, 9},  {2, 4},  {4, 4},  {5, 5},
        };

        auto rgbSum = [&](int x, int y) -> int {
            if (x < 0) x = 0; else if (x >= w) x = w - 1;
            if (y < 0) y = 0; else if (y >= h) y = h - 1;
            const unsigned char* p = pixels + ((size_t)y * w + x) * 4;
            return (int)p[0] + (int)p[1] + (int)p[2];
        };

        double bestRatio = 1e30;
        int    bestC = 0, bestR = 0;

        for (const Cand& c : cands)
        {
            const int cellW = w / c.c;
            const int cellH = h / c.r;
            if (cellW < 16 || cellH < 16) continue;
            // Strict fit: each axis must divide within 1 px. Real LG quilts are
            // mathematically clean; loose fits indicate the wrong grid.
            if (w - cellW * c.c > 1 || h - cellH * c.r > 1) continue;

            constexpr int SX = 8, SY = 8;        // 8x8 samples per cell
            long long hSad = 0;  int hN = 0;     // horizontal-neighbour SAD
            long long vSad = 0;  int vN = 0;     // vertical-neighbour SAD
            long long dSad = 0;  int dN = 0;     // diagonal-opposite SAD (baseline)

            for (int cy = 0; cy < c.r; ++cy)
                for (int cx = 0; cx < c.c; ++cx)
                    for (int sj = 0; sj < SY; ++sj)
                        for (int si = 0; si < SX; ++si)
                        {
                            const int dx = (cellW * (si * 2 + 1)) / (SX * 2);
                            const int dy = (cellH * (sj * 2 + 1)) / (SY * 2);
                            const int yy = cy * cellH + dy;
                            const int xx = cx * cellW + dx;

                            if (cx < c.c - 1)   // horizontal neighbour
                            {
                                hSad += abs(rgbSum(xx, yy) - rgbSum(xx + cellW, yy));
                                ++hN;
                            }
                            if (cy < c.r - 1)   // vertical neighbour (LG layout:
                            {                   // same scene shifted by +cols views)
                                vSad += abs(rgbSum(xx, yy) - rgbSum(xx, yy + cellH));
                                ++vN;
                            }
                            // Diagonal-opposite cell as baseline for global
                            // image variance. Same relative sample inside each.
                            const int diagCx = c.c - 1 - cx;
                            const int diagCy = c.r - 1 - cy;
                            if (diagCx != cx || diagCy != cy)
                            {
                                const int x2 = diagCx * cellW + dx;
                                const int y2 = diagCy * cellH + dy;
                                dSad += abs(rgbSum(xx, yy) - rgbSum(x2, y2));
                                ++dN;
                            }
                        }

            if (hN == 0 || vN == 0 || dN == 0) continue;
            const double hMean = (double)hSad / hN;
            const double vMean = (double)vSad / vN;
            const double dMean = (double)dSad / dN;
            if (dMean < 1.0) continue;
            // Combined ratio: BOTH horizontal AND vertical neighbours should be
            // small relative to the diagonal baseline. Either being large means
            // the grid is wrong on that axis.
            double ratio = ((hMean + vMean) * 0.5) / dMean;

            // Cell-aspect bias: real LG quilt views match one of the LG display
            // aspects (Portrait 3:4 = 0.75, 16:10 landscape = 1.6, 16:9 = 1.78).
            // Grids whose cells fit a known aspect get a small score bonus -
            // breaks ties between numerically-close candidates (e.g. 8x6 vs
            // 11x8 on a square quilt: both fit, but 8x6's 3:4 aspect matches
            // Portrait native exactly so it gets the bonus).
            const double cellAspect = (double)cellW / (double)cellH;
            const double aspects[5] = { 0.75, 1.6, 1.7777, 0.5625, 0.625 };
            double aspectErr = 1.0;
            for (double a : aspects) { double e = std::fabs(cellAspect - a); if (e < aspectErr) aspectErr = e; }
            ratio *= (1.0 + 0.25 * aspectErr);   // 0% extra at perfect match, scales with err

            if (ratio < bestRatio) { bestRatio = ratio; bestC = c.c; bestR = c.r; }
        }

        stbi_image_free(pixels);
        if (bestC == 0 || bestR == 0) return false;
        Log("AutoDetectQuiltGrid: %dx%d (horizontal/diagonal SAD ratio %.3f)",
            bestC, bestR, bestRatio);
        outCols = bestC; outRows = bestR;
        return true;
    }

    // Re-run the image-content detect on the currently-loaded test image and
    // apply the result. No-op when there's no image loaded.
    void AutoDetectQuiltOnCurrent(AppState& app)
    {
        if (app.lastTestImagePath.empty()) return;
        int qc = 0, qr = 0;
        if (!AutoDetectQuiltGrid(app.lastTestImagePath.c_str(), qc, qr)) return;
        app.format        = StereoFormat::Quilt;
        app.quiltCols     = qc;
        app.quiltRows     = qr;
        const int total   = qc * qr;
        app.quiltLeftIdx  = total / 2 - 1; if (app.quiltLeftIdx  < 0)        app.quiltLeftIdx  = 0;
        app.quiltRightIdx = total / 2;     if (app.quiltRightIdx >= total)   app.quiltRightIdx = total - 1;
        app.quiltLeftBlend = app.quiltRightBlend = 0.0f;
        app.captureRebind = true;
    }

    // Load a stereo image from disk as the SourceKind::TestImage source. If the
    // filename embeds a Looking Glass quilt token ("_qs8x6_" etc) the format is
    // auto-set to Quilt with the parsed grid + a centred view pair; otherwise
    // image-content detection runs as a fallback -- so a quilt file without the
    // naming convention still works. Non-quilt images fall through and keep the
    // currently-selected format.
    // True if the file extension (case-insensitive) matches a Lytro
    // light-field container -- .lfp, .lfr, .lfx all use the same LFP
    // container format and resolve through LFPLoadAsStereoSBS.
    static bool IsLytroLightFieldFile(const char* path)
    {
        if (!path) return false;
        const char* dot = strrchr(path, '.');
        if (!dot) return false;
        // Quick ASCII lowercase compare.
        auto eqi = [](const char* a, const char* b) {
            while (*a && *b) {
                char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
                char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
                if (ca != cb) return false;
                ++a; ++b;
            }
            return *a == 0 && *b == 0;
        };
        return eqi(dot, ".lfp") || eqi(dot, ".lfr") || eqi(dot, ".lfx");
    }

    bool LoadTestImage(AppState& app, const char* path)
    {
        if (!path || !*path) return false;

        // Tear down whichever source was last loaded (video or image) so we
        // don't end up with both bound.
        app.video.Close();

        const bool isVideo = VideoSource::IsVideoFile(path);
        const bool isLFP   = IsLytroLightFieldFile(path);
        const bool isEslf  = srw::IsLytroEslfPng(path);
        // Tear down the LFPRenderer if a non-LFP source is loaded -- the
        // render loop's `if (lfpRenderer.HasData())` branch would otherwise
        // shadow the regular image / video path with the stale LFP RT.
        if (!isLFP && !isEslf) app.lfpRenderer.Unload();
        if (isVideo)
        {
            wchar_t wpath[MAX_PATH] = {};
            MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH);
            if (!app.video.Open(app.renderer.Device(), wpath))
            {
                ShowError("Failed to open video file.\n\n"
                          "See srweaver.log next to the executable for details. "
                          "Most common causes: codec missing, file path issue, "
                          "or Windows Media Foundation not fully installed.");
                return false;
            }
        }
        else if (isLFP)
        {
            // Lytro light-field photo: full plenoptic 3D path. The
            // demosaic step inside LFPLoadDemosaicedSensor automatically
            // histogram-matches our colour output against the embedded
            // JPG preview (when present, which is every LFR/LFX) -- so
            // we get 3D parallax AND Lytro's pipeline colour science.
            std::vector<uint8_t> sensorRgb;
            srw::LFPCalibration cal;
            if (!srw::LFPLoadDemosaicedSensor(path, sensorRgb, cal))
            {
                ShowError("Failed to decode Lytro light-field file.\n\n"
                          "See srweaver.log for details. Supports .lfp / .lfr / "
                          ".lfx (Lytro cameras). The file must contain raw "
                          "plenoptic sensor data + calibration metadata.");
                return false;
            }
            const RECT& d = app.srDisplayRect;
            const int dw = d.right - d.left;
            const int dh = d.bottom - d.top;
            const float displayAspect = (dh > 0) ? (float)dw / (float)dh : 1.0f;
            if (!app.lfpRenderer.LoadFromMemory(sensorRgb, cal, displayAspect))
            {
                ShowError("Failed to upload Lytro sensor data to GPU.\n\n"
                          "See srweaver.log for details.");
                return false;
            }
            app.format = StereoFormat::LightField;
        }
        else if (isEslf)
        {
            // Lytro ESLF PNG: a colour-corrected microlens-array image
            // exported from Lytro Desktop. Treats it as if it were our
            // demosaiced sensor output and feeds it through the same
            // LFPRenderer that handles raw LFRs -- so we get 3D
            // parallax AND Lytro's colour science (the PNG is the
            // result of their full render pipeline, baked in). The
            // calibration is synthesised from Illum-typical defaults.
            std::vector<uint8_t> sensorRgb;
            srw::LFPCalibration cal;
            if (!srw::LFPLoadEslfAsSensorRgb(path, sensorRgb, cal))
            {
                ShowError("Failed to load Lytro ESLF PNG.\n\n"
                          "See srweaver.log for details.");
                return false;
            }
            const RECT& d = app.srDisplayRect;
            const int dw = d.right - d.left;
            const int dh = d.bottom - d.top;
            const float displayAspect = (dh > 0) ? (float)dw / (float)dh : 1.0f;
            if (!app.lfpRenderer.LoadFromMemory(sensorRgb, cal, displayAspect))
            {
                ShowError("Failed to upload ESLF data to GPU.\n\n"
                          "See srweaver.log for details.");
                return false;
            }
            app.format = StereoFormat::LightField;
        }
        else
        {
            if (!app.weaver.SetStereoImageFromFile(app.renderer.Device(), path,
                                                   app.format, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))
                return false;
        }
        app.lastTestImagePath = path;   // remembered for AutoDetect re-run

        int qc = 0, qr = 0;
        // ONLY use the LG filename token here. Image-content detection is on a
        // user-triggered button (Auto-detect) instead -- it sometimes guesses
        // wrong, so it shouldn't override the loader silently. If the filename
        // has no token, the user's currently-selected format is kept.
        if (ParseQuiltDims(path, qc, qr))
        {
            app.format        = StereoFormat::Quilt;
            app.quiltCols     = qc;
            app.quiltRows     = qr;
            const int total   = qc * qr;
            app.quiltLeftIdx  = total / 2 - 1; if (app.quiltLeftIdx  < 0)        app.quiltLeftIdx  = 0;
            app.quiltRightIdx = total / 2;     if (app.quiltRightIdx >= total)   app.quiltRightIdx = total - 1;
            Log("LoadTestImage: quilt %dx%d (from filename; default views L=%d R=%d)",
                qc, qr, app.quiltLeftIdx, app.quiltRightIdx);
        }
        // Stop any live capture so its frames don't compete with the test image,
        // then point the source at it and force a converter rebind / pipeline rebuild.
        app.capture.Stop();
        if (app.dxgiActive) { app.captureDxgi.Stop(); app.dxgiActive = false; }
        app.source        = SourceKind::TestImage;
        app.sourceWindow  = nullptr;
        app.sourceMonitor = nullptr;
        app.foreignDisplay = false;
        app.captureRebind = true;
        app.mode          = OutputMode::Fullscreen;
        // A new test image's content aspect / dimensions almost certainly
        // differ from whatever was last shown -- invalidate the LookingGlass
        // saved size so the next LG entry re-fits the window to the new
        // content instead of preserving the old shape.
        app.loupeActive   = false;
        // Media profile remember-and-apply. Stash the path so the auto-
        // save path can write back to the right key when the user tweaks
        // settings, and look up any saved profile for this file -- on hit,
        // overlay the saved format/swap/convergence/anaglyph on top of
        // whatever LoadTestImage just inferred from the filename. Skipped
        // for LFP/ESLF since their format is forced (LightField), and
        // saving sub-settings for them doesn't make sense yet.
        if (!isLFP && !isEslf)
        {
            app.currentMediaPath = path;
            Profile mp;
            if (MediaProfiles::LoadFor(path, mp))
            {
                app.format        = mp.format;
                app.swapEyes      = mp.swapEyes;
                app.convergence   = mp.convergence;
                app.anaglyphCombo = mp.anaglyphCombo;
                app.anaglyphMode  = mp.anaglyphMode;
                app.pulfrichMode  = (PulfrichMode)mp.pulfrichMode;
                app.pulfrichDelay = mp.pulfrichDelay;
                app.pulfrichNd    = mp.pulfrichNd;
                app.framePackMode = mp.framePackMode;
                // Quilt overrides only when the saved values look valid
                // (positive; -1 means "auto" -- the filename parse done
                // earlier in LoadTestImage already picked sensible defaults).
                if (mp.quiltCols > 0) app.quiltCols = mp.quiltCols;
                if (mp.quiltRows > 0) app.quiltRows = mp.quiltRows;
                if (mp.quiltLeftIdx  >= 0) app.quiltLeftIdx  = mp.quiltLeftIdx;
                if (mp.quiltRightIdx >= 0) app.quiltRightIdx = mp.quiltRightIdx;
                Log("LoadTestImage: applied media profile (format=%s)",
                    Profiles::FormatToString(mp.format));
            }
            // Seed the hash so the first auto-save tick doesn't immediately
            // re-write the freshly-loaded state back to disk -- only user
            // tweaks AFTER load should trigger a save.
            app.lastMediaStateHash = HashStereoState(app);
            app.lastMediaSaveMs    = GetTickCount();
        }
        else
        {
            app.currentMediaPath.clear();
        }
        EnsureWeaving(app);
        return true;
    }

    // Tray-menu / GUI handler: pop the standard Open dialog filtered to common
    // image extensions, then hand the picked path to LoadTestImage.
    void OpenLoadTestImageDialog(AppState& app)
    {
        char file[MAX_PATH] = {};
        OPENFILENAMEA ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner   = app.hwnd;
        ofn.lpstrFilter =
            "Stereo media\0*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.mp4;*.mov;*.mkv;*.webm;*.avi;*.m4v;*.wmv;*.lfp;*.lfr;*.lfx\0"
            "Images\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0"
            "Lytro light field\0*.lfp;*.lfr;*.lfx;*_eslf.png;*_qs14x14*.png\0"
            "Videos\0*.mp4;*.mov;*.mkv;*.webm;*.avi;*.m4v;*.wmv\0"
            "All files\0*.*\0";
        ofn.lpstrFile   = file;
        ofn.nMaxFile    = (DWORD)sizeof(file);
        ofn.lpstrTitle  = "Load media (stereo / quilt)";
        ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameA(&ofn))
            LoadTestImage(app, file);
    }

    // Weave another display (real or virtual) — picked from the GUI's display dropdown.
    // The whole captured frame is woven onto the SR display in fullscreen (no region
    // crop, since the source isn't the screen we're drawing on, so there's no feedback).
    void UseDisplay(AppState& app, HMONITOR mon)
    {
        DemoteVRFormatForLiveCapture(app);
        app.capture.SetCaptureCursor(true);   // show the pointer on the captured display
        if (!mon || !app.capture.StartMonitor(mon))
            return;
        app.sourceWindow = nullptr;
        app.source = SourceKind::CaptureMonitor;
        app.sourceMonitor = mon;
        app.foreignDisplay = (mon != SrMonitor(app));
        app.captureRebind = true;
        app.mode = OutputMode::Fullscreen;   // show the whole captured display, woven
        EnsureWeaving(app);
    }

    // Map a window's client area to a rectangle in the captured frame's pixels.
    RECT PassthroughRegion(AppState& app, HWND hwnd)
    {
        const double fw = app.capture.FrameWidth();
        const double fh = app.capture.FrameHeight();

        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfo(mon, &mi);
        const RECT m = mi.rcMonitor;
        const double sx = fw / (m.right - m.left);
        const double sy = fh / (m.bottom - m.top);

        // Client area in screen coordinates.
        RECT c{};
        GetClientRect(hwnd, &c);
        POINT tl{ c.left, c.top }, br{ c.right, c.bottom };
        ClientToScreen(hwnd, &tl);
        ClientToScreen(hwnd, &br);

        RECT r;
        r.left   = (LONG)((tl.x - m.left) * sx);
        r.top    = (LONG)((tl.y - m.top) * sy);
        r.right  = (LONG)((br.x - m.left) * sx);
        r.bottom = (LONG)((br.y - m.top) * sy);
        return r;
    }

    // Resolve the current source frame (test image, video, or capture).
    bool ResolveSource(AppState& app, ID3D11ShaderResourceView*& srv, int& w, int& h)
    {
        if (app.source == SourceKind::TestImage)
        {
            if (app.video.IsOpen())
            {
                app.video.Update(app.renderer.Context());
                srv = app.video.SRV(); w = app.video.Width(); h = app.video.Height();
            }
            else
            {
                srv = app.weaver.SourceSRV(); w = app.weaver.SourceWidth(); h = app.weaver.SourceHeight();
            }
        }
        else if (app.capture.IsActive())
        {
            srv = app.capture.SRV(); w = app.capture.Width(); h = app.capture.Height();
        }
        else { srv = nullptr; w = h = 0; }
        return srv && w > 0 && h > 0;
    }

    // Analyze the current frame and switch to the detected stereo layout.
    // Returns true when done: a layout was found, or the format is one it
    // leaves alone. False: no picture yet, or nothing recognisable in it.
    bool DetectFormat(AppState& app)
    {
        // The detector only ever returns SBS/TAB/Anaglyph. If the user is
        // on a format with its own input pipeline (Katanga shared texture,
        // LFP plenoptic, VR equirect, Quilt, frame-packing, Pulfrich
        // temporal) a "detection" would silently downgrade it -- skip.
        switch (app.format)
        {
        case StereoFormat::Katanga:
        case StereoFormat::LightField:
        case StereoFormat::Quilt:
        case StereoFormat::VR180TAB:
        case StereoFormat::VR180SBS:
        case StereoFormat::VR360TAB:
        case StereoFormat::VR360SBS:
        case StereoFormat::Pulfrich:
        case StereoFormat::FramePacking:
            return true;
        default:
            break;
        }
        ID3D11ShaderResourceView* srv = nullptr; int w = 0, h = 0;
        if (!ResolveSource(app, srv, w, h)) return false;
        StereoFormat f;
        if (app.detector.Detect(srv, w, h, f))
        {
            ChangeFormat(app, f);
            app.tray.SetTooltip("SR Loom — detected a stereo layout");
            return true;
        }
        app.tray.SetTooltip("SR Loom — no stereo layout detected");
        return false;
    }

    // The picture inside black letterbox / pillarbox bars (a video player
    // fullscreen), in analysis px: rows / columns that are all near-black
    // are trimmed off each side.
    RECT TrimBlackBars(const LumaImage& img)
    {
        RECT r{ 0, 0, img.width, img.height };
        auto rowDark = [&](int y) {
            for (int x = 0; x < img.width; x += 4) if (img.at(x, y) > 24) return false;
            return true;
        };
        auto colDark = [&](int x) {
            for (int y = r.top; y < r.bottom; y += 4) if (img.at(x, y) > 24) return false;
            return true;
        };
        while (r.bottom - r.top > 64 && rowDark(r.top)) ++r.top;
        while (r.bottom - r.top > 64 && rowDark(r.bottom - 1)) --r.bottom;
        while (r.right - r.left > 64 && colDark(r.left)) ++r.left;
        while (r.right - r.left > 64 && colDark(r.right - 1)) --r.right;
        return r;
    }

    // Weave the whole display in `fmt` while fullscreen window `fs` stays.
    void EnterFullscreenAuto(AppState& app, HWND fs, StereoFormat fmt, const AnaglyphKind& ana, bool eyeSwap, const char* why)
    {
        char title[128] = "";
        GetWindowTextA(fs, title, (int)sizeof(title));
        Log("Automatic Detection: fullscreen '%s' is %s -- weaving the whole display (%s)",
            title, Profiles::FormatToString(fmt), why);
        app.fsAutoWindow = fs;
        EndAutoStereo(app, "fullscreen 3D");
        if (fmt == StereoFormat::Anaglyph && ana.known)
        {
            app.anaglyphCombo = ana.combo;
            // (Recovered Colour; for a black-and-white or one-colour picture
            // UpdateManualAnaColour decodes it as Mono / with its tint -- the
            // setting itself isn't changed to them.)
            app.anaglyphMode  = 4;
            Log("Automatic Detection: anaglyph is %s (%s)", AnaComboName(ana.combo),
                ana.mode == 3 ? "black-and-white" : ana.mode == 5 ? "one colour" : "colour");
        }
        app.autoEyeSwap = eyeSwap && app.eyeOrderDetect;
        if (app.autoEyeSwap) Log("Automatic Detection: eye order SWAPPED (the second half is the left eye)");
        ChangeFormat(app, fmt);
        ApplyMode(app);
    }

    // Automatic Detection on this display, and a window goes fullscreen on it
    // (a video player, a browser's fullscreen video, a game): is its WHOLE
    // picture 3D? If so, weave the whole display in that layout -- no
    // picture-finding -- until it leaves fullscreen (or another window comes
    // to the front); then Auto Stereo takes over again. Checked often for
    // the first few seconds (a video may start dark), then every 2 s while
    // it stays fullscreen; meanwhile Auto Stereo keeps finding pictures in it.
    void UpdateFullscreenAuto(AppState& app)
    {
        const DWORD now = GetTickCount();
        if ((LONG)(now - app.fsNextPoll) < 0) return;
        app.fsNextPoll = now + 100;
        const HWND fg = GetForegroundWindow();
        const bool ours = fg && IsOwnProcessWindow(fg);   // (our panel in front: no change)
        HWND fs = nullptr;
        if (fg && !ours && MonitorFromWindow(fg, MONITOR_DEFAULTTONULL) == SrMonitor(app) && IsWindowFullscreen(fg) &&
            (!app.autoScopeWindow || GetAncestor(fg, GA_ROOT) == app.autoScopeWindow))   // (a chosen window: only it)
            fs = fg;

        if (app.fsAutoWindow)
        {
            if (ours || fs == app.fsAutoWindow) return;
            Log("Automatic Detection: fullscreen window left -- back to finding 3D pictures");
            app.fsAutoWindow = nullptr;
            app.fsCheckedWindow = nullptr;
            return;   // (the caller turns Auto Stereo back on)
        }
        if (ours) return;
        if (!fs) { app.fsCheckedWindow = nullptr; return; }
        if (fs != app.fsCheckedWindow)
        {
            app.fsCheckedWindow = fs;
            app.fsCheckUntil = now + 5000;   // the quick-check phase
            app.fsNextCheck  = now + 250;    // (let it finish going fullscreen)
        }
        app.scanWantColour = true;          // (anaglyph needs the colour planes)

        // Auto Stereo already found a 3D picture filling most of the screen
        // in it: that's the answer.
        {
            MONITORINFO mi{ sizeof(mi) };
            if (GetMonitorInfo(SrMonitor(app), &mi))
            {
                const long long monArea = (long long)(mi.rcMonitor.right - mi.rcMonitor.left) *
                                          (mi.rcMonitor.bottom - mi.rcMonitor.top);
                for (const WeaveRegion& r : app.regionWeaver.Regions())
                {
                    if (!r.autoDetected || r.host != fs) continue;
                    RECT on{};
                    if (!IntersectRect(&on, &r.screen, &mi.rcMonitor)) continue;
                    const long long a = (long long)(on.right - on.left) * (on.bottom - on.top);
                    if (a * 2 < monArea) continue;
                    AnaglyphKind k;
                    k.combo = r.anaglyphCombo; k.mode = r.anaglyphMode; k.known = r.anaAuto;
                    EnterFullscreenAuto(app, fs, r.format, k, r.autoSwap, "its picture fills the screen");
                    return;
                }
            }
        }

        // A check running in the background: take its verdict when it's in.
        if (app.fsJob.valid())
        {
            if (app.fsJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            const AppState::FsVerdict v = app.fsJob.get();
            if (v.is3D && v.window == fs)   // (an anaglyph's eye order is fixed: red is the left eye)
                EnterFullscreenAuto(app, fs, v.fmt, v.ana, v.fmt != StereoFormat::Anaglyph && v.eye.known && v.eye.swap, v.diag.c_str());
            return;
        }
        if ((LONG)(now - app.fsNextCheck) < 0) return;
        // The whole screen (inside any black bars) as the analyser last saw
        // it, once a frame with colour has arrived; classified on a copy, in
        // the background.
        if (app.analysisImg.width <= 0 || !app.analysisImg.hasColour()) return;
        app.fsNextCheck = now + ((LONG)(now - app.fsCheckUntil) < 0 ? 400 : 2000);
        auto img = std::make_shared<LumaImage>(app.analysisImg);
        app.fsJob = std::async(std::launch::async, [img, fs]() {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            AppState::FsVerdict v;
            v.window = fs;
            const RECT pic = TrimBlackBars(*img);
            float score = 0;
            char diag[256] = "";
            v.is3D = ClassifyStereo(*img, pic, v.fmt, score, diag, sizeof(diag), nullptr, &v.ana, &v.eye);
            v.diag = diag;
            return v;
        });
    }

    // Stereo 3D Input "Automatic Detection", run once per loop. Weaving this
    // display in Fullscreen: Auto Stereo (find 3D pictures / videos and
    // weave just those). Any other source: check the whole picture's layout
    // when the source changes, retrying for a few seconds until there's
    // something recognisable. Nothing here turns weaving on -- it applies
    // once weaving is on.
    // Eye order for a whole picture in a layout picked by hand (SBS, TAB or
    // anaglyph) -- or found for a whole picture by Automatic Detection on
    // another source: every 1.5 s one frame of it is analysed and judged in
    // the background (the same cues as Auto Stereo's). The order only flips
    // when two checks in a row agree. Swap Eyes still flips it on top.
    void UpdateManualEyeOrder(AppState& app)
    {
        const StereoFormat f = app.format;
        const bool layout = f == StereoFormat::FullSBS || f == StereoFormat::HalfSBS ||
                            f == StereoFormat::FullTAB || f == StereoFormat::HalfTAB;   // (not an anaglyph: red is always the left eye)
        // What's being woven (without ResolveSource: that also steps a video).
        ID3D11ShaderResourceView* srv = nullptr; int w = 0, h = 0;
        if (app.source == SourceKind::TestImage)
        {
            if (app.video.IsOpen()) { srv = app.video.SRV(); w = app.video.Width(); h = app.video.Height(); }
            else { srv = app.weaver.SourceSRV(); w = app.weaver.SourceWidth(); h = app.weaver.SourceHeight(); }
        }
        else if (app.capture.IsActive()) { srv = app.capture.CopyView(); w = app.capture.Width(); h = app.capture.Height(); }
        const bool on = app.weavingEnabled && app.eyeOrderDetect && layout && !app.autoStereo && !app.fsAutoWindow;
        if (!on || !srv || w <= 0 || h <= 0)
        {
            app.manualEyeSwap = false; app.manualEyeVotes = 0; app.manualEyePhase = 0; app.manualEyeKey = 0;
            return;
        }
        // A different picture / layout: start over.
        size_t key = std::hash<std::string>()(app.lastTestImagePath);
        key = key * 31 + (size_t)f;
        key = key * 31 + (size_t)app.source;
        key = key * 31 + (size_t)(uintptr_t)app.sourceWindow;
        key = key * 31 + (size_t)app.anaglyphCombo;
        const DWORD now = GetTickCount();
        if (key != app.manualEyeKey)
        {
            app.manualEyeKey = key;
            app.manualEyeSwap = false; app.manualEyeVotes = 0;
            if (app.manualEyePhase != 2) app.manualEyePhase = 0;
            app.manualEyeNext = now + 300;
        }
        if (app.manualEyePhase == 0)
        {
            if ((LONG)(now - app.manualEyeNext) < 0) return;
            if (srv == app.capture.CopyView()) srv = app.capture.SRV();   // (zero-copy: the copy is made when wanted)
            app.analyzer.Submit(srv, w, h, false);   // (SBS / TAB: luminance is enough)
            app.manualEyePhase = 1;
            return;
        }
        if (app.manualEyePhase == 1)
        {
            auto img = std::make_shared<LumaImage>();
            uint64_t id = 0;
            if (!app.analyzer.Latest(*img, id, false)) return;   // (not read back yet)
            const RECT whole{ 0, 0, img->width, img->height };
            const int combo = app.anaglyphCombo;
            app.manualEyeJob = std::async(std::launch::async, [img, whole, f, combo]() {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                EyeOrder eo;
                JudgeEyeOrderOf(*img, whole, f, combo, eo);
                return eo;
            });
            app.manualEyePhase = 2;
            return;
        }
        if (app.manualEyeJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        const EyeOrder eo = app.manualEyeJob.get();
        app.manualEyePhase = 0;
        app.manualEyeNext  = now + 1500;
        if (!eo.known) return;
        if (eo.swap == app.manualEyeSwap) { app.manualEyeVotes = 0; return; }
        if (++app.manualEyeVotes < 2) return;
        app.manualEyeSwap = eo.swap;
        app.manualEyeVotes = 0;
        Log("Eye order (%s): %s (shift all %d, top %d, bottom %d)", Profiles::FormatToString(f),
            eo.swap ? "SWAPPED" : "normal", eo.dxAll, eo.dxTop, eo.dxBottom);
    }

    // Anaglyph picked by hand (or a whole fullscreen one found by Automatic
    // Detection) with Recovered Colour: every 0.7 s one frame is scanned in the
    // background the way Auto Stereo does -- the anaglyph pictures' boxes --
    // and each box checked for what the picture under the anaglyph was
    // (AnalyseAnaPicture). A black-and-white one is decoded as Mono inside its
    // box (there's no colour to recover; Recovered Colour would only guess
    // some), a one-colour one (sepia, a duotone) with its tint; the rest of the
    // frame is recovered as before (Converter::SetAnaBoxes). No box found: the
    // whole frame is checked (a page or bars around the picture are plain grey
    // and don't count) -- there a new picture's first verdict counts at once,
    // after that a change needs two checks that agree.
    // The Custom anaglyph pair's eyedropper. While picking, the weave window takes
    // clicks (it's normally click-through, so the click would go to whatever is
    // underneath); the click's colour comes from the picture being converted, not
    // the screen -- that shows the weave.
    // An eyedropper cursor (Windows has none): a pipette drawn into 32 x 32
    // masks, its tip -- the hot spot -- bottom left. White with a black edge.
    HCURSOR EyedropCursor()
    {
        static HCURSOR s_cur = nullptr;
        if (s_cur) return s_cur;
        BYTE andPlane[32 * 4], xorPlane[32 * 4];
        memset(andPlane, 0xFF, sizeof(andPlane)); memset(xorPlane, 0, sizeof(xorPlane));
        auto segDist = [](float px, float py, float ax, float ay, float bx, float by) {
            const float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
            float t = (vx * wx + vy * wy) / (vx * vx + vy * vy); t = t < 0 ? 0 : t > 1 ? 1 : t;
            const float dx = wx - t * vx, dy = wy - t * vy; return sqrtf(dx * dx + dy * dy); };
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x)
            {
                const float px = x + 0.5f, py = y + 0.5f;
                const float tube = segDist(px, py, 4.0f, 27.0f, 20.0f, 11.0f);   // (the glass tube)
                const float tip  = segDist(px, py, 1.5f, 29.5f, 4.0f, 27.0f);    // (its point)
                const float bulbX = px - 24.0f, bulbY = py - 7.0f;
                const float bulb = sqrtf(bulbX * bulbX + bulbY * bulbY);         // (the rubber bulb)
                int v = -1;   // -1 nothing, 0 black, 1 white
                if (bulb <= 5.5f) v = 0;
                else if (tube <= 1.6f) v = 1;
                else if (tube <= 3.0f || tip <= 1.3f) v = 0;
                if (bulb <= 4.0f || (tube <= 3.0f && px > 17.0f && py < 14.0f && tube > 1.6f)) v = 0;
                if (v < 0) continue;
                const int i = y * 4 + x / 8, bit = 0x80 >> (x % 8);
                andPlane[i] &= (BYTE)~bit;
                if (v == 1) xorPlane[i] |= (BYTE)bit;
            }
        s_cur = CreateCursor(GetModuleHandleW(nullptr), 1, 30, 32, 32, andPlane, xorPlane);
        return s_cur ? s_cur : LoadCursor(nullptr, IDC_CROSS);
    }
    void EyedropEnd(AppState& app)
    {
        if (!app.eyedrop) return;
        app.eyedrop = 0; app.eyedropClicked = false;
        if (app.hwnd && IsWindow(app.hwnd))
        {
            SetWindowLongPtr(app.hwnd, GWL_EXSTYLE, app.eyedropOldEx);
            SetWindowPos(app.hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    }
    void EyedropStart(AppState& app, int which)
    {
        EyedropEnd(app);
        if (!app.hwnd || !IsWindow(app.hwnd) || !app.weavingEnabled) return;
        app.eyedrop = which; app.eyedropClicked = false; app.eyedropSince = GetTickCount64();
        app.eyedropOldEx = GetWindowLongPtr(app.hwnd, GWL_EXSTYLE);
        if (app.eyedropOldEx & WS_EX_TRANSPARENT)
        {
            SetWindowLongPtr(app.hwnd, GWL_EXSTYLE, app.eyedropOldEx & ~(LONG_PTR)WS_EX_TRANSPARENT);
            SetWindowPos(app.hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        Log("Anaglyph: picking the %s colour from the picture", which == 1 ? "left" : "right");
    }
    // The picture's colour (sRGB, 0-1) around the clicked point: the converter's
    // source, scaled to the weave window (what's woven fills it). A 5x5 average.
    bool SampleSourceColour(AppState& app, ID3D11ShaderResourceView* srv, int srcW, int srcH, POINT pt, float rgb[3])
    {
        RECT cr{}; if (!srv || srcW <= 0 || srcH <= 0 || !GetClientRect(app.hwnd, &cr) || cr.right <= 0 || cr.bottom <= 0) return false;
        ID3D11Resource* res = nullptr; srv->GetResource(&res);
        ID3D11Texture2D* tex = nullptr; if (res) { res->QueryInterface(&tex); res->Release(); }
        if (!tex) return false;
        D3D11_TEXTURE2D_DESC td{}; tex->GetDesc(&td);
        const bool bgra = td.Format == DXGI_FORMAT_B8G8R8A8_UNORM || td.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || td.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
        const bool rgba = td.Format == DXGI_FORMAT_R8G8B8A8_UNORM || td.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
        bool ok = false;
        if (bgra || rgba)
        {
            const int x = (std::min)((int)td.Width - 1, (int)((pt.x + 0.5) * srcW / cr.right)), y = (std::min)((int)td.Height - 1, (int)((pt.y + 0.5) * srcH / cr.bottom));
            const int x0 = (std::max)(0, x - 2), y0 = (std::max)(0, y - 2), x1 = (std::min)((int)td.Width, x + 3), y1 = (std::min)((int)td.Height, y + 3);
            D3D11_TEXTURE2D_DESC sd = td; sd.Width = (UINT)(x1 - x0); sd.Height = (UINT)(y1 - y0); sd.MipLevels = 1; sd.ArraySize = 1;
            sd.SampleDesc.Count = 1; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
            ID3D11Texture2D* st = nullptr;
            ID3D11DeviceContext* ctx = app.renderer.Context();
            if (SUCCEEDED(app.renderer.Device()->CreateTexture2D(&sd, nullptr, &st)))
            {
                D3D11_BOX box{ (UINT)x0, (UINT)y0, 0, (UINT)x1, (UINT)y1, 1 };
                ctx->CopySubresourceRegion(st, 0, 0, 0, 0, tex, 0, &box);
                D3D11_MAPPED_SUBRESOURCE m{};
                if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m)))
                {
                    double s[3] = {}; int n = 0;
                    for (UINT yy = 0; yy < sd.Height; ++yy)
                        for (UINT xx = 0; xx < sd.Width; ++xx)
                        {
                            const uint8_t* p = (const uint8_t*)m.pData + (size_t)yy * m.RowPitch + (size_t)xx * 4;
                            s[0] += bgra ? p[2] : p[0]; s[1] += p[1]; s[2] += bgra ? p[0] : p[2]; ++n;
                        }
                    ctx->Unmap(st, 0);
                    if (n > 0) { for (int c = 0; c < 3; ++c) rgb[c] = (float)(s[c] / n / 255.0); ok = true; }
                }
                st->Release();
            }
        }
        tex->Release();
        return ok;
    }

    void UpdateManualAnaColour(AppState& app)
    {
        ID3D11ShaderResourceView* srv = nullptr; int w = 0, h = 0;
        if (app.source == SourceKind::TestImage)
        {
            if (app.video.IsOpen()) { srv = app.video.SRV(); w = app.video.Width(); h = app.video.Height(); }
            else { srv = app.weaver.SourceSRV(); w = app.weaver.SourceWidth(); h = app.weaver.SourceHeight(); }
        }
        else if (app.dxgiActive) { srv = app.captureDxgi.SRV(); w = app.captureDxgi.Width(); h = app.captureDxgi.Height(); }   // (what the converter gets)
        else if (app.capture.IsActive()) { srv = app.capture.CopyView();   /* (only checked here) */ w = app.capture.Width(); h = app.capture.Height(); }
        // (Not for a Custom pair: the check's tests are for the six known pairs.)
        const bool on = app.weavingEnabled && app.format == StereoFormat::Anaglyph && app.anaglyphMode == 4 &&
                        app.anaglyphCombo < kAnaComboCustom &&
                        !app.autoStereo;
        if (!on || !srv || w <= 0 || h <= 0)
        {
            app.manualAnaKind = 0; app.manualAnaTint.reset(); app.manualAnaBoxes.clear(); app.manualAnaVotes = 0; app.manualAnaKey = 0;
            if (app.manualAnaPhase == 2) app.manualAnaStale = true;   // (a running job is collected below, and dropped)
            else { app.manualAnaPhase = 0; app.manualAnaSubmitWanted = false; }
            if (app.manualAnaPhase == 2 && app.manualAnaJob.valid() && app.manualAnaJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            { app.manualAnaJob.get(); app.manualAnaPhase = 0; app.manualAnaStale = false; }
            return;
        }
        // A different picture: start over.
        size_t key = std::hash<std::string>()(app.lastTestImagePath);
        key = key * 31 + (size_t)app.source;
        key = key * 31 + (size_t)(uintptr_t)app.sourceWindow;
        key = key * 31 + (size_t)(uintptr_t)app.fsAutoWindow;
        key = key * 31 + (size_t)app.anaglyphCombo;
        const DWORD now = GetTickCount();
        if (key != app.manualAnaKey)
        {
            app.manualAnaKey = key;
            app.manualAnaKeyAt = now;
            app.manualAnaLogged = false;
            app.manualAnaKind = 0; app.manualAnaTint.reset(); app.manualAnaBoxes.clear(); app.manualAnaVotes = 0;
            if (app.manualAnaPhase == 2) app.manualAnaStale = true;
            else { app.manualAnaPhase = 0; app.manualAnaSubmitWanted = false; }
            app.manualAnaNext = now;   // (at once: a black-and-white picture shown in colour for a moment first was a visible delay)
        }
        if (app.manualAnaPhase == 0)
        {
            if ((LONG)(now - app.manualAnaNext) < 0) return;
            // (Handed to the analyser right before the converter runs, with the
            // same frame -- which the converter keeps as the boxes' reference for
            // following the page as it scrolls: RenderFrame.)
            app.manualAnaSubmitWanted = true;
            app.manualAnaPhase = 3;
            return;
        }
        if (app.manualAnaPhase == 1)
        {
            auto img = std::make_shared<LumaImage>();
            uint64_t id = 0;
            if (!app.analyzer.Latest(*img, id, false)) return;   // (not read back yet)
            if (!img->hasColour()) { app.manualAnaPhase = 0; app.manualAnaNext = now + 300; return; }
            const int combo = app.anaglyphCombo;
            app.manualAnaJob = std::async(std::launch::async, [img, combo]() {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                const RECT whole{ 0, 0, img->width, img->height };
                ScanWindow sw; sw.bounds = whole;
                ScanResult res;
                StereoScanner::Scan(*img, { sw }, {}, res);
                AppState::AnaVerdict v;
                struct Hit { RECT r; int k; std::shared_ptr<const AnaTint> t; };
                std::vector<Hit> hits;
                // The pictures the scan judged 3D, and the others it found that show
                // some sign of being an anaglyph (AnaglyphEvidence): small anaglyph
                // thumbnails often miss the 3D test (and dropped out and came back
                // check to check), while page parts -- red text, an icon -- can pass
                // the black-and-white test and got greyed (stray boxes).
                std::vector<RECT> cands;
                for (const ScanHit& h : res.hits) cands.push_back(h.rect);
                for (const RECT& p : res.pictures)
                {
                    bool hit = false;
                    for (const ScanHit& h : res.hits) if (EqualRect(&h.rect, &p)) { hit = true; break; }
                    if (!hit && AnaglyphEvidence(*img, p)) cands.push_back(p);
                }
                std::sort(cands.begin(), cands.end(), [](const RECT& a, const RECT& b) { return std::tie(a.top, a.left, a.bottom, a.right) < std::tie(b.top, b.left, b.bottom, b.right); });
                cands.erase(std::unique(cands.begin(), cands.end(), [](const RECT& a, const RECT& b) { return EqualRect(&a, &b) != FALSE; }), cands.end());
                // A box holding other pictures (a block of a grid the scan also saw as
                // one) goes: judged black-and-white it greyed the gaps, captions and
                // links between them too -- a grey box wider than any picture.
                {
                    std::vector<RECT> keep;
                    for (const RECT& a : cands)
                    {
                        bool holds = false;
                        for (const RECT& b : cands)
                        {
                            if (EqualRect(&a, &b)) continue;
                            RECT in{};
                            const long bArea = (long)(b.right - b.left) * (b.bottom - b.top);
                            if (IntersectRect(&in, &a, &b) && (long)(in.right - in.left) * (in.bottom - in.top) >= bArea * 8 / 10 &&
                                (long)(a.right - a.left) * (a.bottom - a.top) > bArea * 3 / 2) { holds = true; break; }
                        }
                        if (!holds) keep.push_back(a);
                    }
                    cands.swap(keep);
                }
                for (const RECT& hr : cands)
                {
                    std::shared_ptr<const AnaTint> t;
                    float apart = -1.0f;
                    const int k = AnalyseAnaPicture(*img, hr, combo, &t, &apart);
                    {
                        char d[160];
                        snprintf(d, sizeof(d), "(%ld,%ld %ldx%ld) %s, blocks apart %.3f", hr.left, hr.top,
                                 hr.right - hr.left, hr.bottom - hr.top,
                                 k == 1 ? "black-and-white" : k == 2 ? "one colour" : k == 0 ? "colour" : "can't tell", apart);
                        v.diag.push_back(d);
                    }
                    if (k < 0) continue;
                    ++v.boxes;
                    (k == 1 ? v.grey : k == 2 ? v.tinted : v.colour)++;
                    hits.push_back({ hr, k, t });
                }
                const float iw = 1.0f / (std::max)(img->width, 1), ih = 1.0f / (std::max)(img->height, 1);
                for (const Hit& a : hits)
                {
                    if (a.k == 0)
                    {
                        v.coloured.push_back({ a.r.left * iw, a.r.top * ih, a.r.right * iw, a.r.bottom * ih, 0, nullptr });
                        continue;
                    }
                    // A black-and-white box reaches well past the picture: Mono on
                    // the plain page around it changes nothing, and it covers the
                    // picture's edge (and the recovery's colour fringe on the page
                    // beside it) even a little off after a scroll -- but never into
                    // a colour picture next to it. A one-colour box keeps close.
                    const int pad = a.k == 1 ? 2 : 1;   // (2 analysis px: wider looked like a box round the picture)
                    int pl = pad, pt = pad, pr = pad, pb = pad;
                    for (const Hit& c : hits)
                    {
                        if (c.k != 0) continue;
                        const bool rowsMeet = c.r.top < a.r.bottom + pad && c.r.bottom > a.r.top - pad;
                        const bool colsMeet = c.r.left < a.r.right + pad && c.r.right > a.r.left - pad;
                        if (rowsMeet && c.r.right <= a.r.left && c.r.right > a.r.left - pad) pl = (std::min)(pl, (int)(a.r.left - c.r.right));
                        if (rowsMeet && c.r.left >= a.r.right && c.r.left < a.r.right + pad) pr = (std::min)(pr, (int)(c.r.left - a.r.right));
                        if (colsMeet && c.r.bottom <= a.r.top && c.r.bottom > a.r.top - pad) pt = (std::min)(pt, (int)(a.r.top - c.r.bottom));
                        if (colsMeet && c.r.top >= a.r.bottom && c.r.top < a.r.bottom + pad) pb = (std::min)(pb, (int)(c.r.top - a.r.bottom));
                    }
                    v.special.push_back({ (std::max)(0L, a.r.left - pl) * iw, (std::max)(0L, a.r.top - pt) * ih,
                                          (std::min)((LONG)img->width, a.r.right + pr) * iw,
                                          (std::min)((LONG)img->height, a.r.bottom + pb) * ih, a.k, a.t, 0,
                                          (pad + 2) * iw, (pad + 2) * ih });   // (+2: the found box can be a pixel or two off)
                }
                if (v.boxes == 0) v.kind = AnalyseAnaPicture(*img, whole, combo, &v.tint);
                else v.kind = 0;   // (the boxes say it, each for itself)
                return v;
            });
            app.manualAnaPhase = 2;
            return;
        }
        if (app.manualAnaPhase == 3) return;   // (the frame is handed over in RenderFrame)
        if (!app.manualAnaJob.valid()) { app.manualAnaPhase = 0; return; }   // (never ask a job that isn't there: it throws)
        if (app.manualAnaJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        AppState::AnaVerdict v = app.manualAnaJob.get();
        app.manualAnaPhase = 0;
        if (app.manualAnaStale) { app.manualAnaStale = false; app.manualAnaNext = now; return; }   // (an earlier picture's)
        app.manualAnaNext = now + 700;
        // (Diagnostics, the first 40 checks: each box. No GPU read-back here: it
        // would stall the frame.)
        {
            static int diagLogs = 0;
            if (diagLogs < 40)
            {
                ++diagLogs;
                for (const std::string& d : v.diag) Log("  anaglyph box %s", d.c_str());
            }
        }
        static const char* const kinds[] = { "colour (Recovered Colour)", "black-and-white (Mono)", "one colour (tinted Mono)" };
        if (!app.manualAnaLogged || v.special.size() != app.manualAnaBoxes.size())
        {
            if (v.boxes > 0)
                Log("Anaglyph %s: %d picture box%s -- %d black-and-white, %d one colour, %d colour",
                    app.manualAnaLogged ? "check" : "first check", v.boxes, v.boxes == 1 ? "" : "es", v.grey, v.tinted, v.colour);
            else if (!app.manualAnaLogged)
                Log("Anaglyph first check: no picture box -- the whole frame is %s", v.kind < 0 ? "can't tell" : kinds[v.kind]);
            app.manualAnaLogged = true;
        }
        // The boxes, kept from check to check: a scan that misses a picture
        // once doesn't drop it (it stays up to 3 s after it was last found --
        // the converter re-checks every box every frame anyway, PSAnaBoxCheck);
        // one found again takes the new place and verdict; one now judged
        // colour goes at once.
        {
            auto overlap = [](const AppState::AnaBoxVerdict& a, const AppState::AnaBoxVerdict& b) {
                const float w = (std::min)(a.u1, b.u1) - (std::max)(a.u0, b.u0), h = (std::min)(a.v1, b.v1) - (std::max)(a.v0, b.v0);
                if (w <= 0 || h <= 0) return 0.0f;
                const float i = w * h, ua = (a.u1 - a.u0) * (a.v1 - a.v0), ub = (b.u1 - b.u0) * (b.v1 - b.v0);
                return i / (std::max)(1e-9f, (std::min)(ua, ub));   // (of the smaller: a padded box vs the bare one)
            };
            std::vector<AppState::AnaBoxVerdict> kept;
            for (auto& n : v.special) { n.seen = now; kept.push_back(n); }
            for (const auto& old : app.manualAnaBoxes)
            {
                bool found = false, nowColour = false;
                for (const auto& n : v.special) if (overlap(old, n) > 0.5f) { found = true; break; }
                for (const auto& c : v.coloured) if (overlap(old, c) > 0.5f) { nowColour = true; break; }
                if (!found && !nowColour && (LONG)(now - old.seen) < 3000 && kept.size() < 32) kept.push_back(old);
            }
            app.manualAnaBoxes = std::move(kept);
            app.converter.CommitAnaSnapshot();   // (their positions are that frame's: the converter follows them from it)
        }
        if (v.kind < 0) return;
        if (v.kind == app.manualAnaKind)
        {
            app.manualAnaVotes = 0;
            if (v.kind == 2) app.manualAnaTint = v.tint;   // (the tint as it is now: a video's may drift)
            return;
        }
        // (A new picture's first verdict counts at once: no colour guessed
        // onto a black-and-white picture for 3 s after it opens.)
        if (++app.manualAnaVotes < 2 && (LONG)(now - app.manualAnaKeyAt) > 2500) return;
        app.manualAnaKind = v.kind;
        app.manualAnaTint = v.tint;
        app.manualAnaVotes = 0;
        if (v.boxes == 0) Log("Anaglyph: the whole frame is %s", kinds[v.kind]);
    }

    void UpdateInputChoice(AppState& app)
    {
        if (!app.autoInput || !app.weavingEnabled)
        {
            app.wholeDetectUntil = 0; app.inputCtxKey = 0;
            app.fsAutoWindow = app.fsCheckedWindow = nullptr; app.fsCheckUntil = 0;
            return;
        }
        const bool katanga = app.format == StereoFormat::Katanga || app.katanga.IsReceiving();
        // This display, in Fullscreen or the Looking Glass: Auto Stereo (in the
        // glass, the pictures under it).
        const bool thisDisplay = app.source == SourceKind::CaptureMonitor && !app.foreignDisplay && !katanga;
        const bool thisDisplayFull = thisDisplay && app.mode == OutputMode::Fullscreen;
        // A window picked before Automatic was: its 3D pictures, via Auto Stereo.
        if (app.source == SourceKind::CaptureWindow && app.sourceWindow && IsWindow(app.sourceWindow) &&
            MonitorFromWindow(app.sourceWindow, MONITOR_DEFAULTTONULL) == SrMonitor(app))
        {
            UseWindow(app, app.sourceWindow);
            return;
        }
        if (app.autoScopeWindow && (!IsWindow(app.autoScopeWindow) || app.mode != OutputMode::Fullscreen ||
                                    app.source != SourceKind::CaptureMonitor || app.foreignDisplay))
        {
            Log("Automatic Detection: no chosen window any more -- 3D pictures anywhere on the display");
            app.autoScopeWindow = nullptr;
        }
        if (thisDisplay && (thisDisplayFull || app.mode == OutputMode::LookingGlass))
        {
            app.wholeDetectUntil = 0;
            app.inputCtxKey = 1;
            if (thisDisplayFull) UpdateFullscreenAuto(app);
            else { app.fsAutoWindow = app.fsCheckedWindow = nullptr; }
            if (!app.fsAutoWindow && !app.autoDetect) SetAutoDetect(app, true);
            return;
        }
        app.fsAutoWindow = app.fsCheckedWindow = nullptr;
        app.fsCheckUntil = 0;
        if (app.autoStereo) EndAutoStereo(app, "Automatic Detection: another source");
        // What's being woven: a change starts a new whole-picture check.
        size_t key = std::hash<std::string>()(app.lastTestImagePath);
        key = key * 31 + (size_t)app.source;
        key = key * 31 + (size_t)app.mode;
        key = key * 31 + (size_t)(uintptr_t)app.sourceWindow;
        key = key * 31 + (size_t)(uintptr_t)app.sourceMonitor;
        key = key * 31 + (app.video.IsOpen() ? 1 : 0);
        if (key == 1) key = 2;   // (1 = the Auto Stereo case above)
        const DWORD now = GetTickCount();
        if (key != app.inputCtxKey)
        {
            app.inputCtxKey = key;
            app.wholeDetectUntil = now + 3000;
            app.wholeDetectNext  = now + 150;   // (let the first frames arrive)
        }
        if (!app.wholeDetectUntil || katanga) return;
        if ((LONG)(now - app.wholeDetectUntil) >= 0) { app.wholeDetectUntil = 0; return; }
        if ((LONG)(now - app.wholeDetectNext) < 0) return;
        app.wholeDetectNext = now + 400;
        if (DetectFormat(app))
        {
            app.wholeDetectUntil = 0;
            Log("Automatic Detection: whole picture -> %s", Profiles::FormatToString(app.format));
        }
    }

    // A Stereo 3D Input picked by hand (panel / tray / overlay menu): that's
    // the input now, instead of Automatic Detection. Doesn't turn weaving on
    // -- the choice applies when it is.
    void ManualInputChosen(AppState& app)
    {
        app.autoInput = false;
        app.wholeDetectUntil = 0;
        const HWND scope = app.autoScopeWindow;
        app.autoScopeWindow = nullptr;
        if (app.autoStereo)
        {
            EndAutoStereo(app, "input chosen by hand");
            // Automatic was showing a chosen window's pictures: now weave that
            // window in the chosen layout; otherwise the whole display.
            if (scope && IsWindow(scope)) UseWindow(app, scope);
            else if (app.weavingEnabled) ApplyMode(app);
        }
    }

    // Find the PID of the process publishing to Local\KatangaMappedFile via
    // the kernel handle table: open the mapping ourselves, look up the
    // kernel Object pointer behind our handle, then enumerate ALL handles
    // in the system and return the first PID that isn't ours sharing the
    // same Object. Same trick Process Explorer / Handle.exe use. Returns
    // 0 on any failure -- caller should fall back to generic topmost.
    DWORD FindKatangaPublisherPid()
    {
        using NtQSI_t = NTSTATUS (NTAPI*)(int, PVOID, ULONG, PULONG);
        static auto pNtQSI = (NtQSI_t)GetProcAddress(
            GetModuleHandleA("ntdll.dll"), "NtQuerySystemInformation");
        if (!pNtQSI)
        {
            Log("Katanga/publisher: NtQuerySystemInformation lookup failed");
            return 0;
        }

        HANDLE myMap = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\KatangaMappedFile");
        if (!myMap)
        {
            Log("Katanga/publisher: OpenFileMapping(myself) failed err=%lu", GetLastError());
            return 0;
        }
        const DWORD myPid = GetCurrentProcessId();

        // Buffer-grow loop. Typical handle-table size on a desktop is ~1-4 MB.
        std::vector<uint8_t> buf(64 * 1024);
        NTSTATUS st = STATUS_INFO_LENGTH_MISMATCH;
        ULONG    retLen = 0;
        for (int tries = 0; tries < 10 && st == STATUS_INFO_LENGTH_MISMATCH; ++tries)
        {
            st = pNtQSI(kSystemExtendedHandleInformation, buf.data(),
                        (ULONG)buf.size(), &retLen);
            if (st == STATUS_INFO_LENGTH_MISMATCH)
                buf.resize(buf.size() * 2);
        }
        if (!NT_SUCCESS(st))
        {
            Log("Katanga/publisher: NtQSI status=0x%08X retLen=%lu bufSize=%zu",
                (unsigned)st, retLen, buf.size());
            CloseHandle(myMap);
            return 0;
        }
        auto* info = reinterpret_cast<SYSTEM_HANDLE_INFORMATION_EX*>(buf.data());

        // First pass: find our own entry to capture the kernel Object pointer.
        PVOID    myObject = nullptr;
        ULONG_PTR myCount = 0;   // how many of OUR handles to the mapping (sanity)
        for (ULONG_PTR i = 0; i < info->NumberOfHandles; ++i)
        {
            const auto& e = info->Handles[i];
            if (e.UniqueProcessId == myPid
                && e.HandleValue   == (ULONG_PTR)myMap)
            {
                myObject = e.Object;
                ++myCount;
            }
        }
        CloseHandle(myMap);
        if (!myObject)
        {
            Log("Katanga/publisher: own handle not found in %llu-entry table (myPid=%lu myMap=%p)",
                (unsigned long long)info->NumberOfHandles, myPid, (void*)myMap);
            return 0;
        }

        // Second pass: find another PID with a handle to the same kernel object.
        int otherMatches = 0;
        DWORD firstOther = 0;
        for (ULONG_PTR i = 0; i < info->NumberOfHandles; ++i)
        {
            const auto& e = info->Handles[i];
            if (e.Object == myObject && e.UniqueProcessId != myPid)
            {
                ++otherMatches;
                if (!firstOther) firstOther = (DWORD)e.UniqueProcessId;
            }
        }
        Log("Katanga/publisher: table=%llu handles, ourMatches=%llu obj=%p otherMatches=%d firstOther=%lu",
            (unsigned long long)info->NumberOfHandles,
            (unsigned long long)myCount, myObject, otherMatches, firstOther);
        return firstOther;
    }

    // Pick the largest visible top-level window owned by the given PID.
    // Games sometimes have a small launcher window plus a big render window;
    // largest-area is a reliable heuristic for the actual game window.
    HWND FindMainWindowOfPid(DWORD pid)
    {
        struct Ctx { DWORD pid; HWND best; LONG bestArea; };
        Ctx ctx{ pid, nullptr, 0 };
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            DWORD wpid = 0;
            GetWindowThreadProcessId(h, &wpid);
            if (wpid != c->pid) return TRUE;
            if (!IsWindowVisible(h)) return TRUE;
            if (GetWindow(h, GW_OWNER)) return TRUE;
            RECT r{};
            if (!GetWindowRect(h, &r)) return TRUE;
            const LONG area = (r.right - r.left) * (r.bottom - r.top);
            if (area > c->bestArea) { c->bestArea = area; c->best = h; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
        return ctx.best;
    }

    // --- Katanga auto-receive --------------------------------------------
    //
    // With Settings::ReadKatangaAutoReceive on (default), a Katanga sender
    // appearing (a Katanga game, or a bridge such as markleoryan79's 3D
    // Slicer extension) switches SR Loom into Katanga receiving by itself;
    // when the sender stops, the previous format / mode / weaving state comes
    // back. The existing Katanga machinery (arm mode, publisher discovery,
    // lens hint) does the actual receiving.

    void EndKatangaAuto(AppState& app, const char* why)
    {
        if (!app.katangaAuto) return;
        app.katangaAuto = false;   // before ChangeFormat (it treats format changes as a user takeover)
        // Don't re-engage on whatever handle is still sitting in the mapping.
        app.katangaAutoBlocked = KatangaSource::PublisherPresent();
        Log("Katanga auto-receive: ending (%s) -> restoring previous setup", why);
        ChangeFormat(app, app.katangaPrevFormat);   // stops the receiver, lens back on
        app.mode = app.katangaPrevMode;
        if (!app.katangaPrevWeaving) SetWeaving(app, false);
        else                         ApplyMode(app);
    }

    void BeginKatangaAuto(AppState& app)
    {
        app.katangaPrevFormat  = app.format;
        app.katangaPrevMode    = app.mode;
        app.katangaPrevWeaving = app.weavingEnabled;
        app.katangaAuto        = true;
        app.katangaAutoStartMs = GetTickCount();
        EndAutoStereo(app, "Katanga sender took over");
        Log("Katanga auto-receive: sender detected -> switching to Katanga");
        app.mode = OutputMode::Fullscreen;
        ChangeFormat(app, StereoFormat::Katanga);    // arm: receiver on, lens off, window hidden
        if (!app.weavingEnabled) SetWeaving(app, true);   // SR session up (stays armed until frames)
    }

    // 500ms poll from the main loop.
    void PollKatangaAutoReceive(AppState& app)
    {
        const DWORD now = GetTickCount();
        if (now - app.katangaAutoLastPollMs < 500) return;
        app.katangaAutoLastPollMs = now;

        // katangaAutoBlocked: set when an auto session ends while a handle is
        // still published (sender stopped but left a stale value, or an idle
        // process holds the mapping and never sends frames). Stay blocked
        // until the mapping goes away, so we don't flip formats every few
        // seconds.
        const bool present = KatangaSource::PublisherPresent();
        if (!present) app.katangaAutoBlocked = false;

        if (app.katangaAuto)
        {
            if (!Settings::ReadKatangaAutoReceive() && !app.autoInput)
                EndKatangaAuto(app, "auto-receive turned off");
            else if (!app.katanga.IsReceiving() && now - app.katangaAutoStartMs > 3000)
                EndKatangaAuto(app, "no frames arrived");
            return;
        }
        if (app.format == StereoFormat::Katanga) return;   // manual Katanga (profile) already receives
        // (Automatic Detection always picks up a Katanga sender too.)
        if (!present || app.katangaAutoBlocked || (!Settings::ReadKatangaAutoReceive() && !app.autoInput)) return;
        BeginKatangaAuto(app);
    }

    // Where the Katanga weave goes while receiving: the whole SR display for
    // a fullscreen sender, or exactly over the sender window's client area
    // when it's windowed on the SR display (following it as it moves).
    void PlaceKatangaWeave(AppState& app)
    {
        RECT target = app.srDisplayRect;
        HWND pub = app.katangaPublisherWnd;
        if (pub && IsWindow(pub) && !IsIconic(pub) &&
            MonitorFromWindow(pub, MONITOR_DEFAULTTONULL) == SrMonitor(app) &&
            !IsWindowFullscreen(pub))
        {
            RECT cr{};
            POINT tl{ 0, 0 };
            RECT clipped{};
            if (GetClientRect(pub, &cr) && ClientToScreen(pub, &tl) &&
                cr.right > 0 && cr.bottom > 0)
            {
                const RECT client{ tl.x, tl.y, tl.x + cr.right, tl.y + cr.bottom };
                if (IntersectRect(&clipped, &client, &app.srDisplayRect))
                    target = clipped;
            }
        }
        RECT cur{};
        GetWindowRect(app.hwnd, &cur);
        if (!EqualRect(&cur, &target))
            SetWindowPos(app.hwnd, nullptr, target.left, target.top,
                         target.right - target.left, target.bottom - target.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // --- Auto Stereo: region management --------------------------------
    //
    // Stage 1 (foundation): regions are added by hand -- Ctrl+Alt+A over a
    // window weaves that window's client area in the current format; again
    // over a woven region removes it. Several regions, each with its own
    // format, can be active at once. Detection (image rectangles, format
    // spotters) builds on this.

    // Desktop rect -> capture-frame pixels (the capture covers the SR monitor).
    RECT ScreenToFrame(const AppState& app, const RECT& s)
    {
        MONITORINFO mi{ sizeof(mi) };
        if (!GetMonitorInfo(SrMonitor(app), &mi)) return {};
        const RECT& m = mi.rcMonitor;
        const double fw = app.capture.FrameWidth(), fh = app.capture.FrameHeight();
        if (fw <= 0 || fh <= 0 || m.right <= m.left || m.bottom <= m.top) return {};
        const double sx = fw / (m.right - m.left), sy = fh / (m.bottom - m.top);
        return { (LONG)((s.left - m.left) * sx + 0.5), (LONG)((s.top    - m.top) * sy + 0.5),
                 (LONG)((s.right - m.left) * sx + 0.5), (LONG)((s.bottom - m.top) * sy + 0.5) };
    }

    bool ClientScreenRect(HWND h, RECT& out)
    {
        RECT c{};
        if (!GetClientRect(h, &c)) return false;
        POINT tl{ c.left, c.top }, br{ c.right, c.bottom };
        if (!ClientToScreen(h, &tl) || !ClientToScreen(h, &br)) return false;
        out = { tl.x, tl.y, br.x, br.y };
        return out.right > out.left && out.bottom > out.top;
    }

    // Windows that must always show as plain 2D, never woven: SR Loom's own
    // panel, and terminals (text windows -- weaving only garbles them).
    bool IsKeep2DWindow(const AppState& app, HWND h)
    {
        if (!h) return false;
        if (h == app.gui.Hwnd()) return true;
        char cls[64] = {};
        GetClassNameA(h, cls, (int)sizeof(cls));
        return !strcmp(cls, "CASCADIA_HOSTING_WINDOW_CLASS") ||   // Windows Terminal
               !strcmp(cls, "ConsoleWindowClass") ||              // classic console
               !strcmp(cls, "mintty");                            // Git Bash / MSYS2
    }

    // Topmost visible, real (not ours / shell / click-through overlay)
    // top-level window under a screen point.
    HWND TopLevelWindowAt(POINT pt)
    {
        for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT))
        {
            if (!IsWindowVisible(h) || IsIconic(h) || IsCloaked(h) || IsOwnProcessWindow(h)) continue;
            if (GetWindowLongPtr(h, GWL_EXSTYLE) & WS_EX_TRANSPARENT) continue;
            RECT r{};
            if (!GetWindowRect(h, &r) || !PtInRect(&r, pt)) continue;
            char cls[64] = {};
            GetClassNameA(h, cls, (int)sizeof(cls));
            if (!strcmp(cls, "Progman") || !strcmp(cls, "WorkerW") ||
                !strcmp(cls, "Shell_TrayWnd") || !strcmp(cls, "Shell_SecondaryTrayWnd"))
                return nullptr;   // the desktop / taskbar is under the cursor
            return h;
        }
        return nullptr;
    }

    constexpr DWORD kScanIntervalMs = 100;   // auto-detect: time between scans (one takes ~10-30 ms) ...
    constexpr DWORD kConfirmScanMs  = 0;     // ... straight away when a find awaits its confirming scan
    // Scroll prediction: a scrolling picture is drawn this many frames of its
    // recent speed ahead (capture -> screen is about a frame behind the live
    // page), capped at kMaxLeadPx capture pixels.
    constexpr float kScrollLeadFrames = 1.0f;
    constexpr int   kMaxLeadPx        = 160;

    void EndAutoStereo(AppState& app, const char* why)
    {
        if (!app.autoStereo) return;
        app.autoStereo = false;
        app.autoInputCropped = false;
        app.autoGlass = {};
        app.autoDetect = false;
        app.regionWeaver.Clear();
        app.regionTrackers.clear();
        app.regionOcc.clear(); app.regionOccSig.clear(); app.regionOccArea.clear(); app.regrowAt.clear();
        app.regionFoundOcc.clear(); app.regionUncoveredAt.clear();
        app.regionTrunc.clear(); app.reacqMisses.clear(); app.reacqFirstMiss.clear(); app.scrolledAt.clear(); app.regrowTriedAt.clear();
        app.regionViewPos.clear();
        app.regionAnchorHist.clear();
        app.scanPending.clear(); app.autoMisses.clear();
        app.suppressed.clear(); app.suppressedWindows.clear();
        app.scanWantColour = false;
        app.pickPending = false;
        app.regionsDirty  = false;
        app.captureRebind = true;       // normal pipeline re-binds the weaver input
        app.weaver.LensEnable();
        app.autoStereoLensOn = false;
        Log("Auto Stereo: off (%s)", why);
        UpdateTaskbarCutout(app, true); // back to the normal full-window clip
    }

    // Start Auto Stereo: passthrough capture of the SR display + the
    // Fullscreen weave window (layered, click-through), clipped to the
    // regions -- none yet, so nothing is woven until one is added.
    void EnterAutoStereo(AppState& app)
    {
        if (app.autoStereo) return;
        UsePassthrough(app);
        if (app.mode != OutputMode::LookingGlass)   // (the glass: the pictures under it)
            app.mode = OutputMode::Fullscreen;
        // The global format only matters for the weave window's own
        // behaviour here (each region has its own). Katanga would put the
        // window into its hidden "waiting for a sender" state, and the
        // media-only formats don't apply to live capture.
        if (app.format == StereoFormat::Katanga || app.format == StereoFormat::LightField ||
            IsVRFormat(app.format))
            ChangeFormat(app, StereoFormat::HalfSBS);
        app.autoStereo = true;
        EnsureWeaving(app);
        Log("Auto Stereo: on");
    }

    void RemoveAutoRegion(AppState& app, int id, const char* why)
    {
        app.regionWeaver.Remove(id);
        app.regionTrackers.erase(id);
        app.regionOcc.erase(id); app.regionOccSig.erase(id); app.regionOccArea.erase(id);
        app.regrowAt.erase(id); app.autoMisses.erase(id); app.regionViewPos.erase(id);
        app.regionFoundOcc.erase(id); app.regionUncoveredAt.erase(id);
        app.regionTrunc.erase(id); app.reacqMisses.erase(id); app.reacqFirstMiss.erase(id); app.scrolledAt.erase(id); app.regrowTriedAt.erase(id);
        app.regionAnchorHist.erase(id);
        app.regionsDirty = true;
        Log("Auto Stereo: region %d removed (%s)", id, why);
    }

    RECT FrameToScreen(const AppState& app, const RECT& f)
    {
        MONITORINFO mi{ sizeof(mi) };
        if (!GetMonitorInfo(SrMonitor(app), &mi)) return {};
        const RECT& m = mi.rcMonitor;
        const double fw = app.capture.FrameWidth(), fh = app.capture.FrameHeight();
        if (fw <= 0 || fh <= 0) return {};
        const double sx = (m.right - m.left) / fw, sy = (m.bottom - m.top) / fh;
        return { m.left + (LONG)(f.left * sx + 0.5), m.top + (LONG)(f.top * sy + 0.5),
                 m.left + (LONG)(f.right * sx + 0.5), m.top + (LONG)(f.bottom * sy + 0.5) };
    }

    // Screen rect -> analysis pixels (the analyser's half-res image).
    RECT ScreenToAnalysis(const AppState& app, const RECT& s)
    {
        const RECT f = ScreenToFrame(app, s);
        const int k = ScreenAnalyzer::Scale();
        return { f.left / k, f.top / k, f.right / k, f.bottom / k };
    }

    // A window's frame as drawn (without Windows 10/11's invisible resize border).
    bool VisibleFrameRect(HWND h, RECT& r)
    {
        if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
            return r.right > r.left && r.bottom > r.top;
        return GetWindowRect(h, &r) && r.right > r.left && r.bottom > r.top;
    }

    // Real top-level windows in front of `host` (z-order), as screen rects:
    // they cover part of it, so those parts must not be woven. Skips our own
    // windows, click-through overlays and the taskbar (cut separately).
    void WindowsInFront(const AppState& app, HWND host, std::vector<RECT>& out)
    {
        out.clear();
        if (!host) return;
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfo(SrMonitor(app), &mi);
        const long long monArea = (long long)(mi.rcMonitor.right - mi.rcMonitor.left) *
                                  (mi.rcMonitor.bottom - mi.rcMonitor.top);
        const HWND fg = GetForegroundWindow();
        // From the background window watcher's snapshot (see WindowWatcher).
        const std::shared_ptr<const WinSnapshot> snap = g_winWatch.Latest();
        if (!snap) return;
        for (const WinInfo& w : snap->z)
        {
            const HWND h = w.h;
            if (h == host || w.desktop) break;
            if (w.own && !w.gui) continue;   // (our panel counts)
            if (w.ex & WS_EX_TRANSPARENT) continue;
            if (w.trayWnd) continue;
            RECT onMon{};
            if (!IntersectRect(&onMon, &w.frame, &mi.rcMonitor)) continue;
            // A monitor-sized OVERLAY-type window (layered / tool / no-activate:
            // the NVIDIA, Discord, Steam overlays) that isn't the one in use is
            // an invisible helper, not something covering the screen for real.
            // An ordinary maximised app (Discord over a browser) is in front
            // whether or not it has the focus -- it used to be skipped once
            // the focus moved (e.g. to SR Loom's panel), so the pictures
            // behind it were woven over it.
            const long long a = (long long)(onMon.right - onMon.left) * (onMon.bottom - onMon.top);
            const bool overlayType = (w.ex & (WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) != 0;
            if (a * 100 >= monArea * 95 && h != fg && overlayType) continue;
            out.push_back(onMon);
        }
    }

    // The scrolling viewport under a point: the deepest child window there
    // that is still a big part of the window (a browser's page area, not a
    // toolbar button). The host itself if there's none.
    HWND ViewportWindowAt(HWND host, POINT pt)
    {
        RECT hc{};
        if (!ClientScreenRect(host, hc)) return host;
        const long long hostArea = (long long)(hc.right - hc.left) * (hc.bottom - hc.top);
        HWND best = host;
        for (HWND cur = host;;)
        {
            POINT cp = pt;
            ScreenToClient(cur, &cp);
            HWND c = ChildWindowFromPointEx(cur, cp, CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT);
            if (!c || c == cur) break;
            RECT cr{};
            if (ClientScreenRect(c, cr) &&
                (long long)(cr.right - cr.left) * (cr.bottom - cr.top) * 5 >= hostArea)
                best = c;
            cur = c;
        }
        return best;
    }

    // A viewport window's area on the SR display, in analysis pixels.
    RECT ViewportAnalysisRect(const AppState& app, HWND view)
    {
        MONITORINFO mi{ sizeof(mi) };
        RECT s{}, c{};
        if (!view || !GetMonitorInfo(SrMonitor(app), &mi) || !ClientScreenRect(view, s) ||
            !IntersectRect(&c, &s, &mi.rcMonitor))
            return {};
        return ScreenToAnalysis(app, c);
    }

    // Per frame: follow tracked windows, drop regions whose window is gone,
    // and keep each region's "windows in front" cut-outs up to date.
    void UpdateAutoStereoRegions(AppState& app)
    {
        MONITORINFO mi{ sizeof(mi) };
        if (!GetMonitorInfo(SrMonitor(app), &mi)) return;
        std::vector<int> dead;
        std::map<HWND, std::vector<RECT>> inFront;   // per host, this frame
        const DWORD now = GetTickCount();
        // Window-move compensation: each region's anchor (its window, or the
        // viewport its picture is in) now vs when the displayed capture was
        // taken. The captured picture is drawn shifted by the difference, so
        // it sits where the live window is -- not a capture-delay behind it
        // with the real window showing through ahead of it.
        LARGE_INTEGER qpc{}, qpf{};
        QueryPerformanceCounter(&qpc);
        QueryPerformanceFrequency(&qpf);
        const int64_t now100 = (int64_t)((double)qpc.QuadPart * 1.0e7 / (double)qpf.QuadPart);
        const int64_t capT   = app.capture.IsActive() ? app.capture.LastFrameTime100ns() : 0;
        auto anchorAtCapture = [&](int id, const RECT& nowRect) -> RECT {
            auto& h = app.regionAnchorHist[id];
            h.push_back({ now100, nowRect });
            while (h.size() > 2 && h[1].first < now100 - 5'000'000) h.pop_front();   // ~0.5 s
            if (capT <= 0) return h.size() >= 2 ? h[h.size() - 2].second : nowRect;
            RECT best = h.front().second;
            for (const auto& e : h) { if (e.first <= capT) best = e.second; else break; }
            return best;
        };
        for (WeaveRegion& r : app.regionWeaver.Regions())
        {
            if (r.trackWindow)
            {
                RECT s{}, c{};
                if (!IsWindow(r.trackWindow) || !IsWindowVisible(r.trackWindow) ||
                    (r.host && IsIconic(r.host)) ||
                    !ClientScreenRect(r.trackWindow, s) || !IntersectRect(&c, &s, &mi.rcMonitor))
                {
                    dead.push_back(r.id);
                    continue;
                }
                // Crop where the window was in the displayed capture; draw
                // (and show) it where the window is now.
                RECT capC{};
                const RECT capS = anchorAtCapture(r.id, s);
                if (!IntersectRect(&capC, &capS, &mi.rcMonitor)) capC = c;
                const RECT f = ScreenToFrame(app, capC);
                const POINT wl{ c.left - capC.left, c.top - capC.top };
                if (!EqualRect(&c, &r.screen) || !EqualRect(&f, &r.frame) ||
                    wl.x != r.winLead.x || wl.y != r.winLead.y)
                {
                    r.screen  = c;
                    r.frame   = f;
                    r.winLead = wl;
                    r.lead    = { r.scrollLead.x + wl.x, r.scrollLead.y + wl.y };
                    app.regionsDirty = true;
                }
            }
            else if (r.trackContent && r.host && IsWindow(r.host) && !IsIconic(r.host))
            {
                // A picture in a window: follows the window's moves the same way.
                RECT vs{};
                if (ClientScreenRect(r.viewWindow ? r.viewWindow : r.host, vs))
                {
                    const RECT cap = anchorAtCapture(r.id, vs);
                    const POINT wl{ vs.left - cap.left, vs.top - cap.top };
                    if (wl.x != r.winLead.x || wl.y != r.winLead.y)
                    {
                        r.winLead = wl;
                        r.lead    = { r.scrollLead.x + wl.x, r.scrollLead.y + wl.y };
                        app.regionsDirty = true;
                    }
                }
            }
            else if (r.host && (!IsWindow(r.host) || IsIconic(r.host)))
            {
                dead.push_back(r.id);
                continue;
            }
            if (!r.host) continue;

            // Windows in front of the host, clipped to what we show of this
            // region (cut out of the weave).
            auto it = inFront.find(r.host);
            if (it == inFront.end())
            {
                std::vector<RECT> v;
                WindowsInFront(app, r.host, v);
                it = inFront.emplace(r.host, std::move(v)).first;
            }
            // (A scrolling picture can be anywhere in its viewport this frame
            // -- the GPU moves it -- so its occluders cover the viewport.)
            RECT occBound = r.screen;
            if (r.trackContent) { RECT vs{}; if (ClientScreenRect(r.viewWindow ? r.viewWindow : r.host, vs)) UnionRect(&occBound, &occBound, &vs); }
            std::vector<RECT> occ;
            uint64_t sig = 1469598103934665603ull;
            for (const RECT& w : it->second)
            {
                RECT i{};
                if (!IntersectRect(&i, &w, &occBound)) continue;
                occ.push_back(i);
                sig = (sig ^ ((uint64_t)(uint32_t)i.left << 32 | (uint32_t)i.top)) * 1099511628211ull;
                sig = (sig ^ ((uint64_t)(uint32_t)i.right << 32 | (uint32_t)i.bottom)) * 1099511628211ull;
            }
            if (sig != app.regionOccSig[r.id])
            {
                app.regionOccSig[r.id] = sig;
                app.regionOcc[r.id]    = std::move(occ);
                app.regionsDirty = true;
            }
            // How much of the image's viewport is covered. When that shrinks
            // (a window moved away, or the image's window came to the
            // front), the image may be bigger than the part we could see:
            // try re-growing it (ProcessAutoStereoAnalysis).
            if (r.trackContent)
            {
                RECT vs{};
                long long area = 0;
                if (ClientScreenRect(r.viewWindow ? r.viewWindow : r.host, vs))
                    for (const RECT& w : it->second)
                    {
                        RECT i{};
                        if (IntersectRect(&i, &w, &vs)) area += (long long)(i.right - i.left) * (i.bottom - i.top);
                    }
                auto prev = app.regionOccArea.find(r.id);
                if (prev != app.regionOccArea.end() && area < prev->second)
                    app.regrowAt[r.id] = now + 200;   // let the capture catch up first
                app.regionOccArea[r.id] = area;
                // ... and keep at it while less is covered than when the picture
                // was found: that one try could run on a frame from before the
                // window moved (the capture lags), see nothing new, and the box
                // stayed cut at the window's old edge -- a picture in Discord cut
                // off along the edge of a browser that had been in front of it.
                auto fo = app.regionFoundOcc.find(r.id);
                if (fo == app.regionFoundOcc.end()) app.regionFoundOcc[r.id] = area;
                else if (area < fo->second)
                {
                    if (!app.regionUncoveredAt.count(r.id)) app.regionUncoveredAt[r.id] = now;
                    if (!app.regrowAt.count(r.id) && app.asyncFinds.find(r.id) == app.asyncFinds.end())
                        app.regrowAt[r.id] = now + 700;
                }
                else app.regionUncoveredAt.erase(r.id);
            }
        }
        for (int id : dead)
            RemoveAutoRegion(app, id, "window closed / minimised / off the SR display");
    }

    // Swap Eyes and the anaglyph settings are global: apply them to every
    // region (a region's format is its own).
    void SyncRegionEyes(AppState& app)
    {
        for (WeaveRegion& r : app.regionWeaver.Regions())
        {
            // (A detected anaglyph keeps the colour pair / decode found for it.)
            const int combo = r.anaAuto ? r.anaglyphCombo : app.anaglyphCombo;
            const int mode  = r.anaAuto ? r.anaglyphMode  : app.anaglyphMode;
            // (Its detected eye order flips the panel's Swap Eyes.)
            const bool swap = app.swapEyes != (r.autoSwap && app.eyeOrderDetect);
            if (r.swapEyes != swap || r.anaglyphCombo != combo || r.anaglyphMode != mode)
            {
                r.swapEyes      = swap;
                r.anaglyphCombo = combo;
                r.anaglyphMode  = mode;
                app.regionsDirty = true;
            }
        }
    }

    // Formats a live region can't meaningfully use fall back to Half SBS.
    StereoFormat RegionFormatFor(StereoFormat fmt)
    {
        if (fmt == StereoFormat::Katanga || fmt == StereoFormat::LightField || IsVRFormat(fmt) ||
            fmt == StereoFormat::FrameSequential || fmt == StereoFormat::Pulfrich)
            return StereoFormat::HalfSBS;
        return fmt;
    }

    // Anaglyph colour pairs, for the log (the converter's combo order).
    const char* AnaComboName(int combo)
    {
        static const char* const k[] = { "red/cyan", "red/green", "red/blue", "green/magenta", "amber/blue", "cyan/magenta" };
        return (combo >= 0 && combo < 6) ? k[combo] : "?";
    }

    WeaveRegion NewRegion(const AppState& app, StereoFormat fmt, const AnaglyphKind* ana = nullptr,
                          const EyeOrder* eye = nullptr)
    {
        WeaveRegion r;
        r.format        = fmt;
        r.swapEyes      = app.swapEyes;
        r.anaglyphCombo = app.anaglyphCombo;
        r.anaglyphMode  = app.anaglyphMode;
        if (fmt == StereoFormat::Anaglyph && ana && ana->known)
        {
            // Its own colour pair, Mono for a black-and-white picture, its tint
            // for a one-colour one (AnalyseAnaPicture).
            r.anaglyphCombo = ana->combo;
            r.anaglyphMode  = ana->mode;
            r.anaTint       = ana->mode == 5 ? ana->tint : nullptr;
            r.anaAuto       = true;
            Log("Auto Stereo: anaglyph is %s, %s -> %s decode", AnaComboName(ana->combo),
                ana->mode == 3 ? "black-and-white" : ana->mode == 5 ? "one colour" : "colour",
                ana->mode == 3 ? "mono" : ana->mode == 5 ? "tinted mono" : "recovered colour");
        }
        // (Not for an anaglyph: red is the left eye by the convention anaglyphs
        // are made to -- the glasses fix it -- so a "detected" swap was only ever
        // a misjudgement.)
        if (eye && eye->known && app.eyeOrderDetect && fmt != StereoFormat::Anaglyph)
        {
            // The second half is the left eye (e.g. a cross-view SBS picture).
            r.autoSwap = eye->swap;
            r.swapEyes = app.swapEyes != r.autoSwap;
            Log("Auto Stereo: eye order %s (shift all %d, top %d, bottom %d)",
                eye->swap ? "SWAPPED" : "normal", eye->dxAll, eye->dxTop, eye->dxBottom);
        }
        return r;
    }

    // Whole-window region (the fallback when no image is found under the
    // cursor, or a player / fullscreen video found by the scanner). Returns
    // the region id, 0 if the window isn't on the SR display.
    int AddWindowRegion(AppState& app, HWND h, StereoFormat fmt, bool autoDetected = false,
                        const AnaglyphKind* ana = nullptr, const EyeOrder* eye = nullptr)
    {
        MONITORINFO mi{ sizeof(mi) };
        RECT s{}, c{};
        if (!IsWindow(h) || !GetMonitorInfo(SrMonitor(app), &mi) || !ClientScreenRect(h, s) ||
            !IntersectRect(&c, &s, &mi.rcMonitor))
            return 0;
        WeaveRegion r = NewRegion(app, fmt, ana, eye);
        r.trackWindow  = h;
        r.host         = GetAncestor(h, GA_ROOT);
        r.viewWindow   = h;
        r.autoDetected = autoDetected;
        r.screen       = c;
        r.frame        = ScreenToFrame(app, c);
        const int id = app.regionWeaver.Add(r);
        app.regionsDirty = true;
        Log("Auto Stereo: region %d = %swhole window %p '%s' (%ld,%ld %ldx%ld) as %s", id,
            autoDetected ? "[auto] " : "", (void*)h, WindowTitle(r.host).c_str(),
            c.left, c.top, c.right - c.left, c.bottom - c.top, Profiles::FormatToString(fmt));
        return id;
    }

    // Image region that follows its content as the page scrolls. `t` is a
    // tracker already Reset on the image (analysis px).
    int AddContentRegion(AppState& app, const RegionTracker& t, StereoFormat fmt,
                         HWND host, HWND view, bool autoDetected = false,
                         const AnaglyphKind* ana = nullptr, const EyeOrder* eye = nullptr)
    {
        const int s = ScreenAnalyzer::Scale();
        const RECT a = t.Rect(), v = t.Visible();
        WeaveRegion r = NewRegion(app, fmt, ana, eye);
        r.trackContent = true;
        r.trueAspect   = true;     // a picture on a page: Full SBS keeps its real shape
        r.host         = host;
        r.viewWindow   = view ? view : host;
        r.autoDetected = autoDetected;
        r.frame        = { a.left * s, a.top * s, a.right * s, a.bottom * s };
        r.vis          = { v.left * s, v.top * s, v.right * s, v.bottom * s };
        r.screen       = FrameToScreen(app, r.frame);
        const int id = app.regionWeaver.Add(r);
        app.regionTrackers[id] = t;
        app.regionsDirty = true;
        Log("Auto Stereo: region %d = %simage (%ld,%ld %ldx%ld) in '%s' as %s", id,
            autoDetected ? "[auto] " : "", r.screen.left, r.screen.top,
            r.screen.right - r.screen.left, r.screen.bottom - r.screen.top,
            WindowTitle(host).c_str(), Profiles::FormatToString(fmt));
        return id;
    }

    // Auto-detect: hand the latest (colour) frame and the windows on the SR
    // display to the background scanner.
    void StartAutoScan(AppState& app)
    {
        MONITORINFO mi{ sizeof(mi) };
        if (!GetMonitorInfo(SrMonitor(app), &mi)) return;
        std::vector<ScanWindow> wins;
        std::vector<RECT> above;   // screen rects of windows higher in the z-order
        // The windows, top first, from the background watcher's list (walking
        // them here -- visibility, cloaking, frame bounds -- asked the
        // compositor about every window and stalled the render loop).
        const std::shared_ptr<const WinSnapshot> snap = g_winWatch.Latest();
        if (!snap) return;
        for (const WinInfo& wi : snap->z)
        {
            if (wins.size() >= 8) break;
            const HWND h = wi.h;
            if (wi.desktop) break;                     // the desktop: nothing below
            if (wi.own && !wi.gui) continue;
            if (wi.ex & WS_EX_TRANSPARENT) continue;
            RECT onMon{};
            if (!IntersectRect(&onMon, &wi.frame, &mi.rcMonitor)) continue;
            // The taskbar, our panel and terminals are never scanned (but they
            // do hide what's beneath them).
            const bool shell = wi.trayWnd || wi.keep2D;
            RECT c{}, cm{};
            // (Automatic + a chosen window: only that one is looked in.)
            const bool inScope = !app.autoScopeWindow || h == app.autoScopeWindow;
            if (!shell && inScope && ClientScreenRect(h, c) && IntersectRect(&cm, &c, &mi.rcMonitor) &&
                cm.right - cm.left >= 160 && cm.bottom - cm.top >= 120)
            {
                ScanWindow sw;
                sw.host   = h;
                sw.view   = ViewportWindowAt(h, { (cm.left + cm.right) / 2, (cm.top + cm.bottom) / 2 });
                sw.bounds = ScreenToAnalysis(app, cm);
                long long covered = 0;
                for (const RECT& a : above)
                {
                    RECT i{};
                    if (!IntersectRect(&i, &a, &cm)) continue;
                    sw.covered.push_back(ScreenToAnalysis(app, i));
                    covered += (long long)(i.right - i.left) * (i.bottom - i.top);
                }
                if (covered * 10 < (long long)(cm.right - cm.left) * (cm.bottom - cm.top) * 9)
                    wins.push_back(std::move(sw));   // skip windows (almost) fully hidden
            }
            above.push_back(onMon);
        }

        const int k = ScreenAnalyzer::Scale();
        std::vector<RECT> exclude;
        std::vector<ScanVerify> verify;
        for (const WeaveRegion& r : app.regionWeaver.Regions())
        {
            const RECT a{ r.frame.left / k, r.frame.top / k, r.frame.right / k, r.frame.bottom / k };
            exclude.push_back(a);
            if (!r.autoDetected) continue;
            // Re-check only regions fully on show (a half-hidden SBS picture
            // can't be judged).
            if (r.trackContent)
            {
                auto t = app.regionTrackers.find(r.id);
                if (t == app.regionTrackers.end()) continue;
                const RECT v = t->second.Visible();
                if (!EqualRect(&v, &t->second.Rect())) continue;
            }
            auto oc = app.regionOcc.find(r.id);
            if (oc != app.regionOcc.end() && !oc->second.empty()) continue;
            verify.push_back({ r.id, a, r.format });
        }
        for (const auto& kv : app.suppressed) exclude.push_back(kv.second.Rect());

        app.scanner.Start(std::make_shared<LumaImage>(app.analysisImg), 0, std::move(wins),
                          std::move(exclude), std::move(verify));
        app.lastScanMs = GetTickCount();
        app.scanWantColour = false;
        app.screenChangedSinceScan = false;
    }

    // A confirmed scanner find -> a woven region.
    void AddAutoRegion(AppState& app, const ScanHit& hit, const LumaImage& scanImg)
    {
        if (hit.whole)
        {
            const HWND w = hit.view ? hit.view : hit.host;
            for (HWND s : app.suppressedWindows) if (s == w) return;
            AddWindowRegion(app, w, hit.format, true, &hit.ana, &hit.eye);
            return;
        }
        // Overlapping a picture of the same kind already woven in the same
        // window (a fifth of the smaller box, at least): it's more of that
        // picture (parts of a big one can each pass as 3D -- it was being
        // woven as separate strips). Weave the two as one. Pictures merely
        // side by side (a grid of thumbnails) stay separate. (Only while
        // still: during scrolling the scan's older frame and the tracked
        // position don't line up.)
        RECT want = hit.rect;
        int mergeId = 0;
        for (const WeaveRegion& r : app.regionWeaver.Regions())
        {
            if (!r.autoDetected || !r.trackContent || r.host != hit.host || r.format != hit.format) continue;
            auto tr = app.regionTrackers.find(r.id);
            if (tr == app.regionTrackers.end()) continue;
            if (std::fabs(tr->second.VelocityX()) + std::fabs(tr->second.VelocityY()) > 0.5f) continue;
            const RECT cur = tr->second.Rect();
            RECT i{};
            if (!IntersectRect(&i, &cur, &hit.rect)) continue;
            const double ia = (double)(i.right - i.left) * (i.bottom - i.top);
            const double ca = (double)(cur.right - cur.left) * (cur.bottom - cur.top);
            const double ha = (double)(hit.rect.right - hit.rect.left) * (hit.rect.bottom - hit.rect.top);
            if (ia < 0.2 * (std::min)(ca, ha)) continue;
            UnionRect(&want, &cur, &hit.rect);
            mergeId = r.id;
            break;
        }
        RegionTracker t;
        t.SetViewport(ViewportAnalysisRect(app, hit.view));
        t.Reset(scanImg, want);
        if (!t.Valid()) return;
        if (mergeId) RemoveAutoRegion(app, mergeId, "joined with more of the same picture");
        // The scan ran on an older frame: catch the tracker up (the page may
        // have scrolled meanwhile).
        if (app.analysisImg.width == scanImg.width && app.analysisImg.height == scanImg.height &&
            !t.Track(app.analysisImg))
            return;
        AddContentRegion(app, t, hit.format, hit.host, hit.view, true, &hit.ana, &hit.eye);
    }

    float RectIoU(const RECT& a, const RECT& b)
    {
        RECT i{};
        if (!IntersectRect(&i, &a, &b)) return 0.0f;
        const double ia = (double)(i.right - i.left) * (i.bottom - i.top);
        const double ua = (double)(a.right - a.left) * (a.bottom - a.top) +
                          (double)(b.right - b.left) * (b.bottom - b.top) - ia;
        return ua > 0 ? (float)(ia / ua) : 0.0f;
    }

    // Finished scan: re-check verdicts for woven auto regions, and weave
    // finds that the previous scan also made (same window, place, format).
    void HandleScanResult(AppState& app)
    {
        ScanResult res;
        if (!app.scanner.Take(res)) return;
        if (!app.autoDetect || !res.image) return;   // turned off while it ran

        for (const auto& v : res.verified)
        {
            if (v.second) { app.autoMisses[v.first] = 0; continue; }
            if (++app.autoMisses[v.first] >= 3)
                RemoveAutoRegion(app, v.first, "no longer looks like a 3D image");
        }

        const int k = ScreenAnalyzer::Scale();
        std::vector<ScanHit> next;
        for (const ScanHit& h : res.hits)
        {
            // A clear-cut find is woven straight away; a marginal one waits for
            // the next scan to agree (same window, place and format).
            const bool strong = (h.format == StereoFormat::Anaglyph) ? h.score >= 0.35f : h.score >= 0.6f;
            bool confirmed = strong;
            for (const ScanHit& p : app.scanPending)
                if (p.host == h.host && p.format == h.format && p.whole == h.whole &&
                    RectIoU(p.rect, h.rect) >= 0.85f)
                {
                    confirmed = true;
                    break;
                }
            if (!confirmed) { next.push_back(h); continue; }
            // Still free? (a region may have been added since the scan started)
            bool taken = false;
            for (const WeaveRegion& r : app.regionWeaver.Regions())
            {
                const RECT a{ r.frame.left / k, r.frame.top / k, r.frame.right / k, r.frame.bottom / k };
                if (RectIoU(a, h.rect) > 0.3f) { taken = true; break; }
            }
            for (const auto& kv : app.suppressed)
                if (RectIoU(kv.second.Rect(), h.rect) > 0.3f) { taken = true; break; }
            if (!taken) AddAutoRegion(app, h, *res.image);
        }
        app.scanPending = std::move(next);

        // Log the scan's findings only when they change (the scanner runs
        // continuously while the screen changes).
        size_t key = res.hits.size() * 131 + res.log.size();
        for (const ScanHit& h : res.hits) key = key * 31 + (size_t)h.rect.left * 7 + (size_t)h.rect.top + (size_t)h.format;
        if (key != app.lastScanLogKey)
        {
            app.lastScanLogKey = key;
            Log("Auto Stereo scan: %zu rects judged, %zu 3D (%.0f ms)", res.log.size(), res.hits.size(), res.ms);
            int n = 0;
            for (const std::string& l : res.log)
                if (++n <= 12) Log("  %s", l.c_str());
        }
    }

    // The panel's "Auto Stereo" button.
    void SetAutoDetect(AppState& app, bool on)
    {
        if (on == app.autoDetect) return;
        if (on)
        {
            EnterAutoStereo(app);
            if (!app.autoStereo) return;
            app.autoDetect = true;
            app.scanPending.clear();
            app.lastScanMs = 0;
            app.screenChangedSinceScan = true;
            app.regionsDirty = true;   // keeps the render loop's analysis path awake
            Log("Auto Stereo: detection on (scanning the SR display for 3D images)");
            return;
        }
        app.autoDetect = false;
        app.scanPending.clear();
        app.suppressed.clear();
        app.suppressedWindows.clear();
        app.scanWantColour = false;
        std::vector<int> autoIds;
        for (const WeaveRegion& r : app.regionWeaver.Regions()) if (r.autoDetected) autoIds.push_back(r.id);
        for (int id : autoIds) RemoveAutoRegion(app, id, "detection turned off");
        Log("Auto Stereo: detection off");
        if (app.regionWeaver.Empty() && !app.pickPending)
        {
            EndAutoStereo(app, "detection turned off");
            SetWeaving(app, false);
        }
    }

    // Start a background image-rectangle search around `old` (analysis px)
    // for region `id`: re-acquire (a lost picture) or re-grow (more of it came
    // into view). Works on a copy of the current analysis image.
    void LaunchAsyncFind(AppState& app, int id, bool regrow, const RECT& old, const RECT& bounds)
    {
        if (app.asyncFinds.count(id)) return;
        auto img = std::make_shared<LumaImage>();
        img->width  = app.analysisImg.width;
        img->height = app.analysisImg.height;
        img->pixels = app.analysisImg.pixels;   // luma only (the search needs no colour)
        const int cx = (old.left + old.right) / 2, cy = (old.top + old.bottom) / 2;
        AppState::AsyncFind j;
        j.regrow = regrow;
        j.startedAt = GetTickCount();
        j.old    = old;
        j.img    = img;
        j.fut    = std::async(std::launch::async, [img, cx, cy, bounds]() {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            RECT r{};
            const bool ok = FindImageRect(*img, cx, cy, bounds, r);
            return std::make_pair(ok, r);
        });
        app.asyncFinds.emplace(id, std::move(j));
    }

    // Apply finished background searches. A re-acquired picture is re-learnt
    // on the image it was found in, then caught up to the current frame; a
    // re-grown one only if the new rectangle contains the old and is bigger.
    void PollAsyncFinds(AppState& app)
    {
        std::vector<int> lost;
        for (auto it = app.asyncFinds.begin(); it != app.asyncFinds.end();)
        {
            AppState::AsyncFind& j = it->second;
            if (j.fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { ++it; continue; }
            const auto res = j.fut.get();
            const int id = it->first;
            auto tr = app.regionTrackers.find(id);
            if (tr != app.regionTrackers.end())
            {
                RegionTracker& t = tr->second;
                const RECT old = j.old, found = res.second;
                const double oa = (double)(old.right - old.left) * (old.bottom - old.top);
                const double na = res.first ? (double)(found.right - found.left) * (found.bottom - found.top) : 0.0;
                const bool sameImage = app.analysisImg.width == j.img->width && app.analysisImg.height == j.img->height;
                if (!j.regrow)
                {
                    // Re-acquired (a playing video whose content no longer
                    // tracks, perhaps scrolled too). Same size = the same
                    // picture, maybe moved: take it. A different size is more
                    // likely a different crop of it (video frames vary) -- keep
                    // the one we have, and only give in after 3 in a row.
                    // Same size, or the found box holds the old one (more of the
                    // picture is showing): take it. Inside the old one (part of
                    // a video frame looked like a picture edge): the picture's
                    // still there -- keep its box, re-learnt on this frame. Only
                    // a box that doesn't really overlap (another picture, or
                    // none) counts against it; 3 in a row and it's dropped.
                    const int ow = old.right - old.left, oh = old.bottom - old.top;
                    const int fw = found.right - found.left, fh = found.bottom - found.top;
                    const bool sameSize = res.first && std::abs(fw - ow) <= (std::max)(4, ow * 8 / 100) &&
                                          std::abs(fh - oh) <= (std::max)(4, oh * 8 / 100);
                    RECT inter{};
                    const double ia = (res.first && IntersectRect(&inter, &old, &found))
                                    ? (double)(inter.right - inter.left) * (inter.bottom - inter.top) : 0.0;
                    const double iou = (oa + na - ia) > 0 ? ia / (oa + na - ia) : 0.0;
                    constexpr int tol = 3;
                    const bool holdsOld = res.first && found.left <= old.left + tol && found.top <= old.top + tol &&
                                          found.right >= old.right - tol && found.bottom >= old.bottom - tol;
                    const bool insideOld = res.first && ia >= 0.95 * na && na >= 0.3 * oa;
                    int& misses = app.reacqMisses[id];
                    if (sameSize || (holdsOld && iou >= 0.5))
                    {
                        misses = 0;
                        t.Reset(*j.img, found);
                        app.regionTrunc.erase(id);   // (re-judge its cut-off edges)
                        if (sameImage && !t.Track(app.analysisImg)) lost.push_back(id);
                    }
                    else if (insideOld)
                    {
                        misses = 0;
                        t.Reset(*j.img, old);
                        if (sameImage) t.Track(app.analysisImg);
                    }
                    else
                    {
                        Log("Auto Stereo: region %d re-find didn't match (%dx%d -> %s%dx%d, overlap %.2f), miss %d",
                            id, ow, oh, res.first ? "" : "none ", fw, fh, iou, misses + 1);
                        // (Dropped after 3 in a row AND a while: mid-scroll a few
                        // quick misses in a row are normal -- dropping then made the
                        // picture blink out and come back as a new region.)
                        const DWORD nowMs = GetTickCount();
                        if (misses++ == 0) app.reacqFirstMiss[id] = nowMs;
                        if (misses >= 3 && nowMs - app.reacqFirstMiss[id] >= 400) lost.push_back(id);
                    }
                }
                else
                {
                    constexpr int tol = 3;
                    const bool contains = res.first && found.left <= old.left + tol && found.top <= old.top + tol &&
                                          found.right >= old.right - tol && found.bottom >= old.bottom - tol;
                    // Only the same picture extended the way scrolling (or a
                    // window moving off it) reveals it: same left and right
                    // edges (more above / below), or same top and bottom. A
                    // box that grew every way at once is something else -- a
                    // whole grid of pictures, the page.
                    const int tw = (std::max)(3, (int)(old.right - old.left) * 3 / 100);
                    const int th = (std::max)(3, (int)(old.bottom - old.top) * 3 / 100);
                    const bool sameCols = std::abs(found.left - old.left) <= tw && std::abs(found.right - old.right) <= tw;
                    const bool sameRows = std::abs(found.top - old.top) <= th && std::abs(found.bottom - old.bottom) <= th;
                    if (contains && na >= 1.05 * oa && (sameCols || sameRows))
                    {
                        t.Reset(*j.img, found);
                        if (sameImage) t.Track(app.analysisImg);
                        app.regionTrunc.erase(id);   // (still cut off somewhere? judged again next frame)
                        Log("Auto Stereo: region %d grew to the whole image (%ld,%ld)-(%ld,%ld) analysis px",
                            id, found.left, found.top, found.right, found.bottom);
                    }
                }
            }
            // (A re-grow run on a frame from after the uncovering has had its
            // look: whatever it found, the covered area now is the new
            // baseline -- no more retries unless more is uncovered.)
            if (j.regrow)
            {
                auto ua = app.regionUncoveredAt.find(id);
                auto oa = app.regionOccArea.find(id);
                if (ua != app.regionUncoveredAt.end() && (LONG)(j.startedAt - ua->second) >= 400 && oa != app.regionOccArea.end())
                {
                    app.regionFoundOcc[id] = oa->second;
                    app.regionUncoveredAt.erase(ua);
                }
            }
            it = app.asyncFinds.erase(it);   // (ready: destroying the future doesn't block)
        }
        for (int id : lost) RemoveAutoRegion(app, id, "image scrolled away / changed");
    }

    // New analysed frame: resolve a pending pick, move content regions,
    // re-grow uncovered ones, and feed the auto-detect scanner.
    void ProcessAutoStereoAnalysis(AppState& app)
    {
        app.autoTimeReadbackMs = 0;
        const bool needAnalysis = app.pickPending || !app.regionTrackers.empty() ||
                                  app.autoDetect || !app.suppressed.empty();
        if (!needAnalysis) return;

        uint64_t frameId = 0;
        const auto tRb0 = std::chrono::steady_clock::now();
        const bool fresh = app.analyzer.Latest(app.analysisImg, frameId, app.analysisWaitNewest);
        app.autoTimeReadbackMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tRb0).count();
        const int s = ScreenAnalyzer::Scale();

        if (app.pickPending)
        {
            // A pick waits (briefly) for a frame with colour, so the
            // saved images and the classifier see the red/cyan channels too.
            const bool waitColour = !app.analysisImg.hasColour() &&
                                    GetTickCount() - app.pickStartMs < 400;
            if (fresh && !waitColour)
            {
                // Point and viewport bounds in analysis pixels.
                const RECT pf = ScreenToFrame(app, { app.pickPoint.x, app.pickPoint.y,
                                                     app.pickPoint.x + 1, app.pickPoint.y + 1 });
                const HWND view = ViewportWindowAt(app.pickWindow, app.pickPoint);
                RECT bounds = ViewportAnalysisRect(app, view);
                if (IsRectEmpty(&bounds)) bounds = { 0, 0, app.analysisImg.width, app.analysisImg.height };
                RECT found{};
                const bool gotRect = FindImageRect(app.analysisImg, pf.left / s, pf.top / s, bounds, found);
                // Which format? Judge the picture (or, with none found, the
                // whole viewport); the panel's format is the fallback.
                const RECT judged = gotRect ? found : bounds;
                char diag[512] = "";
                StereoFormat guess{}; float score = 0;
                StereoScores sc;
                AnaglyphKind kind;
                EyeOrder eye;
                const bool looks3D = ClassifyStereo(app.analysisImg, judged, guess, score, diag, sizeof(diag), &sc, &kind, &eye);
                StereoFormat fmt = app.pickFormat;
                const char* why = "panel format";
                if (looks3D) { fmt = guess; why = "detected"; }
                else
                {
                    // You asked for 3D here, so take the clearer of the two
                    // layouts even below the automatic threshold.
                    const int jw = judged.right - judged.left, jh = judged.bottom - judged.top;
                    if (sc.sbs >= 0.2f && sc.sbs >= 1.5f * sc.tab)
                    {
                        fmt = ((float)(jw / 2) / (float)(std::max)(jh, 1) >= 1.1f) ? StereoFormat::FullSBS : StereoFormat::HalfSBS;
                        why = "best guess";
                    }
                    else if (sc.tab >= 0.2f && sc.tab >= 1.5f * sc.sbs)
                    {
                        fmt = ((float)jw / (float)(std::max)(jh / 2, 1) >= 2.6f) ? StereoFormat::HalfTAB : StereoFormat::FullTAB;
                        why = "best guess";
                    }
                }
                Log("Auto Stereo pick: %s (%s; sbs=%.2f tab=%.2f) -- %s", Profiles::FormatToString(fmt), why,
                    sc.sbs, sc.tab, diag);
                if (app.pickDebug)
                {
                    // Debug pick: save what the finder saw next to the exe.
                    wchar_t exe[MAX_PATH] = {};
                    GetModuleFileNameW(nullptr, exe, MAX_PATH);
                    std::wstring dir(exe);
                    dir = dir.substr(0, dir.find_last_of(L'\\')) + L"\\autostereo_debug";
                    CreateDirectoryW(dir.c_str(), nullptr);
                    wchar_t name[64];
                    swprintf_s(name, L"\\pick_%lu.bmp", GetTickCount());
                    const std::wstring path = dir + name;
                    const POINT pa{ pf.left / s, pf.top / s };
                    const bool saved = SaveLumaDebugBmp(app.analysisImg, bounds, gotRect ? &found : nullptr,
                                                        pa, path.c_str());
                    // Plus a clean copy (no markers) usable as an offline
                    // test case, and the red/cyan channels.
                    const std::wstring base = path.substr(0, path.size() - 4);
                    SaveLumaDebugBmp(app.analysisImg, bounds, nullptr, pa, (base + L"_raw.bmp").c_str(), false);
                    SaveColourDebugBmp(app.analysisImg, (base + L"_rc.bmp").c_str());
                    Log("Auto Stereo DEBUG pick: point=(%ld,%ld) bounds=(%ld,%ld)-(%ld,%ld) found=%d rect=(%ld,%ld)-(%ld,%ld) colour=%d saved=%d '%ls'",
                        pa.x, pa.y, bounds.left, bounds.top, bounds.right, bounds.bottom, (int)gotRect,
                        found.left, found.top, found.right, found.bottom, (int)app.analysisImg.hasColour(),
                        (int)saved, path.c_str());
                }
                if (gotRect)
                {
                    RegionTracker t;
                    t.SetViewport(bounds);
                    t.Reset(app.analysisImg, found);
                    AddContentRegion(app, t, fmt, app.pickWindow, view, false, &kind, &eye);
                }
                else
                {
                    Log("Auto Stereo: no image rectangle under the cursor -- using the whole window");
                    AddWindowRegion(app, view ? view : app.pickWindow, fmt, false, &kind, &eye);
                }
                app.pickPending = false;
                if (!app.autoDetect) app.scanWantColour = false;   // the pick's colour request
            }
            else if (GetTickCount() - app.pickStartMs > 600)
            {
                Log("Auto Stereo: no analysed frame in time -- using the whole window");
                AddWindowRegion(app, app.pickWindow, app.pickFormat);
                app.pickPending = false;
                if (!app.autoDetect) app.scanWantColour = false;
            }
        }

        // Background image-rectangle searches (re-acquire a lost picture,
        // re-grow an uncovered one) that have finished: apply them. They run
        // off the render thread -- each is a full search over a window
        // (tens of ms), which used to show as a hitch.
        PollAsyncFinds(app);

        if (!fresh) return;
        const DWORD now = GetTickCount();
        std::vector<int> lost;
        // Each picture's viewport and window move first (in order), then all
        // of them tracked on the new frame AT ONCE, one per core -- a tracker
        // only reads the image. With several pictures on screen, tracking them
        // one after another was most of Auto Stereo's time on this thread.
        std::map<int, RECT> views;
        std::map<int, char> trackedOk;   // id -> Track() result (absent: held this frame)
        {
            std::vector<std::pair<int, RegionTracker*>> todo;
            for (auto& kv : app.regionTrackers)
            {
                WeaveRegion* reg = nullptr;
                for (WeaveRegion& r : app.regionWeaver.Regions()) if (r.id == kv.first) { reg = &r; break; }
                if (!reg) continue;
                const RECT view = ViewportAnalysisRect(app, reg->viewWindow);
                views[kv.first] = view;
                // Window moved since the last track: carry the picture with it.
                auto lp = app.regionViewPos.find(kv.first);
                if (lp != app.regionViewPos.end() && !IsRectEmpty(&view))
                {
                    const int dx = view.left - lp->second.x, dy = view.top - lp->second.y;
                    if (dx || dy) kv.second.Shift(dx, dy);
                }
                if (!IsRectEmpty(&view)) app.regionViewPos[kv.first] = { view.left, view.top };
                // A re-acquire search is out for this one: hold it where it was.
                auto job = app.asyncFinds.find(kv.first);
                if (job != app.asyncFinds.end() && !job->second.regrow) continue;
                kv.second.SetViewport(view);
                todo.push_back({ kv.first, &kv.second });
            }
            std::vector<char> ok(todo.size(), 0);
            std::vector<size_t> idx(todo.size());
            for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
            const LumaImage& img = app.analysisImg;
            if (todo.size() > 1)
                std::for_each(std::execution::par, idx.begin(), idx.end(),
                              [&](size_t i) { ok[i] = todo[i].second->Track(img) ? 1 : 0; });
            else if (todo.size() == 1)
                ok[0] = todo[0].second->Track(img) ? 1 : 0;
            for (size_t i = 0; i < todo.size(); ++i) trackedOk[todo[i].first] = ok[i];
        }
        for (auto& kv : app.regionTrackers)
        {
            RegionTracker& t = kv.second;
            WeaveRegion* reg = nullptr;
            for (WeaveRegion& r : app.regionWeaver.Regions()) if (r.id == kv.first) { reg = &r; break; }
            if (!reg) { lost.push_back(kv.first); continue; }
            const RECT view = views[kv.first];
            auto job = app.asyncFinds.find(kv.first);
            auto tk = trackedOk.find(kv.first);
            if (tk == trackedOk.end()) continue;   // (held for a re-acquire)
            const RECT bounds = IsRectEmpty(&view) ? RECT{ 0, 0, app.analysisImg.width, app.analysisImg.height } : view;
            if (!tk->second)
            {
                // Content AND surroundings changed -- e.g. a playing video
                // that's also being scrolled. Look (in the background) for an
                // image rectangle of about the same size around where it was.
                // Where it should be NOW: lost mid-scroll, the old place holds
                // something else (or nothing) -- searching there dropped pictures
                // and made them anew a moment later (jumpy boxes). Ahead by its
                // scroll speed (analysis px a frame) for the frames the search lags.
                RECT predicted = t.Rect();
                OffsetRect(&predicted, (int)std::lround(t.VelocityX() * 2.0f), (int)std::lround(t.VelocityY() * 2.0f));
                LaunchAsyncFind(app, kv.first, false, predicted, bounds);
                continue;
            }
            // More of the image may have come into view.
            auto rg = app.regrowAt.find(kv.first);
            if (rg != app.regrowAt.end() && (LONG)(now - rg->second) >= 0)
            {
                app.regrowAt.erase(rg);
                if (job == app.asyncFinds.end()) LaunchAsyncFind(app, kv.first, true, t.Rect(), bounds);
            }
            // While a picture scrolls (and once more just after it stops), look
            // for the whole of it every ~0.3 s: scrolling reveals the rest of
            // a picture that was found only partly on show -- also where a
            // page's own header / toolbar hid it, not just the viewport edge.
            if (std::fabs(t.VelocityX()) + std::fabs(t.VelocityY()) > 0.5f) app.scrolledAt[kv.first] = now;
            {
                auto sa = app.scrolledAt.find(kv.first);
                const DWORD tried = app.regrowTriedAt.count(kv.first) ? app.regrowTriedAt[kv.first] : 0;
                if (sa != app.scrolledAt.end() && now - sa->second < 600 && now - tried >= 300 &&
                    app.asyncFinds.find(kv.first) == app.asyncFinds.end())
                {
                    app.regrowTriedAt[kv.first] = now;
                    LaunchAsyncFind(app, kv.first, true, t.Rect(), bounds);
                }
            }
            // A picture found half scrolled into view ends at the viewport's
            // edge -- the rest was hidden. As scrolling brings that edge in
            // (more of the picture showing beyond it), look for the whole
            // picture again: each time it has come in another ~10% further.
            // An edge well inside the viewport that still doesn't grow is the
            // picture's real edge.
            if (!IsRectEmpty(&view))
            {
                const RECT a = t.Rect();
                auto tr = app.regionTrunc.find(kv.first);
                if (tr == app.regionTrunc.end())
                {
                    constexpr int tol = 3;
                    AppState::Truncation tn;
                    tn.edges = (a.top <= view.top + tol ? 1 : 0) | (a.bottom >= view.bottom - tol ? 2 : 0) |
                               (a.left <= view.left + tol ? 4 : 0) | (a.right >= view.right - tol ? 8 : 0);
                    app.regionTrunc.emplace(kv.first, tn);
                }
                else if (tr->second.edges && app.asyncFinds.find(kv.first) == app.asyncFinds.end())
                {
                    const int e = tr->second.edges;
                    int gap = 0;
                    if (e & 1) gap = (std::max)(gap, (int)(a.top - view.top));
                    if (e & 2) gap = (std::max)(gap, (int)(view.bottom - a.bottom));
                    if (e & 4) gap = (std::max)(gap, (int)(a.left - view.left));
                    if (e & 8) gap = (std::max)(gap, (int)(view.right - a.right));
                    const int step = (std::max)(8, (int)(std::max)(a.bottom - a.top, a.right - a.left) / 10);
                    if (gap >= tr->second.lastGap + step)
                    {
                        tr->second.lastGap = gap;
                        LaunchAsyncFind(app, kv.first, true, a, bounds);
                    }
                    if (gap > (int)(std::max)(view.bottom - view.top, view.right - view.left) / 3)
                        tr->second.edges = 0;   // well inside and never grew: it's the real edge
                }
            }
            const RECT a = t.Rect(), v = t.Visible(), vw = t.ViewRect();
            const RECT f{ a.left * s, a.top * s, a.right * s, a.bottom * s };
            const RECT vf{ v.left * s, v.top * s, v.right * s, v.bottom * s };
            const RECT cf{ vw.left * s, vw.top * s, vw.right * s, vw.bottom * s };
            // Scrolling: draw the picture where it will be once this frame is
            // on screen (kScrollLeadFrames of its recent speed ahead), so it
            // keeps up with the live page instead of trailing it.
            auto leadOf = [&](float v) {
                const int px = (int)std::lround(v * kScrollLeadFrames) * s;
                return (std::max)(-kMaxLeadPx, (std::min)(kMaxLeadPx, px));
            };
            const POINT lead{ leadOf(t.VelocityX()), leadOf(t.VelocityY()) };
            if (!EqualRect(&f, &reg->frame) || !EqualRect(&vf, &reg->vis) || !EqualRect(&cf, &reg->clip) ||
                lead.x != reg->scrollLead.x || lead.y != reg->scrollLead.y)
            {
                reg->frame  = f;
                reg->vis    = vf;
                reg->clip   = cf;
                reg->scrollLead = lead;
                reg->lead   = { lead.x + reg->winLead.x, lead.y + reg->winLead.y };
                reg->screen = FrameToScreen(app, f);
                app.regionsDirty = true;
            }
        }
        for (int id : lost) RemoveAutoRegion(app, id, "image scrolled away / changed");

        // Auto finds the user removed stay suppressed while they're on screen.
        for (auto it = app.suppressed.begin(); it != app.suppressed.end();)
        {
            it->second.SetViewport({});
            if (it->second.Track(app.analysisImg)) ++it;
            else it = app.suppressed.erase(it);
        }

        if (app.autoDetect && app.analysisImg.hasColour() && !app.scanner.Busy())
            StartAutoScan(app);
    }

    // Ctrl+Alt+A. Over a woven region: remove it (an auto-detected one stays
    // un-woven while it's on screen). Otherwise: weave the image under the
    // cursor in the current format -- found on the next analysed frame,
    // falling back to the whole window -- entering Auto Stereo if needed.
    // Removing the last region leaves Auto Stereo (unless detection is on).
    void ToggleAutoRegionUnderCursor(AppState& app)
    {
        POINT pt{};
        GetCursorPos(&pt);
        for (const WeaveRegion& r : app.regionWeaver.Regions())
        {
            if (!PtInRect(&r.screen, pt)) continue;
            const int id = r.id;
            if (r.autoDetected)
            {
                if (r.trackContent)
                {
                    auto t = app.regionTrackers.find(id);
                    if (t != app.regionTrackers.end()) app.suppressed[app.nextSuppressId++] = t->second;
                }
                else if (r.trackWindow)
                    app.suppressedWindows.push_back(r.trackWindow);
            }
            RemoveAutoRegion(app, id, "removed by user");
            if (app.regionWeaver.Empty() && !app.autoDetect)
            {
                EndAutoStereo(app, "last region removed");
                SetWeaving(app, false);
            }
            return;
        }

        HWND h = TopLevelWindowAt(pt);
        if (!h) { Log("Auto Stereo: no window under the cursor"); return; }
        MONITORINFO mi{ sizeof(mi) };
        RECT s{}, c{};
        if (!GetMonitorInfo(SrMonitor(app), &mi) || !ClientScreenRect(h, s) ||
            !IntersectRect(&c, &s, &mi.rcMonitor))
        {
            Log("Auto Stereo: window %p is not on the SR display", (void*)h);
            return;
        }

        EnterAutoStereo(app);

        // Find the image under the cursor on the next analysed frame
        // (ProcessAutoStereoAnalysis); the whole window is the fallback.
        app.pickPending = true;
        app.pickPoint   = pt;
        app.pickWindow  = h;
        app.pickStartMs = GetTickCount();
        app.pickFormat  = RegionFormatFor(app.format);
        app.scanWantColour = true;   // read back red/cyan too (anaglyph test)
        app.regionsDirty = true;   // keeps the render loop's analysis path awake
    }

    // The SR display's CURRENT refresh rate (Hz), re-read every few seconds
    // (the mode can change). Per monitor: DWM's global timing reports the
    // Overlay / frame-limiter hooks other programs inject into every D3D app
    // (SR Loom included). A global frame limit in one of them caps the weave.
    std::string LoadedPresentHooks()
    {
        static const wchar_t* const kHooks[] = {
            L"RTSSHooks64.dll", L"RTSSHooks.dll",                // RivaTuner Statistics Server
            L"GameOverlayRenderer64.dll",                         // Steam overlay
            L"DiscordHook64.dll", L"DiscordOverlay64.dll",        // Discord overlay
            L"nvspcap64.dll",                                     // NVIDIA overlay / ShadowPlay
            L"igo64.dll", L"overlay64.dll", L"uplay_overlay64.dll",
            L"OWClient.dll", L"ow-graphics-hook64.dll",           // Overwolf
            L"graphics-hook64.dll",                               // OBS game capture
            L"ReShade64.dll", L"dxgi_hook.dll" };
        std::string found;
        for (const wchar_t* m : kHooks)
            if (GetModuleHandleW(m))
            {
                char n[64]; WideCharToMultiByte(CP_UTF8, 0, m, -1, n, sizeof(n), nullptr, nullptr);
                if (!found.empty()) found += ", ";
                found += n;
            }
        return found;
    }

    // primary display, which may be a 60 Hz screen next to a 160 Hz SR one.
    double SrRefreshHz(const AppState& app, bool now = false)
    {
        static double s_hz = 60.0;
        static DWORD  s_checked = 0;
        if (now || GetTickCount() - s_checked > 3000)
        {
            s_checked = GetTickCount();
            MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
            DEVMODEW dm{}; dm.dmSize = sizeof(dm);
            if (GetMonitorInfoW(SrMonitor(app), &mi) &&
                EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
            {
                if ((double)dm.dmDisplayFrequency != s_hz)
                    Log("SR display refresh: %lu Hz (current mode)", dm.dmDisplayFrequency);
                s_hz = (double)dm.dmDisplayFrequency;
            }
        }
        return s_hz;
    }

    // Pace for the SR display's CURRENT refresh rate, not its maximum: it can be
    // set lower (e.g. 120 Hz), or changed while SR Loom runs.
    void PaceForSrRefresh(AppState& app, bool now)
    {
        const double hz = SrRefreshHz(app, now);
        if (hz > 1.0 && std::abs(hz - app.paceHz) > 0.5)
        {
            app.renderer.SetTargetRefreshHz(hz);
            Log("Pacing for the SR display's %.0f Hz", hz);
            app.paceHz = hz;
        }
    }

    // Windows' display settings changed (WM_DISPLAYCHANGE, settled): a monitor
    // added / removed / rearranged, or a new resolution or refresh rate. Follow
    // the SR display: where it is now, which output to pace on, its refresh.
    void HandleDisplayChange(AppState& app)
    {
        RECT rc{};
        // (Asks the SR session. Without one -- weaving off for a while -- the
        // next weave start reads it, as it always has.)
        const bool known = app.weaver.GetSRDisplayRect(rc);
        Log("Display settings changed%s", known ? "" : " (SR display position re-read when weaving starts)");
        if (known && !EqualRect(&rc, &app.srDisplayRect))
        {
            Log("SR display is now at (%ld,%ld)-(%ld,%ld)", rc.left, rc.top, rc.right, rc.bottom);
            app.srDisplayRect = rc;
            app.srMmPerPx = MonitorMmPerPx(SrMonitor(app));
            if (app.weavingEnabled)
            {
                // (Capturing the SR display: capture it where it is now.)
                if (app.source == SourceKind::CaptureMonitor && !app.foreignDisplay) UsePassthrough(app);
                ApplyMode(app);
            }
        }
        app.renderer.SetVBlankMonitor(SrMonitor(app));
        PaceForSrRefresh(app, true);
    }

    void RenderFrame(AppState& app)
    {
        if (!app.weavingEnabled || !app.renderer.IsValid())
        {
            { AppUnlock unlock; Sleep(10); }
            return;
        }

        if (IsIconic(app.hwnd))
        {
            { AppUnlock unlock; Sleep(10); }
            return;
        }

        // WindowOverlay mode: another app can pop above our overlay and
        // fully hide the woven output. Skip the weave + Present when the
        // SR SDK's Window2 reports the overlay region is fully occluded --
        // saves GPU work + avoids presenting frames the user can't see.
        // (Fullscreen / Katanga modes are HWND_TOPMOST so don't get covered.)
        //
        // HYSTERESIS: during a window drag DWM transiently reports our
        // overlay as not-visible while it catches up on the z-order, which
        // would cause us to skip-then-render every few frames -- visible as
        // flicker (especially in anaglyph where the converter path is more
        // sensitive to frame skips than the identity-SBS fast path). Only
        // actually pause after several consecutive occluded reports, and
        // reset immediately on any "visible".
        static int occludedFrames = 0;
        if (app.mode == OutputMode::WindowOverlay)
        {
            RECT cr{}; GetClientRect(app.hwnd, &cr);
            if (cr.right > cr.left && cr.bottom > cr.top)
            {
                if (app.weaver.IsWindowPartVisible(app.hwnd,
                                                   cr.right - cr.left,
                                                   cr.bottom - cr.top))
                    occludedFrames = 0;
                else
                    ++occludedFrames;
                // ~10 frames @ 60fps = ~166ms before we trust the "occluded"
                // signal. Faster monitors will trip the threshold sooner in
                // wall-clock terms, which is fine -- it's still well past
                // any drag-induced transient.
                if (occludedFrames >= 10)
                {
                    { AppUnlock unlock; Sleep(10); }
                    return;
                }
            }
            else
            {
                occludedFrames = 0;   // degenerate rect -> reset
            }
        }
        else
        {
            occludedFrames = 0;   // mode change -> reset
        }

        // Pace to the display before grabbing the newest frame (lowest latency).
        app.prof.t0 = std::chrono::steady_clock::now();
        if (app.prof.lastEnd.time_since_epoch().count() != 0)
            app.prof.outside += std::chrono::duration<double, std::milli>(app.prof.t0 - app.prof.lastEnd).count();
        g_stall.Mark("pace wait (frame latency)");
        { AppUnlock unlock; app.renderer.WaitForFrame(); }   // (the lock is free while the loop waits: see g_appLock)
        // Weaving live capture into a bit-blt window: pace on the capture
        // itself -- wake the moment a new frame lands (it's produced by the
        // composition we'd otherwise be sleeping through in DwmFlush); if
        // none lands within half a refresh (a still screen), sync to the
        // compositor as before, so head tracking stays smooth. Other sources
        // keep DwmFlush pacing (see Present below).
        app.paceOnCapture = (!app.renderer.IsFlipModel() || app.renderer.IsDComp()) && app.capture.IsActive() &&
                            app.source != SourceKind::TestImage && !app.video.IsOpen() &&
                            !app.katanga.IsReceiving();
        // Waits until ~3 ms before the next refresh: a new frame (a game's,
        // a video's) that arrives by then is woven and shown at that very
        // refresh -- the earliest it can be (3 ms is left for the weave and
        // present). A still screen: nothing arrives, the loop goes on.
        // (Only while there's time to weave it before the display's next
        // refresh -- counted from where that refresh actually is, not from
        // now: a loop that began late in the refresh waited its full 3 ms and
        // ran past it, the frame shown twice.)
        g_stall.Mark("pace wait (capture)");
        {
            const auto tc0 = std::chrono::steady_clock::now();
            const double toBlank = app.renderer.MsToNextVBlank();
            const double budget = toBlank >= 0.0 ? toBlank - 3.0 : 1000.0 / SrRefreshHz(app) - 3.0;
            if (app.paceOnCapture && (budget < 1.0 || ![&] { AppUnlock unlock; return app.capture.WaitForNewFrame((DWORD)budget); }()))
                app.paceOnCapture = false;   // still screen (or no time): sync to the compositor as usual this loop
            app.prof.lastCaptureWaitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tc0).count();
        }
        g_stall.Mark("render loop");
        app.prof.tWait = std::chrono::steady_clock::now();
        app.gpuTimer.Begin();

        // Anti-lag pattern (BlueSkyDefender's: max-frames-in-flight=1, sample input
        // AFTER the wait): tracking source-window position / cursor hover gets read
        // here so it's the freshest state by the time the frame reaches the panel.
        // Reading them BEFORE WaitForFrame would let them go up-to-one-refresh stale.
        // Safety net for the source-switch hand-off: if the capture died or the
        // source moved off live capture, never leave the mode change deferred.
        if (app.captureWarmup && (!app.capture.IsActive() ||
                                  GetTickCount() - app.captureWarmupStartMs > 400))
            EndCaptureWarmup(app);
        UpdateOverlayTracking(app);     // follow the source window in overlay mode
        UpdateLoupeOwnDrag(app);        // our own move/resize of the looking glass (no modal loop)
        UpdateLoupeInteractivity(app);  // hover the chrome to grab/move the looking glass

        // Resolve the current source frame: the test image, or a capture frame.
        ID3D11ShaderResourceView* srcSRV = nullptr;
        int srcW = 0, srcH = 0;
        bool srcEncoded = false;   // srcSRV holds sRGB values read as UNORM: the capture's own frame (zero-copy) or a Katanga texture
        bool capSizeChanged = false;
        bool gotFrame = false;   // a new capture frame arrived this iteration
        if (app.source == SourceKind::TestImage)
        {
            if (app.video.IsOpen())
            {
                // Decode the next video frame (no-op if the previous one is
                // still current) and bind its texture as this frame's source.
                if (app.video.Update(app.renderer.Context()))
                    app.captureRebind = true;   // first ever frame -> ensure weaver re-binds
                srcSRV = app.video.SRV();
                srcW   = app.video.Width();
                srcH   = app.video.Height();
            }
            else
            {
                srcSRV = app.weaver.SourceSRV();
                srcW   = app.weaver.SourceWidth();
                srcH   = app.weaver.SourceHeight();
            }
        }
        else if (app.capture.IsActive())
        {
            // SR-display passthrough: weave only the region beneath the viewer. A
            // foreign display (the picker) is woven whole — no crop.
            if (app.source == SourceKind::CaptureMonitor && !app.foreignDisplay && app.capture.FrameWidth() > 0)
            {
                // Auto Stereo crops per region itself, so it needs the whole frame.
                RECT r = app.autoStereo ? RECT{ 0, 0, 0, 0 } : PassthroughRegion(app, app.hwnd);
                app.capture.SetSourceRegion(r.left, r.top, r.right - r.left, r.bottom - r.top);
            }
            bool wgcGotFrame = false;
            { HitchWatch hw("capture update"); wgcGotFrame = app.capture.Update(capSizeChanged); }
            app.gpuTimer.Stamp(GpuFrameTimer::kCapture);

            // Source-switch hand-off: the new session's first frame is here
            // (or it's taking too long) -- switch the window to the new mode
            // now and re-crop to the new window in this same frame, so the
            // first thing drawn in the new geometry is the new source.
            if (app.captureWarmup &&
                (wgcGotFrame || GetTickCount() - app.captureWarmupStartMs > 250))
            {
                EndCaptureWarmup(app);
                if (app.source == SourceKind::CaptureMonitor && !app.foreignDisplay &&
                    app.capture.FrameWidth() > 0)
                {
                    // Auto Stereo crops per region itself, so it needs the whole frame.
                    RECT r = app.autoStereo ? RECT{ 0, 0, 0, 0 } : PassthroughRegion(app, app.hwnd);
                    app.capture.SetSourceRegion(r.left, r.top, r.right - r.left, r.bottom - r.top);
                    bool recropSizeChanged = false;
                    if (app.capture.Update(recropSizeChanged)) wgcGotFrame = true;
                    if (recropSizeChanged) capSizeChanged = true;
                }
            }

            // Track whether WGC is still delivering frames. An exclusive-fullscreen
            // app on the captured monitor freezes WGC (it sees DWM's last composed
            // frame and nothing new). For FOREIGN-DISPLAY monitor capture only, fall
            // back to DXGI Output Duplication in that case. We never use DXGI for the
            // SR display itself (it would capture our own woven overlay).
            const bool dxgiEligible = (app.source == SourceKind::CaptureMonitor)
                                      && app.foreignDisplay && app.sourceMonitor;
            if (wgcGotFrame) app.wgcStuck = 0;
            else if (dxgiEligible) ++app.wgcStuck;

            if (!app.dxgiActive && dxgiEligible && app.wgcStuck > 30)   // ~500ms at typical render cadence
            {
                if (app.captureDxgi.StartMonitor(app.sourceMonitor))
                {
                    app.dxgiActive = true;
                    app.captureRebind = true;
                    app.dxgiCheckCtr = 0;
                    Log("Capture: WGC stuck for %d ticks, switched to DXGI Output Duplication", app.wgcStuck);
                }
                app.wgcStuck = 0;
            }

            if (app.dxgiActive)
            {
                // Apply the same source region as WGC for foreign-display passthrough
                // (a foreign display is woven whole today, so this is a no-op pre-crop,
                // but matched to WGC's behaviour for symmetry).
                bool dxgiSizeChanged = false;
                gotFrame = app.captureDxgi.Update(dxgiSizeChanged);
                if (dxgiSizeChanged) capSizeChanged = true;
                srcSRV = app.captureDxgi.SRV();
                srcW   = app.captureDxgi.Width();
                srcH   = app.captureDxgi.Height();

                // Periodically check whether the captured monitor's foreground is still
                // covering it. When it stops (user alt-tabbed out, game quit), DXGI is
                // no longer needed and we'd rather be back on WGC (cheaper, supports
                // window exclusion if we switch source modes).
                if (++app.dxgiCheckCtr > 30)
                {
                    app.dxgiCheckCtr = 0;
                    if (!ForegroundCoversMonitor(app.sourceMonitor))
                    {
                        app.captureDxgi.Stop();
                        app.dxgiActive = false;
                        app.captureRebind = true;
                        Log("Capture: foreground no longer fullscreen on monitor, back to WGC");
                    }
                }
            }
            else
            {
                gotFrame = wgcGotFrame;
                // Zero-copy: the converter reads the captured frame itself when
                // it's the whole picture and the mode reads it cheaply. (Not the
                // weaver: its own sRGB decode, setShaderSRGBConversion, was left
                // out of its start-up picture -- washed out -- and switching it
                // stopped the converter's output being taken up.) Auto Stereo
                // crops per region from the copy.
                // (The converter's anaglyph mode as SetFormat below gives it.)
                const int convAnaMode = app.manualAnaKind == 1 ? 3 :
                                        (app.manualAnaKind == 2 && app.manualAnaTint) ? 5 : app.anaglyphMode;
                // (Recovered Colour too: it checks the frame for change before copying
                // it for itself -- Converter::Convert, directSrc.)
                const bool direct = !app.autoStereo && !app.capture.IsHdr() &&   /* (16-bit float frames: the copy -- no decoding to save) */
                                    (srw::Converter::CheapEncodedSource(app.format, convAnaMode) ||
                                    (app.format == StereoFormat::Anaglyph && convAnaMode == 4 && app.converter.RecoveredWantsDirect()));
                srcSRV = direct ? app.capture.DirectSRV(srcEncoded) : app.capture.SRV();
                srcW   = app.capture.Width();
                srcH   = app.capture.Height();
            }
        }

        // External-source format override: Katanga publishes the woven SBS
        // directly via its shared-texture handoff. Replace whatever the
        // Source resolved to. Source/Mode still control PLACEMENT (which
        // window the overlay tracks, fullscreen on the SR display, etc).
        if (app.format == StereoFormat::Katanga)
        {
            const bool wasReceiving = (app.katanga.SRV() != nullptr);
            const bool nowReceiving = app.katanga.Update();
            // Publisher swapped to a new texture (resize) while still
            // receiving: re-bind the weaver's input, or it keeps sampling
            // the released texture (frozen picture / use-after-free).
            if (nowReceiving && app.katanga.Generation() != app.katangaGeneration)
            {
                app.katangaGeneration = app.katanga.Generation();
                app.captureRebind = true;
            }
            if (nowReceiving != wasReceiving)
            {
                app.captureRebind = true;
                if (nowReceiving)
                {
                    // Game just started publishing -- discover the
                    // publisher's main window via the kernel handle table
                    // so we can z-pin above it, re-engage the lens (SR
                    // session has been alive the whole time, only the
                    // SwitchableLensHint was disabled during arm), show the
                    // window, and pin topmost so the game's own window
                    // can't pop above the weave when focused. Fall back to
                    // StartSR if the session was actually torn down.
                    const DWORD pubPid = FindKatangaPublisherPid();
                    app.katangaPublisherWnd = pubPid ? FindMainWindowOfPid(pubPid) : nullptr;
                    // Fallback when NT-API discovery fails (publisher in
                    // higher integrity level, protected process, etc.):
                    // use lastForeground (filtered to never be us, the
                    // shell, or a popup). Better than no Z-target at all.
                    if (!app.katangaPublisherWnd
                        && app.lastForeground && IsWindow(app.lastForeground))
                    {
                        app.katangaPublisherWnd = app.lastForeground;
                        Log("Katanga: publisher discovery failed, falling back to lastForeground=%p",
                            (void*)app.katangaPublisherWnd);
                    }
                    if (!app.weaver.HasWeaver())
                        StartSRSession(app);
                    app.weaver.LensEnable();
                    ShowWindow(app.hwnd, SW_SHOW);
                    SetWindowPos(app.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    Log("Katanga: reception started (%dx%d, publisher pid=%lu hwnd=%p)",
                        app.katanga.Width(), app.katanga.Height(),
                        pubPid, (void*)app.katangaPublisherWnd);
                }
                else
                {
                    // Game closed: drop back to arm-mode (lens off, window
                    // hidden, non-topmost). Render loop keeps polling the
                    // Katanga mapping so the next publishing game auto-
                    // engages without the user touching anything. Clear the
                    // backbuffer first so the panel doesn't briefly show the
                    // last woven frame as the window vanishes.
                    app.katangaPublisherWnd = nullptr;
                    app.renderer.BindAndClearBackBuffer();
                    app.renderer.Present(false);
                    app.weaver.LensDisable();
                    SetWindowPos(app.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    ShowWindow(app.hwnd, SW_HIDE);
                    app.katangaPlacedRect = {};
                    Log("Katanga: reception lost -> arm-mode (waiting for next game)");
                    // Auto-received session: the sender's gone, so put the
                    // user's previous setup back instead of staying armed.
                    if (app.katangaAuto) EndKatangaAuto(app, "sender stopped");
                    return;
                }
            }
            srcSRV   = app.katanga.SRV();
            srcEncoded = app.katanga.Encoded();   // (the original Katanga's plain 8-bit texture: decoded in the converter)
            srcW     = app.katanga.Width();
            srcH     = app.katanga.Height();
            gotFrame = nowReceiving;
            // Pin SR Loom's overlay directly above the publishing game's
            // main window each frame. Many DirectX games SetWindowPos
            // HWND_TOPMOST on themselves when activated, which would pop
            // them above us in the topmost band on a click-into-game --
            // forcing the user to alt-tab to SR Loom to push the weave
            // back up. Inserting just above the game's HWND specifically
            // (not generic HWND_TOPMOST) wins regardless of which band
            // the game is in. SWP_NOACTIVATE means we never steal focus.
            if (nowReceiving && app.katangaPublisherWnd
                && IsWindow(app.katangaPublisherWnd))
            {
                // NOTE: SetWindowPos's hWndInsertAfter is the window that ends
                // up directly ABOVE ours -- passing the game HWND (as this used
                // to) put the weave directly BELOW the game every frame, and
                // stripped our topmost flag if the game wasn't topmost. Instead:
                // only when the game has actually got above us (walk up from it
                // looking for our window), jump back to the top of the topmost
                // band. No per-frame z-order churn when the order is already right.
                bool weAreAbove = false;
                for (HWND h = GetWindow(app.katangaPublisherWnd, GW_HWNDPREV); h;
                     h = GetWindow(h, GW_HWNDPREV))
                    if (h == app.hwnd) { weAreAbove = true; break; }
                if (!weAreAbove)
                    SetWindowPos(app.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            else if (nowReceiving)
            {
                // Fallback when publisher discovery failed: generic topmost.
                SetWindowPos(app.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            if (nowReceiving) PlaceKatangaWeave(app);
        }

        // AUTO STEREO: weave each region with its own format through a
        // composite. The weave window is clipped to the regions (see
        // UpdateTaskbarCutout), so nothing outside them is shown woven.
        if (app.autoStereo)
        {
            if (app.source != SourceKind::CaptureMonitor || app.foreignDisplay || !app.capture.IsActive())
            {
                EndAutoStereo(app, "source is no longer the SR display");
            }
            else
            {
                { HitchWatch hw("Auto Stereo region/window update"); UpdateAutoStereoRegions(app); }
                SyncRegionEyes(app);
                { HitchWatch hw("Auto Stereo scan results"); HandleScanResult(app); }
                // Auto-detect: time for another scan? Only if the screen has
                // changed since the last one, or a find awaits confirmation.
                if (gotFrame) app.screenChangedSinceScan = true;
                if (app.autoDetect && !app.scanWantColour && !app.scanner.Busy() &&
                    GetTickCount() - app.lastScanMs >= (app.scanPending.empty() ? kScanIntervalMs : kConfirmScanMs) &&
                    (app.screenChangedSinceScan || !app.scanPending.empty()))
                    app.scanWantColour = true;
                // Screen analysis: feed frames while tracking image regions;
                // while a pick or a scan is pending, analyse the latest frame
                // even if nothing changed (a static page delivers no new
                // frames). A scan's frame also carries red/cyan.
                const bool analyse = app.capture.HasPicture() &&
                    ((gotFrame && (!app.regionTrackers.empty() || !app.suppressed.empty())) ||
                     app.pickPending || app.scanWantColour);
                if (analyse)
                    app.analyzer.Submit(app.capture.SRV(), app.capture.Width(), app.capture.Height(),
                                        app.scanWantColour);
                // While images are being followed, read this very frame back
                // (a short wait for the GPU) instead of one from 1-2 frames
                // ago: otherwise the woven picture trails behind scrolling.
                app.analysisWaitNewest = !app.gpuTracking && analyse && gotFrame && !app.regionTrackers.empty();
                {
                    // With GPU scroll tracking the CPU tracker (on frames read back a
                    // frame or two ago) only gives the GPU its starting point: its
                    // work -- 1-2 ms a frame -- runs after this frame is presented,
                    // not before its weave (it made frames miss the refresh).
                    if (app.gpuTracking) app.analysisDeferred = true;
                    else
                    {
                        const auto t0 = std::chrono::steady_clock::now();
                        { HitchWatch hw("Auto Stereo analysis/tracking/picks"); ProcessAutoStereoAnalysis(app); }
                        app.autoTimeAnalysisMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    }
                    app.gpuTimer.Stamp(GpuFrameTimer::kAnalysis);
                }
                // GPU scroll tracking: find each content picture in THIS
                // frame's analysis image on the GPU (relative to where the
                // CPU tracker last saw it); the crop, placement and cut-out
                // read the result there, the same frame.
                if (app.gpuTracking)
                {
                    std::vector<GpuTrackJob> jobs;
                    int slot = 0;
                    for (WeaveRegion& r : app.regionWeaver.Regions())
                    {
                        int want = -1;
                        auto t = app.regionTrackers.find(r.id);
                        if (t != app.regionTrackers.end() && t->second.Valid() &&
                            slot < GpuTracker::kMaxSlots && !t->second.RowProfile().empty())
                        {
                            want = slot++;
                            // The window may have moved since the CPU tracker's
                            // frame: its position is known now, so start the
                            // GPU search (and the crop, and the viewport clip)
                            // from where the window is this frame.
                            RECT base = t->second.Rect(), view = t->second.ViewRect();
                            const RECT vnow = ViewportAnalysisRect(app, r.viewWindow);
                            auto lp = app.regionViewPos.find(r.id);
                            if (lp != app.regionViewPos.end() && !IsRectEmpty(&vnow))
                            {
                                const int dx = vnow.left - lp->second.x, dy = vnow.top - lp->second.y;
                                OffsetRect(&base, dx, dy);
                                OffsetRect(&view, dx, dy);
                            }
                            jobs.push_back({ want, base, view, &t->second.RowProfile(), &t->second.ColProfile() });
                            const int k = ScreenAnalyzer::Scale();
                            const RECT f{ base.left * k, base.top * k, base.right * k, base.bottom * k };
                            const RECT c{ view.left * k, view.top * k, view.right * k, view.bottom * k };
                            if (!EqualRect(&f, &r.frame) || !EqualRect(&c, &r.clip))
                            {
                                r.frame  = f;
                                r.clip   = c;
                                r.screen = FrameToScreen(app, f);
                                app.regionsDirty = true;
                            }
                        }
                        if (r.gpuSlot != want) { r.gpuSlot = want; app.regionsDirty = true; }
                    }
                    if (!jobs.empty()) { HitchWatch hw("GPU tracking dispatch"); app.gpuTracker.Run(app.analyzer.LumaSRV(), jobs); }
                    app.gpuTimer.Stamp(GpuFrameTimer::kTracking);
                    app.regionWeaver.SetGpuResults(jobs.empty() ? nullptr : app.gpuTracker.ResultsSRV(),
                                                   ScreenAnalyzer::Scale());
                }
                const bool anyRegion = !app.regionWeaver.Empty();
                // Lens only while there's something to weave.
                if (anyRegion != app.autoStereoLensOn)
                {
                    if (anyRegion) app.weaver.LensEnable(); else app.weaver.LensDisable();
                    app.autoStereoLensOn = anyRegion;
                }
                // Looking Glass: the weave shows just the part under the glass
                // (capture frame px), cut out of the whole-screen composite.
                const bool lg = app.mode == OutputMode::LookingGlass;
                RECT glass{};
                if (lg)
                {
                    RECT cr{};
                    POINT tl{ 0, 0 };
                    GetClientRect(app.hwnd, &cr);
                    ClientToScreen(app.hwnd, &tl);
                    OffsetRect(&cr, tl.x, tl.y);
                    glass = ScreenToFrame(app, cr);
                }
                const bool glassMoved = lg && !EqualRect(&glass, &app.autoGlass);
                const bool rebuild = app.regionsDirty || app.captureRebind || gotFrame || glassMoved;
                if (app.regionsDirty) UpdateTaskbarCutout(app, true);   // re-clip to the new regions
                if (anyRegion && rebuild)
                {
                    bool resized = false;
                    HitchWatch hwBuild("region crops/conversion");
                    const bool built = app.regionWeaver.Build(app.capture.Texture(), app.capture.SRV(), app.capture.SRVFormat(),
                                                              app.capture.Width(), app.capture.Height(), resized);
                    bool cropResized = false;
                    if (built && lg && app.regionWeaver.CropComposite(glass, cropResized))
                    {
                        if (cropResized || resized || app.captureRebind || !app.autoInputCropped)
                        {
                            app.weaver.SetInputView(app.regionWeaver.CroppedSRV(),
                                                    app.regionWeaver.CroppedPerEyeWidth(),
                                                    app.regionWeaver.CroppedHeight(),
                                                    app.regionWeaver.CompositeFormat());
                            app.captureRebind = false;
                            app.autoInputCropped = true;
                        }
                        app.autoGlass = glass;
                    }
                    else if (built && !lg && (resized || app.captureRebind || app.autoInputCropped))
                    {
                        app.weaver.SetInputView(app.regionWeaver.CompositeSRV(),
                                                app.regionWeaver.PerEyeWidth(),
                                                app.regionWeaver.Height(),
                                                app.regionWeaver.CompositeFormat());
                        app.captureRebind = false;
                        app.autoInputCropped = false;
                    }
                }
                app.regionsDirty = false;
                goto skipSourcePipeline;
            }
        }

        // FAST PATH: a live source already in side-by-side layout (full OR half SBS,
        // no eye swap, no convergence shift) is identical to what the converter would
        // output -- so feed the captured texture STRAIGHT to the weaver and skip the
        // conversion pass entirely. Lowest latency / least GPU contention, which
        // matters for games. Works for Half SBS too because the weaver only cares about
        // per-eye-width / height in the source texture; the weaver's internal bilinear
        // sample naturally un-squeezes anamorphic half-SBS when sampling to the woven
        // output's per-eye dimensions. The capture texture updates in place each frame
        // so the weaver's input view only needs re-registering on a rebind or a resize.
        // A video test source is "live" too -- its texture's content changes
        // every frame even though source == TestImage, so the converter has to
        // re-run continuously.
        // Lytro Light Field: per-frame head-tracked plenoptic sampler.
        // The LFPRenderer owns the SBS view -- the source pipeline below
        // is skipped entirely. The user's eye-mm positions are scaled to
        // make typical head leans cover the full (tiny) Lytro aperture --
        // physically not 1:1 but perceptually MUCH more usable since the
        // F01 aperture is just 3.4 mm vs ~62 mm IPD.
        if (app.format == StereoFormat::LightField && app.lfpRenderer.HasData())
        {
            float l[3] = {}, r[3] = {};
            float lu = 0, lv = 0, ru = 0, rv = 0;
            if (app.weaver.GetPredictedEyePositions(l, r))
            {
                // Window-position-aware view angle: SR tracker reports
                // eye position in mm relative to the SR display CENTRE.
                // If we render the LFP in a windowed sub-rect (not full
                // SR display), the user perceives the WINDOW centre, not
                // the display centre, as the "reference point". Subtract
                // the window-centre-to-display-centre offset so the
                // captured scene shifts the way it would for a real
                // window onto a real scene.
                double windowOffsetMmX = 0.0, windowOffsetMmY = 0.0;
                if (app.srMmPerPx > 0.0 && app.mode != OutputMode::Fullscreen)
                {
                    RECT wr{};
                    if (GetWindowRect(app.hwnd, &wr))
                    {
                        const double winCxPx = 0.5 * (wr.left + wr.right);
                        const double winCyPx = 0.5 * (wr.top  + wr.bottom);
                        const double dispCxPx = 0.5 * (app.srDisplayRect.left + app.srDisplayRect.right);
                        const double dispCyPx = 0.5 * (app.srDisplayRect.top  + app.srDisplayRect.bottom);
                        windowOffsetMmX = (winCxPx - dispCxPx) * app.srMmPerPx;
                        windowOffsetMmY = (winCyPx - dispCyPx) * app.srMmPerPx;
                    }
                }
                const float eyeLX = l[0] - (float)windowOffsetMmX;
                const float eyeLY = l[1] - (float)windowOffsetMmY;
                const float eyeRX = r[0] - (float)windowOffsetMmX;
                const float eyeRY = r[1] - (float)windowOffsetMmY;

                const double apertureMmRadius =
                    0.5 * app.lfpRenderer.ApertureDiameterMetres() * 1e3;
                if (apertureMmRadius > 0.0)
                {
                    // GUI-tunable: head-lean distance (mm) that drives
                    // the aperture sample to its edge. Min = aperture
                    // radius (true physical 1:1, microscopic on F01),
                    // default 30, larger = even more amplified parallax.
                    // Note: V is negated -- SR tracker reports head y
                    // positive = up; sensor / image y is positive = down.
                    // So head-up should sample the upper part of the
                    // aperture, which in image coords is negative.
                    const double leanMm = (std::max)((double)app.lfpHeadLeanMm,
                                                      apertureMmRadius);
                    auto clampUnit = [](float v) { return v >  1.0f ? 1.0f
                                                       : (v < -1.0f ? -1.0f : v); };
                    lu = clampUnit((float)( eyeLX / leanMm));
                    lv = clampUnit((float)(-eyeLY / leanMm));
                    ru = clampUnit((float)( eyeRX / leanMm));
                    rv = clampUnit((float)(-eyeRY / leanMm));
                }
            }
            else
            {
                // No eye-track data yet: static L/R sub-aperture extremes
                // so the user sees stereo immediately rather than 2 identical views.
                lu = -0.7f; ru = +0.7f;
            }
            app.lfpRenderer.Run(lu, lv, ru, rv);
            app.weaver.SetInputView(app.lfpRenderer.OutputSRV(),
                                    app.lfpRenderer.OutputPerEyeWidth(),
                                    app.lfpRenderer.OutputHeight(),
                                    app.lfpRenderer.OutputFormat());
            app.captureRebind = false;
            goto skipSourcePipeline;
        }

        {
        const bool liveSource  = (app.source != SourceKind::TestImage)
                              || app.video.IsOpen()
                              || app.format == StereoFormat::Katanga;
        // Quilt is included so the converter re-runs every frame even on a
        // static test image -- its L/R view indices follow the head position.
        const bool temporalFmt = (app.format == StereoFormat::Pulfrich ||
                                  app.format == StereoFormat::FrameSequential ||
                                  app.format == StereoFormat::Quilt);
        const bool noConv      = (app.convergence < 1e-4f && app.convergence > -1e-4f);
        // HalfSBS is identity-passable (each source half = each eye, no
        // transform). FullSBS now does a vertical centre-crop in the
        // shader so a letterboxed 32:9 source projects correctly --
        // can't skip the converter for it.
        const bool halfSbsFmt  = (app.format == StereoFormat::HalfSBS);
        const bool sbsFmt      = (app.format == StereoFormat::FullSBS ||
                                  app.format == StereoFormat::HalfSBS);
        // Katanga always publishes FullSBS-layout pixels; route it through the
        // identity fast path so the weaver's internal bilinear handles any
        // scaling between the game's render res and the output window/display.
        const bool katangaFmt  = (app.format == StereoFormat::Katanga);
        // (+ detected eye order; + the original Katanga's right-eye-first layout)
        const bool katangaRightFirst = katangaFmt && app.katanga.IsReceiving() && app.katanga.RightEyeFirst();
        const bool swapNow = (app.swapEyes != ((app.autoEyeSwap && app.fsAutoWindow) || app.manualEyeSwap)) != katangaRightFirst;
        // (Not the capture's own frame -- zero-copy: that goes through the
        // converter, which decodes it.)
        const bool identitySBS = liveSource && (halfSbsFmt || katangaFmt) && !swapNow && noConv && !srcEncoded;

        if (identitySBS)
        {
            if (srcSRV && srcW > 0 && srcH > 0 && (app.captureRebind || capSizeChanged))
            {
                DXGI_FORMAT srvFmt = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                if      (katangaFmt)     srvFmt = app.katanga.Format();
                else if (app.dxgiActive) srvFmt = app.captureDxgi.SRVFormat();
                else                     srvFmt = app.capture.SRVFormat();
                app.weaver.SetInputView(srcSRV, srcW / 2, srcH, srvFmt);
                app.captureRebind = false;
            }
        }
        // Otherwise convert the source into a side-by-side texture and feed THAT to
        // the weaver. Re-run the (potentially heavy) conversion ONLY when its output
        // can change: a NEW live-capture frame arrived, Pulfrich is temporal, or a
        // setting changed (captureRebind). When the captured content is unchanged we
        // skip the convert and re-weave the cached SBS — the weave still tracks the
        // head every frame, but we don't burn GPU re-converting identical pixels.
        else if (srcSRV && srcW > 0 && srcH > 0 && ((liveSource && gotFrame) || temporalFmt || IsVRFormat(app.format) || app.captureRebind))
        {
            // Quilt: pick L/R view indices from the SR head-pose tracker. Each eye
            // gets the view whose virtual-camera horizontal position matches that
            // eye's lateral position. Without tracking data (cold start), the
            // user-set defaults (centred pair) just stay in place.
            //
            // Mapping: a head sweep of ±HEAD_FULL_SWEEP_MM (~33cm half-sweep at a
            // 60cm viewing distance ≈ 58° angular range, the Looking Glass Portrait
            // native FOV) traverses the full quilt index range. Each eye's lateral
            // offset (head.x ± IOD/2 -- here we just use head.x since the tracker
            // gives us the head centre, not the eye positions; an IOD of ~64mm at
            // a typical 660mm full-sweep adds a ~5-view default baseline between
            // the eyes, which is the parallax that gives 3D depth).
            if (app.format == StereoFormat::Quilt)
            {
                const double HEAD_FULL_SWEEP_MM = 660.0;   // ±330mm covers all views
                // EMA is lighter than the legacy head-centre path because
                // getPredictedEyePositions returns latency-corrected /
                // already-filtered positions -- extra smoothing here just
                // adds perceived lag. Fallback head-pose path still uses
                // the heavier filter (alpha 0.10) under the same constant
                // since head pose is noisier than weaver-predicted eyes.
                const double EMA_ALPHA_EYES = 0.30;        // per-eye (predicted)
                const double EMA_ALPHA_HEAD = 0.10;        // head-centre fallback
                const float  BLEND_SNAP     = 0.08f;       // shimmer-kill near integer boundaries
                const int    total              = app.quiltCols * app.quiltRows;

                // Prefer the weaver's predicted per-eye positions (latency-
                // corrected, real IPD -- no need to assume a 64mm IOD).
                // Falls back to head-centre +/- assumed-IOD if for any reason
                // the weaver isn't ready.
                float lEye[3] = {}, rEye[3] = {};
                double leftEyeX = 0.0, rightEyeX = 0.0;
                bool haveEyes = false;
                bool fromPredicted = false;
                if (app.weaver.GetPredictedEyePositions(lEye, rEye))
                {
                    leftEyeX  = (double)lEye[0];
                    rightEyeX = (double)rEye[0];
                    haveEyes  = true;
                    fromPredicted = true;
                }
                else
                {
                    double hp[3] = {}, ho[3] = {};
                    if (app.weaver.GetHeadPose(hp, ho))
                    {
                        const double IOD_MM_DEFAULT = 64.0;
                        leftEyeX  = hp[0] - IOD_MM_DEFAULT * 0.5;
                        rightEyeX = hp[0] + IOD_MM_DEFAULT * 0.5;
                        haveEyes  = true;
                    }
                }

                if (haveEyes)
                {
                    // EMA per eye -- kills tracker noise without flattening
                    // real IPD differences (which now flow straight through).
                    // Predicted positions are already filtered upstream, so a
                    // higher alpha (less smoothing) keeps response snappy.
                    const double ema = fromPredicted ? EMA_ALPHA_EYES : EMA_ALPHA_HEAD;
                    if (!app.eyesEMAInit)
                    {
                        app.leftEyeXEMA  = leftEyeX;
                        app.rightEyeXEMA = rightEyeX;
                        app.eyesEMAInit  = true;
                    }
                    else
                    {
                        app.leftEyeXEMA  = ema * leftEyeX  + (1.0 - ema) * app.leftEyeXEMA;
                        app.rightEyeXEMA = ema * rightEyeX + (1.0 - ema) * app.rightEyeXEMA;
                    }

                    // Diagnostic: log the predicted eye positions + measured
                    // IPD ~once a second so we can confirm the runtime is
                    // returning sensible values + tell whether the user's
                    // tracked IPD differs from the 64mm fallback assumption.
                    if (fromPredicted)
                    {
                        static DWORD lastEyeLogTick = 0;
                        const DWORD now = GetTickCount();
                        if (now - lastEyeLogTick > 1000)
                        {
                            lastEyeLogTick = now;
                            const float ipd = rEye[0] - lEye[0];
                            Log("Quilt eyes: L=(%.1f,%.1f,%.1f) R=(%.1f,%.1f,%.1f) "
                                "IPD=%.1fmm (smoothed L.x=%.1f R.x=%.1f)",
                                lEye[0], lEye[1], lEye[2],
                                rEye[0], rEye[1], rEye[2], ipd,
                                app.leftEyeXEMA, app.rightEyeXEMA);
                        }
                    }

                    // Window-position perspective shift: when SR Loom is windowed
                    // on the SR display, the user's "looking toward" the window's
                    // SCREEN POSITION, not the panel centre. Subtract the window's
                    // centre-relative-to-panel-centre offset (in mm) from each
                    // eye-x so a head centred ON the window picks views around
                    // N/2 regardless of where the window sits on the panel.
                    double windowOffsetMm = 0.0;
                    if (app.srMmPerPx > 0.0 && app.hwnd)
                    {
                        RECT wr{}; GetWindowRect(app.hwnd, &wr);
                        const int winCenterX  = (wr.left + wr.right) / 2;
                        const int panCenterX  = (app.srDisplayRect.left + app.srDisplayRect.right) / 2;
                        windowOffsetMm = (double)(winCenterX - panCenterX) * app.srMmPerPx;
                    }

                    auto mapEyeXToFrac = [&](double x) -> double {
                        double n = x / HEAD_FULL_SWEEP_MM;     // -0.5..+0.5 typical
                        if (n < -0.5) n = -0.5;
                        if (n >  0.5) n =  0.5;
                        return (0.5 + n) * (double)(total - 1);
                    };
                    auto fracToIdxBlend = [&](double frac, int& outIdx, float& outBlend) {
                        if (frac < 0.0) frac = 0.0;
                        const double maxF = (double)(total - 1);
                        if (frac > maxF) frac = maxF;
                        const int    lo = (int)floor(frac);
                        outIdx   = lo;
                        outBlend = (float)(frac - (double)lo);
                    };
                    // Each eye picks its own view from its OWN absolute
                    // position (no IOD assumption needed). Shader cross-fades
                    // between view[idx] and view[idx+1] by the fractional
                    // component for continuous motion across view boundaries.
                    fracToIdxBlend(mapEyeXToFrac(app.leftEyeXEMA  - windowOffsetMm),
                                   app.quiltLeftIdx,  app.quiltLeftBlend);
                    fracToIdxBlend(mapEyeXToFrac(app.rightEyeXEMA - windowOffsetMm),
                                   app.quiltRightIdx, app.quiltRightBlend);
                    // Anti-shimmer near integer boundaries: snap tiny residual
                    // blend to 0 (still head -> locked to single view -> no
                    // cross-fade flicker); snap near-1 blend to the next view +
                    // 0 blend so the same locked behaviour holds either side.
                    auto snap = [&](int& idx, float& blend) {
                        if (blend < BLEND_SNAP) { blend = 0.0f; return; }
                        if (blend > 1.0f - BLEND_SNAP)
                        {
                            if (idx < total - 1) { ++idx; blend = 0.0f; }
                            else                 { blend = 1.0f; }
                        }
                    };
                    snap(app.quiltLeftIdx,  app.quiltLeftBlend);
                    snap(app.quiltRightIdx, app.quiltRightBlend);
                }
            }

            // (What the picture under an anaglyph was -- UpdateManualAnaColour:
            // black-and-white -> Mono, one colour -> its tint.)
            const bool anaTinted = app.manualAnaKind == 2 && app.manualAnaTint;
            {
                app.converter.SetAnaCustom(AnaCustomFromColours(app.anaCustomL, app.anaCustomR));
            }
            app.converter.SetFormat(app.format, swapNow, app.anaglyphCombo,
                                    app.manualAnaKind == 1 ? 3 : anaTinted ? 5 : app.anaglyphMode);
            app.converter.SetAnaTint(anaTinted ? app.manualAnaTint->single : nullptr,
                                     anaTinted ? app.manualAnaTint->missing : nullptr);
            {
                // (... and on a page of several, each such picture in its own box.)
                Converter::AnaBox boxes[32]; int nb = 0;
                for (const auto& b : app.manualAnaBoxes)
                {
                    if (nb == 32) break;
                    Converter::AnaBox& o = boxes[nb++];
                    o.u0 = b.u0; o.v0 = b.v0; o.u1 = b.u1; o.v1 = b.v1; o.kind = b.kind;
                    o.checkInsetU = b.insetU; o.checkInsetV = b.insetV;
                    if (b.tint) { o.single = b.tint->single; o.missing = b.tint->missing; o.pairA = b.tint->pairA; o.pairB = b.tint->pairB; }
                }
                app.converter.SetAnaBoxes(boxes, nb);
            }
            app.converter.SetConvergence(app.convergence * 0.03f);   // slider -2..2 -> up to ±6% of the width per eye
            {
                int ndN = 0; const NdLevel* nd = PulfrichNdLevels(ndN);
                const float trans = nd[(app.pulfrichNd >= 0 && app.pulfrichNd < ndN) ? app.pulfrichNd : 0].transmission;
                // Affected eye is always the right pane; Swap eyes moves it to the left.
                app.converter.SetPulfrich(app.pulfrichMode, 1, trans, app.pulfrichDelay);
            }
            {
                int fpN = 0; const FramePackPreset* fps = FramePackPresets(fpN);
                const FramePackPreset& fp = fps[(app.framePackMode >= 0 && app.framePackMode < fpN) ? app.framePackMode : 0];
                app.converter.SetFramePacking(fp.eyeFrac, fp.gapFrac, fp.eyeAlign);
            }
            // Light field (an experiment): the Quilt's views interlaced by SR Loom.
            // Which view a sub-pixel shows is where under its lens it is seen from
            // the place the views are aimed at -- straight ahead at the Viewing
            // Distance, or the tracked head with Follow My Head -- as the SDK's own
            // weaving library gives it for this display (GetLightFieldGeometry).
            // The middle of the fan of views then points at that place by itself;
            // Offset turns it from there.
            {
                float lfP = 0.0f, lfS = 0.0f;
                app.lfOffsetNow = app.lfOffset;
                if (app.lfOn && (app.format == StereoFormat::Quilt || app.format == StereoFormat::RGBD))
                {
                    float x = 0.0f, y = 0.0f, z = app.lfDistanceCm;
                    double hp[3] = {}, ho[3] = {};
                    // (Distance From Camera: how far away the tracked viewer is -- the
                    // distance only; the views stay aimed straight ahead.)
                    if (app.lfFollow && app.weaver.GetHeadPose(hp, ho) && hp[2] > 100.0)
                        z = (float)(hp[2] / 10.0);   // (mm -> cm)
                    // (Centre On Me: where the tracked viewer is sideways and in height
                    // too -- the views aimed there.)
                    if (app.lfCentre && app.weaver.GetHeadPose(hp, ho) && hp[2] > 100.0)
                    { x = (float)(hp[0] / 10.0); y = (float)(hp[1] / 10.0); }
                    // (Asked again only when the place moves: a millimetre, or the distance.)
                    if (!app.lfGeoValid || std::abs(x - app.lfGeoX) > 0.1f || std::abs(y - app.lfGeoY) > 0.1f || std::abs(z - app.lfGeoZ) > (app.lfFollow ? 2.0f : 0.1f) ||
                        app.renderer.Width() != app.lfGeoW)
                    {
                        app.lfGeoValid = app.weaver.GetLightFieldGeometry((float)app.renderer.Width(), (float)app.renderer.Height(), x, y, z,
                                                                          app.lfGeoPitch, app.lfGeoSlant, app.lfGeoPhase);
                        app.lfGeoX = x; app.lfGeoY = y; app.lfGeoZ = z; app.lfGeoW = app.renderer.Width();
                        // (How wide the fan of views is there: the phase a centimetre to
                        // the side tells -- for views drawn to order, RGB + depth.)
                        float p1 = 0.0f, s1 = 0.0f, c1 = 0.0f;
                        if (app.lfGeoValid && app.weaver.GetLightFieldGeometry((float)app.renderer.Width(), (float)app.renderer.Height(), x + 1.0f, y, z, p1, s1, c1))
                        {
                            float d = c1 - app.lfGeoPhase; d -= std::floor(d + 0.5f);
                            if (std::abs(d) > 1e-4f) app.lfFanCm = 1.0f / std::abs(d);
                        }
                        if (app.lfGeoValid && !app.lfFollow)
                            Log("Light field: aimed at %.0f cm -- lens pitch %.5f px, slant %.5f, phase at the middle %.3f (from the SR SDK)",
                                z, app.lfGeoPitch, app.lfGeoSlant, app.lfGeoPhase);
                        else if (!app.lfGeoValid) Log("Light field: the SDK gave no lens geometry");
                    }
                    if (app.lfGeoValid)
                    {
                        lfP = app.lfGeoPitch; lfS = app.lfGeoSlant;
                        // (The shader counts the phase from the middle of the picture, + 0.5.)
                        const float o = app.lfGeoPhase - 0.5f + app.lfOffset;
                        app.lfOffsetNow = o - std::floor(o);
                    }
                }
                else app.lfGeoValid = false;
                app.converter.SetLightField(lfP, lfS, (int)app.renderer.Width(), (int)app.renderer.Height());
            }
            {
                // (Light field: the Quilt's pair-of-views parameters carry its own --
                // the right index the alignment pattern's switch, the left blend the
                // fan's offset, the right blend the spread of views shown.)
                const bool rgbd = app.format == StereoFormat::RGBD;
                const bool lf = app.lfOn && (app.format == StereoFormat::Quilt || rgbd);
                // (RGB + depth: the layout bits ride in the column count, from 4 up;
                // the light field's reach in eye spacings, a quarter of it, in the
                // right blend -- the fan's width over 6.3 cm.)
                if (rgbd)
                    app.converter.SetQuilt(4 + (app.rgbdFlags & 7), 1, 0, (lf && app.lfPattern) ? 1 : 0,
                                           lf ? app.lfOffsetNow : 0.0f, lf ? (std::min)(1.0f, app.lfFanCm / 6.3f * 0.25f) : 0.0f);
                else
                app.converter.SetQuilt(app.quiltCols, app.quiltRows,
                                       app.quiltLeftIdx, lf ? (app.lfPattern ? 1 : 0) : app.quiltRightIdx,
                                       lf ? app.lfOffsetNow : app.quiltLeftBlend,
                                       lf ? app.lfSpread : app.quiltRightBlend);
            }
            // VR view -- if Headlook is on, fold the user's head ORIENTATION
            // (not position) into the view direction. LeiaSR ho[] mapping
            // (matches leia-track-app-XYZ's track_pipeline.h):
            //   ho[0] = pitch (rad), ho[1] = yaw (rad), ho[2] = roll (ignored)
            // Two-stage filter: OneEuro low-pass first, then Accela's
            // gain curve (deadzone=0) on top for soft sub-degree damping
            // without the "still still SNAP" jumps a hard deadzone gives.
            // Roll is intentionally never applied -- it makes 360 viewing
            // nauseating.
            float vrYawEffective   = app.vrYaw;
            float vrPitchEffective = app.vrPitch;
            if (IsVRFormat(app.format) && app.vrHeadLook)
            {
                double hp[3] = {}, ho[3] = {};
                if (app.weaver.GetHeadPose(hp, ho))
                {
                    // Stage 1: OneEuro (no timestamp -> uses fixed 60Hz
                    // constructor freq, more stable than GetTickCount's
                    // 15ms granularity at 165Hz render rate).
                    const float yaw1   = app.vrYawOneEuro  .filter((float)ho[1]);
                    const float pitch1 = app.vrPitchOneEuro.filter((float)ho[0]);
                    // Stage 2: Accela gain curve, threshold 0.025 rad
                    // (~1.4°). Tiny inputs sit in the curve's near-zero
                    // first segment (soft damping); real head turns
                    // exceed normalised 1.0 and hit the steep snap zone.
                    const double yawF   = AccelaApply(app.vrYawAccela,   yaw1,   0.025);
                    const double pitchF = AccelaApply(app.vrPitchAccela, pitch1, 0.025);
                    app.vrFilterInit = true;
                    // Negate so "head right -> view right" (raw yaw/pitch
                    // are opposite the viewer's expected direction; matches
                    // leia-track-app's invert_yaw default).
                    vrYawEffective   -= (float)yawF;
                    vrPitchEffective -= (float)pitchF;
                }
            }
            else if (app.vrFilterInit)
            {
                app.vrYawOneEuro  .reset();
                app.vrPitchOneEuro.reset();
                app.vrYawAccela  .init = false;
                app.vrPitchAccela.init = false;
                app.vrFilterInit = false;
            }
            app.converter.SetVRView(vrYawEffective, vrPitchEffective, app.vrZoom);
            // RGB + depth: its figures in the constants it has no other use for (see
            // rgbdView in the shader). The strength: up to 8% of the picture's width
            // of shift per eye spacing, nearest against furthest. Looking around:
            // where the head is, in eye spacings from the middle (smoothed a little).
            if (app.format == StereoFormat::RGBD)
            {
                float lx = 0.0f, ly = 0.0f;
                double hp[3] = {}, ho[3] = {};
                if (!app.weaver.IsLensOnly() && app.weaver.GetHeadPose(hp, ho) && hp[2] > 100.0)   // (always, when there is tracking: the light field is the way to be without)
                { lx = (float)(hp[0] / 63.0); ly = (float)((hp[1]) / 63.0); }
                app.rgbdLookX += (lx - app.rgbdLookX) * 0.35f; app.rgbdLookY += (ly - app.rgbdLookY) * 0.35f;
                app.converter.SetVRView(app.rgbdLookX, app.rgbdLookY, app.rgbdStrength * 0.0008f);
                app.converter.SetFramePacking(app.rgbdFocus * 0.01f, 1.0f, 0.0f);
            }
            // Pane = SWAP CHAIN size, not SR panel size. The weaver samples
            // the SBS pane at the output's UV, so a pane that doesn't match
            // the swap-chain aspect gets squeezed when the weaver writes its
            // lens pattern. Matching them means: fullscreen on the SR panel
            // uses the panel aspect (16:9 -> portrait quilt pillarboxed),
            // windowed at the view aspect uses that aspect (3:4 -> view fills
            // the window, NO pillarboxing).
            {
                const UINT sw = app.renderer.Width();
                const UINT sh = app.renderer.Height();
                if (sw > 0 && sh > 0)
                    app.converter.SetTargetPaneSize((int)sw, (int)sh);
            }
            bool resized = false;
            // (Unchanged capture: the converter keeps its last output.)
            // The anaglyph check's frame (UpdateManualAnaColour): this very one, so the
            // converter's snapshot (the boxes' reference as the page scrolls) is it too.
            if (app.manualAnaSubmitWanted && srcSRV && srcW > 0 && srcH > 0)
            {
                app.analyzer.Submit(srcEncoded ? app.capture.SRV() : srcSRV, srcW, srcH, true);   // (the analyzer wants the _SRGB copy)
                app.converter.RequestAnaSnapshot();
                app.manualAnaSubmitWanted = false;
                if (app.manualAnaPhase == 3) app.manualAnaPhase = 1;
            }
            // (The capture's picture: its content version lets an unchanged one skip
            // the conversion. A Katanga texture has none -- always converted.)
            const bool fromKatanga = katangaFmt && srcSRV == app.katanga.SRV();
            const bool fromCapture = srcSRV && !fromKatanga && (srcEncoded || srcSRV == app.capture.CopyView());
            app.converter.SetSourceVersion(fromCapture ? app.capture.ContentVersion() : 0);
            app.converter.SetSourceEncoded(srcEncoded, fromCapture && srcEncoded ? &app.capture : nullptr);
            app.converter.SetChangeStats(app.perfLog);
            // Recovered Colour is the one conversion that can take several ms of
            // GPU. Done here, it sits between a new frame arriving and the weave:
            // a frame that arrives shortly before the refresh (the loop waits for
            // one until 3 ms before) is then late for it, and the frame before
            // shows twice -- a judder, many times a second on a scrolling page.
            // So it's put off until this loop's frame is on its way (below, after
            // the present): the weave takes the picture converted last loop, on
            // time every time, and the conversion has the whole refresh that
            // follows. The price: the picture one refresh later (head tracking
            // isn't -- the weave is as late as ever).
            if (app.deferHeavyConvert && app.converter.IsRecoveredColour() && fromCapture && app.converter.OutputSRV() &&
                !app.captureRebind && !app.autoStereo)
            {
                app.deferSrc = srcSRV; app.deferW = srcW; app.deferH = srcH;
            }
            else
            // (The Custom pair's eyedropper: the click's colour, from this very picture.)
            if (app.eyedrop && (app.eyedropClicked || GetTickCount64() - app.eyedropSince > 20000))
            {
                float rgb[3];
                if (app.eyedropClicked && SampleSourceColour(app, srcSRV, srcW, srcH, app.eyedropPt, rgb))
                {
                    float* dst = app.eyedrop == 1 ? app.anaCustomL : app.anaCustomR;
                    for (int c = 0; c < 3; ++c) dst[c] = rgb[c];
                    Settings::WriteAnaCustom(app.anaCustomL, app.anaCustomR);
                    Log("Anaglyph: picked the %s colour %.0f %.0f %.0f", app.eyedrop == 1 ? "left" : "right", rgb[0] * 255, rgb[1] * 255, rgb[2] * 255);
                }
                EyedropEnd(app);
            }
            // DX12, Recovered Colour: conversion apart from the weave (see
            // Present12.cpp) -- while conversions are slow (video, a screenful
            // redrawn: over 3.5 ms of GPU each). The weave reads a picture of
            // its own, so it goes out every refresh whatever the conversion
            // takes, and no conversion starts while the one before is still
            // running. Quick conversions (most browsing) stay as they were: the
            // weave waits for them on the GPU and shows them the same refresh.
            Present12* p12 = app.renderer.DX12();
            // (Any layout: Recovered Colour is the one that gets there on a fast
            // GPU, but on a slow one any conversion can outlast a refresh.)
            const bool asyncAble = app.asyncConvert && p12 && app.weaver.IsDX12() &&
                                   fromCapture && !app.deferSrc && !app.autoStereo;
            if (asyncAble)
            {
                const double c = app.gpuTimer.lastConvMs;
                const ULONGLONG nowMs = GetTickCount64();
                // (Three slow ones in a row, quick ones between them aside from
                // frames with nothing to do: one long frame -- the first, a new
                // page -- isn't video.)
                if (app.gpuTimer.lastConvSerial != app.asyncSeenSerial)
                {
                    app.asyncSeenSerial = app.gpuTimer.lastConvSerial;
                    if (c > app.asyncEnterMs) ++app.asyncSlowRun; else if (c > app.asyncEnterMs * 0.17) app.asyncSlowRun = 0;
                    if (c > app.asyncEnterMs * 0.7) app.asyncSlowAt = nowMs;
                }
                if (!app.asyncSlow && app.asyncSlowRun >= 3) { app.asyncSlow = true; Log("Conversion apart from the weave: on (conversions taking %.1f ms)", c); }
                else if (app.asyncSlow && nowMs - app.asyncSlowAt > 2000) { app.asyncSlow = false; Log("Conversion apart from the weave: off (conversions quick again)"); }
            }
            else { app.asyncSlow = false; app.asyncSlowRun = 0; }
            const bool asyncConv = asyncAble && app.asyncSlow;
            app.asyncMode = asyncConv;   // (kept through loops that don't come by here: no new frame)
            app.converter.SetGpuYield(asyncConv ? app.asyncBands : 0);
            if (app.deferSrc) {}   // (put off until after the present: below)
            // (One still running is given a moment -- then the next can start in
            // this very loop; else none starts.)
            else if (asyncConv && p12->IsAsync() && p12->ConversionPending() && !p12->AsyncReady(app.asyncWaitMs)) { p12->NoteSkipped(); }
            else
            {
                if (asyncConv && p12->IsAsync())
                {
                    // (One that finished: settled before the next begins -- its
                    // predicate is reused -- and its picture copied out first.)
                    if (p12->AsyncReady(0.0)) { const int ch = app.converter.OutputChanged(); p12->AsyncResolve(ch < 0 ? 1 : ch); }
                    p12->AsyncBeforeConvert();
                }
                if (app.converter.Convert(srcSRV, srcW, srcH, resized))
                {
                    // (The conversion's end marked here, ahead of what follows: its
                    // GPU time is what the choice above goes by.)
                    if (asyncAble) app.gpuTimer.Stamp(GpuFrameTimer::kOurs);
                    if (asyncConv && p12->IsAsync() && p12->AsyncSubmitted(app.converter.OutputTexture()))
                    {
                        app.captureRebind = false;
                    }
                    else if (asyncConv && !p12->IsAsync())
                    {
                        // (Switching over: this frame still the direct way; from the
                        // next loop the weave has its own picture.)
                        if (resized || app.captureRebind)
                        {
                            app.weaver.SetInputView(app.converter.OutputSRV(), app.converter.OutputPerEyeWidth(),
                                                    app.converter.OutputHeight(), app.converter.OutputFormat());
                            app.captureRebind = false;
                        }
                    }
                    else if (resized || app.captureRebind || app.weaver.HasInputOverride12())
                    {
                        app.weaver.SetInputOverride12(nullptr, 0, 0, DXGI_FORMAT_UNKNOWN);
                        app.weaver.SetInputView(app.converter.OutputSRV(), app.converter.OutputPerEyeWidth(),
                                                app.converter.OutputHeight(), app.converter.OutputFormat());
                        app.captureRebind = false;
                    }
                }
            }
        }
        }   // matches the "{" introduced before the liveSource block by the LFPRenderer branch

        skipSourcePipeline:
        {
            using clk = std::chrono::steady_clock;
            auto ms = [](clk::time_point a, clk::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
            // DirectComposition: cut-outs are just mask data, so keep them
            // current every frame (a moving panel / terminal / pop-up gets its
            // hole in the same frame, not on the next 250 ms poll).
            if (app.renderer.IsDComp() && IsTopmostWeaveMode(app))
            { HitchWatch hw("cut-out update"); UpdateTaskbarCutout(app, false); }
            const clk::time_point tWork = clk::now();
            app.gpuTimer.Stamp(GpuFrameTimer::kOurs);
            if (Present12* p12 = app.renderer.DX12())
            {
                // (Conversion apart from the weave: the newest finished picture.
                // One under way is waited for only briefly: asyncWaitMs.)
                // (SetAsync after this frame's choice: the first frame of a run is
                // still woven the direct way, and the picture kept starts with the
                // next conversion.)
                // (Something else named the weaver's input meanwhile -- another
                // source or layout: this is over.)
                if (app.weaver.TakeOverrideDropped()) app.asyncMode = false;
                const bool was = p12->IsAsync();
                if (app.asyncMode && was)
                {
                    if (p12->AsyncReady(0.0)) p12->AsyncResolve(app.converter.OutputChanged());
                    if (ID3D12Resource* f = p12->AsyncFront())
                        app.weaver.SetInputOverride12(f, app.converter.OutputPerEyeWidth(), app.converter.OutputHeight(), app.converter.OutputFormat());
                }
                else if (!app.asyncMode && app.weaver.HasInputOverride12())
                {
                    app.weaver.SetInputOverride12(nullptr, 0, 0, DXGI_FORMAT_UNKNOWN);
                    app.weaver.SetInputView(app.converter.OutputSRV(), app.converter.OutputPerEyeWidth(),
                                            app.converter.OutputHeight(), app.converter.OutputFormat());
                }
                if (app.asyncMode != was) p12->SetAsync(app.asyncMode);
            }
            app.renderer.BindAndClearBackBuffer();
            {
                HitchWatch hw("SR weave call");
                // (Lens only: the light field's picture is already interlaced --
                // drawn as it is, no weave.)
                if (app.weaver.IsLensOnly()) app.renderer.BlitPicture(app.converter.OutputTexture());
                else if (!app.diagSkipWeave) app.weaver.Weave();   // (DiagSkipWeave: see Settings.h)
            }
            const clk::time_point tWeave = clk::now();
            app.gpuTimer.Stamp(GpuFrameTimer::kWeave);
            if (app.renderer.IsDComp()) app.renderer.ApplyMask();   // (timed apart from the present)
            app.gpuTimer.Stamp(GpuFrameTimer::kMask);
            { HitchWatch hw("present"); app.renderer.Present(false, !app.paceOnCapture); }   // no-vsync: lowest latency (VRR absorbs tearing)
            app.gpuTimer.Stamp(GpuFrameTimer::kPresent);
            const clk::time_point tEnd = clk::now();
            app.prof.lastEnd = tEnd;
            // (The conversion put off above: its result is woven next loop.)
            if (app.deferSrc)
            {
                bool resized = false;
                if (app.converter.Convert(app.deferSrc, app.deferW, app.deferH, resized) && (resized || app.captureRebind))
                {
                    app.weaver.SetInputView(app.converter.OutputSRV(), app.converter.OutputPerEyeWidth(),
                                            app.converter.OutputHeight(), app.converter.OutputFormat());
                    app.captureRebind = false;
                }
                app.deferSrc = nullptr;
                // (Sent to the GPU now. Left in Direct3D's own queue it went with the
                // NEXT loop's commands -- run just before that weave, where it was
                // before, only a refresh later as well.)
                app.renderer.Context()->Flush();
            }
            app.gpuTimer.End();
            if (app.analysisDeferred)   // (see the Auto Stereo analysis above)
            {
                app.analysisDeferred = false;
                const auto t0 = std::chrono::steady_clock::now();
                { HitchWatch hw("Auto Stereo analysis/tracking/picks"); ProcessAutoStereoAnalysis(app); }
                app.autoTimeAnalysisMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            }

            // Frame profile: every 5 s, where the loop's time went.
            AppState::FrameProfile& p = app.prof;
            if (p.t0.time_since_epoch().count() != 0)
            {
                const double total = ms(p.t0, tEnd);
                p.wait += ms(p.t0, p.tWait); p.work += ms(p.tWait, tWork); p.weave += ms(tWork, tWeave);
                p.present += ms(tWeave, tEnd); p.total += total; p.worst = (std::max)(p.worst, total);
                p.analysis += app.autoStereo ? app.autoTimeAnalysisMs : 0.0;
                p.readback += app.autoStereo ? app.autoTimeReadbackMs : 0.0;
                ++p.loops;
                p.frames += gotFrame ? 1 : 0;
                p.compWait += app.renderer.LastCompositorWaitMs(); p.vblankWait += app.renderer.LastVBlankWaitMs();
                const double hz = SrRefreshHz(app);
                // Late: this present more than 1.5 refreshes after the last one
                // (what's seen -- a refresh shown twice). Put down to whichever
                // part of the time between them took most: between loops (window
                // messages, the panel), the compositor still holding the last
                // frame, the refresh cap, the wait for the refresh, the wait for
                // a new capture frame, our work, or weave + present.
                const double sinceLast = p.prevPresent.time_since_epoch().count() != 0 ? ms(p.prevPresent, tEnd) : total;
                const bool hadPrev = p.prevPresent.time_since_epoch().count() != 0;
                const clk::time_point prevPresent = p.prevPresent;
                p.prevPresent = tEnd;
                if (hz > 0 && sinceLast > 1.5 * 1000.0 / hz)
                {
                    ++p.late;
                    const double cw = app.renderer.LastCompositorWaitMs(), vw = app.renderer.LastVBlankWaitMs();
                    const double cap = p.lastCaptureWaitMs;
                    const double parts[7] = {
                        hadPrev ? ms(prevPresent, p.t0) : 0.0,                                  // between loops
                        cw,                                                                      // compositor
                        (std::max)(0.0, ms(p.t0, p.tWait) - cw - vw - cap),                     // refresh cap
                        vw,                                                                      // refresh wait
                        cap,                                                                     // capture wait
                        ms(p.tWait, tWork),                                                      // our work
                        ms(tWork, tEnd) };                                                       // weave + present
                    int* const bins[7] = { &p.lateOutside, &p.lateComp, &p.lateCap, &p.lateBlank, &p.lateCapture, &p.lateWork, &p.latePresent };
                    int best = 0;
                    for (int k = 1; k < 7; ++k) if (parts[k] > parts[best]) best = k;
                    ++*bins[best];
                }
            }
            if (GetTickCount() - p.last >= 5000)
            {
                if (p.loops > 0 && app.perfLog)
                {
                    const double secs = (GetTickCount() - p.last) / 1000.0;
                    Log("Frame profile (%s%s, %s, display %.0f Hz): %.1f loops/s, %.1f new frames/s | avg ms: pace-wait %.2f, "
                        "our work %.2f (auto analysis %.2f, of which readback %.2f), weave %.2f, present+compositor %.2f, whole loop %.2f (worst %.1f) | outside the weave %.2f (panel %.2f)",
                        app.autoStereo ? "Auto Stereo" : (app.mode == OutputMode::LookingGlass ? "Looking Glass" :
                                                          app.mode == OutputMode::Fullscreen ? "Fullscreen" : "other"),
                        app.renderer.IsDComp() ? ", DirectComposition" : app.renderer.IsFlipModel() ? ", flip" : ", bit-blt",
                        FsCurrentFormatLabel(app.format),
                        SrRefreshHz(app),
                        p.loops / secs, p.frames / secs, p.wait / p.loops, p.work / p.loops, p.analysis / p.loops, p.readback / p.loops,
                        p.weave / p.loops, p.present / p.loops, p.total / p.loops, p.worst,
                        p.outside / p.loops, p.gui / p.loops);
                    // (Pacing: frames that took two refreshes -- each one a visible
                    // stutter -- and what the loop waited on.)
                    Log("  pacing: %.1f late frames/s (%d) -- most time in: between loops %d, compositor %d, refresh cap %d, refresh wait %d, capture wait %d, our work %d, weave+present %d | avg waits ms: compositor %.2f, vertical blank %.2f",
                        p.late / secs, p.late, p.lateOutside, p.lateComp, p.lateCap, p.lateBlank, p.lateCapture, p.lateWork, p.latePresent,
                        p.compWait / p.loops, p.vblankWait / p.loops);
                    // GPU side, and the rate Windows actually composes at
                    // (the loop can't outrun the compositor that shows it).
                    DWM_TIMING_INFO ti{ sizeof(ti) };
                    double dwmHz = 0.0;
                    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.rateCompose.uiDenominator)
                        dwmHz = (double)ti.rateCompose.uiNumerator / ti.rateCompose.uiDenominator;
                    GpuFrameTimer& g = app.gpuTimer;
                    uint64_t capFrames = 0;
                    const double capFps = app.capture.TakeDeliveryRate(capFrames);
                    {
                        int qc = 0, qb = 0; app.renderer.TakeQueueStats(qc, qb);
                        if (qc > 0) Log("  swap chain: the frame before still queued at the start of %d of %d loops", qb, qc);
                        // (The Direct3D 12 presenter's own GPU clock: the marks above are
                        // Direct3D 11's and don't see its weave.)
                        {
                            // (How long a picture really takes from the weave call to the
                            // display, against what the weaver predicts the eyes ahead by.)
                            double la = 0, lmin = 0, lmax = 0; int ln = 0;
                            if (app.renderer.TakeDisplayLatency(la, lmin, lmax, ln))
                            {
                                // (The weaver itself is not asked what it predicts by: about 1.9
                                // refreshes, 11.9 ms at 160 Hz, when it was -- 2026-10. Asking it
                                // every few seconds went with a run where the weave sat off the
                                // viewer; not proven the cause, but not worth the risk.)
                                Log("  weave to display: measured avg %.2f ms (%.2f to %.2f, %d frames)", la, lmin, lmax, ln);
                                // (Settings WeaverLatency 1: the weaver told what was measured,
                                // smoothed; it then aims the views at where the eyes will be
                                // when the picture is actually seen.)
                                if (app.weaverLatencyAuto && ln > 100)
                                {
                                    app.latencyEma = app.latencyEma > 0 ? app.latencyEma * 0.7 + la * 0.3 : la;
                                    app.weaver.SetLatencyUs((uint64_t)(app.latencyEma * 1000.0));
                                }
                            }
                        }
                        Present12::AsyncStats as;
                        if (app.renderer.IsDX12() && app.renderer.DX12()->TakeAsyncStats(as))
                            Log("  DX12 conversion apart from the weave: %d pictures shown (%d of them a refresh or more after they were asked for), %d conversions changed nothing, %d loops started none (the last still running) | asked-to-shown avg %.2f ms, worst %.2f",
                                as.shown, as.late, as.same, as.skipped, as.avgMs, as.worstMs);
                        Present12::Times pt;
                        if (app.renderer.IsDX12() && app.renderer.DX12()->TakeTimes(pt))
                            Log("  DX12 presenter (%d frames): GPU ms weave %.2f, mask %.2f | from sending a frame: GPU starts it after %.2f ms, done after %.2f (worst %.2f)",
                                pt.frames, pt.weaveMs, pt.maskMs, pt.startMs, pt.doneMs, pt.worstDoneMs);
                    }
                    {
                        uint64_t du = 0, dunk = 0, dnone = 0; double darea = 0;
                        app.capture.TakeDirtyStats(du, dunk, dnone, darea);
                        if (du > 0)
                            Log("  capture: %llu frames taken -- Windows reported nothing changed in %llu, no information for %llu, else %.0f%% of the screen changed (average)",
                                du, dnone, dunk, darea * 100.0);
                    }
                    // (Outside Auto Stereo the analysis/tracking marks are never
                    // reached: everything after the capture copy is conversion.)
                    if (g.n > 0 && !app.autoStereo)
                        Log("  GPU ms (avg of %d): ours %.2f [capture copy %.2f, conversion %.2f], SR weave %.2f, mask %.2f, present %.2f, conversion after it %.2f | DWM composing at %.1f Hz (our window %.1f Hz) | capture delivered %.1f frames/s",
                            g.n, g.Avg(GpuFrameTimer::kCapture) + g.Avg(GpuFrameTimer::kAnalysis) + g.Avg(GpuFrameTimer::kTracking) + g.Avg(GpuFrameTimer::kOurs),
                            g.Avg(GpuFrameTimer::kCapture),
                            g.Avg(GpuFrameTimer::kAnalysis) + g.Avg(GpuFrameTimer::kTracking) + g.Avg(GpuFrameTimer::kOurs),
                            g.Avg(GpuFrameTimer::kWeave), g.Avg(GpuFrameTimer::kMask), g.Avg(GpuFrameTimer::kPresent), g.Avg(GpuFrameTimer::kEnd), dwmHz,
                            app.renderer.CompositionRateHz(), capFps);
                    else if (g.n > 0)
                        Log("  GPU ms (avg of %d): ours %.2f [capture copy %.2f, analysis %.2f, tracking %.2f, crops+conversion %.2f], SR weave %.2f, mask %.2f, present %.2f, conversion after it %.2f | DWM composing at %.1f Hz (our window %.1f Hz) | capture delivered %.1f frames/s",
                            g.n, g.Avg(GpuFrameTimer::kCapture) + g.Avg(GpuFrameTimer::kAnalysis) + g.Avg(GpuFrameTimer::kTracking) + g.Avg(GpuFrameTimer::kOurs),
                            g.Avg(GpuFrameTimer::kCapture),
                            g.Avg(GpuFrameTimer::kAnalysis), g.Avg(GpuFrameTimer::kTracking), g.Avg(GpuFrameTimer::kOurs),
                            g.Avg(GpuFrameTimer::kWeave), g.Avg(GpuFrameTimer::kMask), g.Avg(GpuFrameTimer::kPresent), g.Avg(GpuFrameTimer::kEnd), dwmHz,
                            app.renderer.CompositionRateHz(), capFps);
                    else
                        Log("  GPU ms: n/a | DWM composing at %.1f Hz", dwmHz);
                    g.Reset();
                    {
                        double rt[8] = {}; int rn = 0;
                        if (app.converter.TakeRecoveryTimes(rt, rn))
                            Log("  Anaglyph recovery GPU ms (avg of %d): coarse search %.2f, descriptors %.2f, refine %.2f, "
                                "occlusion fill %.2f, smoothing %.2f | full-res decode %.2f, colour pyramid %.2f, colour fill %.2f",
                                rn, rt[0], rt[1], rt[2], rt[3], rt[4], rt[5], rt[6], rt[7]);
                        srw::Converter::ChangeStats cs;
                        if (rn > 0 && app.converter.TakeChangeStats(cs))
                            Log("  Recovery frames: %d converted -- %d whole, %d with changes (%.0f%% of the picture redrawn, %.0f%% kept and moved), "
                                "%d of them scrolls (%.0f rows a frame) | scroll reuse %s",
                                cs.frames, cs.full, cs.changed, cs.redrawn * 100.0, cs.moved * 100.0, cs.scrolled, cs.rows,
                                cs.reuse ? "on" : "not running");
                    }
                    // Auto Stereo: each woven picture's geometry (screen px) --
                    // where it is, what it's clipped to, the part on show, and
                    // the windows counted in front of it -- for tracking down
                    // a box drawn in the wrong place or cut off.
                    if (app.autoStereo)
                        for (const WeaveRegion& r : app.regionWeaver.Regions())
                        {
                            const RECT clip = IsRectEmpty(&r.clip) ? RECT{} : FrameToScreen(app, r.clip);
                            const RECT vis  = IsRectEmpty(&r.vis) ? RECT{} : FrameToScreen(app, r.vis);
                            std::string occ;
                            auto oc = app.regionOcc.find(r.id);
                            if (oc != app.regionOcc.end())
                                for (const RECT& o : oc->second)
                                {
                                    char b[64];
                                    _snprintf_s(b, _TRUNCATE, " (%ld,%ld)-(%ld,%ld)", o.left, o.top, o.right, o.bottom);
                                    occ += b;
                                }
                            char viewCls[48] = "";
                            if (r.viewWindow) GetClassNameA(r.viewWindow, viewCls, (int)sizeof(viewCls));
                            // Which windows those are (above the picture's window
                            // in z-order and over its area).
                            if (auto snap = g_winWatch.Latest())
                                for (const WinInfo& w : snap->z)
                                {
                                    if (w.h == r.host || w.desktop) break;
                                    RECT i{};
                                    if (w.own || !IntersectRect(&i, &w.frame, &r.screen)) continue;
                                    char cls[64] = "";
                                    GetClassNameA(w.h, cls, (int)sizeof(cls));
                                    DWORD pid = 0;
                                    GetWindowThreadProcessId(w.h, &pid);
                                    Log("    above it: %p class '%s' title '%.40s' pid %lu frame (%ld,%ld)-(%ld,%ld) ex 0x%llX band %lu",
                                        (void*)w.h, cls, WindowTitle(w.h).c_str(), pid, w.frame.left, w.frame.top,
                                        w.frame.right, w.frame.bottom, (unsigned long long)w.ex, w.band);
                                }
                            Log("  region %d '%.40s' view=%s at (%ld,%ld)-(%ld,%ld) clip (%ld,%ld)-(%ld,%ld) "
                                "showing (%ld,%ld)-(%ld,%ld) lead (%ld,%ld) gpu=%d in front:%s",
                                r.id, WindowTitle(r.host).c_str(), viewCls,
                                r.screen.left, r.screen.top, r.screen.right, r.screen.bottom,
                                clip.left, clip.top, clip.right, clip.bottom,
                                vis.left, vis.top, vis.right, vis.bottom, r.lead.x, r.lead.y, r.gpuSlot,
                                occ.empty() ? " none" : occ.c_str());
                        }
                    // Injected present hooks (logged when the set changes).
                    {
                        static std::string s_hooks = "?";
                        const std::string hooks = LoadedPresentHooks();
                        if (hooks != s_hooks)
                        {
                            s_hooks = hooks;
                            Log("  Present hooks loaded in SR Loom: %s", hooks.empty() ? "none" : hooks.c_str());
                        }
                    }
                }
                const DWORD last = GetTickCount();
                p = AppState::FrameProfile{};
                p.last = last;
                p.lastEnd = tEnd;
            }
        }
    }

    LRESULT WndProcImpl(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // Every message to the weave window, timed: a slow one is logged with
    // who sent it (see LogSlowMessage).
    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        AppLock appLock;   // (never while the render thread runs the loop body: see g_appLock)
        const auto t0 = std::chrono::steady_clock::now();
        const DWORD sent = InSendMessageEx(nullptr);
        const LRESULT r = WndProcImpl(hwnd, msg, wParam, lParam);
        LogSlowMessage("weave", msg, wParam, sent, t0);
        return r;
    }

    LRESULT WndProcImpl(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        AppState* app = g_app;

        switch (msg)
        {
        // (The Custom anaglyph pair's eyedropper: the weave window shows a pipette
        // and takes the next click; a right click gives up.)
        case WM_SETCURSOR:
            if (app && app->eyedrop && (HWND)wParam == app->hwnd) { SetCursor(EyedropCursor()); return TRUE; }
            break;
        case WM_RBUTTONDOWN:
            if (app && app->eyedrop) { EyedropEnd(*app); return 0; }
            break;
        case WM_APP_TRAY:
            if (app && (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU))
            {
                MenuState ms{ app->weavingEnabled, app->mode, app->source,
                              app->format, app->swapEyes, app->anaglyphCombo, app->anaglyphMode,
                              app->pulfrichMode, app->pulfrichDelay, app->pulfrichNd,
                              app->framePackMode,
                              app->openTrack.IsOpenTrackEnabled(),
                              app->openTrack.IsFreeTrackEnabled(),
                              app->openTrack.IsTrackIREnabled(),
                              app->openTrack.GetConfig().outputMode,
                              app->profilesAutoApply,
                              {} };
                ms.profileNames.reserve(app->profiles.size());
                for (const auto& p : app->profiles) ms.profileNames.push_back(p.name);
                if (!app->pendingUpdateUrl.empty()) ms.updateTag = app->pendingUpdateTag;
                ms.autoInput = app->autoInput;
                // The menu on its own thread: its message loop ran on this one
                // -- the render thread -- and the weave froze for as long as the
                // menu was open. The chosen command comes back as WM_COMMAND.
                static std::atomic<bool> s_menuOpen{ false };
                if (!s_menuOpen.exchange(true))
                {
                    TrayIcon* tray = &app->tray;
                    std::thread([ms, hwnd, tray]() {
                        static const wchar_t* kCls = L"SRLoomTrayMenu";
                        WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = kCls;
                        RegisterClassW(&wc);   // (fails harmlessly once registered)
                        HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, kCls, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
                        const UINT cmd = owner ? tray->ShowContextMenu(owner, ms, true) : 0;
                        if (owner) DestroyWindow(owner);
                        if (cmd) PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
                        s_menuOpen = false;
                    }).detach();
                }
            }
            else if (app && LOWORD(lParam) == WM_LBUTTONUP)
            {
                app->gui.Toggle();   // left-click opens/closes the control panel
            }
            else if (app && LOWORD(lParam) == NIN_BALLOONUSERCLICK
                          && !app->pendingUpdateUrl.empty())
            {
                // User clicked the "update available" toast -> open the release page.
                ShellExecuteA(nullptr, "open", app->pendingUpdateUrl.c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
            }
            return 0;

        case WM_APP_UPDATE_RESULT:
            if (app && wParam)
            {
                auto* info = reinterpret_cast<ReleaseInfo*>(wParam);
                char title[128], body[256];
                DWORD infoFlags = NIIF_INFO;
                // Auto-checks now run on launch AND on every panel open, and
                // the found release is remembered across launches -- so only
                // balloon for a release we haven't already announced (the GUI
                // banner + tray item carry it from then on). User-forced
                // checks always get a balloon.
                const bool announce = info->forced ||
                    info->status != ReleaseInfo::Available ||
                    info->tag != app->pendingUpdateTag;
                switch (info->status)
                {
                case ReleaseInfo::Available:
                    app->pendingUpdateUrl = info->url;
                    app->pendingUpdateTag = info->tag;
                    _snprintf_s(title, _TRUNCATE, "SR Loom update available");
                    _snprintf_s(body, _TRUNCATE,
                                "Version %s is out (you have %s). Click to open the release page.",
                                info->tag.c_str(), kAppVersion);
                    Log("UpdateChecker: notified -- latest %s, current %s",
                        info->tag.c_str(), kAppVersion);
                    break;
                case ReleaseInfo::UpToDate:
                    app->pendingUpdateUrl.clear();
                    app->pendingUpdateTag.clear();
                    _snprintf_s(title, _TRUNCATE, "SR Loom is up to date");
                    _snprintf_s(body, _TRUNCATE,
                                "You're on the latest release (%s).", kAppVersion);
                    Log("UpdateChecker: user-checked, already up to date");
                    break;
                case ReleaseInfo::Failed:
                default:
                    app->pendingUpdateUrl.clear();
                    app->pendingUpdateTag.clear();
                    _snprintf_s(title, _TRUNCATE, "Update check failed");
                    _snprintf_s(body, _TRUNCATE,
                                "Couldn't reach the update server. Check your network and try again.");
                    infoFlags = NIIF_WARNING;
                    Log("UpdateChecker: user-checked, fetch failed");
                    break;
                }
                if (announce)
                {
                    NOTIFYICONDATAA nid{ sizeof(nid) };
                    nid.hWnd   = hwnd;
                    nid.uID    = 1;
                    nid.uFlags = NIF_INFO;
                    nid.dwInfoFlags = infoFlags;
                    _snprintf_s(nid.szInfoTitle, _TRUNCATE, "%s", title);
                    _snprintf_s(nid.szInfo,      _TRUNCATE, "%s", body);
                    Shell_NotifyIconA(NIM_MODIFY, &nid);
                }
                delete info;
            }
            return 0;

        case WM_APP_CHECK_UPDATES:
            // User-forced update check from the About popup. Skips the
            // 6-hour throttle and always posts a WM_APP_UPDATE_RESULT so
            // the user sees a balloon either way.
            UpdateChecker::StartAsync(hwnd, WM_APP_UPDATE_RESULT, true);
            return 0;

        case WM_APP_FOREGROUND_CHANGED:
            // Posted by ForegroundEventProc (out-of-context WinEvent hook)
            // when the user alt-tabs / focuses a new window. lParam is the
            // new foreground HWND. We do the actual profile match + apply
            // here on the main thread so ChangeFormat / EnsureWeaving /
            // SetWeaving don't fire from inside the hook callback.
            if (app) HandleForegroundChanged(*app, reinterpret_cast<HWND>(lParam));
            return 0;


        case WM_APP_GUI_CAPTURE_WINDOW:   // GUI window-picker chose a window to make 3D
            if (app) UseWindow(*app, reinterpret_cast<HWND>(lParam));
            return 0;

        case WM_APP_GUI_CAPTURE_DISPLAY:  // GUI display-picker chose a monitor to weave
            if (app) UseDisplay(*app, reinterpret_cast<HMONITOR>(lParam));
            return 0;

        case WM_APP_PIN_INPUT:   // panel pin: this Stereo 3D Input is the default from now on
            if (app)
            {
                app->defaultInput = (int)wParam;
                Settings::WriteDefaultInput(app->defaultInput);
                Log("Default input pinned: %d", app->defaultInput);
            }
            return 0;

        case WM_APP_QUILT_GRID:
            // GUI -> set quilt grid (cols in wParam, rows in lParam). Re-centre
            // the L/R view pair and refresh both the GUI snapshot and the
            // floating overlay's button labels.
            if (app)
            {
                const int c = (int)wParam, r = (int)lParam;
                if (c >= 1 && c <= 12) app->quiltCols = c;
                if (r >= 1 && r <= 9)  app->quiltRows = r;
                const int total = app->quiltCols * app->quiltRows;
                app->quiltLeftIdx  = total / 2 - 1; if (app->quiltLeftIdx  < 0)        app->quiltLeftIdx  = 0;
                app->quiltRightIdx = total / 2;     if (app->quiltRightIdx >= total)   app->quiltRightIdx = total - 1;
                app->quiltLeftBlend = app->quiltRightBlend = 0.0f;
                app->captureRebind = true;
                if (g_fsSet && IsWindowVisible(g_fsSet)) InvalidateRect(g_fsSet, nullptr, FALSE);
            }
            return 0;

        case WM_APP_QUILT_AUTODETECT:
            if (app)
            {
                AutoDetectQuiltOnCurrent(*app);
                if (g_fsSet && IsWindowVisible(g_fsSet)) ShowFsSetOverlay(*app);
            }
            return 0;

        case WM_LBUTTONDOWN:
            if (app && app->eyedrop && hwnd == app->hwnd)
            {
                app->eyedropPt = POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                app->eyedropClicked = true;
                return 0;
            }
            // VR viewer: record the press but don't commit to "drag" yet --
            // any movement past the threshold turns it into a drag-to-look;
            // a release within the threshold falls through to the overlay
            // toggle so the user can still pop the test-image controls.
            if (app && app->source == SourceKind::TestImage && IsVRFormat(app->format))
            {
                app->vrMouseDown  = true;
                app->vrMoved      = false;
                app->vrDragStartX = app->vrDragLastX = GET_X_LPARAM(lParam);
                app->vrDragStartY = app->vrDragLastY = GET_Y_LPARAM(lParam);
                SetCapture(hwnd);
                return 0;
            }
            // Test-image overlays: settings (left) appears in both modes;
            // window-controls (right) only in fullscreen (windowed already has
            // native chrome with min/max/close); video transport (bottom)
            // only when a video file is the source. Click TOGGLES: if any of
            // the overlays are already visible, hide them all (clicking in
            // the middle of the image dismisses the UI); else show.
            if (app && app->source == SourceKind::TestImage)
            {
                const bool anyVisible =
                    (g_fsCtrl && IsWindowVisible(g_fsCtrl)) ||
                    (g_fsSet  && IsWindowVisible(g_fsSet))  ||
                    (g_fsVid  && IsWindowVisible(g_fsVid));
                if (anyVisible)
                {
                    HideFsCtrlOverlay();
                    HideFsSetOverlay();
                    HideFsVidOverlay();
                }
                else
                {
                    if (app->mode == OutputMode::Fullscreen) ShowFsCtrlOverlay(*app);
                    if (app->mode == OutputMode::Fullscreen ||
                        app->mode == OutputMode::Windowed)
                    {
                        ShowFsSetOverlay(*app);
                        if (app->video.IsOpen()) ShowFsVidOverlay(*app);
                    }
                }
            }
            return 0;

        case WM_MOUSEMOVE:
            if (app && app->vrMouseDown && (wParam & MK_LBUTTON))
            {
                const int x = GET_X_LPARAM(lParam);
                const int y = GET_Y_LPARAM(lParam);
                if (!app->vrMoved)
                {
                    const int dx = x - app->vrDragStartX;
                    const int dy = y - app->vrDragStartY;
                    // 5-pixel drag threshold separates an accidental jitter
                    // (= still a click) from an intentional drag-to-look.
                    if (dx * dx + dy * dy > 25) app->vrMoved = true;
                }
                if (app->vrMoved)
                {
                    // px-to-radians: 1800 px ~= 180° -- comfortable viewer
                    // feel, scales down further at higher zoom.
                    const float kPxToRad = 3.14159265f / 1800.0f;
                    const float zoomDiv = (app->vrZoom > 0.1f) ? app->vrZoom : 0.1f;
                    app->vrYaw   -= (x - app->vrDragLastX) * kPxToRad / zoomDiv;
                    app->vrPitch -= (y - app->vrDragLastY) * kPxToRad / zoomDiv;
                    if (app->vrPitch >  1.5f) app->vrPitch =  1.5f;
                    if (app->vrPitch < -1.5f) app->vrPitch = -1.5f;
                    app->vrDragLastX = x;
                    app->vrDragLastY = y;
                    app->captureRebind = true;
                }
                return 0;
            }
            return 0;

        case WM_LBUTTONUP:
            if (app && app->loupeDrag.active)
            {
                EndLoupeOwnDrag(*app);   // our own Looking Glass move/resize ends
                return 0;
            }
            if (app && app->vrMouseDown)
            {
                const bool wasClick = !app->vrMoved;
                app->vrMouseDown = false;
                app->vrMoved     = false;
                ReleaseCapture();
                if (!wasClick) return 0;   // real drag -- consume
                // Quick click on the SR Loom window: toggle the test-image
                // overlays the same way the non-VR click path does.
                const bool anyVisible =
                    (g_fsCtrl && IsWindowVisible(g_fsCtrl)) ||
                    (g_fsSet  && IsWindowVisible(g_fsSet))  ||
                    (g_fsVid  && IsWindowVisible(g_fsVid));
                if (anyVisible)
                {
                    HideFsCtrlOverlay();
                    HideFsSetOverlay();
                    HideFsVidOverlay();
                }
                else
                {
                    if (app->mode == OutputMode::Fullscreen) ShowFsCtrlOverlay(*app);
                    if (app->mode == OutputMode::Fullscreen ||
                        app->mode == OutputMode::Windowed)
                    {
                        ShowFsSetOverlay(*app);
                        if (app->video.IsOpen()) ShowFsVidOverlay(*app);
                    }
                }
                return 0;
            }
            break;

        case WM_MOUSEWHEEL:
            if (app && app->source == SourceKind::TestImage && IsVRFormat(app->format))
            {
                const float delta = (float)GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
                app->vrZoom *= powf(1.12f, delta);   // ~12% per notch
                if (app->vrZoom < 0.2f) app->vrZoom = 0.2f;
                if (app->vrZoom > 3.0f) app->vrZoom = 3.0f;
                app->captureRebind = true;
                return 0;
            }
            break;

        case WM_MOVE:
            // Keep the floating overlays glued to the window's corners.
            if (app) RepositionOverlays(*app);
            break;

        case WM_DROPFILES:
        {
            HDROP drop = (HDROP)wParam;
            wchar_t path[MAX_PATH] = {};
            const UINT cnt = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
            if (cnt > 0 && DragQueryFileW(drop, 0, path, MAX_PATH) > 0 && app)
            {
                char pathA[MAX_PATH] = {};
                WideCharToMultiByte(CP_UTF8, 0, path, -1, pathA, MAX_PATH, nullptr, nullptr);
                LoadTestImage(*app, pathA);
            }
            DragFinish(drop);
            return 0;
        }

        case WM_APP_FS_BUTTON:
            // Posted from the fs-controls overlay popup when the user clicks one
            // of its buttons: 0 = minimise (standard Windows minimise to
            // taskbar), 1 = switch to Windowed mode (movable window with native
            // chrome), 2 = close (stop weaving -- back to tray, lens / camera
            // off). Hide the overlays first so they don't linger past the
            // window state change.
            if (app)
            {
                HideFsCtrlOverlay();
                HideFsSetOverlay();
                HideFsVidOverlay();
                switch (wParam)
                {
                case 0: ShowWindow(app->hwnd, SW_MINIMIZE); break;
                case 1: app->mode = OutputMode::Windowed; ApplyMode(*app); break;
                case 2: SetWeaving(*app, false); break;
                }
            }
            return 0;

        case WM_COMMAND:
        {
            if (!app) break;
            const UINT cmd = LOWORD(wParam);

            // Window-list items occupy a command-id range.
            if (cmd >= ID_TRAY_SRC_WINDOW_BASE && cmd <= ID_TRAY_SRC_WINDOW_MAX)
            {
                UseWindow(*app, app->tray.WindowAt(cmd - ID_TRAY_SRC_WINDOW_BASE));
                return 0;
            }
            // 3D-format items occupy another range.
            if (cmd >= ID_TRAY_FMT_BASE && cmd <= ID_TRAY_FMT_MAX)
            {
                int n = 0;
                const StereoFormatEntry* fmts = StereoFormatList(n);
                const int idx = (int)(cmd - ID_TRAY_FMT_BASE);
                if (idx < n)
                {
                    // Katanga places its own weave (full SR display, or over a
                    // windowed sender) from the Fullscreen mode's layered window.
                    if (fmts[idx].fmt == StereoFormat::Katanga)
                        app->mode = OutputMode::Fullscreen;
                    ChangeFormat(*app, fmts[idx].fmt);
                    ManualInputChosen(*app);
                }
                // Re-show the test-image settings overlay so Quilt's cols/rows
                // buttons appear (or vanish) immediately on a format change.
                if (g_fsSet && IsWindowVisible(g_fsSet)) ShowFsSetOverlay(*app);
                return 0;
            }
            // Anaglyph colour combo / decode mode (also selects the Anaglyph format).
            if (cmd >= ID_TRAY_ANA_COMBO_BASE && cmd <= ID_TRAY_ANA_COMBO_MAX)
            {
                app->anaglyphCombo = (int)(cmd - ID_TRAY_ANA_COMBO_BASE);
                ChangeFormat(*app, StereoFormat::Anaglyph);
                ManualInputChosen(*app);
                return 0;
            }
            if (cmd >= ID_TRAY_ANA_MODE_BASE && cmd <= ID_TRAY_ANA_MODE_MAX)
            {
                int n = 0; const AnaglyphModeEntry* modes = AnaglyphModeList(n);
                const int idx = (int)(cmd - ID_TRAY_ANA_MODE_BASE);
                if (idx < n) app->anaglyphMode = modes[idx].value;   // menu index -> shader mode value
                ChangeFormat(*app, StereoFormat::Anaglyph);
                ManualInputChosen(*app);
                return 0;
            }
            // Pulfrich sub-options (each also selects the Pulfrich format).
            if (cmd >= ID_TRAY_PULF_MODE_BASE && cmd <= ID_TRAY_PULF_MAX)
            {
                if (cmd >= ID_TRAY_PULF_ND_BASE)
                    app->pulfrichNd = (int)(cmd - ID_TRAY_PULF_ND_BASE);
                else if (cmd >= ID_TRAY_PULF_DELAY_BASE)
                    app->pulfrichDelay = (int)(cmd - ID_TRAY_PULF_DELAY_BASE) + 1;
                else
                    app->pulfrichMode = (cmd == ID_TRAY_PULF_MODE_BASE) ? PulfrichMode::TimeDelay
                                                                        : PulfrichMode::NDFilter;
                ChangeFormat(*app, StereoFormat::Pulfrich);
                ManualInputChosen(*app);
                return 0;
            }
            if (cmd >= ID_TRAY_FP_BASE && cmd <= ID_TRAY_FP_MAX)
            {
                app->framePackMode = (int)(cmd - ID_TRAY_FP_BASE);
                ChangeFormat(*app, StereoFormat::FramePacking);
                ManualInputChosen(*app);
                return 0;
            }

            switch (cmd)
            {
            case ID_TRAY_OPEN_UPDATE:
                // Tray "Update available" item / GUI update banner.
                if (!app->pendingUpdateUrl.empty())
                    ShellExecuteA(nullptr, "open", app->pendingUpdateUrl.c_str(),
                                  nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            case ID_TRAY_OPEN_PANEL:
                // Same behaviour as left-clicking the tray icon: show the
                // GUI (or bring it to front if already visible).
                if (!app->gui.IsVisible()) app->gui.Toggle();
                if (app->gui.Hwnd()) SetForegroundWindow(app->gui.Hwnd());
                return 0;
            case ID_TRAY_TOGGLE_WEAVE: SetWeaving(*app, !app->weavingEnabled); return 0;
            case ID_TRAY_MODE_FULLSCREEN:
                app->mode = OutputMode::Fullscreen;
                // (Used to flip TestImage -> passthrough here; we keep the test
                // image so quilt / SBS test files can be viewed fullscreen on the
                // SR display.)
                EnsureWeaving(*app);
                return 0;
            case ID_TRAY_MODE_WINDOWED:
                app->mode = OutputMode::Windowed;
                if (app->weavingEnabled) ApplyMode(*app);
                return 0;
            case ID_TRAY_MODE_OVERLAY:
                app->mode = OutputMode::WindowOverlay;
                if (app->weavingEnabled) ApplyMode(*app);
                return 0;
            case ID_TRAY_CAPTURE_FOREGROUND: CaptureForeground(*app); return 0;
            case ID_TRAY_SWAP_EYES: app->swapEyes = !app->swapEyes; app->captureRebind = true; return 0;
            case ID_TRAY_DETECT: DetectFormat(*app); return 0;
            case ID_TRAY_SRC_TESTIMAGE: OpenLoadTestImageDialog(*app); return 0;
            case ID_TRAY_AUTO_STEREO:
                // Stereo 3D Input -> Automatic Detection. Applies now if
                // weaving, otherwise when weaving is turned on.
                app->autoInput   = true;
                app->inputCtxKey = 0;
                if (app->format == StereoFormat::Katanga) ChangeFormat(*app, StereoFormat::HalfSBS);
                UpdateInputChoice(*app);
                return 0;
            case ID_TRAY_SRC_MONITOR:
                if (!app->autoInput) EndAutoStereo(*app, "Fullscreen chosen");   // back to weaving the whole display
                app->autoScopeWindow = nullptr;   // (the whole display, not a chosen window)
                UsePassthrough(*app);
                app->mode = OutputMode::Fullscreen;   // Monitor always means fullscreen
                EnsureWeaving(*app);
                return 0;
            case ID_TRAY_LOOKING_GLASS:
                if (app->autoStereo && !app->autoInput) EndAutoStereo(*app, "Looking Glass chosen");
                UsePassthrough(*app);
                app->mode = OutputMode::LookingGlass;
                app->loupeInteractive = false;  // click-through by default; Ctrl+Alt to move
                EnsureWeaving(*app);
                return 0;
            case ID_TRAY_EXIT: DestroyWindow(hwnd); return 0;
            case ID_TRAY_HT_TOGGLE:
            {
                // Master tracking toggle: any on -> all off; all off ->
                // restore the everything-on default. Same semantics as
                // the small toggle in the compact GUI panel; openTrack-
                // UserDisabled is updated by the SetOutputs result so
                // the auto-on-on-weave-start path respects this choice.
                const bool anyOn = app->openTrack.IsEnabled();
                app->openTrack.SetOutputs(!anyOn, !anyOn, !anyOn);
                app->openTrackUserDisabled = anyOn;   // "anyOn" was true -> we just turned off
                return 0;
            }
            case ID_TRAY_HT_RECENTER:   // (as Ctrl+Alt+R)
                app->openTrack.CalibrateNeutral();
                return 0;
            case ID_TRAY_HT_PROTO_OT:
            case ID_TRAY_HT_PROTO_FT:
            case ID_TRAY_HT_PROTO_TIR:
            {
                bool ot  = app->openTrack.IsOpenTrackEnabled();
                bool ft  = app->openTrack.IsFreeTrackEnabled();
                bool tir = app->openTrack.IsTrackIREnabled();
                if (cmd == ID_TRAY_HT_PROTO_OT)  ot  = !ot;
                if (cmd == ID_TRAY_HT_PROTO_FT)  ft  = !ft;
                if (cmd == ID_TRAY_HT_PROTO_TIR) tir = !tir;
                app->openTrack.SetOutputs(ot, ft, tir);
                app->openTrackUserDisabled = !(ot || ft || tir);
                return 0;
            }
            }
            // Head-tracking output-mode radios (range).
            if (cmd >= ID_TRAY_HT_MODE_BASE && cmd <= ID_TRAY_HT_MODE_MAX)
            {
                const int idx = (int)(cmd - ID_TRAY_HT_MODE_BASE);
                // Map menu index (0..4) -> pipeline mode value (1..5) --
                // they're aligned 1:1 in the kHTModes order in TrayIcon.cpp.
                const int mode = idx + 1;
                auto cfg = app->openTrack.GetConfig();
                cfg.outputMode = mode;
                app->openTrack.SetConfig(cfg);
                return 0;
            }
            // Profiles submenu commands.
            if (cmd == ID_TRAY_PROFILES_AUTO)
            {
                app->profilesAutoApply = !app->profilesAutoApply;
                Settings::WriteAutoApplyProfiles(app->profilesAutoApply);
                RefreshForegroundHook(*app);
                return 0;
            }
            if (cmd == ID_TRAY_PROFILES_SAVECUR)
            {
                SaveCurrentAsProfile(*app);
                return 0;
            }
            if (cmd == ID_TRAY_PROFILES_OPEN_INI)
            {
                // Open profiles.ini in the user's default text editor.
                // Save first so the file exists with the latest state +
                // the helpful header comments even on a fresh install.
                Profiles::Save(app->profiles);
                PWSTR base = nullptr;
                if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)) && base)
                {
                    std::wstring p = base; CoTaskMemFree(base);
                    p += L"\\SRLoom\\profiles.ini";
                    ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                return 0;
            }
            // Manual-apply: click a profile name in the list. Use the last
            // external foreground's title as the auto-format input (if the
            // profile is set to auto-format) -- the user's picking this
            // profile FOR that window, so its title is the right hint.
            if (cmd >= ID_TRAY_PROFILES_LIST_BASE && cmd <= ID_TRAY_PROFILES_LIST_MAX)
            {
                const size_t idx = (size_t)(cmd - ID_TRAY_PROFILES_LIST_BASE);
                if (idx < app->profiles.size())
                {
                    const std::string title = WindowTitle(app->lastExternalForeground);
                    ApplyProfile(*app, app->profiles[idx], nullptr, title);
                }
                return 0;
            }
            // Delete: click a profile name in the Delete submenu.
            if (cmd >= ID_TRAY_PROFILES_DEL_BASE && cmd <= ID_TRAY_PROFILES_DEL_MAX)
            {
                const size_t idx = (size_t)(cmd - ID_TRAY_PROFILES_DEL_BASE);
                if (idx < app->profiles.size())
                {
                    const std::string name = app->profiles[idx].name;
                    Log("Profiles: deleted '%s'", name.c_str());
                    app->profiles.erase(app->profiles.begin() + idx);
                    Profiles::Save(app->profiles);
                    if (app->lastAppliedProfile == name)
                        app->lastAppliedProfile.clear();
                    if (app->lastAutoAppliedProfile == name)
                        app->lastAutoAppliedProfile.clear();
                }
                return 0;
            }
            break;
        }

        case WM_HOTKEY:
            if (!app) break;
            if (wParam == kHotkeyToggle) SetWeaving(*app, !app->weavingEnabled);
            else if (wParam == kHotkeyMode)
            {
                // Toggle Fullscreen <-> Looking Glass (both weave the monitor).
                if (app->source != SourceKind::CaptureMonitor) UsePassthrough(*app);
                if (app->mode == OutputMode::LookingGlass)
                    app->mode = OutputMode::Fullscreen;
                else { app->mode = OutputMode::LookingGlass; app->loupeInteractive = false; }
                EnsureWeaving(*app);
            }
            else if (wParam == kHotkeyCapture)
            {
                // Make the active window 3D; press again to turn it back off.
                if (app->mode == OutputMode::WindowOverlay && app->weavingEnabled)
                    SetWeaving(*app, false);
                else
                    CaptureForeground(*app);
            }
            // else if (wParam == kHotkeyDetect)  DetectFormat(*app); // auto-detect disabled
            else if (wParam == kHotkeyAutoRegion || wParam == kHotkeyAutoRegionDbg)
            {
                // Auto Stereo: weave / un-weave the image under the cursor.
                // Log which app was in front -- to diagnose apps that swallow
                // Ctrl+Alt+A when they're focused.
                HWND fg = GetForegroundWindow();
                Log("Hotkey Ctrl+Alt+%sA received (foreground: '%s' / %s)",
                    wParam == kHotkeyAutoRegionDbg ? "Shift+" : "",
                    WindowTitle(fg).c_str(), ForegroundExeBaseName(fg).c_str());
                app->pickDebug = (wParam == kHotkeyAutoRegionDbg);
                ToggleAutoRegionUnderCursor(*app);
            }
            else if (wParam == kHotkeyCalibrate)
            {
                // Recenter head tracking: snap the current head pose to
                // "neutral" so games see (0,0,0) / (0,0,0) at the user's
                // current position + orientation. No-op if the bridge
                // isn't enabled (no listener => no last pose to snap from).
                app->openTrack.CalibrateNeutral();
            }
            return 0;

        case WM_SIZE:
            if (app && wParam != SIZE_MINIMIZED)
                app->renderer.Resize(LOWORD(lParam), HIWORD(lParam));
            if (app)
            {
                // Minimised -> the main window is gone from the screen, so the
                // floating overlays would be orphaned. Hide them.
                if (wParam == SIZE_MINIMIZED) { HideFsCtrlOverlay(); HideFsSetOverlay(); HideFsVidOverlay(); }
                else                          RepositionOverlays(*app);

                // The Looking Glass got maximised by a route that bypasses
                // SC_MAXIMIZE (Aero Snap drag-to-top, Win+Up): switch to
                // Fullscreen instead, same as the Maximise button. Remember
                // the loupe's pre-maximise rect (the placement's "normal"
                // rect, in workspace coords -> screen) so it comes back there.
                if (wParam == SIZE_MAXIMIZED && app->mode == OutputMode::LookingGlass)
                {
                    WINDOWPLACEMENT wp{ sizeof(wp) };
                    if (GetWindowPlacement(hwnd, &wp))
                    {
                        RECT r = wp.rcNormalPosition;
                        MONITORINFO mi{ sizeof(mi) };
                        if (GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
                            OffsetRect(&r, mi.rcWork.left - mi.rcMonitor.left,
                                           mi.rcWork.top  - mi.rcMonitor.top);
                        app->loupeSavedRect = r;
                        app->loupeHasSaved  = true;
                    }
                    app->loupeActive = false;   // don't let ApplyMode save the maximised rect
                    app->mode = OutputMode::Fullscreen;
                    PostMessage(hwnd, WM_APP_APPLY_MODE, 0, 0);
                }
            }
            return 0;

        case WM_APP_APPLY_MODE:
            if (app) ApplyMode(*app);
            return 0;

        // While the user drags/resizes the window, the modal move loop blocks our
        // render loop — which stops weave() and freezes head-tracked weaving. Keep
        // rendering from a timer during the move.
        // Looking Glass: move / resize it OURSELVES instead of letting Windows
        // run its modal move loop (which stalls our render loop -- the glass
        // dropped to timer-driven ~60 fps while being dragged). Pressing on
        // the title bar or an edge starts it; the render loop then follows
        // the mouse every frame (UpdateLoupeOwnDrag) until the button's up.
        case WM_NCLBUTTONDOWN:
            if (app && app->mode == OutputMode::LookingGlass &&
                (wParam == HTCAPTION || (wParam >= HTLEFT && wParam <= HTBOTTOMRIGHT)))
            {
                app->loupeDrag.active = true;
                app->loupeDrag.hit    = (int)wParam;
                GetCursorPos(&app->loupeDrag.start);
                GetWindowRect(hwnd, &app->loupeDrag.startRect);
                app->loupeDragging = true;
                SetCapture(hwnd);
                return 0;
            }
            return DefWindowProc(hwnd, msg, wParam, lParam);
        case WM_CAPTURECHANGED:
            if (app && app->loupeDrag.active && (HWND)lParam != hwnd)
            {
                app->loupeDrag.active = false;
                app->loupeDragging = false;
            }
            return 0;

        case WM_ENTERSIZEMOVE:
            if (app) app->loupeDragging = true;
            SetTimer(hwnd, kRenderTimer, 8, nullptr);
            return 0;
        case WM_EXITSIZEMOVE:
            if (app) app->loupeDragging = false;
            KillTimer(hwnd, kRenderTimer);
            return 0;
        case WM_TIMER:
            if (app && wParam == kRenderTimer)
                RenderFrame(*app);
            return 0;

        case WM_GETMINMAXINFO:
        {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 200;
            mmi->ptMinTrackSize.y = 200;
            return 0;
        }

        case WM_SIZING:
        {
            // Lock the windowed aspect when displaying a test image with a
            // known per-eye aspect (LFP, video, image). Without this the
            // user could resize the window to a random shape and the
            // image would stretch / leave large bars. Adjust the rect on
            // the EDGE the user is dragging so it feels natural.
            if (!app || app->mode != OutputMode::Windowed
                || app->source != SourceKind::TestImage)
                return DefWindowProc(hwnd, msg, wParam, lParam);

            // Compute the same per-eye aspect ApplyMode uses.
            int sw = 0, sh = 0;
            if (app->format == StereoFormat::LightField && app->lfpRenderer.HasData())
            { sw = app->lfpRenderer.OutputPerEyeWidth(); sh = app->lfpRenderer.OutputHeight(); }
            else if (app->video.IsOpen()) { sw = app->video.Width(); sh = app->video.Height(); }
            else                          { sw = app->weaver.SourceWidth(); sh = app->weaver.SourceHeight(); }
            if (sw <= 0 || sh <= 0)
                return DefWindowProc(hwnd, msg, wParam, lParam);
            double aw = (double)sw, ah = (double)sh;
            switch (app->format)
            {
            case StereoFormat::Quilt:
                if (app->quiltCols > 0 && app->quiltRows > 0)
                { aw /= app->quiltCols; ah /= app->quiltRows; }
                break;
            case StereoFormat::FullSBS:
            case StereoFormat::HalfSBS:           aw *= 0.5; break;
            case StereoFormat::FullTAB:
            case StereoFormat::HalfTAB:
            case StereoFormat::RowInterleaved:    ah *= 0.5; break;
            case StereoFormat::ColumnInterleaved: aw *= 0.5; break;
            default: break;
            }
            if (aw <= 0 || ah <= 0)
                return DefWindowProc(hwnd, msg, wParam, lParam);
            const double aspect = aw / ah;

            // We're adjusting the WINDOW rect, not the client rect. Subtract
            // the chrome (frame + caption) so the *client* keeps the
            // correct aspect, then re-add.
            RECT* r = reinterpret_cast<RECT*>(lParam);
            RECT chrome{};
            ::AdjustWindowRectEx(&chrome, (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE),
                                 FALSE, (DWORD)GetWindowLongPtr(hwnd, GWL_EXSTYLE));
            const int chromeW = chrome.right  - chrome.left;
            const int chromeH = chrome.bottom - chrome.top;
            const int curW = (r->right  - r->left) - chromeW;
            const int curH = (r->bottom - r->top)  - chromeH;
            if (curW <= 0 || curH <= 0)
                return DefWindowProc(hwnd, msg, wParam, lParam);

            // wParam tells us which edge / corner is being dragged. Width-
            // adjusts override (drag left/right edge), height-adjusts on
            // top/bottom, corners follow whichever side has more change.
            const WPARAM edge = wParam;
            const bool widthEdge  = (edge == WMSZ_LEFT || edge == WMSZ_RIGHT);
            const bool heightEdge = (edge == WMSZ_TOP  || edge == WMSZ_BOTTOM);
            int targetW = curW, targetH = curH;
            if (widthEdge)        targetH = (int)(targetW / aspect + 0.5);
            else if (heightEdge)  targetW = (int)(targetH * aspect + 0.5);
            else { /* corner: use the larger relative change */
                const double byW = (double)targetW / aspect;   // implied H from W
                const double byH = (double)targetH * aspect;   // implied W from H
                if (std::abs(byW - targetH) < std::abs(byH - targetW))
                    targetH = (int)(targetW / aspect + 0.5);
                else
                    targetW = (int)(targetH * aspect + 0.5);
            }

            // Apply the new rect, anchoring on the dragged edge.
            if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT)
                r->left  = r->right  - (targetW + chromeW);
            else
                r->right = r->left   + (targetW + chromeW);
            if (edge == WMSZ_TOP  || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT)
                r->top    = r->bottom - (targetH + chromeH);
            else
                r->bottom = r->top    + (targetH + chromeH);
            return TRUE;
        }




        case WM_SYSCOMMAND:
            // Hijack the native title-bar Maximise -- a true OS maximise would
            // just fill the monitor with chrome still showing, which doesn't
            // match the SR Loom mental model of "go fullscreen". Switch to
            // OutputMode::Fullscreen instead, which removes the chrome AND
            // lays the woven content full-panel (3D works correctly there).
            // Mask off the low 4 bits per SC_* docs (they're internal use).
            if (app && (wParam & 0xFFF0) == SC_MAXIMIZE &&
                app->source == SourceKind::TestImage &&
                app->mode   == OutputMode::Windowed)
            {
                app->mode = OutputMode::Fullscreen;
                ApplyMode(*app);
                return 0;
            }
            // Same for the Looking Glass (Maximise button or title-bar double-
            // click): go Fullscreen on the same source. A real OS maximise of
            // this layered window also made Windows draw the old classic
            // (Win95-style) frame, since DWM doesn't theme a maximised layered
            // window's caption. The loupe's rect is remembered (ApplyMode), so
            // Ctrl+Alt+F / the Looking Glass button brings it back in place.
            if (app && (wParam & 0xFFF0) == SC_MAXIMIZE &&
                app->mode == OutputMode::LookingGlass)
            {
                app->mode = OutputMode::Fullscreen;
                ApplyMode(*app);
                return 0;
            }
            break;   // let DefWindowProc handle every other system command

        case WM_DISPLAYCHANGE:
            // A display was added, removed, moved or changed resolution / refresh.
            // Handled in the main loop once Windows has settled (HandleDisplayChange).
            if (app) app->displayChangedAtMs = GetTickCount64();
            break;

        case WM_SETTINGCHANGE:
            // Windows light/dark app mode may have changed: re-theme the
            // native title bar (Looking Glass / Windowed). Cheap, idempotent.
            if (app && (GetWindowLongPtr(hwnd, GWL_STYLE) & WS_CAPTION))
                ApplyCaptionTheme(hwnd);
            break;

        case WM_CLOSE:
            // Closing the window just pauses weaving and hides to the tray.
            // Hide the floating overlays explicitly so they don't linger as
            // detached popups after the main window's gone.
            HideFsCtrlOverlay();
            HideFsSetOverlay();
            HideFsVidOverlay();
            if (app) SetWeaving(*app, false);
            return 0;

        case WM_DESTROY:
            HideFsCtrlOverlay();
            HideFsSetOverlay();
            HideFsVidOverlay();
            if (app && hwnd != app->hwnd) return 0;   // a replaced window (presenter fallback)
            PostQuitMessage(0);
            return 0;

        case WM_ERASEBKGND:
            return 1;  // we fully draw every frame
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

// One pass of the main loop's work, in two parts so each thread runs its own
// (see g_appLock): `frame` is the logic and RenderFrame (the render thread's),
// `panel` the control panel (the UI thread's -- its window is that thread's).
// With no render thread (Settings RenderThread = 0) one thread runs both.
// The caller holds the lock; the waits inside let go of it.
static void LoopBody(AppState& app, bool frame, bool panel)
{
    if (frame)
    {
        g_stall.Mark("render loop");
        UpdateInputChoice(app);      // Stereo 3D Input "Automatic Detection"
        UpdateManualEyeOrder(app);   // (a whole picture's eye order: layouts picked by hand)
        UpdateManualAnaColour(app);  // (Anaglyph picked by hand: a black-and-white picture under it?)
        UpdateLastForeground(app);   // remember the user's active window for "make 3D"
        RenderFrame(app);

    }
    if (panel)
    {
        // Panel just opened -> re-check GitHub for a new release (the
        // checker's 1-hour throttle keeps this from hammering the API).
        {
            static bool s_guiWasVisible = false;
            const bool guiVisible = app.gui.IsVisible();
            if (guiVisible && !s_guiWasVisible)
                UpdateChecker::StartAsync(app.hwnd, WM_APP_UPDATE_RESULT, false);
            s_guiWasVisible = guiVisible;
        }

        // Render the control panel when it's open, and pick up its convergence slider.
        if (app.gui.IsVisible())
        {
            GuiState gs;
            if (!app.pendingUpdateUrl.empty()) gs.updateTag = app.pendingUpdateTag;
            gs.weaving       = app.weavingEnabled;
            gs.mode          = app.mode;
            gs.source        = app.source;
            gs.format        = app.format;
            gs.swapEyes      = app.swapEyes;
            gs.anaglyphCombo = app.anaglyphCombo;
            for (int c = 0; c < 3; ++c) { gs.anaCustomL[c] = app.anaCustomL[c]; gs.anaCustomR[c] = app.anaCustomR[c]; }
            gs.anaCustomChanged = false;
            gs.anaSavedCount = app.anaSavedCount;
            for (int k = 0; k < app.anaSavedCount; ++k) for (int c = 0; c < 6; ++c) gs.anaSaved[k][c] = app.anaSaved[k][c];
            gs.anaSavedAdd = false; gs.anaSavedLoad = gs.anaSavedDelete = -1;
            gs.anaPickRequest = 0; gs.anaPickActive = app.eyedrop;
            gs.anaglyphMode  = app.anaglyphMode;
            gs.convergence   = app.convergence;
            gs.pulfrichMode  = (int)app.pulfrichMode;
            gs.pulfrichDelay = app.pulfrichDelay;
            gs.pulfrichNd    = app.pulfrichNd;
            gs.framePackMode = app.framePackMode;
            gs.srMonitor     = SrMonitor(app);        // "this display" (excluded from the picker)
            gs.autoStereo    = app.autoDetect;
            gs.autoInput     = app.autoInput;
            gs.defaultInput  = app.defaultInput;
            if (app.autoInput && app.weavingEnabled)
            {
                auto addFmt = [&](StereoFormat f) {
                    for (int i = 0; i < gs.autoFmtCount; ++i) if (gs.autoFmts[i] == f) return;
                    if (gs.autoFmtCount < 4) gs.autoFmts[gs.autoFmtCount++] = f;
                };
                if (app.autoStereo)
                    for (const WeaveRegion& r : app.regionWeaver.Regions()) addFmt(r.format);
                else
                    addFmt(app.format);   // (the whole picture's layout)
            }
            gs.captureMonitor = app.sourceMonitor;    // currently-captured display
            gs.foreignDisplay = app.foreignDisplay;   // a picked display (vs this one) is active
            gs.quiltCols     = app.quiltCols;
            gs.quiltRows     = app.quiltRows;
            gs.hasTestImage  = !app.lastTestImagePath.empty();
            gs.vrHeadLook        = app.vrHeadLook;
            gs.vrZoom            = app.vrZoom;
            gs.vrResetView       = false;
            gs.vrResetZoom       = false;
            gs.vrHeadLookChanged = false;
            // SR Platform runtime version (e.g. "1.34.10.17449") for
            // the About popup. Static helper; pull it fresh each frame
            // so a runtime hot-swap (rare) updates the GUI display.
            static const std::string s_srVersion = [] { const char* v = SRWeaver::GetSRPlatformVersion(); return std::string(v ? v : ""); }();
            const char* sr = s_srVersion.c_str();
            strncpy_s(gs.srPlatformVersion, sr ? sr : "", _TRUNCATE);
            // Light-field parallax-scale state for the GUI slider.
            gs.lfpHeadLeanMm      = app.lfpHeadLeanMm;
            gs.lfpApertureMm      = (float)(app.lfpRenderer.ApertureDiameterMetres() * 1e3);
            gs.lfpHeadLeanChanged = false;
            // OpenTrack snapshot. Mirror live config + counters into
            // the GuiState; GUI mutates + sets openTrackChanged on
            // edit, which we apply below.
            {
                auto ot = app.openTrack.GetConfig();
                gs.openTrackEnabled      = app.openTrack.IsOpenTrackEnabled();
                gs.freeTrackEnabled      = app.openTrack.IsFreeTrackEnabled();
                gs.trackIREnabled        = app.openTrack.IsTrackIREnabled();
                gs.trackIRAvailable      = !app.npClientDir.empty();
                gs.openTrackSensYaw      = ot.sensYaw;
                gs.openTrackSensPitch    = ot.sensPitch;
                gs.openTrackSensRoll     = ot.sensRoll;
                gs.openTrackMode         = ot.outputMode;
                gs.openTrackInvertX      = ot.invertX;
                gs.openTrackInvertY      = ot.invertY;
                gs.openTrackInvertZ      = ot.invertZ;
                gs.openTrackInvertYaw    = ot.invertYaw;
                gs.openTrackInvertPitch  = ot.invertPitch;
                gs.openTrackInvertRoll   = ot.invertRoll;
                gs.openTrackChanged      = false;
                gs.openTrackCalibrate    = false;
                gs.openTrackSentPackets  = app.openTrack.SentPackets();
                strncpy_s(gs.openTrackExePath, app.openTrackExePath.c_str(), _TRUNCATE);
            }
            // Profiles snapshot: name + includeHT per entry, plus
            // master toggle. Click flags (save / apply / delete /
            // toggle-HT / open-ini / autoApply-changed) are RESET
            // here and READ after Render below -- one-shot events
            // triggered by user clicks during the frame.
            {
                gs.profileEntries.clear();
                gs.profileEntries.reserve(app.profiles.size());
                for (const auto& p : app.profiles)
                {
                    GuiState::ProfileEntry e;
                    e.name = p.name;
                    e.includeHT      = p.includeHeadTracking;
                    e.fullscreenOnly = p.fullscreenOnly;
                    e.useAutoFormat  = p.useAutoFormat;
                    gs.profileEntries.push_back(std::move(e));
                }
                gs.profilesAutoApply        = app.profilesAutoApply;
                gs.profilesAutoApplyChanged = false;
                gs.profileSaveCurrent       = false;
                gs.profileApplyIndex        = -1;
                gs.profileUpdateIndex       = -1;
                gs.profileDeleteIndex       = -1;
                gs.profileToggleHTIndex     = -1;
                gs.profileToggleFullscreenIndex = -1;
                gs.profileToggleAutoFormatIndex = -1;
                gs.profilesOpenIni          = false;
            }
            // What's being weaved, for the GUI's collapsed summary line.
            if (app.source == SourceKind::CaptureWindow && app.sourceWindow && IsWindow(app.sourceWindow))
            {
                if (GetWindowTextA(app.sourceWindow, gs.sourceName, (int)sizeof(gs.sourceName)) <= 0)
                    strncpy_s(gs.sourceName, "Window", _TRUNCATE);
            }
            else if (app.autoScopeWindow && app.autoStereo && IsWindow(app.autoScopeWindow))
            {
                gs.windowScoped = true;   // (Automatic: the chosen window's 3D pictures)
                if (GetWindowTextA(app.autoScopeWindow, gs.sourceName, (int)sizeof(gs.sourceName)) <= 0)
                    strncpy_s(gs.sourceName, "Window", _TRUNCATE);
            }
            else
                strncpy_s(gs.sourceName, app.foreignDisplay ? "Display" : "Monitor", _TRUNCATE);
            const auto tGui0 = std::chrono::steady_clock::now();
            bool guiChanged = false;
            // (Drawn with the lock let go: the panel has its own device and reads
            // only gs; the render thread carries on meanwhile.)
            { HitchWatch hw("panel"); AppUnlock unlock; guiChanged = app.gui.Render(gs); }
            app.prof.gui += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tGui0).count();
            if (guiChanged)
            {
                app.convergence   = gs.convergence;
                app.captureRebind = true;   // re-run the conversion with the new convergence
            }
            // (The Custom pair's saved pairs and eyedropper: asked for in the panel.)
            if (gs.anaSavedAdd && app.anaSavedCount < 8)
            {
                for (int c = 0; c < 3; ++c) { app.anaSaved[app.anaSavedCount][c] = app.anaCustomL[c]; app.anaSaved[app.anaSavedCount][3 + c] = app.anaCustomR[c]; }
                ++app.anaSavedCount;
                Settings::WriteAnaSaved(app.anaSaved, app.anaSavedCount);
            }
            if (gs.anaSavedLoad >= 0 && gs.anaSavedLoad < app.anaSavedCount)
            {
                for (int c = 0; c < 3; ++c) { app.anaCustomL[c] = app.anaSaved[gs.anaSavedLoad][c]; app.anaCustomR[c] = app.anaSaved[gs.anaSavedLoad][3 + c]; }
                Settings::WriteAnaCustom(app.anaCustomL, app.anaCustomR);
            }
            if (gs.anaSavedDelete >= 0 && gs.anaSavedDelete < app.anaSavedCount)
            {
                for (int k = gs.anaSavedDelete; k + 1 < app.anaSavedCount; ++k) for (int c = 0; c < 6; ++c) app.anaSaved[k][c] = app.anaSaved[k + 1][c];
                --app.anaSavedCount;
                Settings::WriteAnaSaved(app.anaSaved, app.anaSavedCount);
            }
            if (gs.anaPickRequest) EyedropStart(app, gs.anaPickRequest);
            if (gs.anaCustomChanged)   // (the Custom anaglyph pair's colours, picked in the panel)
            {
                for (int c = 0; c < 3; ++c) { app.anaCustomL[c] = gs.anaCustomL[c]; app.anaCustomR[c] = gs.anaCustomR[c]; }
                Settings::WriteAnaCustom(app.anaCustomL, app.anaCustomR);
            }
            // VR controls.
            if (gs.vrHeadLookChanged)
            {
                app.vrHeadLook = gs.vrHeadLook;
                app.captureRebind = true;
            }
            if (gs.lfpHeadLeanChanged)
            {
                app.lfpHeadLeanMm = gs.lfpHeadLeanMm;
            }
            if (gs.vrResetView)
            {
                app.vrYaw   = 0.0f;
                app.vrPitch = 0.0f;
                app.captureRebind = true;
            }
            if (gs.vrResetZoom)
            {
                app.vrZoom = 1.0f;
                app.captureRebind = true;
            }
            // OpenTrack/FreeTrack state apply-back.
            if (gs.openTrackChanged)
            {
                // Manual toggle is a user override against the auto-on
                // policy: if ALL outputs are off, remember that the
                // user explicitly turned tracking off this session so
                // we don't auto-re-enable on the next render tick.
                // TrackIR only counts if its prerequisites are met --
                // a ticked-but-unavailable TrackIR doesn't keep the
                // bridge alive.
                const bool tirEffective = gs.trackIREnabled && gs.trackIRAvailable;
                app.openTrackUserDisabled =
                    !(gs.openTrackEnabled || gs.freeTrackEnabled || tirEffective);
                // Push the new output set to the bridge. SetOutputs is
                // idempotent and only does work on the delta; failure
                // (e.g. SR Platform service offline) flips the GuiState
                // back so the UI doesn't lie about what's running.
                if (!app.openTrack.SetOutputs(gs.openTrackEnabled,
                                              gs.freeTrackEnabled,
                                              tirEffective))
                {
                    gs.openTrackEnabled = app.openTrack.IsOpenTrackEnabled();
                    gs.freeTrackEnabled = app.openTrack.IsFreeTrackEnabled();
                    gs.trackIREnabled   = app.openTrack.IsTrackIREnabled();
                }
                // Config edits (sliders / mode / inverts).
                if (app.openTrack.IsEnabled())
                {
                    auto cfg = app.openTrack.GetConfig();
                    cfg.sensYaw     = gs.openTrackSensYaw;
                    cfg.sensPitch   = gs.openTrackSensPitch;
                    cfg.sensRoll    = gs.openTrackSensRoll;
                    cfg.outputMode  = gs.openTrackMode;
                    cfg.invertX     = gs.openTrackInvertX;
                    cfg.invertY     = gs.openTrackInvertY;
                    cfg.invertZ     = gs.openTrackInvertZ;
                    cfg.invertYaw   = gs.openTrackInvertYaw;
                    cfg.invertPitch = gs.openTrackInvertPitch;
                    cfg.invertRoll  = gs.openTrackInvertRoll;
                    app.openTrack.SetConfig(cfg);
                }
            }
            if (gs.openTrackCalibrate)
                app.openTrack.CalibrateNeutral();
            // Profiles apply-back: consume click flags from the GUI.
            if (gs.profilesAutoApplyChanged)
            {
                app.profilesAutoApply = gs.profilesAutoApply;
                Settings::WriteAutoApplyProfiles(app.profilesAutoApply);
                RefreshForegroundHook(app);
            }
            if (gs.profileSaveCurrent)
                SaveCurrentAsProfile(app);
            if (gs.profileApplyIndex >= 0 &&
                (size_t)gs.profileApplyIndex < app.profiles.size())
            {
                // Manual apply (tray menu path) -- no captureHwnd,
                // format-only re-bind. Pass the last external foreground
                // window's title so auto-format profiles get a useful
                // hint.
                const std::string title = WindowTitle(app.lastExternalForeground);
                ApplyProfile(app, app.profiles[(size_t)gs.profileApplyIndex],
                             nullptr, title);
            }
            if (gs.profileUpdateIndex >= 0 &&
                (size_t)gs.profileUpdateIndex < app.profiles.size())
            {
                // Update: overwrite the selected profile's settings
                // with the current state. Keeps name + exe + title +
                // includeHeadTracking; refreshes everything else.
                // Lets the user "I tweaked the format/swap mid-game,
                // save those tweaks back to the profile."
                Profile& p = app.profiles[(size_t)gs.profileUpdateIndex];
                p.format       = app.format;
                // Auto-format profiles persist only defaultformat, so
                // that's where the current format has to go for Update
                // to stick (it becomes the no-token fallback).
                if (p.useAutoFormat) p.defaultFormat = app.format;
                p.swapEyes     = app.swapEyes;
                p.convergence  = app.convergence;
                p.anaglyphCombo  = app.anaglyphCombo;
                p.anaglyphMode   = app.anaglyphMode;
                p.pulfrichMode   = (int)app.pulfrichMode;
                p.pulfrichDelay  = app.pulfrichDelay;
                p.pulfrichNd     = app.pulfrichNd;
                p.framePackMode  = app.framePackMode;
                p.quiltCols      = app.quiltCols;
                p.quiltRows      = app.quiltRows;
                p.quiltLeftIdx   = app.quiltLeftIdx;
                p.quiltRightIdx  = app.quiltRightIdx;
                // Refresh the HT snapshot too if the profile is
                // currently flagged to carry HT settings -- same
                // "you're updating the profile, capture everything"
                // semantic as the per-row HT-on toggle.
                if (p.includeHeadTracking)
                {
                    p.htOpenTrack   = app.openTrack.IsOpenTrackEnabled();
                    p.htFreeTrack   = app.openTrack.IsFreeTrackEnabled();
                    p.htTrackIR     = app.openTrack.IsTrackIREnabled();
                    const auto cfg  = app.openTrack.GetConfig();
                    p.htOutputMode  = cfg.outputMode;
                    p.htInvertX     = cfg.invertX;
                    p.htInvertY     = cfg.invertY;
                    p.htInvertZ     = cfg.invertZ;
                    p.htInvertYaw   = cfg.invertYaw;
                    p.htInvertPitch = cfg.invertPitch;
                    p.htInvertRoll  = cfg.invertRoll;
                }
                Profiles::Save(app.profiles);
                Log("Profiles: updated '%s' from current state", p.name.c_str());
            }
            if (gs.profileDeleteIndex >= 0 &&
                (size_t)gs.profileDeleteIndex < app.profiles.size())
            {
                const std::string name = app.profiles[(size_t)gs.profileDeleteIndex].name;
                Log("Profiles: deleted '%s'", name.c_str());
                app.profiles.erase(app.profiles.begin() + gs.profileDeleteIndex);
                Profiles::Save(app.profiles);
                if (app.lastAppliedProfile == name)
                    app.lastAppliedProfile.clear();
                if (app.lastAutoAppliedProfile == name)
                    app.lastAutoAppliedProfile.clear();
            }
            if (gs.profileToggleHTIndex >= 0 &&
                (size_t)gs.profileToggleHTIndex < app.profiles.size())
            {
                Profile& p = app.profiles[(size_t)gs.profileToggleHTIndex];
                p.includeHeadTracking = !p.includeHeadTracking;
                // When the user flips includeHT ON, snapshot the
                // current HT state -- they're saying "remember the
                // HT setup I have RIGHT NOW for this game". Otherwise
                // the saved ht_* fields are whatever was there at
                // save-time, which may be stale.
                if (p.includeHeadTracking)
                {
                    p.htOpenTrack   = app.openTrack.IsOpenTrackEnabled();
                    p.htFreeTrack   = app.openTrack.IsFreeTrackEnabled();
                    p.htTrackIR     = app.openTrack.IsTrackIREnabled();
                    const auto cfg  = app.openTrack.GetConfig();
                    p.htOutputMode  = cfg.outputMode;
                    p.htInvertX     = cfg.invertX;
                    p.htInvertY     = cfg.invertY;
                    p.htInvertZ     = cfg.invertZ;
                    p.htInvertYaw   = cfg.invertYaw;
                    p.htInvertPitch = cfg.invertPitch;
                    p.htInvertRoll  = cfg.invertRoll;
                }
                Profiles::Save(app.profiles);
            }
            if (gs.profileToggleFullscreenIndex >= 0 &&
                (size_t)gs.profileToggleFullscreenIndex < app.profiles.size())
            {
                Profile& p = app.profiles[(size_t)gs.profileToggleFullscreenIndex];
                p.fullscreenOnly = !p.fullscreenOnly;
                Profiles::Save(app.profiles);
                // If this profile is the one currently auto-applied,
                // bring the live state in line with the new setting
                // right away instead of waiting for the next focus change.
                HWND h = app.lastAppliedHwnd;
                if (p.name == app.lastAutoAppliedProfile && h)
                {
                    if (!p.fullscreenOnly)
                    {
                        // Turned OFF: stop the "leave fullscreen -> weave
                        // off" tracking. Keep the debounce so the poll
                        // doesn't re-apply over the user's live tweaks.
                        app.activeFullscreenProfileHwnd = nullptr;
                    }
                    else if (IsWindowFullscreen(h))
                    {
                        // Turned ON while fullscreen: start tracking, so
                        // leaving fullscreen turns the weave off.
                        app.activeFullscreenProfileHwnd = h;
                    }
                    else
                    {
                        // Turned ON while windowed: the profile no longer
                        // applies here. Stop weaving that window and reset
                        // the debounce so going fullscreen re-applies it.
                        if (app.weavingEnabled &&
                            app.source == SourceKind::CaptureWindow &&
                            app.sourceWindow == h)
                            SetWeaving(app, false);
                        app.activeFullscreenProfileHwnd = nullptr;
                        app.lastAppliedHwnd = nullptr;
                        app.lastAutoAppliedProfile.clear();
                    }
                }
            }
            if (gs.profileToggleAutoFormatIndex >= 0 &&
                (size_t)gs.profileToggleAutoFormatIndex < app.profiles.size())
            {
                Profile& p = app.profiles[(size_t)gs.profileToggleAutoFormatIndex];
                p.useAutoFormat = !p.useAutoFormat;
                // When flipping auto-format ON, seed defaultFormat with
                // the current saved format so "detection fails" still
                // yields something sensible (rather than the enum's
                // default HalfSBS regardless of what the user had set).
                // Flipping it OFF restores the fixed format from that
                // fallback (which is what the file actually persists
                // while Auto is on).
                if (p.useAutoFormat) p.defaultFormat = p.format;
                else                 p.format = p.defaultFormat;
                Profiles::Save(app.profiles);
            }
            if (gs.profilesOpenIni)
            {
                Profiles::Save(app.profiles);
                PWSTR base = nullptr;
                if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)) && base)
                {
                    std::wstring p = base; CoTaskMemFree(base);
                    p += L"\\SRLoom\\profiles.ini";
                    ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
        }
    }
    if (frame)
    {
        // After all GUI / WM_COMMAND state changes have settled,
        // check if the currently-loaded image / video has had its
        // stereo settings tweaked -- if so, save the new state back
        // to its media profile. Throttled internally to avoid INI
        // hammering on slider drags.
        AutoSaveMediaProfileIfDirty(app);
        // Fullscreen-condition profile state tick (throttled to
        // 250ms internally). Handles enter/exit fullscreen when the
        // foreground didn't change -- WinEventHook doesn't cover
        // that case.
        PollProfileFullscreenState(app);
        PollKatangaAutoReceive(app);   // throttled to 500ms internally
        // Late Latching switch (panel): apply to the running weaver.
        {
            static DWORD s_last = 0;
            if (GetTickCount() - s_last > 500)
            {
                s_last = GetTickCount();
                const int want = Settings::ReadLateLatching() ? 1 : 0;
                if (app.weavingEnabled && want != app.lateLatchingApplied)
                {
                    app.weaver.SetLateLatching(want != 0);
                    app.lateLatchingApplied = want;   // (once, even if the runtime ignores it)
                }
                if (!app.weavingEnabled) app.lateLatchingApplied = -1;   // re-apply when it restarts
                // The SR display's refresh (current mode), and display
                // settings changes once they've settled (Windows sends a few).
                PaceForSrRefresh(app);
                if (app.displayChangedAtMs && GetTickCount64() - app.displayChangedAtMs > 1000)
                {
                    app.displayChangedAtMs = 0;
                    HandleDisplayChange(app);
                    app.hdrDisplay = app.renderer.DisplayIsHdr();   // (Windows' HDR switch is a display change too)
                }
                // Weaving has been off for a while: let the SR session go
                // (camera off). Kept until now so switching back is instant.
                if (app.weavingEnabled) app.srStopAtMs = 0;
                else if (app.srStopAtMs && GetTickCount64() >= app.srStopAtMs)
                {
                    app.srStopAtMs = 0;
                    app.weaver.StopSR();
                    Log("SR session released (weaving off for %llu s)", kSrKeepAliveMs / 1000);
                }
                // Weaver choice (panel): a different weaver needs a new SR session.
                const int wantWeaver = Settings::ReadWeaverChoice();
                if (app.weaverChoiceSeen < 0) app.weaverChoiceSeen = wantWeaver;
                // ... and so does HDR going on or off (Windows' switch, or a layout
                // the float chain is / isn't for: HdrWanted).
                else if (wantWeaver != app.weaverChoiceSeen || HdrWanted(app, app.hdrDisplay) != app.hdrActive ||
                         (app.weaver.HasWeaver() && LensOnlyWanted(app) != app.weaver.IsLensOnly()))   // (... and the light field's lens-only session)
                {
                    app.weaverChoiceSeen = wantWeaver;
                    if (app.weaver.HasWeaver())
                    {
                        Log("Weaver choice %d, HDR chain %s: restarting the SR session", wantWeaver, HdrWanted(app, app.hdrDisplay) ? "on" : "off");
                        app.weaver.StopSR();
                        app.srStopAtMs = 0;
                        if (app.weavingEnabled)
                        {
                            StartSRSession(app);
                            const bool katangaArmed = (app.format == StereoFormat::Katanga && !app.katanga.IsReceiving());
                            if (katangaArmed) app.weaver.LensDisable(); else app.weaver.LensEnable();
                            app.lateLatchingApplied = -1;
                            app.captureRebind = true;   // (the new weaver needs its input)
                        }
                    }
                }
                // Anti-crosstalk (panel): applied when it changes, and to a new weaver.
                {
                    const int act = Settings::ReadWeaverAct(), pct = Settings::ReadWeaverActStrength();
                    const int key = app.weaver.HasWeaver() ? (act << 16 | pct) + 1 + (app.weaver.WeaverChoice() << 24) : 0;
                    if (key != app.actApplied)
                    {
                        app.actApplied = key;
                        if (key && (act != 0 || pct != 100 || app.actTouched)) { app.weaver.ApplyAct(act, pct); app.actTouched = true; }
                    }
                }
                app.lfOn = Settings::ReadLightField();
                app.lfPitch = Settings::ReadLfPitch();
                app.lfSlantSet = Settings::ReadLfSlant(app.lfSlant);
                app.lfOffset = Settings::ReadLfOffset();
                app.lfFollow = Settings::ReadLfFollow();
                app.lfCentre = false;   // (Centre On Me was taken out again: the camera gives the distance only)
                app.rgbdStrength = (float)Settings::ReadRgbd(0); app.rgbdFocus = (float)Settings::ReadRgbd(1);
                app.rgbdFlags = Settings::ReadRgbd(2); app.rgbdLook = Settings::ReadRgbd(3) != 0;
                app.lfSpread = Settings::ReadLfSpread() / 100.0f;
                app.lfDistanceCm = Settings::ReadLfDistance();
                app.lfPattern = Settings::ReadLfPattern();
                app.perfLog = Settings::ReadPerfLog();
                app.renderer.SetLatencyStats(app.perfLog);
                app.eyeOrderDetect = Settings::ReadEyeOrderDetect();
                const bool skip = Settings::ReadDiagSkipWeave();
                if (skip != app.diagSkipWeave)
                {
                    app.diagSkipWeave = skip;
                    Log("DIAG: SR weave call %s", skip ? "SKIPPED (diagnostic)" : "restored");
                }
            }
        }
        // Taskbar cut-out, every frame: when a window goes borderless-
        // fullscreen (F11) the taskbar vanishes behind it, and a stale
        // hole would show that window mid-resize (a white/grey box) for
        // up to a poll interval. Cheap when nothing changed (a few
        // window-rect reads; SetWindowRgn only on change).
        UpdateTaskbarCutout(app, false);
        // Fullscreen apps popping above the weave (throttled to 250ms --
        // walks the z-order above us).
        {
            static DWORD s_lastWeaveZCheckMs = 0;
            const DWORD nowMs = GetTickCount();
            if (nowMs - s_lastWeaveZCheckMs >= 250)
            {
                s_lastWeaveZCheckMs = nowMs;
                KeepWeaveAboveFullscreenApps(app);
            }
        }
    }
}

// Top-level structured-exception handler: writes the exception code and the
// faulting module name + offset to srweaver.log before the process dies.
// The offset (RVA = addr - module base) is what matters: ASLR moves the exe
// every launch, so a bare address from a user's log can't be mapped back to
// a function, but base+RVA resolves against the release PDB / .map file.
// Returns EXCEPTION_CONTINUE_SEARCH so the default Windows error reporter +
// crash dump generation still run.
static LONG WINAPI SrLoomCrashHandler(EXCEPTION_POINTERS* ep)
{
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    void* const addr = ep->ExceptionRecord->ExceptionAddress;
    char modName[MAX_PATH] = "?";
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &mod) && mod)
        GetModuleFileNameA(mod, modName, MAX_PATH);
    const unsigned long long rva = mod
        ? (unsigned long long)((const char*)addr - (const char*)mod) : 0ull;
    Log("CRASH: code=0x%08X at addr=%p (module: %s +0x%llX)",
        (unsigned)code, addr, modName, rva);
    LogFlush();   // (the writer thread may not get to it before the process dies)
    return EXCEPTION_CONTINUE_SEARCH;
}

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int)
{
    // Install the crash handler first so any later init failure that
    // segfaults / access-violates leaves a useful trail in srweaver.log.
    SetUnhandledExceptionFilter(SrLoomCrashHandler);
    // No IME / Text Services on SR Loom's threads: its hooks run inside every
    // PeekMessage and talk to other processes, a known source of message-pump
    // stalls in games. (Must come before any window is created.)
    ImmDisableIME((DWORD)-1);
    TscPerMs();   // (starts the CPU-time calibration for the hitch log)
    Log("WinMain: SR Loom v%s starting (pid %lu)", kAppVersion, GetCurrentProcessId());

    // Single instance. Two SR Loom processes fight over the SR display /
    // weaver / hotkeys / tray. A second launch just opens the running
    // copy's panel and exits. (The mutex is released by the OS on exit.)
    static HANDLE s_instanceMutex = CreateMutexA(nullptr, FALSE, "Local\\SRLoomSingleInstance");
    const DWORD instanceGle = GetLastError();
    if (s_instanceMutex && instanceGle == ERROR_ALREADY_EXISTS)
    {
        Log("WinMain: another SR Loom is already running -- opening its panel and exiting");
        if (HWND other = FindWindowA(kWindowClass, nullptr))
            PostMessageA(other, WM_COMMAND, MAKEWPARAM(ID_TRAY_OPEN_PANEL, 0), 0);
        return 0;
    }
    {
        const char* sr = SRWeaver::GetSRPlatformVersion();
        Log("WinMain: SR Platform runtime version=%s", (sr && *sr) ? sr : "(unknown)");
    }

    // Per-Monitor-Aware v2: unlike v1, Windows auto-scales the NON-CLIENT area
    // (title bar) per monitor — so the GUI's title bar no longer stays huge when
    // dragged from a 4K/150% display to a 1440p/100% one. Falls back to v1.
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);

    // Media Foundation: needed to decode quilt video files (mp4/mov/etc.).
    VideoSource::Startup();

    // Testing aid: "-noexclude" leaves the window visible to screen capture.
    const bool excludeFromCapture = !(lpCmdLine && strstr(lpCmdLine, "-noexclude"));

    AppState app;
    g_app = &app;

    // Connect to the SR service to ask where the SR display is -- with the
    // lens preference OFF, so the lenses don't switch on for a moment at
    // start-up (the session is closed again before the main loop; weaving
    // opens its own).
    if (!app.weaver.CreateContext(10.0, false))
    {
        ShowError("Could not connect to the Simulated Reality service.\n"
                  "Make sure the SR Platform runtime is installed and running.");
        return 1;
    }
    // Decide where the output window lives (the SR display if available).
    app.srDisplayRect = ResolveTargetRect(app.weaver);

    // Diagnostic snapshot: what does the runtime report for the SR display,
    // and is it Windows' primary monitor? Lets us verify whether SR Loom
    // works on a non-primary SR display setup (the runtime + getLocation()
    // pattern should support it on Windows 11; see docs/sr-non-primary-research.md).
    {
        const RECT& d = app.srDisplayRect;
        const POINT p{ (d.left + d.right) / 2, (d.top + d.bottom) / 2 };
        HMONITOR mon = MonitorFromPoint(p, MONITOR_DEFAULTTONULL);
        MONITORINFOEXA mi{}; mi.cbSize = sizeof(mi);
        const bool gotMi = mon && GetMonitorInfoA(mon, &mi);
        const bool isPrimary = gotMi && (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        Log("WinMain: SR display rect=(%ld,%ld %ld,%ld) %ldx%ld",
            d.left, d.top, d.right, d.bottom,
            d.right - d.left, d.bottom - d.top);
        Log("WinMain: SR display monitor=%p name='%s' primary=%d",
            (void*)mon, gotMi ? mi.szDevice : "(unknown)", isPrimary ? 1 : 0);
        if (!isPrimary)
            Log("WinMain: SR display is NOT Windows primary -- testing non-primary support."
                " If weave doesn't engage, see docs/sr-non-primary-research.md.");
    }

    // Register window class.
    WNDCLASSEXA wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hIcon         = (HICON)LoadImageA(hInstance, MAKEINTRESOURCEA(IDI_TRAY),
                                         IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExA(&wc))
    {
        ShowError("Failed to register window class.");
        return 2;
    }

    // Create the output window on the SR display.
    // DirectComposition presenter (Settings::ReadDirectComposition): the
    // window must be created without a redirection bitmap for it.
    const RECT& d = app.srDisplayRect;
    bool useDComp = Settings::ReadDirectComposition();
    app.hwnd = CreateWindowExA(
        useDComp ? WS_EX_NOREDIRECTIONBITMAP : 0, kWindowClass, kWindowTitle, WS_POPUP,
        d.left, d.top, d.right - d.left, d.bottom - d.top,
        nullptr, nullptr, hInstance, nullptr);
    if (!app.hwnd)
    {
        ShowError("Failed to create output window.");
        return 3;
    }

    // Exclude our own window from screen capture so passthrough / monitor weaving
    // doesn't recursively capture its own output (no feedback loop).
    if (excludeFromCapture)
        SetWindowDisplayAffinity(app.hwnd, WDA_EXCLUDEFROMCAPTURE);

    // No DWM show/hide/resize animations on the weave window. Mode switches
    // (Looking Glass <-> Fullscreen) restyle + resize it, and the animation
    // showed as a white/blue window outline sliding to the new rect.
    // 3 = DWMWA_TRANSITIONS_FORCEDISABLED.
    {
        const BOOL noTransitions = TRUE;
        DwmSetWindowAttribute(app.hwnd, 3, &noTransitions, sizeof(noTransitions));
    }

    // Set up Direct3D. The weaver/SR session is started on demand when weaving is
    // enabled; the default source is the monitor (passthrough), so no initial image.
    app.renderer.SetPlaneMode(Settings::ReadWeavePlane());   // (an experiment: see Settings.h)
    app.renderer.SetAutoPlane(Settings::ReadAutoPlane());
    if (!app.renderer.Initialize(app.hwnd, useDComp))
    {
        if (!useDComp) return 4;
        // DirectComposition unavailable: a classic window + swap chains.
        Log("WinMain: DirectComposition presenter failed -- using the classic presenter");
        useDComp = false;
        app.renderer.Shutdown();
        HWND failed = app.hwnd;
        app.hwnd = nullptr;          // (WM_DESTROY: not our window any more -- don't quit)
        DestroyWindow(failed);
        app.hwnd = CreateWindowExA(0, kWindowClass, kWindowTitle, WS_POPUP,
                                   d.left, d.top, d.right - d.left, d.bottom - d.top,
                                   nullptr, nullptr, hInstance, nullptr);
        if (!app.hwnd) { ShowError("Failed to create output window."); return 3; }
        if (excludeFromCapture) SetWindowDisplayAffinity(app.hwnd, WDA_EXCLUDEFROMCAPTURE);
        const BOOL noTransitions = TRUE;
        DwmSetWindowAttribute(app.hwnd, 3, &noTransitions, sizeof(noTransitions));
        if (!app.renderer.Initialize(app.hwnd)) return 4;
    }
    Log("WinMain: presenter = %s", app.renderer.IsDComp() ? "DirectComposition" : "classic swap chains");
    if (!app.gpuTimer.Init(app.renderer.Device(), app.renderer.Context()))
        Log("WinMain: GPU frame timer unavailable");

    // Cap the render loop at the SR display's max supported refresh. Without this
    // we'd render past the panel's refresh on fast sources, wasting GPU / heat
    // for frames the panel can't show. Auto-scales per panel: Samsung Odyssey 3D
    // ~165 Hz, Acer SpatialLabs View Pro 27 ~160 Hz, future panels higher.
    if (const double maxHz = MaxMonitorRefreshHz(SrMonitor(app)); maxHz > 0.0)
    {
        app.renderer.SetTargetRefreshHz(maxHz);
        Log("WinMain: render cap set to %.1f Hz (SR display max)", maxHz);
    }
    else
    {
        Log("WinMain: SR display refresh unknown, render rate uncapped");
    }
    // SR display physical size in mm/px, needed for the quilt's window-position
    // perspective shift (windowed mode "looks toward" the window's screen
    // position relative to the head). Zero if Windows can't report it.
    app.srMmPerPx = MonitorMmPerPx(SrMonitor(app));
    Log("WinMain: SR display %.4f mm/px", app.srMmPerPx);

    // Initialize live capture (this is the default source).
    const bool capInit = app.capture.Initialize(app.renderer.Device(), app.renderer.Context());
    Log("WinMain: capture.Initialize=%d", capInit ? 1 : 0);
    app.capture.SetZeroCopy(Settings::ReadZeroCopyCapture());
    Log("WinMain: zero-copy capture %s", Settings::ReadZeroCopyCapture() ? "on" : "off");
    // DXGI Output Duplication is the fallback for exclusive-fullscreen capture; it
    // sits idle until the render loop detects WGC isn't delivering frames for a
    // foreign-display monitor source.
    const bool dxgiInit = app.captureDxgi.Initialize(app.renderer.Device(), app.renderer.Context());
    Log("WinMain: captureDxgi.Initialize=%d", dxgiInit ? 1 : 0);

    // Initialize the format-conversion stage (capture/image -> SBS for the weaver).
    if (!app.converter.Initialize(app.renderer.Device(), app.renderer.Context()))
    {
        Log("WinMain: converter.Initialize FAILED (exit 7)");
        return 7;
    }
    Log("WinMain: converter.Initialize OK");
    app.converter.SetScrollReuse(Settings::ReadScrollReuse());
    app.deferHeavyConvert = Settings::ReadDeferRecovered();
    app.asyncConvert = Settings::ReadAsyncConvert();
    app.weaverLatencyAuto = Settings::ReadWeaverLatency() == 1;
    app.asyncWaitMs = Settings::ReadAsyncWaitUs() / 1000.0;
    app.asyncBands = Settings::ReadAsyncBands();
    app.asyncEnterMs = Settings::ReadAsyncEnterUs() / 1000.0;
    Settings::ReadAnaCustom(app.anaCustomL, app.anaCustomR);
    app.anaSavedCount = Settings::ReadAnaSaved(app.anaSaved, 8);
    Log("WinMain: Recovered Colour converted %s", app.deferHeavyConvert ? "after the present (woven next refresh)" : "before the weave");
    Log("WinMain: scroll reuse %s", Settings::ReadScrollReuse() ? "on" : "off");
    if (!app.regionWeaver.Initialize(app.renderer.Device(), app.renderer.Context()))
        Log("WinMain: regionWeaver.Initialize FAILED (Auto Stereo unavailable)");
    if (!app.analyzer.Initialize(app.renderer.Device(), app.renderer.Context()))
        Log("WinMain: analyzer.Initialize FAILED (Auto Stereo image picking unavailable)");
    // GPU scroll tracking needs the DirectComposition presenter (its see-through
    // cut-out reads the GPU result in the same frame).
    app.gpuTracking = app.renderer.IsDComp() &&
                      app.gpuTracker.Initialize(app.renderer.Device(), app.renderer.Context());
    Log("WinMain: GPU scroll tracking %s", app.gpuTracking ? "on" : "off");
    // LFPRenderer is optional -- only used when an LFP file is loaded.
    // If shader compile fails, log and continue without the feature.
    if (!app.lfpRenderer.Initialize(app.renderer.Device(), app.renderer.Context()))
        Log("WinMain: lfpRenderer.Initialize FAILED (LFP files will fall back to CPU SBS)");
    else
        Log("WinMain: lfpRenderer.Initialize OK");
    // Wrap detector / gui / tray init in begin/end + try/catch -- one of these
    // dying silently between converter.Initialize OK and tray.Add was the
    // failure mode reported by the first user, and "begin" / "end" pairs plus
    // exception messages make it obvious which call is to blame.
    Log("WinMain: detector.Initialize() begin");
    try {
        app.detector.Initialize(app.renderer.Device(), app.renderer.Context());  // optional
    } catch (std::exception& e) {
        Log("WinMain: detector.Initialize() std::exception: %s", e.what());
    } catch (...) {
        Log("WinMain: detector.Initialize() unknown exception");
    }
    Log("WinMain: detector.Initialize done");
    app.captureRebind = true;   // bind the weaver to the converter output on the first frame

    // Entire post-detector block is wrapped in try/catch so a failure anywhere
    // here (Win32 registry, FS, SR context creation) degrades head tracking
    // instead of taking the whole app down. Multiple bug reports landed
    // (issue #1, IsPepsiOk's report) with Samsung Odyssey + Win10 22H2 silently
    // exiting somewhere in this window -- without the catch the process dies
    // before reaching the tray, leaving the user with no UI surface at all.
    try
    {
    Log("WinMain: head-tracking bootstrap begin");

    // Best-effort detection of opentrack.exe so the GUI's "Open OpenTrack"
    // button can launch it directly. Looks in common install locations;
    // empty string if not found (button just hidden).
    {
        const char* paths[] = {
            "C:\\Program Files\\opentrack\\opentrack.exe",
            "C:\\Program Files (x86)\\opentrack\\opentrack.exe",
        };
        for (const char* p : paths)
        {
            DWORD attr = GetFileAttributesA(p);
            if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
            {
                app.openTrackExePath = p;
                Log("OpenTrack: detected at %s", p);
                break;
            }
        }
    }

    // TrackIR-via-NPClient bootstrap. Games speak TrackIR by loading
    // NPClient(64).dll, whose location they read from
    // HKLM\Software\NaturalPoint\NATURALPOINT\NPClient Location.
    //
    // We extract our embedded NPClient64.dll (built from OpenTrack's
    // contrib/npclient source, MIT) to %LOCALAPPDATA%\SRLoom\NPClient\
    // and write that path to the registry -- BUT only if the registry
    // either is unset or points at a path with no DLL on disk. If
    // OpenTrack (or any other NPClient publisher) already owns the key
    // and its DLL exists, we leave that pointer alone. OpenTrack's
    // NPClient and ours both read the same FT_SharedMem we write, so
    // games get pose either way. This is the "graceful coexistence"
    // mode the user asked for: we step in only when no one else has.
    auto npClientFileExists = [](const std::string& p) {
        DWORD a = GetFileAttributesA(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    };
    auto readNpDir = [](HKEY root, const char* sub) -> std::string {
        HKEY k = nullptr;
        if (RegOpenKeyExA(root, sub, 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS)
            return {};
        char buf[MAX_PATH]; DWORD sz = sizeof(buf); DWORD type = 0;
        std::string out;
        if (RegQueryValueExA(k, "Path", nullptr, &type, (BYTE*)buf, &sz) == ERROR_SUCCESS
            && type == REG_SZ && sz > 0)
        {
            out.assign(buf, sz - 1);
            while (!out.empty() && out.back() == '\0') out.pop_back();
        }
        RegCloseKey(k);
        return out;
    };
    {
        // Step 1: extract NPClient64.dll from the embedded resource to
        // %LOCALAPPDATA%\SRLoom\NPClient\. copy_if_different semantics
        // via byte comparison -- only write when the existing file
        // doesn't match the embedded version (typical: app upgrade).
        size_t embedSize = 0;
        const void* embedData = LookupEmbeddedResource(IDR_NPCLIENT64, embedSize);
        std::string ourDir;
        std::string ourDll;
        if (embedData && embedSize > 0)
        {
            PWSTR base = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)) && base)
            {
                std::wstring wdir = base;
                CoTaskMemFree(base);
                wdir += L"\\SRLoom\\NPClient";
                SHCreateDirectoryExW(nullptr, wdir.c_str(), nullptr);
                // Convert to narrow for the registry write below. NPClient
                // location is ANSI-only (Windows convention).
                int n = WideCharToMultiByte(CP_ACP, 0, wdir.c_str(), -1, nullptr, 0, nullptr, nullptr);
                if (n > 0)
                {
                    ourDir.resize(n - 1);
                    WideCharToMultiByte(CP_ACP, 0, wdir.c_str(), -1, ourDir.data(), n, nullptr, nullptr);
                }
                ourDll = ourDir + "\\NPClient64.dll";

                bool needWrite = true;
                HANDLE h = CreateFileA(ourDll.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE)
                {
                    LARGE_INTEGER sz{}; GetFileSizeEx(h, &sz);
                    if ((uint64_t)sz.QuadPart == (uint64_t)embedSize)
                    {
                        // Compare bytes; small file (~100KB), cheap.
                        std::vector<uint8_t> onDisk(embedSize);
                        DWORD read = 0;
                        if (ReadFile(h, onDisk.data(), (DWORD)embedSize, &read, nullptr)
                            && read == embedSize
                            && memcmp(onDisk.data(), embedData, embedSize) == 0)
                        {
                            needWrite = false;
                        }
                    }
                    CloseHandle(h);
                }
                if (needWrite)
                {
                    HANDLE w = CreateFileA(ourDll.c_str(), GENERIC_WRITE, 0, nullptr,
                                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (w != INVALID_HANDLE_VALUE)
                    {
                        DWORD wrote = 0;
                        WriteFile(w, embedData, (DWORD)embedSize, &wrote, nullptr);
                        CloseHandle(w);
                        Log("TrackIR: extracted NPClient64.dll (%zu bytes) to %s", embedSize, ourDir.c_str());
                    }
                    else
                    {
                        Log("TrackIR: failed to write %s (err=%lu)", ourDll.c_str(), (unsigned long)GetLastError());
                        ourDll.clear();
                    }
                }
            }
        }

        // Step 2: decide whether to take over the NaturalPoint registry
        // key. Defer to an existing valid path (typically OpenTrack's);
        // claim it ourselves only when no functional pointer is set.
        std::string existingDir = readNpDir(HKEY_LOCAL_MACHINE,
                                            "Software\\NaturalPoint\\NATURALPOINT\\NPClient Location");
        if (existingDir.empty())
            existingDir = readNpDir(HKEY_LOCAL_MACHINE,
                                    "Software\\WOW6432Node\\NaturalPoint\\NATURALPOINT\\NPClient Location");
        bool existingWorks = false;
        if (!existingDir.empty())
        {
            while (!existingDir.empty() && (existingDir.back() == '\\' || existingDir.back() == '/'))
                existingDir.pop_back();
            const bool has32 = npClientFileExists(existingDir + "\\NPClient.dll");
            const bool has64 = npClientFileExists(existingDir + "\\NPClient64.dll");
            existingWorks = has32 || has64;
        }

        if (existingWorks)
        {
            app.npClientDir = existingDir;
            Log("TrackIR: deferring to existing NPClient at %s", existingDir.c_str());
        }
        else if (!ourDll.empty())
        {
            // Write to HKCU rather than HKLM -- HKCU writes don't require
            // admin (writing to HKLM\Software would silently fail on
            // standard user accounts). Games read HKCU after HKLM via the
            // standard registry override, so this works for the typical
            // non-elevated SR Loom user. Both bitness views written so
            // 32-bit games see the same path the 64-bit games do.
            auto writeKey = [](HKEY root, const char* sub, REGSAM extra, const std::string& path) -> bool {
                HKEY k = nullptr;
                LONG r = RegCreateKeyExA(root, sub, 0, nullptr, 0,
                                         KEY_WRITE | extra, nullptr, &k, nullptr);
                if (r != ERROR_SUCCESS) return false;
                r = RegSetValueExA(k, "Path", 0, REG_SZ,
                                   (const BYTE*)path.c_str(), (DWORD)path.size() + 1);
                RegCloseKey(k);
                return r == ERROR_SUCCESS;
            };
            const bool a = writeKey(HKEY_CURRENT_USER,
                                    "Software\\NaturalPoint\\NATURALPOINT\\NPClient Location",
                                    KEY_WOW64_64KEY, ourDir);
            const bool b = writeKey(HKEY_CURRENT_USER,
                                    "Software\\NaturalPoint\\NATURALPOINT\\NPClient Location",
                                    KEY_WOW64_32KEY, ourDir);
            if (a || b)
            {
                app.npClientDir = ourDir;
                Log("TrackIR: registered our NPClient at %s (HKCU write %s/%s)",
                    ourDir.c_str(), a ? "ok" : "fail", b ? "ok" : "fail");
            }
        }
    }

    // OpenTrack UDP + FreeTrack 2.0 default ON at app launch so users don't
    // have to think about head tracking. TrackIR also defaults ON if (and
    // only if) the OpenTrack NPClient install was detected above -- the
    // toggle stays interactable either way, but auto-enabling it when the
    // prerequisites are met means users with OpenTrack installed get
    // TrackIR-aware games working out of the box.
    // Gated by Settings::ReadHeadTrackingOnStartup (default ON). Users who
    // don't use head tracking flip that off in STARTUP -- then no outputs
    // engage at boot and the SR camera stays cold. All three toggles remain
    // interactable in the HEADTRACKING section, so this only affects the
    // launch state.
    Log("WinMain: openTrack.SetOutputs (initial)");
    const bool tirAuto = !app.npClientDir.empty();
    const bool htAutoOn = Settings::ReadHeadTrackingOnStartup();
    if (htAutoOn)
    {
        if (!app.openTrack.SetOutputs(true, true, tirAuto))
            Log("OpenTrack/FreeTrack/TrackIR: initial enable failed (SR Platform service offline?)");
    }
    else
    {
        // openTrackUserDisabled ensures the "opportunistic re-enable on
        // weave-start" branch (SetWeaving) doesn't undo the user's choice
        // by silently turning outputs back on the first time they weave.
        app.openTrackUserDisabled = true;
        Log("Head tracking: startup-off (per Settings), outputs left disabled");
    }
    Log("WinMain: head-tracking bootstrap done");
    }
    catch (std::exception& e)
    {
        Log("WinMain: head-tracking bootstrap std::exception: %s -- continuing without tracking", e.what());
    }
    catch (...)
    {
        Log("WinMain: head-tracking bootstrap unknown exception -- continuing without tracking");
    }

    // Control GUI (left-click the tray icon). Shares the D3D11 device; lives in its
    // own top-level window so it can sit on any monitor with its own taskbar button.
    Log("WinMain: gui.Init() begin");
    bool guiOK = false;
    try {
        guiOK = app.gui.Init(app.hwnd, app.renderer.Device(), app.renderer.Context());
    } catch (std::exception& e) {
        Log("WinMain: gui.Init() std::exception: %s", e.what());
    } catch (...) {
        Log("WinMain: gui.Init() unknown exception");
    }
    Log("WinMain: gui.Init=%d", guiOK ? 1 : 0);
    // Honor the persisted "start in tray" preference. Default is true (tray-only,
    // matches the historical behaviour); flipping it off pops the control panel
    // on launch so users who prefer that workflow see it immediately.
    if (guiOK && !Settings::ReadStartInTray())
    {
        app.gui.Toggle();
        Log("WinMain: StartInTray=false -> control panel shown");
    }

    // Tray icon + global hotkeys.
    Log("WinMain: tray.Add() begin");
    bool trayOK = false;
    try {
        trayOK = app.tray.Add(app.hwnd, "SR Loom — paused (SR off)");
    } catch (std::exception& e) {
        Log("WinMain: tray.Add() std::exception: %s", e.what());
    } catch (...) {
        Log("WinMain: tray.Add() unknown exception");
    }
    Log("WinMain: tray.Add=%d", trayOK ? 1 : 0);

    // Create the floating overlay windows now (all stay hidden until the user
    // clicks inside a test-image weave).
    EnsureFsCtrlWindow(hInstance);
    EnsureFsSetWindow(hInstance);
    EnsureFsVidWindow(hInstance);

    // Accept dropped image files anywhere on the SR Loom main window -- the
    // WM_DROPFILES handler routes through LoadTestImage with the dropped path.
    DragAcceptFiles(app.hwnd, TRUE);
    const int hk1 = RegisterHotKey(app.hwnd, kHotkeyToggle,    MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'W');
    const int hk2 = RegisterHotKey(app.hwnd, kHotkeyMode,      MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'F');
    const int hk3 = RegisterHotKey(app.hwnd, kHotkeyCapture,   MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'C');
    const int hk4 = RegisterHotKey(app.hwnd, kHotkeyCalibrate, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'R');
    const int hk5 = RegisterHotKey(app.hwnd, kHotkeyAutoRegion, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'A');
    const int hk6 = RegisterHotKey(app.hwnd, kHotkeyAutoRegionDbg, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'A');
    Log("WinMain: hotkeys W=%d F=%d C=%d R=%d A=%d ShiftA=%d", hk1, hk2, hk3, hk4, hk5, hk6);

    // Profiles: load list + master enable. Install the foreground-watch
    // hook only if auto-apply is on -- global EVENT_SYSTEM_FOREGROUND
    // hooks have a known cost (every-process delivery thread + DWM
    // bookkeeping) and we don't need them when the feature is off. The
    // toggle handler installs / tears down the hook on the fly when the
    // user flips it.
    app.profiles          = Profiles::Load();
    app.profilesAutoApply = Settings::ReadAutoApplyProfiles();
    app.eyeOrderDetect = Settings::ReadEyeOrderDetect();
    // Stereo 3D Input: start on the pinned default (Side-by-Side (half)
    // unless another one was pinned -- Settings::ReadDefaultInput).
    {
        app.defaultInput = Settings::ReadDefaultInput();
        int n = 0;
        const StereoFormatEntry* fmts = StereoFormatList(n);
        if (app.defaultInput > 0 && app.defaultInput <= n)
        {
            app.autoInput = false;
            app.format = fmts[app.defaultInput - 1].fmt;
        }
        else
            app.defaultInput = 0;
        Log("WinMain: default input = %s", app.autoInput ? "Automatic Detection" : Profiles::FormatToString(app.format));
    }
    RefreshForegroundHook(app);
    Log("WinMain: profiles loaded=%zu autoApply=%d hook=%p",
        app.profiles.size(), (int)app.profilesAutoApply, (void*)app.fgHook);

    // Update notice: first show whatever the last successful check found (so
    // the GUI banner + tray item appear immediately, even when the throttle
    // skips the network), then kick off a background poll of GitHub Releases
    // (throttled to once an hour; also re-run whenever the panel is opened).
    // A newer tag posts WM_APP_UPDATE_RESULT; auto-checks are silent on
    // up-to-date / failure -- only the About popup's forced check reports those.
    if (UpdateChecker::CachedUpdate(app.pendingUpdateTag, app.pendingUpdateUrl))
        Log("UpdateChecker: cached update available -- %s", app.pendingUpdateTag.c_str());
    UpdateChecker::StartAsync(app.hwnd, WM_APP_UPDATE_RESULT, false);
    // RegisterHotKey(app.hwnd, kHotkeyDetect,  MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'D'); // auto-detect disabled

    // Start idle: release the SR session used to query the display rect so the
    // lens and camera stay off until the user enables weaving (Ctrl+Alt+W).
    app.weavingEnabled = false;
    Log("WinMain: StopSR start");
    app.weaver.StopSR();
    Log("WinMain: StopSR done");
    Log("WinMain: ready — idle in tray, entering main loop");
    g_winWatch.Start();   // background window list for the cut-outs (idle until needed)
    // 1 ms timer resolution for this process: waits with short timeouts
    // (the capture wait, frame pacing) otherwise round up to Windows' 15.6 ms
    // default tick -- which held the weave to ~64 frames/s on any display.
    // (Windows 10 2004+ applies this to SR Loom only, not system-wide.)
    timeBeginPeriod(1);

    // The thread that runs the render loop: ahead of normal-priority work (and
    // well ahead of the Auto Stereo scanner) so it isn't made to skip frames,
    // and registered with the Multimedia Class Scheduler as a game's render
    // thread: Windows then schedules it promptly even while a video decodes or
    // a browser scrolls -- a late wake-up is a missed refresh.
    auto setUpRenderThread = [] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        DWORD taskIndex = 0;
        if (HANDLE mm = AvSetMmThreadCharacteristicsW(L"Games", &taskIndex))
        {
            AvSetMmThreadPriority(mm, AVRT_PRIORITY_HIGH);
            Log("WinMain: render thread registered with MMCSS (Games, high)");
        }
        else Log("WinMain: MMCSS registration failed (%lu)", GetLastError());
        g_stall.Start();   // (logs what the render thread is blocked in when it stalls)
    };

    // The render loop on a thread of its own, this one keeping the windows
    // (see g_appLock) -- unless switched off (Settings RenderThread = 0).
    g_uiThreadId = GetCurrentThreadId();
    {
        WNDCLASSW uc{};
        uc.lpfnWndProc = UiCallWndProc; uc.hInstance = hInstance; uc.lpszClassName = L"SRLoomUiCall";
        RegisterClassW(&uc);
        g_uiCallWnd = CreateWindowExW(0, uc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInstance, nullptr);
    }
    const bool useRenderThread = Settings::ReadRenderThread() && g_uiCallWnd;
    Log("WinMain: render loop on %s", useRenderThread ? "its own thread" : "the window thread (RenderThread = 0)");
    std::atomic<bool> renderRun{ true };
    std::thread renderThread;
    if (useRenderThread)
    {
        g_threaded = true;
        renderThread = std::thread([&] {
            g_renderThreadId = GetCurrentThreadId();   // (before anything that could make a window call)
            setUpRenderThread();
            while (renderRun.load())
            {
                g_stall.Arm(app.weavingEnabled);
                g_appLock.lock(); ++t_appLockDepth;
                LoopBody(app, true, false);            // (lets go of the lock while it waits)
                --t_appLockDepth; g_appLock.unlock();
            }
        });
    }
    else setUpRenderThread();

    bool running = true;
    while (running)
    {
        MSG msg{};
        if (g_threaded)
        {
            // This thread only has the windows' messages and the panel to do:
            // sleep until a message arrives, or the open panel is due a redraw.
            MsgWaitForMultipleObjectsEx(0, nullptr, app.gui.IsVisible() ? 4 : 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        else
        {
            g_stall.Arm(app.weavingEnabled);
            g_stall.Mark("message pump");
        }
        {
            // (Each message is timed on its own: a slow one is logged with
            // its id and which window it was for -- see HitchWatch.)
            const auto tPump0 = std::chrono::steady_clock::now();
            double dispatched = 0.0, dispatchedCpu = 0.0;
            const double cpuPump0 = ThreadCpuMs();
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                if (msg.message == WM_QUIT) { running = false; break; }
                const auto tm0 = std::chrono::steady_clock::now();
                const double cpuMsg0 = ThreadCpuMs();
                TranslateMessage(&msg);
                DispatchMessage(&msg);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tm0).count();
                dispatched += ms;
                dispatchedCpu += ThreadCpuMs() - cpuMsg0;
                if (ms > 20.0)
                {
                    char cls[64] = "?";
                    if (msg.hwnd) GetClassNameA(msg.hwnd, cls, (int)sizeof(cls));
                    Log("Hitch: window message 0x%04X (wParam 0x%llX) for %s window \"%s\" took %.1f ms",
                        msg.message, (unsigned long long)msg.wParam,
                        msg.hwnd == g_app->hwnd ? "the weave" : (msg.hwnd ? "another" : "no"), cls, ms);
                }
            }
            // Time not in any dispatched message: messages SENT to our windows
            // (by Windows or other programs), handled inside PeekMessage.
            const double pump = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tPump0).count();
            if (!g_threaded && pump - dispatched > 20.0)   // (with a render thread, this thread's waits hold up no frame)
            {
                Log("Hitch: messages sent to SR Loom's windows took %.1f ms (running %.1f ms of it)",
                    pump - dispatched, ThreadCpuMs() - cpuPump0 - dispatchedCpu);
                // First time: list every window on this thread, so the log shows
                // whose they could be (ours, the tray's, or ones the SR runtime
                // made on this thread).
                static bool s_listed = false;
                if (!s_listed)
                {
                    s_listed = true;
                    EnumThreadWindows(GetCurrentThreadId(), [](HWND h, LPARAM) -> BOOL {
                        char cls[96] = {}, title[96] = {};
                        GetClassNameA(h, cls, (int)sizeof(cls));
                        GetWindowTextA(h, title, (int)sizeof(title));
                        Log("  render-thread window %p class \"%s\" title \"%s\"%s", (void*)h, cls, title,
                            IsWindowVisible(h) ? " (visible)" : "");
                        return TRUE;
                    }, 0);
                }
            }
        }
        if (running)
        {
            if (g_threaded) { AppLock lock; LoopBody(app, false, true); }   // (the panel; the render thread does the rest)
            else LoopBody(app, true, true);
        }
    }

    // The render thread ends first: everything below is one thread's again.
    // (It may be waiting on this thread for a window call: those are still
    // answered while it winds down.)
    if (renderThread.joinable())
    {
        renderRun = false;
        while (WaitForSingleObject(renderThread.native_handle(), 5) == WAIT_TIMEOUT)
        {
            MSG m;
            PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
        }
        renderThread.join();
        g_threaded = false;
    }

    // Shutdown flush: write any pending media-profile changes before the
    // app exits. Without this, edits made in the last quarter-second of
    // the session (the throttle window) would be lost.
    FlushMediaProfileIfDirty(app);

    UnregisterHotKey(app.hwnd, kHotkeyToggle);
    UnregisterHotKey(app.hwnd, kHotkeyMode);
    UnregisterHotKey(app.hwnd, kHotkeyCapture);
    UnregisterHotKey(app.hwnd, kHotkeyCalibrate);
    UnregisterHotKey(app.hwnd, kHotkeyAutoRegion);
    UnregisterHotKey(app.hwnd, kHotkeyAutoRegionDbg);
    if (app.fgHook) { UnhookWinEvent(app.fgHook); app.fgHook = nullptr; }
    // UnregisterHotKey(app.hwnd, kHotkeyDetect); // auto-detect disabled
    app.tray.Remove();
    app.gui.Shutdown();
    app.detector.Shutdown();
    app.video.Close();
    app.katanga.End();
    app.lfpRenderer.Shutdown();
    // Stop OpenTrack BEFORE the weaver shuts down its SRContext --
    // the bridge holds an SR::HeadPoseTracker pointing into that context.
    app.openTrack.Disable();
    app.converter.Shutdown();
    app.capture.Shutdown();
    app.weaver.Shutdown();
    app.renderer.Shutdown();
    VideoSource::Shutdown();
    g_stall.Stop();
    g_winWatch.Stop();
    g_app = nullptr;
    return 0;
}
