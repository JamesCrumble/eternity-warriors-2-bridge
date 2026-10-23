/*
 * ew3gl module 10/12: synchronous queries. The guest blocks until the answer
 * arrives, so every op here replies exactly once, also when it fails.
 */

/* Guest-side op (port/src/gl_bridge.cpp): everything about a linked program in one reply. */
enum { GB_OP_Q_PROGRAM_INFO = 250 };

static int int_count(GLenum pname)
{
    switch (pname) {
    case GL_VIEWPORT: case GL_SCISSOR_BOX: case GL_COLOR_WRITEMASK: return 4;
    case GL_MAX_VIEWPORT_DIMS: case GL_ALIASED_POINT_SIZE_RANGE: case GL_ALIASED_LINE_WIDTH_RANGE: return 2;
    default: return 1;
    }
}

static int float_count(GLenum pname)
{
    switch (pname) {
    case GL_COLOR_CLEAR_VALUE: case GL_BLEND_COLOR: return 4;
    case GL_DEPTH_RANGE: case GL_ALIASED_POINT_SIZE_RANGE: case GL_ALIASED_LINE_WIDTH_RANGE: return 2;
    default: return 1;
    }
}

/* Bytes glReadPixels writes, as the guest measures them (gl_bridge.cpp: image_bytes). */
static size_t pixel_bytes(int32_t w, int32_t h, GLenum format, GLenum type, int32_t align)
{
    int c;
    switch (format) {
    case GL_ALPHA: case GL_LUMINANCE: case GL_DEPTH_COMPONENT: c = 1; break;
    case GL_LUMINANCE_ALPHA: c = 2; break;
    case GL_RGB: c = 3; break;
    default: c = 4; break;
    }
    size_t bpp;
    switch (type) {
    case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_5_5_5_1: bpp = 2; break;
    case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT_OES: bpp = 2 * (size_t)c; break;
    case GL_FLOAT: case GL_INT: case GL_UNSIGNED_INT: bpp = 4 * (size_t)c; break;
    default: bpp = (size_t)c; break;
    }
    if (w <= 0 || h <= 0)
        return 0;
    size_t row = (size_t)w * bpp;
    if (align > 1)
        row = (row + (size_t)align - 1) / (size_t)align * (size_t)align;
    return row * (size_t)(h - 1) + (size_t)w * bpp;
}

/* An info log or source of a shader/program, as one string (caller frees). */
static char *object_text(GLuint obj, int is_prog, GLenum len_pname)
{
    GLint len = 0;
    if (is_prog)
        glGetProgramiv(obj, len_pname, &len);
    else
        glGetShaderiv(obj, len_pname, &len);
    char *s = (char *)calloc(1, (size_t)(len > 0 ? len : 0) + 1);
    if (s && len > 0) {
        if (len_pname == GL_SHADER_SOURCE_LENGTH)
            glGetShaderSource(obj, len, NULL, s);
        else if (is_prog)
            glGetProgramInfoLog(obj, len, NULL, s);
        else
            glGetShaderInfoLog(obj, len, NULL, s);
    }
    return s;
}

static void reply_owned(char *s)
{
    reply_str(s);
    free(s);
}

/* Q_ACTIVE_ATTRIB / Q_ACTIVE_UNIFORM: program index bufsize -> size type name. */
static void q_active(int attrib)
{
    GLuint prog = idmap_get(&g_ids.prog, rd_i());
    GLuint index = (GLuint)rd_i();
    struct { int32_t size, type; char name[256]; } r;
    GLsizei len = 0;
    GLint size = 0;
    GLenum type = 0;
    r.name[0] = 0;
    if (attrib)
        glGetActiveAttrib(prog, index, sizeof(r.name), &len, &size, &type, r.name);
    else
        glGetActiveUniform(prog, index, sizeof(r.name), &len, &size, &type, r.name);
    r.size = size;
    r.type = (int32_t)type;
    reply(&r, 8 + (size_t)(len > 0 ? len : 0));
}

