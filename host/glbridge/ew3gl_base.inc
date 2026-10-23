/*
 * ew3gl module 1/12: includes, logging, settings, guest -> host id maps.
 *
 * ew3gl.c includes the modules in order as one translation unit: the generated
 * gl_ops_gen.inc calls rd_i(), rd_f() and idmap_get(&g_ids.*) as statics.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>   /* before windows.h, which would pull in the old winsock.h */
#include <ws2tcpip.h>
#include <windows.h>
#include <errno.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <SDL2/SDL.h>

/* GLES 2 entry points plus the ES 3.0 ones the bridge forwards (3D textures,
 * glInvalidateFramebuffer). Linked by name: Mesa on WSLg, ANGLE on Windows. */
#define GL_GLEXT_PROTOTYPES 1
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

/* ------------------------------------------------------------- logging */

static FILE *g_log;
static uint64_t g_t0_ns;

static uint64_t now_ns(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static void glog(const char *fmt, ...)
{
    FILE *f = g_log ? g_log : stderr;
    va_list ap;
    va_start(ap, fmt);
    fprintf(f, "[ew2gl %8.3f] ", (double)(now_ns() - g_t0_ns) / 1e9);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    fflush(f);
    va_end(ap);
}

static void gfatal(const char *fmt, ...)
{
    FILE *f = g_log ? g_log : stderr;
    va_list ap;
    va_start(ap, fmt);
    fprintf(f, "[ew2gl] FATAL: ");
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    fflush(f);
    va_end(ap);
    exit(1);
}

/* Logs the first `limit` occurrences of something that may repeat every frame. */
#define GLOG_LIMITED(counter, limit, ...) \
    do { if ((counter)++ < (limit)) glog(__VA_ARGS__); } while (0)

/* ------------------------------------------------------------- settings */

/*
 * Every switch is EW2_<name>, the same prefix the guest uses (PORT_ENV_PREFIX).
 */
static const char *cfg_str(const char *name, const char *fallback)
{
    char full[128];
    snprintf(full, sizeof(full), "EW2_%s", name);
    const char *v = getenv(full);
    if (!v || !*v) {
        snprintf(full, sizeof(full), "EW3_%s", name);
        v = getenv(full);
    }
    return v && *v ? v : fallback;
}

static long cfg_long(const char *name, long fallback)
{
    const char *v = cfg_str(name, NULL);
    if (!v)
        return fallback;
    char *end;
    long r = strtol(v, &end, 0);
    return end != v ? r : fallback;
}

struct config {
    const char *connect;      /* EW3_GL_CONNECT=host:port: TCP to the guest instead of the fifos */
    const char *cmd_path;     /* EW3_GL_CMD: guest -> host fifo */
    const char *rep_path;     /* EW3_GL_REPLY: host -> guest fifo */
    const char *control_dir;  /* EW3_CONTROL_DIR: where input goes (ctl/commands) */
    int width, height;        /* EW3_WIDTH / EW3_HEIGHT: the guest's render surface */
    int vsync;                /* EW3_GL_VSYNC, default 1 */
    int fullscreen;           /* EW3_GL_FULLSCREEN */
    int report_s;             /* EW3_GL_REPORT: seconds between stats lines, 0 = off */
    int check_errors;         /* EW3_GL_CHECK: glGetError after every op (slow; for debugging) */
};
static struct config g_cfg;

static void config_load(void)
{
    g_cfg.connect = cfg_str("GL_CONNECT", NULL);
    g_cfg.cmd_path = cfg_str("GL_CMD", NULL);
    g_cfg.rep_path = cfg_str("GL_REPLY", NULL);
    g_cfg.control_dir = cfg_str("CONTROL_DIR", NULL);
    g_cfg.width = (int)cfg_long("WIDTH", 1280);
    g_cfg.height = (int)cfg_long("HEIGHT", 720);
    g_cfg.vsync = (int)cfg_long("GL_VSYNC", 1);
    g_cfg.fullscreen = (int)cfg_long("GL_FULLSCREEN", 0);
    g_cfg.report_s = (int)cfg_long("GL_REPORT", 5);
    g_cfg.check_errors = (int)cfg_long("GL_CHECK", 0);
    if (g_cfg.width < 320 || g_cfg.width > 4096)
        g_cfg.width = 1280;
    if (g_cfg.height < 240 || g_cfg.height > 4096)
        g_cfg.height = 720;
}

/* ------------------------------------------------------------- id maps */

/*
 * The guest allocates object names itself (1, 2, 3... per kind) so that a
 * glGen* never waits for an answer; the host keeps guest -> real name here.
 */
struct idmap {
    GLuint *a;
    size_t n;
};

static GLuint idmap_get(const struct idmap *m, int32_t guest)
{
    return guest > 0 && (size_t)guest < m->n ? m->a[guest] : 0;
}

static void idmap_put(struct idmap *m, int32_t guest, GLuint host)
{
    if (guest <= 0)
        return;
    if ((size_t)guest >= m->n) {
        size_t n = m->n ? m->n : 1024;
        while (n <= (size_t)guest)
            n *= 2;
        GLuint *a = (GLuint *)realloc(m->a, n * sizeof(GLuint));
        if (!a)
            gfatal("out of memory growing an id map to %zu", n);
        memset(a + m->n, 0, (n - m->n) * sizeof(GLuint));
        m->a = a;
        m->n = n;
    }
    m->a[guest] = host;
}

/* Real name -> guest name, for glGetIntegerv(GL_*_BINDING); rare, so a scan. */
static int32_t idmap_reverse(const struct idmap *m, GLuint host)
{
    if (host == 0)
        return 0;
    for (size_t i = 1; i < m->n; i++)
        if (m->a[i] == host)
            return (int32_t)i;
    return 0;
}

struct ids {
    struct idmap tex, buf, fbo, rbo, prog, shader;
};
static struct ids g_ids;

/* Object kinds in GEN / DELETE messages (port/src/gl_bridge.cpp: KIND_*). */
enum { GEN_TEX = 0, GEN_BUF = 1, GEN_FBO = 2, GEN_RBO = 3 };

/* ------------------------------------------------------------- run state */

static volatile int g_alive = 1;   /* cleared by the window, a dead pipe or a signal */
static long g_swaps;               /* frames presented */
static long g_exec_errors;         /* ops that could not be executed (logged, limited) */
static long g_gl_errors;           /* glGetError hits (logged, limited) */
