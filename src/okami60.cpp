#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <share.h>
#include <limits.h>

#include "shader_src.h"

static HMODULE g_self;
static char    g_dir[MAX_PATH];
static FILE*   g_log;
static CRITICAL_SECTION g_logCs;

struct Config
{
    int   enabled      = 1;
    int   logLevel     = 1;
    int   toggleKey    = VK_F9;
    int   native60Key  = VK_F10;
    int   debugKey     = VK_F11;
    int   fpsKey       = VK_F8;
    int   targetFps    = 60;
    int   coarseRadius = 8;
    int   refineRadius = 1;
    float lambda       = 0.004f;
    float zeroBias     = 0.0015f;
    float sceneCut     = 0.10f;
    float occlK        = 12.0f;
    int   pacing       = 1;
    int   minIntervalUs = 24000;
    int   maxIntervalUs = 50000;
} g_cfg;

static void Log(const char* fmt, ...)
{
    if (!g_log || g_cfg.logLevel <= 0) return;
    EnterCriticalSection(&g_logCs);
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logCs);
}

static volatile const char* g_stage = "startup";
static volatile LONG g_crashLogged;
#define STAGE(s) (g_stage = (s))

static void ModuleOf(void* addr, char* out, size_t n, uintptr_t* off)
{
    HMODULE m = nullptr; out[0] = 0; *off = (uintptr_t)addr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &m) && m)
    {
        char p[MAX_PATH]; GetModuleFileNameA(m, p, MAX_PATH);
        const char* b = strrchr(p, '\\'); snprintf(out, n, "%s", b ? b + 1 : p);
        *off = (uintptr_t)addr - (uintptr_t)m;
    }
    else snprintf(out, n, "?");
}

