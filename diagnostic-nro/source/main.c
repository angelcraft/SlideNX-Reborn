#include <math.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>

#include <switch.h>

#define GPIO_LEFT_RAW_ID   0x0C
#define GPIO_RIGHT_RAW_ID  0x34
#define POLL_INTERVAL_NS   20000000ULL
#define PCM_BUFFER_SIZE    0x10000

static GpioPadSession g_left_pad;
static GpioPadSession g_right_pad;
static bool g_gpio_ready;
static bool g_left_open;
static bool g_right_open;
static bool g_audio_ready;
static bool g_sd_ready;
static bool g_sd_direct_access;
static int g_sd_errno;
static uint8_t *g_pcm_buffer;
static AudioOutBuffer g_audio_buffer;

static Result g_sd_init_rc;
static Result g_sd_rc;
static Result g_gpio_init_rc;
static Result g_left_open_rc = (Result)0xFFFFFFFF;
static Result g_right_open_rc = (Result)0xFFFFFFFF;
static Result g_audio_rc;
static Result g_left_read_rc = (Result)0xFFFFFFFF;
static Result g_right_read_rc = (Result)0xFFFFFFFF;

static void cleanup(void)
{
    if (g_left_open)
        gpioPadClose(&g_left_pad);
    if (g_right_open)
        gpioPadClose(&g_right_pad);
    if (g_gpio_ready)
        gpioExit();

    if (g_audio_ready) {
        if (audoutGetDeviceState() == AudioOutState_Started)
            (void)audoutStopAudioOut();
        audoutExit();
    }

    free(g_pcm_buffer);
    g_pcm_buffer = NULL;

}

static bool initialize_audio(void)
{
    g_pcm_buffer = memalign(0x1000, PCM_BUFFER_SIZE);
    if (g_pcm_buffer == NULL) {
        g_audio_rc = (Result)1;
        return false;
    }

    g_audio_rc = audoutInitialize();
    if (R_FAILED(g_audio_rc)) {
        free(g_pcm_buffer);
        g_pcm_buffer = NULL;
        return false;
    }
    g_audio_ready = true;

    if (audoutGetDeviceState() != AudioOutState_Started) {
        g_audio_rc = audoutStartAudioOut();
        if (R_FAILED(g_audio_rc)) {
            audoutExit();
            g_audio_ready = false;
            free(g_pcm_buffer);
            g_pcm_buffer = NULL;
            return false;
        }
    }

    g_audio_rc = 0;
    return true;
}

/* Generates a short sine tone, so the diagnostic does not need MP3 assets
 * or the optional mpg123 portlib to test the audio output service. */
static Result play_test_tone(double frequency_hz)
{
    const u32 sample_rate = audoutGetSampleRate();
    if (!g_audio_ready || sample_rate == 0 || g_pcm_buffer == NULL)
        return (Result)1;

    const size_t frames = sample_rate / 4; /* 250 ms */
    const size_t bytes = frames * 2 * sizeof(int16_t);
    if (bytes > PCM_BUFFER_SIZE)
        return (Result)2;

    int16_t *samples = (int16_t *)g_pcm_buffer;
    for (size_t i = 0; i < frames; i++) {
        const double phase = (2.0 * 3.14159265358979323846 * frequency_hz * (double)i) / (double)sample_rate;
        const int16_t value = (int16_t)(sin(phase) * 14000.0);
        samples[i * 2] = value;
        samples[i * 2 + 1] = value;
    }

    memset(&g_audio_buffer, 0, sizeof(g_audio_buffer));
    g_audio_buffer.buffer = g_pcm_buffer;
    g_audio_buffer.buffer_size = PCM_BUFFER_SIZE;
    g_audio_buffer.data_size = bytes;
    g_audio_buffer.data_offset = 0;

    AudioOutBuffer *released = NULL;
    return audoutPlayBuffer(&g_audio_buffer, &released);
}

static void initialize_gpio(GpioValue *left_initial, GpioValue *right_initial,
                            bool *left_value_ready, bool *right_value_ready)
{
    g_gpio_init_rc = gpioInitialize();
    if (R_FAILED(g_gpio_init_rc))
        return;
    g_gpio_ready = true;

    g_left_open_rc = gpioOpenSession(&g_left_pad, (GpioPadName)GPIO_LEFT_RAW_ID);
    if (R_SUCCEEDED(g_left_open_rc)) {
        g_left_open = true;
        g_left_open_rc = gpioPadSetDirection(&g_left_pad, GpioDirection_Input);
        if (R_SUCCEEDED(g_left_open_rc)) {
            g_left_read_rc = gpioPadGetValue(&g_left_pad, left_initial);
            *left_value_ready = R_SUCCEEDED(g_left_read_rc);
        }
    }

    g_right_open_rc = gpioOpenSession(&g_right_pad, (GpioPadName)GPIO_RIGHT_RAW_ID);
    if (R_SUCCEEDED(g_right_open_rc)) {
        g_right_open = true;
        g_right_open_rc = gpioPadSetDirection(&g_right_pad, GpioDirection_Input);
        if (R_SUCCEEDED(g_right_open_rc)) {
            g_right_read_rc = gpioPadGetValue(&g_right_pad, right_initial);
            *right_value_ready = R_SUCCEEDED(g_right_read_rc);
        }
    }
}

/*
 * Foreground smoke test. A successful NRO test checks basic service access,
 * raw GPIO values, and audout playback; it does not validate the sysmodule's
 * NPDM permissions or boot/startup behavior.
 */
