#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "converter.h"
#include "slidenx_normalizer.h"

#define TARGET_RATE 48000U
#define MAX_MP3_SIZE (16U * 1024U * 1024U)
#define MAX_SOURCE_SAMPLES (48000U * 120U)
#define WRITE_BUFFER_SIZE 8192U

typedef struct { FILE *file; uint8_t data[WRITE_BUFFER_SIZE]; size_t used; } PcmWriter;

static void set_error(char *text, size_t size, const char *message)
{
    if (text && size) (void)snprintf(text, size, "%s", message);
}

static bool flush_writer(PcmWriter *writer)
{
    if (!writer->used) return true;
    if (fwrite(writer->data, 1, writer->used, writer->file) != writer->used) return false;
    writer->used = 0;
    return true;
}

static bool write_sample(PcmWriter *writer, int16_t sample)
{
    uint16_t value = (uint16_t)sample;
    if (writer->used + 2 > sizeof(writer->data) && !flush_writer(writer)) return false;
    writer->data[writer->used++] = (uint8_t)(value & 0xff);
    writer->data[writer->used++] = (uint8_t)((value >> 8) & 0xff);
    return true;
}

int convert_mp3_to_raw(const char *input_path, const char *output_path,
                       int gain_db, char *error_text, size_t error_text_size)
{
    FILE *input = NULL, *output = NULL;
    uint8_t *compressed = NULL;
    int16_t *mono = NULL;
    size_t mono_count = 0, mono_capacity = 0;
    long file_size;
    size_t got;
    int offset = 0, source_rate = 0, result = -1;
    double gain;
    mp3dec_t decoder;
    mp3dec_frame_info_t info;
    mp3d_sample_t decoded[MINIMP3_MAX_SAMPLES_PER_FRAME];
    PcmWriter writer;

    if (!input_path || !output_path || gain_db < -12 || gain_db > 6) {
        set_error(error_text, error_text_size, "Invalid input, output or gain.");
        return -1;
    }
    input = fopen(input_path, "rb");
    if (!input) { set_error(error_text, error_text_size, "Could not open input MP3."); goto cleanup; }
    if (fseek(input, 0, SEEK_END) != 0 || (file_size = ftell(input)) <= 0 || fseek(input, 0, SEEK_SET) != 0) {
        set_error(error_text, error_text_size, "Input MP3 is empty or unreadable."); goto cleanup;
    }
    if ((unsigned long)file_size > MAX_MP3_SIZE) {
        set_error(error_text, error_text_size, "MP3 exceeds the 16 MiB safety limit."); goto cleanup;
    }
    compressed = (uint8_t *)malloc((size_t)file_size);
    if (!compressed) { set_error(error_text, error_text_size, "Not enough memory for MP3."); goto cleanup; }
    got = fread(compressed, 1, (size_t)file_size, input);
    if (got != (size_t)file_size) { set_error(error_text, error_text_size, "Could not read the whole MP3."); goto cleanup; }

    mp3dec_init(&decoder);
    while (offset < file_size) {
        int samples;
        size_t needed;
        memset(&info, 0, sizeof(info));
        samples = mp3dec_decode_frame(&decoder, compressed + offset,
                                      (int)(file_size - offset), decoded, &info);
        if (info.frame_bytes <= 0) { offset++; continue; }
        offset += info.frame_bytes;
        if (samples <= 0 || info.channels <= 0 || info.channels > 2 || info.hz <= 0) continue;
        if (!source_rate) source_rate = info.hz;
        else if (source_rate != info.hz) {
            set_error(error_text, error_text_size, "MP3 sample rate changes mid-file; export at a constant rate.");
            goto cleanup;
        }

        needed = mono_count + (size_t)samples;
        if (needed > MAX_SOURCE_SAMPLES || needed > (size_t)source_rate * 120U) {
            set_error(error_text, error_text_size, "Audio exceeds the 120-second limit."); goto cleanup;
        }
        if (needed > mono_capacity) {
            size_t capacity = mono_capacity ? mono_capacity : 32768U;
            int16_t *grown;
            while (capacity < needed) {
                if (capacity > MAX_SOURCE_SAMPLES / 2U) { capacity = MAX_SOURCE_SAMPLES; break; }
                capacity *= 2U;
            }
            if (capacity < needed) { set_error(error_text, error_text_size, "Audio is too long."); goto cleanup; }
            grown = (int16_t *)realloc(mono, capacity * sizeof(int16_t));
            if (!grown) { set_error(error_text, error_text_size, "Not enough RAM to decode audio."); goto cleanup; }
            mono = grown;
            mono_capacity = capacity;
        }
        for (int i = 0; i < samples; i++) {
            int32_t left = decoded[i * info.channels];
            int32_t right = info.channels > 1 ? decoded[i * info.channels + 1] : left;
            mono[mono_count + (size_t)i] = (int16_t)((left + right) / 2);
        }
        mono_count = needed;
    }

    if (!source_rate || !mono_count) { set_error(error_text, error_text_size, "No valid MP3 audio frames decoded."); goto cleanup; }

    gain = slidenx_normalized_gain(mono, mono_count, gain_db);
    output = fopen(output_path, "wb");
    if (!output) { set_error(error_text, error_text_size, "Could not create RAW output."); goto cleanup; }
    memset(&writer, 0, sizeof(writer));
    writer.file = output;

    {
        uint64_t total = (uint64_t)mono_count * TARGET_RATE;
        uint64_t output_count = (total + (uint64_t)source_rate / 2U) / (uint64_t)source_rate;
        for (uint64_t i = 0; i < output_count; i++) {
            uint64_t position = i * (uint64_t)source_rate;
            uint64_t index = position / TARGET_RATE;
            uint32_t fraction = (uint32_t)(position % TARGET_RATE);
            int32_t first, second;
            double value;
            long scaled;
            if (index >= mono_count) index = mono_count - 1U;
            first = mono[index];
            second = index + 1U < mono_count ? mono[index + 1U] : first;
            value = (double)first + ((double)second - first) * (double)fraction / TARGET_RATE;
            scaled = lrint(value * gain);
            if (scaled > 32767) scaled = 32767;
            if (scaled < -32768) scaled = -32768;
            if (!write_sample(&writer, (int16_t)scaled)) {
                set_error(error_text, error_text_size, "Could not write RAW audio."); goto cleanup;
            }
        }
    }
    if (!flush_writer(&writer)) { set_error(error_text, error_text_size, "Could not finish RAW output."); goto cleanup; }
    result = 0;
    set_error(error_text, error_text_size, "OK");

cleanup:
    if (output && fclose(output) != 0 && result == 0) {
        result = -1; set_error(error_text, error_text_size, "Could not close RAW output.");
    }
    if (input) fclose(input);
    free(compressed);
    free(mono);
    if (result != 0 && output_path) (void)remove(output_path);
    return result;
}
