/*
 * Milestone tracing.
 *
 * The harness cannot ask the port "did it work?"; it can only read what the
 * port prints. Every observable milestone (module loaded, onCreate returned,
 * gl ready, frame, non-black framebuffer) is announced through trace() so the
 * verification script asserts on facts instead of on the absence of a crash.
 *
 * Enabled by LOADER_TRACE=1 so a released port stays quiet.
 */
#ifndef EW3_TRACE_H
#define EW3_TRACE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Queues "TRACE: <msg>" for stderr (written by a background thread) when LOADER_TRACE is set. EW2_TRACE_SYNC=1 writes inline. */
void trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Writes out everything trace() has queued (it is buffered; see trace.cpp). Call before dying. */
void trace_flush(void);
void trace_flush_crash(void);   /* signal-handler variant: never blocks */

/* Always printed, tracing on or off: the user needs to see why it refused. */
void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif /* EW3_TRACE_H */
