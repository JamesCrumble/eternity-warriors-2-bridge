#include <atomic>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "khronos/glad.h"
#include "port_env.h"
#include "trace.h"

#include "gl_diag.h"

using GetError = GLenum (*)(void);

static std::atomic<int> g_reports(0);
static GetError g_get_error = nullptr;
static constexpr int kReportLimit = 128;

bool gl_diag_enabled(void)
{
    static int enabled = [] {
        const char *value = getenv("EW3_GL_DIAG");
        return value && *value && strcmp(value, "0") != 0;
    }();
    return enabled != 0;
}

extern "C" long android_egl_frames(void);

/* EW2_GL_DIAG_EVERY=N: check only during every Nth frame (default 1: all), so the sync glGetError traffic
 * does not distort the timings of the other frames. */
static bool diag_frame_active(void)
{
    static long every = port_getenv_long("GL_DIAG_EVERY", 1);
    return every <= 1 || android_egl_frames() % every == 0;
}

static GetError raw_get_error(void)
{
    if (!g_get_error)
        g_get_error = (GetError)SDL_GL_GetProcAddress("glGetError");
    return g_get_error;
}

static void report(const char *position, const char *name, GLenum error)
{
    int line = g_reports.fetch_add(1);
    if (line < kReportLimit)
        trace("GLDIAG: %s %s: 0x%04x", position,
              name ? name : "(unknown)", error);
    else if (line == kReportLimit)
        trace("GLDIAG: report limit reached; suppressing further errors");
}

void gl_diag_before(const char *name)
{
    if (!diag_frame_active())
        return;
    GetError get_error = raw_get_error();
    if (!get_error)
        return;

    GLenum error;
    while ((error = get_error()) != GL_NO_ERROR)
        report("pending before", name, error);
}

void gl_diag_after(const char *name)
{
    if (!diag_frame_active())
        return;
    GetError get_error = raw_get_error();
    if (!get_error)
        return;

    GLenum error;
    while ((error = get_error()) != GL_NO_ERROR)
        report("produced by", name, error);
}
