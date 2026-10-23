/*
 * ew3gl module 8/12: object names, shaders and programs, buffers, textures.
 *
 * Payload layouts are the guest's (port/src/gl_bridge.cpp); a data length of
 * -1 stands for a NULL pointer (glBufferData / glTexImage* with no contents).
 */

/* Bound buffers as the guest names them: draws with client arrays rebind these afterwards (module 5). */
static int32_t g_cur_array, g_cur_elem;

/* True on a GLES 3.0 context: the ES3-only entry points may be called. */
static int es3(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = (const char *)glGetString(GL_VERSION);
        v = s && strstr(s, "OpenGL ES 3") ? 1 : 0;
    }
    return v;
}

static void op_gen(void)
{
    int32_t kind = rd_i(), n = rd_i();
    if (n <= 0 || n > 65536)
        return;
    GLuint stack[64];
    GLuint *host = n <= 64 ? stack : (GLuint *)malloc((size_t)n * sizeof(GLuint));
    if (!host)
        return;
    struct idmap *m = NULL;
    switch (kind) {
    case GEN_TEX: glGenTextures(n, host); m = &g_ids.tex; break;
    case GEN_BUF: glGenBuffers(n, host); m = &g_ids.buf; break;
    case GEN_FBO: glGenFramebuffers(n, host); m = &g_ids.fbo; break;
    case GEN_RBO: glGenRenderbuffers(n, host); m = &g_ids.rbo; break;
    default: GLOG_LIMITED(g_exec_errors, 20, "GEN: unknown kind %d", kind); break;
    }
    for (int32_t k = 0; m && k < n; k++)
        idmap_put(m, rd_i(), host[k]);
    if (host != stack)
        free(host);
}

static void op_delete(void)
{
    int32_t kind = rd_i(), n = rd_i();
    for (int32_t k = 0; k < n && !g_underrun; k++) {
        int32_t g = rd_i();
        GLuint h;
        switch (kind) {
        case GEN_TEX: h = idmap_get(&g_ids.tex, g); glDeleteTextures(1, &h); idmap_put(&g_ids.tex, g, 0); break;
        case GEN_BUF: h = idmap_get(&g_ids.buf, g); glDeleteBuffers(1, &h); idmap_put(&g_ids.buf, g, 0); break;
        case GEN_FBO: h = idmap_get(&g_ids.fbo, g); glDeleteFramebuffers(1, &h); idmap_put(&g_ids.fbo, g, 0); break;
        case GEN_RBO: h = idmap_get(&g_ids.rbo, g); glDeleteRenderbuffers(1, &h); idmap_put(&g_ids.rbo, g, 0); break;
        default: break;
        }
        if (kind == GEN_BUF) {
            if (g == g_cur_array) g_cur_array = 0;
            if (g == g_cur_elem) g_cur_elem = 0;
        }
    }
}

static void op_create_shader(void)
{
    GLenum type = (GLenum)rd_i();
    int32_t id = rd_i();
    idmap_put(&g_ids.shader, id, glCreateShader(type));
}

static void op_create_program(void)
{
    idmap_put(&g_ids.prog, rd_i(), glCreateProgram());
}

static void op_shader_source(void)
{
    GLuint sh = idmap_get(&g_ids.shader, rd_i());
    int32_t n = rd_i();
    const GLchar *src = (const GLchar *)rd_data(n);
    if (!src)
        return;
    GLint len = n;
    glShaderSource(sh, 1, &src, &len);
}

/* ------------------------------------------------------------- buffers */

static void op_bind_buffer(void)
{
    GLenum target = (GLenum)rd_i();
    int32_t id = rd_i();
    if (target == GL_ARRAY_BUFFER)
        g_cur_array = id;
    else if (target == GL_ELEMENT_ARRAY_BUFFER)
        g_cur_elem = id;
    glBindBuffer(target, idmap_get(&g_ids.buf, id));
}

static void op_buffer_data(void)
{
    GLenum target = (GLenum)rd_i();
    int32_t size = rd_i();
    GLenum usage = (GLenum)rd_i();
    int32_t n = rd_i();
    glBufferData(target, size, rd_data(n), usage);
}

static void op_buffer_sub_data(void)
{
    GLenum target = (GLenum)rd_i();
    int32_t offset = rd_i(), size = rd_i(), n = rd_i();
    const void *d = rd_data(n);
    if (d)
        glBufferSubData(target, offset, size, d);
}

/* ------------------------------------------------------------- textures */

