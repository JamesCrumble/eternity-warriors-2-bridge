/*
 * ew3gl module 6/12: the window, its GLES context, presenting, and input.
 *
 * SDL2 gives one code path for both targets: on WSLg it makes an EGL context
 * on the Windows GPU (Mesa d3d12), on Windows it loads ANGLE's libEGL.dll /
 * libGLESv2.dll (SDL_HINT_OPENGL_ES_DRIVER). The guest draws to the default
 * framebuffer at EW3_WIDTH x EW3_HEIGHT, so the window keeps exactly that
 * drawable size and mouse coordinates map 1:1 to the guest's surface.
 *
 * Input goes back as the port's control commands (port/src/control.h), the
 * same lines tools/ew3view.c writes: left button = one finger, Escape = BACK.
 * F2 toggles the frame-rate counter (module 4), F12 saves a screenshot (module 5);
 * WASD, F1 and the other game keys are module 7's.
 */
/* Keyboard and mouse mapping (module 7, included after this one). */
static void input_load_keymap(void);
static void input_frame(void);
static int input_key(const SDL_KeyboardEvent *e);
static int input_mouse_button(const SDL_MouseButtonEvent *e);

static SDL_Window *g_win;
static SDL_GLContext g_ctx;
static FILE *g_ctl;                /* <control dir>/commands, NULL = no input */
static int g_touch_down;
static uint64_t g_quit_at;         /* when "quit" was sent; 0 = not yet */

static void control_cmd(const char *fmt, ...)
{
    if (!g_ctl)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_ctl, fmt, ap);
    va_end(ap);
    fflush(g_ctl);
}

/*
 * Context version: 3.0 when available (3D textures, invalidate), else 2.0.
 * The ES profile must already be set when the window is created: SDL on Windows
 * prepares the window for WGL otherwise, and an EGL (ANGLE) context created
 * later never becomes current on it.
 */
static void es_attributes(int major)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
}

static SDL_GLContext make_context(int major)
{
    es_attributes(major);
    return SDL_GL_CreateContext(g_win);
}

static void window_open(void)
{
    SDL_SetMainReady();   /* main() is ours (SDL_MAIN_HANDLED on Windows) */
    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
        gfatal("SDL_Init: %s", SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);   /* opaque: the game never uses destination alpha on screen */
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    int major = (int)cfg_long("GL_ES", 3);   /* EW3_GL_ES=2 asks for a 2.0 context straight away */
    es_attributes(major);
    Uint32 flags = SDL_WINDOW_OPENGL | (g_cfg.fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
    g_win = SDL_CreateWindow("Eternity Warriors 2", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             g_cfg.width, g_cfg.height, flags);
    if (!g_win)
        gfatal("SDL_CreateWindow: %s", SDL_GetError());
    g_ctx = make_context(major);
    if (!g_ctx && major > 2) {
        glog("no GLES %d.0 context (%s); trying 2.0", major, SDL_GetError());
        g_ctx = make_context(2);
    }
    if (!g_ctx)
        gfatal("no GLES context: %s", SDL_GetError());
    if (SDL_GL_MakeCurrent(g_win, g_ctx) != 0)
        gfatal("SDL_GL_MakeCurrent: %s", SDL_GetError());
    /* The GL functions are linked by name: a context that is not current here shows as NULL strings. */
    if (!glGetString(GL_VERSION))
        gfatal("the GLES context is not current (glGetString(GL_VERSION) is NULL): %s", SDL_GetError());
    blobcache_init();
    if (SDL_GL_SetSwapInterval(g_cfg.vsync ? 1 : 0) != 0 && g_cfg.vsync)
        glog("vsync unavailable: %s", SDL_GetError());

    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(g_win, &dw, &dh);
    glog("window %dx%d (drawable %dx%d), video driver %s", g_cfg.width, g_cfg.height, dw, dh,
         SDL_GetCurrentVideoDriver());
    glog("GL_VENDOR %s", (const char *)glGetString(GL_VENDOR));
    glog("GL_RENDERER %s", (const char *)glGetString(GL_RENDERER));
    glog("GL_VERSION %s", (const char *)glGetString(GL_VERSION));
    const char *r = (const char *)glGetString(GL_RENDERER);
    if (r && (strstr(r, "llvmpipe") || strstr(r, "softpipe")))
        glog("WARNING: this is a software renderer, not the GPU (check WSLg / the Mesa d3d12 driver)");

    if (g_cfg.control_dir) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/commands", g_cfg.control_dir);
        g_ctl = fopen(path, "a");
        if (!g_ctl)
            glog("cannot open %s for input: %s", path, strerror(errno));
    } else {
        glog("EW3_CONTROL_DIR is not set: mouse and keys are not sent to the game");
    }
    input_load_keymap();
}

