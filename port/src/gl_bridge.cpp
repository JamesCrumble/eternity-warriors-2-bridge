/*
 * GLES bridge, guest side.
 *
 * The game is an ARM32 process under qemu-user, where the only OpenGL on offer
 * is Mesa's software rasteriser. This file replaces every GL entry point the
 * game can reach with a stub that serialises the call into a pipe; the Android
 * app on the other end replays the stream on the phone's GPU (GlBridge.java).
 *
 * Wire format, guest -> host, little endian:
 *     u32 total length (this header included, multiple of 4)   u32 op   payload
 * Synchronous queries are answered on a second pipe:
 *     u32 length   bytes
 *
 * What makes it work without a round trip per call:
 *  - object names (textures, buffers, framebuffers, renderbuffers, shaders,
 *    programs) are allocated here and translated to real names by the host;
 *  - state the game can only have set itself is never asked for back;
 *  - uniform and attribute locations are queried once per program and cached;
 *  - client-side vertex arrays (pointers into guest memory) are captured at
 *    draw time and shipped inside the draw message, which is the only moment
 *    their contents are defined.
 *
 * Calls that must see the host's answer (compile/link status, locations, info
 * logs, glGet*, glReadPixels) flush the stream and block on the reply.
 *
 * Ops and scalar stubs come from android/tools/gen_glbridge.py.
 */
#define PORT_GL_NO_REDIRECT
#include "gl_bridge.h"

#include <SDL2/SDL.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>

#include <algorithm>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "khronos/glad.h"
#include "frame_stats.h"
#include "so_util.h"
#include "trace.h"
#include "syscall_prof.h"
#include "port_env.h"

PortFrameStats g_port_frame;
extern so_module *ew3_module(void);

/* ------------------------------------------------------------- transport */

static int  g_cmd_fd = -1;
static int  g_rep_fd = -1;
static int  g_enabled = -1;
static long g_frames = 0;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
alignas(8) static uint8_t g_out[1 << 20];
static size_t  g_out_len = 0;

/*
 * <PREFIX>_GL_PROFILE: 1 times each frame's step() and swap wait; 2 also
 * times every bridge call. Under qemu-user clock_gettime is a real syscall,
 * so level 2 inflates what it measures - use it for proportions only.
 */
static int      g_profile = -1;
static int      g_lock_depth;
static uint64_t g_lock_t0, g_bridge_ns, g_bridge_calls;
static uint64_t g_write_ns, g_sync_ns, g_last_swap_ns;

static inline uint64_t now_ns()
{
    uint64_t v = 0;
    __asm__ __volatile__("mrrc p15, 1, %Q0, %R0, c14" : "=r"(v));
    return v << 4;
}

/*
 * The engine makes every GL call from its render thread, and under emulation
 * a mutex per call (thousands per frame) is a real cost. So the first thread
 * owns the bridge without locking; if a second thread ever calls in, locking
 * is switched on for good from then on.
 */
static pthread_t g_owner;
static bool      g_owner_set;
static bool      g_multi;

struct Lock {
    bool locked;
    Lock() : locked(false)
    {
        if (__builtin_expect(g_multi, 0)) {
            pthread_mutex_lock(&g_lock);
            locked = true;
        } else {
            pthread_t me = pthread_self();
            if (!g_owner_set) {
                g_owner = me;
                g_owner_set = true;
                trace("gl bridge: GL owner thread tid=%ld", (long)syscall(SYS_gettid));
            } else if (!pthread_equal(me, g_owner)) {
                g_multi = true;
                trace("gl bridge: GL called from a second thread; locking from now on");
                pthread_mutex_lock(&g_lock);
                locked = true;
            }
        }
        if (g_profile > 0 && g_lock_depth++ == 0) {
            g_lock_t0 = now_ns();
            g_bridge_calls++;
        }
    }
    ~Lock()
    {
        if (g_profile > 0 && --g_lock_depth == 0)
            g_bridge_ns += now_ns() - g_lock_t0;
        if (locked)
            pthread_mutex_unlock(&g_lock);
    }
};

/* The host is gone (app closed or crashed); nothing sensible is left to do. */
static void gone(const char *what)
{
    fprintf(stderr, "GLBRIDGE: %s failed (%s): host is gone, exiting\n", what, strerror(errno));
    fflush(stderr);
    _exit(0);
}

static void write_all(const void *p, size_t n)
{
    const uint8_t *s = (const uint8_t *)p;
    const uint64_t t0 = g_profile > 0 ? now_ns() : sysprof_now();
    while (n > 0) {
        ssize_t w = write(g_cmd_fd, s, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            gone("write");
        }
        s += w;
        n -= (size_t)w;
    }
    const uint64_t dt = (g_profile > 0 ? now_ns() : sysprof_now()) - t0;
    if (g_profile > 0)
        g_write_ns += dt;
    sysprof_note(SP_GL_WRITE, NULL, dt);
}

static void read_all(void *p, size_t n)
{
    uint8_t *d = (uint8_t *)p;
    while (n > 0) {
        ssize_t r = read(g_rep_fd, d, n);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            gone("read");
        }
        if (r == 0) {
            errno = EPIPE;
            gone("read (eof)");
        }
        d += r;
        n -= (size_t)r;
    }
}

static void out_flush()
{
    if (g_out_len) {
        write_all(g_out, g_out_len);
        g_out_len = 0;
    }
}

static void out_raw(const void *p, size_t n)
{
    if (n > sizeof(g_out) - g_out_len) {
        out_flush();
        if (n >= sizeof(g_out) / 2) {
            write_all(p, n);
            return;
        }
    }
    memcpy(g_out + g_out_len, p, n);
    g_out_len += n;
}

static void msg_begin(int op, size_t payload)
{
    uint32_t h[2] = { (uint32_t)((8 + payload + 3) & ~3u), (uint32_t)op };
    out_raw(h, 8);
}

static void msg_end(size_t payload)
{
    static const uint8_t zeros[4] = {0, 0, 0, 0};
    size_t pad = ((payload + 3) & ~(size_t)3) - payload;
    if (pad)
        out_raw(zeros, pad);
}

static void send_msg(int op, const void *hdr, size_t hlen, const void *data, size_t dlen)
{
    /* The common case - a few words of arguments and no data - written as
     * words straight into the buffer: no memcpy calls, which are expensive
     * for tiny sizes when the code is emulated. hlen is always whole words. */
    if (__builtin_expect(dlen == 0, 1) && 8 + hlen <= sizeof(g_out) - g_out_len) {
        uint32_t *w = (uint32_t *)(g_out + g_out_len);
        const uint32_t *h = (const uint32_t *)hdr;
        size_t n = hlen / 4;
        w[0] = (uint32_t)(8 + hlen);
        w[1] = (uint32_t)op;
        for (size_t i = 0; i < n; i++)
            w[2 + i] = h[i];
        g_out_len += 8 + hlen;
        return;
    }
    size_t payload = hlen + dlen;
    msg_begin(op, payload);
    if (hlen)
        out_raw(hdr, hlen);
    if (dlen)
        out_raw(data, dlen);
    msg_end(payload);
}

static void gb_cmd(int op, const void *hdr, size_t hlen, const void *data, size_t dlen)
{
    Lock l;
    send_msg(op, hdr, hlen, data, dlen);
}

static void emit(int op, std::initializer_list<int32_t> hdr, const void *data = NULL, size_t dlen = 0)
{
    gb_cmd(op, hdr.begin(), hdr.size() * 4, data, dlen);
}

static inline void gb_put(int32_t *d, int v) { *d = v; }
static inline void gb_put(int32_t *d, unsigned v) { *d = (int32_t)v; }
static inline void gb_put(int32_t *d, unsigned char v) { *d = v; }
static inline void gb_put(int32_t *d, float v) { memcpy(d, &v, 4); }

/* Frames sent but not yet presented by the host; the guest may run <PREFIX>_GL_INFLIGHT frames ahead
 * (default 2: while the host replays one frame and waits for vsync, the guest already builds the next). */
static int g_inflight = 0;
static int g_max_inflight = 2;
static const uint32_t kAck = 0xFFFFFFFFu;

/* Next reply length, absorbing frame acknowledgements that arrive in between. */
static uint32_t read_reply_len()
{
    for (;;) {
        uint32_t len = 0;
        read_all(&len, 4);
        if (len != kAck)
            return len;
        if (g_inflight > 0)
            g_inflight--;
    }
}

/* Sends a query and waits for the reply. */
static void query(int op, std::initializer_list<int32_t> args, std::vector<uint8_t> &rep,
                  const void *data = NULL, size_t dlen = 0)
{
    Lock l;
    send_msg(op, args.begin(), args.size() * 4, data, dlen);
    out_flush();
    uint64_t q0 = g_profile > 0 ? now_ns() : 0;
    uint32_t len = read_reply_len();
    if (len > (256u << 20)) {
        errno = EPROTO;
        gone("reply length");
    }
    rep.resize(len);
    if (len)
        read_all(rep.data(), len);
    if (g_profile > 0) {
        uint64_t dt = now_ns() - q0;
        g_sync_ns += dt;
        g_port_frame.sync_queries++;
        g_port_frame.sync_ns += dt;
    }
}

static int32_t query_int(int op, std::initializer_list<int32_t> args, const void *data = NULL, size_t dlen = 0)
{
    std::vector<uint8_t> rep;
    query(op, args, rep, data, dlen);
    int32_t v = 0;
    if (rep.size() >= 4)
        memcpy(&v, rep.data(), 4);
    return v;
}

#include "gl_bridge_gen.inc"

/* ------------------------------------------------------------ local state */

enum { KIND_TEX = 0, KIND_BUF = 1, KIND_FBO = 2, KIND_RBO = 3, KIND_SHADER = 4, KIND_PROG = 5 };
static uint32_t g_next_obj[6];

static GLuint g_array_buf = 0;
static GLuint g_elem_buf = 0;
static GLuint g_host_array_buf = 0;
static GLuint g_host_elem_buf = 0;
static GLuint g_fbo = 0;
static GLuint g_rbo = 0;
static uint32_t g_elem_gen = 1;
static std::vector<uint32_t> g_buf_size;
static std::vector<GLenum>   g_buf_usage;

static int g_unpack_align = 4;
static int g_pack_align = 4;
static int g_unpack_row_length = 0;

static bool  g_viewport_set = false;
static GLint g_viewport[4] = { 0, 0, 0, 0 };
static bool  g_scissor_set = false;
static GLint g_scissor[4] = { 0, 0, 0, 0 };
static GLint g_color_mask[4] = { 1, 1, 1, 1 };
static GLint g_depth_mask = 1;

/* Mirrors of GL state the redundancy filters below compare against. */
struct UniSlot {
    uint8_t  kind;    /* 0 = unknown */
    uint8_t  words;
    uint32_t v[16];
};
static std::vector<std::vector<UniSlot> > g_uni;   /* [program][location] */
static GLuint   g_cur_prog;
static GLenum   g_active_unit = 0x84C0;            /* GL_TEXTURE0 */
static GLuint   g_bound_tex[32][2];                /* [unit][2D, cube map] */
static std::vector<std::vector<uint8_t> > g_buf_shadow;  /* VBO & IBO shadow buffers */
struct MapState { std::vector<uint8_t> buf; bool active = false; };
static std::map<GLuint, MapState> g_map;                 /* per-buffer OES_mapbuffer state */

struct Attrib {
    bool enabled;
    bool client;
    GLuint vbo;
    GLint size;
    GLenum type;
    GLboolean normalized;
    GLsizei stride;
    const void *ptr;
};
static const int kMaxAttribs = 16;
static Attrib g_attr[kMaxAttribs];

static std::map<std::pair<uint32_t, std::string>, int> g_attrib_loc;
static std::map<std::pair<uint32_t, std::string>, int> g_uniform_loc;

