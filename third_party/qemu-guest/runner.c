#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static pthread_mutex_t g_vfd_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_vfd = -1;
static volatile int g_saves_ready = 0;

static void setup_fs(void) {
    mkdir("/proc", 0755);
    mkdir("/sys", 0755);
    mkdir("/dev", 0755);
    mkdir("/tmp", 01777);
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mount("tmpfs", "/tmp", "tmpfs", 0, NULL);
    mkdir("/dev/pts", 0755);
    mkdir("/dev/shm", 01777);
    mount("devpts", "/dev/pts", "devpts", 0, NULL);
    mount("tmpfs", "/dev/shm", "tmpfs", 0, NULL);
    mkdir("/tmp/ctl", 0755);
    FILE *f = fopen("/tmp/ctl/commands", "a");
    if (f) fclose(f);
    mkdir("/ew2data/files", 0755);
    mkdir("/ew2data/shared_prefs", 0755);
    mkdir("/system", 0755);
    mkdir("/system/bin", 0755);
    symlink("/bin/true", "/system/bin/ping");
}

static void setup_loopback(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strcpy(ifr.ifr_name, "lo");
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ioctl(s, SIOCSIFADDR, &ifr);
    sin->sin_addr.s_addr = htonl(0xff000000u);
    ioctl(s, SIOCSIFNETMASK, &ifr);
    if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        ioctl(s, SIOCSIFFLAGS, &ifr);
    }
    close(s);
}

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

static int vfd_send(uint32_t cid, uint16_t cmd, const void *payload, uint16_t dlen) {
    pthread_mutex_lock(&g_vfd_lock);
    int fd = g_vfd;
    if (fd < 0) {
        pthread_mutex_unlock(&g_vfd_lock);
        return -1;
    }
    uint8_t hdr[8];
    memcpy(hdr, &cid, 4);
    memcpy(hdr + 4, &cmd, 2);
    memcpy(hdr + 6, &dlen, 2);
    int rc = write_exact(fd, hdr, 8);
    if (rc == 0 && dlen > 0 && payload)
        rc = write_exact(fd, payload, dlen);
    pthread_mutex_unlock(&g_vfd_lock);
    return rc;
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

static void *ctl_thread(void *arg) {
    (void)arg;
    int vfd = open("/dev/vport0p2", O_RDONLY);
    if (vfd < 0) return NULL;
    FILE *out = fopen("/tmp/ctl/commands", "a");
    if (!out) { close(vfd); return NULL; }
    struct pollfd pfd = { vfd, POLLIN, 0 };
    char buf[1024];
    int connected = 0;
    for (;;) {
        int rc = poll(&pfd, 1, connected ? -1 : 100);
        if (rc <= 0) continue;
        if ((pfd.revents & POLLHUP) && !(pfd.revents & POLLIN)) {
            connected = 0;
            usleep(20000);
            continue;
        }
        if (pfd.revents & POLLIN) {
            connected = 1;
            ssize_t n = read(vfd, buf, sizeof(buf));
            if (n > 0) {
                fwrite(buf, 1, (size_t)n, out);
                fflush(out);
            }
        }
    }
    return NULL;
}

#define MAX_CONNS 64
struct Conn {
    int fd;
    uint32_t id;
};

static int make_listener(uint16_t port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, 32) != 0) {
        close(s);
        return -1;
    }
    return s;
}

static const char *kSavePaths[3] = {
    "/ew2data/files/savegame.bin",
    "/ew2data/files/playerprefs.txt",
    "/ew2data/shared_prefs/GWalletHelper.xml"
};
static struct timespec g_save_mtime[3];
static off_t g_save_size[3];

static void write_guest_save(uint32_t cid, const uint8_t *data, uint16_t len) {
    if (cid < 1 || cid > 3) return;
    const char *path = kSavePaths[cid - 1];
    FILE *f = fopen(path, "wb");
    if (f) {
        if (len > 0) fwrite(data, 1, len, f);
        fclose(f);
        struct stat st;
        if (stat(path, &st) == 0) {
            int idx = (int)(cid - 1);
            g_save_mtime[idx] = st.st_mtim;
            g_save_size[idx] = st.st_size;
        }
        printf("[runner] synced host -> guest %s (%u bytes)\n", path, (unsigned)len);
        fflush(stdout);
    }
}