/* Window pixels -> pixels of the guest's surface (they differ under display scaling). */
static void to_surface(int wx, int wy, float *x, float *y)
{
    int ww = 0, wh = 0;
    SDL_GetWindowSize(g_win, &ww, &wh);
    *x = ww > 0 ? (float)wx * (float)g_cfg.width / (float)ww : (float)wx;
    *y = wh > 0 ? (float)wy * (float)g_cfg.height / (float)wh : (float)wy;
}

static void window_event(const SDL_Event *ev)
{
    float x, y;
    switch (ev->type) {
    case SDL_QUIT:
        if (!g_quit_at) {
            glog("window closed: asking the game to quit");
            control_cmd("quit\n");
            g_quit_at = now_ns();
        }
        break;
    case SDL_MOUSEBUTTONDOWN:
        if (input_mouse_button(&ev->button))
            break;
        if (ev->button.button == SDL_BUTTON_LEFT) {
            to_surface(ev->button.x, ev->button.y, &x, &y);
            g_touch_down = 1;
            control_cmd("touch 1 %.0f %.0f 0\n", x, y);
        }
        break;
    case SDL_MOUSEBUTTONUP:
        if (input_mouse_button(&ev->button))
            break;
        if (ev->button.button == SDL_BUTTON_LEFT && g_touch_down) {
            to_surface(ev->button.x, ev->button.y, &x, &y);
            g_touch_down = 0;
            control_cmd("touch 0 %.0f %.0f 0\n", x, y);
        }
        break;
    case SDL_MOUSEMOTION:
        if (g_touch_down) {
            to_surface(ev->motion.x, ev->motion.y, &x, &y);
            control_cmd("touch 2 %.0f %.0f 0\n", x, y);
        }
        break;
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        if (input_key(&ev->key))
            break;
        if (ev->key.keysym.sym == SDLK_ESCAPE && !ev->key.repeat)
            control_cmd("key 4 %s\n", ev->type == SDL_KEYDOWN ? "down" : "up");   /* KEYCODE_BACK */
        else if (ev->key.keysym.sym == SDLK_F2 && ev->type == SDL_KEYDOWN && !ev->key.repeat)
            hud_toggle();
        else if (ev->key.keysym.sym == SDLK_F12 && ev->type == SDL_KEYDOWN && !ev->key.repeat)
            g_shot_wanted = 1;
        break;
    default:
        break;
    }
}

static void window_pump(void)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
        window_event(&ev);
    /* The game normally exits within a frame or two of "quit"; do not hang on a stuck guest. */
    if (g_quit_at && now_ns() - g_quit_at > 5000000000ull) {
        glog("the game did not quit within 5 s; closing");
        g_alive = 0;
    }
}

/* SWAP: present, then let the guest go on. */
static void window_present(void)
{
    input_frame();
    hud_frame(g_win);
    shot_frame();
    SDL_GL_SwapWindow(g_win);
    g_swaps++;
    ack_frame();
    window_pump();
    GLenum e = glGetError();
    if (e != GL_NO_ERROR)
        GLOG_LIMITED(g_gl_errors, 40, "glGetError 0x%04x at frame %ld", e, g_swaps);
}

static void window_close(void)
{
    if (g_ctl)
        fclose(g_ctl);
    if (g_ctx)
        SDL_GL_DeleteContext(g_ctx);
    if (g_win)
        SDL_DestroyWindow(g_win);
    SDL_Quit();
}
