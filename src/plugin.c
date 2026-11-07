/*
 * Budokai3Access loader. Built two ways (build.sh):
 *  - out/plugins/Budokai3Access.dll (-DREX_MOD_PLUGIN): a code plugin for the
 *    port's own plugin loader, port 1.5.6 and later (rex_mod_init below);
 *  - out/rexgpu-access.dll: for older ports, a wrapper around the port's
 *    graphics plugin, described next.
 *
 * dbz3.exe always loads its graphics plugin rexgpu-xenos.dll from the game
 * folder (the name is built into the executable). The installer renames the
 * port's file to rexgpu-xenos-original.dll and puts this one in its place.
 * Both plugin functions go to the original (export forwarders in
 * plugin.def); when the runtime creates its graphics system this wrapper
 * also starts Budokai3Access\Budokai3Access.dll. Nothing runs at DLL load.
 *
 * For development, if Budokai3Access\devreload.txt exists, the loader watches
 * Budokai3Access.dll and swaps in a rebuilt copy while the game keeps running.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>

typedef void (*core_fn)(void);
static wchar_t g_dir[MAX_PATH];
static volatile LONG g_started;
#ifndef REX_MOD_PLUGIN
static void *p_create;                     /* rexgpu-xenos-original.dll!rex_gpu_create */
#endif

static FILETIME mtime(const wchar_t *p) {
    WIN32_FILE_ATTRIBUTE_DATA a; FILETIME z = { 0, 0 };
    return GetFileAttributesExW(p, GetFileExInfoStandard, &a) ? a.ftLastWriteTime : z;
}

static HMODULE load_core(int dev, unsigned n) {
    wchar_t src[MAX_PATH], run[MAX_PATH];
    swprintf(src, MAX_PATH, L"%lsBudokai3Access.dll", g_dir);
    if (!dev) return LoadLibraryW(src);
    /* Load a copy so the build can overwrite Budokai3Access.dll. */
    swprintf(run, MAX_PATH, L"%lsBudokai3Access_run%u.dll", g_dir, n);
    if (!CopyFileW(src, run, FALSE)) return NULL;
    return LoadLibraryW(run);
}

static DWORD WINAPI loader_thread(LPVOID unused) {
    (void)unused;
    wchar_t src[MAX_PATH], flag[MAX_PATH], old[MAX_PATH], logp[MAX_PATH];
    GetModuleFileNameW(NULL, g_dir, MAX_PATH);
    wchar_t *slash = wcsrchr(g_dir, L'\\');
    if (slash) slash[1] = 0;
    wcscat(g_dir, L"Budokai3Access\\");
    swprintf(src, MAX_PATH, L"%lsBudokai3Access.dll", g_dir);
    swprintf(flag, MAX_PATH, L"%lsdevreload.txt", g_dir);
    swprintf(logp, MAX_PATH, L"%lsBudokai3Access.log", g_dir);
    int dev = GetFileAttributesW(flag) != INVALID_FILE_ATTRIBUTES;
    DeleteFileW(logp);
    for (unsigned i = 0; i < 64; i++) { swprintf(old, MAX_PATH, L"%lsBudokai3Access_run%u.dll", g_dir, i); DeleteFileW(old); }
    unsigned n = 0;
    HMODULE core = load_core(dev, n);
    if (!core) return 0;
    core_fn start = (core_fn)GetProcAddress(core, "core_start");
    if (start) start();
    if (!dev) return 0;
    FILETIME t = mtime(src);
    for (;;) {
        Sleep(1000);
        FILETIME t2 = mtime(src);
        if (CompareFileTime(&t, &t2) == 0) continue;
        Sleep(500);                       /* let the build finish writing */
        t = mtime(src);
        core_fn stop = (core_fn)GetProcAddress(core, "core_stop");
        if (stop) stop();
        FreeLibrary(core);
        n = (n + 1) % 64;
        core = load_core(dev, n);
        if (!core) continue;
        start = (core_fn)GetProcAddress(core, "core_start");
        if (start) start();
    }
    return 0;
}

/* Called from rex_gpu_create or rex_mod_init (below). */
void access_start(void) {
    if (InterlockedExchange(&g_started, 1)) return;
    HANDLE h = CreateThread(NULL, 0, loader_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

#ifdef REX_MOD_PLUGIN
/*
 * Port 1.5.6 and later: the port's own plugin loader (issue #23). The game
 * loads plugins\*.dll when dbz3_plugins = true and calls rex_mod_init once,
 * before the game's first instruction. Built as
 * out/plugins/Budokai3Access.dll; the port's graphics plugin is left alone.
 */
#include <stdint.h>
typedef struct RexModHost {
    uint32_t struct_size;
    uint32_t api_version;
    const char *port_version;
    const char *region;
    uint8_t *guest_base;
    void *(*get_function)(uint32_t guest_address);
    int (*set_function)(uint32_t guest_address, void *func);
    void (*log)(const char *plugin, const char *message);
} RexModHost;

__declspec(dllexport) void rex_mod_init(const RexModHost *host) {
    if (host && host->struct_size >= sizeof(RexModHost) && host->log) {
        char msg[200];
        snprintf(msg, sizeof msg, "Budokai 3 Access loader started (port %s, region %s, guest base %p)",
                 host->port_version ? host->port_version : "?", host->region ? host->region : "?",
                 (void *)host->guest_base);
        host->log("Budokai3Access", msg);
        /* The mod reads US addresses at the fixed guest base 0x100000000. */
        if (host->region && strcmp(host->region, "us") != 0)
            host->log("Budokai3Access", "this is not the US game; the mod only knows US addresses and may not work");
        if ((uintptr_t)host->guest_base != 0x100000000ULL)
            host->log("Budokai3Access", "unexpected guest memory base; the mod's self check will switch it off if needed");
    }
    access_start();
}
#else

void *access_real_create(void) {
    if (!p_create) {
        HMODULE x = GetModuleHandleW(L"rexgpu-xenos-original.dll");
        if (!x) x = LoadLibraryW(L"rexgpu-xenos-original.dll");
        p_create = x ? (void *)GetProcAddress(x, "rex_gpu_create") : NULL;
    }
    return p_create;
}

/* rex_gpu_create: start the mod, then continue into the real factory with
 * the caller's arguments untouched (the signature does not matter here). */
__asm__(
    ".globl rex_gpu_create\n"
    "rex_gpu_create:\n"
    "\tpush %rcx\n\tpush %rdx\n\tpush %r8\n\tpush %r9\n"
    "\tsub $0x28, %rsp\n"
    "\tcall access_start\n"
    "\tcall access_real_create\n"
    "\tadd $0x28, %rsp\n"
    "\tpop %r9\n\tpop %r8\n\tpop %rdx\n\tpop %rcx\n"
    "\ttest %rax, %rax\n"
    "\tjz 1f\n"
    "\tjmp *%rax\n"
    "1:\txor %eax, %eax\n\tret\n");
#endif

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}