static void check_and_push_saves(void) {
    for (int i = 0; i < 3; i++) {
        struct stat st;
        if (stat(kSavePaths[i], &st) != 0 || st.st_size <= 0 || st.st_size > 32768)
            continue;
        if (st.st_mtim.tv_sec != g_save_mtime[i].tv_sec ||
            st.st_mtim.tv_nsec != g_save_mtime[i].tv_nsec ||
            st.st_size != g_save_size[i]) {
            FILE *f = fopen(kSavePaths[i], "rb");
            if (!f) continue;
            uint8_t buf[32768];
            size_t n = fread(buf, 1, sizeof(buf), f);
            fclose(f);
            if (n > 0) {
                g_save_mtime[i] = st.st_mtim;
                g_save_size[i] = st.st_size;
                vfd_send((uint32_t)(i + 1), 10, buf, (uint16_t)n);
            }
        }
    }
}

static off_t g_log_off = 0;

static void flush_guest_log(void) {
    int fd = open("/tmp/ew2.log", O_RDONLY);
    if (fd < 0)
        return;
    uint8_t buf[16384];
    for (;;) {
        ssize_t n = pread(fd, buf, sizeof(buf), g_log_off);
        if (n <= 0)
            break;
        if (vfd_send(0, 12, buf, (uint16_t)n) != 0)
            break;
        g_log_off += n;
    }
    close(fd);
}

static void *save_watch_thread(void *arg) {
    (void)arg;
    int ticks = 0;
    for (;;) {
        usleep(200000);
        flush_guest_log();
        if (++ticks >= 7) {
            ticks = 0;
            if (g_saves_ready)
                check_and_push_saves();
        }
    }
    return NULL;
}

