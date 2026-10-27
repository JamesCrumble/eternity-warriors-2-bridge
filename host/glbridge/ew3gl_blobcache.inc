/*
 * Persistent shader cache (EGL_ANDROID_blob_cache).
 *
 * ANGLE compiles every GLSL ES shader to HLSL and then to D3D bytecode with the D3D compiler, 30-160 ms per
 * program and synchronously on the first draw that needs it: each new program is a visible hitch. With the blob
 * cache hooks set, ANGLE stores the result of that work by key and finds it on the next start. One file per
 * blob in EW2_SHADER_CACHE (off when unset or when the driver has no such extension, e.g. Mesa).
 */
typedef void (*blob_set_fn)(const void *key, intptr_t keysize, const void *value, intptr_t valuesize);
typedef intptr_t (*blob_get_fn)(const void *key, intptr_t keysize, void *value, intptr_t valuesize);
typedef unsigned (*blob_install_fn)(void *dpy, blob_set_fn set, blob_get_fn get);

static char g_blob_dir[900];
static double g_blob_ms;   /* time spent in the hooks; a slow one is logged */
static unsigned long g_blob_hits, g_blob_stores;

static void blob_path(const void *key, intptr_t keysize, char *out, size_t outsz)
{
    size_t n = (size_t)snprintf(out, outsz, "%s/", g_blob_dir);
    const unsigned char *k = (const unsigned char *)key;
    for (intptr_t i = 0; i < keysize && i < 64 && n + 3 < outsz; i++)
        n += (size_t)snprintf(out + n, outsz - n, "%02x", k[i]);
}

static void blob_set(const void *key, intptr_t keysize, const void *value, intptr_t valuesize)
{
    uint64_t t0 = now_ns();
    char path[1100], tmp[1120];
    blob_path(key, keysize, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return;
    int ok = fwrite(value, 1, (size_t)valuesize, f) == (size_t)valuesize;
    ok = fclose(f) == 0 && ok;
    if (ok && rename(tmp, path) == 0)
        g_blob_stores++;
    else
        remove(tmp);
    double ms = (double)(now_ns() - t0) / 1e6;
    g_blob_ms += ms;
    if (ms > 100.0)
        glog("shader cache: store of %ld bytes took %.0f ms", (long)valuesize, ms);
}

static intptr_t blob_get(const void *key, intptr_t keysize, void *value, intptr_t valuesize)
{
    uint64_t t0 = now_ns();
    char path[1100];
    blob_path(key, keysize, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0 && value && valuesize >= size && fread(value, 1, (size_t)size, f) == (size_t)size)
        g_blob_hits++;
    fclose(f);
    double ms = (double)(now_ns() - t0) / 1e6;
    g_blob_ms += ms;
    if (ms > 100.0)
        glog("shader cache: load of %ld bytes took %.0f ms", size, ms);
    return size > 0 ? size : 0;
}

static void blobcache_init(void)
{
    const char *dir = cfg_str("SHADER_CACHE", NULL);
    if (!dir || !*dir)
        return;
    blob_install_fn install = (blob_install_fn)SDL_GL_GetProcAddress("eglSetBlobCacheFuncsANDROID");
    void *(*cur_dpy)(void) = (void *(*)(void))SDL_GL_GetProcAddress("eglGetCurrentDisplay");
    if (!install || !cur_dpy) {
        glog("shader cache: EGL_ANDROID_blob_cache is not available here");
        return;
    }
    snprintf(g_blob_dir, sizeof(g_blob_dir), "%s", dir);
    install(cur_dpy(), blob_set, blob_get);
    glog("shader cache: %s", g_blob_dir);
}
