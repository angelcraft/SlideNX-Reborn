#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mpg123.h>
#include <switch.h>

#include "mp3.h"

/* Audout requires page-aligned sample buffers. Keep the allocation and size
 * aligned to 0x1000, and pass the number of decoded bytes as data_size. */
#define PCM_BUFFER_SIZE 0x10000

static uint8_t *g_pcm_buffer;
static AudioOutBuffer g_audio_buffer;
static bool g_mpg123_ready;
static bool g_audout_ready;
static bool g_audio_ready;

int mp3SystemInit(void)
{
    Result rc;

    if (g_audio_ready)
        return 0;

    g_pcm_buffer = memalign(0x1000, PCM_BUFFER_SIZE);
    if (g_pcm_buffer == NULL)
        return -1;

    if (mpg123_init() != MPG123_OK) {
        free(g_pcm_buffer);
        g_pcm_buffer = NULL;
        return -1;
    }
    g_mpg123_ready = true;

    rc = audoutInitialize();
    if (R_FAILED(rc)) {
        mp3SystemExit();
        return -1;
    }
    g_audout_ready = true;

    if (audoutGetDeviceState() != AudioOutState_Started) {
        rc = audoutStartAudioOut();
        if (R_FAILED(rc)) {
            mp3SystemExit();
            return -1;
        }
    }

    g_audio_ready = true;
    return 0;
}

void mp3SystemExit(void)
{
    g_audio_ready = false;

    if (g_audout_ready) {
        if (audoutGetDeviceState() == AudioOutState_Started)
            (void)audoutStopAudioOut();
        audoutExit();
        g_audout_ready = false;
    }

    if (g_mpg123_ready) {
        mpg123_exit();
        g_mpg123_ready = false;
    }

    free(g_pcm_buffer);
    g_pcm_buffer = NULL;
}

int playMp3(const char *file)
{
    mpg123_handle *decoder = NULL;
    int err = MPG123_OK;
    int result = -1;
    long sample_rate;
    int channels = 2;
    int encoding = MPG123_ENC_SIGNED_16;

    if (!g_audio_ready || file == NULL || file[0] == '\0')
        return -1;

    decoder = mpg123_new(NULL, &err);
    if (decoder == NULL) {
        return -1;
    }

    sample_rate = (long)audoutGetSampleRate();
    if (sample_rate <= 0)
        goto cleanup;

    /* Match audout's PCM contract explicitly. The original code used casts
     * from a 32-bit rate / 8-bit channel variable to pointers expected to
     * point to long / int; mpg123_getformat() could overwrite adjacent memory. */
    if (mpg123_param(decoder, MPG123_FORCE_RATE, sample_rate, 0.0) != MPG123_OK)
        goto cleanup;
    if (mpg123_param(decoder, MPG123_FORCE_STEREO, 1, 0.0) != MPG123_OK)
        goto cleanup;
    if (mpg123_format_none(decoder) != MPG123_OK)
        goto cleanup;
    if (mpg123_format(decoder, sample_rate, channels, encoding) != MPG123_OK)
        goto cleanup;
    if (mpg123_open(decoder, file) != MPG123_OK)
        goto cleanup;

    for (;;) {
        size_t bytes_read = 0;
        int decode_status = mpg123_read(decoder, g_pcm_buffer, PCM_BUFFER_SIZE, &bytes_read);

        if (bytes_read > 0) {
            AudioOutBuffer *released = NULL;
            memset(&g_audio_buffer, 0, sizeof(g_audio_buffer));
            g_audio_buffer.buffer = g_pcm_buffer;
            g_audio_buffer.buffer_size = PCM_BUFFER_SIZE;
            g_audio_buffer.data_size = bytes_read;
            g_audio_buffer.data_offset = 0;

            /* Synchronous playback means we don't reuse the PCM allocation
             * until audout has released it. This is intentionally simple and
             * avoids the original double-buffer accounting bug. */
            if (R_FAILED(audoutPlayBuffer(&g_audio_buffer, &released)))
                goto cleanup;
        }

        if (decode_status == MPG123_DONE) {
            result = 0;
            break;
        }
        if (decode_status != MPG123_OK && decode_status != MPG123_NEW_FORMAT)
            goto cleanup;
        if (bytes_read == 0 && decode_status == MPG123_OK)
            goto cleanup; /* Avoid a malformed stream causing a tight loop. */
    }

cleanup:
    if (decoder != NULL) {
        (void)mpg123_close(decoder);
        mpg123_delete(decoder);
    }
    return result;
}
