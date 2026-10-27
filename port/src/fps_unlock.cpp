/* Frame-rate unlock; see fps_unlock.h. */

#include <string.h>

#include "fps_unlock.h"
#include "find_cache.h"
#include "icall_prof.h"
#include "platform.h"
#include "port_env.h"
#include "trace.h"

typedef void (*add_icall_fn)(const char *name, void *fn);
typedef void (*set_fps_fn)(int value);
typedef void (*set_vsync_fn)(int value);
typedef void (*set_quality_fn)(int index, int applyExpensiveChanges);
typedef void *(*get_audio_clip_fn)(void *self, int threeD, int stream, int audioType);
typedef void (*set_volume_fn)(void *self, float volume) __attribute__((pcs("aapcs")));

static add_icall_fn      g_real_add_icall = NULL;
static set_fps_fn        g_real_set_fps = NULL;
static set_vsync_fn      g_real_set_vsync = NULL;
static set_quality_fn    g_real_set_quality = NULL;
static get_audio_clip_fn g_real_get_audio_clip = NULL;
static set_volume_fn     g_real_set_volume = NULL;
static int               g_forced_fps = -1;      /* -1: not read yet */
static volatile int      g_game_called = 0;      /* the game has set a rate itself (and got ours) */

static int forced_fps()
{
    if (g_forced_fps < 0)
        g_forced_fps = (int)port_getenv_long("TARGET_FPS", 120);
    return g_forced_fps;
}

/*
 * What the guest calls instead of Application::set_targetFrameRate.
 *
 * ARM code, not Thumb: this becomes a Mono icall, and Mono's ARM JIT refuses to
 * call a Thumb address unless it believes the CPU has Thumb - which it decides
 * from /proc/cpuinfo, the host's under qemu-user ("Assertion at mini-arm.c:2644,
 * condition `thumb_supported' not met" -> abort -> the game froze at frame 60).
 */
__attribute__((target("arm"), noinline))
static ABI_ATTR void set_fps_hook(int value)
{
    int want = forced_fps();
    g_game_called = 1;
    if (want > 0 && value != want) {
        trace("fps unlock: game asked for targetFrameRate %d, forcing %d", value, want);
        value = want;
    }
    if (g_real_set_fps)
        g_real_set_fps(value);
}

__attribute__((target("arm"), noinline))
static ABI_ATTR void set_vsync_hook(int value)
{
    if (forced_fps() > 0 && value != 0)
        value = 0;
    if (g_real_set_vsync)
        g_real_set_vsync(value);
}

__attribute__((target("arm"), noinline))
static ABI_ATTR void set_quality_hook(int index, int applyExpensiveChanges)
{
    if (g_real_set_quality)
        g_real_set_quality(index, applyExpensiveChanges);
    if (forced_fps() > 0) {
        if (g_real_set_vsync && port_getenv_long("VSYNC", 0) == 0)
            g_real_set_vsync(0);
        if (g_real_set_fps)
            g_real_set_fps(forced_fps());
    }
}

/*
 * SoundManager.StreamPlay loads StreamingAssets/*.mp3 with
 *   using (WWW www = new WWW(filePath)) {
 *       _bgmSource.clip = www.GetAudioClip(false, true, AudioType.MPEG);
 *       ...
 *   }
 * Passing stream=true binds the FMOD stream to the AndroidWWW instance, which
 * is immediately aborted & destroyed when the `using` block disposes `www`.
 * Since unity_www.cpp already buffers the .mp3 in memory synchronously, forcing
 * stream=0 decodes from the completed memory buffer so BGM plays reliably.
 */
extern "C" void unity_www_wait_all();

__attribute__((target("arm"), noinline))
static ABI_ATTR void *get_audio_clip_hook(void *self, int threeD, int stream, int audioType)
{
    if (stream && (audioType == 13 /* MPEG */ || audioType == 0 /* UNKNOWN */)) {
        unity_www_wait_all();
        stream = 0;
    }
    return g_real_get_audio_clip ? g_real_get_audio_clip(self, threeD, stream, audioType) : NULL;
}

