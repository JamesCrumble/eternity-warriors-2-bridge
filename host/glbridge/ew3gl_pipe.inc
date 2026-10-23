/*
 * ew3gl module 3/12: message framing, payload decoding, replies.
 *
 * Guest -> host, little endian, one message per GL call:
 *     u32 total length (header included, multiple of 4)   u32 op   payload
 * Host -> guest: u32 length + bytes for a query, or the single word
 * 0xFFFFFFFF when a frame has been presented (port/src/gl_bridge.cpp, kAck).
 * The bytes travel over the transport of module 2 (fifos or TCP).
 */

/* ------------------------------------------------------------- input buffer */

/* Bytes read but not yet consumed: [g_in + g_in_pos, g_in + g_in_len). */
static uint8_t *g_in;
static size_t g_in_cap, g_in_pos, g_in_len;

/*
 * Makes `need` bytes available contiguously at g_in + g_in_pos, reading as much
 * as the pipe has each time (one read() covers hundreds of small messages).
 * Returns 0 when the guest has gone or the host is shutting down.
 */
static int in_fill(size_t need)
{
    while (g_in_len - g_in_pos < need) {
        if (g_in_pos && g_in_pos == g_in_len) {
            g_in_pos = g_in_len = 0;
        } else if (g_in_pos && g_in_pos + need > g_in_cap) {
            memmove(g_in, g_in + g_in_pos, g_in_len - g_in_pos);
            g_in_len -= g_in_pos;
            g_in_pos = 0;
        }
        if (g_in_pos + need > g_in_cap) {
            size_t cap = g_in_cap ? g_in_cap : (4u << 20);
            while (cap < g_in_pos + need)
                cap *= 2;
            uint8_t *b = (uint8_t *)realloc(g_in, cap);
            if (!b)
                gfatal("out of memory for a %zu byte message", need);
            g_in = b;
            g_in_cap = cap;
        }
        long r = net_read(g_in + g_in_len, g_in_cap - g_in_len);
        if (r < 0) {
            glog("the guest closed the command pipe");
            return 0;
        }
        if (r == 0) {
            if (!g_alive)
                return 0;
            if (!net_wait(10))
                window_pump();
            continue;
        }
        g_in_len += (size_t)r;
    }
    return 1;
}

/* ------------------------------------------------------------- payload decoding */

/* The payload of the message being executed. Reads past its end yield zeros
 * and set g_underrun, so a malformed message cannot walk into the next one. */
static const uint8_t *g_p, *g_end;
static int g_underrun;

static int32_t rd_i(void)
{
    if (g_end - g_p < 4) {
        g_underrun = 1;
        return 0;
    }
    int32_t v;
    memcpy(&v, g_p, 4);
    g_p += 4;
    return v;
}

static GLfloat rd_f(void)
{
    int32_t i = rd_i();
    GLfloat f;
    memcpy(&f, &i, 4);
    return f;
}

/* The next n bytes (NULL when n < 0: the guest passed a NULL pointer), consumed padded to 4. */
static const void *rd_data(int32_t n)
{
    if (n < 0)
        return NULL;
    size_t padded = ((size_t)n + 3) & ~(size_t)3;
    if ((size_t)(g_end - g_p) < (size_t)n) {
        g_underrun = 1;
        g_p = g_end;
        return NULL;
    }
    const void *d = g_p;
    g_p += (size_t)(g_end - g_p) < padded ? (size_t)(g_end - g_p) : padded;
    return d;
}

/* ------------------------------------------------------------- replies */

static uint8_t *g_out;
static size_t g_out_cap;

/* u32 length + bytes. Every synchronous op must send exactly one, or the guest waits forever. */
static void reply(const void *data, size_t n)
{
    if (g_out_cap < 4 + n) {
        size_t cap = g_out_cap ? g_out_cap : 65536;
        while (cap < 4 + n)
            cap *= 2;
        g_out = (uint8_t *)realloc(g_out, cap);
        if (!g_out)
            gfatal("out of memory for a %zu byte reply", n);
        g_out_cap = cap;
    }
    uint32_t len = (uint32_t)n;
    memcpy(g_out, &len, 4);
    if (n)
        memcpy(g_out + 4, data, n);
    if (net_write(g_out, 4 + n) != 0) {
        glog("reply failed; the guest is gone");
        g_alive = 0;
    }
}

static void reply_ints(const int32_t *v, int n) { reply(v, (size_t)n * 4); }
static void reply_int(int32_t v) { reply(&v, 4); }
static void reply_str(const char *s) { reply(s ? s : "", s ? strlen(s) : 0); }

/* Frame presented: the guest may start another one (it runs EW3_GL_INFLIGHT frames ahead). */
static void ack_frame(void)
{
    uint32_t ack = 0xFFFFFFFFu;
    if (net_write(&ack, 4) != 0)
        g_alive = 0;
}
