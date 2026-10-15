#include <switch.h>
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
static bool monitor_info(int fd)
{
    char q[64] = "qRcmd,";
    char r[PKT + 1];
    const char *s = "get info";
    size_t n = strlen(q);
    for (size_t i = 0; s[i] && n + 2 < sizeof(q); ++i) {
        (void)snprintf(q + n, sizeof(q) - n, "%02x", (unsigned char)s[i]);
        n += 2;
    }
    logf1("\n--- monitor get info ---\n");
    if (!send_packet(fd, q)) return false;
    for (unsigned i = 0; i < 128; ++i) {
        if (!read_packet(fd, r, sizeof(r))) { logf1("monitor response timeout\n"); return false; }
        if (r[0] == 'O') {
            char decoded[PKT/2 + 1]; decode_hex(r + 1, decoded, sizeof(decoded));
            logf1("%s", decoded); continue;
        }
        logf1("\n[monitor reply: %s]\n", r);
        return strcmp(r, "OK") == 0;
    }
    logf1("monitor output exceeded safety limit\n"); return false;
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
    printf("Read-only GDB query; no patches or memory writes.\n\n");
    printf("Status: %s\n\n", status_line);
    printf("A: start / retry probe\nPLUS: exit\n\n");
    printf("Report: %s\n", REPORT);
    consoleUpdate(NULL);
}

static void run_probe(void)
{
    loglen = 0; logbuf[0] = 0;
    logf1("OverlayDispTraceProbe report\nProgram ID: 010000000000100C\n");
    logf1("Read-only process/module snapshot. No breakpoints or memory writes.\n");
    snprintf(status_line, sizeof(status_line), "Starting network probe...");
    draw_ui();
    Result rc = socketInitializeDefault();
    if (R_FAILED(rc)) {
        logf1("socketInitializeDefault failed: 0x%08x\n", (unsigned)rc);
        snprintf(status_line, sizeof(status_line), "Socket init failed: 0x%08x", (unsigned)rc);
        save_log(); return;
    }
    snprintf(status_line, sizeof(status_line), "Detecting local network address...");
    draw_ui();
    struct in_addr local; char ip[INET_ADDRSTRLEN] = "127.0.0.1";
    bool have_ip = get_local_ip(&local);
    if (have_ip) (void)snprintf(ip, sizeof(ip), "%s", inet_ntoa(local));
    snprintf(status_line, sizeof(status_line), "Connecting to GDB stub (2 s timeout per address)...");
    draw_ui();
    int fd = try_connect("127.0.0.1"); const char *used_ip = "127.0.0.1";
    if (fd < 0 && have_ip) { fd = try_connect(ip); used_ip = ip; }
    if (fd < 0) {
        logf1("Cannot connect to 127.0.0.1:%d or %s:%d. errno=%d\n", PORT, ip, PORT, errno);
        logf1("Check Atmosphere config and reboot: enable_standalone_gdbstub=u8!0x1; enable_htc=u8!0x0\n");
        snprintf(status_line, sizeof(status_line), "GDB stub not reachable; report saved.");
        save_log(); socketExit(); return;
    }
    logf1("Connected to %s:%d\n", used_ip, PORT);
    (void)snprintf(status_line, sizeof(status_line), "Connected; querying process list...");
    char reply[PKT + 1];
    snprintf(status_line, sizeof(status_line), "Connected. Querying debugger...");
    draw_ui();
    if (command(fd, "qSupported", reply, sizeof(reply))) logf1("\n--- qSupported ---\n%s\n", reply);
    else logf1("qSupported timeout\n");

    snprintf(status_line, sizeof(status_line), "Reading process list...");
    draw_ui();
    char *xml = calloc(1, 50001);
    unsigned long pid = 0;
    if (xml && process_xml(fd, xml, 50001)) pid = find_pid(xml);
    if (!pid) {
        logf1("\nNo overlayDisp PID parsed automatically; process XML above is preserved.\n");
        snprintf(status_line, sizeof(status_line), "Process list saved; inspect report.txt.");
    } else {
        logf1("\nFound overlayDisp PID decimal=%lu hex=%lx\n", pid, pid);
        snprintf(status_line, sizeof(status_line), "Found PID %lu; querying module map...", pid);
        draw_ui();
        char q[64];
        (void)snprintf(q, sizeof(q), "vAttach;%lx", pid);
        if (command(fd, q, reply, sizeof(reply))) {
            logf1("\n--- attach reply ---\n%s\n", reply);
            if (reply[0] == 'T' || reply[0] == 'S' || reply[0] == 'W') {
                (void)monitor_info(fd);
                (void)snprintf(q, sizeof(q), "D;%lx", pid);
                if (command(fd, q, reply, sizeof(reply))) logf1("\n--- detach reply ---\n%s\n", reply);
                else logf1("Detach timeout; reboot if process remains paused.\n");
            } else logf1("Attach rejected; detach not sent.\n");
        } else logf1("Attach timeout; no more commands sent.\n");
        snprintf(status_line, sizeof(status_line), "Done. Report saved for PID %lu.", pid);
    }
    free(xml); close(fd); socketExit();
    logf1("\nProbe finished; no breakpoints or memory writes were requested.\n");
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
        if (down & HidNpadButton_A) { snprintf(status_line, sizeof(status_line), "Running probe..."); run_probe(); }
        draw_ui();
        svcSleepThread(20000000ULL);
    }
    consoleExit(NULL); return 0;
}