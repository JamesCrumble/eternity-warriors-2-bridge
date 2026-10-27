/* See find_cache.h. */

#include <map>
#include <mutex>
#include <stdint.h>
#include <stdlib.h>
#include <string>
#include <string.h>
#include <vector>

#include "find_cache.h"
#include "platform.h"
#include "port_env.h"
#include "so_util.h"
#include "trace.h"

namespace {

typedef void *(*icall4_fn)(void *, void *, void *, void *);
typedef uint32_t (*gchandle_new_fn)(void *obj, int pinned);
typedef void *(*gchandle_target_fn)(uint32_t handle);
typedef void (*gchandle_free_fn)(uint32_t handle);
typedef char *(*type_name_fn)(void *type, int format);

constexpr int kMaxTypes = 8;
constexpr int kMaxElems = 16;

struct Entry {
    bool valid = false;
    uint32_t handle = 0;
    long frame = 0;
    unsigned long gen = 0;
    int n = 0;
    void *elems[kMaxElems];
};

std::mutex g_lock;
std::vector<std::string> g_allowed;
Entry g_entry[kMaxTypes];
std::map<void *, int> g_type_index;   /* MonoType* -> index in g_allowed, -1: not cached */
unsigned long g_gen = 0;
long g_frame = 0;
long g_max_age = 30;
unsigned long g_hits, g_misses, g_invalidations;
bool g_init, g_on;

gchandle_new_fn g_gc_new;
gchandle_target_fn g_gc_target;
gchandle_free_fn g_gc_free;
type_name_fn g_type_name;

void init()
{
    if (g_init)
        return;
    g_init = true;
    const char *v = port_getenv("FIND_CACHE");
    std::string spec = v ? v : "HomeUIController";
    if (spec.empty() || spec == "0" || spec == "off" || spec == "false")
        return;
    size_t pos = 0;
    while (pos <= spec.size() && (int)g_allowed.size() < kMaxTypes) {
        size_t comma = spec.find(',', pos);
        std::string t = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (!t.empty())
            g_allowed.push_back(t);
        if (comma == std::string::npos)
            break;
        pos = comma + 1;
    }
    g_max_age = port_getenv_long("FIND_CACHE_FRAMES", 30);
    so_module *mono = so_find_module("libmono.so");
    if (!mono || g_allowed.empty())
        return;
    g_gc_new = (gchandle_new_fn)so_symbol(mono, "mono_gchandle_new");
    g_gc_target = (gchandle_target_fn)so_symbol(mono, "mono_gchandle_get_target");
    g_gc_free = (gchandle_free_fn)so_symbol(mono, "mono_gchandle_free");
    g_type_name = (type_name_fn)so_symbol(mono, "mono_type_get_name");
    g_on = g_gc_new && g_gc_target && g_gc_free && g_type_name;
    if (g_on)
        trace("findcache: caching FindObjectsOfType for %zu type(s), max age %ld frames", g_allowed.size(), g_max_age);
    else
        trace("findcache: Mono gchandle/type API not found, cache off");
}

/* Index of the type in g_allowed, or -1. Called with g_lock held. */
int type_index(void *mono_type)
{
    auto it = g_type_index.find(mono_type);
    if (it != g_type_index.end())
        return it->second;
    int idx = -1;
    char *n = g_type_name(mono_type, 0);
    if (n) {
        for (size_t i = 0; i < g_allowed.size(); i++)
            if (g_allowed[i] == n)
                idx = (int)i;
    }
    g_type_index[mono_type] = idx;
    return idx;
}

void invalidate_all()
{
    g_gen++;
    g_invalidations++;
}

/* A destroyed object only matters when it is one of the cached elements. */
void invalidate_if_cached(void *obj)
{
    if (!obj)
        return;
    for (int i = 0; i < kMaxTypes; i++) {
        const Entry &e = g_entry[i];
        if (!e.valid)
            continue;
        for (int k = 0; k < e.n; k++)
            if (e.elems[k] == obj) {
                invalidate_all();
                return;
            }
    }
}

icall4_fn g_find_real;

void *find_wrapper(void *type_obj, void *b, void *c, void *d)
{
    std::unique_lock<std::mutex> lock(g_lock);
    int idx = -1;
    if (g_on && type_obj) {
        void *mono_type = *(void **)((char *)type_obj + 8);   /* MonoReflectionType::type */
        idx = type_index(mono_type);
    }
    if (idx < 0) {
        lock.unlock();
        return g_find_real(type_obj, b, c, d);
    }
    Entry &e = g_entry[idx];
    if (e.valid && e.gen == g_gen && g_frame - e.frame < g_max_age) {
        void *target = g_gc_target(e.handle);
        if (target) {
            g_hits++;
            return target;
        }
        e.valid = false;
    }
    lock.unlock();
    void *r = g_find_real(type_obj, b, c, d);
    lock.lock();
    g_misses++;
    if (e.valid || e.handle) {
        g_gc_free(e.handle);
        e.handle = 0;
    }
    e.valid = false;
    if (r) {
        const uint32_t len = *(uint32_t *)((char *)r + 12);   /* MonoArray::max_length */
        e.n = len < (uint32_t)kMaxElems ? (int)len : kMaxElems;
        for (int k = 0; k < e.n; k++)
            e.elems[k] = *(void **)((char *)r + 16 + 4 * k);
        e.handle = g_gc_new(r, 0);
        e.gen = g_gen;
        e.frame = g_frame;
        e.valid = true;
    }
    return r;
}

/* Calls that can change what FindObjectsOfType returns. The first two only when they hit a cached element. */
const char *kInvalidators[] = {
    "UnityEngine.Object::Destroy",
    "UnityEngine.Object::DestroyImmediate",
    "UnityEngine.Application::LoadLevelAsync",
    "UnityEngine.Object::INTERNAL_CALL_Internal_InstantiateSingle",
    "UnityEngine.Object::Internal_CloneSingle",
    "UnityEngine.GameObject::Internal_CreateGameObject",
    "UnityEngine.GameObject::AddComponent",
    "UnityEngine.GameObject::Internal_AddComponentWithType",
};
constexpr int kInval = sizeof(kInvalidators) / sizeof(kInvalidators[0]);
icall4_fn g_inval_real[kInval];

template <int I>
void *inval_wrapper(void *a, void *b, void *c, void *d)
{
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (I < 2)
            invalidate_if_cached(a);
        else
            invalidate_all();
    }
    return g_inval_real[I](a, b, c, d);
}

