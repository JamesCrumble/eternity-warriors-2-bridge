/*
 * com.unity3d.player.WWW - the Java half of UnityEngine.WWW.
 *
 * The engine constructs one per request and start()s it; the Java thread then
 * reports back through static natives (readCallback / doneCallback /
 * errorCallback) keyed by the native request pointer it was given.
 *
 * Local URLs are served for real - "jar:file://<apk>!/<entry>" out of the zip
 * and "file://<path>" from disk - because games load their StreamingAssets that
 * way. http(s):// goes to the local stand-in server (EW3_SERVER, port 18080),
 * with the original host in the Host header so the server sees which of Glu's
 * hosts was meant. Without EW3_SERVER it fails at once with a network
 * error: the real servers are gone, and an instant error is better than a
 * request the game waits on forever.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <string>
#include <vector>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <zip.h>

#include "port_env.h"

#include "dyn.h"
#include "trace.h"

namespace {

JavaVM *g_vm = NULL;
std::string g_obb_path;

struct Request {
    jint ptr;
    std::string url;
    std::vector<char> post;
};

/*
 * One HTTP/1.0 exchange with the stand-in server. Returns the status code
 * (0 when the server cannot be reached); headers in *headers as Unity's Java
 * side hands them over ("HTTP/1.0 200 OK\r\nName: value\r\n..."), body in *body.
 */
