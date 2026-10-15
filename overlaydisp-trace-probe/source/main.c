#include <switch.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>

#define PORT 22225
#define REPORT "sdmc:/switch/OverlayDispTrace/report.txt"
#define SITE_A 0x315040ULL
#define SITE_B 0x31507cULL
#define PKT 8192
#define LOGSZ 60000

static char logbuf[LOGSZ];
static size_t loglen;
static char status_line[180] = "Ready. Press A to query Atmosphere GDB stub.";

static void logf1(const char *fmt, ...)
{
    if (loglen + 1 >= sizeof(logbuf)) return;
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(logbuf + loglen, sizeof(logbuf) - loglen, fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t add = (size_t)n;
        loglen = add >= sizeof(logbuf) - loglen ? sizeof(logbuf) - 1 : loglen + add;
    }
}
static void save_log(void)
{
    (void)mkdir("sdmc:/switch", 0777);
    (void)mkdir("sdmc:/switch/OverlayDispTrace", 0777);
    FILE *f = fopen(REPORT, "wb");
    if (f) { (void)fwrite(logbuf, 1, loglen, f); fclose(f); }
}
static void timeout_socket(int fd)
{
    struct timeval tv = {3, 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}
static bool get_local_ip(struct in_addr *out)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    struct sockaddr_in dst; memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET; dst.sin_port = htons(53);
    (void)inet_pton(AF_INET, "1.1.1.1", &dst.sin_addr);
    bool ok = false;
    if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) == 0) {
        struct sockaddr_in local; socklen_t n = sizeof(local);
        memset(&local, 0, sizeof(local));
        if (getsockname(fd, (struct sockaddr *)&local, &n) == 0 &&
            local.sin_addr.s_addr != htonl(INADDR_ANY)) {
            *out = local.sin_addr; ok = true;
        }
    }
    close(fd); return ok;
}
static int try_connect(const char *ip)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    timeout_socket(fd);
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons(PORT);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) {
        close(fd); return -1;
    }

    /* Bound connect() explicitly: SO_SNDTIMEO doesn't bound connect everywhere. */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd); return -1;
    }
    int rc = connect(fd, (struct sockaddr *)&a, sizeof(a));
    if (rc != 0 && errno != EINPROGRESS && errno != EWOULDBLOCK) {
        close(fd); return -1;
    }
    if (rc != 0) {
        fd_set wfds; FD_ZERO(&wfds); FD_SET(fd, &wfds);
        struct timeval tv = {2, 0};
        rc = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (rc <= 0) {
            close(fd); errno = ETIMEDOUT; return -1;
        }
        int err = 0; socklen_t errlen = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) != 0 || err != 0) {
            close(fd); errno = err ? err : ECONNREFUSED; return -1;
        }
    }
    (void)fcntl(fd, F_SETFL, flags);
    return fd;
}
static bool send_all(int fd, const char *p, size_t n)
{
    size_t k = 0;
    while (k < n) {
        ssize_t r = send(fd, p + k, n - k, 0);
        if (r <= 0) return false;
        k += (size_t)r;
    }
    return true;
}
static bool read_byte(int fd, char *c)
{
    for (;;) {
        ssize_t n = recv(fd, c, 1, 0);
        if (n == 1) return true;
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
}
static bool read_packet(int fd, char *out, size_t cap)
{
    char c;
    do { if (!read_byte(fd, &c)) return false; } while (c != '$');
    size_t n = 0;
    while (true) {
        if (!read_byte(fd, &c)) return false;
        if (c == '#') break;
        if (n + 1 < cap) out[n++] = c;
    }
    char h;
    if (!read_byte(fd, &h) || !read_byte(fd, &h)) return false;
    out[n] = 0;
    (void)send(fd, "+", 1, 0);
    return true;
}
static bool send_packet(int fd, const char *p)
{
    char frame[PKT + 32]; size_t n = strlen(p); unsigned sum = 0;
    if (n > PKT) return false;
    for (size_t i = 0; i < n; ++i) sum = (sum + (unsigned char)p[i]) & 255u;
    int m = snprintf(frame, sizeof(frame), "$%s#%02x", p, sum);
    return m > 0 && (size_t)m < sizeof(frame) && send_all(fd, frame, (size_t)m);
}
static bool command(int fd, const char *p, char *reply, size_t cap)
{
    return send_packet(fd, p) && read_packet(fd, reply, cap);
}
static void decode_hex(const char *src, char *dst, size_t cap)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && src[i + 1] && j + 1 < cap; i += 2) {
        char h[3] = {src[i], src[i+1], 0}; char *end = NULL;
        long v = strtol(h, &end, 16);
        if (!end || *end) break;
        dst[j++] = (char)v;
    }
    dst[j] = 0;
}
static bool is_hex_payload(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || (n & 1)) return false;
    for (size_t i = 0; i < n; ++i) {
        char ch = s[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
              (ch >= 'A' && ch <= 'F'))) return false;
    }
    return true;
}