static int comps(GLenum f)
{
    switch (f) {
    case 0x1906: case 0x1909: case 0x1903: case 0x8D94: case 0x1902: case 0x84F9: return 1;
    case 0x190A: case 0x8227: case 0x8228: return 2;
    case 0x1907: case 0x8D98: return 3;
    default: return 4;
    }
}

static int bytes_per_pixel(GLenum format, GLenum type)
{
    int c = comps(format);
    switch (type) {
    case 0x1401: case 0x1400: return c;
    case 0x1403: case 0x1402: case 0x140B: case 0x8D61: return 2 * c;
    case 0x8363: case 0x8033: case 0x8034: return 2;
    case 0x1405: case 0x1404: case 0x1406: return 4 * c;
    case 0x84FA: case 0x8368: case 0x8C3B: case 0x8C3E: return 4;
    default: return c;
    }
}

/* Bytes a glTexImage call or glReadPixels touches, honouring the row alignment. */
static size_t image_bytes(GLsizei w, GLsizei h, GLsizei d, GLenum format, GLenum type, int align, int row_length)
{
    if (w <= 0 || h <= 0 || d <= 0)
        return 0;
    size_t bpp = (size_t)bytes_per_pixel(format, type);
    size_t row = (size_t)(row_length > 0 ? row_length : w) * bpp;
    if (align > 1)
        row = (row + (size_t)align - 1) / (size_t)align * (size_t)align;
    size_t rows = (size_t)h * (size_t)d;
    return row * (rows - 1) + (size_t)w * bpp;
}

static int type_size(GLenum t)
{
    switch (t) {
    case 0x1400: case 0x1401: return 1;
    case 0x1402: case 0x1403: case 0x140B: return 2;
    default: return 4;
    }
}

/* -------------------------------------------------- stateful call wrappers */

static void gb_glPixelStorei(GLenum pname, GLint param)
{
    if (pname == 0x0CF5) {
        if (g_unpack_align == param) return;
        g_unpack_align = param;
    } else if (pname == 0x0D05) {
        if (g_pack_align == param) return;
        g_pack_align = param;
    } else if (pname == 0x0CF2) {
        if (g_unpack_row_length == param) return;
        g_unpack_row_length = param;
    }
    gb_raw_glPixelStorei(pname, param);
}

/*
 * Everything a freshly linked program will be asked, fetched in one round trip.
 *
 * After linking, the engine walks the program: status, log length, the count
 * of attributes and uniforms, then glGetActive* and glGet*Location for each -
 * a couple of dozen synchronous queries per shader, each draining the whole
 * pipeline, during loading and whenever a new effect first appears in play.
 * The host answers them all at once (GB_OP_Q_PROGRAM_INFO) and they are served
 * from here; anything not covered still goes to the host.
 */
enum { GB_OP_Q_PROGRAM_INFO = 250 };

struct ProgVar {
    GLint size;
    GLenum type;
    std::string name;
};
struct ProgInfo {
    bool valid;
    GLint link_status, log_len, max_attr, max_uni;
    std::vector<ProgVar> attrs, unis;
};
static std::vector<ProgInfo> g_prog_info;

static void program_info_fetch(GLuint program)
{
    static int on = -1;
    if (on < 0)
        on = port_getenv_long("PROGRAM_INFO", 1) != 0;
    if (!on || program == 0)
        return;
    std::vector<uint8_t> rep;
    query(GB_OP_Q_PROGRAM_INFO, { (int32_t)program }, rep);
    if (program >= g_prog_info.size())
        g_prog_info.resize((size_t)program + 16);
    ProgInfo &pi = g_prog_info[program];
    pi = ProgInfo();
    /* link, loglen, nattr, maxattr, nuni, maxuni; then per var: size, type, loc, namelen, name (padded 4). */
    if (rep.size() < 24)
        return;
    const uint8_t *p = rep.data(), *end = p + rep.size();
    int32_t h[6];
    memcpy(h, p, 24);
    p += 24;
    pi.link_status = h[0];
    pi.log_len = h[1];
    pi.max_attr = h[3];
    pi.max_uni = h[5];
    for (int pass = 0; pass < 2; pass++) {
        int n = pass == 0 ? h[2] : h[4];
        std::vector<ProgVar> &vars = pass == 0 ? pi.attrs : pi.unis;
        for (int i = 0; i < n; i++) {
            if (end - p < 16)
                return;
            int32_t v[4];
            memcpy(v, p, 16);
            p += 16;
            size_t nl = (size_t)v[3];
            if ((size_t)(end - p) < ((nl + 3) & ~(size_t)3))
                return;
            ProgVar var;
            var.size = v[0];
            var.type = (GLenum)v[1];
            var.name.assign((const char *)p, nl);
            p += (nl + 3) & ~(size_t)3;
            vars.push_back(var);
            /* Locations go straight into the caches glGet*Location reads. */
            std::map<std::pair<uint32_t, std::string>, int> &cache = pass == 0 ? g_attrib_loc : g_uniform_loc;
            cache[std::make_pair((uint32_t)program, var.name)] = v[2];
            if (pass == 1 && var.name.size() > 3 && var.name.compare(var.name.size() - 3, 3, "[0]") == 0)
                cache[std::make_pair((uint32_t)program, var.name.substr(0, var.name.size() - 3))] = v[2];
        }
    }
    pi.valid = true;
}

static const ProgInfo *program_info(GLuint program)
{
    return program < g_prog_info.size() && g_prog_info[program].valid ? &g_prog_info[program] : NULL;
}

static void gb_glLinkProgram(GLuint program)
{
    g_port_frame.links++;
    {
        Lock l;
        g_attrib_loc.clear();
        g_uniform_loc.clear();
    }
    /* Linking resets the program's uniforms; forget what they held. */
    if (program < g_uni.size())
        g_uni[program].clear();
    gb_raw_glLinkProgram(program);
    program_info_fetch(program);
}

static void gbf_glCompileShader(GLuint s)
{
    g_port_frame.shaders++;
    gb_glCompileShader(s);
}

static void gb_glEnableVertexAttribArray(GLuint index)
{
    if (index < (GLuint)kMaxAttribs) {
        if (g_attr[index].enabled)
            return;
        g_attr[index].enabled = true;
    }
    gb_raw_glEnableVertexAttribArray(index);
}

static void gb_glDisableVertexAttribArray(GLuint index)
{
    if (index < (GLuint)kMaxAttribs) {
        if (!g_attr[index].enabled)
            return;
        g_attr[index].enabled = false;
    }
    gb_raw_glDisableVertexAttribArray(index);
}

/* ------------------------------------------------------------ objects */

static void gen_objects(int kind, GLsizei n, GLuint *out)
{
    if (n <= 0 || !out)
        return;
    Lock l;
    std::vector<int32_t> w((size_t)n + 2);
    w[0] = kind;
    w[1] = n;
    for (GLsizei i = 0; i < n; i++) {
        out[i] = ++g_next_obj[kind];
        w[2 + i] = (int32_t)out[i];
    }
    send_msg(GB_OP_GEN, w.data(), w.size() * 4, NULL, 0);
}

static void delete_objects(int kind, GLsizei n, const GLuint *ids)
{
    if (n <= 0 || !ids)
        return;
    std::vector<int32_t> w((size_t)n + 2);
    w[0] = kind;
    w[1] = n;
    for (GLsizei i = 0; i < n; i++)
        w[2 + i] = (int32_t)ids[i];
    gb_cmd(GB_OP_DELETE, w.data(), w.size() * 4, NULL, 0);
    if (kind == KIND_BUF) {
        g_elem_gen++;
        for (GLsizei i = 0; i < n; i++) {
            if (ids[i] == g_array_buf) g_array_buf = 0;
            if (ids[i] == g_elem_buf) g_elem_buf = 0;
            if (ids[i] == g_host_array_buf) g_host_array_buf = 0;
            if (ids[i] == g_host_elem_buf) g_host_elem_buf = 0;
            for (int a = 0; a < kMaxAttribs; a++)
                if (g_attr[a].vbo == ids[i]) g_attr[a].vbo = 0;
            if (ids[i] < g_buf_shadow.size())
                std::vector<uint8_t>().swap(g_buf_shadow[ids[i]]);
            if (ids[i] < g_buf_size.size())
                g_buf_size[ids[i]] = 0;
            if (ids[i] < g_buf_usage.size())
                g_buf_usage[ids[i]] = 0;
            g_map.erase(ids[i]);
        }
    } else if (kind == KIND_FBO) {
        for (GLsizei i = 0; i < n; i++)
            if (ids[i] == g_fbo) g_fbo = 0;
    } else if (kind == KIND_RBO) {
        for (GLsizei i = 0; i < n; i++)
            if (ids[i] == g_rbo) g_rbo = 0;
    }
}

static void gb_glGenTextures(GLsizei n, GLuint *t) { gen_objects(KIND_TEX, n, t); }
static void gb_glGenBuffers(GLsizei n, GLuint *b) { gen_objects(KIND_BUF, n, b); }
static void gb_glGenFramebuffers(GLsizei n, GLuint *f) { gen_objects(KIND_FBO, n, f); }
static void gb_glGenRenderbuffers(GLsizei n, GLuint *r) { gen_objects(KIND_RBO, n, r); }
static void gb_glDeleteTextures(GLsizei n, const GLuint *t)
{
    /* GL unbinds a deleted texture everywhere it is bound. */
    for (GLsizei i = 0; t && i < n; i++)
        for (int u = 0; u < 32; u++)
            for (int k = 0; k < 2; k++)
                if (g_bound_tex[u][k] == t[i])
                    g_bound_tex[u][k] = 0;
    delete_objects(KIND_TEX, n, t);
}
static void gb_glDeleteBuffers(GLsizei n, const GLuint *b) { delete_objects(KIND_BUF, n, b); }
static void gb_glDeleteFramebuffers(GLsizei n, const GLuint *f) { delete_objects(KIND_FBO, n, f); }
static void gb_glDeleteRenderbuffers(GLsizei n, const GLuint *r) { delete_objects(KIND_RBO, n, r); }

static GLuint gb_glCreateShader(GLenum type)
{
    Lock l;
    GLuint id = ++g_next_obj[KIND_SHADER];
    int32_t w[2] = { (int32_t)type, (int32_t)id };
    send_msg(GB_OP_CREATE_SHADER, w, 8, NULL, 0);
    return id;
}

static GLuint gb_glCreateProgram(void)
{
    Lock l;
    GLuint id = ++g_next_obj[KIND_PROG];
    int32_t w[1] = { (int32_t)id };
    send_msg(GB_OP_CREATE_PROGRAM, w, 4, NULL, 0);
    return id;
}

static void gb_glShaderSource(GLuint shader, GLsizei count, const GLchar *const *strings, const GLint *lengths)
{
    std::string src;
    for (GLsizei i = 0; i < count; i++) {
        if (!strings[i])
            continue;
        if (lengths && lengths[i] >= 0)
            src.append(strings[i], (size_t)lengths[i]);
        else
            src.append(strings[i]);
    }
    emit(GB_OP_SHADER_SOURCE, { (int32_t)shader, (int32_t)src.size() }, src.data(), src.size());
}

/* ------------------------------------------------------------ buffers */

static void gb_glBindBuffer(GLenum target, GLuint buffer)
{
    if (target == 0x8892) {
        g_array_buf = buffer;
        /* Buffer 0 is only bound by the guest before setting client vertex pointers;
         * the host never needs GL_ARRAY_BUFFER=0 because client blocks bind scratch
         * VBOs and restore g_cur_array. Skipping 0 also avoids re-emitting the VBO
         * bind when the guest returns to the same VBO after a client draw. */
        if (buffer == 0 || g_host_array_buf == buffer)
            return;
        g_host_array_buf = buffer;
    } else if (target == 0x8893) {
        g_elem_buf = buffer;
        if (buffer == 0 || g_host_elem_buf == buffer)
            return;
        g_host_elem_buf = buffer;
    }
    emit(GB_OP_BIND_BUFFER, { (int32_t)target, (int32_t)buffer });
}