/* The guest has already applied its GL_UNPACK_ALIGNMENT when it measured the data, and the
 * glPixelStorei call itself was forwarded, so the host unpacks exactly what was sent. */

static void op_tex_image_2d(void)
{
    int32_t t = rd_i(), lv = rd_i(), ifmt = rd_i(), w = rd_i(), h = rd_i(), bd = rd_i(), f = rd_i(), ty = rd_i();
    int32_t n = rd_i();
    glTexImage2D((GLenum)t, lv, ifmt, w, h, bd, (GLenum)f, (GLenum)ty, rd_data(n));
}

static void op_tex_sub_image_2d(void)
{
    int32_t t = rd_i(), lv = rd_i(), x = rd_i(), y = rd_i(), w = rd_i(), h = rd_i(), f = rd_i(), ty = rd_i();
    int32_t n = rd_i();
    const void *d = rd_data(n);
    if (d)
        glTexSubImage2D((GLenum)t, lv, x, y, w, h, (GLenum)f, (GLenum)ty, d);
}

static uint8_t clamp_u8(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static void decode_etc1_block(const uint8_t *src, uint8_t *block)
{
    static const int kMod[8][4] = {
        {  2,   8,  -2,   -8 },
        {  5,  17,  -5,  -17 },
        {  9,  29,  -9,  -29 },
        { 13,  42, -13,  -42 },
        { 18,  60, -18,  -60 },
        { 24,  80, -24,  -80 },
        { 33, 106, -33, -106 },
        { 47, 183, -47, -183 },
    };
    uint32_t high = ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) | ((uint32_t)src[2] << 8) | src[3];
    uint32_t low  = ((uint32_t)src[4] << 24) | ((uint32_t)src[5] << 16) | ((uint32_t)src[6] << 8) | src[7];
    int flip = high & 1;
    int diff = (high >> 1) & 1;
    int r[2], g[2], b[2];
    if (diff) {
        static const int kSign3[8] = { 0, 1, 2, 3, -4, -3, -2, -1 };
        int r0 = (high >> 27) & 0x1f, dr = kSign3[(high >> 24) & 7];
        int g0 = (high >> 19) & 0x1f, dg = kSign3[(high >> 16) & 7];
        int b0 = (high >> 11) & 0x1f, db = kSign3[(high >> 8)  & 7];
        int r1 = r0 + dr; r1 = r1 < 0 ? 0 : (r1 > 31 ? 31 : r1);
        int g1 = g0 + dg; g1 = g1 < 0 ? 0 : (g1 > 31 ? 31 : g1);
        int b1 = b0 + db; b1 = b1 < 0 ? 0 : (b1 > 31 ? 31 : b1);
        r[0] = (r0 << 3) | (r0 >> 2); r[1] = (r1 << 3) | (r1 >> 2);
        g[0] = (g0 << 3) | (g0 >> 2); g[1] = (g1 << 3) | (g1 >> 2);
        b[0] = (b0 << 3) | (b0 >> 2); b[1] = (b1 << 3) | (b1 >> 2);
    } else {
        int r0 = (high >> 28) & 0xf, r1 = (high >> 24) & 0xf;
        int g0 = (high >> 20) & 0xf, g1 = (high >> 16) & 0xf;
        int b0 = (high >> 12) & 0xf, b1 = (high >> 8)  & 0xf;
        r[0] = (r0 << 4) | r0; r[1] = (r1 << 4) | r1;
        g[0] = (g0 << 4) | g0; g[1] = (g1 << 4) | g1;
        b[0] = (b0 << 4) | b0; b[1] = (b1 << 4) | b1;
    }
    int t[2] = { (int)((high >> 5) & 7), (int)((high >> 2) & 7) };
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int sub = flip ? (y >= 2) : (x >= 2);
            int bit = x * 4 + y;
            int idx = (int)(((low >> (16 + bit)) & 1u) << 1) | (int)((low >> bit) & 1u);
            int m = kMod[t[sub]][idx];
            uint8_t *px = block + (y * 4 + x) * 4;
            px[0] = clamp_u8(r[sub] + m);
            px[1] = clamp_u8(g[sub] + m);
            px[2] = clamp_u8(b[sub] + m);
            px[3] = 255;
        }
    }
}

static uint8_t atc_expand(uint32_t v, int bits)
{
    v <<= 8 - bits;
    return (uint8_t)(v | (v >> bits));
}

