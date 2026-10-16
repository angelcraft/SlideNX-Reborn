#include <switch.h>
#include <arpa/inet.h>
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
static char g_pending_stop[RX_CAP];

static void log_line(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("%s\n", line);
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

/* Read one RSP response packet. A timeout before '$' is not an error. */
static int read_packet(char *out, size_t cap)
{
    char c, hash1, hash2;
    size_t used = 0;
    int rc;
    if (!cap) return -1;
    for (;;) {
        rc = read_byte(&c);
        if (rc <= 0) return rc;
        if (c == '$') break;
        /* Ignore RSP acknowledgements and asynchronous non-packet bytes. */
    }
    for (;;) {
        rc = read_byte(&c);
        if (rc <= 0) return -1;
        if (c == '#') break;
        if (used + 1 < cap) out[used++] = c;
    }
    if (read_byte(&hash1) != 1 || read_byte(&hash2) != 1) return -1;
    out[used] = '\0';
    (void)hash1;
    (void)hash2;
    (void)send_all("+", 1);
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

static bool rsp_monitor_get_info(char *text, size_t cap)
{
    static const char encoded[] = "6d6f6e69746f722067657420696e666f";
    char cmd[96];
    size_t used = 0;
    snprintf(cmd, sizeof(cmd), "qRcmd,%s", encoded);
    if (!rsp_send(cmd)) return false;
    for (unsigned packets = 0; packets < 128 && used + 1 < cap; ++packets) {
        char reply[RX_CAP];
        if (read_packet(reply, sizeof(reply)) != 1) return false;
        if (strcmp(reply, "OK") == 0) {
            text[used] = '\0';
            return true;
        }
        if (reply[0] == 'O') {
            const char *p = reply + 1;
            while (p[0] && p[1] && used + 1 < cap) {
                char hex[3] = {p[0], p[1], 0};
                text[used++] = (char)strtoul(hex, NULL, 16);
                p += 2;
            }
            text[used] = '\0';
        } else if (reply[0] == 'E') {
            return false;
        }
    }
    return false;
}

static bool parse_module_base(const char *text, uint64_t *base_out)
{
    const char *p = strstr(text, "overlayDisp module base:");
    if (!p) return false;
    p = strchr(p, '0');
    if (!p) return false;
    *base_out = strtoull(p, NULL, 16);
    return *base_out != 0;
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
    char cmd[96], reply[RX_CAP];
    snprintf(cmd, sizeof(cmd), "vCont;c:p%x.-1", g_pid);
    if (!rsp_command(cmd, reply, sizeof(reply))) {
        log_line("No reply to continue command.");
        return false;
    }
    if (strcmp(reply, "OK") == 0) return true;
    if (reply[0] == 'T' || reply[0] == 'S') {
        snprintf(g_pending_stop, sizeof(g_pending_stop), "%s", reply);
        return true;
    }
    log_line("Continue reply: %s", reply);
    return false;
}

static void remove_all_breakpoints(void)
{
    if (g_bp_size_active) { remove_breakpoint(g_base + OFF_RESOURCE_SIZE_LOAD); g_bp_size_active = false; }
    if (g_bp_init_active) { remove_breakpoint(g_base + OFF_SLOT_INIT_RETURN); g_bp_init_active = false; }
    if (g_bp_end_active) { remove_breakpoint(g_base + OFF_VOICE_END); g_bp_end_active = false; }
    if (g_watchpoint_active && g_slot) { remove_write_watchpoint(g_slot); g_watchpoint_active = false; }
}

static bool attach_debugger(void)
{
    struct sockaddr_in sa;
    struct timeval tv;
    char reply[RX_CAP], xml[XML_CAP], info[RX_CAP];
    Result rc = socketInitializeDefault();
    if (R_FAILED(rc)) {
        log_line("socketInitializeDefault failed: 0x%08x", (unsigned)rc);
        return false;
    }
    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock < 0) { log_line("socket() failed: %d", errno); return false; }
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    (void)setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(GDB_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(g_sock, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        log_line("Could not connect to localhost:%d (errno=%d). Check Atmosphere standalone GDB stub settings.", GDB_PORT, errno);
        return false;
    }
    log_line("Connected to Atmosphere GDB stub on loopback:%d.", GDB_PORT);
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
    if (!rsp_command(reply, reply, sizeof(reply))) {
        log_line("vAttach failed (no RSP response).");
        return false;
    }
    log_line("Attached: %.160s", reply);
    if (!rsp_monitor_get_info(info, sizeof(info))) {
        log_line("monitor get info failed; cannot reliably locate ASLR module base.");
        return false;
    }
    if (!parse_module_base(info, &g_base)) {
        log_line("monitor get info did not return overlayDisp module base. Info follows:\n%s", info);
        return false;
    }
    log_line("overlayDisp module base: 0x%llx", (unsigned long long)g_base);
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

/* Receive asynchronous RSP stop packets while overlayDisp is running. */
static bool wait_stop(char *reply, size_t cap)
{
    if (g_pending_stop[0]) {
        snprintf(reply, cap, "%s", g_pending_stop);
        g_pending_stop[0] = '\0';
        return true;
    }
    while (!g_cancelled) {
        int rc = read_packet(reply, cap);
        if (rc == 1) return true;
        if (rc < 0) {
            log_line("Debugger connection closed or malformed RSP packet.");
            return false;
        }
        try_cancel_from_controller();
    }
    return false;
}

static bool on_breakpoint(uint64_t expected_pc, char *label, size_t label_cap,
                          uint64_t *x8, uint64_t *x9, uint64_t *x21,
                          uint64_t *x23, uint64_t *pc)
{
    if (!read_reg(32, pc) || !read_reg(8, x8) || !read_reg(9, x9) ||
        !read_reg(0x15, x21) || !read_reg(0x17, x23)) return false;
    if (*pc != expected_pc) return false;
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
        if (g_cancelled) {
            const char interrupt = 3;
            (void)send_all(&interrupt, 1);
            char stop[RX_CAP];
            (void)read_packet(stop, sizeof(stop));
        }
        if (g_bp_size_active || g_bp_init_active || g_bp_end_active || g_watchpoint_active)
            remove_all_breakpoints();
        char reply[RX_CAP];
        if (rsp_command("D", reply, sizeof(reply)))
            log_line("Debugger detach: %s", reply);
        close(g_sock);
        g_sock = -1;
        socketExit();
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