static LONG CALLBACK CrashHandler(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_PRIV_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != 0xC0000409)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&g_crashLogged, 1)) return EXCEPTION_CONTINUE_SEARCH;

    char mod[128]; uintptr_t off;
    ModuleOf(ep->ExceptionRecord->ExceptionAddress, mod, sizeof mod, &off);
    Log("CRASH: code=0x%08lx at %s+0x%llx, okami60 stage='%s', thread=%lu", code, mod,
        (unsigned long long)off, (const char*)g_stage, GetCurrentThreadId());
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        Log("  access violation %s address 0x%llx", ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
            (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    CONTEXT* cx = ep->ContextRecord;
    Log("  rip=%llx rsp=%llx rax=%llx rcx=%llx rdx=%llx r8=%llx", cx->Rip, cx->Rsp, cx->Rax, cx->Rcx, cx->Rdx, cx->R8);

    uintptr_t* sp = (uintptr_t*)cx->Rsp;
    int shown = 0;
    for (int i = 0; i < 256 && shown < 16; ++i)
    {
        uintptr_t v;
        if (IsBadReadPtr(sp + i, sizeof v)) break;
        v = sp[i];
        HMODULE m = nullptr;
        if (v > 0x10000 && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                              (LPCSTR)v, &m) && m)
        {
            ModuleOf((void*)v, mod, sizeof mod, &off);
            Log("  stack[%d] %s+0x%llx", i, mod, (unsigned long long)off);
            ++shown;
        }
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

static float IniFloat(const char* path, const char* sec, const char* key, float def)
{
    char buf[64], d[64];
    snprintf(d, sizeof d, "%g", def);
    GetPrivateProfileStringA(sec, key, d, buf, sizeof buf, path);
    return (float)atof(buf);
}

static int IniInt(const char* path, const char* sec, const char* key, int def)
{
    char buf[64], d[32];
    snprintf(d, sizeof d, "%d", def);
    GetPrivateProfileStringA(sec, key, d, buf, sizeof buf, path);
    char* end = nullptr;
    long v = strtol(buf, &end, 0);
    return end == buf ? def : (int)v;
}

static void LoadConfig()
{
    char ini[MAX_PATH];
    snprintf(ini, sizeof ini, "%s\\okami60.ini", g_dir);
    g_cfg.enabled       = IniInt(ini, "General", "Enabled", g_cfg.enabled);
    g_cfg.logLevel      = IniInt(ini, "General", "Log", g_cfg.logLevel);
    g_cfg.toggleKey     = IniInt(ini, "Keys", "ToggleInterpolation", g_cfg.toggleKey);
    g_cfg.native60Key   = IniInt(ini, "Keys", "ToggleNative60Test", g_cfg.native60Key);
    g_cfg.debugKey      = IniInt(ini, "Keys", "CycleDebugView", g_cfg.debugKey);
    g_cfg.fpsKey        = IniInt(ini, "Keys", "CycleFramerate", g_cfg.fpsKey);
    g_cfg.targetFps     = IniInt(ini, "General", "TargetFPS", g_cfg.targetFps);
    g_cfg.targetFps     = g_cfg.targetFps >= 105 ? 120 : g_cfg.targetFps >= 75 ? 90 : g_cfg.targetFps >= 45 ? 60 : 30;
    g_cfg.coarseRadius  = IniInt(ini, "Quality", "CoarseRadius", g_cfg.coarseRadius);
    g_cfg.refineRadius  = IniInt(ini, "Quality", "RefineRadius", g_cfg.refineRadius);
    g_cfg.lambda        = IniFloat(ini, "Quality", "Smoothness", g_cfg.lambda);
    g_cfg.zeroBias      = IniFloat(ini, "Quality", "ZeroBias", g_cfg.zeroBias);
    g_cfg.sceneCut      = IniFloat(ini, "Quality", "SceneCutThreshold", g_cfg.sceneCut);
    g_cfg.occlK         = IniFloat(ini, "Quality", "StaticPreference", g_cfg.occlK);
    g_cfg.pacing        = IniInt(ini, "Timing", "Pacing", g_cfg.pacing);
    g_cfg.minIntervalUs = IniInt(ini, "Timing", "MinGameFrameMs", g_cfg.minIntervalUs / 1000) * 1000;
    g_cfg.maxIntervalUs = IniInt(ini, "Timing", "MaxGameFrameMs", g_cfg.maxIntervalUs / 1000) * 1000;
    if (g_cfg.coarseRadius < 2) g_cfg.coarseRadius = 2;
    if (g_cfg.coarseRadius > 16) g_cfg.coarseRadius = 16;
    if (g_cfg.refineRadius < 0) g_cfg.refineRadius = 0;
    if (g_cfg.refineRadius > 3) g_cfg.refineRadius = 3;
}

static HMODULE g_realDinput;
static FARPROC RealProc(const char* name)
{
    if (!g_realDinput)
    {
        char p[MAX_PATH];
        GetSystemDirectoryA(p, MAX_PATH);
        strcat_s(p, "\\dinput8.dll");
        g_realDinput = LoadLibraryA(p);
        if (!g_realDinput) Log("ERROR: could not load system dinput8.dll (%lu)", GetLastError());
    }
    return g_realDinput ? GetProcAddress(g_realDinput, name) : nullptr;
}

extern "C" {
HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE h, DWORD ver, const IID& riid, void** out, IUnknown* outer)
{
    typedef HRESULT (WINAPI *Fn)(HINSTANCE, DWORD, const IID&, void**, IUnknown*);
    Fn f = (Fn)RealProc("DirectInput8Create");
    return f ? f(h, ver, riid, out, outer) : E_FAIL;
}
HRESULT WINAPI Proxy_DllCanUnloadNow()
{
    typedef HRESULT (WINAPI *Fn)();
    Fn f = (Fn)RealProc("DllCanUnloadNow");
    return f ? f() : S_FALSE;
}
HRESULT WINAPI Proxy_DllGetClassObject(const CLSID& c, const IID& i, void** o)
{
    typedef HRESULT (WINAPI *Fn)(const CLSID&, const IID&, void**);
    Fn f = (Fn)RealProc("DllGetClassObject");
    return f ? f(c, i, o) : E_FAIL;
}
HRESULT WINAPI Proxy_DllRegisterServer()
{
    typedef HRESULT (WINAPI *Fn)();
    Fn f = (Fn)RealProc("DllRegisterServer");
    return f ? f() : E_FAIL;
}
HRESULT WINAPI Proxy_DllUnregisterServer()
{
    typedef HRESULT (WINAPI *Fn)();
    Fn f = (Fn)RealProc("DllUnregisterServer");
    return f ? f() : E_FAIL;
}
void* WINAPI Proxy_GetdfDIJoystick()
{
    typedef void* (WINAPI *Fn)();
    Fn f = (Fn)RealProc("GetdfDIJoystick");
    return f ? f() : nullptr;
}
}

static volatile uint8_t* g_divisor;
static bool g_divisorSearched;

static uint8_t* FindDivisor()
{
    HMODULE m = GetModuleHandleA("main.dll");
    if (!m) return nullptr;
    auto* dos = (IMAGE_DOS_HEADER*)m;
    auto* nt  = (IMAGE_NT_HEADERS64*)((uint8_t*)m + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec)
    {
        if (memcmp(sec->Name, ".text", 5) != 0) continue;
        uint8_t* b = (uint8_t*)m + sec->VirtualAddress;
        size_t   n = sec->Misc.VirtualSize;

        for (size_t i = 0; i + 16 < n; ++i)
        {
            if (b[i] == 0x80 && b[i + 1] == 0x3D && b[i + 6] == 0x01 && b[i + 7] == 0x75 &&
                b[i + 9] == 0xC6 && b[i + 10] == 0x05 && b[i + 15] == 0x3C)
            {
                int32_t disp = *(int32_t*)(b + i + 2);
                uint8_t* target = b + i + 7 + disp;
                Log("frame divisor found at main.dll+0x%llx (code at +0x%llx), value=%u",
                    (unsigned long long)(target - (uint8_t*)m),
                    (unsigned long long)(b + i - (uint8_t*)m), *target);
                return target;
            }
        }
    }
    Log("WARNING: frame divisor pattern not found; falling back to timing-only detection");
    return nullptr;
}

static int ReadDivisor()
{
    if (!g_divisor && !g_divisorSearched)
    {
        if (GetModuleHandleA("main.dll"))
        {
            g_divisorSearched = true;
            g_divisor = FindDivisor();
        }
    }
    return g_divisor ? (int)*g_divisor : -1;
}

typedef HRESULT (STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain*, UINT, UINT);
static PresentFn g_origPresent;

enum { kMaxLevels = 7 };

struct CB
{
    float srcSize[2], invSrcSize[2];
    float dstSize[2], invDstSize[2];
    float fullSize[2], invFullSize[2];
    float lambda, zeroBias;
    int   radius;
    uint32_t flags;
    float sceneCut, flowScale, occlK, t;
};

struct Level
{
    UINT w = 0, h = 0;
    ID3D11Texture2D* lum[2] = {};
    ID3D11ShaderResourceView*  lumSrv[2] = {};
    ID3D11UnorderedAccessView* lumUav[2] = {};
    ID3D11Texture2D* flow[2] = {};
    ID3D11ShaderResourceView*  flowSrv[2] = {};
    ID3D11UnorderedAccessView* flowUav[2] = {};
};

struct Interp
{
    ID3D11Device*        dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    bool  shadersOk = false;
    bool  shadersTried = false;
    ID3D11ComputeShader* csLuma = nullptr, *csDown = nullptr, *csCoarse = nullptr,
                       *csRefine = nullptr, *csProp = nullptr, *csStats = nullptr, *csCompose = nullptr;
    ID3D11Buffer*        cb = nullptr;
    ID3D11SamplerState*  smp = nullptr;

    UINT        w = 0, h = 0;
    DXGI_FORMAT bbFormat = DXGI_FORMAT_UNKNOWN;
    bool        resOk = false;
    bool        resFailed = false;
    ID3D11Texture2D*           color[2] = {};
    ID3D11ShaderResourceView*  colorSrv[2] = {};
    ID3D11Texture2D*           out = nullptr;
    ID3D11UnorderedAccessView* outUav = nullptr;
    ID3D11Texture2D*           stats = nullptr;
    ID3D11ShaderResourceView*  statsSrv = nullptr;
    ID3D11UnorderedAccessView* statsUav = nullptr;
    Level lv[kMaxLevels];
    int   levels = 0;

    int  cur = 0;
    bool haveHistory = false;
} g;

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

static void ReleaseSized()
{
    for (int i = 0; i < 2; ++i) { SafeRelease(g.colorSrv[i]); SafeRelease(g.color[i]); }
    SafeRelease(g.outUav); SafeRelease(g.out);
    SafeRelease(g.statsSrv); SafeRelease(g.statsUav); SafeRelease(g.stats);
    for (int l = 0; l < kMaxLevels; ++l)
        for (int i = 0; i < 2; ++i)
        {
            SafeRelease(g.lv[l].lumSrv[i]); SafeRelease(g.lv[l].lumUav[i]); SafeRelease(g.lv[l].lum[i]);
            SafeRelease(g.lv[l].flowSrv[i]); SafeRelease(g.lv[l].flowUav[i]); SafeRelease(g.lv[l].flow[i]);
        }
    g.levels = 0; g.w = g.h = 0; g.resOk = false; g.haveHistory = false;
}

static const char* kEntries[] = { "CSLuma", "CSDown", "CSSearchCoarse", "CSSearchRefine", "CSPropagate", "CSStats", "CSCompose" };
static const int kNumShaders = 7;
static ID3DBlob* g_blobs[kNumShaders];
static volatile LONG g_blobsReady;

static void CompileBytecode()
{
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    pD3DCompile compile = dc ? (pD3DCompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (!compile) { Log("ERROR: d3dcompiler_47.dll / D3DCompile not available"); InterlockedExchange(&g_blobsReady, -1); return; }
    int64_t t0 = GetTickCount64();
    for (int i = 0; i < kNumShaders; ++i)
    {
        ID3DBlob* err = nullptr;
        HRESULT hr = compile(g_hlsl, strlen(g_hlsl), "okami60.hlsl", nullptr, nullptr, kEntries[i], "cs_5_0",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &g_blobs[i], &err);
        if (err)
        {
            if (FAILED(hr) || g_cfg.logLevel >= 2) Log("shader %s: %s", kEntries[i], (const char*)err->GetBufferPointer());
            err->Release();
        }
        if (FAILED(hr) || !g_blobs[i]) { Log("ERROR: compiling %s failed (0x%08lx)", kEntries[i], hr); InterlockedExchange(&g_blobsReady, -1); return; }
    }
    Log("shaders compiled in %llu ms", (unsigned long long)(GetTickCount64() - t0));
    InterlockedExchange(&g_blobsReady, 1);
}

static bool CreateShaders()
{
    g.shadersTried = true;
    ID3D11ComputeShader** dst[kNumShaders] = { &g.csLuma, &g.csDown, &g.csCoarse, &g.csRefine, &g.csProp, &g.csStats, &g.csCompose };
    for (int i = 0; i < kNumShaders; ++i)
    {
        HRESULT hr = g.dev->CreateComputeShader(g_blobs[i]->GetBufferPointer(), g_blobs[i]->GetBufferSize(), nullptr, dst[i]);
        if (FAILED(hr)) { Log("ERROR: CreateComputeShader %s (0x%08lx)", kEntries[i], hr); return false; }
    }

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = (sizeof(CB) + 15) & ~15u;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g.dev->CreateBuffer(&bd, nullptr, &g.cb))) { Log("ERROR: CreateBuffer cb"); return false; }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    if (FAILED(g.dev->CreateSamplerState(&sd, &g.smp))) { Log("ERROR: CreateSamplerState"); return false; }
    Log("shader objects created");
    return true;
}

