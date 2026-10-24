/*
 * Guest memory access, hooks on recompiled game functions, and the
 * reverse-engineering tools used to build this mod.
 *
 * Tools are driven by writing a line to Budokai3Access\cmd.txt while the game
 * runs (the file is deleted once read); results go to Budokai3Access.log or
 * Budokai3Access\counts.txt:
 *   count arm          hook every game function with a call counter
 *   count reset        zero the counters
 *   count dump         write counters to counts.txt
 *   trace ADDR [N]     log the next N calls of guest function ADDR (hex) with r3..r8
 *   watch ADDR LEN rw|w  hardware watchpoint on guest address; logs the guest
 *                        function touching it (first 64 hits)
 *   unwatch
 *   poke ADDR VALUE    write a big-endian 32-bit value (testing only)
 *   pad BUTTONS MS     hold controller buttons, e.g. "pad lb+a 200"
 */
#include "access.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>
#include "MinHook.h"

#define NFUNC_MAX ((CODE_END - CODE_BASE) / 4)

/* VirtualQuery is slow on the guest arena (thousands of regions), so pages
 * found readable are remembered. Guest heaps are not decommitted while the
 * game runs, so a page that was readable stays readable. */
static volatile LONG g_okpages[65536 / 32];

static int page_ok(uint32_t page) {
    if (g_okpages[page >> 5] & (1u << (page & 31))) return 1;
    MEMORY_BASIC_INFORMATION mi;
    uint8_t *p = gptr(page << 16);
    if (!VirtualQuery(p, &mi, sizeof mi)) return 0;
    if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    if ((uint8_t *)mi.BaseAddress + mi.RegionSize < p + 0x10000) return 0;
    InterlockedOr(&g_okpages[page >> 5], (LONG)(1u << (page & 31)));
    return 1;
}

int guest_ok(uint32_t a, uint32_t len) {
    if (!len) return 1;
    uint32_t last = a + len - 1;
    if (last < a) return 0;
    for (uint32_t pg = a >> 16; pg <= last >> 16; pg++) if (!page_ok(pg)) return 0;
    return 1;
}
uint32_t g32(uint32_t a) { return guest_ok(a, 4) ? _byteswap_ulong(*(uint32_t *)gptr(a)) : 0; }
uint16_t g16(uint32_t a) { return guest_ok(a, 2) ? _byteswap_ushort(*(uint16_t *)gptr(a)) : 0; }
uint8_t  g8(uint32_t a)  { return guest_ok(a, 1) ? *gptr(a) : 0; }
float gf(uint32_t a) { uint32_t v = g32(a); float f; memcpy(&f, &v, 4); return f; }
void w32(uint32_t a, uint32_t v) { if (guest_ok(a, 4)) *(uint32_t *)gptr(a) = _byteswap_ulong(v); }

static void **ftable(void) { return (void **)gptr(FTABLE); }

void *guest_host_fn(uint32_t guest) {
    if (guest < CODE_BASE || guest >= CODE_END || (guest & 3)) return NULL;
    return ftable()[(guest - CODE_BASE) / 4];
}

int guest_ready(void) {
    uint32_t probe = FTABLE + (0x820deac8u - CODE_BASE) * 2;
    if (!guest_ok(FTABLE, 8) || !guest_ok(probe, 8) || !guest_ok(FTABLE + (CODE_END - CODE_BASE) * 2 - 8, 8)) return 0;
    /* The table is filled once the runtime has loaded the game module. */
    return ftable()[0] != NULL && ftable()[(0x820deac8u - CODE_BASE) / 4] != NULL;
}

/* host address -> guest function, for logs */
typedef struct { uintptr_t host; uint32_t guest; } HG;
static HG *g_hg; static int g_nhg;
static int hg_cmp(const void *a, const void *b) {
    uintptr_t x = ((const HG *)a)->host, y = ((const HG *)b)->host;
    return x < y ? -1 : x > y;
}
static void build_hg(void) {
    if (g_hg) return;
    g_hg = (HG *)malloc(sizeof(HG) * NFUNC_MAX);
    void **t = ftable();
    for (uint32_t i = 0; i < NFUNC_MAX; i++)
        if (t[i]) { g_hg[g_nhg].host = (uintptr_t)t[i]; g_hg[g_nhg].guest = CODE_BASE + i * 4; g_nhg++; }
    qsort(g_hg, g_nhg, sizeof(HG), hg_cmp);
}
uint32_t guest_fn_of_host(uintptr_t rip) {
    build_hg();
    int lo = 0, hi = g_nhg - 1, best = -1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (g_hg[m].host <= rip) { best = m; lo = m + 1; } else hi = m - 1; }
    return best >= 0 ? g_hg[best].guest : 0;
}

