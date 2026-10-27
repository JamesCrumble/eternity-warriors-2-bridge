/*
 * ew2host - native Windows host for the ARM32 guest (work in progress, see docs/NATIVE_WINDOWS.md).
 *
 * What exists: the dynarmic A32 binding (cpu.cpp) and the guest address space (mem.c). This file is the start-up
 * and a self-test of exactly those two pieces: it runs small ARM programs in the JIT, checks registers, memory,
 * VFP/Neon, SVC delivery and the fault report for an unmapped page, and measures the JIT's speed.
 *
 *   ew2host.exe              run the self-test
 *   ew2host.exe --selftest   the same
 *
 * What does not exist yet (each is a module of host.h): ELF loader, threads/futex, signals, the Linux system call
 * table, the file-system mapping. Without them the game cannot start.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "host.h"

int g_verbose;

void hlog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

_Noreturn void hfatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("fatal: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

/* ---- the self-test: one core, no threads ---------------------------------------------------------------------- */
static cpu_t *g_c;
static uint32_t g_svc_imm = 0xffffffffu, g_svc_r7;

void threads_invalidate_code(uint32_t addr, uint32_t len)
{
    if (g_c)
        cpu_invalidate(g_c, addr, len);
}

void host_svc(void *opaque, uint32_t imm)
{
    (void)opaque;
    g_svc_imm = imm;
    g_svc_r7 = cpu_regs(g_c)[7];
    cpu_halt(g_c);
}

/* Assembled from the listing in tools/selftest.s (arm-linux-gnueabihf-as -mcpu=cortex-a15). */
static const uint32_t kProg[] = { 0xe3a00007u, 0xe3a01006u, 0xe0020190u, 0xe5832000u, 0xef000042u, 0xe5940000u, 0xe12fff1eu, 0xe30e0100u, 0xe34005f5u, 0xe2500001u, 0x1afffffdu, 0xef000043u, 0xe3a00101u, 0xe3001000u, 0xe3441040u, 0xee000a10u, 0xee001a90u, 0xee201a20u, 0xee112a10u, 0xeea00b10u, 0xeea21b10u, 0xf2804050u, 0xf2004d52u, 0xf4034a8fu, 0xef000044u, 0xe3a07014u, 0xef000000u, 0xef000045u };
enum { OFF_BASIC = 0x00, OFF_LOOP = 0x1c, OFF_FP = 0x30, OFF_SYSCALL = 0x64 };

#define CODE 0x10000u
#define DATA 0x20000u
#define HOLE 0x30000u   /* never mapped */

static int g_failed;
static void check(int ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        g_failed++;
}

static uint32_t run_at(uint32_t off)
{
    uint32_t *r = cpu_regs(g_c);
    g_svc_imm = 0xffffffffu;
    r[15] = CODE + off;
    cpu_set_cpsr(g_c, 0x10);   /* user mode, ARM state */
    return cpu_run(g_c);
}

static int selftest(void)
{
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    printf("ew2host self-test (dynarmic A32 on Windows x64)\n");

    if (mem_init() != 0)
        hfatal("cannot reserve the 4 GiB guest address space");
    cpu_global_init(g_page_table, 1);
    g_c = cpu_create(0, NULL);
    if (!g_c)
        hfatal("cpu_create failed");
    check(mem_map_fixed(CODE, 0x1000, GPROT_READ | GPROT_WRITE | GPROT_EXEC) == 0, "map code page");
    check(mem_map_fixed(DATA, 0x1000, GPROT_READ | GPROT_WRITE) == 0, "map data page");
    memcpy(G(CODE), kProg, sizeof kProg);
    mem_code_changed(CODE, sizeof kProg);

    uint32_t *r = cpu_regs(g_c);
    r[3] = DATA;
    r[4] = HOLE;

    uint32_t h = run_at(OFF_BASIC);
    check((h & CPU_HALT_USER) && g_svc_imm == 0x42, "svc #0x42 reaches the host");
    check(r[2] == 42 && *(uint32_t *)G(DATA) == 42, "7*6 in a register and stored to guest memory");

    h = run_at(OFF_BASIC + 5 * 4);   /* continue at the load from the unmapped page */
    cpu_fault_t fl = cpu_last_fault(g_c);
    check((h & CPU_HALT_MEMORY_ABORT) && fl.kind == CPU_FAULT_READ && fl.addr == HOLE,
          "read of an unmapped page is reported with its address");

    r[3] = DATA;
    h = run_at(OFF_FP);
    float lanes[4];
    memcpy(lanes, G(DATA), sizeof lanes);
    check((h & CPU_HALT_USER) && g_svc_imm == 0x44, "VFP/Neon program ran to its svc");
    check(r[2] == 0x40c00000u, "VFP: 2.0f * 3.0f == 6.0f");
    check(lanes[0] == 6.0f && lanes[1] == 6.0f && lanes[2] == 6.0f && lanes[3] == 6.0f, "Neon: vmla.f32 q on four lanes");

    r[7] = 0;
    h = run_at(OFF_SYSCALL);
    check((h & CPU_HALT_USER) && g_svc_imm == 0 && g_svc_r7 == 20, "Linux-style 'svc 0' with r7 = 20 (getpid) is seen");
    h = run_at(OFF_SYSCALL + 2 * 4);
    check(g_svc_imm == 0x45, "execution resumes after the system call");

    QueryPerformanceCounter(&t0);
    h = run_at(OFF_LOOP);
    QueryPerformanceCounter(&t1);
    double s = (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
    check((h & CPU_HALT_USER) && g_svc_imm == 0x43 && r[0] == 0, "100M-iteration loop completes");
    printf("  JIT speed on this loop: %.2f s for 200M guest instructions = %.0f MIPS\n", s, 200.0 / s);

    printf(g_failed ? "SELF-TEST FAILED (%d)\n" : "SELF-TEST PASSED\n", g_failed);
    return g_failed ? 1 : 0;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--selftest") == 0)
            continue;
        fprintf(stderr, "ew2host: only --selftest exists so far; the game cannot be started natively yet (docs/NATIVE_WINDOWS.md)\n");
        return 2;
    }
    return selftest();
}