/*
 * SoundManager.PlayBGMWithFade leaves _fadeTime == 0f when no BGM is currently
 * playing, and UpdateBGMusic then sets _bgmSource.volume = _fadeTimer / 0f
 * (+Infinity / NaN). Clamp invalid/infinite volumes to [0.0f, 1.0f].
 */
__attribute__((target("arm"), noinline, pcs("aapcs")))
static void set_volume_hook(void *self, float volume)
{
    if (!(volume >= 0.0f) || volume > 1.0f)
        volume = (volume > 1.0f) ? 1.0f : 1.0f;
    if (g_real_set_volume)
        g_real_set_volume(self, volume);
}

/*
 * UnityEngine.Debug::Internal_Log walks the Mono stack on every C# Debug.Log /
 * Debug.LogError call (e.g. GameManager.CheckNetworkReady logging "User not
 * signed in" on every menu save). Bypassing it unless EW2_UNITY_LOG=1 is set
 * eliminates menu transition stutters.
 */
__attribute__((target("arm"), noinline))
static ABI_ATTR void internal_log_noop(int level, void *msg, void *obj)
{
    (void)level;
    (void)msg;
    (void)obj;
}

/* What the guest calls instead of il2cpp_add_internal_call / mono_add_internal_call (same signature). */
static ABI_ATTR void add_icall_hook(const char *name, void *fn)
{
    if (name && !strcmp(name, "UnityEngine.Application::set_targetFrameRate") && forced_fps() > 0) {
        g_real_set_fps = (set_fps_fn)fn;
        fn = (void *)set_fps_hook;
        trace("fps unlock: hooked %s, forcing %d fps", name, forced_fps());
    } else if (name && !strcmp(name, "UnityEngine.QualitySettings::set_vSyncCount") && forced_fps() > 0) {
        g_real_set_vsync = (set_vsync_fn)fn;
        fn = (void *)set_vsync_hook;
        trace("fps unlock: hooked %s", name);
    } else if (name && !strcmp(name, "UnityEngine.QualitySettings::SetQualityLevel") && forced_fps() > 0) {
        g_real_set_quality = (set_quality_fn)fn;
        fn = (void *)set_quality_hook;
        trace("fps unlock: hooked %s", name);
    } else if (name && !strcmp(name, "UnityEngine.WWW::GetAudioClip")) {
        g_real_get_audio_clip = (get_audio_clip_fn)fn;
        fn = (void *)get_audio_clip_hook;
    } else if (name && !strcmp(name, "UnityEngine.AudioSource::set_volume")) {
        g_real_set_volume = (set_volume_fn)fn;
        fn = (void *)set_volume_hook;
    } else if (name && !strcmp(name, "UnityEngine.Debug::Internal_Log") && !port_getenv_long("UNITY_LOG", 0)) {
        fn = (void *)internal_log_noop;
    }
    g_real_add_icall(name, icall_prof_wrap(name, find_cache_wrap(name, fn)));
}

extern "C" void *fps_unlock_hook_symbol(const char *name, void *sym)
{
    if (sym && name && !g_real_add_icall &&
        (!strcmp(name, "il2cpp_add_internal_call") || !strcmp(name, "mono_add_internal_call"))) {
        g_real_add_icall = (add_icall_fn)sym;
        trace("fps unlock: %s intercepted", name);
        return (void *)add_icall_hook;
    }
    return sym;
}

/*
 * Only for a game that never sets a rate itself (Shadowgun's IL2CPP build relied on this). Mono games
 * such as EW3 set it from C# early on, and calling the icall from here before Mono is up froze Unity 4.5
 * at frame 60 - so it waits much longer and is skipped once the game has called.
 */
extern "C" void fps_unlock_tick(long frame)
{
    if (frame >= 120 && frame % 300 == 0 && g_real_set_vsync && port_getenv_long("VSYNC", 0) == 0) {
        if (frame == 120)
            trace("fps unlock: QualitySettings.vSyncCount = 0");
        g_real_set_vsync(0);
    }
    if (frame == 120 && !g_game_called && g_real_set_fps && forced_fps() > 0) {
        trace("fps unlock: setting targetFrameRate %d", forced_fps());
        g_real_set_fps(forced_fps());
    }
}
