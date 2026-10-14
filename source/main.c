#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <switch.h>
#include "converter.h"

#define SLIDENX_DIR "sdmc:/SlideNX"
#define INPUT_IN "sdmc:/SlideNX/IN.mp3"
#define INPUT_OUT "sdmc:/SlideNX/OUT.mp3"
#define OUTPUT_DIR "sdmc:/SlideNX/output"
#define OUTPUT_IN "sdmc:/SlideNX/output/joint.raw"
#define OUTPUT_OUT "sdmc:/SlideNX/output/joint_out.raw"
#define OUTPUT_IN_TMP "sdmc:/SlideNX/output/joint.raw.tmp"
#define OUTPUT_OUT_TMP "sdmc:/SlideNX/output/joint_out.raw.tmp"
#define BACKUP_DIR "sdmc:/SlideNX/backup"
#define TARGET_DIR "sdmc:/atmosphere/contents/010000000000100C/romfs/sound"
#define TARGET_IN "sdmc:/atmosphere/contents/010000000000100C/romfs/sound/joint.raw"
#define TARGET_OUT "sdmc:/atmosphere/contents/010000000000100C/romfs/sound/joint_out.raw"
#define BACKUP_IN "sdmc:/SlideNX/backup/joint.raw"
#define BACKUP_OUT "sdmc:/SlideNX/backup/joint_out.raw"
#define STATE_IN "sdmc:/SlideNX/backup/joint.state"
#define STATE_OUT "sdmc:/SlideNX/backup/joint_out.state"
#define STATUS_LEN 256

static bool g_sd_ready;
static bool g_outputs_ready;
static char g_status[STATUS_LEN] = "Put IN.mp3 and OUT.mp3 in sdmc:/SlideNX/.";

static bool path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static bool is_regular_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static bool ensure_dir(const char *path)
{
    struct stat st;
    if (mkdir(path, 0777) == 0)
        return true;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool ensure_slide_dirs(void)
{
    return ensure_dir(SLIDENX_DIR) && ensure_dir(OUTPUT_DIR) && ensure_dir(BACKUP_DIR);
}

static bool ensure_target_dir(void)
{
    return ensure_dir("sdmc:/atmosphere") &&
           ensure_dir("sdmc:/atmosphere/contents") &&
           ensure_dir("sdmc:/atmosphere/contents/010000000000100C") &&
           ensure_dir("sdmc:/atmosphere/contents/010000000000100C/romfs") &&
           ensure_dir(TARGET_DIR);
}

static bool mount_sd_if_needed(void)
{
    DIR *probe = opendir("sdmc:/");
    if (probe != NULL) {
        closedir(probe);
        return true;
    }

    Result rc = fsInitialize();
    if (R_FAILED(rc)) {
        snprintf(g_status, sizeof(g_status), "Filesystem init failed (0x%08x).", (unsigned)rc);
        return false;
    }
    rc = fsdevMountSdmc();
    if (R_FAILED(rc)) {
        snprintf(g_status, sizeof(g_status), "SD mount failed (0x%08x).", (unsigned)rc);
        return false;
    }
    probe = opendir("sdmc:/");
    if (probe == NULL) {
        snprintf(g_status, sizeof(g_status), "SD root is not accessible.");
        return false;
    }
    closedir(probe);
    return true;
}

static bool copy_file(const char *source, const char *destination)
{
    FILE *in = fopen(source, "rb");
    FILE *out;
    unsigned char buffer[8192];
    size_t got;
    bool ok = true;

    if (in == NULL)
        return false;
    out = fopen(destination, "wb");
    if (out == NULL) {
        fclose(in);
        return false;
    }
    while ((got = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, got, out) != got) {
            ok = false;
            break;
        }
    }
    if (ferror(in))
        ok = false;
    if (fclose(in) != 0)
        ok = false;
    if (fclose(out) != 0)
        ok = false;
    if (!ok)
        (void)remove(destination);
    return ok;
}

static bool remove_if_present(const char *path)
{
    if (!path_exists(path))
        return true;
    return remove(path) == 0;
}

static bool write_state(const char *path, const char *value)
{
    FILE *file = fopen(path, "w");
    if (file == NULL)
        return false;
    if (fprintf(file, "%s\n", value) < 0) {
        fclose(file);
        (void)remove(path);
        return false;
    }
    if (fclose(file) != 0) {
        (void)remove(path);
        return false;
    }
    return true;
}