static void decode_atc_color_block(const uint8_t *src, uint8_t *block)
{
    uint32_t c0 = (uint32_t)src[0] | ((uint32_t)src[1] << 8);
    uint32_t c1 = (uint32_t)src[2] | ((uint32_t)src[3] << 8);
    uint8_t colors[4][4] = { { 0 } };
    if ((c0 & 0x8000u) == 0) {
        colors[0][0] = atc_expand((c0 >> 10) & 0x1f, 5);
        colors[0][1] = atc_expand((c0 >> 5)  & 0x1f, 5);
        colors[0][2] = atc_expand((c0 >> 0)  & 0x1f, 5);
        colors[3][0] = atc_expand((c1 >> 11) & 0x1f, 5);
        colors[3][1] = atc_expand((c1 >> 5)  & 0x3f, 6);
        colors[3][2] = atc_expand((c1 >> 0)  & 0x1f, 5);
        for (int ch = 0; ch < 3; ch++) {
            colors[1][ch] = (uint8_t)((5 * colors[0][ch] + 3 * colors[3][ch]) / 8);
            colors[2][ch] = (uint8_t)((3 * colors[0][ch] + 5 * colors[3][ch]) / 8);
        }
    } else {
        colors[2][0] = atc_expand((c0 >> 10) & 0x1f, 5);
        colors[2][1] = atc_expand((c0 >> 5)  & 0x1f, 5);
        colors[2][2] = atc_expand((c0 >> 0)  & 0x1f, 5);
        colors[3][0] = atc_expand((c1 >> 11) & 0x1f, 5);
        colors[3][1] = atc_expand((c1 >> 5)  & 0x3f, 6);
        colors[3][2] = atc_expand((c1 >> 0)  & 0x1f, 5);
        for (int ch = 0; ch < 3; ch++) {
            int v = (int)colors[2][ch] - (int)colors[3][ch] / 4;
            colors[1][ch] = (uint8_t)(v < 0 ? 0 : v);
        }
    }
    uint32_t indices = (uint32_t)src[4] | ((uint32_t)src[5] << 8) | ((uint32_t)src[6] << 16) | ((uint32_t)src[7] << 24);
    for (int p = 0; p < 16; p++) {
        int idx = (int)((indices >> (p * 2)) & 3u);
        memcpy(block + p * 4, colors[idx], 3);
        block[p * 4 + 3] = 255;
    }
}

static void decode_atc_explicit_alpha(const uint8_t *src, uint8_t *block)
{
    for (int p = 0; p < 16; p++) {
        uint8_t packed = src[p >> 1];
        uint8_t nib = (p & 1) ? (packed >> 4) : (packed & 0x0f);
        block[p * 4 + 3] = (uint8_t)(nib * 17);
    }
}