/*
 * Vertex and index buffers, shadowed.
 * 1) A draw that takes its indices from a buffer but its vertices from client
 *    memory uses the shadow copy to know which vertices the draw reaches.
 * 2) In Town, the engine re-uploads ~315 KB/frame via glBufferData; comparing
 *    against the shadow copy skips unchanged buffers and sends only modified
 *    byte ranges via GB_OP_BUFFER_SUB_DATA.
 */
static std::vector<uint8_t> *buf_shadow(GLuint id)
{
    if (id == 0)
        return NULL;
    if (id >= g_buf_shadow.size())
        g_buf_shadow.resize((size_t)id + 64);
    return &g_buf_shadow[id];
}

static std::vector<uint8_t> *elem_shadow(GLuint id)
{
    return buf_shadow(id);
}

static void shadow_write(GLenum target, size_t offset, const void *data, size_t size, bool whole)
{
    GLuint id = target == 0x8893 ? g_elem_buf : (target == 0x8892 ? g_array_buf : 0);
    std::vector<uint8_t> *s = buf_shadow(id);
    if (!s)
        return;
    if (target == 0x8893)
        g_elem_gen++;
    if (whole)
        s->assign(size, 0);
    if (offset + size > s->size())
        s->resize(offset + size, 0);
    if (data && size)
        memcpy(s->data() + offset, data, size);
}

static void emit_buffer_diff(GLenum target, std::vector<uint8_t> *s, size_t offset, const void *data, size_t size)
{
    const uint8_t *src = (const uint8_t *)data;
    uint8_t *dst = s->data() + offset;
    size_t first = 0;
    while (first + 8 <= size) {
        uint64_t a, b;
        memcpy(&a, dst + first, 8);
        memcpy(&b, src + first, 8);
        if (a != b)
            break;
        first += 8;
    }
    while (first < size && dst[first] == src[first])
        first++;
    if (first == size)
        return;
    if (target == 0x8893)
        g_elem_gen++;
    size_t last = size;
    while (last >= first + 8) {
        uint64_t a, b;
        memcpy(&a, dst + last - 8, 8);
        memcpy(&b, src + last - 8, 8);
        if (a != b)
            break;
        last -= 8;
    }
    while (last > first && dst[last - 1] == src[last - 1])
        last--;
    size_t diff_len = last - first;
    memcpy(dst + first, src + first, diff_len);
    g_port_frame.buf_bytes += diff_len;
    emit(GB_OP_BUFFER_SUB_DATA,
         { (int32_t)target, (int32_t)(offset + first), (int32_t)diff_len, (int32_t)diff_len },
         src + first, diff_len);
}

static void gb_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
    GLuint id = target == 0x8893 ? g_elem_buf : (target == 0x8892 ? g_array_buf : 0);
    if (id >= g_buf_size.size()) {
        g_buf_size.resize((size_t)id + 64, 0);
        g_buf_usage.resize((size_t)id + 64, 0);
    }
    size_t sz = size > 0 ? (size_t)size : 0;
    std::vector<uint8_t> *s = buf_shadow(id);
    bool same_alloc = (id != 0 && s && g_buf_size[id] == (uint32_t)sz && g_buf_usage[id] == usage && s->size() == sz && sz > 0);
    g_buf_size[id] = (uint32_t)sz;
    g_buf_usage[id] = usage;

    if (same_alloc) {
        if (!data) {
            /* Buffer orphaning of identical size/usage: host buffer is already allocated. */
            return;
        }
        emit_buffer_diff(target, s, 0, data, sz);
        return;
    }

    if (data && size > 0)
        g_port_frame.buf_bytes += sz;
    shadow_write(target, 0, data, sz, true);
    emit(GB_OP_BUFFER_DATA, { (int32_t)target, (int32_t)size, (int32_t)usage, data ? (int32_t)size : -1 },
         data, data && size > 0 ? sz : 0);
}

static void gb_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
{
    if (!data || size <= 0 || offset < 0)
        return;
    GLuint id = target == 0x8893 ? g_elem_buf : (target == 0x8892 ? g_array_buf : 0);
    std::vector<uint8_t> *s = buf_shadow(id);
    size_t off = (size_t)offset, sz = (size_t)size;
    if (s && off + sz <= s->size()) {
        emit_buffer_diff(target, s, off, data, sz);
        return;
    }
    g_port_frame.buf_bytes += sz;
    shadow_write(target, off, data, sz, false);
    emit(GB_OP_BUFFER_SUB_DATA, { (int32_t)target, (int32_t)offset, (int32_t)size, (int32_t)size }, data, sz);
}

/* The engine maps multiple buffers simultaneously for multithreaded skinning;
 * keep per-buffer scratch memory initialized from shadow and upload only the modified range on unmap. */
static void *gb_glMapBufferOES(GLenum target, GLenum access)
{
    (void)access;
    GLuint id = target == 0x8893 ? g_elem_buf : (target == 0x8892 ? g_array_buf : 0);
    if (id == 0 || id >= g_buf_size.size() || g_buf_size[id] == 0)
        return NULL;
    size_t sz = g_buf_size[id];
    MapState &ms = g_map[id];
    ms.buf.resize(sz);
    std::vector<uint8_t> *s = buf_shadow(id);
    if (s && s->size() == sz)
        memcpy(ms.buf.data(), s->data(), sz);
    else
        memset(ms.buf.data(), 0, sz);
    ms.active = true;
    return ms.buf.data();
}

static GLboolean gb_glUnmapBufferOES(GLenum target)
{
    GLuint id = target == 0x8893 ? g_elem_buf : (target == 0x8892 ? g_array_buf : 0);
    if (id == 0)
        return GL_FALSE;
    std::map<GLuint, MapState>::iterator it = g_map.find(id);
    if (it == g_map.end() || !it->second.active)
        return GL_FALSE;
    MapState &ms = it->second;
    ms.active = false;
    std::vector<uint8_t> *s = buf_shadow(id);
    size_t sz = ms.buf.size();
    const uint8_t *ptr = ms.buf.data();
    if (s && s->size() == sz) {
        emit_buffer_diff(target, s, 0, ptr, sz);
    } else {
        shadow_write(target, 0, ptr, sz, false);
        emit(GB_OP_BUFFER_SUB_DATA, { (int32_t)target, 0, (int32_t)sz, (int32_t)sz }, ptr, sz);
    }
    return GL_TRUE;
}

/* ------------------------------------------------------------ textures */


/* Each distinct texture upload format is logged once: which formats the engine really uses on this driver. */
static void note_tex_format(const char *kind, unsigned fmt, unsigned type, int w, int h, int level)
{
    static std::map<std::pair<std::string, unsigned long long>, int> seen;
    unsigned long long key = ((unsigned long long)fmt << 32) | type;
    std::pair<std::string, unsigned long long> k(kind, key);
    if (seen.find(k) != seen.end())
        return;
    seen[k] = 1;
    trace("gl bridge: texture upload %s format 0x%x type 0x%x first seen as %dx%d level %d", kind, fmt, type, w, h, level);
}

static void gb_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                            GLint border, GLenum format, GLenum type, const void *pixels)
{
    size_t n = pixels ? image_bytes(width, height, 1, format, type, g_unpack_align, g_unpack_row_length) : 0;
    if (pixels) { g_port_frame.textures++; g_port_frame.tex_bytes += n; }
    note_tex_format("2D", internalformat * 0x10000u + format, type, width, height, level);
    emit(GB_OP_TEX_IMAGE_2D, { (int32_t)target, level, internalformat, width, height, border, (int32_t)format,
                               (int32_t)type, pixels ? (int32_t)n : -1 }, pixels, n);
}

static void gb_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                               GLsizei height, GLenum format, GLenum type, const void *pixels)
{
    if (!pixels)
        return;
    size_t n = image_bytes(width, height, 1, format, type, g_unpack_align, g_unpack_row_length);
    g_port_frame.textures++; g_port_frame.tex_bytes += n;
    emit(GB_OP_TEX_SUB_IMAGE_2D, { (int32_t)target, level, xoffset, yoffset, width, height, (int32_t)format,
                                   (int32_t)type, (int32_t)n }, pixels, n);
}

static void gb_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                                      GLsizei height, GLint border, GLsizei imageSize, const void *data)
{
    size_t n = data && imageSize > 0 ? (size_t)imageSize : 0;
    if (n) { g_port_frame.textures++; g_port_frame.tex_bytes += n; }
    note_tex_format("compressed", internalformat, 0, width, height, level);
    emit(GB_OP_COMPRESSED_TEX_IMAGE_2D, { (int32_t)target, level, (int32_t)internalformat, width, height, border,
                                          imageSize, data ? (int32_t)n : -1 }, data, n);
}

static void gb_glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                                         GLsizei height, GLenum format, GLsizei imageSize, const void *data)
{
    if (!data || imageSize <= 0)
        return;
    g_port_frame.textures++; g_port_frame.tex_bytes += (size_t)imageSize;
    emit(GB_OP_COMPRESSED_TEX_SUB_IMAGE_2D, { (int32_t)target, level, xoffset, yoffset, width, height,
                                              (int32_t)format, imageSize, imageSize }, data, (size_t)imageSize);
}

static void gb_glTexImage3DOES(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height,
                               GLsizei depth, GLint border, GLenum format, GLenum type, const void *pixels)
{
    size_t n = pixels ? image_bytes(width, height, depth, format, type, g_unpack_align, g_unpack_row_length) : 0;
    if (pixels) { g_port_frame.textures++; g_port_frame.tex_bytes += n; }
    emit(GB_OP_TEX_IMAGE_3D, { (int32_t)target, level, (int32_t)internalformat, width, height, depth, border,
                               (int32_t)format, (int32_t)type, pixels ? (int32_t)n : -1 }, pixels, n);
}

static void gb_glTexSubImage3DOES(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                                  GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type,
                                  const void *pixels)
{
    if (!pixels)
        return;
    size_t n = image_bytes(width, height, depth, format, type, g_unpack_align, g_unpack_row_length);
    g_port_frame.textures++; g_port_frame.tex_bytes += n;
    emit(GB_OP_TEX_SUB_IMAGE_3D, { (int32_t)target, level, xoffset, yoffset, zoffset, width, height, depth,
                                   (int32_t)format, (int32_t)type, (int32_t)n }, pixels, n);
}

static void gb_glCompressedTexImage3DOES(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                                         GLsizei height, GLsizei depth, GLint border, GLsizei imageSize,
                                         const void *data)
{
    size_t n = data && imageSize > 0 ? (size_t)imageSize : 0;
    if (n) { g_port_frame.textures++; g_port_frame.tex_bytes += n; }
    emit(GB_OP_COMPRESSED_TEX_IMAGE_3D, { (int32_t)target, level, (int32_t)internalformat, width, height, depth,
                                          border, imageSize, data ? (int32_t)n : -1 }, data, n);
}

static void gb_glCompressedTexSubImage3DOES(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                            GLint zoffset, GLsizei width, GLsizei height, GLsizei depth,
                                            GLenum format, GLsizei imageSize, const void *data)
{
    if (!data || imageSize <= 0)
        return;
    g_port_frame.textures++; g_port_frame.tex_bytes += (size_t)imageSize;
    emit(GB_OP_COMPRESSED_TEX_SUB_IMAGE_3D, { (int32_t)target, level, xoffset, yoffset, zoffset, width, height,
                                              depth, (int32_t)format, imageSize, imageSize }, data, (size_t)imageSize);
}

/* ------------------------------------------------------------ uniforms */

/* ------------------------------------------------------- redundant state */

/*
 * The engine re-sends state it has already set: the same texture to the same
 * unit, the same program, uniforms with the values they already hold. Each
 * of those costs an encode here (emulated) and a JNI call on the host, and a
 * frame carries a few thousand. What GL would ignore is not sent.
 *
 * Uniform values belong to the program, so they are cached per program and
 * location, up to a mat4; anything larger - bone palettes - always goes, and
 * clears the cached slots it overlaps. Relinking a program resets its cache.
 */
