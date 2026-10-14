#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <switch.h>
#include "converter.h"
#include "preview.h"

#define ROOT "sdmc:/SlideNX"
#define SOUNDS ROOT "/sounds"
#define SETTINGS ROOT "/settings.ini"
#define SETTINGS_TMP ROOT "/settings.ini.tmp"
#define LEGACY_IN ROOT "/IN.mp3"
#define LEGACY_OUT ROOT "/OUT.mp3"
#define OUTPUT_DIR ROOT "/output"
#define RAW_IN OUTPUT_DIR "/joint.raw"
#define RAW_OUT OUTPUT_DIR "/joint_out.raw"
#define TMP_IN OUTPUT_DIR "/joint.raw.tmp"
#define TMP_OUT OUTPUT_DIR "/joint_out.raw.tmp"
#define BACKUP_DIR ROOT "/backup"
#define BACKUP_IN BACKUP_DIR "/joint.raw"
#define BACKUP_OUT BACKUP_DIR "/joint_out.raw"
#define STATE_IN BACKUP_DIR "/joint.state"
#define STATE_OUT BACKUP_DIR "/joint_out.state"
#define TARGET_DIR "sdmc:/atmosphere/contents/010000000000100C/romfs/sound"
#define TARGET_IN TARGET_DIR "/joint.raw"
#define TARGET_OUT TARGET_DIR "/joint_out.raw"
#define PATH_MAX_NX 600
#define NAME_MAX_NX 256
#define MAX_FILES 96
#define PICKER_ROWS 14

typedef enum { UI_MAIN, UI_PICK_CONNECT, UI_PICK_DISCONNECT } UiMode;
static bool g_sd_ready;
static bool g_outputs_ready;
static int g_gain_db;
static int g_menu_cursor;
static int g_picker_cursor;
static UiMode g_ui_mode = UI_MAIN;
static size_t g_sound_count;
static char g_sound_files[MAX_FILES][NAME_MAX_NX];
static char g_connect[NAME_MAX_NX] = "";
static char g_disconnect[NAME_MAX_NX] = "";
static char g_status[256] = "Place MP3 files in sdmc:/SlideNX/sounds/.";

static void copy_name(char *destination, size_t destination_size, const char *source)
{
    size_t length;
    if (destination == NULL || destination_size == 0) return;
    if (source == NULL) { destination[0] = '\0'; return; }
    length = strlen(source);
    if (length >= destination_size) length = destination_size - 1;
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static bool regular_nonempty(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static bool ensure_dir(const char *path)
{
    struct stat st;
    if (mkdir(path, 0777) == 0) return true;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool ensure_app_dirs(void)
{
    return ensure_dir(ROOT) && ensure_dir(SOUNDS) &&
           ensure_dir(OUTPUT_DIR) && ensure_dir(BACKUP_DIR);
}

static bool ensure_target_dir(void)
{
    return ensure_dir("sdmc:/atmosphere") &&
           ensure_dir("sdmc:/atmosphere/contents") &&
           ensure_dir("sdmc:/atmosphere/contents/010000000000100C") &&
           ensure_dir("sdmc:/atmosphere/contents/010000000000100C/romfs") &&
           ensure_dir(TARGET_DIR);
}

static bool mount_sd(void)
{
    DIR *probe = opendir("sdmc:/");
    if (probe) { closedir(probe); return true; }
    Result rc = fsInitialize();
    if (R_FAILED(rc)) {
        snprintf(g_status, sizeof(g_status), "Filesystem init failed: 0x%08x.", (unsigned)rc);
        return false;
    }
    rc = fsdevMountSdmc();
    if (R_FAILED(rc)) {
        snprintf(g_status, sizeof(g_status), "SD mount failed: 0x%08x.", (unsigned)rc);
        return false;
    }
    probe = opendir("sdmc:/");
    if (!probe) { snprintf(g_status, sizeof(g_status), "SD root unavailable."); return false; }
    closedir(probe);
    return true;
}

static bool copy_file(const char *source, const char *destination)
{
    FILE *in = fopen(source, "rb");
    FILE *out;
    unsigned char buffer[8192];
    size_t n;
    bool ok = true;
    if (!in) return false;
    out = fopen(destination, "wb");
    if (!out) { fclose(in); return false; }
    while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, n, out) != n) { ok = false; break; }
    }
    if (ferror(in)) ok = false;
    if (fclose(in) != 0) ok = false;
    if (fclose(out) != 0) ok = false;
    if (!ok) (void)remove(destination);
    return ok;
}