/* ---------- hooks used by the mod ---------- */
static int g_hook_count, g_hook_bad;
int guest_hook(uint32_t guest, void *detour, void **orig) {
    void *h = guest_host_fn(guest);
    if (!h) { log_line("hook %08x: no host function", guest); return 0; }
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(h, &mi, sizeof mi) || mi.Type != MEM_IMAGE ||
        !(mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
        log_line("hook %08x: %p is not game code", guest, h);
        g_hook_bad++;
        return 0;
    }
    MH_STATUS s = MH_CreateHook(h, detour, orig);
    if (s != MH_OK) { log_line("hook %08x: MinHook error %d", guest, (int)s); return 0; }
    g_hook_count++;
    return 1;
}
int guest_hooks_bad(void) { return g_hook_bad; }
void guest_hooks_apply(void) {
    MH_STATUS s = MH_EnableHook(MH_ALL_HOOKS);
    log_line("%d hooks enabled (%d)", g_hook_count, (int)s);
}

/* ---------- RE tools: per-function counting / tracing stubs ---------- */
static volatile LONG64 *g_counts;      /* per function index */
static volatile LONG *g_trace;         /* calls left to log */
static guest_fn *g_orig;               /* trampoline per function index */
static uint8_t *g_stubs;
static int g_armed;

static void stub_handler(PPCContext *ctx, uint8_t *base, uint64_t idx) {
    g_counts[idx]++;
    if (g_trace[idx] > 0) {
        LONG left = InterlockedDecrement(&g_trace[idx]);
        if (left >= 0) {
            uintptr_t ret = (uintptr_t)__builtin_return_address(0);
            log_line("TRACE %08x r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x r8=%08x r1=%08x from %08x",
                     CODE_BASE + (uint32_t)idx * 4, (uint32_t)ctx->r3, (uint32_t)ctx->r4, (uint32_t)ctx->r5,
                     (uint32_t)ctx->r6, (uint32_t)ctx->r7, (uint32_t)ctx->r8, (uint32_t)ctx->r1, guest_fn_of_host(ret));
        }
    }
    CALL_ORIG(g_orig[idx], ctx, base);
}

static uint8_t *make_stub(uint32_t idx) {
    uint8_t *p = g_stubs + (size_t)idx * 32;
    p[0] = 0x49; p[1] = 0xB8; uint64_t v = idx; memcpy(p + 2, &v, 8);                 /* mov r8, idx */
    p[10] = 0x48; p[11] = 0xB8; void *h = (void *)stub_handler; memcpy(p + 12, &h, 8);  /* mov rax, handler */
    p[20] = 0xFF; p[21] = 0xE0;                                                          /* jmp rax */
    return p;
}

static void tools_alloc(void) {
    if (g_stubs) return;
    g_counts = (volatile LONG64 *)calloc(NFUNC_MAX, sizeof(LONG64));
    g_trace = (volatile LONG *)calloc(NFUNC_MAX, sizeof(LONG));
    g_orig = (guest_fn *)calloc(NFUNC_MAX, sizeof(guest_fn));
    g_stubs = (uint8_t *)VirtualAlloc(NULL, (size_t)NFUNC_MAX * 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}

static int stub_hook(uint32_t idx) {
    if (g_orig[idx]) return 1;
    void *h = ftable()[idx];
    if (!h) return 0;
    if (MH_CreateHook(h, make_stub(idx), (void **)&g_orig[idx]) != MH_OK) return 0;
    return MH_EnableHook(h) == MH_OK || 1;
}

static void count_arm(void) {
    tools_alloc();
    if (g_armed) return;
    void **t = ftable();
    int n = 0, fail = 0;
    for (uint32_t i = 0; i < NFUNC_MAX; i++) {
        if (!t[i] || g_orig[i]) continue;
        if (MH_CreateHook(t[i], make_stub(i), (void **)&g_orig[i]) == MH_OK) n++; else fail++;
    }
    MH_STATUS s = MH_EnableHook(MH_ALL_HOOKS);
    g_armed = 1;
    log_line("count armed: %d functions hooked, %d failed, enable %d", n, fail, (int)s);
}

static void count_dump(void) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%lscounts.txt", g_mod_dir);
    FILE *f = _wfopen(path, L"w");
    if (!f) return;
    for (uint32_t i = 0; i < NFUNC_MAX; i++)
        if (g_counts && g_counts[i]) fprintf(f, "%08x %lld\n", CODE_BASE + i * 4, (long long)g_counts[i]);
    fclose(f);
    log_line("counts written");
}

/* ---------- hardware watchpoints ---------- */
static volatile uintptr_t g_watch_addr;
static volatile int g_watch_hits;
static int g_watch_len, g_watch_rw;