static bool monitor_info_text(int fd, char *out, size_t out_cap)
{
    char q[64] = "qRcmd,";
    char r[PKT + 1];
    size_t used = 0;
    if (out_cap == 0) return false;
    out[0] = '\0';
    const char *s = "get info";
    size_t n = strlen(q);
    for (size_t i = 0; s[i] && n + 2 < sizeof(q); ++i) {
        (void)snprintf(q + n, sizeof(q) - n, "%02x", (unsigned char)s[i]);
        n += 2;
    }
    logf1("\n--- monitor get info ---\n");
    if (!send_packet(fd, q)) {
        logf1("Could not send monitor command.\n");
        return false;
    }
    for (unsigned i = 0; i < 128; ++i) {
        if (!read_packet(fd, r, sizeof(r))) {
            logf1("Monitor response timed out.\n");
            return false;
        }
        if (strcmp(r, "OK") == 0) break;

        const char *hex = NULL;
        if (r[0] == 'O' && is_hex_payload(r + 1)) hex = r + 1;
        else if (is_hex_payload(r)) hex = r;

        if (hex != NULL) {
            char decoded[PKT / 2 + 1];
            decode_hex(hex, decoded, sizeof(decoded));
            if (strstr(decoded, "Process:") != NULL && strstr(decoded, "Modules:") != NULL) {
                size_t add = strlen(decoded);
                if (add > out_cap - used - 1) add = out_cap - used - 1;
                if (add) {
                    memcpy(out + used, decoded, add);
                    used += add;
                    out[used] = '\0';
                }
                break;
            }
            if (r[0] == 'O') {
                size_t add = strlen(decoded);
                if (add > out_cap - used - 1) add = out_cap - used - 1;
                if (add) {
                    memcpy(out + used, decoded, add);
                    used += add;
                    out[used] = '\0';
                }
                continue;
            }
        }

        logf1("[monitor final response: %s]\n", r);
        break;
    }
    if (used != 0) {
        logf1("%s", out);
        if (out[used - 1] != '\n') logf1("\n");
    }
    return used != 0;
}

static bool parse_module_base(const char *info, uint64_t *base)
{
    const char *modules = strstr(info, "Modules:");
    if (!modules) return false;
    const char *p = strstr(modules, "0x");
    if (!p) return false;
    char *end = NULL;
    unsigned long long value = strtoull(p + 2, &end, 16);
    if (end == p + 2 || end == NULL || value == 0) return false;
    *base = (uint64_t)value;
    return true;
}

static bool read_pc_from_stop(const char *reply, uint64_t *pc)
{
    /* GDB AArch64 register 32 (hex ID 20) is PC; the value is little-endian. */
    const char *p = strstr(reply, "20:");
    if (!p) return false;
    p += 3;
    uint64_t value = 0;
    unsigned bytes = 0;
    while (bytes < 8 && p[0] && p[1] && p[0] != ';' && p[1] != ';') {
        char pair[3] = {p[0], p[1], 0};
        char *end = NULL;
        long b = strtol(pair, &end, 16);
        if (!end || *end || end == pair) break;
        value |= ((uint64_t)(uint8_t)b) << (bytes * 8);
        ++bytes;
        p += 2;
    }
    if (bytes != 8) return false;
    *pc = value;
    return true;
}

static bool parse_le_u64(const char *hex, uint64_t *value)
{
    if (strlen(hex) < 16) return false;
    uint64_t out = 0;
    for (unsigned i = 0; i < 8; ++i) {
        char pair[3] = {hex[i * 2], hex[i * 2 + 1], 0};
        char *end = NULL;
        long byte_value = strtol(pair, &end, 16);
        if (!end || *end || end == pair) return false;
        out |= ((uint64_t)(uint8_t)byte_value) << (i * 8);
    }
    *value = out;
    return true;
}