static bool remove_if_exists(const char *path)
{
    return !exists(path) || remove(path) == 0;
}

static bool mp3_name(const char *name)
{
    const char *dot;
    if (!name || !*name || name[0] == '.') return false;
    dot = strrchr(name, '.');
    return dot && strcasecmp(dot, ".mp3") == 0;
}

static bool safe_mp3_name(const char *name)
{
    return mp3_name(name) && strchr(name, '/') == NULL && strchr(name, '\\') == NULL;
}

static bool sound_path(const char *name, char *buffer, size_t size)
{
    int n;
    if (!safe_mp3_name(name) || !buffer || size == 0) return false;
    n = snprintf(buffer, size, "%s/%s", SOUNDS, name);
    return n >= 0 && (size_t)n < size;
}

static int name_compare(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

static void scan_sounds(void)
{
    DIR *dir = opendir(SOUNDS);
    struct dirent *entry;
    g_sound_count = 0;
    if (!dir) {
        snprintf(g_status, sizeof(g_status), "Cannot open sounds folder.");
        return;
    }
    while ((entry = readdir(dir)) != NULL && g_sound_count < MAX_FILES) {
        char path[PATH_MAX_NX];
        struct stat st;
        int n;
        if (!mp3_name(entry->d_name)) continue;
        n = snprintf(path, sizeof(path), "%s/%s", SOUNDS, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) continue;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) continue;
        copy_name(g_sound_files[g_sound_count], NAME_MAX_NX, entry->d_name);
        g_sound_count++;
    }
    closedir(dir);
    qsort(g_sound_files, g_sound_count, NAME_MAX_NX, name_compare);
}

static void migrate_legacy(void)
{
    char dst[PATH_MAX_NX];
    scan_sounds();
    if (g_sound_count != 0) return;
    if (regular_nonempty(LEGACY_IN) && sound_path("IN.mp3", dst, sizeof(dst)))
        (void)copy_file(LEGACY_IN, dst);
    if (regular_nonempty(LEGACY_OUT) && sound_path("OUT.mp3", dst, sizeof(dst)))
        (void)copy_file(LEGACY_OUT, dst);
    scan_sounds();
}

static bool chosen_file_exists(const char *name)
{
    char path[PATH_MAX_NX];
    return sound_path(name, path, sizeof(path)) && regular_nonempty(path);
}

static bool save_settings(void)
{
    FILE *fp;
    if (!ensure_app_dirs()) return false;
    fp = fopen(SETTINGS_TMP, "w");
    if (!fp) return false;
    if (fprintf(fp, "connect=%s\ndisconnect=%s\ngain_db=%d\n",
                g_connect, g_disconnect, g_gain_db) < 0) {
        fclose(fp); (void)remove_if_exists(SETTINGS_TMP); return false;
    }
    if (fclose(fp) != 0) { (void)remove_if_exists(SETTINGS_TMP); return false; }
    if (!remove_if_exists(SETTINGS) || rename(SETTINGS_TMP, SETTINGS) != 0) {
        (void)remove_if_exists(SETTINGS_TMP); return false;
    }
    return true;
}

static void load_settings(void)
{
    FILE *fp = fopen(SETTINGS, "r");
    char line[384], connect[NAME_MAX_NX] = "", disconnect[NAME_MAX_NX] = "";
    int gain = 0;
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            char *eq = strchr(line, '='), *key, *value;
            size_t len;
            if (!eq) continue;
            *eq = '\0'; key = line; value = eq + 1;
            while (isspace((unsigned char)*key)) key++;
            while (isspace((unsigned char)*value)) value++;
            len = strlen(value);
            while (len && isspace((unsigned char)value[len - 1])) value[--len] = '\0';
            if (!strcasecmp(key, "connect") && safe_mp3_name(value))
                copy_name(connect, sizeof(connect), value);
            else if (!strcasecmp(key, "disconnect") && safe_mp3_name(value))
                copy_name(disconnect, sizeof(disconnect), value);
            else if (!strcasecmp(key, "gain_db")) {
                char *end = NULL; long v = strtol(value, &end, 10);
                if (end != value && *end == '\0' && v >= -12 && v <= 6) gain = (int)v;
            }
        }
        fclose(fp);
    }
    g_gain_db = gain;
    if (chosen_file_exists(connect)) copy_name(g_connect, sizeof(g_connect), connect);
    else if (g_sound_count) copy_name(g_connect, sizeof(g_connect), g_sound_files[0]);
    else g_connect[0] = '\0';

    if (chosen_file_exists(disconnect)) copy_name(g_disconnect, sizeof(g_disconnect), disconnect);
    else if (g_sound_count) copy_name(g_disconnect, sizeof(g_disconnect), g_sound_files[g_sound_count > 1 ? 1 : 0]);
    else g_disconnect[0] = '\0';

    g_connect[sizeof(g_connect) - 1] = '\0';
    g_disconnect[sizeof(g_disconnect) - 1] = '\0';
}

