#include <switch.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/select.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>

#define GDB_PORT 22225
#define RX_CAP 8192
#define XML_CAP 20000
#define LOG_DIR "sdmc:/switch/OverlayDispJointOutProbe"
#define LOG_PATH LOG_DIR "/report.txt"
#define OFF_RESOURCE_SIZE_LOAD 0x35686cULL
#define OFF_SLOT_INIT_RETURN  0x3568ccULL
#define OFF_VOICE_END         0x356fe4ULL

static int g_sock = -1;
static bool g_socket_initialized;
static bool g_attached;
static bool g_target_stopped = true;
static FILE *g_log;
static PadState g_pad;
static uint64_t g_base;
static unsigned g_pid;
static char g_pid_hex[16];
static char g_thread[64];
static uint64_t g_slot;
static bool g_have_slot;
static bool g_end_handler_seen;
static bool g_watchpoint_active;
static bool g_bp_size_active;
static bool g_bp_init_active;
static bool g_bp_end_active;
static bool g_done;
static bool g_cancelled;

static void log_line(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("%s\n", line);
    /* libnx's console is not visible until it is explicitly refreshed. */
    consoleUpdate(NULL);
    if (g_log) {
        fprintf(g_log, "%s\n", line);
        fflush(g_log);
    }
}

static bool send_all(const char *data, size_t size)
{
    size_t sent = 0;
    while (sent < size) {
        ssize_t n = send(g_sock, data + sent, size - sent, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

/* Return 1 for a byte, 0 for socket timeout, -1 for a hard error. */
static int read_byte(char *out)
{
    for (;;) {
        ssize_t n = recv(g_sock, out, 1, 0);
        if (n == 1) return 1;
        if (n == 0) return -1;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
}

/* Read one RSP packet and expand GDB run-length encoding. Return 0 only
 * when a timeout occurs before the packet starts; partial packets are errors. */
static int read_packet(char *out, size_t cap)
{
    char c, checksum1, checksum2;
    char raw[RX_CAP + 1];
    size_t raw_used = 0, out_used = 0;
    int rc;
    if (!out || cap == 0) return -1;

    for (;;) {
        rc = read_byte(&c);
        if (rc <= 0) return rc;
        if (c == '$') break;
    }
    for (;;) {
        rc = read_byte(&c);
        if (rc <= 0) return -1;
        if (c == '#') break;
        if (raw_used < RX_CAP) raw[raw_used++] = c;
    }
    if (read_byte(&checksum1) != 1 || read_byte(&checksum2) != 1) return -1;
    raw[raw_used] = '\0';
    (void)checksum1;
    (void)checksum2;
    (void)send_all("+", 1);

    size_t pos = 0;
    while (pos < raw_used && out_used + 1 < cap) {
        if (raw[pos] == '*' && out_used > 0 && pos + 1 < raw_used &&
            (unsigned char)raw[pos + 1] >= 32 && (unsigned char)raw[pos + 1] <= 126) {
            size_t repeats = (size_t)((unsigned char)raw[pos + 1] - 29);
            const char previous = out[out_used - 1];
            pos += 2;
            while (repeats-- > 0 && out_used + 1 < cap)
                out[out_used++] = previous;
        } else {
            out[out_used++] = raw[pos++];
        }
    }
    out[out_used] = '\0';
    return 1;
}
static bool rsp_send(const char *payload)
{
    char frame[RX_CAP + 64];
    size_t n = strlen(payload);
    unsigned sum = 0;
    if (n + 5 >= sizeof(frame)) return false;
    frame[0] = '$';
    memcpy(frame + 1, payload, n);
    for (size_t i = 0; i < n; ++i)
        sum = (sum + (unsigned char)payload[i]) & 0xff;
    snprintf(frame + 1 + n, 5, "#%02x", sum);
    return send_all(frame, n + 4);
}

static bool rsp_command(const char *cmd, char *reply, size_t reply_cap)
{
    if (!rsp_send(cmd)) return false;
    return read_packet(reply, reply_cap) == 1;
}

static bool rsp_ok(const char *cmd)
{
    char reply[RX_CAP];
    if (!rsp_command(cmd, reply, sizeof(reply))) {
        log_line("RSP command timeout/error: %s", cmd);
        return false;
    }
    if (strcmp(reply, "OK") != 0) {
        log_line("RSP command rejected: %s -> %s", cmd, reply);
        return false;
    }
    return true;
}

static bool rsp_read_processes(char *xml, size_t cap)
{
    size_t used = 0;
    unsigned offset = 0;
    while (used + 2 < cap) {
        char cmd[96], reply[RX_CAP];
        snprintf(cmd, sizeof(cmd), "qXfer:osdata:read:processes:%x,400", offset);
        if (!rsp_command(cmd, reply, sizeof(reply))) return false;
        if (reply[0] != 'm' && reply[0] != 'l') {
            log_line("Process list query returned: %.100s", reply);
            return false;
        }
        size_t n = strlen(reply + 1);
        if (n > cap - used - 1) n = cap - used - 1;
        memcpy(xml + used, reply + 1, n);
        used += n;
        xml[used] = '\0';
        if (reply[0] == 'l') return true;
        if (!n) return false;
        offset += (unsigned)n;
    }
    return false;
}

static bool find_overlaydisp_pid(const char *xml, unsigned *pid_out)
{
    const char *cmd = strstr(xml, "<column name=\"command\">overlayDisp</column>");
    if (!cmd) return false;
    const char *start = cmd;
    while (start > xml && (size_t)(cmd - start) < 200) {
        if (strncmp(start, "<column name=\"pid\">", 19) == 0) {
            start += 19;
            char *end = NULL;
            unsigned long v = strtoul(start, &end, 10);
            if (end == start || !v || v > 0xffff) return false;
            *pid_out = (unsigned)v;
            return true;
        }
        --start;
    }
    return false;
}

static bool is_hex_string(const char *s)
{
    size_t n = strlen(s);
    if (!n || (n & 1U)) return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

static bool decode_hex_text(const char *hex, char *out, size_t cap)
{
    size_t n = strlen(hex);
    if (!out || cap == 0 || !is_hex_string(hex) || n / 2 >= cap) return false;
    for (size_t i = 0; i < n / 2; ++i) {
        char pair[3] = {hex[i * 2], hex[i * 2 + 1], 0};
        char *end = NULL;
        unsigned long v = strtoul(pair, &end, 16);
        if (end == pair || *end || v > 255) return false;
        out[i] = (char)v;
    }
    out[n / 2] = '\0';
    return true;
}

static bool rsp_monitor_get_info(char *text, size_t cap)
{
    static const char encoded[] = "67657420696e666f"; /* "get info" */
    char cmd[96];
    if (!text || cap == 0) return false;
    text[0] = '\0';

    snprintf(cmd, sizeof(cmd), "qRcmd,%s", encoded);
    if (!rsp_send(cmd)) {
        log_line("monitor get info: failed to send qRcmd request.");
        return false;
    }

    size_t used = 0;
    for (unsigned packets = 0; packets < 128 && used + 1 < cap; ++packets) {
        char reply[RX_CAP];
        if (read_packet(reply, sizeof(reply)) != 1) {
            log_line("monitor get info: timed out or malformed response after %u packet(s).", packets);
            return false;
        }

        if (strcmp(reply, "OK") == 0) {
            text[used] = '\0';
            if (!used) log_line("monitor get info: stub returned OK without any output.");
            return used > 0;
        }

        if (reply[0] == 'E') {
            log_line("monitor get info: stub returned error packet '%s'.", reply);
            return false;
        }

        /* Standard GDB RSP console output: O followed by hex-encoded bytes. */
        if (reply[0] == 'O' && is_hex_string(reply + 1)) {
            char decoded[RX_CAP];
            if (!decode_hex_text(reply + 1, decoded, sizeof(decoded))) {
                log_line("monitor get info: couldn't decode standard O packet.");
                return false;
            }
            size_t n = strlen(decoded);
            if (n > cap - used - 1) n = cap - used - 1;
            memcpy(text + used, decoded, n);
            used += n;
            text[used] = '\0';
            continue;
        }

        /*
         * Atmosphere's standalone GDB stub currently hex-encodes the entire
         * monitor output directly in qRcmd's reply (without the standard
         * leading 'O' packet marker / trailing 'OK'). Accept that form too.
         */
        if (is_hex_string(reply)) {
            if (!decode_hex_text(reply, text, cap)) {
                log_line("monitor get info: couldn't decode Atmosphere's raw hex reply.");
                return false;
            }
            used = strlen(text);
            log_line("monitor get info: decoded Atmosphere raw-hex qRcmd response (%u chars).",
                     (unsigned)used);
            return used > 0;
        }

        log_line("monitor get info: unexpected RSP reply '%.*s'.",
                 160, reply);
        return false;
    }

    text[used] = '\0';
    log_line("monitor get info: response did not terminate within the packet/output limit.");
    return false;
}

static bool parse_module_base(const char *text, uint64_t *base_out)
{
    /* Atmosphere's "get info" prints a Modules section with the module base
       followed by its end address and ".elf"; it does not print the phrase
       "overlayDisp module base:" used by our own logs. */
    const char *p = strstr(text, "Modules:");
    if (!p) return false;
    p = strstr(p, "0x");
    if (!p) return false;
    char *end = NULL;
    unsigned long long value = strtoull(p + 2, &end, 16);
    if (end == p + 2 || value == 0) return false;
    *base_out = (uint64_t)value;
    return true;
}

static bool select_thread(const char *stop_reply)
{
    const char *p = strstr(stop_reply, "thread:");
    if (!p) return false;
    p += strlen("thread:");
    size_t n = strcspn(p, ";,");
    if (!n || n >= sizeof(g_thread)) return false;
    memcpy(g_thread, p, n);
    g_thread[n] = '\0';
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "Hg%s", g_thread);
    return rsp_ok(cmd);
}

/* RSP register IDs use hexadecimal numbering; returned register bytes are little-endian. */
static bool read_reg(unsigned regno, uint64_t *value)
{
    char cmd[24], reply[RX_CAP];
    snprintf(cmd, sizeof(cmd), "p%x", regno);
    if (!rsp_command(cmd, reply, sizeof(reply))) return false;
    if (strlen(reply) < 16 || reply[0] == 'E') return false;
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) {
        char hex[3] = {reply[i * 2], reply[i * 2 + 1], 0};
        char *end = NULL;
        unsigned long b = strtoul(hex, &end, 16);
        if (end == hex || b > 255) return false;
        v |= ((uint64_t)b << (i * 8));
    }
    *value = v;
    return true;
}

static bool write_reg(unsigned regno, uint64_t value)
{
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "P%x=%02x%02x%02x%02x%02x%02x%02x%02x",
             regno,
             (unsigned)(value & 0xff), (unsigned)((value >> 8) & 0xff),
             (unsigned)((value >> 16) & 0xff), (unsigned)((value >> 24) & 0xff),
             (unsigned)((value >> 32) & 0xff), (unsigned)((value >> 40) & 0xff),
             (unsigned)((value >> 48) & 0xff), (unsigned)((value >> 56) & 0xff));
    return rsp_ok(cmd);
}

static bool read_mem(uint64_t addr, void *buffer, size_t len)
{
    char cmd[96], reply[RX_CAP];
    if (len * 2 + 1 > sizeof(reply)) return false;
    snprintf(cmd, sizeof(cmd), "m%llx,%zx", (unsigned long long)addr, len);
    if (!rsp_command(cmd, reply, sizeof(reply))) return false;
    if (reply[0] == 'E' || strlen(reply) < len * 2) return false;
    unsigned char *out = (unsigned char *)buffer;
    for (size_t i = 0; i < len; ++i) {
        char hex[3] = {reply[i * 2], reply[i * 2 + 1], 0};
        char *end = NULL;
        unsigned long b = strtoul(hex, &end, 16);
        if (end == hex || b > 255) return false;
        out[i] = (unsigned char)b;
    }
    return true;
}

static bool read_string(uint64_t addr, char *out, size_t cap)
{
    if (!cap) return false;
    size_t ask = cap - 1;
    if (ask > 64) ask = 64;
    unsigned char tmp[64];
    if (!read_mem(addr, tmp, ask)) return false;
    size_t n = 0;
    while (n < ask && tmp[n] != 0) ++n;
    memcpy(out, tmp, n);
    out[n] = '\0';
    return true;
}

static bool read_slot_values(uint64_t slot, uint32_t *state, uint64_t *count,
                             uint64_t *cursor, uint64_t *data)
{
    unsigned char raw[0x20];
    if (!read_mem(slot, raw, sizeof(raw))) return false;
    memcpy(state, raw, 4);
    memcpy(data, raw + 8, 8);
    memcpy(count, raw + 0x10, 8);
    memcpy(cursor, raw + 0x18, 8);
    return true;
}

static bool set_breakpoint(uint64_t addr)
{
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "Z0,%llx,4", (unsigned long long)addr);
    return rsp_ok(cmd);
}
static bool remove_breakpoint(uint64_t addr)
{
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "z0,%llx,4", (unsigned long long)addr);
    return rsp_ok(cmd);
}
static bool set_write_watchpoint(uint64_t addr)
{
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "Z2,%llx,4", (unsigned long long)addr);
    return rsp_ok(cmd);
}
static bool remove_write_watchpoint(uint64_t addr)
{
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "z2,%llx,4", (unsigned long long)addr);
    return rsp_ok(cmd);
}

