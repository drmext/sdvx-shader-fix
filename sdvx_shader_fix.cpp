#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#include <new>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MinHook.h"
#include "shaders_bytecode.h"

/*
 * Spice -k hook for SOUND VOLTEX on Wine (Nabla KFC:A:G:A:2025120900 / 2026040700 + older).
 *
 * Wine's D3DXCompileShader fails (or succeeds inconsistently) on Pseudo3d,
 * AFP, and Cubism Live2D shaders. Mixing Wine-compiled VS with fxc PS breaks
 * interpolators; Cubism SetUpMask failures leave characters garbled/missing.
 *
 * Also: Cubism's Normal technique uses D3DXCreateEffect; Wine fails that
 * compile so Live2D never gets past draw early-outs.
 *
 * Fix: on known (src_len, profile, entry) matches, return fxc-precompiled
 * bytecode. With force_fxc=1, also replace successful Wine compiles so VS/PS
 * always come from the same compiler. For Cubism FX, feed an fxc .fxo into
 * D3DXCreateEffect (Normal-only 760; full multi-technique 3130 / 4157).
 *
 * Multi-version: pre-20251209 HSV uses src_len=5852 (legacy rewrite blob).
 * Early builds lack Pseudo3d/csm CompileShader sources — those keys no-op.
 *
 * Whisky: ship official Microsoft d3dx9_43.dll next to the game/exe. Wine's
 * builtin system32 copy can AV inside GetShaderConstantTable / CreateEffect.
 */

#define INI_NAME L"sdvx_shader_fix_64bit.ini"
#define LOG_NAME L"sdvx_shader_fix_64bit.log"

#ifndef D3DX_DEFAULT
#define D3DX_DEFAULT ((UINT)-1)
#endif

/* Minimal ID3DXBuffer (d3dx9shader.h) — avoid linking d3dx9.lib */
MIDL_INTERFACE("8BA5FB08-5195-40e2-AC58-0D989C3A0102")
ID3DXBuffer : public IUnknown
{
public:
    virtual LPVOID STDMETHODCALLTYPE GetBufferPointer(void) = 0;
    virtual DWORD STDMETHODCALLTYPE GetBufferSize(void) = 0;
};

typedef struct _D3DXMACRO {
    LPCSTR Name;
    LPCSTR Definition;
} D3DXMACRO;

typedef interface ID3DXInclude ID3DXInclude;
typedef interface ID3DXConstantTable ID3DXConstantTable;

typedef HRESULT(WINAPI *D3DXCompileShader_t)(
    LPCSTR pSrcData,
    UINT SrcDataLen,
    CONST D3DXMACRO *pDefines,
    ID3DXInclude *pInclude,
    LPCSTR pFunctionName,
    LPCSTR pProfile,
    DWORD Flags,
    ID3DXBuffer **ppShader,
    ID3DXBuffer **ppErrorMsgs,
    ID3DXConstantTable **ppConstantTable);

typedef HRESULT(WINAPI *D3DXGetShaderConstantTable_t)(
    CONST DWORD *pFunction, ID3DXConstantTable **ppConstantTable);

typedef HRESULT(WINAPI *D3DXCreateEffect_t)(
    void *pDevice,
    LPCVOID pSrcData,
    UINT SrcDataLen,
    CONST D3DXMACRO *pDefines,
    ID3DXInclude *pInclude,
    DWORD Flags,
    void *pPool,
    void **ppEffect,
    ID3DXBuffer **ppCompilationErrors);

static HMODULE g_self;
static uintptr_t g_base;

static int g_log_enabled = 1;
static int g_log_verbose = 0;
static int g_enabled = 1;
static int g_fallback = 1;
static int g_force_fxc = 1;

static CRITICAL_SECTION g_log_cs;
static wchar_t g_dir[MAX_PATH];
static FILE *g_log;

static D3DXCompileShader_t g_orig_compile;
static D3DXGetShaderConstantTable_t g_get_ctable;
static D3DXCreateEffect_t g_orig_create_effect;