static DXGI_FORMAT TypelessOf(DXGI_FORMAT f, DXGI_FORMAT* viewFmt)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        *viewFmt = DXGI_FORMAT_R8G8B8A8_UNORM; return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        *viewFmt = DXGI_FORMAT_B8G8R8A8_UNORM; return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM:
        *viewFmt = DXGI_FORMAT_R10G10B10A2_UNORM; return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
        *viewFmt = DXGI_FORMAT_R16G16B16A16_FLOAT; return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    default:
        *viewFmt = DXGI_FORMAT_UNKNOWN; return DXGI_FORMAT_UNKNOWN;
    }
}

static bool MakeTex(UINT w, UINT h, DXGI_FORMAT fmt, DXGI_FORMAT view, UINT bind,
                    ID3D11Texture2D** t, ID3D11ShaderResourceView** srv, ID3D11UnorderedAccessView** uav)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = fmt;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = bind;
    if (FAILED(g.dev->CreateTexture2D(&td, nullptr, t))) return false;
    if (srv)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC s = {};
        s.Format = view; s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; s.Texture2D.MipLevels = 1;
        if (FAILED(g.dev->CreateShaderResourceView(*t, &s, srv))) return false;
    }
    if (uav)
    {
        D3D11_UNORDERED_ACCESS_VIEW_DESC u = {};
        u.Format = view; u.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (FAILED(g.dev->CreateUnorderedAccessView(*t, &u, uav))) return false;
    }
    return true;
}

