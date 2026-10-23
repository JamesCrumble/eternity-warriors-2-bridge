/*
 * Eternity Warriors 3 (Unity 4.5.2f1, Mono, armeabi-v7a) loader - entry point.
 *
 * On Android the Java class com.unity3d.player.UnityPlayer drives the engine:
 * it loads libmain.so, which loads libunity.so (which in turn dlopens
 * libmono.so), and then calls the natives libunity registered from its
 * JNI_OnLoad. There is no Java here, so this file plays UnityPlayer. The call
 * order is transcribed from the game's own classes.dex (UnityPlayer.<init> and
 * the UnityMain thread); Unity 4.5 differs from 5.x in the signatures:
 *
 *     nativeFile(apk path)            the engine reads assets/bin/Data out of the zip
 *     nativeFile(obb path)            "useObb" in the activity's meta-data
 *     initJni(context)
 *     nativeInitWWW(WWW.class)
 *     nativeSetDefaultDisplay(0)
 *     nativeRecreateGfxState(surface)
 *     nativeResize(w, h, w, h)
 *     nativeResume, nativeFocusChanged(true)
 *     nativeRender()                  once per frame; presents through eglSwapBuffers
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>

#include <pthread.h>

#include <SDL2/SDL.h>

#include "so_util.h"
#include "khronos/gles2.h"

#include "jni.h"
#include "dyn.h"

#include "app_exit.h"
#include "crash.h"
#include "fps_unlock.h"
#include "fix_path.h"
#include "gl_bridge.h"
#include "port_env.h"
#include "pc_prof.h"
#include "port_version.h"
#include "syscall_prof.h"
#include "trace.h"
#include "unity_java.h"
#include "control.h"

extern "C" void android_egl_init(SDL_Window *window, SDL_GLContext gl);
extern "C" void android_platform_init(SDL_Window *win, int w, int h);
extern "C" void viewport_scale_init(int phys_w, int phys_h);
extern "C" long android_egl_frames(void);
extern "C" void crash_report_thread_init(void);
extern "C" void android_egl_set_swap_hook(void (*hook)(long frame));
extern "C" void *eglGetDisplay(void *display_id);
extern "C" unsigned int eglSwapBuffers(void *dpy, void *surface);
void fmod_audio_start(JNIEnv *env);

static const char *kNativeLibDir = "lib/armeabi-v7a";

static int kWidth  = 1280;
static int kHeight = 720;

static so_module *g_unity = NULL;

/* Kept for the inherited pthread/crash helpers, which name one module. */
so_module *ew3_module(void) { return g_unity; }

static int report_unresolved_symbols(so_module *mod)
{
    int missing = 0;

    for (int i = 0; i < mod->num_dynsym; i++) {
        Elf_Sym *sym = &mod->dynsym[i];
        if (sym->st_shndx != SHN_UNDEF)
            continue;

        const char *name = mod->dynstr + sym->st_name;
        if (!name || !*name)
            continue;

        if (so_resolve_link(mod, name))
            continue;

        if (ELF32_ST_BIND(sym->st_info) == STB_WEAK)
            continue;

        fprintf(stderr, "unresolved symbol: %s (%s)\n", name, mod->soname ? mod->soname : "?");
        missing++;
    }

    fflush(stderr);
    return missing;
}

/*
 * Android's NDK emits calls to the kernel helper page (0xffff0fc0 __kuser_cmpxchg,
 * 0xffff0fa0 __kuser_memory_barrier) via literal pool pointers. Replacing those
 * literal-pool entries with a normal executable page holding inline ldrex/strex
 * and dmb ish avoids kernel-page helper jumps on every atomic operation.
 */
