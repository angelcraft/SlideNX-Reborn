#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "mp3.h"

/* A full stereo MP3 frame can need 1152 * 2 channels * 2 bytes = 4608
 * bytes. 0x1000 (4096) was too small and caused normal frames to fail. */
#define PCM_BUFFER_SIZE 0x2000

static uint8_t *g_pcm_buffer;
static bool g_audout_ready;
static bool g_audio_ready;

int mp3SystemInit(void)
{
    Result rc;
    if (g_audio_ready) return 0;
    g_pcm_buffer = memalign(0x1000, PCM_BUFFER_SIZE);
    if (!g_pcm_buffer) return -1;
    rc = audoutInitialize();
    if (R_FAILED(rc)) { free(g_pcm_buffer); g_pcm_buffer = NULL; return -1; }
    g_audout_ready = true;
    if (audoutGetDeviceState() != AudioOutState_Started) {
        rc = audoutStartAudioOut();
        if (R_FAILED(rc)) { mp3SystemExit(); return -1; }
    }
    g_audio_ready = true;
    return 0;
}

void mp3SystemExit(void)
{
    g_audio_ready = false;
    if (g_audout_ready) {
        if (audoutGetDeviceState() == AudioOutState_Started) (void)audoutStopAudioOut();
        audoutExit();
        g_audout_ready = false;
    }
    free(g_pcm_buffer);
    g_pcm_buffer = NULL;
}

int playMp3(const char *file)
{
    FILE *fp = NULL;
    uint8_t *compressed = NULL;
    long file_size;
    size_t got;
    int result = -1;
    long offset = 0;
    mp3dec_t decoder;
    mp3dec_frame_info_t info;
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

    if (!g_audio_ready || !g_pcm_buffer || !file || !*file) return -1;
    fp = fopen(file, "rb");
    if (!fp) return -1;
    if (fseek(fp, 0, SEEK_END) != 0 || (file_size = ftell(fp)) <= 0 ||
        fseek(fp, 0, SEEK_SET) != 0) goto cleanup;

    /* minimp3's frame decoder takes an int input length. Avoid overflow on
     * malformed or unexpectedly huge files instead of narrowing long silently. */
    if (file_size > 16 * 1024 * 1024) goto cleanup;
    compressed = malloc((size_t)file_size);
    if (!compressed) goto cleanup;
    got = fread(compressed, 1, (size_t)file_size, fp);
    if (got != (size_t)file_size) goto cleanup;

    mp3dec_init(&decoder);
    result = 0;
    while (offset < file_size) {
        int remaining = (int)(file_size - offset);
        int samples = mp3dec_decode_frame(&decoder, compressed + offset,
                                          remaining, pcm, &info);
        if (info.frame_bytes <= 0) {
            /* Skip invalid bytes so malformed files cannot stall forever. */
            offset++;
            continue;
        }
        offset += info.frame_bytes;
        if (samples <= 0 || info.channels <= 0) continue;

        /* minimp3 emits signed 16-bit PCM. Duplicate mono to stereo. */
        int16_t *out = (int16_t *)g_pcm_buffer;
        size_t out_samples = (size_t)samples * 2;
        size_t data_bytes = out_samples * sizeof(int16_t);
        if (data_bytes > PCM_BUFFER_SIZE) { result = -1; break; }
        for (int i = 0; i < samples; i++) {
            int16_t left = pcm[i * info.channels];
            int16_t right = info.channels > 1 ? pcm[i * info.channels + 1] : left;
            out[i * 2] = left;
            out[i * 2 + 1] = right;
        }

        AudioOutBuffer audio = {0};
        AudioOutBuffer *released = NULL;
        u32 released_count = 0;
        audio.buffer = g_pcm_buffer;
        audio.buffer_size = PCM_BUFFER_SIZE;
        audio.data_size = data_bytes;
        audio.data_offset = 0;
        if (R_FAILED(audoutPlayBuffer(&audio, &released))) { result = -1; break; }

        /* Wait until audout releases this buffer before reusing its memory. */
        while (released == NULL) {
            svcSleepThread(1000000ULL);
            released_count = 0;
            Result wait_rc = audoutWaitPlayFinish(&released, &released_count, 100000000ULL);
            if (R_FAILED(wait_rc) && wait_rc != MAKERESULT(Module_Libnx, LibnxError_TimedOut)) {
                result = -1;
                goto cleanup;
            }
        }
    }

cleanup:
    free(compressed);
    if (fp) fclose(fp);
    return result;
}