static bool EnsureSized(const D3D11_TEXTURE2D_DESC& bd)
{
    if (g.resOk && bd.Width == g.w && bd.Height == g.h && bd.Format == g.bbFormat) return true;
    if (g.resFailed && bd.Width == g.w && bd.Height == g.h && bd.Format == g.bbFormat) return false;
    ReleaseSized();
    g.w = bd.Width; g.h = bd.Height; g.bbFormat = bd.Format;
    g.resFailed = true;

    if (bd.SampleDesc.Count != 1) { Log("back buffer is MSAA (%u) - interpolation disabled", bd.SampleDesc.Count); return false; }
    DXGI_FORMAT view;
    DXGI_FORMAT tl = TypelessOf(bd.Format, &view);
    if (tl == DXGI_FORMAT_UNKNOWN) { Log("unsupported back buffer format %d", (int)bd.Format); return false; }
    UINT sup = 0;
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 s2 = { view, 0 };
    g.dev->CheckFormatSupport(view, &sup);
    g.dev->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &s2, sizeof s2);
    if (!(sup & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) || !(s2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE))
        Log("note: driver does not report typed UAV stores for format %d; trying anyway", (int)view);

    for (int i = 0; i < 2; ++i)
        if (!MakeTex(g.w, g.h, tl, view, D3D11_BIND_SHADER_RESOURCE, &g.color[i], &g.colorSrv[i], nullptr))
        { Log("ERROR: color textures"); return false; }
    if (!MakeTex(g.w, g.h, tl, view, D3D11_BIND_UNORDERED_ACCESS, &g.out, nullptr, &g.outUav))
    { Log("ERROR: output texture"); return false; }

    UINT lw = (g.w + 1) / 2, lh = (g.h + 1) / 2;
    g.levels = 0;
    while (g.levels < kMaxLevels)
    {
        Level& L = g.lv[g.levels];
        L.w = lw; L.h = lh;
        for (int i = 0; i < 2; ++i)
        {
            if (!MakeTex(lw, lh, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT,
                         D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, &L.lum[i], &L.lumSrv[i], &L.lumUav[i]) ||
                !MakeTex(lw, lh, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
                         D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, &L.flow[i], &L.flowSrv[i], &L.flowUav[i]))
            { Log("ERROR: pyramid level %d", g.levels); return false; }
        }
        ++g.levels;
        if (lw / 2 < 40 || lh / 2 < 24) break;
        lw = (lw + 1) / 2; lh = (lh + 1) / 2;
    }

    {
        ID3D11Texture2D* st = nullptr;
        if (!MakeTex(2, 1, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT,
                     D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, &st, &g.statsSrv, &g.statsUav))
        { SafeRelease(st); Log("ERROR: stats texture"); return false; }
        g.stats = st;
    }

    g.resOk = true; g.resFailed = false; g.haveHistory = false;
    Log("resources: %ux%u fmt=%d, %d pyramid levels (coarsest %ux%u)",
        g.w, g.h, (int)g.bbFormat, g.levels, g.lv[g.levels - 1].w, g.lv[g.levels - 1].h);
    return true;
}

struct CsState
{
    ID3D11ComputeShader* cs = nullptr;
    ID3D11ClassInstance* inst[256]; UINT nInst = 256;
    ID3D11Buffer* cb = nullptr;
    ID3D11ShaderResourceView* srv[4] = {};
    ID3D11UnorderedAccessView* uav = nullptr;
    ID3D11SamplerState* smp = nullptr;
};

static void SaveCs(CsState& s)
{
    s.nInst = 256;
    g.ctx->CSGetShader(&s.cs, s.inst, &s.nInst);
    g.ctx->CSGetConstantBuffers(0, 1, &s.cb);
    g.ctx->CSGetShaderResources(0, 4, s.srv);
    g.ctx->CSGetUnorderedAccessViews(0, 1, &s.uav);
    g.ctx->CSGetSamplers(0, 1, &s.smp);
}

static void RestoreCs(CsState& s)
{
    g.ctx->CSSetShader(s.cs, s.nInst ? s.inst : nullptr, s.nInst);
    g.ctx->CSSetConstantBuffers(0, 1, &s.cb);
    g.ctx->CSSetShaderResources(0, 4, s.srv);
    UINT keep = (UINT)-1;
    g.ctx->CSSetUnorderedAccessViews(0, 1, &s.uav, &keep);
    g.ctx->CSSetSamplers(0, 1, &s.smp);
    SafeRelease(s.cs);
    for (UINT i = 0; i < s.nInst; ++i) SafeRelease(s.inst[i]);
    SafeRelease(s.cb);
    for (auto& v : s.srv) SafeRelease(v);
    SafeRelease(s.uav);
    SafeRelease(s.smp);
}

