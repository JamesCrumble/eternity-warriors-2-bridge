#include "trace.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int trace_enabled(void)
{
    /* Resolved once: this sits on the frame path later on. */
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("LOADER_TRACE");
        cached = (v && *v && *v != '0') ? 1 : 0;
    }
    return cached;
}

/*
 * trace() used to fprintf + fflush(stderr) on the calling thread: one write(2) per line, and under emulation a
 * syscall is expensive enough to distort the frame times the log is supposed to measure. Now the caller only
 * formats the line and appends it to a growing buffer; a writer thread swaps that buffer out every few
 * milliseconds and writes it to fd 2. Nothing is ever dropped: the buffer grows if the disk is slower than the
 * game (in practice it holds what is produced in ~10 ms).
 * EW2_TRACE_SYNC=1 restores the old synchronous behaviour.
 */
namespace {

constexpr size_t kLine = 8192;

struct Buf {
    char *data;
    size_t len, cap;
};

Buf g_buf;                              /* being filled by trace() */
Buf g_spare;                            /* being written (or idle) */
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_once_t g_once = PTHREAD_ONCE_INIT;
int g_sync;

void write_all(const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(2, p, n);
        if (w < 0)
            return;
        p += w;
        n -= (size_t)w;
    }
}

/* Moves everything queued so far to fd 2. Called by the writer thread, fatal(), atexit and the crash handler. */
void drain(bool nowait = false)
{
    static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;   /* one drainer at a time keeps lines in order */
    if (nowait ? pthread_mutex_trylock(&out_lock) != 0 : pthread_mutex_lock(&out_lock) != 0)
        return;
    if (nowait ? pthread_mutex_trylock(&g_lock) == 0 : pthread_mutex_lock(&g_lock) == 0) {
        Buf full = g_buf;
        g_buf = g_spare;                /* the (emptied) spare becomes the buffer being filled */
        g_buf.len = 0;
        pthread_mutex_unlock(&g_lock);
        if (full.len)
            write_all(full.data, full.len);
        full.len = 0;
        g_spare = full;                 /* keep its capacity for the next swap */
    }
    pthread_mutex_unlock(&out_lock);
}

void *writer_main(void *)
{
    for (;;) {
        struct timespec ts = {0, 10 * 1000 * 1000};   /* 10 ms */
        nanosleep(&ts, NULL);
        drain();
    }
    return NULL;
}

void start_writer()
{
    const char *s = getenv("EW2_TRACE_SYNC");
    g_sync = (s && *s && *s != '0') ? 1 : 0;
    if (!g_sync) {
        pthread_t t;
        if (pthread_create(&t, NULL, writer_main, NULL) == 0)
            pthread_detach(t);
        else
            g_sync = 1;
    }
    atexit([] { drain(); });
}

void emit(const char *prefix, const char *fmt, va_list ap)
{
    pthread_once(&g_once, start_writer);
    char line[kLine];
    int n = snprintf(line, sizeof(line), "%s", prefix);
    if (n < 0)
        return;
    int m = vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
    if (m < 0)
        return;
    size_t len = (size_t)n + (size_t)m;
    if (len > sizeof(line) - 2)
        len = sizeof(line) - 2;
    line[len++] = '\n';
    if (g_sync) {
        write_all(line, len);
        return;
    }
    pthread_mutex_lock(&g_lock);
    if (g_buf.len + len > g_buf.cap) {
        size_t cap = g_buf.cap ? g_buf.cap : (256u << 10);
        while (cap < g_buf.len + len)
            cap *= 2;
        char *grown = (char *)realloc(g_buf.data, cap);
        if (grown) {
            g_buf.data = grown;
            g_buf.cap = cap;
        }
    }
    if (g_buf.len + len <= g_buf.cap) {
        memcpy(g_buf.data + g_buf.len, line, len);
        g_buf.len += len;
    }
    pthread_mutex_unlock(&g_lock);
}

} // namespace

extern "C" void trace_flush(void)
{
    if (!g_sync)
        drain();
}

/* From a fault handler: best effort, never waits for a lock the faulting thread may already hold. */
extern "C" void trace_flush_crash(void)
{
    if (!g_sync)
        drain(true);
}

extern "C" void trace(const char *fmt, ...)
{
    if (!trace_enabled())
        return;
    va_list ap;
    va_start(ap, fmt);
    emit("TRACE: ", fmt, ap);
    va_end(ap);
}

extern "C" void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit("FATAL: ", fmt, ap);
    va_end(ap);
    trace_flush();
}
