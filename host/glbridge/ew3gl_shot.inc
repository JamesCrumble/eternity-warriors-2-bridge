/*
 * ew3gl module 5/12: screenshots of the presented frame (HUD included).
 *
 * F12 in the window, or a file named "shot" appearing in EW3_CONTROL_DIR (so a
 * script, or a person in another terminal, can ask for one: `touch ctl/shot`).
 * The frame goes to <control dir>/ew3gl_shot.bmp (32-bit, bottom-up) and the
 * trigger file is removed. The trigger is checked every 30 frames: on Windows
 * the control directory is on the WSL share, where a stat is not free.
 */
static int g_shot_wanted;

static void shot_save(void)
{
    if (!g_cfg.control_dir) {
        glog("screenshot: EW3_CONTROL_DIR is not set");
        return;
    }
    int w = g_cfg.width, h = g_cfg.height;
    uint8_t *px = (uint8_t *)malloc((size_t)w * (size_t)h * 4);
    if (!px)
        return;
    GLint fbo = 0, pack = 4;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glPixelStorei(GL_PACK_ALIGNMENT, pack);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {   /* RGBA -> BGRA */
        uint8_t t = px[i * 4];
        px[i * 4] = px[i * 4 + 2];
        px[i * 4 + 2] = t;
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/ew3gl_shot.bmp", g_cfg.control_dir);
    FILE *f = fopen(path, "wb");
    if (!f) {
        glog("screenshot: cannot write %s", path);
        free(px);
        return;
    }
    uint32_t data = (uint32_t)w * (uint32_t)h * 4;
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t v;
    v = 54 + data; memcpy(hdr + 2, &v, 4);
    v = 54; memcpy(hdr + 10, &v, 4);
    v = 40; memcpy(hdr + 14, &v, 4);
    v = (uint32_t)w; memcpy(hdr + 18, &v, 4);
    v = (uint32_t)h; memcpy(hdr + 22, &v, 4);          /* positive: rows bottom-up, as GL reads them */
    hdr[26] = 1;
    hdr[28] = 32;
    memcpy(hdr + 34, &data, 4);
    fwrite(hdr, 1, sizeof(hdr), f);
    fwrite(px, 1, data, f);
    fclose(f);
    free(px);
    glog("screenshot: frame %ld -> %s", g_swaps, path);
}

/* Called for every frame just before it is presented. */
static void shot_frame(void)
{
    if (g_cfg.control_dir && g_swaps % 30 == 0) {
        char trig[1024];
        snprintf(trig, sizeof(trig), "%s/shot", g_cfg.control_dir);
        FILE *t = fopen(trig, "rb");
        if (t) {
            fclose(t);
            remove(trig);
            g_shot_wanted = 1;
        }
    }
    if (g_shot_wanted) {
        g_shot_wanted = 0;
        shot_save();
    }
}
