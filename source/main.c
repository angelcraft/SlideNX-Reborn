#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include <switch.h>
#include <switch/services/fatal.h>

#include "mp3.h"

#define INNER_HEAP_SIZE 0x80000
#define GPIO_LEFT_RAW_ID  0x0C
#define GPIO_RIGHT_RAW_ID 0x34
#define POLL_INTERVAL_NS  20000000ULL /* 20 ms */
#define DEBOUNCE_SAMPLES  5           /* 100 ms of stable readings */
#define MAX_SOUND_FILES   96
#define MAX_SOUND_PATH    512
#define CONFIG_PATH       "sdmc:/SlideNX/config.ini"
#define SOUND_DIRECTORY   "sdmc:/SlideNX/sounds"
#define ATTACH_PATH       "sdmc:/SlideNX/sounds/attach.mp3"
#define DETACH_PATH       "sdmc:/SlideNX/sounds/detach.mp3"
#define LOG_PATH          "sdmc:/SlideNX/SlideNX.log"

/* These raw GPIO identifiers are inherited from upstream SlideNX. Their
 * behavior must still be verified on OLED hardware. */

size_t nx_inner_heap_size = INNER_HEAP_SIZE;
char nx_inner_heap[INNER_HEAP_SIZE];
u32 __nx_applet_type = AppletType_None;

static GpioPadSession g_left_pad;
static GpioPadSession g_right_pad;
static bool g_gpio_initialized;
static bool g_left_open;
static bool g_right_open;
static bool g_audio_initialized;
static bool g_random_mode;
static char g_sound_paths[MAX_SOUND_FILES][MAX_SOUND_PATH];
static size_t g_sound_count;

void __libnx_initheap(void)
{
    extern char *fake_heap_start;
    extern char *fake_heap_end;

    fake_heap_start = nx_inner_heap;
    fake_heap_end = nx_inner_heap + nx_inner_heap_size;
}

void __attribute__((weak)) __appInit(void)
{
    Result rc = smInitialize();
    if (R_FAILED(rc))
        fatalThrow(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

    rc = fsInitialize();
    if (R_FAILED(rc))
        fatalThrow(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));
}

void __attribute__((weak)) userAppExit(void);

void __attribute__((weak)) __appExit(void)
{
    (void)fsdevUnmountAll();
    fsExit();
    smExit();
}

static void log_message(const char *format, ...)
{
    char message[384];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    FILE *log = fopen(LOG_PATH, "a");
    if (log != NULL) {
        fprintf(log, "[SlideNX Reborn] %s\n", message);
        fclose(log);
    }
}

static bool is_mp3_filename(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot != NULL && strcasecmp(dot, ".mp3") == 0;
}

static bool read_random_mode(void)
{
    FILE *file = fopen(CONFIG_PATH, "r");
    char line[128];
    bool random_mode = false;

    if (file == NULL)
        return false;

    while (fgets(line, sizeof(line), file) != NULL) {
        char *cursor = line;
        char *equals;
        char *value;

        while (isspace((unsigned char)*cursor))
            cursor++;
        if (*cursor == '\0' || *cursor == '#' || *cursor == ';')
            continue;
        if (strncmp(cursor, "random", 6) != 0)
            continue;

        equals = strchr(cursor, '=');
        if (equals == NULL)
            continue;
        value = equals + 1;
        while (isspace((unsigned char)*value))
            value++;
        if (strncasecmp(value, "true", 4) == 0 || *value == '1')
            random_mode = true;
        else if (strncasecmp(value, "false", 5) == 0 || *value == '0')
            random_mode = false;
    }

    fclose(file);
    return random_mode;
}

static void load_sound_files(void)
{
    DIR *directory = opendir(SOUND_DIRECTORY);
    struct dirent *entry;

    g_sound_count = 0;
    if (directory == NULL) {
        log_message("Cannot open sound directory (%s).", SOUND_DIRECTORY);
        return;
    }

    while ((entry = readdir(directory)) != NULL && g_sound_count < MAX_SOUND_FILES) {
        char full_path[MAX_SOUND_PATH];
        struct stat info;
        int written;

        if (entry->d_name[0] == '.' || !is_mp3_filename(entry->d_name))
            continue;

        written = snprintf(full_path, sizeof(full_path), "%s/%s", SOUND_DIRECTORY, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(full_path))
            continue;
        if (stat(full_path, &info) != 0 || !S_ISREG(info.st_mode))
            continue;

        strncpy(g_sound_paths[g_sound_count], full_path, MAX_SOUND_PATH - 1);
        g_sound_paths[g_sound_count][MAX_SOUND_PATH - 1] = '\0';
        g_sound_count++;
    }

    closedir(directory);
    log_message("Loaded %lu MP3 file(s); random mode=%s.",
                (unsigned long)g_sound_count, g_random_mode ? "true" : "false");
}

static void play_connection_sound(bool attached)
{
    const char *path;
    int rc;

    if (!g_audio_initialized) {
        log_message("Skipping sound: audio runtime is not initialized.");
        return;
    }

    if (g_random_mode && g_sound_count > 0) {
        path = g_sound_paths[(size_t)rand() % g_sound_count];
    } else {
        path = attached ? ATTACH_PATH : DETACH_PATH;
    }

    rc = playMp3(path);
    if (rc != 0)
        log_message("Could not play '%s' (decoder/audio/file error, rc=%d).", path, rc);
}

