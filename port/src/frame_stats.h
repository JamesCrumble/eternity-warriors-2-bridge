#pragma once
/*
 * Counters the port's own wrappers bump during a frame, reset at
 * gl_bridge_profile_step(1) and read by slow_frame() at step(0) so a hitch line
 * says what the port saw happen on that frame without an extra pass over the log.
 */
#include <stdint.h>

struct PortFrameStats {
    unsigned opens;             /* bionic_open + bionic_fopen calls */
    char     last_open[64];     /* tail of the last path opened this frame */
    unsigned io_reads;          /* read / pread64 calls */
    uint64_t io_bytes;          /* bytes read by read / pread64 this frame */
    unsigned textures;          /* glTex*Image* + glCompressedTex*Image* calls */
    uint64_t tex_bytes;         /* bytes uploaded by those calls */
    uint64_t buf_bytes;         /* glBufferData + glBufferSubData + unmap diff bytes */
    uint64_t vtx_bytes;         /* client-side vertex/index bytes captured in draws */
    unsigned draws;             /* glDrawArrays + glDrawElements calls */
    unsigned uniforms;          /* non-redundant glUniform* calls emitted */
    unsigned shaders;           /* glCompileShader calls */
    unsigned links;             /* glLinkProgram calls */
    unsigned sync_queries;      /* synchronous host round-trips (query()) */
    uint64_t sync_ns;           /* time spent blocked in query() waiting for host */
    unsigned touches;           /* touch events injected before this frame */
    int      touch_age_ms;      /* event-to-injection age of the last touch, or -1 */
};

extern PortFrameStats g_port_frame;