static UniSlot *uni_slot(GLint loc, bool create)
{
    if (loc < 0 || loc >= 4096 || g_cur_prog == 0)
        return NULL;
    if (g_cur_prog >= g_uni.size()) {
        if (!create)
            return NULL;
        g_uni.resize((size_t)g_cur_prog + 16);
    }
    std::vector<UniSlot> &p = g_uni[g_cur_prog];
    if ((size_t)loc >= p.size()) {
        if (!create)
            return NULL;
        p.resize((size_t)loc + 8, UniSlot());
    }
    return &p[(size_t)loc];
}

/* True when the value is what the slot already holds (caller then skips the call). */
static bool uni_same(int kind, GLint loc, GLsizei count, const void *v, size_t bytes)
{
    if (count != 1 || bytes > 64) {
        /* An array upload: forget every slot it writes over. */
        for (GLsizei i = 0; i < count; i++) {
            UniSlot *s = uni_slot(loc + i, false);
            if (s)
                s->kind = 0;
        }
        return false;
    }
    UniSlot *s = uni_slot(loc, true);
    if (!s)
        return false;
    size_t words = bytes / 4;
    if (s->kind == kind + 1 && s->words == words && memcmp(s->v, v, bytes) == 0)
        return true;
    s->kind = (uint8_t)(kind + 1);
    s->words = (uint8_t)words;
    memcpy(s->v, v, bytes);
    return false;
}

static void gbf_glUseProgram(GLuint program)
{
    if (program == g_cur_prog && program != 0)
        return;
    g_cur_prog = program;
    gb_glUseProgram(program);
}

static void gbf_glActiveTexture(GLenum unit)
{
    if (unit == g_active_unit)
        return;
    g_active_unit = unit;
    gb_glActiveTexture(unit);
}

static void gbf_glBindTexture(GLenum target, GLuint tex)
{
    unsigned unit = g_active_unit - 0x84C0;
    int slot = target == 0x0DE1 ? 0 : (target == 0x8513 ? 1 : -1);
    if (slot >= 0 && unit < 32) {
        if (g_bound_tex[unit][slot] == tex)
            return;
        g_bound_tex[unit][slot] = tex;
    }
    gb_glBindTexture(target, tex);
}

static void gbf_glBindFramebuffer(GLenum target, GLuint framebuffer)
{
    if (target == 0x8D40) {
        if (g_fbo == framebuffer)
            return;
        g_fbo = framebuffer;
    }
    gb_glBindFramebuffer(target, framebuffer);
}

static void gbf_glBindRenderbuffer(GLenum target, GLuint renderbuffer)
{
    if (target == 0x8D41) {
        if (g_rbo == renderbuffer)
            return;
        g_rbo = renderbuffer;
    }
    gb_glBindRenderbuffer(target, renderbuffer);
}

static void gbf_glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    if (g_viewport_set && g_viewport[0] == x && g_viewport[1] == y && g_viewport[2] == w && g_viewport[3] == h)
        return;
    g_viewport_set = true;
    g_viewport[0] = x; g_viewport[1] = y; g_viewport[2] = w; g_viewport[3] = h;
    gb_glViewport(x, y, w, h);
}

static void gbf_glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
    if (g_scissor_set && g_scissor[0] == x && g_scissor[1] == y && g_scissor[2] == w && g_scissor[3] == h)
        return;
    g_scissor_set = true;
    g_scissor[0] = x; g_scissor[1] = y; g_scissor[2] = w; g_scissor[3] = h;
    gb_glScissor(x, y, w, h);
}

static void gbf_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    GLint ir = r != 0, ig = g != 0, ib = b != 0, ia = a != 0;
    if (g_color_mask[0] == ir && g_color_mask[1] == ig && g_color_mask[2] == ib && g_color_mask[3] == ia)
        return;
    g_color_mask[0] = ir; g_color_mask[1] = ig; g_color_mask[2] = ib; g_color_mask[3] = ia;
    gb_glColorMask(r, g, b, a);
}

static void gbf_glDepthMask(GLboolean flag)
{
    GLint f = flag != 0;
    if (g_depth_mask == f)
        return;
    g_depth_mask = f;
    gb_glDepthMask(flag);
}

static int cap_slot(GLenum cap)
{
    switch (cap) {
    case 0x0BE2: return 0; /* GL_BLEND */
    case 0x0B44: return 1; /* GL_CULL_FACE */
    case 0x0B71: return 2; /* GL_DEPTH_TEST */
    case 0x0BD0: return 3; /* GL_DITHER */
    case 0x8037: return 4; /* GL_POLYGON_OFFSET_FILL */
    case 0x809E: return 5; /* GL_SAMPLE_ALPHA_TO_COVERAGE */
    case 0x80A0: return 6; /* GL_SAMPLE_COVERAGE */
    case 0x0C11: return 7; /* GL_SCISSOR_TEST */
    case 0x0B90: return 8; /* GL_STENCIL_TEST */
    default: return -1;
    }
}
static uint8_t g_cap_state[9]; /* 0 = unknown, 1 = disabled, 2 = enabled */

static void gbf_glEnable(GLenum cap)
{
    int s = cap_slot(cap);
    if (s >= 0) {
        if (g_cap_state[s] == 2)
            return;
        g_cap_state[s] = 2;
    }
    gb_glEnable(cap);
}

static void gbf_glDisable(GLenum cap)
{
    int s = cap_slot(cap);
    if (s >= 0) {
        if (g_cap_state[s] == 1)
            return;
        g_cap_state[s] = 1;
    }
    gb_glDisable(cap);
}

static void gbf_glUniform1i(GLint loc, GLint v)
{
    if (loc < 0 || uni_same(100, loc, 1, &v, 4))
        return;
    g_port_frame.uniforms++;
    gb_glUniform1i(loc, v);
}

static void gbf_glUniform1f(GLint loc, GLfloat v)
{
    if (loc < 0 || uni_same(101, loc, 1, &v, 4))
        return;
    g_port_frame.uniforms++;
    gb_glUniform1f(loc, v);
}

static void uniform_v(int kind, int comps_per, GLint loc, GLsizei count, const void *v, GLboolean transpose)
{
    if (count <= 0 || !v || loc < 0)
        return;
    size_t n = (size_t)count * (size_t)comps_per * 4;
    /* glUniform1i and glUniform1iv set the same slot; key both by kind+comps. */
    int key = kind == 0 ? 101 : (kind == 1 ? 100 : kind + (transpose ? 50 : 0));
    if (uni_same(key, loc, count, v, n))
        return;
    g_port_frame.uniforms++;
    emit(GB_OP_UNIFORM_V, { kind, loc, count, (int32_t)transpose }, v, n);
}

static void gb_glUniform1fv(GLint l, GLsizei c, const GLfloat *v) { uniform_v(0, 1, l, c, v, 0); }
static void gb_glUniform1iv(GLint l, GLsizei c, const GLint *v) { uniform_v(1, 1, l, c, v, 0); }
static void gb_glUniform2fv(GLint l, GLsizei c, const GLfloat *v) { uniform_v(2, 2, l, c, v, 0); }
static void gb_glUniform2iv(GLint l, GLsizei c, const GLint *v) { uniform_v(3, 2, l, c, v, 0); }
static void gb_glUniform3fv(GLint l, GLsizei c, const GLfloat *v) { uniform_v(4, 3, l, c, v, 0); }
static void gb_glUniform3iv(GLint l, GLsizei c, const GLint *v) { uniform_v(5, 3, l, c, v, 0); }
static void gb_glUniform4fv(GLint l, GLsizei c, const GLfloat *v) { uniform_v(6, 4, l, c, v, 0); }
static void gb_glUniform4iv(GLint l, GLsizei c, const GLint *v) { uniform_v(7, 4, l, c, v, 0); }
static void gb_glUniformMatrix4fv(GLint l, GLsizei c, GLboolean t, const GLfloat *v) { uniform_v(8, 16, l, c, v, t); }
static void gb_glUniformMatrix2fv(GLint l, GLsizei c, GLboolean t, const GLfloat *v) { uniform_v(9, 4, l, c, v, t); }
static void gb_glUniformMatrix3fv(GLint l, GLsizei c, GLboolean t, const GLfloat *v) { uniform_v(10, 9, l, c, v, t); }
static void gb_glUniform2f(GLint l, GLfloat a, GLfloat b) { GLfloat v[2] = { a, b }; uniform_v(2, 2, l, 1, v, 0); }
static void gb_glUniform3f(GLint l, GLfloat a, GLfloat b, GLfloat c) { GLfloat v[3] = { a, b, c }; uniform_v(4, 3, l, 1, v, 0); }
static void gb_glUniform4f(GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d) { GLfloat v[4] = { a, b, c, d }; uniform_v(6, 4, l, 1, v, 0); }
static void gb_glUniform2i(GLint l, GLint a, GLint b) { GLint v[2] = { a, b }; uniform_v(3, 2, l, 1, v, 0); }
static void gb_glUniform3i(GLint l, GLint a, GLint b, GLint c) { GLint v[3] = { a, b, c }; uniform_v(5, 3, l, 1, v, 0); }
static void gb_glUniform4i(GLint l, GLint a, GLint b, GLint c, GLint d) { GLint v[4] = { a, b, c, d }; uniform_v(7, 4, l, 1, v, 0); }

/* ------------------------------------------------------------ draws */

static void gb_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
                                     const void *pointer)
{
    if (index >= (GLuint)kMaxAttribs)
        return;
    Attrib &a = g_attr[index];
    bool is_client = (g_array_buf == 0);
    if (!is_client && !a.client && a.vbo == g_array_buf && a.size == size && a.type == type &&
        a.normalized == normalized && a.stride == stride && a.ptr == pointer) {
        return;
    }
    a.size = size;
    a.type = type;
    a.normalized = normalized;
    a.stride = stride;
    a.ptr = pointer;
    a.client = is_client;
    a.vbo = is_client ? 0 : g_array_buf;
    if (!a.client)
        emit(GB_OP_ATTRIB_POINTER_VBO, { (int32_t)index, size, (int32_t)type, (int32_t)normalized, stride,
                                         (int32_t)(uintptr_t)pointer });
}

/*
 * One upload: a run of client memory and every enabled attribute that reads from it.
 *
 * The engine keeps its CPU-skinned and dynamic batch vertices interleaved (position, normal,
 * tangent, UV, color in one array with a common stride), and a block per attribute sent that
 * whole array once for each of them - megabytes a frame through the pipe. Attributes that share
 * a stride and start within one stride of each other are therefore grouped into one block.
 */
struct Block {
    const uint8_t *base;    /* lowest attribute pointer, vertex 0 */
    size_t stride, span;    /* span: bytes of one vertex that the attributes cover */
    int n;
    int idx[kMaxAttribs];
    const uint8_t *data;
    size_t len;
};

/* Client arrays that are switched on, covering vertices [v0, v0 + vcount). */
static void collect_client(GLint v0, GLsizei vcount, std::vector<Block> &out)
{
    for (int i = 0; i < kMaxAttribs; i++) {
        const Attrib &a = g_attr[i];
        if (!a.enabled || !a.client || !a.ptr || vcount <= 0)
            continue;
        size_t elem = (size_t)a.size * (size_t)type_size(a.type);
        size_t stride = a.stride ? (size_t)a.stride : elem;
        const uint8_t *p = (const uint8_t *)a.ptr;
        Block *b = NULL;
        for (size_t k = 0; k < out.size() && !b; k++) {
            Block &c = out[k];
            if (c.stride != stride)
                continue;
            const uint8_t *lo = p < c.base ? p : c.base;
            const uint8_t *hi = p + elem > c.base + c.span ? p + elem : c.base + c.span;
            if ((size_t)(hi - lo) <= stride) {
                c.base = lo;
                c.span = (size_t)(hi - lo);
                b = &c;
            }
        }
        if (!b) {
            out.emplace_back();
            b = &out.back();
            b->base = p;
            b->stride = stride;
            b->span = elem;
            b->n = 0;
        }
        b->idx[b->n++] = i;
    }
    for (size_t k = 0; k < out.size(); k++) {
        Block &b = out[k];
        b.data = b.base + (size_t)v0 * b.stride;
        b.len = (size_t)(vcount - 1) * b.stride + b.span;
    }
}