int fetch_http(const Request &r, std::string *headers, std::vector<char> *body)
{
    const char *srv = port_getenv("SERVER");
    if (!srv || !*srv)
        return 0;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return 0;
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(18080);
    a.sin_addr.s_addr = inet_addr(srv);
    if (connect(fd, (sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return 0;
    }
    std::string rest = r.url.substr(r.url.find("//") == std::string::npos ? 0 : r.url.find("//") + 2);
    std::string host = rest.substr(0, rest.find('/'));
    std::string path = rest.find('/') == std::string::npos ? "/" : rest.substr(rest.find('/'));
    std::string req = std::string(r.post.empty() ? "GET " : "POST ") + path + " HTTP/1.0\r\nHost: " + host +
                      "\r\nContent-Length: " + std::to_string(r.post.size()) + "\r\nConnection: close\r\n\r\n";
    req.append(r.post.begin(), r.post.end());
    for (size_t off = 0; off < req.size();) {
        ssize_t n = send(fd, req.data() + off, req.size() - off, 0);
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    std::vector<char> resp;
    char buf[65536];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0)
        resp.insert(resp.end(), buf, buf + n);
    close(fd);
    std::string text(resp.begin(), resp.end());
    size_t end = text.find("\r\n\r\n");
    if (end == std::string::npos)
        return 0;
    *headers = text.substr(0, end + 2);
    body->assign(resp.begin() + (long)end + 4, resp.end());
    int code = 0;
    sscanf(text.c_str(), "HTTP/%*s %d", &code);
    return code;
}

bool read_file(const std::string &path, std::vector<char> *out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    if (fseek(f, 0, SEEK_END) == 0) {
        long sz = ftell(f);
        if (sz > 0)
            out->reserve((size_t)sz);
        fseek(f, 0, SEEK_SET);
    }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        out->insert(out->end(), buf, buf + n);
    fclose(f);
    return true;
}

std::mutex g_zip_lock;
std::map<std::string, zip_t *> g_zip_cache;

bool read_zip_entry(const std::string &zip_path, const std::string &entry, std::vector<char> *out)
{
    std::lock_guard<std::mutex> lock(g_zip_lock);
    zip_t *z = NULL;
    auto it = g_zip_cache.find(zip_path);
    if (it != g_zip_cache.end()) {
        z = it->second;
    } else {
        int err = 0;
        z = zip_open(zip_path.c_str(), ZIP_RDONLY, &err);
        if (!z)
            return false;
        g_zip_cache[zip_path] = z;
    }
    zip_stat_t st;
    if (zip_stat(z, entry.c_str(), 0, &st) == 0 && (st.valid & ZIP_STAT_SIZE))
        out->reserve((size_t)st.size);
    zip_file_t *zf = zip_fopen(z, entry.c_str(), 0);
    if (!zf)
        return false;
    char buf[65536];
    zip_int64_t n;
    while ((n = zip_fread(zf, buf, sizeof(buf))) > 0)
        out->insert(out->end(), buf, buf + n);
    zip_fclose(zf);
    return true;
}

bool fetch_local(const std::string &url, std::vector<char> *out, bool *is_local)
{
    *is_local = true;
    if (url.compare(0, 9, "jar:file:") == 0) {
        std::string rest = url.substr(9);
        while (rest.compare(0, 2, "//") == 0)
            rest.erase(0, 1);
        size_t bang = rest.find("!/");
        if (bang == std::string::npos)
            return false;
        std::string zip_path = rest.substr(0, bang);
        std::string entry = rest.substr(bang + 2);
        if (read_zip_entry(zip_path, entry, out))
            return true;
        if (!g_obb_path.empty() && g_obb_path != zip_path)
            return read_zip_entry(g_obb_path, entry, out);
        return false;
    }
    if (url.compare(0, 5, "file:") == 0) {
        std::string rest = url.substr(5);
        while (rest.compare(0, 2, "//") == 0)
            rest.erase(0, 1);
        return read_file(rest, out);
    }
    if (!url.empty() && url[0] == '/')
        return read_file(url, out);
    /* Built-in offline responses for EW2 version check & store data */
    if (url.size() >= 8 && url.compare(url.size() - 8, 8, "/version") == 0) {
        const char *ver = "Version\nskip\nskip\nfalse\nAAAAAAAAAA\n0\n";
        out->assign(ver, ver + strlen(ver));
        return true;
    }
    if (url.find("056a68f") != std::string::npos && !g_obb_path.empty()) {
        size_t slash = url.find_last_of('/');
        std::string fname = (slash == std::string::npos) ? url : url.substr(slash + 1);
        if (read_zip_entry(g_obb_path, "assets/" + fname, out))
            return true;
    }
    *is_local = false;
    return false;
}

/*
 * The callback sequence of Unity 4.5's Java WWW.run(), which the engine relies
 * on (WWW.LoadFromCacheOrDownload never completes without it): every header as
 * "name: value\n\r" (the HTTP status line under the key "null", as
 * HttpURLConnection reports it), content-length and content-type explicitly,
 * then the body in chunks, each followed by progressCallback, then done.
 * progressCallback takes floats; the engine is armeabi-v7a (softfp), hence pcs("aapcs").
 */
typedef void (*ProgressFn)(JNIEnv *, jclass, jint, jfloat, jfloat, jdouble, jint) __attribute__((pcs("aapcs")));

void *request_main(void *arg)
{
    Request *r = (Request *)arg;
    JNIEnv *env = NULL;
    g_vm->AttachCurrentThread(&env, NULL);

    const char *W = "com/unity3d/player/WWW";
    auto done  = (void (*)(JNIEnv *, jclass, jint))dyn_native(W, "doneCallback");
    auto error = (void (*)(JNIEnv *, jclass, jint, jstring))dyn_native(W, "errorCallback");
    auto read  = (jboolean (*)(JNIEnv *, jclass, jint, jbyteArray, jint))dyn_native(W, "readCallback");
    auto header = (jboolean (*)(JNIEnv *, jclass, jint, jstring))dyn_native(W, "headerCallback");
    auto progress = (ProgressFn)dyn_native(W, "progressCallback");
    jclass cls = (jclass)dyn_class(W);

    std::vector<char> data;
    bool local = false;
    bool ok = fetch_local(r->url, &data, &local);
    std::string err = local ? "File not found" : "java.net.UnknownHostException: no network";
    std::string hdr;
    if (!local) {
        std::string raw;
        int code = fetch_http(*r, &raw, &data);
        ok = code >= 200 && code < 400;
        if (code >= 400)
            err = std::to_string(code) + " " + std::string(data.begin(), data.end());
        /* "HTTP/1.0 200 OK\r\nName: v\r\n..." -> Java's layout */
        size_t pos = 0;
        bool first = true;
        while (pos < raw.size()) {
            size_t eol = raw.find("\r\n", pos);
            std::string line = raw.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
            pos = eol == std::string::npos ? raw.size() : eol + 2;
            if (line.empty())
                continue;
            if (first) {
                hdr += "null: " + line + "\n\r";
                first = false;
            } else if (strncasecmp(line.c_str(), "content-length:", 15) && strncasecmp(line.c_str(), "content-type:", 13)) {
                hdr += line + "\n\r";
            }
        }
        trace("WWW %s -> HTTP %d (%zu bytes)", r->url.c_str(), code, data.size());
    } else {
        trace("WWW %s -> %s (%zu bytes)", r->url.c_str(), ok ? "ok" : "not found", data.size());
    }

    if (ok && read && done) {
        hdr += "content-length: " + std::to_string(data.size()) + "\n\r";
        hdr += std::string("content-type: ") + (local ? "content/unknown" : "application/octet-stream") + "\n\r";
        if (header)
            header(env, cls, r->ptr, env->NewStringUTF(hdr.c_str()));
        const size_t chunk = 256 * 1024;
        static const long kbps = port_getenv_long("WWW_KBPS", 0);   /* 0: memory speed */
        const jint total = (jint)data.size();
        size_t first_cap = data.size() < chunk ? data.size() : chunk;
        jbyteArray arr = first_cap > 0 ? env->NewByteArray((jsize)first_cap) : NULL;
        for (size_t off = 0; off < data.size(); off += chunk) {
            size_t n = data.size() - off < chunk ? data.size() - off : chunk;
            env->SetByteArrayRegion(arr, 0, (jsize)n, (const jbyte *)data.data() + off);
            /* The return value is not "stop": Unity 4.5 answers false for every chunk
             * it accepts (every response, the JSON ones included, parsed fine). Breaking
             * on it delivered only the first chunk - a bundle's cached copy came out
             * truncated and was thrown away. */
            read(env, cls, r->ptr, arr, (jint)n);
            if (progress)
                progress(env, cls, r->ptr, 1.0f, total ? (float)(off + n) / (float)total : 1.0f, 0.0, total);
            if (kbps > 0)
                usleep((useconds_t)(n * 1000 / kbps));
        }
        if (arr)
            env->DeleteLocalRef(arr);
        /* EW3_WWW_KBPS > 0 simulates a slow network (debug aid). */
        done(env, cls, r->ptr);
    } else if (error) {
        error(env, cls, r->ptr, env->NewStringUTF(err.c_str()));
    } else if (done) {
        done(env, cls, r->ptr);
    }
    delete r;
    return NULL;
}

/*
 * Requests run on a few long-lived workers, never on a thread per request: the
 * callbacks into libunity register the calling thread with Mono's collector,
 * and a thread that then exits leaves Boehm GC scanning a stack that is gone
 * (SIGSEGV in GC_push_all_stack). Several workers so one slow download does
 * not hold up the small JSON requests behind it.
 */
std::mutex g_q_lock;
std::condition_variable g_q_cv;
std::condition_variable g_done_cv;
std::deque<Request *> g_queue;
int g_active = 0;

void *worker_main(void *)
{
    for (;;) {
        Request *r;
        {
            std::unique_lock<std::mutex> l(g_q_lock);
            g_q_cv.wait(l, [] { return !g_queue.empty(); });
            r = g_queue.front();
            g_queue.pop_front();
            g_active++;
        }
        request_main(r);
        {
            std::lock_guard<std::mutex> l(g_q_lock);
            if (--g_active == 0 && g_queue.empty())
                g_done_cv.notify_all();
        }
    }
    return NULL;
}

void start_request(DynObject *o)
{
    Request *r = (Request *)o->priv;
    o->priv = NULL;
    if (!r)
        return;
    static std::once_flag once;
    std::call_once(once, [] {
        for (int i = 0; i < 4; i++) {
            pthread_t t;
            pthread_create(&t, NULL, worker_main, NULL);
        }
    });
    std::lock_guard<std::mutex> l(g_q_lock);
    g_queue.push_back(r);
    g_q_cv.notify_one();
}

} // namespace

extern "C" void unity_www_wait_all()
{
    std::unique_lock<std::mutex> l(g_q_lock);
    g_done_cv.wait_for(l, std::chrono::milliseconds(2000), [] { return g_queue.empty() && g_active == 0; });
}

void unity_www_register(JNIEnv *env, const std::string &obb_path)
{
    env->GetJavaVM(&g_vm);
    g_obb_path = obb_path;
    const char *W = "com/unity3d/player/WWW";
    dyn_method(W, "<init>(ILjava/lang/String;[BLjava/util/Map;)V", [](JNIEnv *e, jobject self, const jvalue *a) {
        DynObject *o = (DynObject *)self;
        Request *r = new Request{a[0].i, dyn_str(a[1].l), {}};
        if (a[2].l) {
            jsize n = e->GetArrayLength((jarray)a[2].l);
            r->post.resize((size_t)n);
            if (n)
                e->GetByteArrayRegion((jbyteArray)a[2].l, 0, n, (jbyte *)r->post.data());
        }
        o->priv = r;
        /* Unity 4.5's Java WWW starts its thread from the constructor; 5.x calls start(). */
        start_request(o);
        return JV();
    });
    dyn_method(W, "start()V", [](JNIEnv *, jobject self, const jvalue *) {
        start_request((DynObject *)self);
        return JV();
    });
    dyn_method(W, "join()V", [](JNIEnv *, jobject, const jvalue *) { return JV(); });
    dyn_method(W, "isAlive()Z", [](JNIEnv *, jobject, const jvalue *) { return JZ(false); });
}