static void refresh_all(void)
{
    if (!ensure_app_dirs()) { snprintf(g_status, sizeof(g_status), "Could not create SlideNX folders."); return; }
    migrate_legacy();
    load_settings();
    g_outputs_ready = regular_nonempty(RAW_IN) && regular_nonempty(RAW_OUT);
    snprintf(g_status, sizeof(g_status), "Found %lu MP3 file(s). Settings loaded.",
             (unsigned long)g_sound_count);
}

static void open_picker(bool connect)
{
    scan_sounds();
    if (!g_sound_count) {
        snprintf(g_status, sizeof(g_status), "No MP3 files in sdmc:/SlideNX/sounds/.");
        return;
    }
    const char *current = connect ? g_connect : g_disconnect;
    g_picker_cursor = 0;
    for (size_t i = 0; i < g_sound_count; i++)
        if (!strcasecmp(g_sound_files[i], current)) { g_picker_cursor = (int)i; break; }
    g_ui_mode = connect ? UI_PICK_CONNECT : UI_PICK_DISCONNECT;
}

static void accept_picker(void)
{
    if (!g_sound_count || g_picker_cursor < 0 || (size_t)g_picker_cursor >= g_sound_count)
        return;
    if (g_ui_mode == UI_PICK_CONNECT) {
        copy_name(g_connect, sizeof(g_connect), g_sound_files[g_picker_cursor]);
        g_connect[sizeof(g_connect) - 1] = '\0';
        snprintf(g_status, sizeof(g_status), "Connect sound set: %.170s", g_connect);
    } else {
        copy_name(g_disconnect, sizeof(g_disconnect), g_sound_files[g_picker_cursor]);
        g_disconnect[sizeof(g_disconnect) - 1] = '\0';
        snprintf(g_status, sizeof(g_status), "Disconnect sound set: %.155s", g_disconnect);
    }
    if (!save_settings())
        snprintf(g_status, sizeof(g_status), "Selection changed, but settings.ini could not be saved.");
    g_outputs_ready = false;
    g_ui_mode = UI_MAIN;
}

static void convert_both(void)
{
    char in_path[PATH_MAX_NX], out_path[PATH_MAX_NX], error[192] = "";
    g_outputs_ready = false;
    if (!sound_path(g_connect, in_path, sizeof(in_path)) ||
        !sound_path(g_disconnect, out_path, sizeof(out_path)) ||
        !regular_nonempty(in_path) || !regular_nonempty(out_path)) {
        snprintf(g_status, sizeof(g_status), "Choose valid connect and disconnect MP3s first.");
        return;
    }
    if (!ensure_app_dirs()) { snprintf(g_status, sizeof(g_status), "Could not create output folder."); return; }
    (void)remove_if_exists(TMP_IN); (void)remove_if_exists(TMP_OUT);

    if (convert_mp3_to_raw(in_path, TMP_IN, g_gain_db, error, sizeof(error)) != 0) {
        snprintf(g_status, sizeof(g_status), "Connect conversion failed: %.170s", error);
        (void)remove_if_exists(TMP_IN); return;
    }
    error[0] = '\0';
    if (convert_mp3_to_raw(out_path, TMP_OUT, g_gain_db, error, sizeof(error)) != 0) {
        snprintf(g_status, sizeof(g_status), "Disconnect conversion failed: %.160s", error);
        (void)remove_if_exists(TMP_IN); (void)remove_if_exists(TMP_OUT); return;
    }
    if (!regular_nonempty(TMP_IN) || !regular_nonempty(TMP_OUT)) {
        snprintf(g_status, sizeof(g_status), "Empty RAW output. LayeredFS was not changed.");
        (void)remove_if_exists(TMP_IN); (void)remove_if_exists(TMP_OUT); return;
    }
    if (!remove_if_exists(RAW_IN) || rename(TMP_IN, RAW_IN) != 0 ||
        !remove_if_exists(RAW_OUT) || rename(TMP_OUT, RAW_OUT) != 0) {
        snprintf(g_status, sizeof(g_status), "Could not save both RAW files. LayeredFS was not changed.");
        (void)remove_if_exists(TMP_IN); (void)remove_if_exists(TMP_OUT); return;
    }
    g_outputs_ready = true;
    (void)save_settings();
    snprintf(g_status, sizeof(g_status), "Auto-normalized both sounds; manual gain %s%d dB. Not installed yet.",
             g_gain_db > 0 ? "+" : "", g_gain_db);
}

