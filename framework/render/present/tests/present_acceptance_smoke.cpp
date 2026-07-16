// framework/render/present/tests/present_acceptance_smoke.cpp
// Acceptance smoke consumer for phyriad::render::present (FR-RENDER-1, §1.8 test 2).
//
// This is the ONE thing the stage44a probe cannot test: a real frame flowing
// THROUGH PresentSurface::submit() — the §7 VK/D3D11 shared-texture bridge +
// keyed mutex + CopyResource + Present, driven by the framework primitive rather
// than the probe's inline recipe. It is NOT a re-implementation of the probe:
//   - click-through, capture-exclusion, and the {style}×{wda} matrix → the probe
//     (G:\phyriad\build-stage44a\stage44a_present_probe.exe) is the Q7 harness.
//   - THIS consumer adds: create() a DcompCt+WDA PresentSurface, produce a known
//     BGRA frame in a SHARED (NT-handle + keyed-mutex) D3D11 texture, submit() it
//     N times, spawn PresentMon against THIS PID, print the PresentMode VERBATIM.
//
// Honesty (D-14): the PresentMode is printed exactly as PresentMon reports it for
// this process; no massaging. If PresentMon is absent or attributes nothing, that
// limitation is printed, not hidden. Every spawned process/window is torn down.
//
// Producer note: the smoke producer creates the source texture on the SAME D3D11
// device family as the presenter (one adapter), exercising the keyed-mutex import
// path. The cross-ADAPTER VK→D3D11 case (the real Ayama producer) uses the
// identical NT-handle import; that path is the consumer's responsibility to export
// (vkGetMemoryWin32HandleKHR) and is out of scope for this single-process smoke.

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WINVER
#  define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <phyriad/render/present/PresentSurface.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace pp = phyriad::render::present;

namespace {
template <class T> void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
const char* kPresentMon = "F:\\PresetMon\\PresentMon.exe";

double now_ms() {
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
}

// ── the producer: a known BGRA frame in a shared keyed-mutex texture ────────
struct Producer {
    ID3D11Device*        dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11Texture2D*     tex = nullptr;
    IDXGIKeyedMutex*     km  = nullptr;
    HANDLE               nt  = nullptr;
    uint32_t W = 0, H = 0;
    const char* err = nullptr;

    bool init(uint32_t w, uint32_t h) {
        W = w; H = h;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL fl;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx))) { err = "producer D3D11CreateDevice"; return false; }

        D3D11_TEXTURE2D_DESC td{};
        td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex))) { err = "producer CreateTexture2D(shared)"; return false; }

        // paint it a known solid colour via an RTV clear (green, premultiplied α=1).
        ID3D11RenderTargetView* rtv = nullptr;
        if (SUCCEEDED(dev->CreateRenderTargetView(tex, nullptr, &rtv)) && rtv) {
            const float c[4] = { 0.0f, 1.0f, 0.0f, 1.0f };  // BGRA → green
            ctx->ClearRenderTargetView(rtv, c);
            ctx->Flush();
            rel(rtv);
        }

        tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&km);
        IDXGIResource1* res1 = nullptr;
        tex->QueryInterface(__uuidof(IDXGIResource1), (void**)&res1);
        if (!res1) { err = "producer IDXGIResource1 QI"; return false; }
        HRESULT hr = res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &nt);
        rel(res1);
        if (FAILED(hr) || !nt) { err = "producer CreateSharedHandle"; return false; }
        return true;
    }
    void shut() {
        if (nt) { CloseHandle(nt); nt = nullptr; }
        rel(km); rel(tex); rel(ctx); rel(dev);
    }
};

// ── PresentMon: spawn against THIS PID, parse the dominant PresentMode verbatim ─
struct PmResult { bool spawned=false; std::string mode; int rows=0; double rate=0, p95=0; std::string note; };

int col_index(const std::string& header, const char* name) {
    std::vector<std::string> cols; std::string cur;
    for (char c : header) {
        if (c == ',') { cols.push_back(cur); cur.clear(); }
        else if (c != '\r' && c != '\n') cur.push_back(c);
    }
    cols.push_back(cur);
    for (size_t i = 0; i < cols.size(); ++i) {
        std::string a = cols[i], b = name;
        for (auto& ch : a) ch = (char)tolower((unsigned char)ch);
        for (auto& ch : b) ch = (char)tolower((unsigned char)ch);
        if (a == b) return (int)i;
    }
    return -1;
}
std::string field(const std::string& line, int idx) {
    if (idx < 0) return "";
    int cur = 0; size_t start = 0;
    for (size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == ',') {
            if (cur == idx) {
                std::string s = line.substr(start, i - start);
                while (!s.empty() && (s.back()=='\r' || s.back()=='\n' || s.back()==' ')) s.pop_back();
                while (!s.empty() && s.front()==' ') s.erase(s.begin());
                return s;
            }
            cur++; start = i + 1;
        }
    }
    return "";
}