static bool continue_process(void)
{
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "vCont;c:p%x.-1", g_pid);
    /* Run-control requests are asynchronous: do not wait for a reply here,
       because the next packet may be an ACK or a real breakpoint stop. */
    if (!rsp_send(cmd)) {
        log_line("Could not send resume command: %s", cmd);
        return false;
    }
    g_target_stopped = false;
    return true;
}

static void set_socket_timeout_ms(int fd, long milliseconds);

/* Stop overlayDisp before removing traps. If a real stop cannot be confirmed,
 * do not issue breakpoint-removal or detach commands while it may be running. */
static bool interrupt_and_wait_for_stop(void)
{
    const char interrupt = 3;
    if (!send_all(&interrupt, 1)) {
        log_line("Could not send debugger interrupt.");
        return false;
    }
    set_socket_timeout_ms(g_sock, 1500);
    for (unsigned i = 0; i < 8; ++i) {
        char reply[RX_CAP];
        int rc = read_packet(reply, sizeof(reply));
        if (rc != 1) {
            log_line("Timed out waiting for a stop after interrupt.");
            set_socket_timeout_ms(g_sock, 1000);
            return false;
        }
        if (strcmp(reply, "OK") == 0 || reply[0] == '\0') {
            log_line("Ignoring queued response while waiting for interrupt stop: '%s'", reply);
            continue;
        }
        if (reply[0] == 'T' || reply[0] == 'S') {
            g_target_stopped = true;
            log_line("Confirmed stopped state after interrupt: %s", reply);
            set_socket_timeout_ms(g_sock, 1000);
            return true;
        }
        log_line("Unexpected packet while waiting for interrupt stop: %.100s", reply);
    }
    set_socket_timeout_ms(g_sock, 1000);
    return false;
}

