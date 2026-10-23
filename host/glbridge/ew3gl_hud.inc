/*
 * ew3gl module 4/12: frame-rate counter, drawn over the game's frame just before
 * it is presented, and shown in the window title.
 *
 * The digits are seven-segment glyphs made of scissored glClear rectangles: no
 * shader, buffer or texture of ours, and only five pieces of GL state to put
 * back (framebuffer, scissor test and box, clear colour, colour mask), so the
 * game never sees a difference.
 *
 * EW3_GL_HUD=1 (default) shows it; F2 toggles it in the window.
 */
static const char *input_mode_name(void);   /* module 7 */

static int g_hud_on = -1;
static double g_hud_fps;           /* presents per second over the last half second */
static uint64_t g_hud_t0;
static long g_hud_frames;

/* Segments a b c d e f g as bits 0..6 (a top, then clockwise, g middle). */
static unsigned char seg_mask(char c)
{
    switch (c) {
    case '0': return 0x3F; case '1': return 0x06; case '2': return 0x5B; case '3': return 0x4F;
    case '4': return 0x66; case '5': return 0x6D; case '6': return 0x7D; case '7': return 0x07;
    case '8': return 0x7F; case '9': return 0x6F;
    case 'F': return 0x71; case 'P': return 0x73; case 'S': return 0x6D;
    default: return 0;
    }
}

/* A filled rectangle in window pixels, (x, y) = top-left. */
static void hud_rect(int x, int y, int w, int h, float r, float g, float b)
{
    glScissor(x, g_cfg.height - y - h, w, h);
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

/* One glyph with segment thickness t, glyph box 4t x 7t. */
static void hud_glyph(int x, int y, int t, char c)
{
    unsigned char m = seg_mask(c);
    const float r = 0.35f, g = 1.0f, b = 0.35f;
    int w = 4 * t, h = 7 * t, mid = y + 3 * t;
    if (m & 0x01) hud_rect(x, y, w, t, r, g, b);                   /* a */
    if (m & 0x02) hud_rect(x + w - t, y, t, 4 * t, r, g, b);       /* b */
    if (m & 0x04) hud_rect(x + w - t, mid, t, 4 * t, r, g, b);     /* c */
    if (m & 0x08) hud_rect(x, y + h - t, w, t, r, g, b);           /* d */
    if (m & 0x10) hud_rect(x, mid, t, 4 * t, r, g, b);             /* e */
    if (m & 0x20) hud_rect(x, y, t, 4 * t, r, g, b);               /* f */
    if (m & 0x40) hud_rect(x, mid, w, t, r, g, b);                 /* g */
}

static void hud_draw(const char *text)
{
    GLint fbo = 0, box[4] = { 0 }, mask[4] = { 1, 1, 1, 1 };
    GLfloat clear[4] = { 0 };
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_SCISSOR_BOX, box);
    glGetIntegerv(GL_COLOR_WRITEMASK, mask);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glEnable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    int t = g_cfg.height / 240 > 2 ? g_cfg.height / 240 : 2;
    int n = (int)strlen(text), x0 = 8, y0 = 8, pad = t * 2;
    hud_rect(x0, y0, pad * 2 + n * 6 * t - 2 * t, pad * 2 + 7 * t, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < n; i++)
        if (text[i] != ' ')
            hud_glyph(x0 + pad + i * 6 * t, y0 + pad, t, text[i]);

    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    glColorMask((GLboolean)mask[0], (GLboolean)mask[1], (GLboolean)mask[2], (GLboolean)mask[3]);
    glScissor(box[0], box[1], box[2], box[3]);
    if (!scissor)
        glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
}

/* Called for every frame just before it is presented. */
static void hud_frame(SDL_Window *win)
{
    if (g_hud_on < 0)
        g_hud_on = (int)cfg_long("GL_HUD", 1) != 0;
    uint64_t now = now_ns();
    if (!g_hud_t0)
        g_hud_t0 = now;
    g_hud_frames++;
    if (now - g_hud_t0 >= 500000000ull) {
        g_hud_fps = (double)g_hud_frames * 1e9 / (double)(now - g_hud_t0);
        g_hud_frames = 0;
        g_hud_t0 = now;
        char title[160];
        snprintf(title, sizeof(title), "Eternity Warriors 2 - %.0f fps - %s", g_hud_fps, input_mode_name());
        SDL_SetWindowTitle(win, title);
    }
    if (g_hud_on) {
        char text[32];
        snprintf(text, sizeof(text), "%3.0f FPS", g_hud_fps);
        hud_draw(text);
    }
}

static void hud_toggle(void)
{
    g_hud_on = !g_hud_on;
}
