/*
 * ew3gl module 9/12: uniforms, vertex attributes, draws, framebuffer invalidation.
 *
 * Client-side vertex arrays (pointers into guest memory) arrive inside the draw
 * message as blocks; each is uploaded into a per-attribute scratch buffer and
 * the attribute is pointed at it, then the guest's own GL_ARRAY_BUFFER binding
 * is restored. Client indices go through one scratch element buffer likewise.
 */
#define MAX_ATTRIBS 16

static GLuint g_scratch_vbo[MAX_ATTRIBS];
static GLuint g_scratch_ebo;

/* Payload words left in the current message. */
static size_t left_bytes(void) { return (size_t)(g_end - g_p); }

/*
 * UNIFORM_V: kind loc count transpose, then count * per values.
 * kind: 0 1fv, 1 1iv, 2 2fv, 3 2iv, 4 3fv, 5 3iv, 6 4fv, 7 4iv, 8 mat4, 9 mat2, 10 mat3.
 */
static void op_uniform_v(void)
{
    int32_t kind = rd_i(), loc = rd_i(), count = rd_i(), transpose = rd_i();
    int per = kind == 8 ? 16 : kind == 9 ? 4 : kind == 10 ? 9 : kind / 2 + 1;
    if (kind < 0 || kind > 10 || count <= 0 || (size_t)count * (size_t)per * 4 > left_bytes()) {
        GLOG_LIMITED(g_exec_errors, 20, "UNIFORM_V: bad kind %d / count %d", kind, count);
        return;
    }
    /* Messages start 4-aligned in the input buffer, so the values can be used in place. */
    const GLfloat *f = (const GLfloat *)(const void *)g_p;
    const GLint *iv = (const GLint *)(const void *)g_p;
    GLboolean tr = transpose ? GL_TRUE : GL_FALSE;
    switch (kind) {
    case 0: glUniform1fv(loc, count, f); break;
    case 1: glUniform1iv(loc, count, iv); break;
    case 2: glUniform2fv(loc, count, f); break;
    case 3: glUniform2iv(loc, count, iv); break;
    case 4: glUniform3fv(loc, count, f); break;
    case 5: glUniform3iv(loc, count, iv); break;
    case 6: glUniform4fv(loc, count, f); break;
    case 7: glUniform4iv(loc, count, iv); break;
    case 8: glUniformMatrix4fv(loc, count, tr, f); break;
    case 9: glUniformMatrix2fv(loc, count, tr, f); break;
    case 10: glUniformMatrix3fv(loc, count, tr, f); break;
    default: break;
    }
    g_p += (size_t)count * (size_t)per * 4;
}

/* ATTRIB_POINTER_VBO: index size type normalized stride offset (an offset into the bound buffer). */
static void op_attrib_pointer_vbo(void)
{
    int32_t idx = rd_i(), size = rd_i(), type = rd_i(), norm = rd_i(), stride = rd_i(), offset = rd_i();
    glVertexAttribPointer((GLuint)idx, size, (GLenum)type, norm ? GL_TRUE : GL_FALSE, stride,
                          (const void *)(uintptr_t)(uint32_t)offset);
}

static int attr_type_size(GLenum t)
{
    switch (t) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT_OES: return 2;
    default: return 4;
    }
}

/*
 * nclient blocks of: {nattr, stride, len}, then nattr * {index, size, type, normalized, offset},
 * then len bytes (padded). A block is one run of client memory with every attribute that reads
 * from it (interleaved arrays arrive once, not once per attribute); it starts at first_vertex,
 * and the buffer leaves that much room in front so vertex numbers keep their meaning.
 */
