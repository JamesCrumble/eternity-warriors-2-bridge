#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int write_exact(int fd, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t *)buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) { usleep(1000); continue; }
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int read_exact(int fd, void *buf, size_t n) {
    uint8_t *p = (uint8_t *)buf;
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) { usleep(1000); continue; }
            return -1;
        }
        if (r == 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int make_listener(uint16_t port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, 32) != 0) {
        close(s);
        return -1;
    }
    return s;
}

static void *ctl_thread(void *arg) {
    (void)arg;
    int ls = make_listener(18091);
    if (ls < 0) return NULL;
    for (;;) {
        int cfd = accept(ls, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        int one = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        FILE *out = fopen("/tmp/ew2_ctl/commands", "a");
        if (!out) {
            close(cfd);
            continue;
        }
        char buf[1024];
        for (;;) {
            ssize_t n = read(cfd, buf, sizeof(buf));
            if (n > 0) {
                fwrite(buf, 1, (size_t)n, out);
                fflush(out);
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
        fclose(out);
        close(cfd);
    }
    close(ls);
    return NULL;
}

static void *audio_thread(void *arg) {
    (void)arg;
    unlink("/tmp/ew2_audio.fifo");
    mkfifo("/tmp/ew2_audio.fifo", 0666);
    int fifo_fd = open("/tmp/ew2_audio.fifo", O_RDWR);
    if (fifo_fd < 0) return NULL;

    int ls = make_listener(18093);
    if (ls < 0) return NULL;
    for (;;) {
        int cfd = accept(ls, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        int one = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        uint8_t buf[16384];
        for (;;) {
            ssize_t n = read(fifo_fd, buf, sizeof(buf));
            if (n > 0) {
                if (write_exact(cfd, buf, (size_t)n) != 0)
                    break;
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
        close(cfd);
    }
    close(ls);
    return NULL;
}

#define MAX_CONNS 64
struct Conn {
    int fd;
    uint32_t id;
};

static int g_mux_ready = 0;

static void *net_mux_thread(void *arg) {
    (void)arg;
    int ls_mux = make_listener(18092);
    int ls_http = make_listener(18080);
    int ls_cgs  = make_listener(18024);
    if (ls_mux < 0 || ls_http < 0 || ls_cgs < 0) return NULL;
    g_mux_ready = 1;

    int vfd = -1;
    int one = 1;

    struct Conn conns[MAX_CONNS];
    for (int i = 0; i < MAX_CONNS; i++) { conns[i].fd = -1; conns[i].id = 0; }
    uint32_t next_id = 1;
    uint8_t buf[32768];

    for (;;) {
        while (vfd < 0) {
            vfd = accept(ls_mux, NULL, NULL);
            if (vfd < 0) {
                if (errno == EINTR) continue;
                return NULL;
            }
            setsockopt(vfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        }

        struct pollfd pfds[3 + MAX_CONNS];
        int map_idx[3 + MAX_CONNS];
        int nfds = 0;

        pfds[nfds] = (struct pollfd){ vfd, POLLIN, 0 };
        map_idx[nfds++] = -1;
        pfds[nfds] = (struct pollfd){ ls_http, POLLIN, 0 };
        map_idx[nfds++] = -2;
        pfds[nfds] = (struct pollfd){ ls_cgs, POLLIN, 0 };
        map_idx[nfds++] = -3;

        for (int i = 0; i < MAX_CONNS; i++) {
            if (conns[i].fd >= 0) {
                pfds[nfds] = (struct pollfd){ conns[i].fd, POLLIN, 0 };
                map_idx[nfds++] = i;
            }
        }

        int rc = poll(pfds, (nfds_t)nfds, 50);
        if (rc <= 0) continue;

        for (int j = 0; j < nfds; j++) {
            short rev = pfds[j].revents;
            if (!rev) continue;
            int idx = map_idx[j];
            if (idx == -2 || idx == -3) {
                int ls = (idx == -2) ? ls_http : ls_cgs;
                uint16_t port = (idx == -2) ? 18080 : 18024;
                int cfd = accept(ls, NULL, NULL);
                if (cfd >= 0) {
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    int slot = -1;
                    for (int i = 0; i < MAX_CONNS; i++) {
                        if (conns[i].fd < 0) { slot = i; break; }
                    }
                    if (slot < 0) {
                        close(cfd);
                    } else {
                        uint32_t cid = next_id++;
                        conns[slot].fd = cfd;
                        conns[slot].id = cid;
                        uint8_t hdr[8];
                        uint16_t cmd = 1;
                        memcpy(hdr, &cid, 4);
                        memcpy(hdr + 4, &cmd, 2);
                        memcpy(hdr + 6, &port, 2);
                        write_exact(vfd, hdr, 8);
                    }
                }
            } else if (idx == -1) {
                if (rev & (POLLHUP | POLLERR)) {
                    close(vfd);
                    vfd = -1;
                    break;
                }
                if (rev & POLLIN) {
                    uint8_t hdr[8];
                    if (read_exact(vfd, hdr, 8) == 0) {
                        uint32_t cid;
                        uint16_t cmd, dlen;
                        memcpy(&cid, hdr, 4);
                        memcpy(&cmd, hdr + 4, 2);
                        memcpy(&dlen, hdr + 6, 2);
                        if (dlen > 0 && dlen <= sizeof(buf)) {
                            read_exact(vfd, buf, dlen);
                        }
                        int slot = -1;
                        for (int i = 0; i < MAX_CONNS; i++) {
                            if (conns[i].fd >= 0 && conns[i].id == cid) { slot = i; break; }
                        }
                        if (slot >= 0) {
                            if (cmd == 2 && dlen > 0) {
                                if (write_exact(conns[slot].fd, buf, dlen) != 0) {
                                    close(conns[slot].fd);
                                    conns[slot].fd = -1;
                                }
                            } else if (cmd == 3) {
                                close(conns[slot].fd);
                                conns[slot].fd = -1;
                            }
                        }
                    } else {
                        close(vfd);
                        vfd = -1;
                        break;
                    }
                }
            } else if (idx >= 0) {
                int cfd = conns[idx].fd;
                uint32_t cid = conns[idx].id;
                ssize_t n = read(cfd, buf, 16384);
                if (n > 0) {
                    uint8_t hdr[8];
                    uint16_t cmd = 2, dlen = (uint16_t)n;
                    memcpy(hdr, &cid, 4);
                    memcpy(hdr + 4, &cmd, 2);
                    memcpy(hdr + 6, &dlen, 2);
                    write_exact(vfd, hdr, 8);
                    write_exact(vfd, buf, (size_t)n);
                } else {
                    close(cfd);
                    conns[idx].fd = -1;
                    uint8_t hdr[8];
                    uint16_t cmd = 3, dlen = 0;
                    memcpy(hdr, &cid, 4);
                    memcpy(hdr + 4, &cmd, 2);
                    memcpy(hdr + 6, &dlen, 2);
                    write_exact(vfd, hdr, 8);
                }
            }
        }
    }
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    const char *root = (argc > 1) ? argv[1] : ".";
    const char *w    = (argc > 2) ? argv[2] : "1280";
    const char *h    = (argc > 3) ? argv[3] : "720";
    const char *gpu  = (argc > 4 && argv[4][0]) ? argv[4] : "Adreno (TM) 420";

    mkdir("/tmp/ew2_ctl", 0755);
    FILE *f = fopen("/tmp/ew2_ctl/commands", "wb");
    if (f) fclose(f);

    pthread_t t_ctl, t_net, t_aud;
    pthread_create(&t_ctl, NULL, ctl_thread, NULL);
    pthread_create(&t_net, NULL, net_mux_thread, NULL);
    pthread_create(&t_aud, NULL, audio_thread, NULL);

    for (int i = 0; i < 50 && !g_mux_ready; i++)
        usleep(10000);

    /* Stage sysroot and game assets into persistent native Linux /opt/ew2_rt (ext4)
     * and bind /opt/ew2_rt/ew2data/files directly to <root>/Saves. */
    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
             "if [ ! -f /opt/ew2_rt/ew2data/base.apk ]; then "
             "mkdir -p /opt/ew2_rt/sysroot/lib/arm-linux-gnueabihf /opt/ew2_rt/app /opt/ew2_rt/ew2data && "
             "cp -rf '%s/third_party/sysroot-armhf/lib/arm-linux-gnueabihf/'*.so* /opt/ew2_rt/sysroot/lib/arm-linux-gnueabihf/ && "
             "cp -rf '%s/third_party/sysroot-armhf/usr/lib/arm-linux-gnueabihf/'*.so* /opt/ew2_rt/sysroot/lib/arm-linux-gnueabihf/ && "
             "cp -f '%s/third_party/sysroot-armhf/lib/arm-linux-gnueabihf/ld-2.31.so' /opt/ew2_rt/sysroot/lib/ld-linux-armhf.so.3 && "
             "cp -f '%s/sdl-arm/lib/libSDL2-2.0.so.0' /opt/ew2_rt/sysroot/lib/arm-linux-gnueabihf/libSDL2-2.0.so.0 && "
             "chmod +x /opt/ew2_rt/sysroot/lib/ld-linux-armhf.so.3 /opt/ew2_rt/sysroot/lib/arm-linux-gnueabihf/*.so* && "
             "cp -rf '%s/ew2data/base.apk' '%s/ew2data/glu_properties.txt' '%s/ew2data/lib' '%s/ew2data/Android' /opt/ew2_rt/ew2data/; "
             "fi && "
             "cp -f '%s/port/build/ew2' /opt/ew2_rt/app/ew2 && chmod +x /opt/ew2_rt/app/ew2 && "
             "mkdir -p '%s/Saves' && rm -rf /opt/ew2_rt/ew2data/files && ln -sfn '%s/Saves' /opt/ew2_rt/ew2data/files",
             root, root, root, root, root, root, root, root, root, root, root);
    if (system(cmd) != 0) {
        fprintf(stderr, "[wsl_runner] warning: staging command returned non-zero\n");
    }

    setenv("QEMU_LD_PREFIX", "/opt/ew2_rt/sysroot", 1);
    setenv("LD_LIBRARY_PATH", "/opt/ew2_rt/sysroot/lib/arm-linux-gnueabihf", 1);
    setenv("TZ", "UTC0", 1);
    setenv("LOADER_TRACE", "1", 1);
    setenv("EW2_GL_PROFILE", "1", 1);
    setenv("EW2_SYSCALL_PROF", "1", 1);
    setenv("EW2_PC_PROF", "1", 1);
    setenv("SDL_VIDEODRIVER", "offscreen", 1);
    setenv("EW2_GL_BRIDGE", "1", 1);
    setenv("EW2_GL_LISTEN", "18090", 1);
    setenv("EW2_GL_INFLIGHT", "4", 1);
    setenv("EW2_AUDIO_DEV", "/tmp/ew2_audio.fifo", 1);
    setenv("EW2_SERVER", "127.0.0.1", 1);
    setenv("EW2_CONTROL_DIR", "/tmp/ew2_ctl", 1);
    setenv("EW2_ALLOW_UNRESOLVED", "1", 1);
    setenv("EW2_WIDTH", w, 1);
    setenv("EW2_HEIGHT", h, 1);
    setenv("EW2_TARGET_FPS", "120", 1);
    setenv("EW2_GL_RENDERER", gpu, 1);

    printf("[wsl_runner] starting qemu-arm /opt/ew2_rt/app/ew2 (%sx%s, GPU=%s)...\n", w, h, gpu);
    fflush(stdout);

    pid_t pid = fork();
    if (pid == 0) {
        char *qargv[] = {
            (char *)"qemu-arm",
            (char *)"-L",
            (char *)"/opt/ew2_rt/sysroot",
            (char *)"/opt/ew2_rt/app/ew2",
            (char *)"/opt/ew2_rt/ew2data",
            NULL
        };
        execvp("qemu-arm", qargv);
        perror("execvp qemu-arm");
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    printf("[wsl_runner] ew2 exited: status=0x%x (code=%d, sig=%d)\n",
           status, WIFEXITED(status) ? WEXITSTATUS(status) : -1,
           WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    fflush(stdout);
    return 0;
}