static void log_msg(const char *fmt, ...)
{
    if (!g_log_enabled || !g_log)
        return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    EnterCriticalSection(&g_log_cs);
    fprintf(g_log, "%02u:%02u:%02u.%03u ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_log_cs);
}

static void init_paths(void)
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);
    g_dir[0] = 0;
    if (n == 0 || n >= MAX_PATH)
        return;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash)
        return;
    *slash = 0;
    wcsncpy_s(g_dir, path, _TRUNCATE);
}

static void open_log(void)
{
    if (!g_log_enabled || !g_dir[0])
        return;
    wchar_t log_path[MAX_PATH];
    _snwprintf_s(log_path, _TRUNCATE, L"%s\\" LOG_NAME, g_dir);
    _wfopen_s(&g_log, log_path, L"a");
}

static void ini_path(wchar_t *ini, size_t n)
{
    _snwprintf_s(ini, n, _TRUNCATE, L"%s\\" INI_NAME, g_dir);
}

static int ini_int(const wchar_t *key, int def)
{
    wchar_t ini[MAX_PATH];
    if (!g_dir[0])
        return def;
    ini_path(ini, MAX_PATH);
    return (int)GetPrivateProfileIntW(L"sdvx_shader_fix", key, def, ini);
}

static const SdvxShaderBlob *find_blob(LPCSTR src, UINT src_len, LPCSTR entry, LPCSTR profile)
{
    int i;
    (void)src;
    if (!entry || !profile)
        return NULL;
    for (i = 0; i < SDVX_SHADER_BLOB_COUNT; i++) {
        if (g_sdvx_shader_blobs[i].src_len == src_len &&
            strcmp(g_sdvx_shader_blobs[i].profile, profile) == 0 &&
            strcmp(g_sdvx_shader_blobs[i].entry, entry) == 0)
            return &g_sdvx_shader_blobs[i];
    }
    return NULL;
}

static void clear_create_effect_outs(void **ppEffect, ID3DXBuffer **ppCompilationErrors)
{
    if (ppEffect && *ppEffect) {
        ((IUnknown *)*ppEffect)->Release();
        *ppEffect = NULL;
    }
    if (ppCompilationErrors && *ppCompilationErrors) {
        (*ppCompilationErrors)->Release();
        *ppCompilationErrors = NULL;
    }
}

/* ---- SEH wrappers (keep C++ dtors out of these frames) ---- */

static HRESULT safe_orig_compile(
    LPCSTR pSrcData,
    UINT SrcDataLen,
    CONST D3DXMACRO *pDefines,
    ID3DXInclude *pInclude,
    LPCSTR pFunctionName,
    LPCSTR pProfile,
    DWORD Flags,
    ID3DXBuffer **ppShader,
    ID3DXBuffer **ppErrorMsgs,
    ID3DXConstantTable **ppConstantTable)
{
    HRESULT hr = E_FAIL;
    if (!g_orig_compile)
        return E_FAIL;
    __try {
        hr = g_orig_compile(
            pSrcData, SrcDataLen, pDefines, pInclude, pFunctionName, pProfile, Flags,
            ppShader, ppErrorMsgs, ppConstantTable);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_msg("D3DXCompileShader exception code=0x%08X", (unsigned)GetExceptionCode());
        hr = E_FAIL;
    }
    return hr;
}

static HRESULT safe_orig_create_effect(
    void *pDevice,
    LPCVOID pSrcData,
    UINT SrcDataLen,
    CONST D3DXMACRO *pDefines,
    ID3DXInclude *pInclude,
    DWORD Flags,
    void *pPool,
    void **ppEffect,
    ID3DXBuffer **ppCompilationErrors)
{
    HRESULT hr = E_FAIL;
    if (!g_orig_create_effect)
        return E_FAIL;
    __try {
        hr = g_orig_create_effect(
            pDevice, pSrcData, SrcDataLen, pDefines, pInclude, Flags, pPool, ppEffect,
            ppCompilationErrors);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_msg("D3DXCreateEffect exception code=0x%08X", (unsigned)GetExceptionCode());
        hr = E_FAIL;
    }
    return hr;
}