static void remove_all_breakpoints(void)
{
    if (g_bp_size_active) { remove_breakpoint(g_base + OFF_RESOURCE_SIZE_LOAD); g_bp_size_active = false; }
    if (g_bp_init_active) { remove_breakpoint(g_base + OFF_SLOT_INIT_RETURN); g_bp_init_active = false; }
    if (g_bp_end_active) { remove_breakpoint(g_base + OFF_VOICE_END); g_bp_end_active = false; }
    if (g_watchpoint_active && g_slot) { remove_write_watchpoint(g_slot); g_watchpoint_active = false; }
}

static bool validate_target_code(void)
{
    struct Signature {
        uint64_t offset;
        uint32_t expected;
        const char *label;
    };
    /* Instruction words from the exact original NSO supplied for this audit.
       Read live memory and fail closed if this isn't the expected build. */
    static const struct Signature signatures[] = {
        {0x356868ULL, 0xb9401129U, "resource byte-size load (LDR W9,[X9,#0x10])"},
        {0x35686cULL, 0xd341fd29U, "resource sample-count shift (LSR X9,X9,#1)"},
        {0x3568ccULL, 0xa9434ff4U, "voice-slot setup epilogue"},
        {0x356fe4ULL, 0x3940f008U, "voice end handler"}
    };
    for (size_t i = 0; i < sizeof(signatures) / sizeof(signatures[0]); ++i) {
        unsigned char raw[4];
        if (!read_mem(g_base + signatures[i].offset, raw, sizeof(raw))) {
            log_line("BUILD CHECK FAILED: cannot read instruction at module+0x%llx (%s).",
                     (unsigned long long)signatures[i].offset, signatures[i].label);
            return false;
        }
        uint32_t actual = (uint32_t)raw[0] | ((uint32_t)raw[1] << 8) |
                         ((uint32_t)raw[2] << 16) | ((uint32_t)raw[3] << 24);
        if (actual != signatures[i].expected) {
            log_line("BUILD CHECK FAILED at module+0x%llx: expected %08x, got %08x. Refusing to set breakpoints.",
                     (unsigned long long)signatures[i].offset,
                     signatures[i].expected, actual);
            return false;
        }
        log_line("BUILD CHECK OK module+0x%llx: %08x (%s)",
                 (unsigned long long)signatures[i].offset, actual, signatures[i].label);
    }
    char cue[32];
    if (!read_string(g_base + 0x38f80dULL, cue, sizeof(cue)) ||
        strcmp(cue, "SeJointOut") != 0) {
        log_line("BUILD CHECK FAILED: expected SeJointOut string is not at module+0x38f80d. Refusing to set breakpoints.");
        return false;
    }
    log_line("BUILD CHECK OK: SeJointOut label at module+0x38f80d.");
    return true;
}