static float g_composeT = 0.5f;
static void SetCB(UINT sw, UINT sh, UINT dw, UINT dh, int radius, uint32_t flags)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g.ctx->Map(g.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    CB c = {};
    c.srcSize[0] = (float)sw; c.srcSize[1] = (float)sh; c.invSrcSize[0] = 1.f / sw; c.invSrcSize[1] = 1.f / sh;
    c.dstSize[0] = (float)dw; c.dstSize[1] = (float)dh; c.invDstSize[0] = 1.f / dw; c.invDstSize[1] = 1.f / dh;
    c.fullSize[0] = (float)g.w; c.fullSize[1] = (float)g.h; c.invFullSize[0] = 1.f / g.w; c.invFullSize[1] = 1.f / g.h;
    c.lambda = g_cfg.lambda; c.zeroBias = g_cfg.zeroBias; c.radius = radius; c.flags = flags;
    c.sceneCut = g_cfg.sceneCut; c.flowScale = (float)g.w / (float)g.lv[0].w; c.occlK = g_cfg.occlK; c.t = g_composeT;
    memcpy(m.pData, &c, sizeof c);
    g.ctx->Unmap(g.cb, 0);
}

static void Bind(ID3D11ShaderResourceView* t0, ID3D11ShaderResourceView* t1,
                 ID3D11ShaderResourceView* t2, ID3D11ShaderResourceView* t3, ID3D11UnorderedAccessView* u0)
{
    ID3D11UnorderedAccessView* nu = nullptr; UINT keep = (UINT)-1;
    ID3D11ShaderResourceView* ns[4] = {};

    g.ctx->CSSetUnorderedAccessViews(0, 1, &nu, &keep);
    g.ctx->CSSetShaderResources(0, 4, ns);
    ID3D11ShaderResourceView* s[4] = { t0, t1, t2, t3 };
    g.ctx->CSSetShaderResources(0, 4, s);
    g.ctx->CSSetUnorderedAccessViews(0, 1, &u0, &keep);
}

static UINT Groups(UINT n, UINT g8 = 8) { return (n + g8 - 1) / g8; }

static uint32_t g_debugView;

static void BuildPyramid(int slot)
{
    g.ctx->CSSetShader(g.csLuma, nullptr, 0);
    SetCB(g.w, g.h, g.lv[0].w, g.lv[0].h, 0, 0);
    Bind(g.colorSrv[slot], nullptr, nullptr, nullptr, g.lv[0].lumUav[slot]);
    g.ctx->Dispatch(Groups(g.lv[0].w), Groups(g.lv[0].h), 1);

    g.ctx->CSSetShader(g.csDown, nullptr, 0);
    for (int l = 1; l < g.levels; ++l)
    {
        SetCB(g.lv[l - 1].w, g.lv[l - 1].h, g.lv[l].w, g.lv[l].h, 0, 0);
        Bind(g.lv[l - 1].lumSrv[slot], nullptr, nullptr, nullptr, g.lv[l].lumUav[slot]);
        g.ctx->Dispatch(Groups(g.lv[l].w), Groups(g.lv[l].h), 1);
    }
}

static void Propagate(int l, int p, int c)
{
    Level& L = g.lv[l];
    g.ctx->CSSetShader(g.csProp, nullptr, 0);
    SetCB(L.w, L.h, L.w, L.h, 4, 0);
    Bind(L.lumSrv[p], L.lumSrv[c], L.flowSrv[0], nullptr, L.flowUav[1]);
    g.ctx->Dispatch(Groups(L.w), Groups(L.h), 1);
    SetCB(L.w, L.h, L.w, L.h, 1, 0);
    Bind(L.lumSrv[p], L.lumSrv[c], L.flowSrv[1], nullptr, L.flowUav[0]);
    g.ctx->Dispatch(Groups(L.w), Groups(L.h), 1);
    Bind(nullptr, nullptr, nullptr, nullptr, nullptr);
    g.ctx->CopyResource(L.flow[1], L.flow[0]);
}

static void Synthesize()
{
    int c = g.cur, p = 1 - g.cur;
    int top = g.levels - 1;

    STAGE("synth: coarse search");

    g.ctx->CSSetShader(g.csCoarse, nullptr, 0);
    SetCB(g.lv[top].w, g.lv[top].h, g.lv[top].w, g.lv[top].h, g_cfg.coarseRadius, 0);
    Bind(g.lv[top].lumSrv[p], g.lv[top].lumSrv[c], nullptr, nullptr, g.lv[top].flowUav[0]);
    g.ctx->Dispatch(Groups(g.lv[top].w), Groups(g.lv[top].h), 1);

    Propagate(top, p, c);

    SetCB(g.lv[top].w, g.lv[top].h, g.lv[top].w, g.lv[top].h, 0, 0);
    g.ctx->CSSetShader(g.csStats, nullptr, 0);
    Bind(g.lv[top].lumSrv[p], g.lv[top].lumSrv[c], g.lv[top].flowSrv[1], nullptr, g.statsUav);
    g.ctx->Dispatch(1, 1, 1);

    for (int l = top - 1; l >= 0; --l)
    {
        Level& L = g.lv[l];
        g.ctx->CSSetShader(g.csRefine, nullptr, 0);
        SetCB(L.w, L.h, L.w, L.h, g_cfg.refineRadius, 0);
        Bind(L.lumSrv[p], L.lumSrv[c], g.lv[l + 1].flowSrv[1], nullptr, L.flowUav[0]);
        g.ctx->Dispatch(Groups(L.w), Groups(L.h), 1);

        Propagate(l, p, c);
    }

    Bind(nullptr, nullptr, nullptr, nullptr, nullptr);
}