PmResult run_presentmon(DWORD pid, double seconds, const char* csv) {
    PmResult r;
    if (GetFileAttributesA(kPresentMon) == INVALID_FILE_ATTRIBUTES) {
        r.note = std::string("PresentMon not found at ") + kPresentMon; return r; }
    DeleteFileA(csv);
    char cmd[1024];
    std::snprintf(cmd, sizeof(cmd),
        "\"%s\" --process_id %lu --output_file \"%s\" --timed %d --v2_metrics "
        "--no_console_stats --terminate_after_timed --stop_existing_session",
        kPresentMon, (unsigned long)pid, csv, (int)(seconds+0.5));
    STARTUPINFOA si{}; si.cb=sizeof(si); si.dwFlags=STARTF_USESHOWWINDOW; si.wShowWindow=SW_HIDE;
    PROCESS_INFORMATION pi{}; char cm[1024]; std::strncpy(cm,cmd,sizeof(cm)); cm[sizeof(cm)-1]=0;
    if (!CreateProcessA(nullptr,cm,nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)) {
        r.note = "CreateProcess(PresentMon) gle=" + std::to_string(GetLastError()); return r; }
    r.spawned = true; CloseHandle(pi.hThread);
    if (WaitForSingleObject(pi.hProcess,(DWORD)((seconds+8.0)*1000.0)) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess,1); r.note = "PresentMon did not exit (killed)"; }
    CloseHandle(pi.hProcess);

    FILE* f = std::fopen(csv,"rb");
    if (!f) { r.note += " | no CSV produced"; return r; }
    std::string header; std::vector<std::string> lines; char buf[4096];
    while (std::fgets(buf,sizeof(buf),f)) { std::string ln=buf; if(header.empty())header=ln; else lines.push_back(ln); }
    std::fclose(f);
    const int ci_mode = col_index(header,"PresentMode");
    const int ci_ft   = (col_index(header,"MsBetweenPresents")>=0)?col_index(header,"MsBetweenPresents"):col_index(header,"FrameTime");
    std::vector<std::string> modes; std::vector<double> fts;
    for (auto& ln : lines) { if(ci_mode>=0){ auto mv=field(ln,ci_mode); if(!mv.empty())modes.push_back(mv);}
        if(ci_ft>=0){ auto fv=field(ln,ci_ft); if(!fv.empty()){ char*e=nullptr; double d=strtod(fv.c_str(),&e); if(e!=fv.c_str())fts.push_back(d);} } }
    r.rows=(int)lines.size();
    { std::vector<std::pair<std::string,int>> t; for(auto&m:modes){bool fnd=false; for(auto&x:t)if(x.first==m){x.second++;fnd=true;break;} if(!fnd)t.push_back({m,1});}
      int best=-1; for(auto&x:t)if(x.second>best){best=x.second;r.mode=x.first;} }
    if (!fts.empty()) { std::sort(fts.begin(),fts.end()); double s=0; for(double d:fts)s+=d; double mean=s/fts.size();
        r.rate = mean>0?1000.0/mean:0.0; r.p95 = fts[(size_t)std::min(fts.size()-1,(size_t)(fts.size()*0.95))]; }
    if (r.mode.empty()) r.note += " | no attributable PresentMode rows for our PID";
    return r;
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int monitor = 0; double seconds = 5.0; uint32_t w = 1280, h = 720;
    for (int i=1;i<argc;++i){ std::string a=argv[i];
        auto nx=[&](const char* d){ return (i+1<argc)?argv[++i]:d; };
        if (a=="--monitor") monitor=atoi(nx("0"));
        else if (a=="--seconds") seconds=atof(nx("5"));
        else if (a=="--size") { w=(uint32_t)atoi(nx("1280")); h=(uint32_t)atoi(nx("720")); }
        else if (a=="--help") { std::printf("usage: present_acceptance_smoke [--monitor N] [--seconds S] [--size W H]\n"); return 0; }
    }

    std::printf("================================================================================\n");
    std::printf("[present_acceptance_smoke] phyriad::render::present — frame-through-submit() harness\n");
    std::printf("  the §1.8 test-2 path the stage44a probe cannot test: a real frame flowing\n");
    std::printf("  through PresentSurface::submit() (shared keyed-mutex texture + CopyResource + Present).\n");
    std::printf("  PresentMon: %s   monitor=%d size=%ux%u seconds=%.0f\n", kPresentMon, monitor, w, h, seconds);
    std::printf("================================================================================\n\n");

    // ── producer: a known shared frame ──────────────────────────────────────
    Producer prod;
    if (!prod.init(w, h)) { std::printf("PRODUCER ERROR: %s\n", prod.err); return 2; }
    std::printf("producer: shared keyed-mutex BGRA texture %ux%u, NT handle=%p\n", w, h, (void*)prod.nt);

    // ── create the framework surface (the measured default config) ──────────
    pp::PresentSurfaceDesc desc{};
    desc.monitor_index = monitor;
    desc.width = w; desc.height = h;        // windowed overlay (not full extent) so it is a smoke, not a takeover
    // defaults: Style::DcompCt + ExcludeFromCapture + Immediate.
    auto sr = pp::PresentSurface::create(desc);
    if (!sr.has_value()) {
        std::printf("PresentSurface::create FAILED: ErrorCode=%u\n", (unsigned)sr.error().code);
        prod.shut(); return 3;
    }
    pp::PresentSurface surface = std::move(*sr);
    std::printf("PresentSurface::create OK | is_click_through=%s capture_excluded=%s\n",
                surface.is_click_through() ? "YES" : "NO",
                surface.capture_excluded() ? "YES" : "NO");

    pp::SharedFrameHandle frame{};
    frame.nt_handle = prod.nt;
    frame.keyed_mutex_key = 0;     // both sides use key 0 (single-process smoke)
    frame.width = w; frame.height = h;

    // ── present loop on THIS thread — the create() thread. The submit()
    //    threading contract: the window's message queue lives on the creating
    //    thread, so the ghosting pump inside submit() only drains from here
    //    (a helper-thread pumper drains nothing — the bug the stage44a probe
    //    had). PresentMon's blocking wait moves to a worker instead.
    std::atomic<uint64_t> ok_submits{0}, err_submits{0};
    phyriad::ErrorCode last_err = phyriad::ErrorCode::None;

    std::atomic<bool> pm_done{false};
    PmResult pm;
    char csv[260]; std::snprintf(csv,sizeof(csv),"present_smoke_pm.csv");
    std::thread pm_thr([&]{
        Sleep(300);  // a few frames before measuring
        pm = run_presentmon(GetCurrentProcessId(), seconds, csv);
        pm_done.store(true);
    });

    {
        const double period = 1000.0 / 240.0;
        double next = now_ms();
        while (!pm_done.load()) {
            auto r = surface.submit(frame);
            if (r.has_value()) ok_submits.fetch_add(1);
            else { err_submits.fetch_add(1); last_err = r.error().code; }
            next += period;
            for (;;) { double rem = next - now_ms(); if (rem <= 0) break;
                if (rem > 2.0) Sleep((DWORD)(rem-1.5)); else std::this_thread::yield(); }
            if (now_ms() > next + 8.0*period) next = now_ms();
        }
    }
    pm_thr.join();
    DeleteFileA(csv);

    // ── report (PresentMode VERBATIM) ───────────────────────────────────────
    std::printf("\n--------------------------------------------------------------------------------\n");
    std::printf("RESULT (PresentMode VERBATIM from PresentMon for THIS PID):\n");
    std::printf("  submits: ok=%llu err=%llu (last_err ErrorCode=%u)\n",
                (unsigned long long)ok_submits.load(), (unsigned long long)err_submits.load(),
                (unsigned)last_err);
    std::printf("  is_click_through = %s\n", surface.is_click_through() ? "YES" : "NO");
    std::printf("  capture_excluded = %s\n", surface.capture_excluded() ? "YES" : "NO");
    if (pm.spawned)
        std::printf("  PresentMode      = \"%s\"  (rows=%d rate=%.1fHz p95=%.2fms)\n",
                    pm.mode.empty() ? "(none)" : pm.mode.c_str(), pm.rows, pm.rate, pm.p95);
    if (!pm.note.empty()) std::printf("  PresentMon note  : %s\n", pm.note.c_str());
    std::printf("--------------------------------------------------------------------------------\n");
    std::printf("Acceptance: a frame reached the panel through PresentSurface::submit() %llu time(s).\n",
                (unsigned long long)ok_submits.load());
    std::printf("For click-through + capture-exclusion + the {style}x{wda} matrix, run the probe:\n");
    std::printf("  G:\\phyriad\\build-stage44a\\stage44a_present_probe.exe --style dcomp-ct --wda\n");
    std::printf("================================================================================\n");

    prod.shut();
    return (ok_submits.load() > 0) ? 0 : 1;
}

// Made with my soul - Swately <3