static HRESULT safe_get_ctable(CONST DWORD *pFunction, ID3DXConstantTable **ppConstantTable)
{
    HRESULT hr = E_FAIL;
    if (!g_get_ctable || !ppConstantTable)
        return E_FAIL;
    *ppConstantTable = NULL;
    __try {
        hr = g_get_ctable(pFunction, ppConstantTable);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_msg("D3DXGetShaderConstantTable exception code=0x%08X", (unsigned)GetExceptionCode());
        *ppConstantTable = NULL;
        hr = E_FAIL;
    }
    return hr;
}

/* ---- synthetic ID3DXBuffer ---- */

class SynthBuffer : public ID3DXBuffer {
    LONG m_refs;
    uint8_t *m_data;
    DWORD m_size;

public:
    SynthBuffer(const uint8_t *data, DWORD size)
        : m_refs(1), m_data(NULL), m_size(size)
    {
        m_data = (uint8_t *)malloc(size ? size : 1);
        if (m_data && size)
            memcpy(m_data, data, size);
    }

    ~SynthBuffer()
    {
        free(m_data);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv)
    {
        if (!ppv)
            return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(ID3DXBuffer)) {
            *ppv = static_cast<ID3DXBuffer *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef(void)
    {
        return (ULONG)InterlockedIncrement(&m_refs);
    }

    ULONG STDMETHODCALLTYPE Release(void)
    {
        LONG n = InterlockedDecrement(&m_refs);
        if (n == 0)
            delete this;
        return (ULONG)n;
    }

    LPVOID STDMETHODCALLTYPE GetBufferPointer(void)
    {
        return m_data;
    }

    DWORD STDMETHODCALLTYPE GetBufferSize(void)
    {
        return m_size;
    }
};

static HRESULT make_synth_buffer(const SdvxShaderBlob *blob, ID3DXBuffer **out)
{
    SynthBuffer *buf;
    if (!out || !blob || !blob->data || !blob->size)
        return E_FAIL;
    buf = new (std::nothrow) SynthBuffer(blob->data, blob->size);
    if (!buf || !buf->GetBufferPointer()) {
        delete buf;
        return E_OUTOFMEMORY;
    }
    *out = buf;
    return S_OK;
}

static HRESULT apply_blob(
    const SdvxShaderBlob *blob,
    ID3DXBuffer **ppShader,
    ID3DXBuffer **ppErrorMsgs,
    ID3DXConstantTable **ppConstantTable,
    const char *tag)
{
    HRESULT syn;

    if (ppShader && *ppShader) {
        (*ppShader)->Release();
        *ppShader = NULL;
    }
    if (ppErrorMsgs && *ppErrorMsgs) {
        (*ppErrorMsgs)->Release();
        *ppErrorMsgs = NULL;
    }
    if (ppConstantTable && *ppConstantTable) {
        ((IUnknown *)*ppConstantTable)->Release();
        *ppConstantTable = NULL;
    }

    syn = make_synth_buffer(blob, ppShader);
    if (FAILED(syn)) {
        log_msg("  %s FAIL name=%s syn=0x%08X", tag, blob->name, (unsigned)syn);
        return syn;
    }
    if (ppConstantTable && g_get_ctable && *ppShader) {
        HRESULT ct = safe_get_ctable(
            (CONST DWORD *)(*ppShader)->GetBufferPointer(), ppConstantTable);
        log_msg(
            "  %s OK name=%s cso=%u ctab=0x%08X ctab_ptr=%p",
            tag, blob->name, (unsigned)blob->size, (unsigned)ct,
            (void *)(*ppConstantTable));
    } else {
        log_msg("  %s OK name=%s cso=%u", tag, blob->name, (unsigned)blob->size);
    }
    return S_OK;
}

static HRESULT WINAPI detour_compile(
    LPCSTR pSrcData,
    UINT SrcDataLen,
    CONST D3DXMACRO *pDefines,
    ID3DXInclude *pInclude,
    LPCSTR pFunctionName,
    LPCSTR pProfile,
    DWORD Flags,
    ID3DXBuffer **ppShader,
    ID3DXBuffer **ppErrorMsgs,
    ID3DXConstantTable **ppConstantTable)
{
    HRESULT hr = safe_orig_compile(
        pSrcData, SrcDataLen, pDefines, pInclude, pFunctionName, pProfile, Flags,
        ppShader, ppErrorMsgs, ppConstantTable);

    const SdvxShaderBlob *blob = find_blob(pSrcData, SrcDataLen, pFunctionName, pProfile);
    if (g_log_verbose || (blob && g_fallback) || FAILED(hr)) {
        log_msg(
            "D3DXCompileShader profile=%s entry=%s src_len=%u hr=0x%08X blob=%s",
            pProfile ? pProfile : "(null)",
            pFunctionName ? pFunctionName : "(null)",
            (unsigned)SrcDataLen,
            (unsigned)hr,
            blob ? blob->name : "-");
    }

    if (!g_fallback || !blob || !ppShader)
        return hr;

    if (FAILED(hr)) {
        if (SUCCEEDED(apply_blob(blob, ppShader, ppErrorMsgs, ppConstantTable, "fallback")))
            hr = S_OK;
    } else if (g_force_fxc) {
        /* Wine may compile VS OK while PS falls back to fxc — mismatched
         * compilers break interpolators. Prefer our fxc blob when present. */
        ID3DXBuffer *wine_shader = *ppShader;
        ID3DXBuffer *wine_err = (ppErrorMsgs) ? *ppErrorMsgs : NULL;
        ID3DXConstantTable *wine_ct = (ppConstantTable) ? *ppConstantTable : NULL;
        *ppShader = NULL;
        if (ppErrorMsgs)
            *ppErrorMsgs = NULL;
        if (ppConstantTable)
            *ppConstantTable = NULL;
        if (SUCCEEDED(apply_blob(blob, ppShader, ppErrorMsgs, ppConstantTable, "force_fxc"))) {
            if (wine_shader)
                wine_shader->Release();
            if (wine_err)
                wine_err->Release();
            if (wine_ct)
                ((IUnknown *)wine_ct)->Release();
        } else {
            *ppShader = wine_shader;
            if (ppErrorMsgs)
                *ppErrorMsgs = wine_err;
            if (ppConstantTable)
                *ppConstantTable = wine_ct;
        }
    }
    return hr;
}

/* Length-bounded needle search — D3DX sources need not be NUL-terminated. */
static int src_contains(LPCVOID pSrcData, UINT src_len, const char *needle)
{
    const char *hay;
    size_t nlen;
    UINT i;
    if (!pSrcData || !needle || src_len == 0)
        return 0;
    nlen = strlen(needle);
    if (nlen == 0 || nlen > src_len)
        return 0;
    hay = (const char *)pSrcData;
    for (i = 0; i + (UINT)nlen <= src_len; i++) {
        if (memcmp(hay + i, needle, nlen) == 0)
            return 1;
    }
    return 0;
}

/* Cubism LApp CreateEffect sources:
 *  - Normal-only: src_len 760 (Nabla / post-2019 Normal FX).
 *  - Full multi-technique (SetupMask+Normal+Masked*): 3130 (early) or 4157 (Exceed).
 * Wine fails these compiles; we feed prebuilt .fxo. */
static int is_cubism_fx(LPCVOID pSrcData, UINT SrcDataLen)
{
    if (!src_contains(pSrcData, SrcDataLen, "ShaderNames_Normal"))
        return 0;
    if (SrcDataLen == CUBISM_NORMAL_FX_SRC_LEN)
        return 1;
    if (src_contains(pSrcData, SrcDataLen, "ShaderNames_SetupMask"))
        return 1;
    return 0;
}

static void cubism_fxo_select(
    LPCVOID pSrcData, UINT src_len, const uint8_t **fxo, UINT *fxo_size)
{
    int has_setup = src_contains(pSrcData, src_len, "ShaderNames_SetupMask");
    int has_inverted = src_contains(pSrcData, src_len, "ShaderNames_NormalMaskedInverted");

    if (has_setup) {
        if (has_inverted || src_len == CUBISM_FULL_FX_SRC_LEN_4157) {
            *fxo = k_cubism_full_4157_fxo;
            *fxo_size = CUBISM_FULL_FXO_4157_SIZE;
        } else {
            *fxo = k_cubism_full_3130_fxo;
            *fxo_size = CUBISM_FULL_FXO_3130_SIZE;
        }
        return;
    }
    *fxo = k_cubism_normal_fxo;
    *fxo_size = CUBISM_NORMAL_FXO_SIZE;
}

static HRESULT WINAPI detour_create_effect(
    void *pDevice,
    LPCVOID pSrcData,
    UINT SrcDataLen,
    CONST D3DXMACRO *pDefines,
    ID3DXInclude *pInclude,
    DWORD Flags,
    void *pPool,
    void **ppEffect,
    ID3DXBuffer **ppCompilationErrors)
{
    HRESULT hr;
    int cubism = is_cubism_fx(pSrcData, SrcDataLen);
    const uint8_t *fxo = NULL;
    UINT fxo_size = 0;
    int has_setup = src_contains(pSrcData, SrcDataLen, "ShaderNames_SetupMask");

    if (g_log_verbose) {
        log_msg(
            "D3DXCreateEffect enter src_len=%u cubism=%d setup=%d",
            (unsigned)SrcDataLen, cubism, has_setup);
    }

    if (cubism)
        cubism_fxo_select(pSrcData, SrcDataLen, &fxo, &fxo_size);

    if (cubism && g_fallback && g_force_fxc && fxo) {
        clear_create_effect_outs(ppEffect, ppCompilationErrors);
        hr = safe_orig_create_effect(
            pDevice, fxo, fxo_size, pDefines, pInclude,
            Flags, pPool, ppEffect, ppCompilationErrors);
        log_msg(
            "D3DXCreateEffect Cubism src_len=%u fxo=%u hr=0x%08X effect=%p setup=%d",
            (unsigned)SrcDataLen, (unsigned)fxo_size, (unsigned)hr,
            (ppEffect ? *ppEffect : NULL), has_setup);
        if (SUCCEEDED(hr))
            return hr;
        log_msg("  Cubism fxo failed, falling through to Wine/source");
        clear_create_effect_outs(ppEffect, ppCompilationErrors);
    }

    hr = safe_orig_create_effect(
        pDevice, pSrcData, SrcDataLen, pDefines, pInclude, Flags, pPool, ppEffect, ppCompilationErrors);

    if (g_log_verbose || cubism || FAILED(hr)) {
        log_msg(
            "D3DXCreateEffect source src_len=%u hr=0x%08X effect=%p cubism=%d setup=%d",
            (unsigned)SrcDataLen, (unsigned)hr, (ppEffect ? *ppEffect : NULL), cubism, has_setup);
    }

    if (cubism && FAILED(hr) && g_fallback && fxo) {
        clear_create_effect_outs(ppEffect, ppCompilationErrors);
        HRESULT hr2 = safe_orig_create_effect(
            pDevice, fxo, fxo_size, pDefines, pInclude,
            Flags, pPool, ppEffect, ppCompilationErrors);
        log_msg(
            "  Cubism CreateEffect fallback fxo=%u hr=0x%08X effect=%p",
            (unsigned)fxo_size, (unsigned)hr2, (ppEffect ? *ppEffect : NULL));
        if (SUCCEEDED(hr2))
            return hr2;
    }
    return hr;
}

static FARPROC get_export(const wchar_t *mod, const char *name)
{
    HMODULE h = GetModuleHandleW(mod);
    if (!h)
        h = LoadLibraryW(mod);
    if (!h)
        return NULL;
    return GetProcAddress(h, name);
}

static int module_directory(HMODULE mod, wchar_t *out, size_t out_n)
{
    wchar_t path[MAX_PATH];
    DWORD n;
    wchar_t *slash;
    if (!out || out_n == 0)
        return 0;
    out[0] = 0;
    if (!mod)
        return 0;
    n = GetModuleFileNameW(mod, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return 0;
    slash = wcsrchr(path, L'\\');
    if (!slash)
        return 0;
    *slash = 0;
    wcsncpy_s(out, out_n, path, _TRUNCATE);
    return out[0] != 0;
}

static HMODULE load_d3dx_from_dir(const wchar_t *dir)
{
    static const wchar_t *const names[] = {
        L"d3dx9_43.dll",
        L"d3dx9_42.dll",
        L"d3dx9_39.dll",
    };
    int i;
    if (!dir || !dir[0])
        return NULL;
    for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        wchar_t path[MAX_PATH];
        HMODULE h;
        _snwprintf_s(path, _TRUNCATE, L"%s\\%s", dir, names[i]);
        h = LoadLibraryW(path);
        if (h)
            return h;
    }
    return NULL;
}

/*
 * Prefer an already-mapped d3dx, else load from hook / exe / soundvoltex dirs
 * (official redistributable beside the game), and only then system search.
 * Wine's builtin system32 d3dx9_43 can crash under MinHook + fxc CT paths.
 */
static HMODULE find_or_load_d3dx(void)
{
    static const wchar_t *const names[] = {
        L"d3dx9_43.dll",
        L"d3dx9_42.dll",
        L"d3dx9_39.dll",
    };
    int i;
    wchar_t exe_dir[MAX_PATH];
    wchar_t sdvx_dir[MAX_PATH];
    HMODULE sdvx;

    for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        HMODULE h = GetModuleHandleW(names[i]);
        if (h)
            return h;
    }

