/*
 * Signals, across the bionic/glibc layout difference.
 *
 * On 32-bit ARM bionic's sigset_t is one unsigned long and its struct sigaction
 * is 16 bytes; glibc's sigset_t is 128 bytes and sits in the middle of struct
 * sigaction. Bound straight through, sigemptyset() alone writes 124 bytes past
 * the caller's variable. libil2cpp.so needs these to work for real: its
 * collector stops the world by signalling every thread and parking it in
 * sigsuspend().
 *
 * Handlers for the fatal signals are not installed: Unity registers its own
 * crash handler for them, which would replace the port's crash report with a
 * silent exit.
 */
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "so_util.h"
#include "thunk_gen.h"
#include "trace.h"

extern "C" {

/*
 * Called by the port's crash handler before it reports. Returns true when Mono
 * had a handler for the signal and it returned - i.e. it took the fault and
 * rewrote the context to resume somewhere (the exception throw path).
 */
bool signal_guest_handler(int sig, siginfo_t *si, void *ucontext);

int  bionic_setjmp(void *env);
void bionic_longjmp(void *env, int value);

typedef uint32_t bionic_sigset_t;

struct bionic_sigaction {
    void *handler;
    bionic_sigset_t mask;
    int flags;
    void *restorer;
};

static void to_host(const bionic_sigset_t *in, sigset_t *out)
{
    sigemptyset(out);
    for (int s = 1; s <= 32; s++)
        if (*in & (1u << (s - 1)))
            sigaddset(out, s);
}
static void to_bionic(const sigset_t *in, bionic_sigset_t *out)
{
    *out = 0;
    for (int s = 1; s <= 32; s++)
        if (sigismember(in, s) == 1)
            *out |= 1u << (s - 1);
}
static bool is_fatal(int sig)
{
    return sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE || sig == SIGABRT ||
           sig == SIGTRAP || sig == SIGSTKFLT;
}

static int s_sigemptyset(bionic_sigset_t *s) { *s = 0; return 0; }
static int s_sigfillset(bionic_sigset_t *s) { *s = ~0u; return 0; }
static int s_sigaddset(bionic_sigset_t *s, int sig) { if (sig < 1 || sig > 32) { errno = EINVAL; return -1; } *s |= 1u << (sig - 1); return 0; }
static int s_sigdelset(bionic_sigset_t *s, int sig) { if (sig < 1 || sig > 32) { errno = EINVAL; return -1; } *s &= ~(1u << (sig - 1)); return 0; }
static int s_sigismember(const bionic_sigset_t *s, int sig) { if (sig < 1 || sig > 32) return 0; return (*s >> (sig - 1)) & 1; }

static int mask_common(int (*fn)(int, const sigset_t *, sigset_t *), int how, const bionic_sigset_t *set,
                       bionic_sigset_t *old)
{
    sigset_t hs, ho;
    if (set)
        to_host(set, &hs);
    int r = fn(how, set ? &hs : NULL, old ? &ho : NULL);
    if (old && r == 0)
        to_bionic(&ho, old);
    return r;
}
static int s_sigprocmask(int how, const bionic_sigset_t *set, bionic_sigset_t *old)
{
    return mask_common(sigprocmask, how, set, old);
}
static int s_pthread_sigmask(int how, const bionic_sigset_t *set, bionic_sigset_t *old)
{
    return mask_common(pthread_sigmask, how, set, old);
}
static int s_sigsuspend(const bionic_sigset_t *mask)
{
    sigset_t hs;
    to_host(mask, &hs);
    return sigsuspend(&hs);
}
static int s_sigpending(bionic_sigset_t *set)
{
    sigset_t hs;
    int r = sigpending(&hs);
    if (r == 0)
        to_bionic(&hs, set);
    return r;
}

/*
 * Mono is the exception to "no guest handlers for fatal signals": a null
 * dereference in JIT-compiled C# is a SIGSEGV that Mono's handler turns into a
 * NullReferenceException and resumes from. So a handler that lives in
 * libmono.so is remembered, and the port's crash handler offers it the fault
 * first (signal_guest_handler below). Everyone else's - Unity's crash handler
 * in particular - is still ignored.
 */
static void *g_mono_handler[65];
static int   g_mono_flags[65];

static bool is_mono_code(void *fn)
{
    so_module *m = so_module_for_address((uintptr_t)fn);
    return m && m->soname && strcmp(m->soname, "libmono.so") == 0;
}

static int s_sigaction(int sig, const struct bionic_sigaction *act, struct bionic_sigaction *old)
{
    if (is_fatal(sig)) {
        if (old) {
            memset(old, 0, sizeof(*old));
            if (sig > 0 && sig < 65 && g_mono_handler[sig]) {
                old->handler = g_mono_handler[sig];
                old->flags = g_mono_flags[sig];
            }
        }
        if (act && sig > 0 && sig < 65 && act->handler != (void *)SIG_DFL && act->handler != (void *)SIG_IGN &&
            is_mono_code(act->handler)) {
            g_mono_handler[sig] = act->handler;
            g_mono_flags[sig] = act->flags;
            trace("sigaction(%d): Mono's handler %p will get faults first", sig, act->handler);
        }
        return 0;
    }
    struct sigaction ha, ho;
    if (act) {
        memset(&ha, 0, sizeof(ha));
        ha.sa_handler = (void (*)(int))act->handler;
        to_host(&act->mask, &ha.sa_mask);
        ha.sa_flags = act->flags & ~0x04000000; /* SA_RESTORER */
    }
    int r = sigaction(sig, act ? &ha : NULL, old ? &ho : NULL);
    if (old && r == 0) {
        old->handler = (void *)ho.sa_handler;
        to_bionic(&ho.sa_mask, &old->mask);
        old->flags = ho.sa_flags;
        old->restorer = NULL;
    }
    return r;
}
static void *s_bsd_signal(int sig, void *handler)
{
    if (is_fatal(sig))
        return NULL;
    return (void *)signal(sig, (void (*)(int))handler);
}
static int s_tkill(int tid, int sig) { return (int)syscall(SYS_tkill, tid, sig); }

/*
 * bionic's pthread_cleanup_push/pop are macros over these two, and the handlers
 * must also run when the thread leaves through pthread_exit(). Mono's Boehm GC
 * depends on it: GC_start_routine pushes GC_thread_exit_proc, and a thread that
 * exits without it stays registered - the next collection scans a stack that no
 * longer exists (SIGSEGV in GC_push_all_stack). So the frames are chained per
 * thread and s_pthread_exit unwinds the chain first.
 */
#include <setjmp.h>

__thread jmp_buf t_thread_exit_jmp;
__thread void *t_thread_exit_val = NULL;
__thread int t_has_thread_exit_jmp = 0;

struct bionic_cleanup { struct bionic_cleanup *prev; void (*routine)(void *); void *arg; };
static __thread struct bionic_cleanup *t_cleanup;
static void s_cleanup_push(struct bionic_cleanup *c, void (*routine)(void *), void *arg)
{
    c->prev = t_cleanup;
    c->routine = routine;
    c->arg = arg;
    t_cleanup = c;
}
static void s_cleanup_pop(struct bionic_cleanup *c, int execute)
{
    t_cleanup = c->prev;
    if (execute && c->routine)
        c->routine(c->arg);
}
static void s_pthread_exit(void *value)
{
    while (struct bionic_cleanup *c = t_cleanup) {
        t_cleanup = c->prev;
        if (c->routine)
            c->routine(c->arg);
    }
    if (t_has_thread_exit_jmp) {
        t_has_thread_exit_jmp = 0;
        t_thread_exit_val = value;
        longjmp(t_thread_exit_jmp, 1);
    }
    pthread_exit(value);
}

static double s_cosh(double x) { return cosh(x); }
static double s_sinh(double x) { return sinh(x); }
static double s_tanh(double x) { return tanh(x); }

extern char **environ;

} /* extern "C" */