/* Capture the state that existed before SlideNX first installs anything. */
static bool ensure_baseline(const char *target, const char *backup, const char *state)
{
    FILE *file = fopen(state, "r");
    if (file != NULL) {
        fclose(file);
        if (path_exists(target) && !is_regular_file(target)) {
            snprintf(g_status, sizeof(g_status), "Target is not a regular file: %.190s", target);
            return false;
        }
        return true;
    }

    if (path_exists(target) && !is_regular_file(target)) {
        snprintf(g_status, sizeof(g_status), "Refusing to replace non-file target: %.175s", target);
        return false;
    }
    if (is_regular_file(target)) {
        if (!copy_file(target, backup) || !is_regular_file(backup)) {
            snprintf(g_status, sizeof(g_status), "Could not back up target: %.190s", target);
            return false;
        }
        if (!write_state(state, "present")) {
            snprintf(g_status, sizeof(g_status), "Could not record backup state.");
            return false;
        }
        return true;
    }
    if (!write_state(state, "missing")) {
        snprintf(g_status, sizeof(g_status), "Could not record original LayeredFS state.");
        return false;
    }
    return true;
}

static bool restore_one(const char *target, const char *backup, const char *state)
{
    FILE *file = fopen(state, "r");
    char value[24] = {0};
    char temporary[600];

    if (file == NULL)
        return false;
    if (fgets(value, sizeof(value), file) == NULL) {
        fclose(file);
        return false;
    }
    fclose(file);

    if (strncmp(value, "present", 7) == 0) {
        if (!is_regular_file(backup))
            return false;
        if (snprintf(temporary, sizeof(temporary), "%s.slidenx-restore-tmp", target) >= (int)sizeof(temporary))
            return false;
        (void)remove_if_present(temporary);
        if (!copy_file(backup, temporary))
            return false;
        if (!remove_if_present(target) || rename(temporary, target) != 0) {
            (void)remove_if_present(temporary);
            return false;
        }
        return true;
    }
    if (strncmp(value, "missing", 7) == 0)
        return remove_if_present(target);
    return false;
}

static bool rollback_target(const char *target, const char *previous, bool had_previous)
{
    if (!remove_if_present(target))
        return false;
    if (!had_previous)
        return true;
    if (!is_regular_file(previous))
        return false;
    return rename(previous, target) == 0;
}

static void refresh_status(void)
{
    snprintf(g_status, sizeof(g_status),
             "IN.mp3: %s | OUT.mp3: %s | Converted pair: %s",
             is_regular_file(INPUT_IN) ? "OK" : "MISSING",
             is_regular_file(INPUT_OUT) ? "OK" : "MISSING",
             is_regular_file(OUTPUT_IN) && is_regular_file(OUTPUT_OUT) ? "YES" : "NO");
}

static void convert_both(void)
{
    char error[192] = {0};
    g_outputs_ready = false;

    if (!is_regular_file(INPUT_IN) || !is_regular_file(INPUT_OUT)) {
        snprintf(g_status, sizeof(g_status), "Add both IN.mp3 and OUT.mp3 to sdmc:/SlideNX first.");
        return;
    }
    if (!ensure_slide_dirs()) {
        snprintf(g_status, sizeof(g_status), "Could not create SlideNX output/backup folders.");
        return;
    }

    (void)remove_if_present(OUTPUT_IN_TMP);
    (void)remove_if_present(OUTPUT_OUT_TMP);
    if (convert_mp3_to_raw(INPUT_IN, OUTPUT_IN_TMP, error, sizeof(error)) != 0) {
        snprintf(g_status, sizeof(g_status), "IN.mp3 conversion failed: %.175s", error);
        (void)remove_if_present(OUTPUT_IN_TMP);
        return;
    }

    error[0] = '\0';
    if (convert_mp3_to_raw(INPUT_OUT, OUTPUT_OUT_TMP, error, sizeof(error)) != 0) {
        snprintf(g_status, sizeof(g_status), "OUT.mp3 conversion failed: %.165s", error);
        (void)remove_if_present(OUTPUT_IN_TMP);
        (void)remove_if_present(OUTPUT_OUT_TMP);
        return;
    }

    if (!is_regular_file(OUTPUT_IN_TMP) || !is_regular_file(OUTPUT_OUT_TMP)) {
        snprintf(g_status, sizeof(g_status), "Converter created an empty file; LayeredFS was not changed.");
        (void)remove_if_present(OUTPUT_IN_TMP);
        (void)remove_if_present(OUTPUT_OUT_TMP);
        return;
    }

    if (!copy_file(OUTPUT_IN_TMP, OUTPUT_IN) || !is_regular_file(OUTPUT_IN) ||
        !copy_file(OUTPUT_OUT_TMP, OUTPUT_OUT) || !is_regular_file(OUTPUT_OUT)) {
        snprintf(g_status, sizeof(g_status), "Could not save both converted files. LayeredFS was not changed.");
        (void)remove_if_present(OUTPUT_IN_TMP);
        (void)remove_if_present(OUTPUT_OUT_TMP);
        return;
    }

    (void)remove_if_present(OUTPUT_IN_TMP);
    (void)remove_if_present(OUTPUT_OUT_TMP);
    g_outputs_ready = true;
    snprintf(g_status, sizeof(g_status), "Converted both to mono PCM 16-bit LE, 48000 Hz.");
}

