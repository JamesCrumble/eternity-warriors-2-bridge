/*
 * ew3gl module 11/12: per-op statistics, logged every EW3_GL_REPORT seconds:
 * frame rate, stream volume, how busy the host was, the busiest ops by count
 * and by bytes, and every synchronous query (each one stalls the pipeline).
 */
#define N_OPS 256

static long g_op_count[N_OPS];
static uint64_t g_op_bytes[N_OPS];
static uint64_t g_op_ns[N_OPS];
static long g_cmds;
static uint64_t g_bytes, g_busy_ns, g_report_t0;
static long g_report_swaps;

static const char *op_name(int op)
{
    if (op == GB_OP_Q_PROGRAM_INFO)
        return "Q_PROGRAM_INFO";
    if (op > 0 && (size_t)op < sizeof(gb_op_names) / sizeof(gb_op_names[0]))
        return gb_op_names[op];
    return "?";
}

static void stats_note(int op, uint32_t len, uint64_t busy_ns)
{
    g_cmds++;
    g_bytes += len;
    g_busy_ns += busy_ns;
    if (op >= 0 && op < N_OPS) {
        g_op_count[op]++;
        g_op_bytes[op] += len;
        g_op_ns[op] += busy_ns;
    }
}

/* The `top` busiest ops by count (mode=0), bytes (mode=1), or time ns (mode=2), per frame, appended to buf. */
static void stats_top(char *buf, size_t cap, const char *title, int mode, double frames, int top)
{
    size_t n = strlen(buf);
    n += (size_t)snprintf(buf + n, n < cap ? cap - n : 0, " | %s:", title);
    unsigned char used[N_OPS] = { 0 };
    for (int k = 0; k < top; k++) {
        int best = -1;
        for (int i = 0; i < N_OPS; i++) {
            uint64_t v = mode == 2 ? g_op_ns[i] : (mode == 1 ? g_op_bytes[i] : (uint64_t)g_op_count[i]);
            uint64_t b = best < 0 ? 0 : (mode == 2 ? g_op_ns[best] : (mode == 1 ? g_op_bytes[best] : (uint64_t)g_op_count[best]));
            if (!used[i] && v > 0 && (best < 0 || v > b))
                best = i;
        }
        if (best < 0)
            break;
        used[best] = 1;
        if (mode == 2) {
            double ms = (double)g_op_ns[best] / 1e6 / frames;
            n += (size_t)snprintf(buf + n, n < cap ? cap - n : 0, " %s=%.2f", op_name(best), ms);
        } else {
            double v = (double)(mode == 1 ? g_op_bytes[best] : (uint64_t)g_op_count[best]) / frames;
            n += (size_t)snprintf(buf + n, n < cap ? cap - n : 0, " %s=%.0f", op_name(best), v);
        }
    }
}

static void stats_maybe_report(void)
{
    uint64_t now = now_ns();
    if (!g_report_t0) {
        g_report_t0 = now;
        return;
    }
    if (g_cfg.report_s <= 0 || now - g_report_t0 < (uint64_t)g_cfg.report_s * 1000000000ull)
        return;
    double secs = (double)(now - g_report_t0) / 1e9;
    long frames_n = g_swaps - g_report_swaps;
    double frames = frames_n > 0 ? (double)frames_n : 1.0;

    char line[2048];
    snprintf(line, sizeof(line), "%.1f fps, %ld cmds, %.1f MB/s, frames %ld, busy %.0f%%",
             (double)frames_n / secs, g_cmds, (double)g_bytes / 1e6 / secs, g_swaps,
             100.0 * (double)g_busy_ns / (double)(now - g_report_t0));
    /* Queries are listed whatever their count: each drains the whole pipeline. */
    size_t n = strlen(line);
    n += (size_t)snprintf(line + n, sizeof(line) - n, " | sync/frame:");
    for (int op = GB_OP_Q_GET_STRING; op <= GB_OP_Q_READ_PIXELS; op++)
        if (g_op_count[op] && n < sizeof(line))
            n += (size_t)snprintf(line + n, sizeof(line) - n, " %s=%.2f", op_name(op), g_op_count[op] / frames);
    if (g_op_count[GB_OP_Q_PROGRAM_INFO] && n < sizeof(line))
        snprintf(line + n, sizeof(line) - n, " Q_PROGRAM_INFO=%.2f", g_op_count[GB_OP_Q_PROGRAM_INFO] / frames);
    stats_top(line, sizeof(line), "ms/frame", 2, frames, 6);
    stats_top(line, sizeof(line), "per frame", 0, frames, 8);
    stats_top(line, sizeof(line), "bytes/frame", 1, frames, 5);
    glog("%s", line);

    memset(g_op_count, 0, sizeof(g_op_count));
    memset(g_op_bytes, 0, sizeof(g_op_bytes));
    memset(g_op_ns, 0, sizeof(g_op_ns));
    g_cmds = 0;
    g_bytes = g_busy_ns = 0;
    g_report_t0 = now;
    g_report_swaps = g_swaps;
}