/* Q_ATTRIB_LOC / Q_UNIFORM_LOC: program, then a NUL-terminated name. */
static void q_location(int attrib)
{
    GLuint prog = idmap_get(&g_ids.prog, rd_i());
    char name[256];
    size_t n = 0;
    while (g_p + n < g_end && g_p[n] && n < sizeof(name) - 1) {
        name[n] = (char)g_p[n];
        n++;
    }
    name[n] = 0;
    reply_int(attrib ? glGetAttribLocation(prog, name) : glGetUniformLocation(prog, name));
}

/*
 * Q_PROGRAM_INFO: link, loglen, nattr, maxattr, nuni, maxuni; then for every
 * attribute and uniform: size, type, location, name length, name (padded to 4).
 */
static void q_program_info(void)
{
    GLuint prog = idmap_get(&g_ids.prog, rd_i());
    static const GLenum kPn[6] = { GL_LINK_STATUS, GL_INFO_LOG_LENGTH, GL_ACTIVE_ATTRIBUTES,
                                   GL_ACTIVE_ATTRIBUTE_MAX_LENGTH, GL_ACTIVE_UNIFORMS, GL_ACTIVE_UNIFORM_MAX_LENGTH };
    int32_t st[6];
    for (int k = 0; k < 6; k++) {
        GLint v = 0;
        glGetProgramiv(prog, kPn[k], &v);
        st[k] = v;
    }
    if (!st[0])
        st[2] = st[4] = 0;
    size_t cap = 24 + (size_t)(st[2] + st[4]) * (16 + 260), n = 24;
    uint8_t *b = (uint8_t *)malloc(cap);
    if (!b) {
        reply(NULL, 0);
        return;
    }
    memcpy(b, st, 24);
    for (int pass = 0; pass < 2; pass++) {
        for (int32_t k = 0; k < (pass ? st[4] : st[2]); k++) {
            char name[256] = "";
            GLsizei len = 0;
            GLint size = 0;
            GLenum type = 0;
            if (pass == 0)
                glGetActiveAttrib(prog, (GLuint)k, sizeof(name), &len, &size, &type, name);
            else
                glGetActiveUniform(prog, (GLuint)k, sizeof(name), &len, &size, &type, name);
            int32_t e[4] = { size, (int32_t)type,
                             pass ? glGetUniformLocation(prog, name) : glGetAttribLocation(prog, name), len };
            memcpy(b + n, e, 16);
            memset(b + n + 16, 0, ((size_t)len + 3) & ~(size_t)3);
            memcpy(b + n + 16, name, (size_t)len);
            n += 16 + (((size_t)len + 3) & ~(size_t)3);
        }
    }
    reply(b, n);
    free(b);
}

