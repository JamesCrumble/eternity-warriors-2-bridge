/* See icall_prof.h. */

#include <map>
#include <mutex>
#include <string>
#include <string.h>
#include <time.h>
#include <utility>

#include "icall_prof.h"
#include "platform.h"
#include "port_env.h"
#include "so_util.h"
#include "trace.h"

namespace {

typedef void *(*icall4_fn)(void *, void *, void *, void *);

struct Slot {
    const char *name;
    bool takes_type;          /* first argument is a System.Type (MonoReflectionType*) */
    icall4_fn real;
    unsigned long calls;
    unsigned long long ns;
    unsigned long long max_ns;
};

struct TypeStat {
    unsigned long calls = 0;
    unsigned long long ns = 0;
};

constexpr int kSlots = 10;
Slot g_slots[kSlots] = {
    {"UnityEngine.Object::FindObjectsOfType", true, NULL, 0, 0, 0},
    {"UnityEngine.Object::FindObjectOfType", true, NULL, 0, 0, 0},
    {"UnityEngine.Resources::FindObjectsOfTypeAll", true, NULL, 0, 0, 0},
    {"UnityEngine.GameObject::FindGameObjectsWithTag", false, NULL, 0, 0, 0},
    {"UnityEngine.GameObject::FindGameObjectWithTag", false, NULL, 0, 0, 0},
    {"UnityEngine.GameObject::Find", false, NULL, 0, 0, 0},
    {"UnityEngine.Resources::Load", false, NULL, 0, 0, 0},
    {"UnityEngine.Object::INTERNAL_CALL_Internal_InstantiateSingle", false, NULL, 0, 0, 0},
    {"UnityEngine.Object::Internal_CloneSingle", false, NULL, 0, 0, 0},
    {"UnityEngine.Object::Destroy", false, NULL, 0, 0, 0},
};
std::mutex g_lock;
std::map<std::pair<int, void *>, TypeStat> g_types;   /* (slot, MonoType*) */
long g_last_report_frame = 0;

unsigned long long now_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}

template <int I>
ABI_ATTR void *wrapper(void *a, void *b, void *c, void *d)
{
    Slot &s = g_slots[I];
    const unsigned long long t0 = now_ns();
    void *r = s.real(a, b, c, d);
    const unsigned long long dt = now_ns() - t0;
    std::lock_guard<std::mutex> lock(g_lock);
    s.calls++;
    s.ns += dt;
    if (dt > s.max_ns)
        s.max_ns = dt;
    if (s.takes_type && a) {
        void *type = *(void **)((char *)a + 8);   /* MonoReflectionType::type, after the MonoObject header */
        TypeStat &t = g_types[{I, type}];
        t.calls++;
        t.ns += dt;
    }
    return r;
}

icall4_fn g_wrappers[kSlots] = {
    wrapper<0>, wrapper<1>, wrapper<2>, wrapper<3>, wrapper<4>,
    wrapper<5>, wrapper<6>, wrapper<7>, wrapper<8>, wrapper<9>,
};

bool enabled()
{
    static int v = port_getenv_bool("ICALL_PROF", 0);
    return v != 0;
}

std::string type_name(void *type)
{
    typedef char *(*get_name_fn)(void *type, int name_format);
    static get_name_fn get_name = NULL;
    static bool tried = false;
    if (!tried) {
        tried = true;
        so_module *mono = so_find_module("libmono.so");
        if (mono)
            get_name = (get_name_fn)so_symbol(mono, "mono_type_get_name");
    }
    if (!get_name || !type)
        return "?";
    char *n = get_name(type, 0);
    return n ? n : "?";
}

} // namespace

extern "C" void *icall_prof_wrap(const char *name, void *fn)
{
    if (!name || !fn || !enabled())
        return fn;
    for (int i = 0; i < kSlots; i++) {
        if (!strcmp(name, g_slots[i].name)) {
            g_slots[i].real = (icall4_fn)fn;
            trace("icallprof: wrapping %s", name);
            return (void *)g_wrappers[i];
        }
    }
    return fn;
}

extern "C" void icall_prof_tick(long frame)
{
    if (!enabled() || frame - g_last_report_frame < 120)
        return;
    const long frames = frame - g_last_report_frame;
    g_last_report_frame = frame;
    std::lock_guard<std::mutex> lock(g_lock);
    for (int i = 0; i < kSlots; i++) {
        Slot &s = g_slots[i];
        if (!s.real || s.calls == 0)
            continue;
        trace("icallprof: %-58s %6.2f calls/frame  %6.3f ms/frame  (%.3f ms/call, max %.2f ms)", s.name,
              (double)s.calls / frames, (double)s.ns / 1e6 / frames, (double)s.ns / 1e6 / s.calls,
              (double)s.max_ns / 1e6);
        if (s.takes_type) {
            for (auto &kv : g_types) {
                if (kv.first.first != i)
                    continue;
                trace("icallprof:     type %-50s %6.2f calls/frame  %6.3f ms/frame", type_name(kv.first.second).c_str(),
                      (double)kv.second.calls / frames, (double)kv.second.ns / 1e6 / frames);
            }
        }
        s.calls = 0;
        s.ns = 0;
        s.max_ns = 0;
    }
    g_types.clear();
}