    {
        HMODULE h = load_d3dx_from_dir(g_dir);
        if (h)
            return h;
    }

    if (module_directory(GetModuleHandleW(NULL), exe_dir, MAX_PATH)) {
        HMODULE h = load_d3dx_from_dir(exe_dir);
        if (h)
            return h;
    }

    sdvx = GetModuleHandleW(L"soundvoltex.dll");
    if (module_directory(sdvx, sdvx_dir, MAX_PATH)) {
        HMODULE h = load_d3dx_from_dir(sdvx_dir);
        if (h)
            return h;
    }

    for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        HMODULE h = LoadLibraryW(names[i]);
        if (h)
            return h;
    }
    return NULL;
}

static int wait_modules(void)
{
    const DWORD timeout_ms = 180000;
    DWORD start = GetTickCount();
    while (GetTickCount() - start < timeout_ms) {
        HMODULE mod = GetModuleHandleW(L"soundvoltex.dll");
        if (!mod) {
            Sleep(50);
            continue;
        }
        if (find_or_load_d3dx()) {
            g_base = (uintptr_t)mod;
            return 1;
        }
        Sleep(50);
    }
    return 0;
}

static void install_hooks_cleanup(void)
{
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_orig_compile = NULL;
    g_orig_create_effect = NULL;
    g_get_ctable = NULL;
}

