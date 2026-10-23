/*
 * ew3host: shared declarations of the C side.
 *
 * Module map:
 *   cpu.cpp     dynarmic binding (the only C++)
 *   mem.c       guest address space
 *   elf.c       loading the guest binary and its interpreter, initial stack
 *   thread.c    guest threads (one Windows thread + one core each), futex
 *   signal.c    guest signal delivery and rt_sigreturn
 *   syscall.c   the Linux ARM EABI system call table
 *   vfs.c       guest paths -> host paths, descriptors (files, pipes, sockets)
 *   main.c      start-up
 */
#pragma once
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include "cpu.h"
#include "mem.h"

/* ---- logging ---------------------------------------------------------------- */
extern int g_verbose;   /* EW3HOST_VERBOSE: 1 = syscalls that fail, 2 = every syscall */
void hlog(const char *fmt, ...);
_Noreturn void hfatal(const char *fmt, ...);

/* ---- Linux errno (negated as syscall results) ------------------------------- */
enum {
    L_EPERM = 1, L_ENOENT = 2, L_ESRCH = 3, L_EINTR = 4, L_EIO = 5, L_ENXIO = 6, L_E2BIG = 7, L_ENOEXEC = 8,
    L_EBADF = 9, L_ECHILD = 10, L_EAGAIN = 11, L_ENOMEM = 12, L_EACCES = 13, L_EFAULT = 14, L_EBUSY = 16,
    L_EEXIST = 17, L_EXDEV = 18, L_ENODEV = 19, L_ENOTDIR = 20, L_EISDIR = 21, L_EINVAL = 22, L_ENFILE = 23,
    L_EMFILE = 24, L_ENOTTY = 25, L_EFBIG = 27, L_ENOSPC = 28, L_ESPIPE = 29, L_EROFS = 30, L_EPIPE = 32,
    L_ERANGE = 34, L_ENAMETOOLONG = 36, L_ENOSYS = 38, L_ENOTEMPTY = 39, L_ELOOP = 40, L_ENODATA = 61,
    L_ETIMEDOUT = 110, L_ENOTSOCK = 88, L_EDESTADDRREQ = 89, L_EMSGSIZE = 90, L_EPROTOTYPE = 91,
    L_ENOPROTOOPT = 92, L_EPROTONOSUPPORT = 93, L_EOPNOTSUPP = 95, L_EAFNOSUPPORT = 97, L_EADDRINUSE = 98,
    L_EADDRNOTAVAIL = 99, L_ENETUNREACH = 101, L_ECONNABORTED = 103, L_ECONNRESET = 104, L_ENOBUFS = 105,
    L_EISCONN = 106, L_ENOTCONN = 107, L_ECONNREFUSED = 111, L_EHOSTUNREACH = 113, L_EALREADY = 114,
    L_EINPROGRESS = 115,
};

/* ---- threads ----------------------------------------------------------------- */
#define MAX_THREADS 256
#define NSIG_L 64

typedef struct gsigaction {
    uint32_t handler;    /* 0 = SIG_DFL, 1 = SIG_IGN */
    uint32_t flags;
    uint32_t restorer;
    uint64_t mask;
} gsigaction_t;

typedef struct gthread {
    int tid;
    int slot;                 /* index in the thread table and dynarmic core id */
    cpu_t *cpu;
    void *os_thread;          /* HANDLE */
    uint32_t clear_child_tid; /* guest address, CLONE_CHILD_CLEARTID */
    uint32_t robust_list;

    /* signals */
    uint64_t sigmask;
    volatile uint64_t pending;   /* bits 1<<(sig-1), set by any thread */
    uint32_t pending_info_addr[NSIG_L + 1];
    int pending_code[NSIG_L + 1];
    uint32_t altstack_sp, altstack_size;
    int on_altstack;
    int in_sigsuspend;
    uint64_t saved_mask;      /* restored after the handler of a sigsuspend wake */

    /* blocking: every interruptible wait parks on wake_event */
    void *wake_event;         /* HANDLE, auto-reset */
    volatile int exiting;
} gthread_t;

extern __declspec(thread) gthread_t *t_self;

int threads_init(void);
gthread_t *thread_new(void);
gthread_t *thread_by_tid(int tid);
void thread_free(gthread_t *t);
void threads_invalidate_code(uint32_t addr, uint32_t len);
/* Runs t's core until the thread exits; does not return. */
_Noreturn void thread_run(gthread_t *t);
int thread_clone(uint32_t flags, uint32_t stack, uint32_t ptid, uint32_t tls, uint32_t ctid);
_Noreturn void thread_exit(int code);
_Noreturn void process_exit(int code);

/* An interruptible sleep: returns 0 on timeout/wake, -L_EINTR when a signal is pending.
 * timeout_ms < 0: forever. */
int thread_wait(int64_t timeout_ms);
/* True when an unblocked signal is pending on the calling thread. */
int signal_pending_now(void);

int64_t sys_futex(uint32_t uaddr, int op, uint32_t val, uint32_t timeout, uint32_t uaddr2, uint32_t val3, int time64);
void futex_wake_addr(uint32_t uaddr, int count);

/* ---- signals ------------------------------------------------------------------ */
extern gsigaction_t g_sigactions[NSIG_L + 1];
/* Queue sig for t (or any thread when t is NULL) and kick it out of its wait / JIT. */
void signal_send(gthread_t *t, int sig, int code, uint32_t fault_addr);
/* Called on the thread's own run loop before re-entering the JIT. */
void signal_deliver_pending(gthread_t *t);
/* A synchronous fault (SIGSEGV/SIGBUS/SIGILL/SIGTRAP) on the current thread. */
void signal_fault(gthread_t *t, int sig, int code, uint32_t addr);
int64_t sys_rt_sigreturn(gthread_t *t);

/* ---- syscalls ------------------------------------------------------------------ */
void syscall_dispatch(gthread_t *t);

/* ---- vfs ------------------------------------------------------------------------ */
extern char g_rootfs[1024];     /* host directory that is the guest's "/" */
void vfs_init(void);
/* Guest path (relative to the guest cwd when not absolute) -> host path (UTF-8, '/' separators). */
int vfs_host_path(const char *guest, char *out, size_t outsz);
/* Adds a mount: guest prefix -> host directory. */
void vfs_mount(const char *guest_prefix, const char *host_dir);

/* ---- elf ------------------------------------------------------------------------ */
typedef struct {
    uint32_t entry;     /* where execution starts (the interpreter's entry) */
    uint32_t sp;        /* initial stack pointer */
} elf_start_t;
int elf_load_program(const char *guest_path, int argc, char **argv, char **envp, elf_start_t *out);