static void *net_mux_thread(void *arg) {
    (void)arg;
    int vfd = open("/dev/vport0p3", O_RDWR);
    if (vfd < 0) return NULL;
    struct pollfd vp = { vfd, POLLOUT, 0 };
    while (poll(&vp, 1, 100) >= 0 && (vp.revents & POLLHUP))
        usleep(20000);
    g_vfd = vfd;

    int ls_http = make_listener(18080);
    int ls_cgs  = make_listener(18024);
    struct Conn conns[MAX_CONNS];
    for (int i = 0; i < MAX_CONNS; i++) { conns[i].fd = -1; conns[i].id = 0; }
    uint32_t next_id = 1;
    uint8_t buf[32768];

    for (;;) {
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

        int rc = poll(pfds, (nfds_t)nfds, -1);
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
                    int one = 1;
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
                        vfd_send(cid, 1, NULL, port);
                    }
                }
            } else if (idx == -1) {
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
                        if (cmd == 10) {
                            write_guest_save(cid, buf, dlen);
                        } else if (cmd == 11) {
                            g_saves_ready = 1;
                        } else {
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
                        }
                    }
                }
            } else if (idx >= 0) {
                int cfd = conns[idx].fd;
                uint32_t cid = conns[idx].id;
                ssize_t n = read(cfd, buf, 16384);
                if (n > 0) {
                    vfd_send(cid, 2, buf, (uint16_t)n);
                } else {
                    close(cfd);
                    conns[idx].fd = -1;
                    vfd_send(cid, 3, NULL, 0);
                }
            }
        }
    }
    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    setup_fs();
    setup_loopback();

    for (int i = 0; i < 3; i++) {
        struct stat st;
        if (stat(kSavePaths[i], &st) == 0) {
            g_save_mtime[i] = st.st_mtim;
            g_save_size[i] = st.st_size;
        }
    }

    pthread_t t_ctl, t_net, t_sav;
    pthread_create(&t_ctl, NULL, ctl_thread, NULL);
    pthread_create(&t_net, NULL, net_mux_thread, NULL);
    pthread_create(&t_sav, NULL, save_watch_thread, NULL);

    /* Wait up to 2.5s for host to push initial Saves/savegame.bin over vport0p3 */
    for (int i = 0; i < 125 && !g_saves_ready; i++)
        usleep(20000);
    g_saves_ready = 1;

    setenv("LD_LIBRARY_PATH", "/lib/arm-linux-gnueabihf", 1);
    setenv("TZ", "UTC0", 1);
    setenv("LOADER_TRACE", "1", 1);
    setenv("EW2_GL_PROFILE", "1", 1);
    setenv("EW2_SYSCALL_PROF", "1", 1);
    setenv("EW2_PC_PROF", "1", 1);
    setenv("SDL_VIDEODRIVER", "offscreen", 1);
    setenv("EW2_GL_BRIDGE", "1", 1);
    setenv("EW2_GL_CMD", "/dev/vport0p1", 1);
    setenv("EW2_GL_REPLY", "/dev/vport0p1", 1);
    setenv("EW2_AUDIO_DEV", "/dev/vport0p4", 1);
    setenv("EW2_SERVER", "127.0.0.1", 1);
    setenv("EW2_CONTROL_DIR", "/tmp/ctl", 1);
    setenv("EW2_ALLOW_UNRESOLVED", "1", 1);
    setenv("EW2_GL_INFLIGHT", "4", 1);
    setenv("EW2_WIDTH", "1280", 1);
    setenv("EW2_HEIGHT", "720", 1);
    setenv("EW2_TARGET_FPS", "120", 1);
    setenv("EW2_GL_RENDERER", "Adreno (TM) 420", 1);

    FILE *cmdf = fopen("/proc/cmdline", "r");
    if (cmdf) {
        char cmdline[1024] = {0};
        if (fgets(cmdline, sizeof(cmdline), cmdf)) {
            char *p = strtok(cmdline, " \t\r\n");
            while (p) {
                if (strncmp(p, "ew2_w=", 6) == 0 && p[6])
                    setenv("EW2_WIDTH", p + 6, 1);
                else if (strncmp(p, "ew2_h=", 6) == 0 && p[6])
                    setenv("EW2_HEIGHT", p + 6, 1);
                else if (strncmp(p, "ew2_fps=", 8) == 0 && p[8])
                    setenv("EW2_TARGET_FPS", p + 8, 1);
                else if (strncmp(p, "ew2_trace=0", 11) == 0) {
                    setenv("LOADER_TRACE", "0", 1);
                    setenv("EW2_GL_PROFILE", "0", 1);
                    setenv("EW2_PC_PROF", "0", 1);
                    setenv("EW2_SYSCALL_PROF", "0", 1);
                }
                else if (strncmp(p, "ew2_gpu=", 8) == 0 && p[8]) {
                    char gpu[128];
                    snprintf(gpu, sizeof(gpu), "%s", p + 8);
                    for (char *c = gpu; *c; c++)
                        if (*c == '_') *c = ' ';
                    setenv("EW2_GL_RENDERER", gpu, 1);
                }
                p = strtok(NULL, " \t\r\n");
            }
        }
        fclose(cmdf);
    }

    struct stat st;
    int sr = stat("/ew2data/base.apk", &st);
    printf("[runner] stat(/ew2data/base.apk) = %d (errno=%d: %s, size=%lld)\n",
           sr, errno, strerror(errno), (long long)st.st_size);
    fflush(stdout);

    int init_log = open("/tmp/ew2.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (init_log >= 0) close(init_log);

    pid_t pid = fork();
    if (pid == 0) {
        /* Always redirect ew2 stdout/stderr to tmpfs (/tmp/ew2.log) instead of emulated
         * PL011 UART ttyAMA0 (where every character traps into QEMU MMIO). save_watch_thread
         * streams /tmp/ew2.log over virtio-serial vport0p3 (cmd=12) to host logs/play.log. */
        int log_fd = open("/tmp/ew2.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (log_fd >= 0) {
            dup2(log_fd, 1);
            dup2(log_fd, 2);
            if (log_fd > 2) close(log_fd);
        }
        char *argv[] = { (char *)"/app/ew2", (char *)"/ew2data", NULL };
        execv("/app/ew2", argv);
        perror("execv /app/ew2");
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    flush_guest_log();
    check_and_push_saves();
    usleep(100000);
    printf("ew2 exited: status=0x%x (code=%d, sig=%d)\n",
           status, WIFEXITED(status) ? WEXITSTATUS(status) : -1,
           WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    fflush(stdout);
    reboot(RB_POWER_OFF);
    return 0;
}