static int install_hooks(void)
{
    MH_STATUS st;
    FARPROC p;
    const wchar_t *d3dx_name = L"d3dx9_43.dll";

    if (!GetModuleHandleW(L"d3dx9_43.dll")) {
        if (GetModuleHandleW(L"d3dx9_42.dll"))
            d3dx_name = L"d3dx9_42.dll";
        else if (GetModuleHandleW(L"d3dx9_39.dll"))
            d3dx_name = L"d3dx9_39.dll";
    }

    st = MH_Initialize();
    if (st != MH_OK) {
        log_msg("MH_Initialize: %s", MH_StatusToString(st));
        return 1;
    }

    p = get_export(d3dx_name, "D3DXCompileShader");
    if (!p) {
        log_msg("D3DXCompileShader not found in %ls", d3dx_name);
        install_hooks_cleanup();
        return 1;
    }
    st = MH_CreateHook((LPVOID)p, (LPVOID)detour_compile, (LPVOID *)&g_orig_compile);
    if (st != MH_OK) {
        log_msg("MH_CreateHook D3DXCompileShader: %s", MH_StatusToString(st));
        install_hooks_cleanup();
        return 1;
    }
    log_msg("hooked %ls!D3DXCompileShader @ %p", d3dx_name, (void *)p);

    g_get_ctable = (D3DXGetShaderConstantTable_t)get_export(d3dx_name, "D3DXGetShaderConstantTable");
    if (g_get_ctable)
        log_msg("resolved D3DXGetShaderConstantTable @ %p", (void *)g_get_ctable);
    else
        log_msg("D3DXGetShaderConstantTable missing");

    p = get_export(d3dx_name, "D3DXCreateEffect");
    if (!p) {
        log_msg("D3DXCreateEffect not found in %ls", d3dx_name);
        install_hooks_cleanup();
        return 1;
    }
    st = MH_CreateHook((LPVOID)p, (LPVOID)detour_create_effect, (LPVOID *)&g_orig_create_effect);
    if (st != MH_OK) {
        log_msg("MH_CreateHook D3DXCreateEffect: %s", MH_StatusToString(st));
        install_hooks_cleanup();
        return 1;
    }
    log_msg("hooked %ls!D3DXCreateEffect @ %p", d3dx_name, (void *)p);

    st = MH_EnableHook(MH_ALL_HOOKS);
    if (st != MH_OK) {
        log_msg("MH_EnableHook: %s", MH_StatusToString(st));
        install_hooks_cleanup();
        return 1;
    }
    return 0;
}

