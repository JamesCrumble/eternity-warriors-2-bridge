/*
 * zlib and two libstdc++ leftovers for Unity 4.5's libunity.so.
 *
 * Unity 5 links zlib into the engine; 4.5 imports it from the system libz.so.
 * z_stream has the same layout under bionic and glibc on ARM32 (32-bit uLong
 * and pointers, no floats), so the host's zlib is bound directly. The
 * *Init_ entry points still check ZLIB_VERSION's major digit and
 * sizeof(z_stream), which is the check we want.
 */
#include <new>
#include <zlib.h>

#include "so_util.h"
#include "thunk_gen.h"

extern "C" void __cxa_guard_abort(void *guard);

DynLibFunction symtable_zlib[] = {
    NO_THUNK("crc32",         (uintptr_t)&crc32),
    NO_THUNK("adler32",       (uintptr_t)&adler32),
    NO_THUNK("inflateInit_",  (uintptr_t)&inflateInit_),
    NO_THUNK("inflateInit2_", (uintptr_t)&inflateInit2_),
    NO_THUNK("inflate",       (uintptr_t)&inflate),
    NO_THUNK("inflateEnd",    (uintptr_t)&inflateEnd),
    NO_THUNK("inflateReset",  (uintptr_t)&inflateReset),
    NO_THUNK("deflateInit_",  (uintptr_t)&deflateInit_),
    NO_THUNK("deflateInit2_", (uintptr_t)&deflateInit2_),
    NO_THUNK("deflate",       (uintptr_t)&deflate),
    NO_THUNK("deflateEnd",    (uintptr_t)&deflateEnd),
    NO_THUNK("deflateReset",  (uintptr_t)&deflateReset),
    NO_THUNK("compress",      (uintptr_t)&compress),
    NO_THUNK("uncompress",    (uintptr_t)&uncompress),
    /* std::nothrow is a data object: the game reads its address, never its bytes. */
    NO_THUNK("_ZSt7nothrow",  (uintptr_t)&std::nothrow),
    NO_THUNK("__cxa_guard_abort", (uintptr_t)&__cxa_guard_abort),
    { NULL, 0 },
};