static void close_runtime_services(void)
{
    if (g_left_open) {
        gpioPadClose(&g_left_pad);
        g_left_open = false;
    }
    if (g_right_open) {
        gpioPadClose(&g_right_pad);
        g_right_open = false;
    }
    if (g_gpio_initialized) {
        gpioExit();
        g_gpio_initialized = false;
    }
    if (g_audio_initialized) {
        mp3SystemExit();
        g_audio_initialized = false;
    }
}

static bool initialize_gpio(GpioValue *left_initial, GpioValue *right_initial)
{
    Result rc = gpioInitialize();
    if (R_FAILED(rc)) {
        log_message("gpioInitialize failed: 0x%08x.", (unsigned)rc);
        return false;
    }
    g_gpio_initialized = true;

    rc = gpioOpenSession(&g_left_pad, (GpioPadName)GPIO_LEFT_RAW_ID);
    if (R_FAILED(rc)) {
        log_message("Opening left Joy-Con GPIO 0x%02x failed: 0x%08x.", GPIO_LEFT_RAW_ID, (unsigned)rc);
        return false;
    }
    g_left_open = true;

    rc = gpioOpenSession(&g_right_pad, (GpioPadName)GPIO_RIGHT_RAW_ID);
    if (R_FAILED(rc)) {
        log_message("Opening right Joy-Con GPIO 0x%02x failed: 0x%08x.", GPIO_RIGHT_RAW_ID, (unsigned)rc);
        return false;
    }
    g_right_open = true;

    rc = gpioPadSetDirection(&g_left_pad, GpioDirection_Input);
    if (R_FAILED(rc)) {
        log_message("Setting left GPIO input failed: 0x%08x.", (unsigned)rc);
        return false;
    }
    rc = gpioPadSetDirection(&g_right_pad, GpioDirection_Input);
    if (R_FAILED(rc)) {
        log_message("Setting right GPIO input failed: 0x%08x.", (unsigned)rc);
        return false;
    }

    rc = gpioPadGetValue(&g_left_pad, left_initial);
    if (R_FAILED(rc)) {
        log_message("Reading left GPIO failed: 0x%08x.", (unsigned)rc);
        return false;
    }
    rc = gpioPadGetValue(&g_right_pad, right_initial);
    if (R_FAILED(rc)) {
        log_message("Reading right GPIO failed: 0x%08x.", (unsigned)rc);
        return false;
    }

    log_message("GPIO sessions ready; initial levels L=%u R=%u.",
                (unsigned)*left_initial, (unsigned)*right_initial);
    return true;
}

/* Require several consecutive reads before accepting a GPIO transition.
 * This filters short glitches/noisy edges that otherwise trigger duplicate
 * sounds. It cannot validate whether these raw GPIO IDs are correct for OLED. */
static bool stable_transition(GpioValue current, GpioValue *candidate,
                              unsigned *count, GpioValue previous)
{
    if (current == previous) {
        *candidate = previous;
        *count = 0;
        return false;
    }

    if (current != *candidate) {
        *candidate = current;
        *count = 1;
    } else if (*count < DEBOUNCE_SAMPLES) {
        (*count)++;
    }

    return *count >= DEBOUNCE_SAMPLES;
}

int main(void)
{
    GpioValue left_previous = GpioValue_Low;
    GpioValue right_previous = GpioValue_Low;
    GpioValue left_candidate = GpioValue_Low;
    GpioValue right_candidate = GpioValue_Low;
    unsigned left_stable_count = 0;
    unsigned right_stable_count = 0;
    Result rc;

    rc = fsdevMountSdmc();
    if (R_SUCCEEDED(rc))
        log_message("Startup. SD card mounted.");
    else
        log_message("SD mount failed: 0x%08x.", (unsigned)rc);

    g_random_mode = read_random_mode();
    load_sound_files();
    srand((unsigned)time(NULL));

    if (mp3SystemInit() == 0) {
        g_audio_initialized = true;
        log_message("Audio/MP3 runtime ready; output sample rate=%u.", (unsigned)audoutGetSampleRate());
    } else {
        log_message("Audio/MP3 initialization failed; monitoring will continue without sound.");
    }

    if (!initialize_gpio(&left_previous, &right_previous)) {
        log_message("GPIO initialization failed; module will exit so it does not spin uselessly.");
        close_runtime_services();
        return 1;
    }
    left_candidate = left_previous;
    right_candidate = right_previous;

    for (;;) {
        GpioValue left_now;
        GpioValue right_now;
        Result left_rc;
        Result right_rc;

        svcSleepThread(POLL_INTERVAL_NS);

        left_rc = gpioPadGetValue(&g_left_pad, &left_now);
        right_rc = gpioPadGetValue(&g_right_pad, &right_now);

        if (R_FAILED(left_rc)) {
            left_stable_count = 0;
            left_candidate = left_previous;
        } else if (stable_transition(left_now, &left_candidate, &left_stable_count, left_previous)) {
            GpioValue old_value = left_previous;
            left_previous = left_candidate;
            left_stable_count = 0;
            log_message("Left GPIO stable transition %u -> %u.", (unsigned)old_value, (unsigned)left_previous);
            /* Preserve upstream polarity: low=attached, high=detached. */
            play_connection_sound(left_previous == GpioValue_Low);
        }

        if (R_FAILED(right_rc)) {
            right_stable_count = 0;
            right_candidate = right_previous;
        } else if (stable_transition(right_now, &right_candidate, &right_stable_count, right_previous)) {
            GpioValue old_value = right_previous;
            right_previous = right_candidate;
            right_stable_count = 0;
            log_message("Right GPIO stable transition %u -> %u.", (unsigned)old_value, (unsigned)right_previous);
            play_connection_sound(right_previous == GpioValue_Low);
        }
    }

    close_runtime_services();
    return 0;
}