static inline size_t pad4(size_t n) { return (n + 3) & ~(size_t)3; }

/* Per block: {attributes, stride, bytes}, then {index, size, type, normalized, offset} per attribute, then the data. */
static void write_blocks(const std::vector<Block> &blocks)
{
    for (size_t i = 0; i < blocks.size(); i++) {
        const Block &b = blocks[i];
        int32_t h[3 + 5 * kMaxAttribs] = { b.n, (int32_t)b.stride, (int32_t)b.len };
        for (int k = 0; k < b.n; k++) {
            const Attrib &a = g_attr[b.idx[k]];
            int32_t *w = h + 3 + 5 * k;
            w[0] = b.idx[k];
            w[1] = a.size;
            w[2] = (int32_t)a.type;
            w[3] = (int32_t)a.normalized;
            w[4] = (int32_t)((const uint8_t *)a.ptr - b.base);
        }
        out_raw(h, (size_t)(3 + 5 * b.n) * 4);
        out_raw(b.data, b.len);
        msg_end(b.len);
        g_port_frame.vtx_bytes += b.len;
    }
}

static size_t blocks_bytes(const std::vector<Block> &blocks)
{
    size_t n = 0;
    for (size_t i = 0; i < blocks.size(); i++)
        n += 12 + 20 * (size_t)blocks[i].n + pad4(blocks[i].len);
    return n;
}

static void gb_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    if (count <= 0)
        return;
    g_port_frame.draws++;
    Lock l;
    std::vector<Block> blocks;
    collect_client(first, count, blocks);
    size_t payload = 16 + blocks_bytes(blocks);
    msg_begin(GB_OP_DRAW_ARRAYS, payload);
    int32_t h[4] = { (int32_t)mode, first, count, (int32_t)blocks.size() };
    out_raw(h, sizeof(h));
    write_blocks(blocks);
}

/* The vertices an indexed draw reaches: [lo, hi]. */
static void index_range(const void *indices, GLsizei count, GLenum type, uint32_t &lo, uint32_t &hi)
{
    uint32_t m = 0, n = 0xFFFFFFFFu;
    if (type == 0x1401) {
        const uint8_t *p = (const uint8_t *)indices;
        for (GLsizei i = 0; i < count; i++) { if (p[i] > m) m = p[i]; if (p[i] < n) n = p[i]; }
    } else if (type == 0x1403) {
        const uint16_t *p = (const uint16_t *)indices;
        for (GLsizei i = 0; i < count; i++) { if (p[i] > m) m = p[i]; if (p[i] < n) n = p[i]; }
    } else {
        const uint32_t *p = (const uint32_t *)indices;
        for (GLsizei i = 0; i < count; i++) { if (p[i] > m) m = p[i]; if (p[i] < n) n = p[i]; }
    }
    lo = n;
    hi = m;
}

/*
 * The same answer for indices that live in a buffer, remembered: a mesh is drawn with the same
 * slice of the same index buffer every frame, and walking it again each time is emulated work.
 * Any write to any index buffer forgets everything.
 */
struct RangeMemo {
    uint32_t buf, off, count, type, gen, lo, hi;
};
static RangeMemo g_range_memo[512];

static void shadow_range(GLuint buf, const std::vector<uint8_t> &s, size_t off, GLsizei count, GLenum type,
                         uint32_t &lo, uint32_t &hi)
{
    RangeMemo &m = g_range_memo[(buf * 31u + (uint32_t)off * 7u + (uint32_t)count) & 511u];
    if (m.gen == g_elem_gen && m.buf == buf && m.off == (uint32_t)off && m.count == (uint32_t)count &&
        m.type == type) {
        lo = m.lo;
        hi = m.hi;
        return;
    }
    index_range(s.data() + off, count, type, lo, hi);
    m = { buf, (uint32_t)off, (uint32_t)count, (uint32_t)type, g_elem_gen, lo, hi };
}

static void gb_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    if (count <= 0)
        return;
    g_port_frame.draws++;
    Lock l;
    std::vector<Block> blocks;
    const bool client_indices = g_elem_buf == 0;
    size_t idx_bytes = 0;
    uint32_t lo = 0, hi = 0;

    bool any_client = false;
    for (int i = 0; i < kMaxAttribs; i++)
        any_client |= g_attr[i].enabled && g_attr[i].client && g_attr[i].ptr;

    if (client_indices) {
        if (!indices)
            return;
        idx_bytes = (size_t)count * (size_t)type_size(type);
        if (any_client) {
            index_range(indices, count, type, lo, hi);
            if (hi >= lo)
                collect_client((GLint)lo, (GLsizei)(hi - lo) + 1, blocks);
        }
    } else if (any_client) {
        /* Indices in a buffer, vertices in client memory: the shadow copy
         * says which vertices the draw reaches. */
        std::vector<uint8_t> *s = elem_shadow(g_elem_buf);
        size_t off = (size_t)(uintptr_t)indices;
        size_t need = off + (size_t)count * (size_t)type_size(type);
        if (s && need <= s->size()) {
            shadow_range(g_elem_buf, *s, off, count, type, lo, hi);
            if (hi >= lo)
                collect_client((GLint)lo, (GLsizei)(hi - lo) + 1, blocks);
        } else {
            static bool warned = false;
            if (!warned) {
                warned = true;
                trace("gl bridge: indexed draw past its index buffer's known contents (%zu > %zu)",
                      need, s ? s->size() : (size_t)0);
            }
        }
    }

    /* Only vertices [lo, hi] travel; the host leaves room for the ones before them, so the indices
     * still mean what they meant. */
    size_t payload = 28 + blocks_bytes(blocks) + (client_indices ? pad4(idx_bytes) : 0);
    msg_begin(GB_OP_DRAW_ELEMENTS, payload);
    int32_t h[7] = { (int32_t)mode, count, (int32_t)type, client_indices ? 1 : 0,
                     client_indices ? (int32_t)idx_bytes : (int32_t)(uintptr_t)indices, (int32_t)blocks.size(),
                     blocks.empty() ? 0 : (int32_t)lo };
    out_raw(h, sizeof(h));
    write_blocks(blocks);
    if (client_indices) {
        out_raw(indices, idx_bytes);
        msg_end(idx_bytes);
        g_port_frame.vtx_bytes += idx_bytes;
    }
}

static void gb_glInvalidateFramebuffer(GLenum target, GLsizei num, const GLenum *attachments)
{
    if (num <= 0 || !attachments)
        return;
    emit(GB_OP_INVALIDATE_FB, { (int32_t)target, num, 0, 0, 0, 0, 0 }, attachments, (size_t)num * 4);
}

static void gb_glInvalidateSubFramebuffer(GLenum target, GLsizei num, const GLenum *attachments, GLint x, GLint y,
                                          GLsizei w, GLsizei h)
{
    if (num <= 0 || !attachments)
        return;
    emit(GB_OP_INVALIDATE_FB, { (int32_t)target, num, 1, x, y, w, h }, attachments, (size_t)num * 4);
}

static void gb_glFlush(void)
{
    Lock l;
    out_flush();
}

static GLenum gb_glGetError(void) { return 0; }

/* Additional GLES 2.0 entry points the Mass Effect engine imports. */
static void gb_glGetIntegerv(GLenum pname, GLint *data);
static void gb_glFinish(void) { gb_glFlush(); }
static void gb_glBindAttribLocation(GLuint program, GLuint index, const GLchar *name)
{
    size_t n = name ? strlen(name) : 0;
    emit(GB_OP_BIND_ATTRIB_LOC, { (int32_t)program, (int32_t)index, (int32_t)n }, name, n);
}
static void gb_glTexParameterfv(GLenum target, GLenum pname, const GLfloat *p) { if (p) gb_glTexParameterf(target, pname, p[0]); }
static void gb_glTexParameteriv(GLenum target, GLenum pname, const GLint *p) { if (p) gb_glTexParameteri(target, pname, p[0]); }
static void gb_glVertexAttrib1fv(GLuint i, const GLfloat *v) { gb_glVertexAttrib1f(i, v[0]); }
static void gb_glVertexAttrib2fv(GLuint i, const GLfloat *v) { gb_glVertexAttrib2f(i, v[0], v[1]); }
static void gb_glVertexAttrib3fv(GLuint i, const GLfloat *v) { gb_glVertexAttrib3f(i, v[0], v[1], v[2]); }
static void gb_glVertexAttrib4fv(GLuint i, const GLfloat *v) { gb_glVertexAttrib4f(i, v[0], v[1], v[2], v[3]); }
static void gb_glReleaseShaderCompiler(void) {}
static GLboolean gb_glIsEnabled(GLenum cap)
{
    int s = cap_slot(cap);
    return (s >= 0 && g_cap_state[s] == 2) ? GL_TRUE : GL_FALSE;
}
static GLboolean gb_glIsTexture(GLuint id) { return id != 0; }
static GLboolean gb_glIsBuffer(GLuint id) { return id != 0; }
static GLboolean gb_glIsFramebuffer(GLuint id) { return id != 0; }
static GLboolean gb_glIsRenderbuffer(GLuint id) { return id != 0; }
static GLboolean gb_glIsProgram(GLuint id) { return id != 0; }
static GLboolean gb_glIsShader(GLuint id) { return id != 0; }
static void gb_glGetShaderPrecisionFormat(GLenum, GLenum precisiontype, GLint *range, GLint *precision)
{
    bool is_int = precisiontype >= 0x8DF3;  /* GL_LOW_INT..GL_HIGH_INT */
    if (range) { range[0] = is_int ? 31 : 127; range[1] = is_int ? 30 : 127; }
    if (precision) *precision = is_int ? 0 : 23;
}
static void gb_glGetBooleanv(GLenum pname, GLboolean *data)
{
    GLint v[16] = { 0 };
    gb_glGetIntegerv(pname, v);
    if (data) for (int i = 0; i < 4; i++) data[i] = v[i] != 0;
}
static void gb_glGetTexParameteriv(GLenum, GLenum, GLint *p) { if (p) *p = 0; }
static void gb_glGetTexParameterfv(GLenum, GLenum, GLfloat *p) { if (p) *p = 0; }
static void gb_glGetBufferParameteriv(GLenum, GLenum, GLint *p) { if (p) *p = 0; }
static void gb_glGetFramebufferAttachmentParameteriv(GLenum, GLenum, GLenum, GLint *p) { if (p) *p = 0; }
static void gb_glGetRenderbufferParameteriv(GLenum, GLenum, GLint *p) { if (p) *p = 0; }
static void gb_glGetUniformfv(GLuint, GLint, GLfloat *p) { if (p) *p = 0; }
static void gb_glGetUniformiv(GLuint, GLint, GLint *p) { if (p) *p = 0; }
static void gb_glGetVertexAttribfv(GLuint, GLenum, GLfloat *p) { if (p) *p = 0; }
static void gb_glGetVertexAttribiv(GLuint, GLenum, GLint *p) { if (p) *p = 0; }
static void gb_glGetAttachedShaders(GLuint, GLsizei, GLsizei *count, GLuint *) { if (count) *count = 0; }

/* Optional extension entry points the engine imports but the bridge does not serve (not advertised). */
static void gb_glEGLImageTargetTexture2DOES(GLenum, void *) {}
static void gb_glEGLImageTargetRenderbufferStorageOES(GLenum, void *) {}
static void gb_glGetBufferPointervOES(GLenum, GLenum, void **p) { if (p) *p = NULL; }
static void gb_glGetVertexAttribPointerv(GLuint, GLenum, void **p) { if (p) *p = NULL; }
static void gb_glGetProgramBinaryOES(GLuint, GLsizei, GLsizei *len, GLenum *, void *) { if (len) *len = 0; }
static void gb_glProgramBinaryOES(GLuint, GLenum, const void *, GLint) {}
static void gb_glShaderBinary(GLsizei, const GLuint *, GLenum, const void *, GLsizei) {}
static void gb_glFramebufferTexture3DOES(GLenum, GLenum, GLenum, GLuint, GLint, GLint) {}
static void gb_glCopyTexSubImage3DOES(GLenum, GLint, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei) {}