static uintptr_t g_exe_lo, g_exe_hi;

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = ep->ContextRecord;
    /* Our watchpoint uses DR0 only; swallow its traps even right after unwatch. */
    if (!(c->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    c->Dr6 = 0;
    if (!g_watch_addr) { c->Dr7 &= ~0xF0003ULL; return EXCEPTION_CONTINUE_EXECUTION; }
    uintptr_t rip = c->Rip;
    if (rip >= g_exe_lo && rip < g_exe_hi) {
        LONG n = InterlockedIncrement((volatile LONG *)&g_watch_hits);
        if (n <= 64)
            log_line("WATCH hit %d: guest fn %08x rip=%p rcx=%p rdi=%p value now %08x", (int)n, guest_fn_of_host(rip),
                     (void *)rip, (void *)c->Rcx, (void *)c->Rdi, _byteswap_ulong(*(uint32_t *)g_watch_addr));
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void set_dr_all(uintptr_t addr, int len, int rw) {
    DWORD me = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te; te.dwSize = sizeof te;
    int n = 0;
    if (Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == me) continue;
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!th) continue;
        SuspendThread(th);
        CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(th, &c)) {
            if (addr) {
                int lenbits = len == 1 ? 0 : len == 2 ? 1 : len == 8 ? 2 : 3;
                c.Dr0 = addr;
                c.Dr7 = (c.Dr7 & ~0xF0003ULL) | 1 | ((uint64_t)(rw ? 3 : 1) << 16) | ((uint64_t)lenbits << 18);
            } else { c.Dr0 = 0; c.Dr7 &= ~0xF0003ULL; }
            SetThreadContext(th, &c); n++;
        }
        ResumeThread(th); CloseHandle(th);
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
    log_line("debug registers set on %d threads", n);
}

static PVOID g_veh;

void tools_shutdown(void) {
    if (g_watch_addr) { set_dr_all(0, 0, 0); g_watch_addr = 0; }
    MH_DisableHook(MH_ALL_HOOKS);
    for (int i = 0; i < 300 && g_inflight; i++) Sleep(10);
    Sleep(100);
    MH_Uninitialize();
    if (g_veh) RemoveVectoredExceptionHandler(g_veh);
    g_veh = NULL;
}

void tools_init(void) {
    HMODULE exe = GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((uint8_t *)exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    g_exe_lo = (uintptr_t)exe; g_exe_hi = g_exe_lo + nt->OptionalHeader.SizeOfImage;
    MH_Initialize();
    g_veh = AddVectoredExceptionHandler(1, veh);
}

static void run_cmd(char *line) {
    char cmd[32] = { 0 }, a1[64] = { 0 }, a2[64] = { 0 }, a3[64] = { 0 };
    sscanf(line, "%31s %63s %63s %63s", cmd, a1, a2, a3);
    log_line("cmd: %s", line);
    if (!strcmp(cmd, "count")) {
        if (!strcmp(a1, "arm")) count_arm();
        else if (!strcmp(a1, "reset")) { if (g_counts) memset((void *)g_counts, 0, NFUNC_MAX * sizeof(LONG64)); }
        else if (!strcmp(a1, "dump")) count_dump();
    } else if (!strcmp(cmd, "trace")) {
        tools_alloc();
        uint32_t ga = (uint32_t)strtoul(a1, NULL, 16);
        if (ga < CODE_BASE || ga >= CODE_END) return;
        uint32_t idx = (ga - CODE_BASE) / 4;
        if (!stub_hook(idx)) { log_line("trace: could not hook %08x", ga); return; }
        g_trace[idx] = a2[0] ? atoi(a2) : 20;
    } else if (!strcmp(cmd, "watch")) {
        uint32_t ga = (uint32_t)strtoul(a1, NULL, 16);
        g_watch_len = a2[0] ? atoi(a2) : 4;
        g_watch_rw = !strcmp(a3, "rw");
        g_watch_hits = 0;
        g_watch_addr = (uintptr_t)gptr(ga);
        set_dr_all(g_watch_addr, g_watch_len, g_watch_rw);
    } else if (!strcmp(cmd, "poke")) {      /* poke ADDR VALUE: write a big-endian 32-bit value (testing) */
        w32((uint32_t)strtoul(a1, NULL, 16), (uint32_t)strtoul(a2, NULL, 0));
    } else if (!strcmp(cmd, "texlog")) {
        extern volatile LONG g_texlog_left;
        g_texlog_left = a1[0] ? atoi(a1) : 400;
    } else if (!strcmp(cmd, "sprlog")) {
        extern volatile LONG g_sprlog_left;
        g_sprlog_left = a1[0] ? atoi(a1) : 200;
    } else if (!strcmp(cmd, "pin")) {       /* pin ADDR VALUE MS (testing) */
        pad_pin((uint32_t)strtoul(a1, NULL, 16), (uint32_t)strtoul(a2, NULL, 0), a3[0] ? atoi(a3) : 5000);
    } else if (!strcmp(cmd, "pad")) {
        pad_inject(a1, a2[0] ? atoi(a2) : 150);
    } else if (!strcmp(cmd, "unwatch")) {
        set_dr_all(0, 0, 0);
        g_watch_addr = 0;
    }
}

void tools_poll(void) {
    static DWORD last;
    if (GetTickCount() - last < 300) return;
    last = GetTickCount();
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%lscmd.txt", g_mod_dir);
    FILE *f = _wfopen(path, L"r");
    if (!f) return;
    char line[256];
    char lines[16][256]; int n = 0;
    while (n < 16 && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0]) strcpy(lines[n++], line);
    }
    fclose(f);
    _wremove(path);
    for (int i = 0; i < n; i++) run_cmd(lines[i]);
}