static bool read_pc_register(int fd, uint64_t *pc)
{
    char reply[PKT + 1];
    if (!command(fd, "p20", reply, sizeof(reply))) {
        logf1("RSP p20 (PC register) query timed out.\n");
        return false;
    }
    if (reply[0] == 'E' || !parse_le_u64(reply, pc)) {
        logf1("Could not decode PC register reply: %s\n", reply);
        return false;
    }
    return true;
}

static bool write_pc_register(int fd, uint64_t pc)
{
    char q[64], reply[PKT + 1];
    (void)snprintf(q, sizeof(q), "P20=%02x%02x%02x%02x%02x%02x%02x%02x",
                   (unsigned)(pc & 0xff), (unsigned)((pc >> 8) & 0xff),
                   (unsigned)((pc >> 16) & 0xff), (unsigned)((pc >> 24) & 0xff),
                   (unsigned)((pc >> 32) & 0xff), (unsigned)((pc >> 40) & 0xff),
                   (unsigned)((pc >> 48) & 0xff), (unsigned)((pc >> 56) & 0xff));
    if (!command(fd, q, reply, sizeof(reply))) {
        logf1("RSP PC register write timed out.\n");
        return false;
    }
    logf1("PC register write for 0x%" PRIx64 ": %s\n", pc, reply);
    return strcmp(reply, "OK") == 0;
}

static void set_socket_timeout_ms(int fd, long ms)
{
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/* Read-only memory inspection. RSP uses hexadecimal address and byte length. */
static bool rsp_read_memory(int fd, uint64_t address, size_t length, char *hex, size_t cap)
{
    char q[96];
    if (length == 0 || length > (PKT / 2) || cap < length * 2 + 1) {
        logf1("Memory read parameters invalid (length=%zu, cap=%zu).\n", length, cap);
        return false;
    }
    (void)snprintf(q, sizeof(q), "m%" PRIx64 ",%zx", address, length);
    if (!command(fd, q, hex, cap)) {
        logf1("Memory read timed out at 0x%" PRIx64 ".\n", address);
        return false;
    }
    if (strlen(hex) != length * 2 || !is_hex_payload(hex)) {
        logf1("Memory read at 0x%" PRIx64 " returned an unexpected payload (length=%zu): %.80s\n",
              address, strlen(hex), hex);
        return false;
    }
    return true;
}

static void dump_code_region(int fd, uint64_t address, size_t length, const char *label)
{
    char hex[0x202];
    if (length == 0 || length > 0x100 || sizeof(hex) < length * 2 + 1) {
        logf1("Skipping oversized code dump '%s' (length=0x%zx).\n", label, length);
        return;
    }
    logf1("\n--- %s ---\nAddress=0x%" PRIx64 ", length=0x%zx\n", label, address, length);
    if (rsp_read_memory(fd, address, length, hex, sizeof(hex))) {
        logf1("Bytes (AArch64 little-endian):\n%s\n", hex);
    } else {
        logf1("Read failed for '%s'.\n", label);
    }
}

static void dump_candidate_code(int fd, uint64_t module_base)
{
    /* Read-only, before breakpoints are installed. */
    dump_code_region(fd, module_base + SITE_A - 0x20, 0x80,
                     "candidate callsites A/B");
    /* Both BL encodings in the first dump resolve to module + 0x356710.
       Read the callee body so we can identify its behavior offline. */
    dump_code_region(fd, module_base + 0x356710, 0x100,
                     "shared call target module+0x356710");
}

static void dump_x1_pointed_memory(int fd)
{
    char reply[PKT + 1], hex[0x81];
    uint64_t address = 0;
    if (!command(fd, "p1", reply, sizeof(reply)) || !parse_le_u64(reply, &address)) {
        logf1("  x1 pointed data: unable to read pointer register.\n");
        return;
    }
    logf1("  x1 pointed data at 0x%" PRIx64 " (64 bytes):\n", address);
    if (!rsp_read_memory(fd, address, 64, hex, sizeof(hex))) {
        logf1("    <memory not readable>\n");
        return;
    }
    logf1("    hex=%s\n", hex);
    char ascii[65];
    for (unsigned i = 0; i < 64; ++i) {
        char pair[3] = {hex[i * 2], hex[i * 2 + 1], 0};
        char *end = NULL;
        long v = strtol(pair, &end, 16);
        if (!end || *end || end == pair) { ascii[i] = '?'; continue; }
        ascii[i] = (v >= 0x20 && v <= 0x7e) ? (char)v : '.';
    }
    ascii[64] = '\0';
    logf1("    ascii=%s\n", ascii);
}

static void dump_hit_registers(int fd, const char *site, uint64_t pc)
{
    char cmd[8], reply[PKT + 1];
    uint64_t value = 0;
    logf1("Hit context %s: PC=0x%" PRIx64 "\n", site, pc);
    for (unsigned i = 0; i < 8; ++i) {
        (void)snprintf(cmd, sizeof(cmd), "p%x", i);
        if (command(fd, cmd, reply, sizeof(reply)) && parse_le_u64(reply, &value)) {
            logf1("  x%u=0x%" PRIx64 "\n", i, value);
        } else {
            logf1("  x%u=<unavailable: %.40s>\n", i, reply);
        }
    }
    const struct { const char *name; const char *reg; } extra[] = {
        {"x29", "p1d"}, {"x30/LR", "p1e"}, {"SP", "p1f"}
    };
    for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]); ++i) {
        if (command(fd, extra[i].reg, reply, sizeof(reply)) &&
            parse_le_u64(reply, &value)) {
            logf1("  %s=0x%" PRIx64 "\n", extra[i].name, value);
        } else {
            logf1("  %s=<unavailable: %.40s>\n", extra[i].name, reply);
        }
    }
    dump_x1_pointed_memory(fd);
    save_log();
}

