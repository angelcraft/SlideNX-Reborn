#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <switch.h>

#include "mp3.h"

#define GPIO_LEFT_RAW_ID   0x0C
#define GPIO_RIGHT_RAW_ID  0x34
#define POLL_INTERVAL_NS   20000000ULL
#define ATTACH_PATH        "sdmc:/SlideNX/sounds/attach.mp3"
#define DETACH_PATH        "sdmc:/SlideNX/sounds/detach.mp3"

static GpioPadSession g_left_pad;
static GpioPadSession g_right_pad;
static bool g_gpio_ready;
static bool g_left_open;
static bool g_right_open;
static bool g_audio_ready;
static bool g_sd_ready;

static void cleanup(void)
{
    if (g_left_open)
        gpioPadClose(&g_left_pad);
    if (g_right_open)
        gpioPadClose(&g_right_pad);
    if (g_gpio_ready)
        gpioExit();
    if (g_audio_ready)
        mp3SystemExit();
    if (g_sd_ready)
        (void)fsdevUnmountAll();
}

/*
 * This NRO is a diagnostic, not the final sysmodule. It shares the MP3
 * playback implementation with the sysmodule candidate, but runs as
 * user-launched homebrew so we can first check basic service access and
 * raw GPIO values on the target console.
 */
int main(void)
{
    Result sd_rc;
    Result gpio_rc = 0;
    Result left_rc = 0;
    Result right_rc = 0;
    GpioValue left_previous = GpioValue_Low;
    GpioValue right_previous = GpioValue_Low;
    bool left_value_ready = false;
    bool right_value_ready = false;
    char last_event[96] = "No GPIO transitions seen yet.";
    char last_audio[128] = "Use A = attach.mp3, B = detach.mp3.";
    PadState pad;

    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    sd_rc = fsdevMountSdmc();
    g_sd_ready = R_SUCCEEDED(sd_rc);

    if (mp3SystemInit() == 0)
        g_audio_ready = true;

    gpio_rc = gpioInitialize();
    if (R_SUCCEEDED(gpio_rc)) {
        g_gpio_ready = true;

        gpio_rc = gpioOpenSession(&g_left_pad, (GpioPadName)GPIO_LEFT_RAW_ID);
        if (R_SUCCEEDED(gpio_rc)) {
            g_left_open = true;
            gpio_rc = gpioPadSetDirection(&g_left_pad, GpioDirection_Input);
        }

        if (g_left_open && R_SUCCEEDED(gpio_rc)) {
            gpio_rc = gpioOpenSession(&g_right_pad, (GpioPadName)GPIO_RIGHT_RAW_ID);
            if (R_SUCCEEDED(gpio_rc)) {
                g_right_open = true;
                gpio_rc = gpioPadSetDirection(&g_right_pad, GpioDirection_Input);
            }
        }

        if (g_left_open && R_SUCCEEDED(gpioPadGetValue(&g_left_pad, &left_previous)))
            left_value_ready = true;
        if (g_right_open && R_SUCCEEDED(gpioPadGetValue(&g_right_pad, &right_previous)))
            right_value_ready = true;
    }

    while (appletMainLoop()) {
        hidScanInput();
        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad);

        if (down & KEY_PLUS)
            break;

        if ((down & KEY_A) && g_audio_ready) {
            int play_rc = playMp3(ATTACH_PATH);
            snprintf(last_audio, sizeof(last_audio), "A / attach.mp3: playback result %d.", play_rc);
        }
        if ((down & KEY_B) && g_audio_ready) {
            int play_rc = playMp3(DETACH_PATH);
            snprintf(last_audio, sizeof(last_audio), "B / detach.mp3: playback result %d.", play_rc);
        }

        if (g_left_open && left_value_ready) {
            GpioValue current;
            left_rc = gpioPadGetValue(&g_left_pad, &current);
            if (R_SUCCEEDED(left_rc) && current != left_previous) {
                snprintf(last_event, sizeof(last_event), "LEFT GPIO: %u -> %u.", (unsigned)left_previous, (unsigned)current);
                left_previous = current;
            }
        }
        if (g_right_open && right_value_ready) {
            GpioValue current;
            right_rc = gpioPadGetValue(&g_right_pad, &current);
            if (R_SUCCEEDED(right_rc) && current != right_previous) {
                snprintf(last_event, sizeof(last_event), "RIGHT GPIO: %u -> %u.", (unsigned)right_previous, (unsigned)current);
                right_previous = current;
            }
        }

        consoleClear();
        printf("SlideNX Reborn - GPIO diagnostic\n");
        printf("=================================\n\n");
        printf("This is a test NRO, not the sysmodule.\n");
        printf("Firmware target: Horizon 22.x / Switch OLED\n\n");

        printf("SD mount:    %s (0x%08x)\n", g_sd_ready ? "OK" : "FAILED", (unsigned)sd_rc);
        printf("MP3/audio:   %s\n", g_audio_ready ? "READY" : "FAILED");
        printf("GPIO service:%s (0x%08x)\n", g_gpio_ready ? "READY" : "FAILED", (unsigned)(g_gpio_ready ? 0 : gpio_rc));
        printf("Left session:%s\n", g_left_open ? "OPEN" : "NOT OPEN");
        printf("Right session:%s\n\n", g_right_open ? "OPEN" : "NOT OPEN");

        if (left_value_ready)
            printf("Left GPIO  0x%02X = %u (read rc 0x%08x)\n", GPIO_LEFT_RAW_ID, (unsigned)left_previous, (unsigned)left_rc);
        else
            printf("Left GPIO  0x%02X = unavailable\n", GPIO_LEFT_RAW_ID);
        if (right_value_ready)
            printf("Right GPIO 0x%02X = %u (read rc 0x%08x)\n", GPIO_RIGHT_RAW_ID, (unsigned)right_previous, (unsigned)right_rc);
        else
            printf("Right GPIO 0x%02X = unavailable\n", GPIO_RIGHT_RAW_ID);

        printf("\nLast change: %s\n", last_event);
        printf("Audio test:  %s\n\n", last_audio);
        printf("Put your own attach.mp3 and detach.mp3 in\n");
        printf("SD:/SlideNX/sounds/ to test audio output.\n\n");
        printf("A: play attach.mp3   B: play detach.mp3\n");
        printf("Attach/detach each Joy-Con to watch raw GPIO.\n");
        printf("PLUS: exit diagnostic\n");

        consoleUpdate(NULL);
        svcSleepThread(POLL_INTERVAL_NS);
    }

    cleanup();
    consoleExit(NULL);
    return 0;
}
