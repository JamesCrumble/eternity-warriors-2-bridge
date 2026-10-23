/*
 * ew3view: shows the frames a software-rendered run writes to EW3_FB_SHM in a
 * native window (WSLg on Windows 11), and sends the mouse back as touches.
 *
 *   build: tools/play_wslg.sh builds it (see there for the SDL2 headers)
 *   run:   build/ew3view <fb file> <control dir>     (tools/play_wslg.sh does both)
 *
 * Input goes through the port's control channel (<control dir>/commands, read
 * by port/src/control.cpp): left button = one finger, Escape = BACK. Closing
 * the window asks the game to quit.
 */
#include <SDL2/SDL.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct fb { uint32_t magic, w, h, seq; };

static FILE *g_cmd;

static void command(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_cmd, fmt, ap);
    va_end(ap);
    fflush(g_cmd);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <fb file> <control dir>\n", argv[0]);
        return 2;
    }
    char path[1024];
    snprintf(path, sizeof(path), "%s/commands", argv[2]);
    g_cmd = fopen(path, "a");
    if (!g_cmd) {
        perror(path);
        return 1;
    }

    /* Wait for the game to create and fill the header. */
    struct fb *fb = NULL;
    size_t size = 0;
    for (int i = 0; i < 6000 && !fb; i++) {
        int fd = open(argv[1], O_RDONLY);
        struct stat st;
        if (fd >= 0 && fstat(fd, &st) == 0 && (size_t)st.st_size > sizeof(struct fb)) {
            void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
            if (p != MAP_FAILED) {
                struct fb *f = p;
                if (__atomic_load_n(&f->magic, __ATOMIC_ACQUIRE) == 0x46335745u &&
                    (size_t)st.st_size >= sizeof(struct fb) + (size_t)f->w * f->h * 4) {
                    fb = f;
                    size = (size_t)st.st_size;
                } else {
                    munmap(p, (size_t)st.st_size);
                }
            }
        }
        if (fd >= 0)
            close(fd);
        if (!fb)
            usleep(100 * 1000);
    }
    if (!fb) {
        fprintf(stderr, "ew3view: no frames in %s\n", argv[1]);
        return 1;
    }
    (void)size;
    const int w = (int)fb->w, h = (int)fb->h;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("Eternity Warriors 3", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                                       SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!win || !ren) {
        fprintf(stderr, "window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_RenderSetLogicalSize(ren, w, h);
    /* glReadPixels gives RGBA bytes = ABGR8888 on little endian, rows bottom-up. */
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h);

    uint32_t last = ~0u;
    int down = 0, running = 1;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            float x, y;
            switch (ev.type) {
            case SDL_QUIT:
                command("quit\n");
                running = 0;
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
            case SDL_MOUSEMOTION: {
                int mx = ev.type == SDL_MOUSEMOTION ? ev.motion.x : ev.button.x;
                int my = ev.type == SDL_MOUSEMOTION ? ev.motion.y : ev.button.y;
                x = (float)mx;   /* logical size: SDL already maps window -> game pixels */
                y = (float)my;
                if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
                    down = 1;
                    command("touch 1 %.0f %.0f 0\n", x, y);
                } else if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT && down) {
                    down = 0;
                    command("touch 0 %.0f %.0f 0\n", x, y);
                } else if (ev.type == SDL_MOUSEMOTION && down) {
                    command("touch 2 %.0f %.0f 0\n", x, y);
                }
                break;
            }
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                if (ev.key.keysym.sym == SDLK_ESCAPE && !ev.key.repeat)
                    command("key 4 %s\n", ev.type == SDL_KEYDOWN ? "down" : "up");
                break;
            }
        }
        uint32_t seq = __atomic_load_n(&fb->seq, __ATOMIC_ACQUIRE);
        if (seq == last) {
            SDL_Delay(5);   /* no new frame: do not spin a core redrawing the old one */
            continue;
        }
        last = seq;
        SDL_UpdateTexture(tex, NULL, fb + 1, w * 4);
        SDL_RenderClear(ren);
        SDL_RenderCopyEx(ren, tex, NULL, NULL, 0, NULL, SDL_FLIP_VERTICAL);
        SDL_RenderPresent(ren);
    }
    SDL_Quit();
    return 0;
}