static bool rsp_breakpoint(int fd, bool add, uint64_t address)
{
    char q[80], reply[PKT + 1];
    (void)snprintf(q, sizeof(q), "%s,%" PRIx64 ",4", add ? "Z0" : "z0", address);
    if (!command(fd, q, reply, sizeof(reply))) {
        logf1("Breakpoint command timed out for 0x%" PRIx64 ".\n", address);
        return false;
    }
    logf1("%s breakpoint 0x%" PRIx64 ": %s\n", add ? "Set" : "Removed", address, reply);
    return strcmp(reply, "OK") == 0;
}

static bool rsp_detach(int fd, unsigned long pid)
{
    char q[64], reply[PKT + 1];
    (void)snprintf(q, sizeof(q), "D;%lx", pid);
    if (command(fd, q, reply, sizeof(reply))) {
        logf1("Debugger detach reply: %s\n", reply);
        return true;
    }
    logf1("Debugger detach timed out. If overlayDisp remains paused, reboot the Switch.\n");
    return false;
}

static bool rsp_supports_vcont(int fd)
{
    char reply[PKT + 1];
    if (!command(fd, "vCont?", reply, sizeof(reply))) {
        logf1("Could not query vCont support; refusing to continue a system process unsafely.\n");
        return false;
    }
    logf1("vCont support reply: %s\n", reply);
    return strstr(reply, "vCont;c") != NULL;
}

static bool rsp_resume_process(int fd, unsigned long pid)
{
    char q[80];
    (void)snprintf(q, sizeof(q), "vCont;c:p%lx.-1", pid);
    logf1("Resume all overlayDisp threads: %s\n", q);
    return send_packet(fd, q);
}

static bool get_stopped_thread(const char *stop_reply, char *thread, size_t cap)
{
    const char *p = strstr(stop_reply, "thread:");
    if (!p) return false;
    p += strlen("thread:");
    const char *end = strchr(p, ';');
    size_t n = end ? (size_t)(end - p) : strlen(p);
    if (n == 0 || n >= cap) return false;
    memcpy(thread, p, n);
    thread[n] = '\0';
    return true;
}

static bool is_stop_reply(const char *reply)
{
    return reply && (reply[0] == 'T' || reply[0] == 'S' ||
                     reply[0] == 'W' || reply[0] == 'X');
}

/* Ctrl-C does not guarantee the very next RSP packet is a stop reply:
   a queued command acknowledgement (OK) may arrive first. Keep waiting
   for a real stop before attempting to remove software breakpoints. */
static bool rsp_interrupt_and_wait(int fd, char *stop_reply, size_t cap)
{
    const char interrupt = 3;
    if (send(fd, &interrupt, 1, 0) != 1) {
        logf1("Could not send debugger interrupt.\n");
        return false;
    }

    set_socket_timeout_ms(fd, 1500);
    for (unsigned i = 0; i < 6; ++i) {
        if (!read_packet(fd, stop_reply, cap)) {
            logf1("Timed out waiting for a real stop reply after Ctrl-C.\n");
            set_socket_timeout_ms(fd, 3000);
            return false;
        }
        if (strcmp(stop_reply, "OK") == 0 || stop_reply[0] == '\0') {
            logf1("Ignored non-stop reply during interrupt: '%s'\n", stop_reply);
            continue;
        }
        if (is_stop_reply(stop_reply)) {
            logf1("Confirmed stopped state after interrupt: %s\n", stop_reply);
            set_socket_timeout_ms(fd, 3000);
            return true;
        }
        logf1("Ignored unexpected packet while waiting for interrupt stop: %s\n", stop_reply);
    }
    set_socket_timeout_ms(fd, 3000);
    logf1("No confirmed stopped state after Ctrl-C.\n");
    return false;
}