DynLibFunction symtable_signal[] = {
    THUNK_SPECIFIC("sigemptyset", s_sigemptyset),
    THUNK_SPECIFIC("sigfillset", s_sigfillset),
    THUNK_SPECIFIC("sigaddset", s_sigaddset),
    THUNK_SPECIFIC("sigdelset", s_sigdelset),
    THUNK_SPECIFIC("sigismember", s_sigismember),
    THUNK_SPECIFIC("sigprocmask", s_sigprocmask),
    THUNK_SPECIFIC("pthread_sigmask", s_pthread_sigmask),
    THUNK_SPECIFIC("sigsuspend", s_sigsuspend),
    THUNK_SPECIFIC("sigpending", s_sigpending),
    THUNK_SPECIFIC("sigaction", s_sigaction),
    THUNK_SPECIFIC("bsd_signal", s_bsd_signal),
    THUNK_SPECIFIC("signal", s_bsd_signal),
    THUNK_SPECIFIC("tkill", s_tkill),
    THUNK_SPECIFIC("__pthread_cleanup_push", s_cleanup_push),
    THUNK_SPECIFIC("__pthread_cleanup_pop", s_cleanup_pop),
    THUNK_SPECIFIC("pthread_exit", s_pthread_exit),
    /* The signal mask is not saved; the engine only uses these as setjmp/longjmp. */
    NO_THUNK("sigsetjmp",  (uintptr_t)&bionic_setjmp),
    NO_THUNK("siglongjmp", (uintptr_t)&bionic_longjmp),
    NO_THUNK("_setjmp",    (uintptr_t)&bionic_setjmp),
    NO_THUNK("_longjmp",   (uintptr_t)&bionic_longjmp),
    NO_THUNK("environ",    (uintptr_t)&environ),
    THUNK_SPECIFIC("cosh", s_cosh),
    THUNK_SPECIFIC("sinh", s_sinh),
    THUNK_SPECIFIC("tanh", s_tanh),
    { NULL, 0 },
};

/* Defined after the table so it can see g_mono_handler; see the declaration above. */
extern "C" bool signal_guest_handler(int sig, siginfo_t *si, void *ucontext)
{
    if (sig <= 0 || sig >= 65 || !g_mono_handler[sig])
        return false;
    /* ucontext_t: uc_mcontext sits at the same offset under bionic and glibc on
     * ARM (it is the kernel's struct sigcontext), which is all Mono reads and writes. */
    if (g_mono_flags[sig] & SA_SIGINFO)
        ((void (*)(int, siginfo_t *, void *))g_mono_handler[sig])(sig, si, ucontext);
    else
        ((void (*)(int))g_mono_handler[sig])(sig);
    return true;
}