static void set_socket_timeout_ms(int fd, long milliseconds)
{
    struct timeval tv;
    tv.tv_sec = milliseconds / 1000;
    tv.tv_usec = (milliseconds % 1000) * 1000;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static bool get_local_ip(struct in_addr *out)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(53);
    if (inet_pton(AF_INET, "1.1.1.1", &dst.sin_addr) != 1) {
        close(fd);
        return false;
    }
    bool ok = false;
    /* UDP connect selects a route; no application payload is sent. */
    if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) == 0) {
        struct sockaddr_in local;
        socklen_t length = sizeof(local);
        memset(&local, 0, sizeof(local));
        if (getsockname(fd, (struct sockaddr *)&local, &length) == 0 &&
            local.sin_addr.s_addr != htonl(INADDR_ANY)) {
            *out = local.sin_addr;
            ok = true;
        }
    }
    close(fd);
    return ok;
}

static int try_connect(const char *ip)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    set_socket_timeout_ms(fd, 1500);

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(GDB_PORT);
    if (inet_pton(AF_INET, ip, &address.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    int old_flags = fcntl(fd, F_GETFL, 0);
    if (old_flags < 0 || fcntl(fd, F_SETFL, old_flags | O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }
    int rc = connect(fd, (struct sockaddr *)&address, sizeof(address));
    if (rc != 0 && errno != EINPROGRESS && errno != EWOULDBLOCK) {
        close(fd);
        return -1;
    }
    if (rc != 0) {
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(fd, &writable);
        struct timeval timeout = {2, 0};
        rc = select(fd + 1, NULL, &writable, NULL, &timeout);
        if (rc <= 0) {
            close(fd);
            errno = ETIMEDOUT;
            return -1;
        }
        int socket_error = 0;
        socklen_t error_length = sizeof(socket_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) != 0 ||
            socket_error != 0) {
            close(fd);
            errno = socket_error ? socket_error : ECONNREFUSED;
            return -1;
        }
    }
    (void)fcntl(fd, F_SETFL, old_flags);
    set_socket_timeout_ms(fd, 1000);
    return fd;
}

static bool attach_debugger(void)
{
    char reply[RX_CAP], xml[XML_CAP], info[RX_CAP];
    Result rc = socketInitializeDefault();
    if (R_FAILED(rc)) {
        log_line("socketInitializeDefault failed: 0x%08x", (unsigned)rc);
        return false;
    }
    g_socket_initialized = true;

    struct in_addr local;
    char local_ip[INET_ADDRSTRLEN] = "127.0.0.1";
    bool have_local_ip = get_local_ip(&local);
    if (have_local_ip)
        (void)snprintf(local_ip, sizeof(local_ip), "%s", inet_ntoa(local));

    g_sock = try_connect("127.0.0.1");
    const char *connected_ip = "127.0.0.1";
    if (g_sock < 0 && have_local_ip) {
        g_sock = try_connect(local_ip);
        connected_ip = local_ip;
    }
    if (g_sock < 0) {
        log_line("Cannot connect to Atmosphere GDB stub at 127.0.0.1:%d or %s:%d (errno=%d).",
                 GDB_PORT, local_ip, GDB_PORT, errno);
        return false;
    }
    log_line("Connected to Atmosphere GDB stub at %s:%d.", connected_ip, GDB_PORT);

    if (rsp_command("qSupported", reply, sizeof(reply)))
        log_line("qSupported: %s", reply);
    if (!rsp_read_processes(xml, sizeof(xml))) {
        log_line("Could not enumerate target processes via qXfer:osdata.");
        return false;
    }
    if (!find_overlaydisp_pid(xml, &g_pid)) {
        log_line("overlayDisp PID was not found in debugger process list.");
        return false;
    }
    snprintf(g_pid_hex, sizeof(g_pid_hex), "%x", g_pid);
    log_line("Found overlayDisp: PID decimal %u (0x%s).", g_pid, g_pid_hex);

    snprintf(reply, sizeof(reply), "vAttach;%s", g_pid_hex);
    char attach_reply[RX_CAP];
    if (!rsp_command(reply, attach_reply, sizeof(attach_reply)) ||
        (attach_reply[0] != 'T' && attach_reply[0] != 'S' && attach_reply[0] != 'W')) {
        log_line("vAttach failed or returned unexpected response: %.160s", attach_reply);
        return false;
    }
    g_attached = true;
    g_target_stopped = true;
    log_line("Attached: %.160s", attach_reply);

    if (!rsp_command("vCont?", reply, sizeof(reply)) || strstr(reply, "vCont;c") == NULL) {
        log_line("This stub did not advertise vCont continue support; refusing to install breakpoints.");
        return false;
    }
    log_line("vCont support: %s", reply);

    if (!rsp_monitor_get_info(info, sizeof(info))) {
        log_line("Atmosphere 'get info' command failed; no breakpoints were set.");
        return false;
    }
    if (!parse_module_base(info, &g_base)) {
        log_line("Could not parse overlayDisp module base from monitor output. Output follows:\n%s", info);
        return false;
    }
    log_line("overlayDisp module base: 0x%llx", (unsigned long long)g_base);
    if (!validate_target_code()) return false;
    return true;
}

static bool arm_size_breakpoint(void)
{
    if (!set_breakpoint(g_base + OFF_RESOURCE_SIZE_LOAD)) return false;
    g_bp_size_active = true;
    log_line("Armed temporary breakpoint at module+0x%llx (resource byte size load).",
             (unsigned long long)OFF_RESOURCE_SIZE_LOAD);
    return continue_process();
}

static void try_cancel_from_controller(void)
{
    padUpdate(&g_pad);
    if (padGetButtonsDown(&g_pad) & HidNpadButton_Plus) g_cancelled = true;
}

/* Receive asynchronous RSP packets while the HOME process is running. */
static bool is_stop_reply(const char *reply)
{
    return reply && (reply[0] == 'T' || reply[0] == 'S' ||
                     reply[0] == 'W' || reply[0] == 'X');
}

static bool wait_stop(char *reply, size_t cap)
{
    while (!g_cancelled) {
        int rc = read_packet(reply, cap);
        if (rc == 1) {
            if (is_stop_reply(reply)) g_target_stopped = true;
            return true;
        }
        if (rc < 0) {
            log_line("Debugger connection closed or malformed RSP packet.");
            return false;
        }
        /* Keep the Homebrew applet UI alive while waiting for RSP data. */
        consoleUpdate(NULL);
        try_cancel_from_controller();
    }
    return false;
}

static bool on_breakpoint(uint64_t expected_pc, char *label, size_t label_cap,
                          uint64_t *x8, uint64_t *x9, uint64_t *x21,
                          uint64_t *x23, uint64_t *pc)
{
    if (!read_reg(32, pc)) return false;
    /* Some GDB stubs report the PC just after a software BRK. Rewind it so
       the replaced original instruction still executes after trap removal. */
    if (*pc == expected_pc + 4) {
        if (!write_reg(32, expected_pc)) return false;
        *pc = expected_pc;
    }
    if (*pc != expected_pc) return false;
    if (!read_reg(8, x8) || !read_reg(9, x9) ||
        !read_reg(0x15, x21) || !read_reg(0x17, x23)) return false;
    if (!read_string(*x21, label, label_cap)) return false;
    return true;
}

static bool do_runtime_trace(void)
{
    char stop[RX_CAP], label[96];
    uint64_t pc, x8, x9, x21, x23;
    uint64_t size_pc = g_base + OFF_RESOURCE_SIZE_LOAD;
    uint64_t init_pc = g_base + OFF_SLOT_INIT_RETURN;
    uint64_t end_pc = g_base + OFF_VOICE_END;
    if (!arm_size_breakpoint()) return false;
    log_line("Ready. Disconnect one Joy-Con once; capture includes the live resource size and voice-slot end state.");
    log_line("Run only on the HOME menu. Press PLUS to abort and clean up.");

    while (!g_cancelled && !g_done) {
        if (!wait_stop(stop, sizeof(stop))) break;
        if (stop[0] != 'T' && stop[0] != 'S') {
            log_line("Ignoring non-stop RSP reply: %.100s", stop);
            continue;
        }
        if (!select_thread(stop)) {
            log_line("Could not select stopped thread: %.120s", stop);
            continue_process();
            continue;
        }
        if (!on_breakpoint(size_pc, label, sizeof(label), &x8, &x9, &x21, &x23, &pc)) {
            uint64_t current_pc = 0, x0 = 0;
            (void)read_reg(32, &current_pc);
            (void)read_reg(0, &x0);
            if (g_have_slot && current_pc == end_pc && x0 == g_slot) {
                uint32_t state = 0;
                uint64_t count = 0, cursor = 0, data = 0;
                if (read_slot_values(g_slot, &state, &count, &cursor, &data)) {
                    log_line("VOICE_END_HANDLER: state=%u cursor=%llu samples=%llu data=0x%llx",
                             state, (unsigned long long)cursor, (unsigned long long)count,
                             (unsigned long long)data);
                    g_end_handler_seen = true;
                    remove_breakpoint(end_pc);
                    g_bp_end_active = false;
                    if (!g_watchpoint_active) {
                        log_line("RESULT: reached the voice end handler, but watchpoints are unsupported so final state timing is not captured.");
                        g_done = true;
                    }
                }
            } else if (g_have_slot && (strstr(stop, "watch:") || strstr(stop, "awatch:") || strstr(stop, "rwatch:"))) {
                uint32_t state = 0;
                uint64_t count = 0, cursor = 0, data = 0;
                if (read_slot_values(g_slot, &state, &count, &cursor, &data)) {
                    log_line("SLOT_STATE_WATCH: state=%u cursor=%llu samples=%llu data=0x%llx PC=0x%llx",
                             state, (unsigned long long)cursor, (unsigned long long)count,
                             (unsigned long long)data, (unsigned long long)current_pc);
                    if (state == 0) {
                        if (g_end_handler_seen && cursor >= count)
                            log_line("RESULT: slot stopped at/after its loaded sample count; normal end is consistent with this trace.");
                        else if (!g_end_handler_seen && cursor < count)
                            log_line("RESULT: slot became idle before its loaded sample count; this is evidence of an early state change, not just a short RAW.");
                        else
                            log_line("RESULT: slot is idle; use the cursor/count/PC values above to classify this transition.");
                        g_done = true;
                    }
                }
            }
            if (!g_done && !g_cancelled) continue_process();
            continue;
        }

        if (strcmp(label, "SeJointOut") != 0) {
            continue_process();
            continue;
        }

        log_line("JOINT_OUT_RESOURCE: label=%s data=0x%llx byte_size=%u sample_count_if_PCM16=%llu",
                 label, (unsigned long long)x8, (unsigned)(x9 & 0xffffffffULL),
                 (unsigned long long)((x9 & 0xffffffffULL) / 2ULL));
        if (g_bp_size_active) { remove_breakpoint(size_pc); g_bp_size_active = false; }
        if (!set_breakpoint(init_pc)) {
            log_line("Could not arm slot-init return breakpoint; resource size was still captured.");
            g_done = true;
            break;
        }
        g_bp_init_active = true;
        if (!continue_process()) return false;

        bool got_slot = false;
        while (!g_cancelled && !got_slot) {
            if (!wait_stop(stop, sizeof(stop))) break;
            if (stop[0] != 'T' && stop[0] != 'S') { continue_process(); continue; }
            if (!select_thread(stop)) { continue_process(); continue; }
            if (!read_reg(32, &pc) || !read_reg(0x15, &x21) || !read_reg(0x17, &x23)) {
                continue_process();
                continue;
            }
            if (pc == init_pc + 4) {
                if (!write_reg(32, init_pc)) {
                    log_line("Could not rewind PC at slot-init breakpoint.");
                    return false;
                }
                pc = init_pc;
            }
            if (pc == init_pc && read_string(x21, label, sizeof(label)) && strcmp(label, "SeJointOut") == 0) {
                g_slot = x23;
                g_have_slot = true;
                uint32_t state = 0;
                uint64_t count = 0, cursor = 0, data = 0;
                if (read_slot_values(g_slot, &state, &count, &cursor, &data)) {
                    log_line("VOICE_SLOT: ptr=0x%llx state=%u bytes=%llu samples=%llu cursor=%llu data=0x%llx",
                             (unsigned long long)g_slot, state,
                             (unsigned long long)(count * 2), (unsigned long long)count,
                             (unsigned long long)cursor, (unsigned long long)data);
                } else {
                    log_line("VOICE_SLOT: pointer=0x%llx captured, but fields could not be read.", (unsigned long long)g_slot);
                }
                got_slot = true;
            }
            if (!got_slot && !g_cancelled) continue_process();
        }
        if (g_cancelled || !got_slot) break;
        if (g_bp_init_active) { remove_breakpoint(init_pc); g_bp_init_active = false; }
        if (set_breakpoint(end_pc)) g_bp_end_active = true;
        if (set_write_watchpoint(g_slot)) {
            g_watchpoint_active = true;
            log_line("Armed write-watchpoint for this voice-slot state word at 0x%llx.", (unsigned long long)g_slot);
        } else {
            log_line("WARNING: GDB stub rejected the state watchpoint. Size reading remains valid, but early-stop detection may be incomplete.");
        }
        if (!continue_process()) return false;

        while (!g_cancelled && !g_done) {
            if (!wait_stop(stop, sizeof(stop))) break;
            if (stop[0] != 'T' && stop[0] != 'S') { continue_process(); continue; }
            if (!select_thread(stop)) { continue_process(); continue; }
            uint64_t current_pc = 0, x0 = 0;
            (void)read_reg(32, &current_pc);
            (void)read_reg(0, &x0);
            if (current_pc == end_pc + 4 && x0 == g_slot) {
                if (write_reg(32, end_pc)) current_pc = end_pc;
                else log_line("Could not rewind PC at voice-end breakpoint.");
            }
            if (current_pc == end_pc && x0 == g_slot) {
                uint32_t state = 0;
                uint64_t count = 0, cursor = 0, data = 0;
                if (read_slot_values(g_slot, &state, &count, &cursor, &data)) {
                    log_line("VOICE_END_HANDLER: state=%u cursor=%llu samples=%llu data=0x%llx",
                             state, (unsigned long long)cursor, (unsigned long long)count,
                             (unsigned long long)data);
                    g_end_handler_seen = true;
                }
                if (g_bp_end_active) { remove_breakpoint(end_pc); g_bp_end_active = false; }
                if (!g_watchpoint_active) {
                    log_line("RESULT: voice reached the end handler. The state watchpoint was rejected, so final state timing was not independently captured.");
                    g_done = true;
                }
            } else if (strstr(stop, "watch:") || strstr(stop, "awatch:") || strstr(stop, "rwatch:")) {
                uint32_t state = 0;
                uint64_t count = 0, cursor = 0, data = 0;
                if (read_slot_values(g_slot, &state, &count, &cursor, &data)) {
                    log_line("SLOT_STATE_WATCH: state=%u cursor=%llu samples=%llu data=0x%llx PC=0x%llx",
                             state, (unsigned long long)cursor, (unsigned long long)count,
                             (unsigned long long)data, (unsigned long long)current_pc);
                    if (state == 0) {
                        if (g_end_handler_seen && cursor >= count)
                            log_line("RESULT: slot stopped after its loaded sample count; normal end is consistent with this trace.");
                        else if (!g_end_handler_seen && cursor < count)
                            log_line("RESULT: slot became idle before its loaded sample count; that points to an early state stop/reinitialisation rather than a short RAW.");
                        else
                            log_line("RESULT: slot is idle; inspect cursor/sample count and PC above.");
                        g_done = true;
                    }
                }
            }
            if (!g_done && !g_cancelled) continue_process();
        }
        break;
    }
    return g_done;
}

int main(void)
{
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    (void)fsdevMountSdmc();
    (void)mkdir("sdmc:/switch", 0777);
    (void)mkdir(LOG_DIR, 0777);
    g_log = fopen(LOG_PATH, "w");

    log_line("OverlayDisp JointOut Runtime Probe");
    log_line("Target title ID 010000000000100C; expected build ID prefix 69f8ce5b22dd143a78b4e0a95863c200c0de703a.");
    log_line("Read-only diagnostic: temporary RAM breakpoints/watchpoint only; no IPS, RAW or on-disk executable is written.");
    log_line("Run on the HOME menu. The Atmosphere standalone GDB stub must be enabled on this console.");

    if (attach_debugger()) {
        bool completed = do_runtime_trace();
        if (!completed && !g_cancelled)
            log_line("Trace stopped without a complete end-state capture; any resource-size line remains useful.");
    }

    if (g_sock >= 0) {
        if (g_attached && !g_target_stopped) {
            if (!interrupt_and_wait_for_stop()) {
                log_line("CRITICAL CLEANUP WARNING: could not confirm overlayDisp is stopped.");
                log_line("No breakpoint-removal or detach command was sent while it may be running.");
                log_line("Fully power off/reboot the Switch before further HOME-menu audio tests.");
            }
        }
        if (g_attached && g_target_stopped) {
            if (g_bp_size_active || g_bp_init_active || g_bp_end_active || g_watchpoint_active)
                remove_all_breakpoints();
            char reply[RX_CAP];
            if (rsp_command("D", reply, sizeof(reply))) {
                log_line("Debugger detach: %s", reply);
                g_attached = false;
            } else {
                log_line("Debugger detach timed out; reboot the console if overlayDisp remains paused.");
            }
        }
        close(g_sock);
        g_sock = -1;
    }
    if (g_socket_initialized) {
        socketExit();
        g_socket_initialized = false;
    }

    if (g_cancelled) log_line("User cancelled with PLUS; cleanup was attempted.");
    else if (g_done) log_line("Trace completed.");
    log_line("Report path: " LOG_PATH);
    log_line("Exit with PLUS.");
    while (appletMainLoop()) {
        padUpdate(&g_pad);
        if (padGetButtonsDown(&g_pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
        svcSleepThread(100000000ULL);
    }
    if (g_log) { fclose(g_log); g_log = NULL; }
    consoleExit(NULL);
    return 0;
}
