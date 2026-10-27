#include <SDL2/SDL.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static Uint32 g_init_flags = 0;
static uint64_t g_start_ns = 0;

static uint64_t monotonic_ns(void) {
    uint64_t v = 0;
    const char *no = getenv("EW2_NO_CNTVCT");   /* plain qemu-arm cannot read CNTVCT */
    if (no && *no && *no != '0') {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    }
    __asm__ __volatile__("mrrc p15, 1, %Q0, %R0, c14" : "=r"(v));
    return v << 4;
}

struct DummyWindow {
    int w, h;
};
static struct DummyWindow g_win = {1280, 720};

static pthread_mutex_t g_ev_lock = PTHREAD_MUTEX_INITIALIZER;
static SDL_Event g_events[256];
static int g_ev_head = 0;
static int g_ev_tail = 0;

struct AudioState {
    int open;
    int paused;
    int freq;
    int channels;
    int bytes_per_sec;
    uint64_t drain_anchor_ns;
    int64_t queued_bytes;
    int udp_sock;
};
static struct AudioState g_audio = {0, 1, 44100, 2, 44100 * 4, 0, 0, -1};
static pthread_mutex_t g_audio_lock = PTHREAD_MUTEX_INITIALIZER;

static void audio_update_locked(void) {
    uint64_t now = monotonic_ns();
    if (!g_audio.open || g_audio.paused || g_audio.drain_anchor_ns == 0) {
        g_audio.drain_anchor_ns = now;
        return;
    }
    uint64_t elapsed_ns = now - g_audio.drain_anchor_ns;
    int64_t drained = (int64_t)((double)elapsed_ns * g_audio.bytes_per_sec / 1000000000.0);
    if (drained > 0) {
        g_audio.queued_bytes -= drained;
        if (g_audio.queued_bytes < 0)
            g_audio.queued_bytes = 0;
        g_audio.drain_anchor_ns = now;
    }
}

#ifdef __cplusplus
extern "C" {
#endif

int SDL_Init(Uint32 flags) {
    if (!g_start_ns) g_start_ns = monotonic_ns();
    g_init_flags |= flags;
    return 0;
}

int SDL_InitSubSystem(Uint32 flags) {
    return SDL_Init(flags);
}

Uint32 SDL_WasInit(Uint32 flags) {
    return flags ? (g_init_flags & flags) : g_init_flags;
}

const char *SDL_GetError(void) {
    return "";
}

void SDL_GetVersion(SDL_version *ver) {
    if (ver) {
        ver->major = 2;
        ver->minor = 30;
        ver->patch = 0;
    }
}

const char *SDL_GetRevision(void) {
    return "ew3-sdl-shim";
}

int SDL_GetNumVideoDrivers(void) { return 1; }
const char *SDL_GetVideoDriver(int i) { (void)i; return "offscreen"; }
int SDL_GetNumAudioDevices(int iscapture) { (void)iscapture; return 1; }
const char *SDL_GetAudioDeviceName(int index, int iscapture) { (void)index; (void)iscapture; return "ew3-audio"; }
const char *SDL_GetCurrentAudioDriver(void) { return "ew3-udp"; }

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags) {
    (void)title; (void)x; (void)y; (void)flags;
    g_win.w = w > 0 ? w : 1280;
    g_win.h = h > 0 ? h : 720;
    return (SDL_Window *)&g_win;
}

SDL_Window *SDL_GetWindowFromID(Uint32 id) {
    (void)id;
    return (SDL_Window *)&g_win;
}

void SDL_GetWindowSize(SDL_Window *win, int *w, int *h) {
    (void)win;
    if (w) *w = g_win.w;
    if (h) *h = g_win.h;
}

void SDL_GL_GetDrawableSize(SDL_Window *win, int *w, int *h) {
    (void)win;
    if (w) *w = g_win.w;
    if (h) *h = g_win.h;
}