int main(void)
{
    GpioValue left_previous = GpioValue_Low;
    GpioValue right_previous = GpioValue_Low;
    bool left_value_ready = false;
    bool right_value_ready = false;
    char last_event[96] = "No GPIO transitions seen yet.";
    char last_audio[128] = "A = 660 Hz tone; B = 440 Hz tone.";
    PadState pad;

    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    /*
     * Homebrew Menu may already have mounted sdmc:. Probe it first; calling
     * fsdevMountSdmc() a second time can return an error even though SD access
     * is already working. Only initialize/mount as a fallback if the probe
     * fails.
     */
    {
        DIR *sd_dir = opendir("sdmc:/switch");
        g_sd_direct_access = (sd_dir != NULL);
        g_sd_errno = g_sd_direct_access ? 0 : errno;
        if (sd_dir != NULL)
            closedir(sd_dir);
    }

    if (g_sd_direct_access) {
        g_sd_init_rc = 0;
        g_sd_rc = 0;
        g_sd_ready = true;
    } else {
        g_sd_init_rc = fsInitialize();
        if (R_SUCCEEDED(g_sd_init_rc)) {
            g_sd_rc = fsdevMountSdmc();
            g_sd_ready = R_SUCCEEDED(g_sd_rc);
        } else {
            g_sd_rc = g_sd_init_rc;
            g_sd_ready = false;
        }

        DIR *sd_dir = opendir("sdmc:/switch");
        g_sd_direct_access = (sd_dir != NULL);
        g_sd_errno = g_sd_direct_access ? 0 : errno;
        if (sd_dir != NULL)
            closedir(sd_dir);

        if (g_sd_direct_access)
            g_sd_ready = true;
    }
    (void)initialize_audio();
    initialize_gpio(&left_previous, &right_previous, &left_value_ready, &right_value_ready);

    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad);

        if (down & HidNpadButton_Plus)
            break;

        if (down & HidNpadButton_A) {
            const Result rc = play_test_tone(660.0);
            snprintf(last_audio, sizeof(last_audio), "A: 660 Hz tone result = 0x%08x.", (unsigned)rc);
        }
        if (down & HidNpadButton_B) {
            const Result rc = play_test_tone(440.0);
            snprintf(last_audio, sizeof(last_audio), "B: 440 Hz tone result = 0x%08x.", (unsigned)rc);
        }

        if (g_left_open && left_value_ready) {
            GpioValue current;
            g_left_read_rc = gpioPadGetValue(&g_left_pad, &current);
            if (R_SUCCEEDED(g_left_read_rc) && current != left_previous) {
                snprintf(last_event, sizeof(last_event), "LEFT GPIO: %u -> %u.", (unsigned)left_previous, (unsigned)current);
                left_previous = current;
            }
        }
        if (g_right_open && right_value_ready) {
            GpioValue current;
            g_right_read_rc = gpioPadGetValue(&g_right_pad, &current);
            if (R_SUCCEEDED(g_right_read_rc) && current != right_previous) {
                snprintf(last_event, sizeof(last_event), "RIGHT GPIO: %u -> %u.", (unsigned)right_previous, (unsigned)current);
                right_previous = current;
            }
        }

        consoleClear();
        printf("SlideNX Reborn - GPIO / AUDIO TEST\n");
        printf("==================================\n\n");
        printf("Diagnostic NRO only - not the installable sysmodule.\n\n");
        printf("FS init:     %s (0x%08x)\n", R_SUCCEEDED(g_sd_init_rc) ? "OK" : "FAILED", (unsigned)g_sd_init_rc);
        printf("SD mount:    %s (0x%08x)\n", g_sd_ready ? "OK" : "FAILED", (unsigned)g_sd_rc);
        printf("SD /switch:  %s (errno %d)\n", g_sd_direct_access ? "ACCESSIBLE" : "UNAVAILABLE", g_sd_errno);
        printf("Audio out:   %s (0x%08x)\n", g_audio_ready ? "READY" : "FAILED", (unsigned)g_audio_rc);
        printf("GPIO service:%s (0x%08x)\n", g_gpio_ready ? "READY" : "FAILED", (unsigned)g_gpio_init_rc);
        printf("Left session:%s (0x%08x)\n", g_left_open ? "OPEN" : "NOT OPEN", (unsigned)g_left_open_rc);
        printf("Right session:%s (0x%08x)\n\n", g_right_open ? "OPEN" : "NOT OPEN", (unsigned)g_right_open_rc);

        if (left_value_ready)
            printf("Left raw GPIO  0x%02X = %u (read 0x%08x)\n", GPIO_LEFT_RAW_ID, (unsigned)left_previous, (unsigned)g_left_read_rc);
        else
            printf("Left raw GPIO  0x%02X = unavailable\n", GPIO_LEFT_RAW_ID);
        if (right_value_ready)
            printf("Right raw GPIO 0x%02X = %u (read 0x%08x)\n", GPIO_RIGHT_RAW_ID, (unsigned)right_previous, (unsigned)g_right_read_rc);
        else
            printf("Right raw GPIO 0x%02X = unavailable\n", GPIO_RIGHT_RAW_ID);

        printf("\nLast change: %s\n", last_event);
        printf("Audio test:  %s\n\n", last_audio);
        printf("A: play 660 Hz tone    B: play 440 Hz tone\n");
        printf("Attach/detach each Joy-Con to watch raw GPIO values.\n");
        printf("PLUS: exit diagnostic\n");

        consoleUpdate(NULL);
        svcSleepThread(POLL_INTERVAL_NS);
    }

    cleanup();
    consoleExit(NULL);
    return 0;
}