static void install_both(void)
{
    const char *temp_in = "sdmc:/atmosphere/contents/010000000000100C/romfs/sound/joint.raw.slidenx-new";
    const char *temp_out = "sdmc:/atmosphere/contents/010000000000100C/romfs/sound/joint_out.raw.slidenx-new";
    const char *prev_in = "sdmc:/atmosphere/contents/010000000000100C/romfs/sound/joint.raw.slidenx-prev";
    const char *prev_out = "sdmc:/atmosphere/contents/010000000000100C/romfs/sound/joint_out.raw.slidenx-prev";
    bool had_in;
    bool had_out;

    if (!g_outputs_ready || !is_regular_file(OUTPUT_IN) || !is_regular_file(OUTPUT_OUT)) {
        snprintf(g_status, sizeof(g_status), "Press A to convert both MP3s before installing.");
        return;
    }
    if (!ensure_slide_dirs() || !ensure_target_dir()) {
        snprintf(g_status, sizeof(g_status), "Could not create LayeredFS folders.");
        return;
    }
    if (!ensure_baseline(TARGET_IN, BACKUP_IN, STATE_IN) ||
        !ensure_baseline(TARGET_OUT, BACKUP_OUT, STATE_OUT))
        return;

    if ((path_exists(TARGET_IN) && !is_regular_file(TARGET_IN)) ||
        (path_exists(TARGET_OUT) && !is_regular_file(TARGET_OUT))) {
        snprintf(g_status, sizeof(g_status), "Refusing to replace a target that is not a regular file.");
        return;
    }

    had_in = is_regular_file(TARGET_IN);
    had_out = is_regular_file(TARGET_OUT);
    (void)remove_if_present(temp_in);
    (void)remove_if_present(temp_out);
    (void)remove_if_present(prev_in);
    (void)remove_if_present(prev_out);

    if (!copy_file(OUTPUT_IN, temp_in) || !is_regular_file(temp_in) ||
        !copy_file(OUTPUT_OUT, temp_out) || !is_regular_file(temp_out)) {
        (void)remove_if_present(temp_in);
        (void)remove_if_present(temp_out);
        snprintf(g_status, sizeof(g_status), "Could not stage both sounds; existing files were not changed.");
        return;
    }
    if ((had_in && !copy_file(TARGET_IN, prev_in)) ||
        (had_out && !copy_file(TARGET_OUT, prev_out))) {
        (void)remove_if_present(temp_in);
        (void)remove_if_present(temp_out);
        (void)remove_if_present(prev_in);
        (void)remove_if_present(prev_out);
        snprintf(g_status, sizeof(g_status), "Could not create temporary rollback copies.");
        return;
    }

    if (!remove_if_present(TARGET_IN) || !remove_if_present(TARGET_OUT)) {
        (void)rollback_target(TARGET_IN, prev_in, had_in);
        (void)rollback_target(TARGET_OUT, prev_out, had_out);
        (void)remove_if_present(temp_in);
        (void)remove_if_present(temp_out);
        snprintf(g_status, sizeof(g_status), "Could not prepare target files; rollback attempted.");
        return;
    }

    if (rename(temp_in, TARGET_IN) != 0) {
        (void)rollback_target(TARGET_IN, prev_in, had_in);
        (void)rollback_target(TARGET_OUT, prev_out, had_out);
        (void)remove_if_present(temp_out);
        snprintf(g_status, sizeof(g_status), "Could not install joint.raw (errno %d); rollback attempted.", errno);
        return;
    }
    if (rename(temp_out, TARGET_OUT) != 0) {
        (void)rollback_target(TARGET_IN, prev_in, had_in);
        (void)rollback_target(TARGET_OUT, prev_out, had_out);
        (void)remove_if_present(temp_out);
        snprintf(g_status, sizeof(g_status), "Could not install joint_out.raw (errno %d); rollback attempted.", errno);
        return;
    }

    (void)remove_if_present(prev_in);
    (void)remove_if_present(prev_out);
    snprintf(g_status, sizeof(g_status), "Installed both sounds to overlayDisp. Reboot to test.");
}