/* ------------------------------------------------------------ queries */

/* Only extensions whose entry points the bridge can serve are advertised. */
static std::string filter_extensions(const std::string &all)
{
    static const char *const kKeep[] = {
        "GL_OES_compressed_ETC1_RGB8_texture", "GL_EXT_texture_compression_s3tc",
        "GL_EXT_texture_compression_dxt1", "GL_AMD_compressed_ATC_texture",
        "GL_ATI_texture_compression_atitc", "GL_OES_depth_texture", "GL_OES_depth24",
        "GL_OES_packed_depth_stencil", "GL_OES_rgb8_rgba8", "GL_OES_standard_derivatives",
        "GL_OES_element_index_uint", "GL_EXT_texture_filter_anisotropic", "GL_OES_texture_half_float",
        "GL_OES_texture_half_float_linear", "GL_OES_texture_float", "GL_OES_texture_float_linear",
        "GL_EXT_texture_format_BGRA8888", "GL_EXT_blend_minmax", "GL_OES_mapbuffer", "GL_OES_texture_3D",
        "GL_EXT_discard_framebuffer", "GL_EXT_shader_framebuffer_fetch", "GL_OES_fbo_render_mipmap",
        NULL,
    };
    std::string out;
    size_t pos = 0;
    while (pos < all.size()) {
        size_t end = all.find(' ', pos);
        if (end == std::string::npos)
            end = all.size();
        std::string tok = all.substr(pos, end - pos);
        for (int i = 0; kKeep[i]; i++) {
            if (tok == kKeep[i]) {
                if (!out.empty())
                    out += ' ';
                out += tok;
                break;
            }
        }
        pos = end + 1;
    }
    static const char *const kAlways[] = {
        "GL_OES_compressed_ETC1_RGB8_texture",
        "GL_AMD_compressed_ATC_texture",
        "GL_ATI_texture_compression_atitc",
        NULL,
    };
    for (int i = 0; kAlways[i]; i++) {
        if ((" " + out + " ").find(std::string(" ") + kAlways[i] + " ") == std::string::npos) {
            if (!out.empty())
                out += ' ';
            out += kAlways[i];
        }
    }
    return out;
}

static const GLubyte *gb_glGetString(GLenum name)
{
    static std::map<GLenum, std::string> cache;
    static pthread_mutex_t str_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&str_lock);
    std::map<GLenum, std::string>::iterator it = cache.find(name);
    if (it == cache.end()) {
        std::vector<uint8_t> rep;
        query(GB_OP_Q_GET_STRING, { (int32_t)name }, rep);
        std::string s((const char *)rep.data(), rep.size());
        /* The engine picks its quality profile by renderer name and gives an
         * unknown GPU the heaviest one; <PREFIX>_GL_RENDERER names a known one. */
        const char *renderer = port_getenv("GL_RENDERER");
        if (name == 0x1F01 && renderer && *renderer) {
            trace("gl bridge: renderer %s reported as %s", s.c_str(), renderer);
            s = renderer;
        }
        if (name == 0x1F02) {
            /* The context the engine asked for is ES2; a host that answers
             * "OpenGL ES 3.2" would send it looking for ES3 entry points. */
            const char *version = port_getenv("GL_VERSION");
            trace("gl bridge: host version %s", s.c_str());
            s = (version && *version) ? version : "OpenGL ES 2.0";
        }
        if (name == 0x1F03) {
            trace("gl bridge: host extensions: %s", s.c_str());
            s = filter_extensions(s);
            trace("gl bridge: exposed extensions: %s", s.c_str());
        } else if (name == 0x1F00 || name == 0x1F01 || name == 0x1F02) {
            trace("gl bridge: host GL string 0x%04x = %s", name, s.c_str());
        }
        it = cache.insert(std::make_pair(name, s)).first;
    }
    const GLubyte *result = (const GLubyte *)it->second.c_str();
    pthread_mutex_unlock(&str_lock);
    return result;
}

static void gb_glGetIntegerv(GLenum pname, GLint *data)
{
    if (!data)
        return;
    switch (pname) {
    case 0x8CA6: /* GL_FRAMEBUFFER_BINDING */
        data[0] = (GLint)g_fbo;
        return;
    case 0x8CA7: /* GL_RENDERBUFFER_BINDING */
        data[0] = (GLint)g_rbo;
        return;
    case 0x8894: /* GL_ARRAY_BUFFER_BINDING */
        data[0] = (GLint)g_array_buf;
        return;
    case 0x8895: /* GL_ELEMENT_ARRAY_BUFFER_BINDING */
        data[0] = (GLint)g_elem_buf;
        return;
    case 0x8B8D: /* GL_CURRENT_PROGRAM */
        data[0] = (GLint)g_cur_prog;
        return;
    case 0x84E0: /* GL_ACTIVE_TEXTURE */
        data[0] = (GLint)g_active_unit;
        return;
    case 0x8069: /* GL_TEXTURE_BINDING_2D */
        data[0] = (g_active_unit >= 0x84C0 && g_active_unit < 0x84C0 + 32)
                      ? (GLint)g_bound_tex[g_active_unit - 0x84C0][0] : 0;
        return;
    case 0x8514: /* GL_TEXTURE_BINDING_CUBE_MAP */
        data[0] = (g_active_unit >= 0x84C0 && g_active_unit < 0x84C0 + 32)
                      ? (GLint)g_bound_tex[g_active_unit - 0x84C0][1] : 0;
        return;
    case 0x0CF5: /* GL_UNPACK_ALIGNMENT */
        data[0] = g_unpack_align;
        return;
    case 0x0D05: /* GL_PACK_ALIGNMENT */
        data[0] = g_pack_align;
        return;
    case 0x0C23: /* GL_COLOR_WRITEMASK */
        memcpy(data, g_color_mask, 16);
        return;
    case 0x0B72: /* GL_DEPTH_WRITEMASK */
        data[0] = g_depth_mask;
        return;
    case 0x0BA2: /* GL_VIEWPORT */
        if (g_viewport_set) {
            memcpy(data, g_viewport, 16);
            return;
        }
        break;
    case 0x0C10: /* GL_SCISSOR_BOX */
        if (g_scissor_set) {
            memcpy(data, g_scissor, 16);
            return;
        }
        break;
    default:
        break;
    }

    size_t max_bytes = (pname == 0x86A3) ? 256 : 64;
    static std::map<GLenum, std::vector<uint8_t> > int_cache;
    {
        Lock l;
        std::map<GLenum, std::vector<uint8_t> >::iterator it = int_cache.find(pname);
        if (it != int_cache.end()) {
            size_t n = it->second.size() < max_bytes ? it->second.size() : max_bytes;
            if (n)
                memcpy(data, it->second.data(), n);
            return;
        }
    }
    std::vector<uint8_t> rep;
    query(GB_OP_Q_GET_INTEGERV, { (int32_t)pname }, rep);
    size_t n = rep.size() < max_bytes ? rep.size() : max_bytes;
    if (n)
        memcpy(data, rep.data(), n);
    if (pname != 0x0BA2 && pname != 0x0C10) {
        Lock l;
        int_cache[pname] = rep;
    }
}

static void gb_glGetFloatv(GLenum pname, GLfloat *data)
{
    if (!data)
        return;
    static std::map<GLenum, std::vector<uint8_t> > float_cache;
    {
        Lock l;
        std::map<GLenum, std::vector<uint8_t> >::iterator it = float_cache.find(pname);
        if (it != float_cache.end()) {
            size_t n = it->second.size() < 64 ? it->second.size() : 64;
            if (n)
                memcpy(data, it->second.data(), n);
            return;
        }
    }
    std::vector<uint8_t> rep;
    query(GB_OP_Q_GET_FLOATV, { (int32_t)pname }, rep);
    size_t n = rep.size() < 64 ? rep.size() : 64;
    if (n)
        memcpy(data, rep.data(), n);
    {
        Lock l;
        float_cache[pname] = rep;
    }
}

static void gb_glGetShaderiv(GLuint shader, GLenum pname, GLint *params)
{
    if (!params)
        return;
    /* Avoid a synchronous round-trip per compiled shader: host ANGLE compiles all
     * shaders cleanly, and glLinkProgram's GB_OP_Q_PROGRAM_INFO synchronises the
     * linked program immediately afterwards. */
    if (pname == 0x8B81 /* GL_COMPILE_STATUS */) {
        *params = 1;
        return;
    }
    if (pname == 0x8B84 /* GL_INFO_LOG_LENGTH */) {
        *params = 0;
        return;
    }
    *params = query_int(GB_OP_Q_GET_SHADERIV, { (int32_t)shader, (int32_t)pname });
}

static void gb_glGetProgramiv(GLuint program, GLenum pname, GLint *params)
{
    const ProgInfo *pi = program_info(program);
    if (pi && params) {
        switch (pname) {
        case 0x8B82: *params = pi->link_status; return;           /* GL_LINK_STATUS */
        case 0x8B84: *params = pi->log_len; return;               /* GL_INFO_LOG_LENGTH */
        case 0x8B89: *params = (GLint)pi->attrs.size(); return;   /* GL_ACTIVE_ATTRIBUTES */
        case 0x8B8A: *params = pi->max_attr; return;              /* GL_ACTIVE_ATTRIBUTE_MAX_LENGTH */
        case 0x8B86: *params = (GLint)pi->unis.size(); return;    /* GL_ACTIVE_UNIFORMS */
        case 0x8B87: *params = pi->max_uni; return;               /* GL_ACTIVE_UNIFORM_MAX_LENGTH */
        default: break;
        }
    }
    int32_t v = query_int(GB_OP_Q_GET_PROGRAMIV, { (int32_t)program, (int32_t)pname });
    if (params)
        *params = v;
}

static void copy_text(const std::vector<uint8_t> &rep, size_t skip, GLsizei bufsize, GLsizei *length, GLchar *out)
{
    size_t have = rep.size() > skip ? rep.size() - skip : 0;
    size_t n = bufsize > 0 ? (size_t)bufsize - 1 : 0;
    if (have < n)
        n = have;
    if (out && bufsize > 0) {
        if (n)
            memcpy(out, rep.data() + skip, n);
        out[n] = 0;
    }
    if (length)
        *length = (GLsizei)n;
}

static void gb_glGetShaderInfoLog(GLuint shader, GLsizei bufsize, GLsizei *length, GLchar *log)
{
    std::vector<uint8_t> rep;
    query(GB_OP_Q_SHADER_LOG, { (int32_t)shader, bufsize }, rep);
    copy_text(rep, 0, bufsize, length, log);
}

static void gb_glGetProgramInfoLog(GLuint program, GLsizei bufsize, GLsizei *length, GLchar *log)
{
    std::vector<uint8_t> rep;
    query(GB_OP_Q_PROGRAM_LOG, { (int32_t)program, bufsize }, rep);
    copy_text(rep, 0, bufsize, length, log);
}

static void gb_glGetShaderSource(GLuint shader, GLsizei bufsize, GLsizei *length, GLchar *source)
{
    std::vector<uint8_t> rep;
    query(GB_OP_Q_SHADER_SOURCE, { (int32_t)shader, bufsize }, rep);
    copy_text(rep, 0, bufsize, length, source);
}

static void active_var(int op, GLuint program, GLuint index, GLsizei bufsize, GLsizei *length, GLint *size,
                       GLenum *type, GLchar *name)
{
    std::vector<uint8_t> rep;
    query(op, { (int32_t)program, (int32_t)index, bufsize }, rep);
    int32_t sz = 0, ty = 0;
    if (rep.size() >= 8) {
        memcpy(&sz, rep.data(), 4);
        memcpy(&ty, rep.data() + 4, 4);
    }
    if (size)
        *size = sz;
    if (type)
        *type = (GLenum)ty;
    copy_text(rep, 8, bufsize, length, name);
}

