#include "audio_lab.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "minimp3.h"

#define MAX_MP3_SIZE (16U * 1024U * 1024U)
#define MAX_SOURCE_SAMPLES (48000U * 120U)
#define WAVE_CHUNK_SAMPLES 512U
#define MAX_WAVE_CHUNKS (MAX_SOURCE_SAMPLES / WAVE_CHUNK_SAMPLES + 2U)

static void set_error(char *text, size_t size, const char *message)
{
    if (text && size) (void)snprintf(text, size, "%s", message);
}

static double dbfs(double value)
{
    if (value <= 0.0) return -96.0;
    double db = 20.0 * log10(value / 32768.0);
    return db < -96.0 ? -96.0 : db;
}

int audio_lab_analyze_mp3(const char *input_path, AudioLabInfo *info,
                          char *error_text, size_t error_text_size)
{
    FILE *fp = NULL;
    uint8_t *compressed = NULL;
    uint16_t *chunks = NULL;
    long file_size;
    size_t got, total_samples = 0, chunk_count;
    int offset = 0, source_rate = 0, result = -1;
    double peak = 0.0, sum_squares = 0.0;
    mp3dec_t decoder;
    mp3dec_frame_info_t frame_info;
    mp3d_sample_t decoded[MINIMP3_MAX_SAMPLES_PER_FRAME];

    if (!input_path || !info) {
        set_error(error_text, error_text_size, "Invalid MP3 path or result buffer.");
        return -1;
    }
    memset(info, 0, sizeof(*info));
    fp = fopen(input_path, "rb");
    if (!fp) { set_error(error_text, error_text_size, "Could not open selected MP3."); goto cleanup; }
    if (fseek(fp, 0, SEEK_END) != 0 || (file_size = ftell(fp)) <= 0 ||
        fseek(fp, 0, SEEK_SET) != 0) {
        set_error(error_text, error_text_size, "MP3 is empty or unreadable."); goto cleanup;
    }
    if ((unsigned long)file_size > MAX_MP3_SIZE) {
        set_error(error_text, error_text_size, "MP3 exceeds the 16 MiB limit."); goto cleanup;
    }
    compressed = (uint8_t *)malloc((size_t)file_size);
    chunks = (uint16_t *)calloc(MAX_WAVE_CHUNKS, sizeof(uint16_t));
    if (!compressed || !chunks) {
        set_error(error_text, error_text_size, "Not enough memory to analyze this MP3."); goto cleanup;
    }
    got = fread(compressed, 1, (size_t)file_size, fp);
    if (got != (size_t)file_size) { set_error(error_text, error_text_size, "Could not read the whole MP3."); goto cleanup; }

    mp3dec_init(&decoder);
    while (offset < file_size) {
        int samples;
        memset(&frame_info, 0, sizeof(frame_info));
        samples = mp3dec_decode_frame(&decoder, compressed + offset,
                                      (int)(file_size - offset), decoded, &frame_info);
        if (frame_info.frame_bytes <= 0) { offset++; continue; }
        offset += frame_info.frame_bytes;
        if (samples <= 0 || frame_info.channels <= 0 || frame_info.channels > 2 || frame_info.hz <= 0)
            continue;
        if (!source_rate) source_rate = frame_info.hz;
        else if (source_rate != frame_info.hz) {
            set_error(error_text, error_text_size, "MP3 sample rate changes mid-file."); goto cleanup;
        }
        if (total_samples + (size_t)samples > MAX_SOURCE_SAMPLES ||
            total_samples + (size_t)samples > (size_t)source_rate * 120U) {
            set_error(error_text, error_text_size, "Audio exceeds the 120-second safety limit."); goto cleanup;
        }

        for (int i = 0; i < samples; ++i) {
            int32_t left = decoded[i * frame_info.channels];
            int32_t right = frame_info.channels > 1 ? decoded[i * frame_info.channels + 1] : left;
            double value = (double)((left + right) / 2);
            double magnitude = value < 0.0 ? -value : value;
            size_t chunk_index = total_samples / WAVE_CHUNK_SAMPLES;
            if (magnitude > peak) peak = magnitude;
            sum_squares += value * value;
            if (chunk_index >= MAX_WAVE_CHUNKS) {
                set_error(error_text, error_text_size, "Audio waveform exceeds the supported limit."); goto cleanup;
            }
            if (magnitude > chunks[chunk_index]) chunks[chunk_index] = (uint16_t)magnitude;
            total_samples++;
        }
    }

    if (!source_rate || !total_samples) {
        set_error(error_text, error_text_size, "No valid MP3 audio frames decoded."); goto cleanup;
    }

    info->duration_ms = (uint32_t)(((uint64_t)total_samples * 1000U) / (uint64_t)source_rate);
    info->peak_dbfs = dbfs(peak);
    info->rms_dbfs = dbfs(sqrt(sum_squares / (double)total_samples));
    chunk_count = (total_samples + WAVE_CHUNK_SAMPLES - 1U) / WAVE_CHUNK_SAMPLES;
    for (size_t column = 0; column < AUDIO_LAB_WAVE_SAMPLES; ++column) {
        size_t from = (column * chunk_count) / AUDIO_LAB_WAVE_SAMPLES;
        size_t to = ((column + 1U) * chunk_count) / AUDIO_LAB_WAVE_SAMPLES;
        uint16_t column_peak = 0;
        if (to <= from) to = from + 1U;
        if (to > chunk_count) to = chunk_count;
        for (size_t i = from; i < to; ++i)
            if (chunks[i] > column_peak) column_peak = chunks[i];
        info->waveform[column] = (uint16_t)(((uint32_t)column_peak * 1000U) / 32768U);
    }
    info->valid = 1;
    result = 0;
    set_error(error_text, error_text_size, "OK");

cleanup:
    if (fp) fclose(fp);
    free(compressed);
    free(chunks);
    return result;
}