static void install_fast_cas(so_module *mod)
{
    if (!port_getenv_long("FAST_CAS", 1))
        return;
    static uint32_t *s_cas = NULL;
    static uint32_t *s_dmb = NULL;
    if (!s_cas) {
        static const uint32_t kCode[] = {
            /* __kuser_cmpxchg: int (int oldval, int newval, volatile int *ptr) */
            0xf57ff05bu,   /* 0: dmb ish              */
            0xe1923f9fu,   /* 1: ldrex r3, [r2]       */
            0xe0533000u,   /* 2: subs  r3, r3, r0     */
            0x01823f91u,   /* 3: strexeq r3, r1, [r2] */
            0x03330001u,   /* 4: teqeq r3, #1         */
            0x0afffff9u,   /* 5: beq   1b             */
            0xf57ff05bu,   /* 6: dmb ish              */
            0xe2730000u,   /* 7: rsbs  r0, r3, #0     */
            0xe12fff1eu,   /* 8: bx    lr             */
            /* __kuser_memory_barrier: void (void) */
            0xf57ff05bu,   /* 9: dmb ish              */
            0xe12fff1eu,   /* 10: bx   lr             */
        };
        void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED)
            return;
        memcpy(page, kCode, sizeof(kCode));
        __builtin___clear_cache((char *)page, (char *)page + sizeof(kCode));
        mprotect(page, 4096, PROT_READ | PROT_EXEC);
        s_cas = (uint32_t *)page;
        s_dmb = s_cas + 9;
    }
    mprotect((void *)mod->text_base, mod->text_size, PROT_READ | PROT_WRITE | PROT_EXEC);
    uint32_t *w = (uint32_t *)mod->text_base;
    size_t n = mod->text_size / 4;
    int patched_cas = 0, patched_dmb = 0;
    for (size_t i = 0; i < n; i++) {
        if (w[i] == 0xffff0fc0u) {
            w[i] = (uint32_t)(uintptr_t)s_cas;
            patched_cas++;
        } else if (w[i] == 0xffff0fa0u) {
            w[i] = (uint32_t)(uintptr_t)s_dmb;
            patched_dmb++;
        }
    }
    __builtin___clear_cache((char *)mod->text_base, (char *)mod->text_base + mod->text_size);
    if (patched_cas || patched_dmb)
        trace("fast_cas (%s): %d __kuser_cmpxchg -> %p, %d __kuser_memory_barrier -> %p",
              mod->soname ? mod->soname : "?", patched_cas, (void *)s_cas, patched_dmb, (void *)s_dmb);
}

static bool insn_is_vcmp(uint32_t insn)
{
    return (insn & 0xffb00f70u) == 0xeeb00a40u && (((insn >> 16) & 0xfu) == 4u || ((insn >> 16) & 0xfu) == 5u);
}

static bool insn_sets_apsr(uint32_t insn)
{
    if (insn == 0xeef1fa10u)
        return true;
    if ((insn & 0x0e000000u) == 0x0a000000u)
        return true;
    uint32_t op = insn & 0x0fb00000u;
    if (op == 0x03100000u || op == 0x03300000u || op == 0x03500000u || op == 0x03700000u ||
        op == 0x01100000u || op == 0x01300000u || op == 0x01500000u || op == 0x01700000u)
        return true;
    if ((insn & 0x0c100000u) == 0x00100000u)
        return true;
    return false;
}

static bool insn_touches_vfp_reg(uint32_t insn, uint32_t s_reg)
{
    uint32_t cp = insn & 0x0f000e00u;
    if (cp == 0x0e000a00u || cp == 0x0d000a00u || cp == 0x0c000a00u ||
        cp == 0x0e000b00u || cp == 0x0d000b00u || cp == 0x0c000b00u) {
        uint32_t d = (insn >> 22) & 1u;
        uint32_t vd = (insn >> 12) & 0xfu;
        if (((vd << 1) | d) == s_reg && !insn_is_vcmp(insn))
            return true;
    }
    return false;
}

/*
 * Unity 4.x's inline Abs(float v) is written as `v < 0.0F ? -v : v`, which NDK GCC
 * compiles into `vcmpz.f32 Sd; vmrs APSR_nzcv, fpscr; vnegmi.f32 Sd, Sd`. Under QEMU
 * TCG each such sequence calls softfloat comparison helpers and splits the JIT block.
 * Upgrade verified dataflow-safe triples in-place to unconditional `vabs.f32 Sd, Sd`.
 */
static void install_fast_vabs(so_module *mod)
{
    uint32_t *w = (uint32_t *)mod->text_base;
    size_t n = mod->text_size / 4;
    int patched = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 8; i + 16 < n; i++) {
            uint32_t insn = w[i];
            if ((insn & 0xffbf0fc0u) != 0x4eb10a40u)
                continue;
            uint32_t d = (insn >> 22) & 1u, vd = (insn >> 12) & 0xfu;
            uint32_t m = (insn >> 5) & 1u,  vm = insn & 0xfu;
            if (d != m || vd != vm)
                continue;
            uint32_t s_reg = (vd << 1) | d;
            uint32_t vcmp = 0xeeb50ac0u | (d << 22) | (vd << 12);
            size_t vmrs_idx = 0;
            for (size_t j = 1; j <= 4; j++) {
                uint32_t wi = w[i - j];
                if (wi == 0xeef1fa10u) {
                    vmrs_idx = i - j;
                    break;
                }
                if ((wi >> 28) != 0xeu || insn_sets_apsr(wi) || insn_touches_vfp_reg(wi, s_reg))
                    break;
            }
            if (!vmrs_idx)
                continue;
            size_t vcmp_idx = 0;
            for (size_t j = 1; j <= 4 && vmrs_idx >= j; j++) {
                uint32_t wj = w[vmrs_idx - j];
                if (wj == vcmp) {
                    vcmp_idx = vmrs_idx - j;
                    break;
                }
                if (insn_is_vcmp(wj) || (wj >> 28) != 0xeu || insn_sets_apsr(wj) || insn_touches_vfp_reg(wj, s_reg))
                    break;
            }
            if (!vcmp_idx)
                continue;
            bool ok_fwd = false;
            for (size_t k = 1; k < 16; k++) {
                uint32_t wk = w[i + k];
                if ((wk >> 28) != 0xeu) {
                    ok_fwd = false;
                    break;
                }
                if (insn_sets_apsr(wk) || wk == 0xe12fff1eu || (wk & 0xffff8000u) == 0xe8bd8000u) {
                    ok_fwd = true;
                    break;
                }
            }
            if (ok_fwd) {
                w[vcmp_idx] = 0xe320f000u; /* nop */
                w[vmrs_idx] = 0xe320f000u; /* nop */
                w[i] = 0xeeb00ac0u | (d << 22) | (vd << 12) | (d << 5) | vd; /* vabs.f32 Sd, Sd */
                patched++;
            }
        }
    }
    if (patched > 0) {
        __builtin___clear_cache((char *)mod->text_base, (char *)mod->text_base + mod->text_size);
        trace("fast_vabs (%s): upgraded %d vcmpz+vmrs+vnegmi sequences to vabs.f32",
              mod->soname ? mod->soname : "?", patched);
    }
}