static void Compose(float t)
{
    int c = g.cur, p = 1 - g.cur;
    STAGE("synth: compose");
    g_composeT = t;
    uint32_t flags = g_debugView == 1 ? 1u : g_debugView == 2 ? 2u : 0u;
    g.ctx->CSSetShader(g.csCompose, nullptr, 0);
    SetCB(g.lv[0].w, g.lv[0].h, g.w, g.h, 0, flags);
    Bind(g.colorSrv[p], g.colorSrv[c], g.lv[0].flowSrv[1], g.statsSrv, g.outUav);
    g.ctx->Dispatch(Groups(g.w), Groups(g.h), 1);
    Bind(nullptr, nullptr, nullptr, nullptr, nullptr);
}

static LARGE_INTEGER g_qpf;
static int64_t Now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static double  ToUs(int64_t ticks) { return (double)ticks * 1e6 / (double)g_qpf.QuadPart; }

static void WaitUntil(int64_t target)
{
    for (;;)
    {
        int64_t now = Now();
        if (now >= target) return;
        double remUs = ToUs(target - now);
        if (remUs > 2500.0) Sleep(1);
        else if (remUs > 300.0) Sleep(0);
        else YieldProcessor();
    }
}

static bool g_interpOn = true;
static bool g_force60;
static bool KeyPressed(int vk)
{
    static bool down[256];
    if (vk <= 0 || vk > 255) return false;
    bool d = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = d && !down[vk];
    down[vk] = d;
    return edge;
}

static bool GameHasFocus()
{
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static int64_t g_lastCall;
static double  g_emaUs = 33333.0;
static uint64_t g_frames, g_generated;

static void SaveTargetFps()
{
    char ini[MAX_PATH], v[16];
    snprintf(ini, sizeof ini, "%s\\okami60.ini", g_dir);
    snprintf(v, sizeof v, "%d", g_cfg.targetFps);
    WritePrivateProfileStringA("General", "TargetFPS", v, ini);
}

static int RefreshHz(IDXGISwapChain* sc)
{
    static int hz = 0; static uint64_t last = 0;
    uint64_t now = GetTickCount64();
    if (hz && now - last < 2000) return hz;
    last = now;
    DXGI_SWAP_CHAIN_DESC d;
    HWND w = SUCCEEDED(sc->GetDesc(&d)) ? d.OutputWindow : nullptr;
    HMONITOR mon = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXA mi; mi.cbSize = sizeof mi;
    DEVMODEA dm = {}; dm.dmSize = sizeof dm;
    int r = 60;
    if (GetMonitorInfoA(mon, &mi) && EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        r = (int)dm.dmDisplayFrequency;
    if (r != hz) Log("display refresh rate: %d Hz", r);
    hz = r;
    return hz;
}

static int EffectiveMultiplier(IDXGISwapChain* sc, UINT sync)
{
    int want = g_cfg.targetFps / 30;
    int mult = want;
    if (sync > 0)
    {
        int cap = (RefreshHz(sc) + 2) / (30 * (int)sync);
        if (cap < 1) cap = 1;
        if (mult > cap) mult = cap;
    }
    static int lastLogged = -1;
    if (mult != lastLogged)
    {
        Log("frame-rate mode: target %d fps -> showing %d fps%s", g_cfg.targetFps, mult * 30,
            mult < want ? " (capped by display refresh rate with v-sync on)" : "");
        lastLogged = mult;
    }
    return mult;
}

#include "menu.inc"

typedef void (*SwapFn)(void* self, UINT sync);
static SwapFn            g_origSwap;
static IDXGISwapChain**  g_pEngineSc;
static void*             g_swapSelf;
static bool              g_engineMode;

static HRESULT PassPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    if (g_engineMode) { g_origSwap(g_swapSelf, sync); return S_OK; }
    return g_origPresent(sc, sync, flags);
}

static HRESULT GenPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    if (g_engineMode) return sc->Present(sync, flags);
    return g_origPresent(sc, sync, flags);
}