/* With vCont support, step only the thread that hit the breakpoint. The
   command is asynchronous: wait for the resulting stop packet, ignoring
   queued command acknowledgements such as "OK". */
static bool rsp_single_step_thread(int fd, const char *thread, char *stop_reply, size_t cap)
{
    char q[128];
    (void)snprintf(q, sizeof(q), "vCont;s:%s", thread);
    logf1("Single-step stopped thread: %s\n", q);
    if (!send_packet(fd, q)) {
        logf1("Could not send vCont single-step request.\n");
        return false;
    }

    set_socket_timeout_ms(fd, 3000);
    for (unsigned i = 0; i < 8; ++i) {
        if (!read_packet(fd, stop_reply, cap)) {
            logf1("Timed out waiting for vCont single-step stop reply.\n");
            set_socket_timeout_ms(fd, 3000);
            return false;
        }
        if (strcmp(stop_reply, "OK") == 0 || stop_reply[0] == '\0') {
            logf1("Ignored non-stop reply during single-step: '%s'\n", stop_reply);
            continue;
        }
        if (stop_reply[0] == 'T' || stop_reply[0] == 'S') {
            char stopped_thread[64];
            if (!get_stopped_thread(stop_reply, stopped_thread, sizeof(stopped_thread))) {
                logf1("Single-step stop reply omitted thread identity: %s\n", stop_reply);
                set_socket_timeout_ms(fd, 3000);
                return false;
            }
            if (strcmp(stopped_thread, thread) != 0) {
                logf1("Single-step stopped unexpected thread %s (expected %s): %s\n",
                      stopped_thread, thread, stop_reply);
                set_socket_timeout_ms(fd, 3000);
                return false;
            }
            logf1("Single-step stop reply confirmed for thread %s: %s\n",
                  stopped_thread, stop_reply);
            set_socket_timeout_ms(fd, 3000);
            return true;
        }
        logf1("Unexpected response during single-step: %s\n", stop_reply);
        set_socket_timeout_ms(fd, 3000);
        return false;
    }
    set_socket_timeout_ms(fd, 3000);
    logf1("Too many non-stop replies while waiting for single-step.\n");
    return false;
}