static void preview_selected(bool connecting)
{
    const char *name = connecting ? g_connect : g_disconnect;
    char path[PATH_MAX_NX];
    char error[192] = "";

    if (!sound_path(name, path, sizeof(path)) || !regular_nonempty(path)) {
        snprintf(g_status, sizeof(g_status), "Select a valid MP3 before previewing.");
        return;
    }

    snprintf(g_status, sizeof(g_status), "Playing an 8-second preview of %.140s.", name);
    if (preview_mp3(path, g_gain_db, error, sizeof(error)) == 0) {
        snprintf(g_status, sizeof(g_status), "Preview finished: %.170s", name);
    } else {
        snprintf(g_status, sizeof(g_status), "Preview failed: %.180s", error);
    }
}

static bool baseline(const char *target, const char *backup, const char *state)
{
    FILE *fp = fopen(state, "r");
    if (fp) { fclose(fp); return !exists(target) || regular_nonempty(target); }
    if (exists(target) && !regular_nonempty(target)) {
        snprintf(g_status, sizeof(g_status), "Refusing to replace non-file target: %.170s", target);
        return false;
    }
    if (regular_nonempty(target)) {
        if (!copy_file(target, backup) || !regular_nonempty(backup)) {
            snprintf(g_status, sizeof(g_status), "Could not back up target: %.170s", target);
            return false;
        }
        fp = fopen(state, "w"); if (!fp) return false;
        fputs("present\n", fp); return fclose(fp) == 0;
    }
    fp = fopen(state, "w"); if (!fp) return false;
    fputs("missing\n", fp); return fclose(fp) == 0;
}

static bool read_state(const char *path, char *value, size_t size)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return false;
    if (!fgets(value, (int)size, fp)) { fclose(fp); return false; }
    fclose(fp); return true;
}

static bool restore_one(const char *target, const char *backup, const char *state)
{
    char value[24] = "", temporary[PATH_MAX_NX];
    if (!read_state(state, value, sizeof(value))) return false;
    if (!strncmp(value, "present", 7)) {
        if (!regular_nonempty(backup) ||
            snprintf(temporary, sizeof(temporary), "%s.slidenx-restore-tmp", target) >= (int)sizeof(temporary))
            return false;
        (void)remove_if_exists(temporary);
        if (!copy_file(backup, temporary)) return false;
        if (!remove_if_exists(target) || rename(temporary, target) != 0) {
            (void)remove_if_exists(temporary); return false;
        }
        return true;
    }
    if (!strncmp(value, "missing", 7)) return remove_if_exists(target);
    return false;
}

static bool rollback_target(const char *target, const char *previous, bool existed)
{
    if (!remove_if_exists(target)) return false;
    if (!existed) return true;
    return regular_nonempty(previous) && rename(previous, target) == 0;
}