/* glGetActive* from the program info fetched at link time; false when it does not cover the call. */
static bool active_var_local(const std::vector<ProgVar> *vars, GLuint index, GLsizei bufsize, GLsizei *length,
                             GLint *size, GLenum *type, GLchar *name)
{
    if (!vars || index >= vars->size())
        return false;
    const ProgVar &v = (*vars)[index];
    if (size)
        *size = v.size;
    if (type)
        *type = v.type;
    size_t n = bufsize > 0 ? (size_t)bufsize - 1 : 0;
    if (v.name.size() < n)
        n = v.name.size();
    if (name && bufsize > 0) {
        memcpy(name, v.name.data(), n);
        name[n] = 0;
    }
    if (length)
        *length = (GLsizei)n;
    return true;
}

static void gb_glGetActiveAttrib(GLuint p, GLuint i, GLsizei b, GLsizei *len, GLint *size, GLenum *type, GLchar *name)
{
    const ProgInfo *pi = program_info(p);
    if (active_var_local(pi ? &pi->attrs : NULL, i, b, len, size, type, name))
        return;
    active_var(GB_OP_Q_ACTIVE_ATTRIB, p, i, b, len, size, type, name);
}

static void gb_glGetActiveUniform(GLuint p, GLuint i, GLsizei b, GLsizei *len, GLint *size, GLenum *type, GLchar *name)
{
    const ProgInfo *pi = program_info(p);
    if (active_var_local(pi ? &pi->unis : NULL, i, b, len, size, type, name))
        return;
    active_var(GB_OP_Q_ACTIVE_UNIFORM, p, i, b, len, size, type, name);
}

static GLint location(int op, std::map<std::pair<uint32_t, std::string>, int> &cache, GLuint program,
                      const GLchar *name)
{
    if (!name)
        return -1;
    std::pair<uint32_t, std::string> key(program, name);
    {
        Lock l;
        std::map<std::pair<uint32_t, std::string>, int>::iterator it = cache.find(key);
        if (it != cache.end())
            return it->second;
    }
    int32_t loc = query_int(op, { (int32_t)program }, name, strlen(name) + 1);
    Lock l;
    cache[key] = loc;
    return loc;
}

static GLint gb_glGetAttribLocation(GLuint program, const GLchar *name)
{
    return location(GB_OP_Q_ATTRIB_LOC, g_attrib_loc, program, name);
}

static GLint gb_glGetUniformLocation(GLuint program, const GLchar *name)
{
    return location(GB_OP_Q_UNIFORM_LOC, g_uniform_loc, program, name);
}

static GLenum gb_glCheckFramebufferStatus(GLenum target)
{
    (void)target;
    return 0x8CD5; /* GL_FRAMEBUFFER_COMPLETE */
}

/*
 * Reading pixels back is a full stall: every queued call is replayed and the
 * GPU drained before the answer comes. The engine does it every frame for lens
 * flare visibility, so a repeated read of the same rectangle is answered from
 * the last real one and refreshed only every <PREFIX>_READPIXELS_EVERY frames
 * (default 3; 1 = always read). A one-off read - a screenshot - is always real.
 */
struct ReadCache {
    int32_t key[7];
    long frame;
    std::vector<uint8_t> data;
};
static std::vector<ReadCache> g_read_cache;

static void gb_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                            void *pixels)
{
    static long every = -1;
    if (every < 0)
        every = port_getenv_long("READPIXELS_EVERY", 3);
    size_t want0 = image_bytes(width, height, 1, format, type, g_pack_align, 0);
    int32_t key[7] = { x, y, width, height, (int32_t)format, (int32_t)type, g_pack_align };
    ReadCache *hit = NULL;
    if (every > 1 && want0 <= 4096) {
        for (size_t i = 0; i < g_read_cache.size(); i++)
            if (memcmp(g_read_cache[i].key, key, sizeof(key)) == 0)
                hit = &g_read_cache[i];
        if (hit && g_frames - hit->frame < every && hit->frame != g_frames) {
            if (pixels && hit->data.size() >= want0)
                memcpy(pixels, hit->data.data(), want0);
            return;
        }
    }
    std::vector<uint8_t> rep;
    query(GB_OP_Q_READ_PIXELS, { x, y, width, height, (int32_t)format, (int32_t)type, g_pack_align }, rep);
    size_t want = want0;
    size_t n = rep.size() < want ? rep.size() : want;
    if (pixels && n)
        memcpy(pixels, rep.data(), n);
    if (every > 1 && want0 <= 4096) {
        if (!hit && g_read_cache.size() < 64) {
            g_read_cache.push_back(ReadCache());
            hit = &g_read_cache.back();
            memcpy(hit->key, key, sizeof(key));
            static int logged;
            if (logged++ < 8)
                trace("gl bridge: glReadPixels %dx%d at %d,%d fmt 0x%x type 0x%x - cached, refreshed every %ld frames",
                      width, height, x, y, format, type, every);
        }
        if (hit) {
            hit->frame = g_frames;
            hit->data.assign(rep.begin(), rep.begin() + n);
        }
    }
}

/* ------------------------------------------------------------ resolver */

struct Entry { const char *name; void *fn; };

#define GB_ENTRY(n) { #n, (void *)gb_##n },
static const Entry kEntries[] = {
    /* State filters first: the first match wins over the generated stubs. */
    { "glUseProgram", (void *)gbf_glUseProgram },
    { "glActiveTexture", (void *)gbf_glActiveTexture },
    { "glBindTexture", (void *)gbf_glBindTexture },
    { "glBindFramebuffer", (void *)gbf_glBindFramebuffer },
    { "glBindRenderbuffer", (void *)gbf_glBindRenderbuffer },
    { "glViewport", (void *)gbf_glViewport },
    { "glScissor", (void *)gbf_glScissor },
    { "glColorMask", (void *)gbf_glColorMask },
    { "glDepthMask", (void *)gbf_glDepthMask },
    { "glEnable", (void *)gbf_glEnable },
    { "glDisable", (void *)gbf_glDisable },
    { "glUniform1i", (void *)gbf_glUniform1i },
    { "glUniform1f", (void *)gbf_glUniform1f },
    { "glCompileShader", (void *)gbf_glCompileShader },
    GB_SIMPLE_CALLS(GB_ENTRY)
#undef GB_ENTRY
#define E(n) { #n, (void *)gb_##n },
    E(glGenTextures) E(glGenBuffers) E(glGenFramebuffers) E(glGenRenderbuffers)
    E(glDeleteTextures) E(glDeleteBuffers) E(glDeleteFramebuffers) E(glDeleteRenderbuffers)
    E(glCreateShader) E(glCreateProgram) E(glShaderSource)
    E(glBindBuffer) E(glBufferData) E(glBufferSubData) E(glMapBufferOES) E(glUnmapBufferOES)
    E(glTexImage2D) E(glTexSubImage2D) E(glCompressedTexImage2D) E(glCompressedTexSubImage2D)
    E(glTexImage3DOES) E(glTexSubImage3DOES) E(glCompressedTexImage3DOES) E(glCompressedTexSubImage3DOES)
    E(glUniform1fv) E(glUniform1iv) E(glUniform2fv) E(glUniform2iv) E(glUniform3fv) E(glUniform3iv)
    E(glUniform4fv) E(glUniform4iv) E(glUniformMatrix4fv) E(glUniformMatrix2fv) E(glUniformMatrix3fv)
    E(glUniform2f) E(glUniform3f) E(glUniform4f) E(glUniform2i) E(glUniform3i) E(glUniform4i)
    E(glFinish) E(glBindAttribLocation) E(glTexParameterfv) E(glTexParameteriv)
    E(glVertexAttrib1fv) E(glVertexAttrib2fv) E(glVertexAttrib3fv) E(glVertexAttrib4fv) E(glReleaseShaderCompiler)
    E(glIsEnabled) E(glIsTexture) E(glIsBuffer) E(glIsFramebuffer) E(glIsRenderbuffer) E(glIsProgram) E(glIsShader)
    E(glGetShaderPrecisionFormat) E(glGetBooleanv) E(glGetTexParameteriv) E(glGetTexParameterfv) E(glGetBufferParameteriv)
    E(glGetFramebufferAttachmentParameteriv) E(glGetRenderbufferParameteriv) E(glGetUniformfv) E(glGetUniformiv)
    E(glGetVertexAttribfv) E(glGetVertexAttribiv) E(glGetAttachedShaders)
    E(glEGLImageTargetTexture2DOES) E(glEGLImageTargetRenderbufferStorageOES) E(glGetBufferPointervOES)
    E(glGetVertexAttribPointerv) E(glGetProgramBinaryOES) E(glProgramBinaryOES) E(glShaderBinary)
    E(glFramebufferTexture3DOES) E(glCopyTexSubImage3DOES)
    E(glVertexAttribPointer) E(glDrawArrays) E(glDrawElements)
    E(glInvalidateFramebuffer) E(glInvalidateSubFramebuffer)
    E(glFlush) E(glGetError)
    E(glGetString) E(glGetIntegerv) E(glGetFloatv) E(glGetShaderiv) E(glGetProgramiv)
    E(glGetShaderInfoLog) E(glGetProgramInfoLog) E(glGetShaderSource)
    E(glGetActiveAttrib) E(glGetActiveUniform) E(glGetAttribLocation) E(glGetUniformLocation)
    E(glCheckFramebufferStatus) E(glReadPixels)
#undef E
    { "glDiscardFramebufferEXT", (void *)gb_glInvalidateFramebuffer },
    { NULL, NULL },
};

extern "C" int gl_bridge_enabled(void)
{
    if (g_enabled < 0) {
        const char *v = port_getenv("GL_BRIDGE");
        g_enabled = (v && *v && *v != '0') ? 1 : 0;
    }
    return g_enabled;
}

/*
 * <PREFIX>_GL_LISTEN=<port>: instead of the two pipes, one TCP socket on
 * 127.0.0.1 carries both directions. The host connects to it - ew3gl.exe on
 * Windows reaches a guest in WSL through WSL's localhost forwarding.
 */
static int listen_for_host(long port)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(ls, 1) != 0) {
        fatal("GL bridge: cannot listen on 127.0.0.1:%ld: %s", port, strerror(errno));
        return -1;
    }
    trace("gl bridge: waiting for the GL host on 127.0.0.1:%ld", port);
    int s;
    while ((s = accept(ls, NULL, NULL)) < 0 && errno == EINTR) {
    }
    close(ls);
    if (s < 0) {
        fatal("GL bridge: accept: %s", strerror(errno));
        return -1;
    }
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int big = 4 << 20;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, &big, sizeof(big));
    return s;
}