/* Returns 0 when op is not a query. */
static int exec_query(int op)
{
    switch (op) {
    case GB_OP_Q_GET_STRING:
        reply_str((const char *)glGetString((GLenum)rd_i()));
        return 1;
    case GB_OP_Q_GET_INTEGERV: {
        GLenum pname = (GLenum)rd_i();
        GLint v[64] = { 0 };
        int n = int_count(pname);
        if (pname == GL_COMPRESSED_TEXTURE_FORMATS) {
            GLint num = 0;
            glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &num);
            n = num < 60 ? num : 60;
            if (n > 0)
                glGetIntegerv(pname, v);
            v[n++] = 0x8D64; /* GL_ETC1_RGB8_OES */
            v[n++] = 0x8C92; /* GL_ATC_RGB_AMD */
            v[n++] = 0x8C93; /* GL_ATC_RGBA_EXPLICIT_ALPHA_AMD */
            v[n++] = 0x87EE; /* GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD */
            reply_ints(v, n);
            return 1;
        }
        glGetIntegerv(pname, v);
        if (pname == GL_NUM_COMPRESSED_TEXTURE_FORMATS)
            v[0] = (v[0] < 60 ? v[0] : 60) + 4;
        switch (pname) {   /* the guest knows objects by its own names */
        case GL_FRAMEBUFFER_BINDING: v[0] = idmap_reverse(&g_ids.fbo, (GLuint)v[0]); break;
        case GL_RENDERBUFFER_BINDING: v[0] = idmap_reverse(&g_ids.rbo, (GLuint)v[0]); break;
        case GL_TEXTURE_BINDING_2D: case GL_TEXTURE_BINDING_CUBE_MAP: v[0] = idmap_reverse(&g_ids.tex, (GLuint)v[0]); break;
        case GL_ARRAY_BUFFER_BINDING: case GL_ELEMENT_ARRAY_BUFFER_BINDING: v[0] = idmap_reverse(&g_ids.buf, (GLuint)v[0]); break;
        case GL_CURRENT_PROGRAM: v[0] = idmap_reverse(&g_ids.prog, (GLuint)v[0]); break;
        default: break;
        }
        reply_ints(v, n);
        return 1;
    }
    case GB_OP_Q_GET_FLOATV: {
        GLenum pname = (GLenum)rd_i();
        GLfloat v[16] = { 0 };
        glGetFloatv(pname, v);
        reply(v, (size_t)float_count(pname) * 4);
        return 1;
    }
    case GB_OP_Q_GET_SHADERIV: {
        GLuint sh = idmap_get(&g_ids.shader, rd_i());
        GLint v = 0;
        glGetShaderiv(sh, (GLenum)rd_i(), &v);
        reply_int(v);
        return 1;
    }
    case GB_OP_Q_GET_PROGRAMIV: {
        GLuint pr = idmap_get(&g_ids.prog, rd_i());
        GLint v = 0;
        glGetProgramiv(pr, (GLenum)rd_i(), &v);
        reply_int(v);
        return 1;
    }
    case GB_OP_Q_SHADER_LOG:
        reply_owned(object_text(idmap_get(&g_ids.shader, rd_i()), 0, GL_INFO_LOG_LENGTH));
        return 1;
    case GB_OP_Q_PROGRAM_LOG:
        reply_owned(object_text(idmap_get(&g_ids.prog, rd_i()), 1, GL_INFO_LOG_LENGTH));
        return 1;
    case GB_OP_Q_SHADER_SOURCE:
        reply_owned(object_text(idmap_get(&g_ids.shader, rd_i()), 0, GL_SHADER_SOURCE_LENGTH));
        return 1;
    case GB_OP_Q_ACTIVE_ATTRIB: q_active(1); return 1;
    case GB_OP_Q_ACTIVE_UNIFORM: q_active(0); return 1;
    case GB_OP_Q_ATTRIB_LOC: q_location(1); return 1;
    case GB_OP_Q_UNIFORM_LOC: q_location(0); return 1;
    case GB_OP_Q_CHECK_FB:
        reply_int((int32_t)glCheckFramebufferStatus((GLenum)rd_i()));
        return 1;
    case GB_OP_Q_READ_PIXELS: {
        int32_t x = rd_i(), y = rd_i(), w = rd_i(), h = rd_i(), f = rd_i(), ty = rd_i(), align = rd_i();
        size_t n = pixel_bytes(w, h, (GLenum)f, (GLenum)ty, align);
        uint8_t *px = (uint8_t *)calloc(1, n + 4);
        if (px) {
            glPixelStorei(GL_PACK_ALIGNMENT, align > 0 ? align : 4);
            glReadPixels(x, y, w, h, (GLenum)f, (GLenum)ty, px);
        }
        reply(px, px ? n : 0);
        free(px);
        return 1;
    }
    case GB_OP_Q_PROGRAM_INFO: q_program_info(); return 1;
    default:
        return 0;
    }
    reply(NULL, 0);
    return 1;
}