static bool trace_jointout_routes(int fd, unsigned long pid, uint64_t module_base, PadState *pad)
{
    uint64_t addr_a = module_base + SITE_A;
    uint64_t addr_b = module_base + SITE_B;
    unsigned hits_a = 0, hits_b = 0, other_stops = 0;
    char reply[PKT + 1];
    bool target_stopped = true; /* vAttach leaves the process stopped in all-stop mode. */

    logf1("\n--- SeJointOut runtime trace ---\n");
    logf1("Candidate callsite A: module + 0x%llx = 0x%" PRIx64 "\n",
          (unsigned long long)SITE_A, addr_a);
    logf1("Candidate callsite B: module + 0x%llx = 0x%" PRIx64 "\n",
          (unsigned long long)SITE_B, addr_b);
    logf1("Temporary runtime breakpoints only; no files on SD are patched.\n");

    /* overlayDisp is multithreaded. Do not use legacy 'c': on this target it
       can leave the system UI appearing frozen. Require vCont so we can resume
       every thread in the process explicitly, and fail closed if unsupported. */
    if (!rsp_supports_vcont(fd)) {
        logf1("This GDB stub did not advertise vCont support; aborting before installing breakpoints.\n");
        logf1("Detaching now to release overlayDisp rather than risk leaving it paused.\n");
        save_log();
        (void)rsp_detach(fd, pid);
        snprintf(status_line, sizeof(status_line), "vCont unsupported; safely aborted and detached.");
        return false;
    }

    dump_candidate_code(fd, module_base);

    if (!rsp_breakpoint(fd, true, addr_a)) {
        logf1("Could not set breakpoint A; refusing to start trace.\n");
        (void)rsp_detach(fd, pid);
        return false;
    }
    if (!rsp_breakpoint(fd, true, addr_b)) {
        (void)rsp_breakpoint(fd, false, addr_a);
        logf1("Could not set breakpoint B; removed A and aborted.\n");
        (void)rsp_detach(fd, pid);
        return false;
    }
    save_log();

    if (!rsp_resume_process(fd, pid)) {
        logf1("Could not resume all overlayDisp threads.\n");
        (void)rsp_breakpoint(fd, false, addr_a);
        (void)rsp_breakpoint(fd, false, addr_b);
        (void)rsp_detach(fd, pid);
        return false;
    }
    target_stopped = false;

    bool finished = false;
    while (!finished) {
        consoleClear();
        printf("OVERLAYDISP SOUND TRACE\n=======================\n\n");
        printf("Temporary debugger breakpoints (RAM only)\n");
        printf("Site A hits: %u\nSite B hits: %u\nOther stops: %u\n\n",
               hits_a, hits_b, other_stops);
        printf("Disconnect/reconnect Joy-Cons to reproduce.\n");
        printf("Press PLUS to stop and clean up breakpoints.\n");
        consoleUpdate(NULL);

        set_socket_timeout_ms(fd, 500);
        bool got_stop = read_packet(fd, reply, sizeof(reply));
        set_socket_timeout_ms(fd, 3000);
        padUpdate(pad);
        if (padGetButtonsDown(pad) & HidNpadButton_Plus) {
            /* The poll may already have consumed the actual stop packet just
               before PLUS was detected. Preserve that state instead of issuing
               another interrupt and misreading a queued OK as a stop. */
            if (got_stop && is_stop_reply(reply)) {
                target_stopped = true;
                logf1("PLUS pressed after stop was already received: %s\n", reply);
            } else if (!target_stopped) {
                target_stopped = rsp_interrupt_and_wait(fd, reply, sizeof(reply));
            } else {
                logf1("PLUS pressed while target was already known stopped.\n");
            }
            finished = true;
            continue;
        }
        if (!got_stop) continue;

        /* A debugger may deliver a queued "OK" packet before the async stop packet.
         * It is a command response, not a CPU stop; ignore it and keep polling. */
        if (strcmp(reply, "OK") == 0 || reply[0] == '\0') {
            logf1("Ignored non-stop debugger reply while running: '%s'\n", reply);
            continue;
        }
        if (!is_stop_reply(reply)) {
            ++other_stops;
            logf1("Ignored non-stop debugger packet: %s\n", reply);
            save_log();
            continue;
        }
        target_stopped = true;

        uint64_t pc = 0;
        if (!read_pc_register(fd, &pc)) {
            ++other_stops;
            logf1("Could not identify actual stop PC after reply: %s\n", reply);
            save_log();
            finished = true;
            continue;
        }
        uint64_t offset = pc >= module_base ? pc - module_base : UINT64_MAX;
        uint64_t hit_site = 0;
        if (offset == SITE_A || offset == SITE_A + 4) { ++hits_a; hit_site = addr_a; }
        else if (offset == SITE_B || offset == SITE_B + 4) { ++hits_b; hit_site = addr_b; }
        else {
            ++other_stops;
            logf1("Unexpected stop PC=0x%" PRIx64 " (module offset 0x%" PRIx64 "): %s\n",
                  pc, offset, reply);
            save_log();
            finished = true;
            continue;
        }
        logf1("Route hit: %s at PC 0x%" PRIx64 " (A=%u, B=%u)\n",
              hit_site == addr_a ? "A" : "B", pc, hits_a, hits_b);
        dump_hit_registers(fd, hit_site == addr_a ? "A" : "B", pc);
        save_log();

        /* Remove the trap; normalise PC to the original instruction if the stub
         * reports it after the four-byte BRK; single-step only the hit thread. */
        if (!rsp_breakpoint(fd, false, hit_site)) {
            logf1("Could not remove hit breakpoint; aborting trace.\n");
            finished = true;
            continue;
        }
        if (pc == hit_site + 4 && !write_pc_register(fd, hit_site)) {
            logf1("Could not restore PC to the breakpoint instruction; aborting.\n");
            finished = true;
            continue;
        }
        char hit_thread[64];
        if (!get_stopped_thread(reply, hit_thread, sizeof(hit_thread))) {
            logf1("Stop reply did not identify the hit thread; refusing legacy single-step. Reply: %s\n", reply);
            finished = true;
            continue;
        }
        logf1("Hit thread: %s\n", hit_thread);
        target_stopped = false;
        if (!rsp_single_step_thread(fd, hit_thread, reply, sizeof(reply))) {
            logf1("Single-step over callsite failed; target state is uncertain; cleanup will attempt to stop it first.\n");
            finished = true;
            continue;
        }
        target_stopped = true;
        if (!rsp_breakpoint(fd, true, hit_site)) {
            logf1("Could not re-arm hit breakpoint; aborting trace.\n");
            finished = true;
            continue;
        }
        if (!rsp_resume_process(fd, pid)) {
            logf1("Could not resume all overlayDisp threads after hit.\n");
            finished = true;
            continue;
        }
        target_stopped = false;
    }

    /* Safety-first teardown: never write/remove a software breakpoint while
       the process may be executing. First get a confirmed stop; if that fails,
       avoid further code-memory modifications and try to detach. */
    if (!target_stopped) {
        logf1("Cleanup: target may be running; requesting a confirmed stop.\n");
        target_stopped = rsp_interrupt_and_wait(fd, reply, sizeof(reply));
    }
    if (target_stopped) {
        (void)rsp_breakpoint(fd, false, addr_a);
        (void)rsp_breakpoint(fd, false, addr_b);
    } else {
        logf1("Cleanup warning: no confirmed stop; skipped breakpoint removal while execution state is uncertain.\n");
    }
    (void)rsp_detach(fd, pid);

    logf1("\n--- trace result ---\nSite A hits: %u\nSite B hits: %u\nOther stops: %u\n",
          hits_a, hits_b, other_stops);
    logf1("If both counters stay at 0 after several disconnect events, this routing hypothesis was not observed.\n");
    save_log();
    snprintf(status_line, sizeof(status_line), "Trace finished: A=%u B=%u other=%u. Report saved.", hits_a, hits_b, other_stops);
    return true;
}

