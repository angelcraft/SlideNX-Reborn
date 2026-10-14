#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <malloc.h>
#include <switch.h>
#include "minimp3.h"
#include "preview.h"
#include "slidenx_normalizer.h"

#define TARGET_RATE 48000U
#define MAX_MP3_SIZE (16U * 1024U * 1024U)
#define PREVIEW_SECONDS 8U
#define MAX_SOURCE_SAMPLES (TARGET_RATE * PREVIEW_SECONDS)
#define PCM_FRAMES_PER_BUFFER 1024U
#define PCM_BUFFER_BYTES (PCM_FRAMES_PER_BUFFER * 2U * sizeof(int16_t))

static void set_error(char *text, size_t size, const char *message)
{
    if (text && size) (void)snprintf(text, size, "%s", message);
}

static void wait_for_buffer(AudioOutBuffer **released)
{
    u32 count = 0;
    while (*released == NULL) {
        Result rc = audoutWaitPlayFinish(released, &count, 100000000ULL);
        if (R_FAILED(rc))
            svcSleepThread(1000000ULL);
    }
}

int preview_mp3(const char *input_path, int gain_db,
                char *error_text, size_t error_text_size)
{
    FILE *fp = NULL;
    uint8_t *compressed = NULL;
    int16_t *mono = NULL;
    size_t mono_count = 0, mono_capacity = 0;
    size_t max_source_samples = MAX_SOURCE_SAMPLES;
    long file_size;
    size_t got;
    int offset = 0, source_rate = 0, result = -1;
    bool audio_initialized = false, started_here = false;
    uint8_t *pcm_buffer = NULL;
    double gain;
    mp3dec_t decoder;
    mp3dec_frame_info_t info;
    mp3d_sample_t decoded[MINIMP3_MAX_SAMPLES_PER_FRAME];

    if (!input_path || gain_db < -12 || gain_db > 6) {
        set_error(error_text, error_text_size, "Invalid MP3 path or gain.");
        return -1;
    }

    fp = fopen(input_path, "rb");
    if (!fp) { set_error(error_text, error_text_size, "Could not open selected MP3."); goto cleanup; }
    if (fseek(fp, 0, SEEK_END) != 0 || (file_size = ftell(fp)) <= 0 || fseek(fp, 0, SEEK_SET) != 0) {
        set_error(error_text, error_text_size, "MP3 is empty or unreadable."); goto cleanup;
    }
    if ((unsigned long)file_size > MAX_MP3_SIZE) {
        set_error(error_text, error_text_size, "MP3 exceeds 16 MiB."); goto cleanup;
    }
    compressed = (uint8_t *)malloc((size_t)file_size);
    if (!compressed) { set_error(error_text, error_text_size, "Not enough memory to read MP3."); goto cleanup; }
    got = fread(compressed, 1, (size_t)file_size, fp);
    if (got != (size_t)file_size) { set_error(error_text, error_text_size, "Could not read the entire MP3."); goto cleanup; }

    mp3dec_init(&decoder);
    while (offset < file_size && mono_count < max_source_samples) {
        int samples;
        size_t remaining, take, needed;
        memset(&info, 0, sizeof(info));
        samples = mp3dec_decode_frame(&decoder, compressed + offset,
                                      (int)(file_size - offset), decoded, &info);
        if (info.frame_bytes <= 0) { offset++; continue; }
        offset += info.frame_bytes;
        if (samples <= 0 || info.channels <= 0 || info.channels > 2 || info.hz <= 0) continue;

        if (source_rate == 0) {
            source_rate = info.hz;
            max_source_samples = (size_t)source_rate * PREVIEW_SECONDS;
            if (max_source_samples > MAX_SOURCE_SAMPLES) max_source_samples = MAX_SOURCE_SAMPLES;
        } else if (source_rate != info.hz) {
            set_error(error_text, error_text_size, "MP3 sample rate changes mid-file."); goto cleanup;
        }

        remaining = max_source_samples - mono_count;
        take = (size_t)samples < remaining ? (size_t)samples : remaining;
        needed = mono_count + take;
        if (needed > mono_capacity) {
            size_t capacity = mono_capacity ? mono_capacity : 32768U;
            int16_t *grown;
            if (capacity > max_source_samples) capacity = max_source_samples;
            while (capacity < needed) {
                size_t next = capacity * 2U;
                capacity = next > max_source_samples ? max_source_samples : next;
                if (capacity < needed && capacity == max_source_samples) break;
            }
            if (capacity < needed) { set_error(error_text, error_text_size, "Preview buffer limit reached."); goto cleanup; }
            grown = (int16_t *)realloc(mono, capacity * sizeof(int16_t));
            if (!grown) { set_error(error_text, error_text_size, "Not enough memory for preview."); goto cleanup; }
            mono = grown; mono_capacity = capacity;
        }
        for (size_t i = 0; i < take; i++) {
            int32_t left = decoded[i * (size_t)info.channels];
            int32_t right = info.channels > 1 ? decoded[i * (size_t)info.channels + 1] : left;
            mono[mono_count + i] = (int16_t)((left + right) / 2);
        }
        mono_count = needed;
    }

    if (!source_rate || !mono_count) { set_error(error_text, error_text_size, "No decodable MP3 audio frames."); goto cleanup; }

    gain = slidenx_normalized_gain(mono, mono_count, gain_db);

    pcm_buffer = (uint8_t *)memalign(0x1000, PCM_BUFFER_BYTES);
    if (!pcm_buffer) { set_error(error_text, error_text_size, "Could not allocate aligned audio buffer."); goto cleanup; }

    {
        Result rc = audoutInitialize();
        if (R_FAILED(rc)) { set_error(error_text, error_text_size, "Audio output initialization failed."); goto cleanup; }
        audio_initialized = true;
        if (audoutGetDeviceState() != AudioOutState_Started) {
            rc = audoutStartAudioOut();
            if (R_FAILED(rc)) { set_error(error_text, error_text_size, "Could not start audio output."); goto cleanup; }
            started_here = true;
        }
    }

    {
        uint64_t scaled_count = (uint64_t)mono_count * TARGET_RATE;
        uint64_t total_frames = (scaled_count + (uint64_t)source_rate / 2U) / (uint64_t)source_rate;

        for (uint64_t base = 0; base < total_frames; base += PCM_FRAMES_PER_BUFFER) {
            size_t frames = (size_t)((total_frames - base) > PCM_FRAMES_PER_BUFFER ?
                                     PCM_FRAMES_PER_BUFFER : (total_frames - base));
            int16_t *samples = (int16_t *)pcm_buffer;
            AudioOutBuffer audio = {0};
            AudioOutBuffer *released = NULL;

            for (size_t frame = 0; frame < frames; frame++) {
                uint64_t i = base + frame;
                uint64_t position = i * (uint64_t)source_rate;
                uint64_t index = position / TARGET_RATE;
                uint32_t fraction = (uint32_t)(position % TARGET_RATE);
                int32_t first, second;
                long value;
                if (index >= mono_count) index = mono_count - 1U;
                first = mono[index];
                second = index + 1U < mono_count ? mono[index + 1U] : first;
                value = lrint(((double)first + ((double)second - first) * (double)fraction / TARGET_RATE) * gain);
                if (value > 32767) value = 32767;
                if (value < -32768) value = -32768;
                samples[frame * 2] = (int16_t)value;
                samples[frame * 2 + 1] = (int16_t)value;
            }

            audio.buffer = pcm_buffer;
            audio.buffer_size = PCM_BUFFER_BYTES;
            audio.data_size = frames * 2U * sizeof(int16_t);
            audio.data_offset = 0;
            if (R_FAILED(audoutPlayBuffer(&audio, &released))) {
                set_error(error_text, error_text_size, "Audio output rejected the preview buffer."); goto cleanup;
            }
            wait_for_buffer(&released);
        }
    }

    result = 0;
    set_error(error_text, error_text_size, "OK");

cleanup:
    if (audio_initialized) {
        if (started_here && audoutGetDeviceState() == AudioOutState_Started)
            (void)audoutStopAudioOut();
        audoutExit();
    }
    free(pcm_buffer);
    free(mono);
    free(compressed);
    if (fp) fclose(fp);
    if (result != 0 && error_text && error_text_size && error_text[0] == '\0')
        set_error(error_text, error_text_size, "Preview failed.");
    return result;
}
