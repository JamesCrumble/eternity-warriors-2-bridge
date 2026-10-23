/*
 * Sockets: either no network at all, or a network that leads only to our server.
 *
 * Without EW3_SERVER the port behaves like a phone in airplane mode: socket()
 * fails and every caller takes its offline path (the inherited behaviour).
 *
 * With EW3_SERVER=<ipv4> every outgoing connection goes to that address
 * instead of the one the game asked for, with the port translated through a
 * small table - Glu's servers are long gone and tools/ew3server stands in for
 * them. Ports the table does not name (Photon's 5055/5058, analytics, ads) are
 * refused, which the game handles as "no connection". Host names resolve to the
 * server's address.
 *
 * Socket calls go to the host unchanged: struct sockaddr, msghdr, pollfd,
 * fd_set, the option numbers and errno are the Linux kernel's on both sides.
 * struct addrinfo is not - bionic puts ai_canonname before ai_addr, glibc after
 * - so getaddrinfo is answered here.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "port_env.h"
#include "so_util.h"
#include "thunk_gen.h"
#include "trace.h"

extern "C" {

/* Guest port -> server port. Edge TCP (gluon-prdedge-*:4024), the edge load
 * balancer (lb.*.gluon.glu.com:8015, normally reached through WWW) and plain HTTP. */
static const struct { int from, to; } kPortMap[] = {
    {4024, 18024},
    {8015, 18080},
    {80,   18080},
    /* 443 is not mapped: the server speaks plain HTTP, and a refused connection is
     * an answer TLS clients handle; a non-TLS reply is not. */
};

static in_addr_t server_addr(void)
{
    static in_addr_t a = 0;
    static bool done = false;
    if (!done) {
        done = true;
        const char *s = port_getenv("SERVER");
        if (s && *s) {
            a = inet_addr(s);
            trace("network: everything goes to the local server %s", s);
        } else {
            trace("network: no EW3_SERVER - sockets refused, the game runs offline");
        }
    }
    return a;
}

ABI_ATTR int bionic_socket(int domain, int type, int protocol)
{
    if (!server_addr() || (domain != AF_INET && domain != AF_INET6)) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    /* Everything is IPv4 here: the server address is. */
    return socket(AF_INET, type, protocol);
}

ABI_ATTR int bionic_connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    in_addr_t srv = server_addr();
    if (!srv || !addr || (addr->sa_family != AF_INET && addr->sa_family != AF_INET6)) {
        errno = ECONNREFUSED;
        return -1;
    }
    int port = ntohs(addr->sa_family == AF_INET ? ((const sockaddr_in *)addr)->sin_port
                                                : ((const sockaddr_in6 *)addr)->sin6_port);
    for (auto &m : kPortMap) {
        if (m.from != port)
            continue;
        sockaddr_in to;
        memset(&to, 0, sizeof(to));
        to.sin_family = AF_INET;
        to.sin_port = htons((uint16_t)m.to);
        to.sin_addr.s_addr = srv;
        trace("network: connect :%d -> server :%d", port, m.to);
        return connect(fd, (const sockaddr *)&to, sizeof(to));
    }
    trace("network: connect to port %d refused (not served)", port);
    errno = ECONNREFUSED;
    return -1;
}

/* bionic's struct addrinfo. */
struct bionic_addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    char *ai_canonname;
    struct sockaddr *ai_addr;
    struct bionic_addrinfo *ai_next;
};

ABI_ATTR int bionic_getaddrinfo(const char *node, const char *service, const struct bionic_addrinfo *hints,
                                struct bionic_addrinfo **res)
{
    in_addr_t srv = server_addr();
    if (!srv)
        return EAI_AGAIN;
    if (hints && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET && hints->ai_family != AF_INET6)
        return EAI_FAMILY;
    struct Block { bionic_addrinfo ai; sockaddr_in sa; char name[256]; };
    Block *b = (Block *)calloc(1, sizeof(Block));
    if (!b)
        return EAI_MEMORY;
    b->sa.sin_family = AF_INET;
    b->sa.sin_port = htons(service ? (uint16_t)atoi(service) : 0);
    /* Numeric addresses keep their own value; names become the server. connect()
     * redirects either way, but a numeric loopback should look like itself. */
    in_addr_t numeric = node ? inet_addr(node) : INADDR_NONE;
    b->sa.sin_addr.s_addr = numeric != INADDR_NONE ? numeric : srv;
    b->ai.ai_family = AF_INET;
    b->ai.ai_socktype = hints && hints->ai_socktype ? hints->ai_socktype : SOCK_STREAM;
    b->ai.ai_protocol = hints ? hints->ai_protocol : 0;
    b->ai.ai_addrlen = sizeof(sockaddr_in);
    b->ai.ai_addr = (sockaddr *)&b->sa;
    if (node && hints && (hints->ai_flags & AI_CANONNAME)) {
        strncpy(b->name, node, sizeof(b->name) - 1);
        b->ai.ai_canonname = b->name;
    }
    *res = &b->ai;
    return 0;
}

ABI_ATTR void bionic_freeaddrinfo(struct bionic_addrinfo *ai)
{
    free(ai);   /* one block per answer, see above */
}

ABI_ATTR const char *bionic_gai_strerror(int code)
{
    return gai_strerror(code);
}

ABI_ATTR struct hostent *bionic_gethostbyname(const char *name)
{
    static in_addr_t addr;
    static char *list[2] = {(char *)&addr, NULL};
    static char *aliases[1] = {NULL};
    static char hname[256];
    static struct hostent he;
    if (!server_addr()) {
        h_errno = TRY_AGAIN;
        return NULL;
    }
    in_addr_t numeric = name ? inet_addr(name) : INADDR_NONE;
    addr = numeric != INADDR_NONE ? numeric : server_addr();
    strncpy(hname, name ? name : "", sizeof(hname) - 1);
    he.h_name = hname;
    he.h_aliases = aliases;
    he.h_addrtype = AF_INET;
    he.h_length = 4;
    he.h_addr_list = list;
    return &he;
}

ABI_ATTR struct hostent *bionic_gethostbyaddr(const void *, socklen_t, int)
{
    h_errno = HOST_NOT_FOUND;
    return NULL;
}

ABI_ATTR int *bionic_get_h_errno(void)
{
    return &h_errno;
}

} /* extern "C" */

DynLibFunction symtable_net[] = {
    THUNK_SPECIFIC("socket",        bionic_socket),
    THUNK_SPECIFIC("connect",       bionic_connect),
    THUNK_SPECIFIC("getaddrinfo",   bionic_getaddrinfo),
    THUNK_SPECIFIC("freeaddrinfo",  bionic_freeaddrinfo),
    THUNK_SPECIFIC("gai_strerror",  bionic_gai_strerror),
    THUNK_SPECIFIC("gethostbyname", bionic_gethostbyname),
    THUNK_SPECIFIC("gethostbyaddr", bionic_gethostbyaddr),
    THUNK_SPECIFIC("__get_h_errno", bionic_get_h_errno),
    { NULL, 0 },
};