static HRESULT DoFrame(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    if (flags & DXGI_PRESENT_TEST) return PassPresent(sc, sync, flags);

    int64_t now = Now();
    double dtUs = g_lastCall ? ToUs(now - g_lastCall) : 0.0;
    g_lastCall = now;
    if (dtUs > 5000.0 && dtUs < 100000.0) g_emaUs = g_emaUs * 0.9 + dtUs * 0.1;
    ++g_frames;

    if (GameHasFocus())
    {
        if (KeyPressed(g_cfg.toggleKey))
        {
            g_interpOn = !g_interpOn; g.haveHistory = false;
            Log("interpolation %s", g_interpOn ? "ON" : "OFF");
        }
        if (KeyPressed(g_cfg.fpsKey))
        {
            g_cfg.targetFps = g_cfg.targetFps >= 120 ? 30 : g_cfg.targetFps + 30;
            g.haveHistory = false;
            SaveTargetFps();
            menu::RefreshValueText();
            Log("target frame rate %d fps", g_cfg.targetFps);
        }
        if (KeyPressed(g_cfg.debugKey))
        {
            g_debugView = (g_debugView + 1) % 3;
            Log("debug view %u (%s)", g_debugView, g_debugView == 0 ? "normal" : g_debugView == 1 ? "motion vectors" : "plain blend");
        }
        if (KeyPressed(g_cfg.native60Key))
        {
            ReadDivisor();
            if (g_divisor)
            {
                g_force60 = !g_force60;
                if (!g_force60) *g_divisor = 2;
                Log("NATIVE 60 TEST %s (game logic runs every display frame - expect speed/physics issues)",
                    g_force60 ? "ON" : "OFF");
            }
            else Log("native 60 test unavailable: divisor not located");
        }
    }

    int div = ReadDivisor();
    if (g_force60 && g_divisor) { *g_divisor = 1; div = 1; }

    bool cadence30 = (div == 2) || (div < 0 && g_emaUs > g_cfg.minIntervalUs);
    bool frameOk   = dtUs > g_cfg.minIntervalUs && dtUs < g_cfg.maxIntervalUs;
    if (!g_cfg.enabled || !g_interpOn || !cadence30 || g_cfg.targetFps <= 30)
    {
        g.haveHistory = false;
        return PassPresent(sc, sync, flags);
    }

    STAGE("hook: setup");
    if (!g.dev)
    {
        if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g.dev)) || !g.dev)
        {
            Log("ERROR: swap chain has no D3D11 device"); g_cfg.enabled = 0;
            return PassPresent(sc, sync, flags);
        }
        g.dev->GetImmediateContext(&g.ctx);
        Log("device feature level 0x%x", (unsigned)g.dev->GetFeatureLevel());
    }
    if (g_blobsReady == 0) return PassPresent(sc, sync, flags);
    if (!g.shadersTried) g.shadersOk = g_blobsReady == 1 && CreateShaders();
    if (!g.shadersOk) return PassPresent(sc, sync, flags);

    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb)
        return PassPresent(sc, sync, flags);
    D3D11_TEXTURE2D_DESC bd; bb->GetDesc(&bd);
    STAGE("ensure resources");
    if (!EnsureSized(bd)) { bb->Release(); return PassPresent(sc, sync, flags); }

    STAGE("save CS state");
    CsState saved; SaveCs(saved);
    g.ctx->CSSetConstantBuffers(0, 1, &g.cb);
    g.ctx->CSSetSamplers(0, 1, &g.smp);

    int next = g.haveHistory ? 1 - g.cur : g.cur;
    STAGE("copy back buffer");
    g.ctx->CopyResource(g.color[next], bb);
    g.cur = next;
    STAGE("build pyramid");
    BuildPyramid(g.cur);

    bool doInterp = g.haveHistory && frameOk;
    if (g_cfg.logLevel >= 3)
        Log("present: dt=%.2fms ema=%.2fms div=%d hist=%d interp=%d sync=%u flags=0x%x", dtUs / 1000.0, g_emaUs / 1000.0,
            div, (int)g.haveHistory, (int)doInterp, sync, flags);
    int mult = EffectiveMultiplier(sc, sync);
    if (mult < 2) doInterp = false;
    if (doInterp) { STAGE("synthesize"); Synthesize(); Compose(1.0f / mult); }
    STAGE("restore CS state");
    RestoreCs(saved);

    HRESULT hr = S_OK;
    if (doInterp)
    {
        double periodUs = div == 2 ? 1e6 / 30.0 : g_emaUs;
        if (periodUs > 50000.0) periodUs = 50000.0;
        int64_t step = (int64_t)(periodUs / mult * (double)g_qpf.QuadPart / 1e6);
        int64_t t0 = 0;

        for (int k = 1; k < mult; ++k)
        {
            if (k > 1)
            {
                CsState s2; SaveCs(s2);
                g.ctx->CSSetConstantBuffers(0, 1, &g.cb);
                g.ctx->CSSetSamplers(0, 1, &g.smp);
                Compose((float)k / mult);
                RestoreCs(s2);
                if (g_cfg.pacing) WaitUntil(t0 + step * (k - 1));
                if (!bb && FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) bb = nullptr;
            }
            if (bb)
            {
                STAGE("copy generated frame");
                g.ctx->CopyResource(bb, g.out);
                bb->Release(); bb = nullptr;
            }
            if (k == 1) t0 = Now();
            STAGE("present generated");
            hr = GenPresent(sc, sync, flags);
            ++g_generated;
            if (FAILED(hr)) { Log("present(generated) failed 0x%08lx", hr); g.haveHistory = false; return hr; }
        }

        if (g_cfg.pacing) WaitUntil(t0 + step * (mult - 1));
        STAGE("copy real frame back");
        if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb)
        {
            g.ctx->CopyResource(bb, g.color[g.cur]);
            bb->Release(); bb = nullptr;
        }
        STAGE("present real");
        hr = PassPresent(sc, sync, flags);
    }
    else
    {
        bb->Release();
        STAGE("present (history frame)");
        hr = PassPresent(sc, sync, flags);
    }
    STAGE("game code");
    g.haveHistory = true;

    if (g_cfg.logLevel >= 2 && (g_frames % 300) == 0)
        Log("frames=%llu generated=%llu ema=%.2fms div=%d", g_frames, g_generated, g_emaUs / 1000.0, div);
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    return DoFrame(sc, sync, flags);
}

static void HookedSwap(void* self, UINT sync)
{
    IDXGISwapChain* sc = g_pEngineSc ? *g_pEngineSc : nullptr;
    if (!sc) { g_origSwap(self, sync); return; }
    g_swapSelf = self;
    DoFrame(sc, sync, 0);
}