icall4_fn g_inval_wrappers[kInval] = {
    inval_wrapper<0>, inval_wrapper<1>, inval_wrapper<2>, inval_wrapper<3>,
    inval_wrapper<4>, inval_wrapper<5>, inval_wrapper<6>, inval_wrapper<7>,
};

} // namespace

extern "C" void *find_cache_wrap(const char *name, void *fn)
{
    if (!name || !fn)
        return fn;
    std::lock_guard<std::mutex> lock(g_lock);
    init();
    if (g_allowed.empty())
        return fn;
    if (!strcmp(name, "UnityEngine.Object::FindObjectsOfType")) {
        g_find_real = (icall4_fn)fn;
        return (void *)find_wrapper;
    }
    for (int i = 0; i < kInval; i++)
        if (!strcmp(name, kInvalidators[i])) {
            g_inval_real[i] = (icall4_fn)fn;
            return (void *)g_inval_wrappers[i];
        }
    return fn;
}

extern "C" void find_cache_tick(long frame)
{
    std::lock_guard<std::mutex> lock(g_lock);
    g_frame = frame;
    if (g_on && frame > 0 && frame % 1200 == 0 && (g_hits + g_misses)) {
        trace("findcache: %lu hits, %lu misses, %lu invalidations in the last 1200 frames", g_hits, g_misses,
              g_invalidations);
        g_hits = g_misses = g_invalidations = 0;
    }
}