static bool process_xml(int fd, char *xml, size_t cap)
{
    char q[100], r[PKT + 1]; size_t used = 0; unsigned offset = 0;
    logf1("\n--- qXfer process list ---\n");
    while (offset < 50000 && used + 2 < cap) {
        (void)snprintf(q, sizeof(q), "qXfer:osdata:read:processes:%x,800", offset);
        if (!command(fd, q, r, sizeof(r))) { logf1("qXfer timeout at %x\n", offset); return false; }
        if (r[0] != 'm' && r[0] != 'l') { logf1("Unsupported qXfer reply: %.120s\n", r); return false; }
        size_t chunk = strlen(r + 1);
        if (chunk >= cap - used) chunk = cap - used - 1;
        memcpy(xml + used, r + 1, chunk); used += chunk; xml[used] = 0; offset += (unsigned)chunk;
        if (r[0] == 'l') break;
        if (!chunk) return false;
    }
    logf1("%s\n", xml); return used != 0;
}
static unsigned long find_pid(const char *xml)
{
    const char *hit = strstr(xml, "overlayDisp");
    if (!hit) hit = strstr(xml, "010000000000100C");
    if (!hit) hit = strstr(xml, "010000000000100c");
    if (!hit) return 0;
    const char *item = xml, *scan = xml;
    while ((scan = strstr(scan, "<item")) != NULL && scan < hit) { item = scan; scan += 5; }
    const char *end = strstr(item, "</item>");
    if (end && hit > end) return 0;
    const char *pid = strstr(item, "name=\"pid\"");
    if (!pid || (end && pid > end)) pid = strstr(item, "name=\"PID\"");
    if (!pid || (end && pid > end)) return 0;
    const char *gt = strchr(pid, '>');
    if (!gt || (end && gt > end)) return 0;
    ++gt; while (*gt == ' ' || *gt == '\t') ++gt;
    char digits[24]; size_t n = 0;
    while (gt[n] >= '0' && gt[n] <= '9' && n + 1 < sizeof(digits)) { digits[n] = gt[n]; ++n; }
    digits[n] = 0; return n ? strtoul(digits, NULL, 10) : 0;
}
static void draw_ui(void)
{
    consoleClear();
    printf("OVERLAYDISP TRACE PROBE\n=======================\n\n");
    printf("Temporary runtime trace of SeJointOut routes.\n\n");
    printf("Status: %s\n\n", status_line);
    printf("A: start trace (then disconnect Joy-Cons)\nPLUS: exit\n\n");
    printf("Report: %s\n", REPORT);
    consoleUpdate(NULL);
}