static DWORD WINAPI init_thread(LPVOID)
{
    InitializeCriticalSection(&g_log_cs);
    init_paths();
    g_log_enabled = ini_int(L"log", 1);
    g_log_verbose = ini_int(L"log_verbose", 0);
    g_enabled = ini_int(L"enabled", 1);
    g_fallback = ini_int(L"fallback", 1);
    g_force_fxc = ini_int(L"force_fxc", 1);
    open_log();

    log_msg(
        "sdvx_shader_fix starting (enabled=%d fallback=%d force_fxc=%d log_verbose=%d blobs=%d)",
        g_enabled, g_fallback, g_force_fxc, g_log_verbose, SDVX_SHADER_BLOB_COUNT);

    if (!wait_modules()) {
        log_msg("timeout waiting for soundvoltex.dll / d3dx9");
        return 1;
    }
    log_msg("soundvoltex at %p", (void *)g_base);
    {
        HMODULE d3dx = find_or_load_d3dx();
        wchar_t d3dx_path[MAX_PATH];
        if (d3dx && GetModuleFileNameW(d3dx, d3dx_path, MAX_PATH))
            log_msg("d3dx module %p path=%ls", (void *)d3dx, d3dx_path);
        else
            log_msg("d3dx module present but path unknown");
    }

    if (!g_enabled) {
        log_msg("disabled by ini");
        return 0;
    }

    if (install_hooks() != 0)
        return 1;

    log_msg("hooks enabled");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = mod;
        DisableThreadLibraryCalls(mod);
        HANDLE t = CreateThread(NULL, 0, init_thread, NULL, 0, NULL);
        if (t)
            CloseHandle(t);
    }
    return TRUE;
}