extern "C" int gl_bridge_init(void)
{
    long port = port_getenv_long("GL_LISTEN", 0);
    if (port > 0) {
        int s = listen_for_host(port);
        if (s < 0)
            return 0;
        g_cmd_fd = g_rep_fd = s;
        g_max_inflight = (int)port_getenv_long("GL_INFLIGHT", 2);
        if (g_max_inflight < 1)
            g_max_inflight = 1;
        trace("gl bridge: connected over TCP (port %ld), %d frame(s) in flight", port, g_max_inflight);
        return 1;
    }
    const char *cmd = port_getenv("GL_CMD");
    const char *rep = port_getenv("GL_REPLY");
    if (!cmd || !rep) {
        fatal("GL bridge: set GL_LISTEN=<port>, or the GL_CMD and GL_REPLY pipe paths");
        return 0;
    }
    if (strcmp(cmd, rep) == 0) {
        g_cmd_fd = g_rep_fd = open(cmd, O_RDWR);
        if (g_cmd_fd < 0) {
            fatal("GL bridge: cannot open %s: %s", cmd, strerror(errno));
            return 0;
        }
        trace("gl bridge: waiting for host on %s", cmd);
        struct pollfd pfd = { g_cmd_fd, POLLOUT, 0 };
        while (poll(&pfd, 1, 100) >= 0 && (pfd.revents & POLLHUP))
            usleep(20000);
    } else {
        g_cmd_fd = open(cmd, O_WRONLY);
        g_rep_fd = open(rep, O_RDONLY);
    }
    if (g_cmd_fd < 0 || g_rep_fd < 0) {
        fatal("GL bridge: cannot open the pipes (%s, %s): %s", cmd, rep, strerror(errno));
        return 0;
    }
    g_max_inflight = (int)port_getenv_long("GL_INFLIGHT", 2);
    if (g_max_inflight < 1)
        g_max_inflight = 1;
    /* A bigger command pipe lets the guest write ahead while the host is busy with the previous frame. */
    long kb = port_getenv_long("GL_PIPE_KB", 1024);
    int got = fcntl(g_cmd_fd, 1031 /* F_SETPIPE_SZ */, (int)(kb * 1024));
    trace("gl bridge: connected (%s, %s), %d frame(s) in flight, command pipe %d KB", cmd, rep,
          g_max_inflight, got > 0 ? got / 1024 : -1);
    return 1;
}

/*
 * Per-frame timings with <PREFIX>_GL_PROFILE=1: the engine's step(), the part
 * of it spent encoding GL calls, and the wait for the host to present. Logged
 * as averages every 120 frames, with a single-line attribution for any frame
 * that exceeds <PREFIX>_SLOW_FRAME_MS (default 25 ms).
 */
static uint64_t g_step_ns, g_swap_ns, g_frame_t0;
static uint64_t g_cpu_ns, g_cpu_t0, g_period_ns, g_prev_begin;
static uint64_t g_bridge_frame_t0, g_write_frame_t0, g_sync_frame_t0;
static long     g_prof_frames;
static double   g_step_win[120];
static double   g_usual_step_ms = 16.0;

/* Unity 4.2.1f4 PlayerLoop (+0x2c9420) nanosecond counters at libunity.so + 0xa21278:
 * [0]=FixedUpdate, [1]=Physics, [2]=Scripts (Update+LateUpdate), [3]=Coroutines,
 * [4]=Skinning/Batching, [5]=Animations, [6]=RenderCleanup. */
static uint64_t g_u_t0[7];
static uint64_t g_u_sum[7];
static uint64_t g_u_render_sum;
static uint64_t g_win_draws, g_win_vtx_bytes, g_win_unif;
static uint64_t g_win_opens, g_win_io_bytes, g_win_tex_bytes, g_win_buf_bytes;

static void read_unity_timers(uint64_t out[7])
{
    so_module *m = ew3_module();
    if (!m || !m->text_base) {
        memset(out, 0, 7 * sizeof(uint64_t));
        return;
    }
    const volatile uint64_t *p = (const volatile uint64_t *)(m->text_base + 0xa21278);
    for (int i = 0; i < 7; i++)
        out[i] = p[i];
}

/* CPU time of the calling thread. Under qemu-user this is the host thread that runs the guest thread, so
 * it counts emulation work only: when step() takes longer than this, the thread was waiting, not computing. */
static uint64_t thread_cpu_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void slow_frame(uint64_t step_ns, const uint64_t u_dt[7], uint64_t u_render_ns, uint64_t bridge_frame_ns)
{
    static long thresh_ms = -2;
    if (thresh_ms == -2)
        thresh_ms = port_getenv_long("SLOW_FRAME_MS", 25);
    double ms = step_ns / 1e6;
    if (thresh_ms <= 0 || ms < thresh_ms || ms < g_usual_step_ms * 1.5)
        return;
    PortFrameStats &f = g_port_frame;
    char line[768];
    int n = snprintf(line, sizeof(line), "slow frame %ld: step %.0f ms (usual %.0f)",
                     g_frames, ms, g_usual_step_ms);
    n += snprintf(line + n, sizeof(line) - n,
                  " | unity scripts=%.1fms coro=%.1fms anim=%.1fms phys=%.1fms skin=%.1fms render=%.1fms(bridge=%.1fms)",
                  u_dt[2] / 1e6, u_dt[3] / 1e6, u_dt[5] / 1e6, (u_dt[0] + u_dt[1]) / 1e6,
                  u_dt[4] / 1e6, u_render_ns / 1e6, bridge_frame_ns / 1e6);
    if (f.draws)
        n += snprintf(line + n, sizeof(line) - n, " | draws %u (vtx %llu KB, unif %u)",
                      f.draws, (unsigned long long)(f.vtx_bytes / 1024), f.uniforms);
    if (f.opens || f.io_reads)
        n += snprintf(line + n, sizeof(line) - n, " | io %u opens (%s), %u reads (%llu KB)",
                      f.opens, f.last_open[0] ? f.last_open : "-", f.io_reads,
                      (unsigned long long)(f.io_bytes / 1024));
    if (f.textures)
        n += snprintf(line + n, sizeof(line) - n, " | tex %u (%llu KB)",
                      f.textures, (unsigned long long)(f.tex_bytes / 1024));
    if (f.buf_bytes >= 64 * 1024)
        n += snprintf(line + n, sizeof(line) - n, " | buf %llu KB",
                      (unsigned long long)(f.buf_bytes / 1024));
    if (f.shaders || f.links)
        n += snprintf(line + n, sizeof(line) - n, " | shaders %u, links %u", f.shaders, f.links);
    if (f.sync_queries)
        n += snprintf(line + n, sizeof(line) - n, " | sync %u (%.1f ms)",
                      f.sync_queries, f.sync_ns / 1e6);
    if (f.touches)
        n += snprintf(line + n, sizeof(line) - n, " | touches %u", f.touches);
    trace("%s", line);
}

extern "C" void gl_bridge_profile_step(int begin)
{
    if (g_profile < 0)
        g_profile = (int)port_getenv_long("GL_PROFILE", 0);
    if (begin) {
        g_port_frame = PortFrameStats();
        g_port_frame.touch_age_ms = -1;
        g_last_swap_ns = 0;
    }
    if (g_profile <= 0)
        return;
    if (begin) {
        g_frame_t0 = now_ns();
        if (g_prev_begin)
            g_period_ns += g_frame_t0 - g_prev_begin;
        g_prev_begin = g_frame_t0;
        g_cpu_t0 = thread_cpu_ns();
        g_bridge_frame_t0 = g_bridge_ns;
        g_write_frame_t0 = g_write_ns;
        g_sync_frame_t0 = g_sync_ns;
        read_unity_timers(g_u_t0);
        return;
    }
    uint64_t raw_wall_ns = now_ns() - g_frame_t0;
    uint64_t step_ns = raw_wall_ns > g_last_swap_ns ? (raw_wall_ns - g_last_swap_ns) : raw_wall_ns;
    uint64_t u_t1[7], u_dt[7];
    read_unity_timers(u_t1);
    uint64_t u_tracked = 0;
    for (int i = 0; i < 7; i++) {
        u_dt[i] = u_t1[i] >= g_u_t0[i] ? (u_t1[i] - g_u_t0[i]) : 0;
        g_u_sum[i] += u_dt[i];
        u_tracked += u_dt[i];
    }
    uint64_t u_render_ns = step_ns > u_tracked ? (step_ns - u_tracked) : 0;
    g_u_render_sum += u_render_ns;
    uint64_t bridge_frame_ns = g_bridge_ns >= g_bridge_frame_t0 ? (g_bridge_ns - g_bridge_frame_t0) : 0;

    g_win_draws += g_port_frame.draws;
    g_win_vtx_bytes += g_port_frame.vtx_bytes;
    g_win_unif += g_port_frame.uniforms;
    g_win_opens += g_port_frame.opens;
    g_win_io_bytes += g_port_frame.io_bytes;
    g_win_tex_bytes += g_port_frame.tex_bytes;
    g_win_buf_bytes += g_port_frame.buf_bytes;

    slow_frame(step_ns, u_dt, u_render_ns, bridge_frame_ns);
    g_step_ns += step_ns;
    g_step_win[g_prof_frames % 120] = step_ns / 1e6;
    g_cpu_ns += thread_cpu_ns() - g_cpu_t0;
    if (++g_prof_frames % 120 == 0) {
        double n = 120.0;
        std::sort(g_step_win, g_step_win + 120);
        g_usual_step_ms = g_step_win[60];
        double bridge_enc_ns = (double)g_bridge_ns - (double)g_write_ns - (double)g_sync_ns;
        if (bridge_enc_ns < 0)
            bridge_enc_ns = 0;
        trace("glprof: period %.1f ms, step %.1f ms (p95 %.1f, max %.1f; cpu %.1f ms, bridge %.1f ms, %.0f calls), swap wait %.1f ms per frame",
              g_period_ns / n / 1e6, g_step_ns / n / 1e6, g_step_win[114], g_step_win[119],
              g_cpu_ns / n / 1e6, g_bridge_ns / n / 1e6, g_bridge_calls / n, g_swap_ns / n / 1e6);
        trace("glprof_detail: unity[scripts=%.2f coro=%.2f anim=%.2f fixed=%.2f phys=%.2f skin=%.2f render+gui=%.2f ms] "
              "bridge[enc=%.2f write=%.2f sync=%.2f ms] "
              "work[draws=%.0f vtx=%.1fKB unif=%.0f io_opens=%.1f io=%.1fKB tex=%.1fKB buf=%.1fKB]",
              g_u_sum[2] / n / 1e6, g_u_sum[3] / n / 1e6, g_u_sum[5] / n / 1e6,
              g_u_sum[0] / n / 1e6, g_u_sum[1] / n / 1e6, g_u_sum[4] / n / 1e6,
              (g_u_render_sum + g_u_sum[6]) / n / 1e6,
              bridge_enc_ns / n / 1e6, g_write_ns / n / 1e6, g_sync_ns / n / 1e6,
              g_win_draws / n, g_win_vtx_bytes / n / 1024.0, g_win_unif / n,
              g_win_opens / n, g_win_io_bytes / n / 1024.0,
              g_win_tex_bytes / n / 1024.0, g_win_buf_bytes / n / 1024.0);
        g_step_ns = g_bridge_ns = g_bridge_calls = g_swap_ns = g_cpu_ns = g_period_ns = 0;
        g_write_ns = g_sync_ns = g_u_render_sum = 0;
        memset(g_u_sum, 0, sizeof(g_u_sum));
        g_win_draws = g_win_vtx_bytes = g_win_unif = 0;
        g_win_opens = g_win_io_bytes = g_win_tex_bytes = g_win_buf_bytes = 0;
    }
}

extern "C" void gl_bridge_swap(void)
{
    Lock l;
    send_msg(GB_OP_SWAP, NULL, 0, NULL, 0);
    out_flush();
    g_inflight++;
    /* The flush above is bridge time; the wait below is the host's. */
    uint64_t w0 = g_profile > 0 ? now_ns() : 0;
    while (g_inflight > g_max_inflight) {
        uint32_t v = 0;
        read_all(&v, 4);
        if (v != kAck) {
            errno = EPROTO;
            gone("unexpected reply while waiting for a frame");
        }
        g_inflight--;
    }
    if (g_profile > 0) {
        uint64_t w = now_ns() - w0;
        g_swap_ns += w;
        g_last_swap_ns += w;
        g_bridge_ns -= w;  /* the Lock destructor will count the wait as bridge time */
    }
    g_frames++;
    if (g_frames <= 3)
        trace("gl bridge: frame %ld sent", g_frames);
}

extern "C" void *port_gl_getproc(const char *name)
{
    if (!gl_bridge_enabled())
        return SDL_GL_GetProcAddress(name);
    if (!name)
        return NULL;
    for (int i = 0; kEntries[i].name; i++) {
        if (strcmp(kEntries[i].name, name) == 0)
            return kEntries[i].fn;
    }
    return NULL;
}