static void restore_baseline(void)
{
    bool has_in = path_exists(STATE_IN);
    bool has_out = path_exists(STATE_OUT);
    bool in_ok;
    bool out_ok;

    if (!has_in || !has_out) {
        snprintf(g_status, sizeof(g_status), "No complete baseline. Restore is available after first install.");
        return;
    }
    if (!ensure_target_dir()) {
        snprintf(g_status, sizeof(g_status), "Cannot access LayeredFS folder.");
        return;
    }
    in_ok = restore_one(TARGET_IN, BACKUP_IN, STATE_IN);
    out_ok = restore_one(TARGET_OUT, BACKUP_OUT, STATE_OUT);
    if (in_ok && out_ok)
        snprintf(g_status, sizeof(g_status), "Restored pre-SlideNX state (or removed new overrides).");
    else
        snprintf(g_status, sizeof(g_status), "Restore partial: joint.raw %s, joint_out.raw %s. Keep backups.",
                 in_ok ? "OK" : "FAILED", out_ok ? "OK" : "FAILED");
}

int main(void)
{
    PadState pad;

    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    g_sd_ready = mount_sd_if_needed();
    if (g_sd_ready) {
        if (!ensure_slide_dirs()) {
            snprintf(g_status, sizeof(g_status), "SD is available but SlideNX folders could not be created.");
        } else {
            g_outputs_ready = is_regular_file(OUTPUT_IN) && is_regular_file(OUTPUT_OUT);
            refresh_status();
        }
    }

    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad);

        if (down & HidNpadButton_Plus)
            break;
        if (down & HidNpadButton_A) {
            if (g_sd_ready)
                convert_both();
            else
                snprintf(g_status, sizeof(g_status), "SD is not accessible; cannot convert.");
        }
        if (down & HidNpadButton_Y) {
            if (g_sd_ready)
                install_both();
            else
                snprintf(g_status, sizeof(g_status), "SD is not accessible; cannot install.");
        }
        if (down & HidNpadButton_X) {
            if (g_sd_ready)
                restore_baseline();
            else
                snprintf(g_status, sizeof(g_status), "SD is not accessible; cannot restore.");
        }
        if (down & HidNpadButton_B) {
            if (g_sd_ready)
                refresh_status();
            else
                snprintf(g_status, sizeof(g_status), "SD is not accessible.");
        }

        consoleClear();
        printf("SlideNX Reborn 2.0 - Sound Converter\n");
        printf("===================================\n\n");
        printf("Foreground homebrew app (.nro), not a sysmodule.\n\n");
        printf("Input files:\n");
        printf("  sdmc:/SlideNX/IN.mp3   -> joint.raw\n");
        printf("  sdmc:/SlideNX/OUT.mp3  -> joint_out.raw\n\n");
        printf("Output: mono signed PCM 16-bit LE, 48000 Hz, headerless RAW\n");
        printf("Status: %s\n\n", g_status);
        printf("A: Convert both MP3 files\n");
        printf("Y: Install both converted RAW files to LayeredFS\n");
        printf("X: Restore baseline captured before first install\n");
        printf("B: Refresh input/output status\n");
        printf("PLUS: Exit\n\n");
        printf("Target: atmosphere/contents/010000000000100C/romfs/sound/\n");
        printf("Back up your SD first. Reboot after installing.\n");

        consoleUpdate(NULL);
        svcSleepThread(16000000ULL);
    }

    consoleExit(NULL);
    return 0;
}