/*
 * The loader's post-relocation hook, once per module (libmain, libunity, and
 * later libmono from inside dlopen). Missing imports are listed here, before
 * any of the module's code runs; they are fatal unless EW3_ALLOW_UNRESOLVED
 * is set, in which case the first call to one aborts with its PLT stub.
 */
extern "C" int so_after_relocate(so_module *mod)
{
    const char *name = mod->soname ? mod->soname : "module";
    trace("module loaded: %s text=%p size=%zu", name, (void *)mod->text_base, (size_t)mod->text_size);
    install_fast_cas(mod);

    if (mod->soname && strcmp(mod->soname, "libunity.so") == 0) {
        g_unity = mod;
        crash_report_init(mod, "libunity.so");
        /*
         * Fix Unity 4.5.3f3 CachedRawStream bugs when loading uncompressed
         * UnityRaw bundles via WWW.LoadFromCacheOrDownload:
         * 1) CachedRawStream::vtable[5] (file +0x901994, vaddr +0x902994) had
         *    0x3d5eb8 (GetType() == 2) instead of 0x3d5ed8 (GetType() == 3).
         * 2) CachedRawStream::Finish (+0x3dbbf4) called 0x3dad3c (which re-runs
         *    ReadDirectory on the overwritten 256 KB scratch buffer) instead of
         *    0x3da1fc (CheckCRC).
         */
        {
            uintptr_t vslot = mod->text_base + 0x902994;
            uintptr_t call_at = mod->text_base + 0x3dbbf4;
            if (*(uint32_t *)vslot == (uint32_t)(mod->text_base + 0x3d5eb8) &&
                *(uint32_t *)call_at == 0xebfffc50u) {
                uintptr_t vpage = vslot & ~(uintptr_t)0xfff;
                mprotect((void *)vpage, 0x1000, PROT_READ | PROT_WRITE);
                *(volatile uint32_t *)vslot = (uint32_t)(mod->text_base + 0x3d5ed8);
                uintptr_t cpage = call_at & ~(uintptr_t)0xfff;
                mprotect((void *)cpage, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);
                *(volatile uint32_t *)call_at = 0xebfff980u; /* bl 0x3da1fc */
                __builtin___clear_cache((char *)call_at, (char *)call_at + 4);
                trace("patched libunity.so CachedRawStream (vslot=%p, call=%p)", (void *)vslot, (void *)call_at);
            }
        }
        /*
         * Fix Unity 4.5 Android timer & per-draw-call profiler bottlenecks:
         * 1) GetTimeSinceStartup (+0x3ddd3c) checks s_eglGetSystemTimeFrequencyNV
         *    without a "tried" guard, so on non-Tegra GPUs it calls
         *    eglGetProcAddress("eglGetSystemTimeFrequencyNV") and
         *    eglGetProcAddress("eglGetSystemTimeNV") on every single call
         *    before falling back to +0x25e2e8 (clock_gettime). Patch +0x3ddd3c
         *    to branch directly to +0x25e2e8 (0xeafa0169).
         * 2) GfxDeviceGLES20::DrawElements (+0x3af5f0, +0x3af634) and
         *    GfxDeviceGLES20::DrawArrays (+0x3b10f0, +0x3b1178) call
         *    gettimeofday@plt before and after every draw call just to accumulate
         *    editor profiler drawCallTime stats. NOP those 4 calls.
         */
        {
            uintptr_t nv_timer = mod->text_base + 0x3ddd3c;
            if (*(uint32_t *)nv_timer == 0xe59f30e0u) {
                *(volatile uint32_t *)nv_timer = 0xeafa0169u; /* b 0x25e2e8 */
                __builtin___clear_cache((char *)nv_timer, (char *)nv_timer + 4);
            }
            static const uint32_t kDrawTimerOffsets[] = {
                0x3af5f0u, 0x3af634u, 0x3b10f0u, 0x3b1178u
            };
            for (uint32_t off : kDrawTimerOffsets) {
                uintptr_t at = mod->text_base + off;
                if ((*(uint32_t *)at & 0xff000000u) == 0xeb000000u) {
                    *(volatile uint32_t *)at = 0xe320f000u; /* nop */
                    __builtin___clear_cache((char *)at, (char *)at + 4);
                }
            }
            /*
             * 3) Under QEMU TCG, JobScheduler (+0x35ba00) spawning 3 UnityWorker
             *    threads causes ~6 cross-vCPU futex (sem_post/sem_wait) wakeups
             *    and IPIs every frame for tiny skinning/culling jobs. Running
             *    JobScheduler inline on UnityMain (numThreads = 0) eliminates
             *    all JobScheduler futex syscalls while keeping UnityPreload and
             *    FMOD audio on separate vCPUs.
             */
            if (port_getenv_long("JOB_THREADS", 0) == 0) {
                uintptr_t js = mod->text_base + 0x35ba00;
                if (*(uint32_t *)js == 0xe2451001u) { /* sub r1, r5, #1 */
                    *(volatile uint32_t *)js = 0xe3a01000u; /* mov r1, #0 */
                    __builtin___clear_cache((char *)js, (char *)js + 4);
                }
            }
            /*
             * 4) IntersectAABBFrustum (+0x208538, #1 leaf hotspot in libunity.so at 12.1%
             *    of engine thread CPU time) computes |nx|, |ny|, |nz| using three
             *    vcmpz.f32 + vmrs APSR_nzcv, fpscr + vnegmi.f32 sequences per plane,
             *    plus vcmpz.f32 + vmrs at the end of the loop. Under QEMU TCG each
             *    vcmpz+vmrs+vnegmi calls softfloat helpers and splits the JIT block.
             *    Replace +0x20858c..0x2085bc with unconditional vabs.f32 and an integer
             *    sign-bit test (vmov r12, s15; cmp r12, #0; bpl +0x208560).
             */
            {
                uintptr_t fn = mod->text_base + 0x20858c;
                if (*(uint32_t *)fn == 0xeef56ac0u && *(uint32_t *)(fn + 0x40) == 0x5affffe3u) {
                    static const uint32_t kFastFrustum[] = {
                        0xeef06ae6u, /* vabs.f32 s13, s13        */
                        0xeeb06ac6u, /* vabs.f32 s12, s12        */
                        0xeeb07ac7u, /* vabs.f32 s14, s14        */
                        0xee775aa5u, /* vadd.f32 s11, s15, s11   */
                        0xee667a05u, /* vmul.f32 s15, s12, s10   */
                        0xee467aa4u, /* vmla.f32 s15, s13, s9    */
                        0xee477a22u, /* vmla.f32 s15, s14, s5    */
                        0xee757aa7u, /* vadd.f32 s15, s11, s15   */
                        0xee17ca90u, /* vmov     r12, s15        */
                        0xe35c0000u, /* cmp      r12, #0         */
                        0x5affffe9u, /* bpl      0x208560        */
                        0xe3a00000u, /* mov      r0, #0          */
                        0xe12fff1eu, /* bx       lr              */
                    };
                    memcpy((void *)fn, kFastFrustum, sizeof(kFastFrustum));
                    __builtin___clear_cache((char *)fn, (char *)fn + sizeof(kFastFrustum));
                }
            }
            install_fast_vabs(mod);
            /*
             * Unity 4.2.1f4 GetTimeInNanoseconds (+0x2bb248) is called 24 times per
             * frame inside PlayerLoop (+0x2c9420) to populate the engine's subsystem
             * timer table at +0xa21278. Replace its gettimeofday@plt + 64-bit multiply
             * sequence with a 5-instruction CNTVCT hardware counter read (62.5 MHz << 4).
             */
            {
                uintptr_t fn = mod->text_base + 0x2bb248;
                if (*(uint32_t *)fn == 0xe52de004u && *(uint32_t *)(fn + 4) == 0xe24dd014u) {
                    static const uint32_t kFastTimerNs[] = {
                        0xec510f1eu, /* mrrc p15, 1, r0, r1, c14 */
                        0xe1a01201u, /* lsl  r1, r1, #4          */
                        0xe1811e20u, /* orr  r1, r1, r0, lsr #28 */
                        0xe1a00200u, /* lsl  r0, r0, #4          */
                        0xe12fff1eu, /* bx   lr                  */
                    };
                    uintptr_t page = fn & ~(uintptr_t)0xfff;
                    mprotect((void *)page, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);
                    memcpy((void *)fn, kFastTimerNs, sizeof(kFastTimerNs));
                    __builtin___clear_cache((char *)fn, (char *)fn + sizeof(kFastTimerNs));
                    trace("patched libunity.so GetTimeInNanoseconds (+0x2bb248) -> CNTVCT");
                }
            }
            trace("patched libunity.so GetTimeSinceStartup (+0x3ddd3c), per-draw gettimeofday, JobScheduler, and IntersectAABBFrustum (+0x20858c)");
        }
        /* Bring-up aid: EW3_TRAP_AT=<hex offset> plants an undefined instruction
         * there, so the crash report shows the registers and memory at that point. */
        for (const char *t = getenv("EW3_TRAP_AT"); t && *t;) {   /* comma-separated */
            char *end;
            uintptr_t at = mod->text_base + strtoul(t, &end, 16);
            uintptr_t page = at & ~(uintptr_t)0xfff;
            mprotect((void *)page, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);
            *(volatile uint32_t *)at = 0xe7f000f0;   /* udf #0 */
            __builtin___clear_cache((char *)at, (char *)at + 4);
            trace("trap planted at libunity.so+0x%lx", (unsigned long)(at - mod->text_base));
            t = *end == ',' ? end + 1 : end + strlen(end);
        }
    }

    int missing = report_unresolved_symbols(mod);
    if (missing == 0)
        return 0;

    if (port_getenv_long("ALLOW_UNRESOLVED", 0)) {
        trace("%d import(s) of %s have no implementation; continuing as asked", missing, name);
        return 0;
    }
    fatal("%d import(s) of %s have no implementation (listed above).", missing, name);
    return 1;
}