static void install_both(void)
{
    const char *tmp_in = TARGET_IN ".slidenx-new";
    const char *tmp_out = TARGET_OUT ".slidenx-new";
    const char *prev_in = TARGET_IN ".slidenx-prev";
    const char *prev_out = TARGET_OUT ".slidenx-prev";
    bool old_in, old_out;

    if (!g_outputs_ready || !regular_nonempty(RAW_IN) || !regular_nonempty(RAW_OUT)) {
        snprintf(g_status, sizeof(g_status), "Convert both selected MP3s before installing."); return;
    }
    if (!ensure_app_dirs() || !ensure_target_dir()) {
        snprintf(g_status, sizeof(g_status), "Could not create LayeredFS folders."); return;
    }
    if (!baseline(TARGET_IN, BACKUP_IN, STATE_IN) || !baseline(TARGET_OUT, BACKUP_OUT, STATE_OUT)) {
        snprintf(g_status, sizeof(g_status), "Baseline backup failed; installation cancelled."); return;
    }
    if ((exists(TARGET_IN) && !regular_nonempty(TARGET_IN)) ||
        (exists(TARGET_OUT) && !regular_nonempty(TARGET_OUT))) {
        snprintf(g_status, sizeof(g_status), "Refusing to replace non-file target."); return;
    }

    old_in = regular_nonempty(TARGET_IN);
    old_out = regular_nonempty(TARGET_OUT);
    (void)remove_if_exists(tmp_in); (void)remove_if_exists(tmp_out);
    (void)remove_if_exists(prev_in); (void)remove_if_exists(prev_out);
    if (!copy_file(RAW_IN, tmp_in) || !regular_nonempty(tmp_in) ||
        !copy_file(RAW_OUT, tmp_out) || !regular_nonempty(tmp_out)) {
        (void)remove_if_exists(tmp_in); (void)remove_if_exists(tmp_out);
        snprintf(g_status, sizeof(g_status), "Could not stage outputs; old sounds untouched."); return;
    }
    if ((old_in && !copy_file(TARGET_IN, prev_in)) || (old_out && !copy_file(TARGET_OUT, prev_out))) {
        (void)remove_if_exists(tmp_in); (void)remove_if_exists(tmp_out);
        (void)remove_if_exists(prev_in); (void)remove_if_exists(prev_out);
        snprintf(g_status, sizeof(g_status), "Could not prepare rollback copies; nothing installed."); return;
    }

    if (!remove_if_exists(TARGET_IN) || !remove_if_exists(TARGET_OUT)) {
        (void)rollback_target(TARGET_IN, prev_in, old_in);
        (void)rollback_target(TARGET_OUT, prev_out, old_out);
        (void)remove_if_exists(tmp_in); (void)remove_if_exists(tmp_out);
        snprintf(g_status, sizeof(g_status), "Target staging failed; rollback attempted."); return;
    }
    if (rename(tmp_in, TARGET_IN) != 0) {
        (void)rollback_target(TARGET_IN, prev_in, old_in);
        (void)rollback_target(TARGET_OUT, prev_out, old_out);
        (void)remove_if_exists(tmp_out);
        snprintf(g_status, sizeof(g_status), "Could not install joint.raw; rollback attempted."); return;
    }
    if (rename(tmp_out, TARGET_OUT) != 0) {
        (void)rollback_target(TARGET_IN, prev_in, old_in);
        (void)rollback_target(TARGET_OUT, prev_out, old_out);
        (void)remove_if_exists(tmp_out);
        snprintf(g_status, sizeof(g_status), "Could not install joint_out.raw; rollback attempted."); return;
    }
    (void)remove_if_exists(prev_in); (void)remove_if_exists(prev_out);
    snprintf(g_status, sizeof(g_status), "Both sounds installed. Reboot to test.");
}

static void restore_baseline(void)
{
    bool in_ok, out_ok;
    if (!exists(STATE_IN) || !exists(STATE_OUT)) {
        snprintf(g_status, sizeof(g_status), "No baseline backup. Install first to enable restore."); return;
    }
    if (!ensure_target_dir()) { snprintf(g_status, sizeof(g_status), "LayeredFS folder unavailable."); return; }
    in_ok = restore_one(TARGET_IN, BACKUP_IN, STATE_IN);
    out_ok = restore_one(TARGET_OUT, BACKUP_OUT, STATE_OUT);
    snprintf(g_status, sizeof(g_status), "Restore: joint.raw %s; joint_out.raw %s.",
             in_ok ? "OK" : "FAILED", out_ok ? "OK" : "FAILED");
}

static void adjust_gain(int delta)
{
    g_gain_db += delta;
    if (g_gain_db < -12) g_gain_db = -12;
    if (g_gain_db > 6) g_gain_db = 6;
    g_outputs_ready = false;
    if (!save_settings())
        snprintf(g_status, sizeof(g_status), "Gain %s%d dB; failed to save settings.", g_gain_db > 0 ? "+" : "", g_gain_db);
    else
        snprintf(g_status, sizeof(g_status), "Conversion gain is %s%d dB.", g_gain_db > 0 ? "+" : "", g_gain_db);
}

static void draw_picker(void)
{
    size_t start = g_picker_cursor >= PICKER_ROWS ? (size_t)(g_picker_cursor - PICKER_ROWS + 1) : 0;
    consoleClear();
    printf("SlideNX Reborn 2.3 - Select %s sound\n\n",
           g_ui_mode == UI_PICK_CONNECT ? "CONNECT" : "DISCONNECT");
    printf("Folder: sdmc:/SlideNX/sounds/\nUP/DOWN to navigate; A select; B cancel.\n");
    printf("Found %lu MP3 file(s).\n\n", (unsigned long)g_sound_count);
    for (size_t i = start; i < g_sound_count && i < start + PICKER_ROWS; i++)
        printf("%c %s\n", (int)i == g_picker_cursor ? '>' : ' ', g_sound_files[i]);
    printf("\nA: Choose  B: Cancel\n");
    consoleUpdate(NULL);
}