int SDL_GL_SetAttribute(SDL_GLattr attr, int val) { (void)attr; (void)val; return 0; }
SDL_GLContext SDL_GL_CreateContext(SDL_Window *win) { (void)win; return (SDL_GLContext)(uintptr_t)1; }
int SDL_GL_MakeCurrent(SDL_Window *win, SDL_GLContext ctx) { (void)win; (void)ctx; return 0; }
void SDL_GL_SwapWindow(SDL_Window *win) { (void)win; }
void *SDL_GL_GetProcAddress(const char *proc) { (void)proc; return NULL; }

Uint32 SDL_GetTicks(void) {
    if (!g_start_ns) g_start_ns = monotonic_ns();
    return (Uint32)((monotonic_ns() - g_start_ns) / 1000000ULL);
}

void SDL_Delay(Uint32 ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

int SDL_PollEvent(SDL_Event *event) {
    pthread_mutex_lock(&g_ev_lock);
    if (g_ev_head == g_ev_tail) {
        pthread_mutex_unlock(&g_ev_lock);
        return 0;
    }
    if (event) {
        *event = g_events[g_ev_head];
    }
    g_ev_head = (g_ev_head + 1) & 255;
    pthread_mutex_unlock(&g_ev_lock);
    return 1;
}

int SDL_PushEvent(SDL_Event *event) {
    if (!event) return 0;
    pthread_mutex_lock(&g_ev_lock);
    int next = (g_ev_tail + 1) & 255;
    if (next != g_ev_head) {
        g_events[g_ev_tail] = *event;
        g_ev_tail = next;
    }
    pthread_mutex_unlock(&g_ev_lock);
    return 1;
}

int SDL_NumJoysticks(void) { return 0; }
SDL_bool SDL_IsGameController(int idx) { (void)idx; return SDL_FALSE; }
SDL_GameController *SDL_GameControllerOpen(int idx) { (void)idx; return NULL; }
void SDL_GameControllerClose(SDL_GameController *c) { (void)c; }
const char *SDL_GameControllerName(SDL_GameController *c) { (void)c; return ""; }
SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *c) { (void)c; return NULL; }
SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick *j) { (void)j; return -1; }

void SDL_Log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void *SDL_memset(void *dst, int c, size_t len) {
    return memset(dst, c, len);
}

SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int iscapture, const SDL_AudioSpec *desired, SDL_AudioSpec *obtained, int allowed_changes) {
    (void)device; (void)iscapture; (void)allowed_changes;
    pthread_mutex_lock(&g_audio_lock);
    if (desired && obtained) *obtained = *desired;
    g_audio.open = 1;
    g_audio.paused = 1;
    g_audio.freq = desired && desired->freq > 0 ? desired->freq : 44100;
    g_audio.channels = desired && desired->channels > 0 ? desired->channels : 2;
    int bps = desired ? (SDL_AUDIO_BITSIZE(desired->format) / 8) : 2;
    if (bps <= 0) bps = 2;
    g_audio.bytes_per_sec = g_audio.freq * g_audio.channels * bps;
    g_audio.queued_bytes = 0;
    g_audio.drain_anchor_ns = monotonic_ns();
    const char *adev = getenv("EW2_AUDIO_DEV");
    if (!adev || !*adev)
        adev = getenv("EW3_AUDIO_DEV");
    if (adev && *adev && g_audio.udp_sock < 0) {
        g_audio.udp_sock = open(adev, O_WRONLY);
    }
    pthread_mutex_unlock(&g_audio_lock);
    return 2;
}

void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on) {
    (void)dev;
    pthread_mutex_lock(&g_audio_lock);
    audio_update_locked();
    g_audio.paused = pause_on ? 1 : 0;
    g_audio.drain_anchor_ns = monotonic_ns();
    pthread_mutex_unlock(&g_audio_lock);
}

