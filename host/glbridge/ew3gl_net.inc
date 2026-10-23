/*
 * ew3gl module 2/12: the transport under the message stream.
 *
 *  - fifos (EW3_GL_CMD / EW3_GL_REPLY, Linux only): host and guest on one machine.
 *    The guest blocks in open() until both ends exist, so the host creates the
 *    fifos when missing and opens them without blocking: the command fifo
 *    read-only/non-blocking (EOF after data has flowed = the guest is gone), the
 *    reply fifo read-write so open() does not wait for the guest's reader.
 *  - TCP (EW3_GL_CONNECT=host:port, Linux and Windows): one socket carries both
 *    directions. The guest listens (EW3_GL_LISTEN on its side) and the host
 *    connects, so ew3gl.exe on Windows reaches a guest in WSL through WSL's
 *    localhost forwarding, with nothing for the firewall to block.
 *
 * Interface: transport_open(), net_wait(ms), net_read(), net_write().
 */

/* Window events keep flowing while the guest is busy (module 4). */
static void window_pump(void);

#ifdef _WIN32
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define sock_close closesocket
static int sock_again(void) { int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINTR; }
#else
typedef int sock_t;
#define BAD_SOCK (-1)
#define sock_close close
static int sock_again(void) { return errno == EAGAIN || errno == EINTR; }
static int g_cmd_fd = -1, g_rep_fd = -1;
#endif

static sock_t g_sock = BAD_SOCK;
static int g_seen_data;   /* before the guest connects, "no writer" is not an EOF */

/* host:port -> a connected socket, or BAD_SOCK (the guest is not listening yet). */
static sock_t tcp_connect(const char *spec)
{
    char host[256];
    const char *colon = strrchr(spec, ':');
    if (!colon || colon == spec || (size_t)(colon - spec) >= sizeof(host))
        gfatal("EW3_GL_CONNECT must be host:port, not '%s'", spec);
    memcpy(host, spec, (size_t)(colon - spec));
    host[colon - spec] = 0;
    struct addrinfo hints, *ai = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, colon + 1, &hints, &ai) != 0 || !ai)
        gfatal("cannot resolve %s", spec);
    sock_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s != BAD_SOCK && connect(s, ai->ai_addr, (int)ai->ai_addrlen) != 0) {
        sock_close(s);
        s = BAD_SOCK;
    }
    freeaddrinfo(ai);
    if (s == BAD_SOCK)
        return BAD_SOCK;
    /* Queries are small and synchronous: no Nagle delay. Big buffers for texture uploads. */
    int one = 1, big = 4 << 20;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&big, sizeof(big));
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
#endif
    return s;
}

#ifndef _WIN32
static int make_fifo(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0)
        return S_ISFIFO(st.st_mode) ? 0 : -1;
    return mkfifo(path, 0600);
}

static void fifos_open(void)
{
    if (!g_cfg.cmd_path || !g_cfg.rep_path)
        gfatal("set EW3_GL_CONNECT=host:port, or EW3_GL_CMD and EW3_GL_REPLY to the fifo paths");
    if (make_fifo(g_cfg.cmd_path) || make_fifo(g_cfg.rep_path))
        gfatal("cannot create fifos %s, %s: %s", g_cfg.cmd_path, g_cfg.rep_path, strerror(errno));
    g_cmd_fd = open(g_cfg.cmd_path, O_RDONLY | O_NONBLOCK);
    g_rep_fd = open(g_cfg.rep_path, O_RDWR);
    if (g_cmd_fd < 0 || g_rep_fd < 0)
        gfatal("cannot open fifos %s, %s: %s", g_cfg.cmd_path, g_cfg.rep_path, strerror(errno));
    /* A bigger pipe lets the guest write ahead while a frame is being drawn (the guest asks too). */
    fcntl(g_cmd_fd, 1031 /* F_SETPIPE_SZ */, 1 << 20);
    glog("fifos open: commands %s, replies %s", g_cfg.cmd_path, g_cfg.rep_path);
}
#endif

static void transport_open(void)
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        gfatal("WSAStartup failed");
#else
    signal(SIGPIPE, SIG_IGN);
    if (!g_cfg.connect) {
        fifos_open();
        return;
    }
#endif
    if (!g_cfg.connect)
        gfatal("set EW3_GL_CONNECT=host:port (the guest's EW3_GL_LISTEN)");
    glog("connecting to the game at %s", g_cfg.connect);
    uint64_t t0 = now_ns();
    while (g_alive && (g_sock = tcp_connect(g_cfg.connect)) == BAD_SOCK) {
        /* The guest listens only once it has started; keep the window alive meanwhile. */
        window_pump();
        SDL_Delay(200);
        if (now_ns() - t0 > 10000000000ull) {
            glog("still waiting for the game at %s", g_cfg.connect);
            t0 = now_ns();
        }
    }
    if (g_sock != BAD_SOCK)
        glog("connected to the game at %s", g_cfg.connect);
}

/* 1 when the transport has data (or EOF) within ms milliseconds. */
static int net_wait(int ms)
{
#ifndef _WIN32
    if (g_sock == BAD_SOCK) {
        struct pollfd p = { g_cmd_fd, POLLIN, 0 };
        return poll(&p, 1, ms) > 0;
    }
#endif
    if (g_sock == BAD_SOCK)
        return 0;
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(g_sock, &rd);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    return select((int)g_sock + 1, &rd, NULL, NULL, &tv) > 0;
}

/* Bytes read; 0 = nothing right now; -1 = the guest closed its end. */
static long net_read(void *p, size_t n)
{
    long r;
    if (g_sock != BAD_SOCK) {
        r = (long)recv(g_sock, (char *)p, (int)(n > (1u << 30) ? (1u << 30) : n), 0);
        if (r < 0)
            return sock_again() ? 0 : -1;
        if (r == 0)
            return -1;   /* a TCP peer that closes has gone, connected or not */
    } else {
#ifdef _WIN32
        return -1;
#else
        r = (long)read(g_cmd_fd, p, n);
        if (r < 0)
            return errno == EAGAIN || errno == EINTR ? 0 : -1;
        if (r == 0)
            return g_seen_data ? -1 : 0;
#endif
    }
    g_seen_data = 1;
    return r;
}

static int net_write(const void *p, size_t n)
{
    const char *s = (const char *)p;
    while (n > 0) {
        long w;
        if (g_sock != BAD_SOCK) {
            w = (long)send(g_sock, s, (int)(n > (1u << 30) ? (1u << 30) : n), 0);
            if (w < 0 && sock_again()) {
                /* Non-blocking socket with a full send buffer: wait until it drains. */
                fd_set wr;
                FD_ZERO(&wr);
                FD_SET(g_sock, &wr);
                struct timeval tv = { 0, 50000 };
                select((int)g_sock + 1, NULL, &wr, NULL, &tv);
                continue;
            }
        } else {
#ifdef _WIN32
            return -1;
#else
            w = (long)write(g_rep_fd, s, n);
            if (w < 0 && errno == EINTR)
                continue;
#endif
        }
        if (w <= 0)
            return -1;
        s += w;
        n -= (size_t)w;
    }
    return 0;
}
