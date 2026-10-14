#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "mp3.h"

#define PCM_BUFFER_SIZE 0x1000

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
    int offset = 0;
    mp3dec_t decoder;
    mp3dec_frame_info_t info;
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

    if (!g_audio_ready || !file || !*file) return -1;
    fp = fopen(file, "rb");
    if (!fp) return -1;
    if (fseek(fp, 0, SEEK_END) != 0 || (file_size = ftell(fp)) <= 0 ||
        fseek(fp, 0, SEEK_SET) != 0) goto cleanup;
    compressed = malloc((size_t)file_size);
    if (!compressed) goto cleanup;
    got = fread(compressed, 1, (size_t)file_size, fp);
    if (got != (size_t)file_size) goto cleanup;

    mp3dec_init(&decoder);
    result = 0;
    while (offset < file_size) {
        int samples = mp3dec_decode_frame(&decoder, compressed + offset,
                                          (int)(file_size - offset), pcm, &info);
        if (info.frame_bytes <= 0) {
            /* Skip invalid bytes so malformed files cannot stall forever. */
            offset++;
            continue;
        }
        offset += info.frame_bytes;
        if (samples <= 0 || info.channels <= 0) continue;

        /* libnx audio output expects aligned buffers. minimp3 emits signed 16-bit
           PCM; mono frames are duplicated to stereo. Audio is submitted one
           decoded frame at a time to keep memory use small. */
        int16_t *out = (int16_t *)g_pcm_buffer;
        size_t out_samples = (size_t)samples * 2;
        if (out_samples * sizeof(int16_t) > PCM_BUFFER_SIZE) { result = -1; break; }
        for (int i = 0; i < samples; i++) {
            int16_t left = pcm[i * info.channels];
            int16_t right = info.channels > 1 ? pcm[i * info.channels + 1] : left;
            out[i * 2] = left;
            out[i * 2 + 1] = right;
        }

        AudioOutBuffer audio = {0};
        AudioOutBuffer *released = NULL;
        audio.buffer = g_pcm_buffer;
        audio.buffer_size = PCM_BUFFER_SIZE;
        audio.data_size = out_samples * sizeof(int16_t);
        audio.data_offset = 0;
        if (R_FAILED(audoutPlayBuffer(&audio, &released))) { result = -1; break; }
        /* Wait until audio service releases the buffer before reusing it. */
        while (released == NULL) {
            svcSleepThread(1000000ULL);
            (void)audoutWaitPlayFinish(&released, 1, 100000000ULL);
        }
    }

cleanup:
    free(compressed);
    if (fp) fclose(fp);
    return result;
}