static void client_blocks(int32_t nclient, int32_t first_vertex)
{
    for (int32_t k = 0; k < nclient && !g_underrun; k++) {
        int32_t nattr = rd_i(), stride = rd_i(), len = rd_i();
        if (nattr < 0 || nattr > MAX_ATTRIBS) {
            g_underrun = 1;
            break;
        }
        int32_t attrs[MAX_ATTRIBS][5];
        for (int32_t a = 0; a < nattr; a++) {
            attrs[a][0] = rd_i();
            attrs[a][1] = rd_i();
            attrs[a][2] = rd_i();
            attrs[a][3] = rd_i();
            attrs[a][4] = rd_i();
        }
        const void *d = rd_data(len);
        if (k < 0 || k >= MAX_ATTRIBS || !d)
            continue;
        if (!g_scratch_vbo[k])
            glGenBuffers(1, &g_scratch_vbo[k]);
        glBindBuffer(GL_ARRAY_BUFFER, g_scratch_vbo[k]);
        size_t lead = (size_t)(first_vertex > 0 ? first_vertex : 0) * (size_t)stride;
        if (lead == 0) {
            glBufferData(GL_ARRAY_BUFFER, len, d, GL_STREAM_DRAW);
        } else {
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(lead + (size_t)len), NULL, GL_STREAM_DRAW);
            glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)lead, len, d);
        }
        for (int32_t a = 0; a < nattr; a++) {
            int32_t idx = attrs[a][0], size = attrs[a][1], type = attrs[a][2], norm = attrs[a][3], off = attrs[a][4];
            if (idx >= 0 && idx < MAX_ATTRIBS) {
                glVertexAttribPointer((GLuint)idx, size, (GLenum)type, norm ? GL_TRUE : GL_FALSE, stride,
                                      (const void *)(uintptr_t)(uint32_t)off);
            }
        }
    }
    if (nclient > 0)
        glBindBuffer(GL_ARRAY_BUFFER, idmap_get(&g_ids.buf, g_cur_array));
}

/* DRAW_ARRAYS: mode first count nclient, blocks. */
static void op_draw_arrays(void)
{
    int32_t mode = rd_i(), first = rd_i(), count = rd_i(), nclient = rd_i();
    client_blocks(nclient, first);
    if (!g_underrun)
        glDrawArrays((GLenum)mode, first, count);
}

/*
 * DRAW_ELEMENTS: mode count type idx_mode idx_val nclient first_vertex, blocks,
 * then for idx_mode 1 the indices themselves (idx_val bytes); for idx_mode 0 idx_val is
 * the offset into the bound element buffer.
 */
static void op_draw_elements(void)
{
    int32_t mode = rd_i(), count = rd_i(), type = rd_i(), idx_mode = rd_i(), idx_val = rd_i(), nclient = rd_i(), first_vertex = rd_i();
    client_blocks(nclient, first_vertex);
    if (g_underrun)
        return;
    if (idx_mode == 1) {
        const void *d = rd_data(idx_val);
        if (!d)
            return;
        if (!g_scratch_ebo)
            glGenBuffers(1, &g_scratch_ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_scratch_ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx_val, d, GL_STREAM_DRAW);
        glDrawElements((GLenum)mode, count, (GLenum)type, NULL);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, idmap_get(&g_ids.buf, g_cur_elem));
    } else {
        glDrawElements((GLenum)mode, count, (GLenum)type, (const void *)(uintptr_t)(uint32_t)idx_val);
    }
}

/* INVALIDATE_FB: target num sub x y w h, then num attachments. Only a hint: dropped without ES3. */
static void op_invalidate_fb(void)
{
    int32_t target = rd_i(), num = rd_i(), sub = rd_i(), x = rd_i(), y = rd_i(), w = rd_i(), h = rd_i();
    if (num <= 0 || num > 16 || (size_t)num * 4 > left_bytes())
        return;
    GLenum att[16];
    for (int32_t k = 0; k < num; k++)
        att[k] = (GLenum)rd_i();
    if (!es3())
        return;
    if (sub)
        glInvalidateSubFramebuffer((GLenum)target, num, att, x, y, w, h);
    else
        glInvalidateFramebuffer((GLenum)target, num, att);
}

/* BIND_ATTRIB_LOC: program index n, then n bytes of name (no terminator). */
static void op_bind_attrib_loc(void)
{
    GLuint prog = idmap_get(&g_ids.prog, rd_i());
    int32_t idx = rd_i(), n = rd_i();
    const char *s = (const char *)rd_data(n);
    if (!s || n <= 0 || n > 255)
        return;
    char name[256];
    memcpy(name, s, (size_t)n);
    name[n] = 0;
    glBindAttribLocation(prog, (GLuint)idx, name);
}