static void decode_atc_interp_alpha(const uint8_t *src, uint8_t *block)
{
    uint8_t a[8];
    a[0] = src[0];
    a[1] = src[1];
    if (a[0] > a[1]) {
        for (int i = 1; i <= 6; i++)
            a[i + 1] = (uint8_t)(((7 - i) * a[0] + i * a[1]) / 7);
    } else {
        for (int i = 1; i <= 4; i++)
            a[i + 1] = (uint8_t)(((5 - i) * a[0] + i * a[1]) / 5);
        a[6] = 0;
        a[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++)
        bits |= ((uint64_t)src[2 + i]) << (i * 8);
    for (int p = 0; p < 16; p++)
        block[p * 4 + 3] = a[(bits >> (p * 3)) & 7u];
}

static uint8_t *decode_compressed_rgba(int32_t ifmt, int32_t w, int32_t h, int32_t size, const void *data)
{
    if (!data || w <= 0 || h <= 0 || size <= 0)
        return NULL;
    size_t bpb = (ifmt == 0x8D64 || ifmt == 0x8C92) ? 8 : ((ifmt == 0x8C93 || ifmt == 0x87EE) ? 16 : 0);
    if (!bpb)
        return NULL;
    size_t bx_cnt = ((size_t)w + 3) / 4;
    size_t by_cnt = ((size_t)h + 3) / 4;
    if (bx_cnt * by_cnt * bpb > (size_t)size)
        return NULL;
    static uint8_t *s_rgba = NULL;
    static size_t s_cap = 0;
    size_t need = (size_t)w * (size_t)h * 4;
    if (need > s_cap) {
        uint8_t *nb = (uint8_t *)realloc(s_rgba, need);
        if (!nb)
            return NULL;
        s_rgba = nb;
        s_cap = need;
    }
    const uint8_t *src = (const uint8_t *)data;
    uint8_t block[64];
    for (size_t by = 0; by < by_cnt; by++) {
        for (size_t bx = 0; bx < bx_cnt; bx++) {
            if (ifmt == 0x8D64) {
                decode_etc1_block(src, block);
            } else if (ifmt == 0x8C92) {
                decode_atc_color_block(src, block);
            } else if (ifmt == 0x8C93) {
                decode_atc_color_block(src + 8, block);
                decode_atc_explicit_alpha(src, block);
            } else {
                decode_atc_color_block(src + 8, block);
                decode_atc_interp_alpha(src, block);
            }
            src += bpb;
            for (int y = 0; y < 4; y++) {
                size_t dy = by * 4 + (size_t)y;
                if (dy >= (size_t)h)
                    continue;
                size_t cw = (size_t)w - bx * 4;
                if (cw > 4)
                    cw = 4;
                memcpy(s_rgba + (dy * (size_t)w + bx * 4) * 4, block + y * 16, cw * 4);
            }
        }
    }
    return s_rgba;
}

static void op_compressed_tex_image_2d(void)
{
    int32_t t = rd_i(), lv = rd_i(), ifmt = rd_i(), w = rd_i(), h = rd_i(), bd = rd_i(), size = rd_i();
    int32_t n = rd_i();
    const void *d = rd_data(n);
    uint8_t *rgba = decode_compressed_rgba(ifmt, w, h, size, d);
    if (rgba) {
        glTexImage2D((GLenum)t, lv, GL_RGBA, w, h, bd, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        return;
    }
    if (ifmt == 0x8D64 || ifmt == 0x8C92 || ifmt == 0x8C93 || ifmt == 0x87EE) {
        glTexImage2D((GLenum)t, lv, GL_RGBA, w, h, bd, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        return;
    }
    glCompressedTexImage2D((GLenum)t, lv, (GLenum)ifmt, w, h, bd, size, d);
}

static void op_compressed_tex_sub_image_2d(void)
{
    int32_t t = rd_i(), lv = rd_i(), x = rd_i(), y = rd_i(), w = rd_i(), h = rd_i(), f = rd_i(), size = rd_i();
    int32_t n = rd_i();
    const void *d = rd_data(n);
    if (!d)
        return;
    uint8_t *rgba = decode_compressed_rgba(f, w, h, size, d);
    if (rgba) {
        glTexSubImage2D((GLenum)t, lv, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        return;
    }
    glCompressedTexSubImage2D((GLenum)t, lv, x, y, w, h, (GLenum)f, size, d);
}

static void op_tex_image_3d(void)
{
    int32_t t = rd_i(), lv = rd_i(), ifmt = rd_i(), w = rd_i(), h = rd_i(), dp = rd_i(), bd = rd_i();
    int32_t f = rd_i(), ty = rd_i(), n = rd_i();
    const void *d = rd_data(n);
    if (!es3()) {
        GLOG_LIMITED(g_exec_errors, 20, "glTexImage3D needs a GLES 3.0 context; dropped");
        return;
    }
    glTexImage3D((GLenum)t, lv, ifmt, w, h, dp, bd, (GLenum)f, (GLenum)ty, d);
}

static void op_tex_sub_image_3d(void)
{
    int32_t t = rd_i(), lv = rd_i(), x = rd_i(), y = rd_i(), z = rd_i(), w = rd_i(), h = rd_i(), dp = rd_i();
    int32_t f = rd_i(), ty = rd_i(), n = rd_i();
    const void *d = rd_data(n);
    if (d && es3())
        glTexSubImage3D((GLenum)t, lv, x, y, z, w, h, dp, (GLenum)f, (GLenum)ty, d);
}

static void op_compressed_tex_image_3d(void)
{
    int32_t t = rd_i(), lv = rd_i(), ifmt = rd_i(), w = rd_i(), h = rd_i(), dp = rd_i(), bd = rd_i();
    int32_t size = rd_i(), n = rd_i();
    const void *d = rd_data(n);
    if (es3())
        glCompressedTexImage3D((GLenum)t, lv, (GLenum)ifmt, w, h, dp, bd, size, d);
}

static void op_compressed_tex_sub_image_3d(void)
{
    int32_t t = rd_i(), lv = rd_i(), x = rd_i(), y = rd_i(), z = rd_i(), w = rd_i(), h = rd_i(), dp = rd_i();
    int32_t f = rd_i(), size = rd_i(), n = rd_i();
    const void *d = rd_data(n);
    if (d && es3())
        glCompressedTexSubImage3D((GLenum)t, lv, x, y, z, w, h, dp, (GLenum)f, size, d);
}