static void run_probe(PadState *pad)
{
    loglen = 0;
    logbuf[0] = '\0';
    logf1("OverlayDispTraceProbe - SeJointOut runtime trace\n");
    logf1("Program ID: 010000000000100C\n");
    logf1("Runtime trace candidates: module offsets 0x315040 and 0x31507c.\n");
    logf1("Breakpoints exist only in debugger memory and will be removed at exit.\n");

    snprintf(status_line, sizeof(status_line), "Starting debugger connection...");
    draw_ui();
    Result rc = socketInitializeDefault();
    if (R_FAILED(rc)) {
        logf1("socketInitializeDefault failed: 0x%08x\n", (unsigned)rc);
        snprintf(status_line, sizeof(status_line), "Socket init failed: 0x%08x", (unsigned)rc);
        save_log();
        return;
    }

    struct in_addr local;
    char ip[INET_ADDRSTRLEN] = "127.0.0.1";
    bool have_ip = get_local_ip(&local);
    if (have_ip) (void)snprintf(ip, sizeof(ip), "%s", inet_ntoa(local));

    snprintf(status_line, sizeof(status_line), "Connecting to Atmosphere GDB stub...");
    draw_ui();
    int fd = try_connect("127.0.0.1");
    const char *used_ip = "127.0.0.1";
    if (fd < 0 && have_ip) { fd = try_connect(ip); used_ip = ip; }
    if (fd < 0) {
        logf1("Cannot connect to 127.0.0.1:%d or %s:%d. errno=%d\n", PORT, ip, PORT, errno);
        snprintf(status_line, sizeof(status_line), "GDB stub unavailable. Report saved.");
        save_log();
        socketExit();
        return;
    }
    logf1("Connected to %s:%d\n", used_ip, PORT);

    char reply[PKT + 1];
    if (command(fd, "qSupported", reply, sizeof(reply)))
        logf1("qSupported reply: %s\n", reply);

    snprintf(status_line, sizeof(status_line), "Reading process list...");
    draw_ui();
    char *xml = (char *)calloc(1, 50001);
    unsigned long pid = 0;
    if (xml && process_xml(fd, xml, 50001)) pid = find_pid(xml);
    if (!pid) {
        logf1("Could not locate overlayDisp PID in GDB process list.\n");
        snprintf(status_line, sizeof(status_line), "overlayDisp not found; report saved.");
        save_log();
        free(xml);
        close(fd);
        socketExit();
        return;
    }
    free(xml);
    logf1("Found overlayDisp PID decimal=%lu hex=%lx\n", pid, pid);

    (void)snprintf(status_line, sizeof(status_line), "Attaching to overlayDisp PID %lu...", pid);
    draw_ui();
    reply[0] = '\0';
    char q[64];
    (void)snprintf(q, sizeof(q), "vAttach;%lx", pid);
    if (!command(fd, q, reply, sizeof(reply)) ||
        (reply[0] != 'T' && reply[0] != 'S' && reply[0] != 'W')) {
        logf1("Attach failed or unexpected reply: %s\n", reply);
        snprintf(status_line, sizeof(status_line), "Attach failed; report saved.");
        save_log();
        close(fd);
        socketExit();
        return;
    }
    logf1("Attach reply: %s\n", reply);

    char info[8192];
    uint64_t module_base = 0;
    if (!monitor_info_text(fd, info, sizeof(info)) || !parse_module_base(info, &module_base)) {
        logf1("Could not decode module base from Atmosphere monitor get info. No breakpoints were set.\n");
        snprintf(status_line, sizeof(status_line), "Module base not decoded; report saved.");
        save_log();
        (void)rsp_detach(fd, pid);
        close(fd);
        socketExit();
        return;
    }
    logf1("overlayDisp module base: 0x%" PRIx64 "\n", module_base);
    logf1("Breakpoint targets: 0x%" PRIx64 " and 0x%" PRIx64 "\n",
          module_base + SITE_A, module_base + SITE_B);
    save_log();

    snprintf(status_line, sizeof(status_line), "Trace ready. Disconnect Joy-Cons; PLUS stops.");
    draw_ui();
    (void)trace_jointout_routes(fd, pid, module_base, pad);
    close(fd);
    socketExit();
    save_log();
}
int main(void)
{
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad; padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad); u64 down = padGetButtonsDown(&pad);
        if (down & HidNpadButton_Plus) break;
        if (down & HidNpadButton_A) { snprintf(status_line, sizeof(status_line), "Starting SeJointOut trace..."); run_probe(&pad); }
        draw_ui();
        svcSleepThread(20000000ULL);
    }
    consoleExit(NULL); return 0;
}