static bool find_obb(const char *dir, char *out, size_t outsz)
{
    DIR *d = opendir(dir);
    if (!d)
        return false;
    bool found = false;
    /* readdir64: under a 32-bit qemu-user the plain call fails with EOVERFLOW on
     * filesystems that hand out 64-bit directory offsets. */
    struct dirent64 *e;
    while ((e = readdir64(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n > 4 && strcmp(e->d_name + n - 4, ".obb") == 0 && strncmp(e->d_name, "main.", 5) == 0) {
            snprintf(out, outsz, "%s/%s", dir, e->d_name);
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

/*
 * Bring-up aid for runs on the PC: EW3_SHOT_EVERY=N writes every Nth
 * presented frame to ctl/shot_<frame>.bmp, read back just before the swap.
 */
static long g_shot_every = 0;

/*
 * EW3_FB_SHM=<file>: every presented frame is copied into a shared file
 * (header: 'EW3F', width, height, frame counter; then RGBA rows bottom-up), for
 * tools/ew3view.c to show in a native window - the way to see a software-
 * rendered run on a desktop whose window system the guest's SDL cannot reach.
 */
struct FbShm { uint32_t magic, w, h, seq; };
static FbShm *g_fb = NULL;
static void fb_shm_init()
{
    const char *path = port_getenv("FB_SHM");
    if (!path || !*path)
        return;
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    size_t size = sizeof(FbShm) + (size_t)kWidth * kHeight * 4;
    if (fd < 0 || ftruncate(fd, (off_t)size) != 0) {
        trace("FB_SHM: cannot create %s", path);
        return;
    }
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED)
        return;
    g_fb = (FbShm *)p;
    g_fb->w = (uint32_t)kWidth;
    g_fb->h = (uint32_t)kHeight;
    g_fb->seq = 0;
    __atomic_store_n(&g_fb->magic, 0x46335745u, __ATOMIC_RELEASE);   /* "EW3F" */
    trace("FB_SHM: frames go to %s (%dx%d)", path, kWidth, kHeight);
}

static void swap_hook(long frame)
{
    if (g_fb && !gl_bridge_enabled()) {
        static void (*read_pixels)(int, int, int, int, unsigned, unsigned, void *) =
            (decltype(read_pixels))SDL_GL_GetProcAddress("glReadPixels");
        if (read_pixels) {
            read_pixels(0, 0, kWidth, kHeight, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, g_fb + 1);
            __atomic_store_n(&g_fb->seq, (uint32_t)frame, __ATOMIC_RELEASE);
        }
    }
    if (!g_shot_every || frame % g_shot_every != 0 || gl_bridge_enabled())
        return;
    void (*read_pixels)(int, int, int, int, unsigned, unsigned, void *) =
        (decltype(read_pixels))SDL_GL_GetProcAddress("glReadPixels");
    if (!read_pixels)
        return;
    const int w = kWidth, h = kHeight;
    unsigned char *px = (unsigned char *)malloc((size_t)w * h * 4);
    if (!px)
        return;
    read_pixels(0, 0, w, h, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, px);
    char path[64];
    mkdir("ctl", 0755);
    snprintf(path, sizeof(path), "ctl/shot_%06ld.bmp", frame);
    FILE *f = fopen(path, "wb");
    if (f) {
        unsigned size = 54 + (unsigned)w * h * 3;
        unsigned char hdr[54] = {'B', 'M'};
        memcpy(hdr + 2, &size, 4);
        hdr[10] = 54; hdr[14] = 40;
        memcpy(hdr + 18, &w, 4);
        memcpy(hdr + 22, &h, 4);   /* positive height: bottom-up, as GL returns it */
        hdr[26] = 1; hdr[28] = 24;
        fwrite(hdr, 1, 54, f);
        unsigned char *row = (unsigned char *)malloc((size_t)w * 3);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                row[x * 3 + 0] = px[(y * w + x) * 4 + 2];
                row[x * 3 + 1] = px[(y * w + x) * 4 + 1];
                row[x * 3 + 2] = px[(y * w + x) * 4 + 0];
            }
            fwrite(row, 1, (size_t)w * 3, f);
        }
        free(row);
        fclose(f);
        trace("screenshot %s", path);
    }
    free(px);
}

template <typename F> static F native(const char *name)
{
    void *p = dyn_native("com/unity3d/player/UnityPlayer", name);
    if (!p)
        trace("UnityPlayer.%s was not registered by libunity", name);
    return (F)p;
}

static int port_main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (argc >= 2 && strcmp(argv[1], "--version") == 0) {
        printf("%s\n", EW3_PORT_VERSION);
        return 0;
    }

    trace("Eternity Warriors 2 port v%s", EW3_PORT_VERSION);

    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <game-directory>\n"
                "\n"
                "The directory holds your own copy of the game:\n"
                "  lib/armeabi-v7a/{libmain,libunity,libmono}.so\n"
                "  base.apk   (the original APK; the engine reads its assets from it)\n"
                "  main.*.obb (the expansion file)\n",
                argv[0]);
        return 2;
    }

    char game_dir[PATH_MAX];
    if (!realpath(argv[1], game_dir)) {
        fatal("'%s': %s", argv[1], strerror(errno));
        return 1;
    }
    io_set_game_dir(game_dir);
    {
        char sub[PATH_MAX];
        snprintf(sub, sizeof(sub), "%s/files", game_dir);
        mkdir(sub, 0755);
        snprintf(sub, sizeof(sub), "%s/shared_prefs", game_dir);
        mkdir(sub, 0755);
    }

    {
        long w = port_getenv_long("WIDTH", kWidth), h = port_getenv_long("HEIGHT", kHeight);
        if (w >= 320 && w <= 4096 && h >= 240 && h <= 4096) {
            kWidth = (int)w;
            kHeight = (int)h;
        }
    }

    char lib_dir[PATH_MAX], apk_path[PATH_MAX], obb_path[PATH_MAX] = "";
    snprintf(lib_dir, sizeof(lib_dir), "%s/%s", game_dir, kNativeLibDir);
    snprintf(apk_path, sizeof(apk_path), "%s/base.apk", game_dir);

    struct stat st;
    if (stat(apk_path, &st) != 0) {
        fatal("'%s' does not exist.", apk_path);
        return 1;
    }
    /* The game's own script looks for the expansion file where Android keeps it,
     * relative to "external storage" - which is the game directory here. */
    char obb_dir[PATH_MAX];
    snprintf(obb_dir, sizeof(obb_dir), "%s/Android/obb/com.glu.ewarriors2", game_dir);
    if (!find_obb(obb_dir, obb_path, sizeof(obb_path)) && !find_obb(game_dir, obb_path, sizeof(obb_path)))
        trace("no main.*.obb in %s - the engine will only see the APK", game_dir);

    /* GL before the modules: the import tables are filled from the live context. */
    SDL_Window   *window = NULL;
    SDL_GLContext gl     = NULL;

    if (gl_bridge_enabled()) {
        /* GL runs in the Android app; the window and the context are only tokens here. */
        if (SDL_Init(SDL_INIT_VIDEO) != 0)
            trace("SDL_Init(video) failed: %s", SDL_GetError());
        window = SDL_CreateWindow("Eternity Warriors 2", 0, 0, kWidth, kHeight, 0);
        if (!gl_bridge_init()) {
            fatal("could not connect to the GL host.");
            return 1;
        }
        gl = (SDL_GLContext)(uintptr_t)1;
    } else if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        trace("SDL_Init(video) failed: %s", SDL_GetError());
    } else {
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        window = SDL_CreateWindow("Eternity Warriors 2", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  kWidth, kHeight, SDL_WINDOW_OPENGL);
        if (!window)
            trace("SDL_CreateWindow failed: %s", SDL_GetError());
        else if (!(gl = SDL_GL_CreateContext(window)))
            trace("no GLES 2.0 context: %s", SDL_GetError());
    }

    {
        /* The inherited viewport remap is for a fixed-size engine; Unity renders at
         * whatever the surface reports, so logical == physical. */
        char dim[16];
        snprintf(dim, sizeof(dim), "%d", kWidth);
        setenv("EW3_SCREEN_W", dim, 1);
        snprintf(dim, sizeof(dim), "%d", kHeight);
        setenv("EW3_SCREEN_H", dim, 1);
    }
    if (gl) {
        load_gles1_funcs();
        load_gles2_funcs();
        viewport_scale_init(kWidth, kHeight);
        if (!gl_bridge_enabled()) {
            const GLubyte *(*get_string)(GLenum) =
                (const GLubyte *(*)(GLenum))SDL_GL_GetProcAddress("glGetString");
            if (get_string)
                trace("GL_VERSION=%s | GL_RENDERER=%s", (const char *)get_string(0x1F02),
                      (const char *)get_string(0x1F01));
            /* The engine makes the context current itself, possibly on another thread. */
            SDL_GL_MakeCurrent(window, NULL);
        }
    }
    g_shot_every = port_getenv_long("SHOT_EVERY", 0);
    fb_shm_init();
    android_egl_set_swap_hook(swap_hook);
    android_egl_init(window, gl);
    android_platform_init(window, kWidth, kHeight);

    JavaVM *vm  = NULL;
    JNIEnv *env = NULL;
    if (JNI_CreateJavaVM(&vm, &env, NULL) != JNI_OK || !vm || !env) {
        fatal("could not create the JNI environment.");
        return 1;
    }

    UnityJavaConfig cfg;
    cfg.game_dir = game_dir;
    cfg.apk_path = apk_path;
    cfg.obb_path = obb_path;
    cfg.width = kWidth;
    cfg.height = kHeight;
    unity_java_init(env, cfg);
    control_init();

    so_set_options(NULL, lib_dir);

    /* libunity.so names libmain.so in DT_NEEDED, so this maps both. */
    so_module *unity = so_load_module("libunity.so", NULL, NULL);
    if (!unity) {
        fatal("could not load libunity.so from '%s'.", lib_dir);
        fflush(NULL);
        _exit(1);
    }

    auto JNI_OnLoad = (int (*)(JavaVM *, void *))so_symbol(unity, "JNI_OnLoad");
    if (!JNI_OnLoad) {
        fatal("libunity.so exports no JNI_OnLoad.");
        _exit(1);
    }
    trace("-> libunity JNI_OnLoad");
    int ver = JNI_OnLoad(vm, NULL);
    trace("<- JNI_OnLoad = 0x%x", ver);

    typedef jobject self_t;
    auto initJni         = native<void (*)(JNIEnv *, self_t)>("initJni");
    auto nativeFile      = native<void (*)(JNIEnv *, self_t, jstring)>("nativeFile");
    auto nativeInitWWW   = native<void (*)(JNIEnv *, jclass, jclass)>("nativeInitWWW");
    auto nativeDisplay   = native<void (*)(JNIEnv *, self_t, jobject)>("nativeSetDefaultDisplay");
    auto nativeInit      = native<void (*)(JNIEnv *, self_t, jint, jint)>("nativeInit");
    auto nativeGfx       = native<void (*)(JNIEnv *, self_t)>("nativeRecreateGfxState");
    auto nativeResize    = native<void (*)(JNIEnv *, self_t, jint, jint, jint, jint)>("nativeResize");
    auto unityAndroidInit = native<jboolean (*)(JNIEnv *, self_t, jstring, jstring)>("unityAndroidInit");
    auto unityAndroidPrepareGameLoop = native<void (*)(JNIEnv *, self_t)>("unityAndroidPrepareGameLoop");
    auto nativeResume    = native<void (*)(JNIEnv *, self_t)>("nativeResume");
    auto nativeFocus     = native<void (*)(JNIEnv *, self_t, jboolean)>("nativeFocusChanged");
    auto nativeRender    = native<jboolean (*)(JNIEnv *, self_t)>("nativeRender");
    auto initPlayerPrefs = (void (*)(JNIEnv *, jclass))dyn_native("com/unity3d/player/PlayerPrefs", "InitPlayerPrefs");

    if (!initJni || !nativeFile || !nativeGfx || !nativeResume || !nativeRender) {
        fatal("libunity.so did not register the UnityPlayer natives the port drives.");
        fflush(NULL);
        _exit(1);
    }

    jobject player  = unity_java_player();

    trace("-> nativeFile %s", apk_path);
    nativeFile(env, player, env->NewStringUTF(apk_path));
    if (obb_path[0]) {
        trace("-> nativeFile %s", obb_path);
        nativeFile(env, player, env->NewStringUTF(obb_path));
    }
    trace("-> initJni");
    initJni(env, player);
    if (initPlayerPrefs) {
        trace("-> PlayerPrefs.InitPlayerPrefs");
        initPlayerPrefs(env, (jclass)dyn_new("com/unity3d/player/PlayerPrefs"));
    }
    if (nativeInitWWW)
        nativeInitWWW(env, (jclass)dyn_class("com/unity3d/player/UnityPlayer"), (jclass)dyn_class("com/unity3d/player/WWW"));
    if (nativeDisplay)
        nativeDisplay(env, player, unity_java_display());
    if (nativeInit) {
        trace("-> nativeInit(%d, %d)", kWidth, kHeight);
        nativeInit(env, player, kWidth, kHeight);
    }

    trace("-> nativeRecreateGfxState");
    nativeGfx(env, player);
    if (nativeResize)
        nativeResize(env, player, kWidth, kHeight, kWidth, kHeight);

    /* Unity 4.2.1f4 runs on Android GLSurfaceView, which calls eglSwapBuffers
     * after every onDrawFrame() -> nativeRender(). */
    trace("-> splash nativeRender");
    gl_bridge_profile_step(1);
    nativeRender(env, player);
    if (android_egl_frames() == 0)
        eglSwapBuffers(eglGetDisplay(NULL), (void *)(uintptr_t)0xE6157FAC);
    gl_bridge_profile_step(0);

    if (unityAndroidInit) {
        trace("-> unityAndroidInit(assets/bin/, %s)", lib_dir);
        jboolean ok = unityAndroidInit(env, player, env->NewStringUTF("assets/bin/"), env->NewStringUTF(lib_dir));
        trace("<- unityAndroidInit = %d", (int)ok);
    }
    if (nativeResize)
        nativeResize(env, player, kWidth, kHeight, kWidth, kHeight);
    if (unityAndroidPrepareGameLoop) {
        trace("-> unityAndroidPrepareGameLoop");
        unityAndroidPrepareGameLoop(env, player);
    }

    trace("-> nativeResume");
    nativeResume(env, player);
    /* UnityPlayer.resume() starts the FMOD audio device in Unity 4.x (jni/fmod_audio.cpp). */
    if (port_getenv_long("FMOD_DEVICE", 1))
        fmod_audio_start(env);
    if (nativeFocus)
        nativeFocus(env, player, 1);

    const long frame_limit = port_getenv_long("FRAME_LIMIT", 0);
    const bool use_bridge = gl_bridge_enabled() != 0;
    long frames = 0;
    while (frame_limit == 0 || frames < frame_limit) {
        if (!use_bridge) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT)
                    goto done;
                control_sdl_event(env, &ev, kWidth, kHeight);
            }
        }
        if (android_app_exit_requested())
            goto done;

        if (!control_tick(env, frames))
            goto done;
        unity_java_frame(env, frames);
        fps_unlock_tick(frames);
        sysprof_tick(frames);
        pc_prof_tick(frames);

        if (frames < 5)
            trace("-> nativeRender #%ld", frames + 1);
        long prev_presented = android_egl_frames();
        gl_bridge_profile_step(1);
        jboolean alive = nativeRender(env, player);
        if (alive && android_egl_frames() == prev_presented)
            eglSwapBuffers(eglGetDisplay(NULL), (void *)(uintptr_t)0xE6157FAC);
        gl_bridge_profile_step(0);
        frames++;
        if (frames <= 5 || frames % 100 == 0)
            trace("frames=%ld presented=%ld", frames, android_egl_frames());
        if (!alive) {
            trace("nativeRender returned false - the engine asked to quit");
            break;
        }
    }

