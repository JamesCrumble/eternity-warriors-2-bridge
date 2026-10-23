/*
 * ew2gl module 7/12: keyboard and mouse -> Eternity Warriors 2 touch controls.
 *
 * Control Scheme (matching PC controls screenshot):
 *   W, A, S, D / Arrows   8-directional isometric movement (virtual joystick)
 *   LMB (Left Mouse)      Primary Attack / Axe combo (in battle mode; F1 toggles touch/battle)
 *   RMB (Right Mouse)     Block / Shield / Parry (Skill 2)
 *   Space                 Dash / Roll / Evade (Skill 3)
 *   MMB (Middle Mouse)    Hold for Charged Whirlwind (Skill 1), steered with WASD
 *   Key 1                 Health Potion (HP)
 *   Key 2                 Rage Potion (Rage)
 *   J / Enter             Primary Attack (works in any mouse mode)
 *   Esc                   Pause / Back (Android KEYCODE_BACK)
 *   F1                    Toggle mouse mode: "touch" (cursor clicks UI) vs "battle" (LMB = Attack)
 */
enum {
    BTN_ATTACK,
    BTN_SKILL1,   /* Whirlwind (MMB hold) */
    BTN_SKILL2,   /* Block / Parry (RMB) */
    BTN_SKILL3,   /* Dash / Roll (Space) */
    BTN_HP,       /* Health Potion (Key 1) */
    BTN_RAGE,     /* Rage Potion (Key 2) */
    BTN_PAUSE,    /* Pause button */
    BTN_SPECIAL,  /* Special button */
    BTN_COUNT
};
static const char *const kBtnNames[BTN_COUNT] = {
    "attack", "skill1", "skill2", "skill3", "hp", "rage", "pause", "special"
};
/* Exact 1280x720 coordinates from combatScene.unity (UICamera ortho_size=320, bottom-right Anchor):
 *   attack:  (1181, 621) -> (0.9227, 0.8625)
 *   skill1:  (1066, 506) -> (0.8328, 0.7028)  [Whirlwind]
 *   skill2:  (1192, 470) -> (0.9313, 0.6528)  [Block / Parry]
 *   skill3:  (1030, 632) -> (0.8047, 0.8778)  [Dash / Roll]
 *   hp:      (1235, 259) -> (0.9648, 0.3597)  [Health Potion]
 *   rage:    (1235, 169) -> (0.9648, 0.2347)  [Rage Potion]
 *   pause:   (1232,  51) -> (0.9625, 0.0708)
 *   special: (1232, 365) -> (0.9625, 0.5069)
 */
static float g_btn[BTN_COUNT][2] = {
    { 0.9227f, 0.8625f },   /* BTN_ATTACK */
    { 0.8328f, 0.7028f },   /* BTN_SKILL1 (Whirlwind) */
    { 0.9313f, 0.6528f },   /* BTN_SKILL2 (Block) */
    { 0.8047f, 0.8778f },   /* BTN_SKILL3 (Dash) */
    { 0.9648f, 0.3597f },   /* BTN_HP (Key 1) */
    { 0.9648f, 0.2347f },   /* BTN_RAGE (Key 2) */
    { 0.9625f, 0.0708f },   /* BTN_PAUSE */
    { 0.9625f, 0.5069f },   /* BTN_SPECIAL */
};
static int g_btn_down[BTN_COUNT];
/* Joystick center (0.18, 0.72) = (230, 518) at 1280x720, well inside Joystick.validCenter() bounds (>= 128 px). */
static float g_joy[3] = { 0.18f, 0.72f, 0.12f };
static int g_keymap_on = -1, g_mouse_battle;

/* Joystick state: what the keys want, and what was last sent. */
static int g_joy_down, g_joy_wait;
static float g_joy_want[2], g_joy_sent[2];

/* Finger ids: 0 the mouse, 1 the joystick, 2.. one per button. */
#define FINGER_JOY 1
#define FINGER_BTN(b) (2 + (b))

static void input_load_keymap(void)
{
    g_keymap_on = (int)cfg_long("GL_KEYMAP", 1) != 0;
    if (!g_cfg.control_dir)
        return;
    char path[1024], line[256], name[64];
    snprintf(path, sizeof(path), "%s/keymap.txt", g_cfg.control_dir);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        float x, y, r = 0;
        int n = sscanf(line, "%63s %f %f %f", name, &x, &y, &r);
        if (n < 3 || name[0] == '#')
            continue;
        if (!strcmp(name, "joystick")) {
            g_joy[0] = x, g_joy[1] = y;
            if (n == 4)
                g_joy[2] = r;
            continue;
        }
        for (int b = 0; b < BTN_COUNT; b++)
            if (!strcmp(name, kBtnNames[b]))
                g_btn[b][0] = x, g_btn[b][1] = y;
    }
    fclose(f);
    glog("keymap: %s loaded", path);
}

static void btn_press(int b, int down)
{
    if (b < 0 || b >= BTN_COUNT || g_btn_down[b] == down)
        return;
    g_btn_down[b] = down;
    control_cmd("touch %d %.0f %.0f %d\n", down ? 1 : 0, g_btn[b][0] * g_cfg.width, g_btn[b][1] * g_cfg.height,
                FINGER_BTN(b));
}