static LRESULT CALLBACK DummyWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static bool InstallPresentHook()
{
    HMODULE d3d11 = LoadLibraryA("d3d11.dll");
    if (!d3d11) { Log("ERROR: d3d11.dll"); return false; }
    auto create = (PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain");
    if (!create) { Log("ERROR: D3D11CreateDeviceAndSwapChain"); return false; }

    WNDCLASSA wc = {};
    wc.lpfnWndProc = DummyWndProc;
    wc.hInstance = g_self;
    wc.lpszClassName = "okami60_dummy";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr, nullptr, g_self, nullptr);
    if (!hwnd) { Log("ERROR: dummy window"); return false; }

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 16; sd.BufferDesc.Height = 16;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* sc = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr))
        hr = create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr) || !sc) { Log("ERROR: dummy device (0x%08lx)", hr); DestroyWindow(hwnd); return false; }

    void** vt = *(void***)sc;
    DWORD old;
    if (VirtualProtect(&vt[8], sizeof(void*), PAGE_READWRITE, &old))
    {
        g_origPresent = (PresentFn)vt[8];
        vt[8] = (void*)&HookedPresent;
        VirtualProtect(&vt[8], sizeof(void*), old, &old);
        FlushInstructionCache(GetCurrentProcess(), &vt[8], sizeof(void*));
        Log("IDXGISwapChain::Present hooked (vtable %p, original %p)", (void*)vt, (void*)g_origPresent);
    }
    else Log("ERROR: VirtualProtect on vtable failed (%lu)", GetLastError());

    sc->Release(); ctx->Release(); dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, g_self);
    return g_origPresent != nullptr;
}

static bool InstallEngineHook()
{
    HMODULE fk = GetModuleHandleA("flower_kernel.dll");
    if (!fk) return false;
    uint8_t* thunk = (uint8_t*)GetProcAddress(fk, "?swap@SwapChain@render@m2@@QEAAXI@Z");
    if (!thunk || thunk[0] != 0xE9) { Log("engine hook: swap export not found / unexpected (%p)", (void*)thunk); return false; }
    uint8_t* real = thunk + 5 + *(int32_t*)(thunk + 1);

    static const uint8_t pat[] = { 0x45, 0x33, 0xC0, 0x8B, 0xD3, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x40 };
    IDXGISwapChain** pSc = nullptr;
    for (int i = 0; i < 0x80 && !pSc; ++i)
        if (real[i] == 0x48 && real[i + 1] == 0x8B && real[i + 2] == 0x0D && memcmp(real + i + 7, pat, sizeof pat) == 0)
            pSc = (IDXGISwapChain**)(real + i + 7 + *(int32_t*)(real + i + 3));
    if (!pSc) { Log("engine hook: swap chain global not found"); return false; }

    uint8_t* stub = nullptr;
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity;
    for (uintptr_t d = gran; d < 0x70000000 && !stub; d += gran)
    {
        uintptr_t base = ((uintptr_t)thunk & ~(gran - 1));
        if (base > d) stub = (uint8_t*)VirtualAlloc((void*)(base - d), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (!stub) stub = (uint8_t*)VirtualAlloc((void*)(base + d), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    }
    if (!stub) { Log("engine hook: no memory near flower_kernel"); return false; }
    stub[0] = 0xFF; stub[1] = 0x25; *(int32_t*)(stub + 2) = 0;
    *(void**)(stub + 6) = (void*)&HookedSwap;
    FlushInstructionCache(GetCurrentProcess(), stub, 14);

    int64_t rel = (int64_t)(stub - (thunk + 5));
    if (rel != (int32_t)rel) { Log("engine hook: stub out of range"); return false; }
    g_origSwap = (SwapFn)real;
    g_pEngineSc = pSc;
    DWORD old;
    if (!VirtualProtect(thunk + 1, 4, PAGE_EXECUTE_READWRITE, &old)) { Log("engine hook: VirtualProtect failed"); return false; }
    *(int32_t*)(thunk + 1) = (int32_t)rel;
    VirtualProtect(thunk + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), thunk, 5);
    g_engineMode = true;
    return true;
}

static DWORD WINAPI InitThread(LPVOID)
{
    Log("okami60 loaded (enabled=%d, toggle=0x%x, native60 test=0x%x, debug=0x%x)",
        g_cfg.enabled, g_cfg.toggleKey, g_cfg.native60Key, g_cfg.debugKey);
    if (!g_cfg.enabled) return 0;
    AddVectoredExceptionHandler(0, CrashHandler);
    if (g_engineMode)
        Log("engine hook: flower_kernel SwapChain::swap detoured (swap chain ptr at %p)", (void*)g_pEngineSc);
    else
    {
        Log("flower_kernel.dll not present - test mode, hooking IDXGISwapChain::Present vtable");
        InstallPresentHook();
    }
    CompileBytecode();
    if (!menu::g_ok)
        for (int i = 0; i < 600 && !menu::Install(); ++i) Sleep(100);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = h;
        DisableThreadLibraryCalls(h);
        QueryPerformanceFrequency(&g_qpf);
        InitializeCriticalSection(&g_logCs);
        GetModuleFileNameA(h, g_dir, MAX_PATH);
        char* s = strrchr(g_dir, '\\'); if (s) *s = 0;
        char logPath[MAX_PATH];
        snprintf(logPath, sizeof logPath, "%s\\okami60.log", g_dir);
        g_log = _fsopen(logPath, "w", _SH_DENYWR);
        LoadConfig();
        if (g_cfg.enabled) { InstallEngineHook(); menu::Install(); }
        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