int SDL_QueueAudio(SDL_AudioDeviceID dev, const void *data, Uint32 len) {
    (void)dev;
    if (!data || !len) return 0;
    pthread_mutex_lock(&g_audio_lock);
    audio_update_locked();
    g_audio.queued_bytes += len;
    if (g_audio.udp_sock >= 0 && len <= 60000) {
        uint8_t pkt[60016];
        uint32_t hdr[3] = {0x41335745u, (uint32_t)g_audio.freq, (uint32_t)len};
        memcpy(pkt, hdr, 12);
        memcpy(pkt + 12, data, len);
        size_t total = (size_t)len + 12;
        size_t off = 0;
        while (off < total) {
            ssize_t w = write(g_audio.udp_sock, pkt + off, total - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN) { usleep(500); continue; }
                break;
            }
            off += (size_t)w;
        }
    }
    pthread_mutex_unlock(&g_audio_lock);
    return 0;
}

Uint32 SDL_GetQueuedAudioSize(SDL_AudioDeviceID dev) {
    (void)dev;
    pthread_mutex_lock(&g_audio_lock);
    audio_update_locked();
    Uint32 r = (Uint32)(g_audio.queued_bytes > 0 ? g_audio.queued_bytes : 0);
    pthread_mutex_unlock(&g_audio_lock);
    return r;
}

void SDL_ClearQueuedAudio(SDL_AudioDeviceID dev) {
    (void)dev;
    pthread_mutex_lock(&g_audio_lock);
    g_audio.queued_bytes = 0;
    g_audio.drain_anchor_ns = monotonic_ns();
    pthread_mutex_unlock(&g_audio_lock);
}

void SDL_CloseAudioDevice(SDL_AudioDeviceID dev) {
    (void)dev;
    pthread_mutex_lock(&g_audio_lock);
    g_audio.open = 0;
    g_audio.queued_bytes = 0;
    pthread_mutex_unlock(&g_audio_lock);
}

static Sint64 rw_size(SDL_RWops *ctx) {
    FILE *f = (FILE *)ctx->hidden.stdio.fp;
    if (!f) return -1;
    long cur = ftell(f);
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    fseek(f, cur, SEEK_SET);
    return (Sint64)end;
}

static Sint64 rw_seek(SDL_RWops *ctx, Sint64 offset, int whence) {
    FILE *f = (FILE *)ctx->hidden.stdio.fp;
    if (!f || fseek(f, (long)offset, whence) != 0) return -1;
    return (Sint64)ftell(f);
}

static size_t rw_read(SDL_RWops *ctx, void *ptr, size_t size, size_t maxnum) {
    FILE *f = (FILE *)ctx->hidden.stdio.fp;
    return f ? fread(ptr, size, maxnum, f) : 0;
}

static size_t rw_write(SDL_RWops *ctx, const void *ptr, size_t size, size_t num) {
    FILE *f = (FILE *)ctx->hidden.stdio.fp;
    return f ? fwrite(ptr, size, num, f) : 0;
}

static int rw_close(SDL_RWops *ctx) {
    if (!ctx) return 0;
    FILE *f = (FILE *)ctx->hidden.stdio.fp;
    if (f) fclose(f);
    free(ctx);
    return 0;
}

SDL_RWops *SDL_RWFromFile(const char *file, const char *mode) {
    if (!file || !mode) return NULL;
    FILE *f = fopen(file, mode);
    if (!f) return NULL;
    SDL_RWops *rw = (SDL_RWops *)calloc(1, sizeof(SDL_RWops));
    rw->size = rw_size;
    rw->seek = rw_seek;
    rw->read = rw_read;
    rw->write = rw_write;
    rw->close = rw_close;
    rw->type = SDL_RWOPS_STDFILE;
    rw->hidden.stdio.fp = f;
    return rw;
}

Sint64 SDL_RWsize(SDL_RWops *ctx) { return ctx && ctx->size ? ctx->size(ctx) : -1; }
size_t SDL_RWread(SDL_RWops *ctx, void *ptr, size_t size, size_t maxnum) { return ctx && ctx->read ? ctx->read(ctx, ptr, size, maxnum) : 0; }
int SDL_RWclose(SDL_RWops *ctx) { return ctx && ctx->close ? ctx->close(ctx) : 0; }

#ifdef __cplusplus
} // extern "C"
#endif