/* The direction the movement keys ask for, unit length or zero. */
static void joy_from_keys(void)
{
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    float x = (float)((k[SDL_SCANCODE_D] || k[SDL_SCANCODE_RIGHT]) - (k[SDL_SCANCODE_A] || k[SDL_SCANCODE_LEFT]));
    float y = (float)((k[SDL_SCANCODE_S] || k[SDL_SCANCODE_DOWN]) - (k[SDL_SCANCODE_W] || k[SDL_SCANCODE_UP]));
    if (x && y)
        x *= 0.7071f, y *= 0.7071f;
    g_joy_want[0] = x, g_joy_want[1] = y;
}

/*
 * Once per presented frame. The joystick centres itself where the finger lands,
 * so the drag goes out two frames after the touch-down: sent together, the game
 * would see the finger land on the dragged spot and read no direction at all.
 */
static void input_frame(void)
{
    if (g_keymap_on <= 0)
        return;
    joy_from_keys();
    int want = g_joy_want[0] != 0 || g_joy_want[1] != 0;
    float cx = g_joy[0] * g_cfg.width, cy = g_joy[1] * g_cfg.height, r = g_joy[2] * g_cfg.height;
    if (want && !g_joy_down) {
        control_cmd("touch 1 %.0f %.0f %d\n", cx, cy, FINGER_JOY);
        g_joy_down = 1, g_joy_wait = 2;
        g_joy_sent[0] = g_joy_sent[1] = 0;
    } else if (g_joy_down && g_joy_wait > 0) {
        g_joy_wait--;
    } else if (g_joy_down && want &&
               (g_joy_want[0] != g_joy_sent[0] || g_joy_want[1] != g_joy_sent[1])) {
        control_cmd("touch 2 %.0f %.0f %d\n", cx + g_joy_want[0] * r, cy + g_joy_want[1] * r, FINGER_JOY);
        g_joy_sent[0] = g_joy_want[0], g_joy_sent[1] = g_joy_want[1];
    } else if (g_joy_down && !want) {
        control_cmd("touch 0 %.0f %.0f %d\n", cx + g_joy_sent[0] * r, cy + g_joy_sent[1] * r, FINGER_JOY);
        g_joy_down = 0;
    }
}

/* Keys other than movement (polled each frame) and the mode switch. 1 = handled. */
static int input_key(const SDL_KeyboardEvent *e)
{
    if (g_keymap_on <= 0 || e->repeat)
        return 0;
    int down = e->type == SDL_KEYDOWN, b = -1;
    switch (e->keysym.sym) {
    case SDLK_F1:
        if (down) {
            g_mouse_battle = !g_mouse_battle;
            if (!g_mouse_battle)
                btn_press(BTN_ATTACK, 0);
            glog("mouse: %s", g_mouse_battle ? "battle (LMB=Attack, RMB=Block, MMB=Whirlwind)" : "touch (LMB=UI, RMB=Block, MMB=Whirlwind)");
        }
        return 1;
    case SDLK_j: case SDLK_RETURN: b = BTN_ATTACK; break;
    case SDLK_SPACE: b = BTN_SKILL3; break;   /* Dash / Roll / Evade */
    case SDLK_1: case SDLK_KP_1: b = BTN_HP; break;     /* Health Potion */
    case SDLK_2: case SDLK_KP_2: b = BTN_RAGE; break;   /* Rage Potion */
    case SDLK_k: b = BTN_SKILL1; break;       /* Keyboard fallback for Whirlwind */
    case SDLK_l: b = BTN_SKILL2; break;       /* Keyboard fallback for Block */
    default: return 0;
    }
    btn_press(b, down);
    return 1;
}

/* Mouse buttons:
 * - RMB (Right Mouse Button): ALWAYS Block / Parry (BTN_SKILL2)
 * - MMB (Middle Mouse Button / Wheel click & hold): ALWAYS Charged Whirlwind (BTN_SKILL1)
 * - LMB (Left Mouse Button): Primary Attack (BTN_ATTACK) in battle mode (F1), or UI touch in touch mode.
 */
static int input_mouse_button(const SDL_MouseButtonEvent *e)
{
    if (g_keymap_on <= 0)
        return 0;
    int down = e->type == SDL_MOUSEBUTTONDOWN;
    if (e->button == SDL_BUTTON_RIGHT) {
        btn_press(BTN_SKILL2, down);   /* RMB: Block / Parry */
        return 1;
    }
    if (e->button == SDL_BUTTON_MIDDLE) {
        btn_press(BTN_SKILL1, down);   /* MMB hold: Charged Whirlwind */
        return 1;
    }
    if (e->button == SDL_BUTTON_LEFT && g_mouse_battle) {
        btn_press(BTN_ATTACK, down);   /* LMB: Primary Attack in battle mode */
        return 1;
    }
    return 0;
}

/* For the window title. */
static const char *input_mode_name(void)
{
    if (g_keymap_on <= 0)
        return "keys off";
    return g_mouse_battle ? "mouse: battle (F1)" : "mouse: touch (F1)";
}