static void draw_main(void)
{
    const char *items[] = {"Choose connect sound", "Choose disconnect sound",
        "Preview connect sound", "Preview disconnect sound", "Conversion gain",
        "Convert selected sounds", "Install to LayeredFS", "Restore previous sounds",
        "Refresh MP3 list", "Exit"};
    char gain_text[48];
    snprintf(gain_text, sizeof(gain_text), "Conversion gain: %s%d dB",
             g_gain_db > 0 ? "+" : "", g_gain_db);

    consoleClear();
    printf("SlideNX Reborn 2.3 - Sound Studio\n");
    printf("=================================\n\n");
    printf("CONNECT:    %s\n", g_connect[0] ? g_connect : "(not selected)");
    printf("DISCONNECT: %s\n\n", g_disconnect[0] ? g_disconnect : "(not selected)");
    printf("MP3 folder: sdmc:/SlideNX/sounds/\n");
    printf("Output RAW: sdmc:/SlideNX/output/\n");
    printf("Format: mono PCM 16-bit LE, 48000 Hz, headerless\n");
    printf("Auto level: RMS target -18 dBFS; peak ceiling -3 dBFS\n\n");
    for (int i = 0; i < 10; i++)
        printf("%c %s\n", i == g_menu_cursor ? '>' : ' ', i == 4 ? gain_text : items[i]);
    printf("\nStatus: %s\n\n", g_status);
    printf("UP/DOWN: Navigate   A: Select   LEFT/RIGHT: Gain +/-1 dB on gain row\n");
    printf("Y: Install   X: Restore   B: Refresh list   PLUS: Exit\n");
    consoleUpdate(NULL);
}

int main(void)
{
    PadState pad;
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    g_sd_ready = mount_sd();
    if (g_sd_ready) refresh_all();

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);

        if (down & HidNpadButton_Plus) {
            if (g_ui_mode != UI_MAIN) {
                g_ui_mode = UI_MAIN;
                snprintf(g_status, sizeof(g_status), "Picker cancelled.");
            } else break;
        }

        if (g_ui_mode != UI_MAIN) {
            if (down & HidNpadButton_B) {
                g_ui_mode = UI_MAIN;
                snprintf(g_status, sizeof(g_status), "Picker cancelled.");
            } else if (down & HidNpadButton_Up) {
                if (g_picker_cursor > 0) g_picker_cursor--;
            } else if (down & HidNpadButton_Down) {
                if ((size_t)(g_picker_cursor + 1) < g_sound_count) g_picker_cursor++;
            } else if (down & HidNpadButton_A) accept_picker();
            if (g_ui_mode != UI_MAIN) draw_picker();
            else draw_main();
        } else {
            if (down & HidNpadButton_Up) { if (g_menu_cursor > 0) g_menu_cursor--; }
            if (down & HidNpadButton_Down) { if (g_menu_cursor < 9) g_menu_cursor++; }
            if (g_menu_cursor == 4 && (down & HidNpadButton_Left)) adjust_gain(-1);
            if (g_menu_cursor == 4 && (down & HidNpadButton_Right)) adjust_gain(1);

            if (down & HidNpadButton_B) { if (g_sd_ready) refresh_all(); }
            if (down & HidNpadButton_X) { if (g_sd_ready) restore_baseline(); }
            if (down & HidNpadButton_Y) { if (g_sd_ready) install_both(); }
            if (down & HidNpadButton_A) {
                if (!g_sd_ready) snprintf(g_status, sizeof(g_status), "SD card unavailable.");
                else switch (g_menu_cursor) {
                    case 0: open_picker(true); break;
                    case 1: open_picker(false); break;
                    case 2: preview_selected(true); break;
                    case 3: preview_selected(false); break;
                    case 4: adjust_gain(1); break;
                    case 5: convert_both(); break;
                    case 6: install_both(); break;
                    case 7: restore_baseline(); break;
                    case 8: refresh_all(); break;
                    case 9: consoleExit(NULL); return 0;
                }
            }
            if (g_ui_mode == UI_MAIN) draw_main();
        }
        svcSleepThread(16000000ULL);
    }
    consoleExit(NULL);
    return 0;
}
