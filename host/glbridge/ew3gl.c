/*
 * ew3gl: host side of the GLES bridge. Replays the game's OpenGL ES calls,
 * serialised by the guest port (port/src/gl_bridge.cpp, EW3_GL_BRIDGE=1),
 * on the real GPU, presents the frames in a window and sends mouse and keys
 * back through the port's control channel. A C port of shadowgun-probe's
 * GlBridge.java.
 *
 *   Windows (ANGLE -> Direct3D 11):  host/build.sh ew3gl -> host/build-win/ew3gl.exe,
 *                                    fed over TCP by the guest under qemu in WSL (tools/play_win.sh)
 *   WSL (WSLg; software GL on Ubuntu 20.04's Mesa):  host/glbridge/build.sh, fifos (tools/play_gpu.sh)
 *
 * Settings (environment, EW3_ prefix like the guest's):
 *   EW3_GL_CONNECT=host:port   TCP to the guest (its EW3_GL_LISTEN), or else
 *   EW3_GL_CMD, EW3_GL_REPLY   the two fifos (created when missing; Linux only)
 *   EW3_WIDTH, EW3_HEIGHT      render surface, must match the guest's
 *   EW3_CONTROL_DIR            where ctl/commands lives (input)
 *   EW3_GL_VSYNC=1  EW3_GL_FULLSCREEN=0  EW3_GL_REPORT=5 (s)  EW3_GL_CHECK=0  EW3_GL_HUD=1  EW3_GL_ES=3  EW3_GL_DIAG=1  EW3_GL_KEYMAP=1
 *   EW3_GL_LOG=<file>          log there instead of stderr
 *
 * Modules, one translation unit (included in this order):
 *   ew3gl_base.inc     includes, logging, settings, id maps
 *   ew3gl_net.inc      transport: fifos or TCP
 *   ew3gl_pipe.inc     framing, payload decoding, replies
 *   gl_ops_gen.inc     generated: op numbers, op names, scalar GL calls
 *   ew3gl_hud.inc      frame-rate counter (on screen and in the title; F2)
 *   ew3gl_shot.inc     screenshots (F12, or `touch <control dir>/shot`)
 *   ew3gl_window.inc   SDL window + GLES context, present, mouse as one finger
 *   ew3gl_input.inc    WASD / mouse / keys -> the game's touch controls (EW3_GL_KEYMAP, F1)
 *   ew3gl_objects.inc  names, shaders, programs, buffers, textures
 *   ew3gl_draw.inc     uniforms, attributes, draws, invalidation
 *   ew3gl_query.inc    synchronous queries
 *   ew3gl_stats.inc    per-op statistics
 *   ew3gl_diag.inc     shader compile / program link failures, logged
 */
#include "ew3gl_base.inc"
#include "ew3gl_net.inc"
#include "ew3gl_pipe.inc"
#include "gl_ops_gen.inc"
#include "ew3gl_hud.inc"
#include "ew3gl_shot.inc"
#include "ew3gl_window.inc"
#include "ew3gl_input.inc"
#include "ew3gl_objects.inc"
#include "ew3gl_draw.inc"
#include "ew3gl_query.inc"
#include "ew3gl_stats.inc"
#include "ew3gl_diag.inc"

/* Executes one message whose payload is [g_p, g_end). */
static void dispatch(int op)
{
    if (gb_exec_simple(op))
        return;
    switch (op) {
    case GB_OP_SWAP: window_present(); break;
    case GB_OP_GEN: op_gen(); break;
    case GB_OP_DELETE: op_delete(); break;
    case GB_OP_BIND_BUFFER: op_bind_buffer(); break;
    case GB_OP_BUFFER_DATA: op_buffer_data(); break;
    case GB_OP_BUFFER_SUB_DATA: op_buffer_sub_data(); break;
    case GB_OP_TEX_IMAGE_2D: op_tex_image_2d(); break;
    case GB_OP_TEX_SUB_IMAGE_2D: op_tex_sub_image_2d(); break;
    case GB_OP_COMPRESSED_TEX_IMAGE_2D: op_compressed_tex_image_2d(); break;
    case GB_OP_COMPRESSED_TEX_SUB_IMAGE_2D: op_compressed_tex_sub_image_2d(); break;
    case GB_OP_TEX_IMAGE_3D: op_tex_image_3d(); break;
    case GB_OP_TEX_SUB_IMAGE_3D: op_tex_sub_image_3d(); break;
    case GB_OP_COMPRESSED_TEX_IMAGE_3D: op_compressed_tex_image_3d(); break;
    case GB_OP_COMPRESSED_TEX_SUB_IMAGE_3D: op_compressed_tex_sub_image_3d(); break;
    case GB_OP_CREATE_SHADER: op_create_shader(); break;
    case GB_OP_CREATE_PROGRAM: op_create_program(); break;
    case GB_OP_SHADER_SOURCE: op_shader_source(); break;
    case GB_OP_UNIFORM_V: op_uniform_v(); break;
    case GB_OP_ATTRIB_POINTER_VBO: op_attrib_pointer_vbo(); break;
    case GB_OP_DRAW_ARRAYS: op_draw_arrays(); break;
    case GB_OP_DRAW_ELEMENTS: op_draw_elements(); break;
    case GB_OP_INVALIDATE_FB: op_invalidate_fb(); break;
    case GB_OP_BIND_ATTRIB_LOC: op_bind_attrib_loc(); break;
    default:
        if (!exec_query(op))
            GLOG_LIMITED(g_exec_errors, 20, "unknown op %d", op);
        break;
    }
}

/* Reads and executes messages until the guest goes away or the window closes. */
static void run(void)
{
    while (g_alive) {
        if (!in_fill(8))
            return;
        uint32_t len, op;
        memcpy(&len, g_in + g_in_pos, 4);
        memcpy(&op, g_in + g_in_pos + 4, 4);
        if (len < 8 || (len & 3) || len > (256u << 20)) {
            glog("bad message length %u (op %u); the stream is out of step, stopping", len, op);
            return;
        }
        if (!in_fill(len))
            return;
        g_p = g_in + g_in_pos + 8;
        g_end = g_in + g_in_pos + len;
        g_underrun = 0;

        uint64_t t0 = now_ns();
        dispatch((int)op);
        diag_after((int)op, g_in + g_in_pos + 8, g_end);
        if (g_underrun)
            GLOG_LIMITED(g_exec_errors, 20, "op %s (%u): payload shorter than its fields (%u bytes)",
                         op_name((int)op), op, len);
        if (g_cfg.check_errors) {
            GLenum e = glGetError();
            if (e != GL_NO_ERROR)
                GLOG_LIMITED(g_gl_errors, 200, "glGetError 0x%04x after %s", e, op_name((int)op));
        }
        stats_note((int)op, len, now_ns() - t0);
        g_in_pos += len;
        if (op == GB_OP_SWAP)
            stats_maybe_report();
    }
}

#ifndef _WIN32
static void on_signal(int sig)
{
    (void)sig;
    g_alive = 0;
}
#endif

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    g_t0_ns = now_ns();
    const char *log_path = cfg_str("GL_LOG", NULL);
    if (log_path && !(g_log = fopen(log_path, "a")))
        fprintf(stderr, "ew3gl: cannot open %s, logging to stderr\n", log_path);
    config_load();
#ifndef _WIN32
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#endif
    /* The window first: the guest's first queries (glGetString) need a live context. */
    window_open();
    transport_open();
    glog("waiting for the game (%dx%d)", g_cfg.width, g_cfg.height);
    run();
    glog("stopped after %ld frames", g_swaps);
    window_close();
    return 0;
}
