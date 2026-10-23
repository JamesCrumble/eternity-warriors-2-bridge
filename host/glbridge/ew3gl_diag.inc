/*
 * ew3gl module 12/12: diagnostics that need a look after an op has run.
 *
 * Shader compile and program link failures: the engine finds out through the
 * synchronous queries, but Unity 4.5 logs nothing, and a failed shader leaves
 * the object it draws black or missing. Every failure is logged with the
 * compiler's message and the start of the source (limited to 40 of each).
 * EW3_GL_DIAG=0 turns it off.
 */
static long g_diag_compile_fails, g_diag_link_fails;

static void diag_log_source(GLuint sh)
{
    GLint len = 0;
    glGetShaderiv(sh, GL_SHADER_SOURCE_LENGTH, &len);
    if (len <= 0)
        return;
    char *src = (char *)calloc(1, (size_t)len + 1);
    if (!src)
        return;
    glGetShaderSource(sh, len, NULL, src);
    /* The first lines are enough to recognise the shader; the whole thing can be dumped later. */
    int lines = 0;
    for (char *p = src; *p; p++)
        if (*p == '\n' && ++lines == 12) {
            *p = 0;
            break;
        }
    glog("  source (start):\n%s", src);
    free(src);
}

/* payload: the message as received; op: what it was. Runs after dispatch(). */
static void diag_after(int op, const uint8_t *payload, const uint8_t *end)
{
    static int on = -1;
    if (on < 0)
        on = (int)cfg_long("GL_DIAG", 1) != 0;
    if (!on)
        return;

    if (end - payload < 4)
        return;
    int32_t guest;
    memcpy(&guest, payload, 4);
    if (op == GB_OP_glCompileShader) {
        GLuint sh = idmap_get(&g_ids.shader, guest);
        GLint ok = 1;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (ok || g_diag_compile_fails++ >= 40)
            return;
        char *log = object_text(sh, 0, GL_INFO_LOG_LENGTH);
        glog("shader %d (host %u) failed to compile: %s", guest, sh, log ? log : "");
        free(log);
        diag_log_source(sh);
    } else if (op == GB_OP_glLinkProgram) {
        GLuint pr = idmap_get(&g_ids.prog, guest);
        GLint ok = 1;
        glGetProgramiv(pr, GL_LINK_STATUS, &ok);
        if (ok || g_diag_link_fails++ >= 40)
            return;
        char *log = object_text(pr, 1, GL_INFO_LOG_LENGTH);
        glog("program %d (host %u) failed to link: %s", guest, pr, log ? log : "");
        free(log);
    }
}