done:
    control_shutdown(frames);
    trace("run finished: %ld frames, %ld presented", frames, android_egl_frames());
    fflush(NULL);
    _exit(0);
}

/*
 * The engine does not run on the process's initial thread. On Android it lives
 * on the "UnityMain" Java thread; here it gets a thread of its own with a stack
 * sized for the engine plus the host code underneath every libc and JNI call.
 * On the initial thread under qemu-user the same start-up ran out of stack about
 * one launch in two.
 */
struct MainArgs { int argc; char **argv; int rc; };
static void *main_thread(void *p)
{
    MainArgs *a = (MainArgs *)p;
    crash_report_thread_init();
    {
        pthread_attr_t at;
        void *base = NULL;
        size_t size = 0;
        if (pthread_getattr_np(pthread_self(), &at) == 0) {
            pthread_attr_getstack(&at, &base, &size);
            pthread_attr_destroy(&at);
        }
        int marker = 0;
        trace("engine thread stack %p..%p (%zu KiB), sp near %p", base, (char *)base + size, size / 1024, (void *)&marker);
    }
    a->rc = port_main(a->argc, a->argv);
    return NULL;
}

int main(int argc, char **argv)
{
    MainArgs args = {argc, argv, 1};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u * 1024 * 1024);
    pthread_t t;
    if (pthread_create(&t, &attr, main_thread, &args) != 0)
        return port_main(argc, argv);
    pthread_join(t, NULL);
    return args.rc;
}
