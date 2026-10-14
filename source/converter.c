#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "converter.h"

#define TARGET_RATE 48000U
#define MAX_MP3_SIZE (16U * 1024U * 1024U)
#define MAX_SOURCE_SAMPLES (48000U * 120U)
#define WRITE_BUFFER_SIZE 8192U

typedef struct {
    FILE *file;
    uint8_t data[WRITE_BUFFER_SIZE];
    size_t used;
} PcmWriter;

static void set_error(char *text, size_t size, const char *message)
{
    if (text != NULL && size > 0)
        (void)snprintf(text, size, "%s", message);
}

static bool flush_writer(PcmWriter *writer)
{
    if (writer->used == 0)
        return true;
    if (fwrite(writer->data, 1, writer->used, writer->file) != writer->used)
        return false;
    writer->used = 0;
    return true;
}

static bool write_sample(PcmWriter *writer, int16_t sample)
{
    uint16_t value = (uint16_t)sample;
    if (writer->used + 2 > sizeof(writer->data) && !flush_writer(writer))
        return false;
    writer->data[writer->used++] = (uint8_t)(value & 0xFF);
    writer->data[writer->used++] = (uint8_t)((value >> 8) & 0xFF);
    return true;
}

int convert_mp3_to_raw(const char *input_path, const char *output_path,
                       char *error_text, size_t error_text_size)
{
    FILE *input = NULL;
    FILE *output = NULL;
    uint8_t *compressed = NULL;
    int16_t *mono = NULL;
    size_t mono_count = 0;
    size_t mono_capacity = 0;
    long file_size;
    size_t read_count;
    int offset = 0;
    int source_rate = 0;
    int result = -1;
    mp3dec_t decoder;
    mp3dec_frame_info_t info;
    mp3d_sample_t decoded[MINIMP3_MAX_SAMPLES_PER_FRAME];
    PcmWriter writer;

    if (input_path == NULL || output_path == NULL) {
        set_error(error_text, error_text_size, "Invalid input/output path.");
        return -1;
    }
    input = fopen(input_path, "rb");
    if (input == NULL) {
        set_error(error_text, error_text_size, "Could not open input MP3.");
        goto cleanup;
    }
    if (fseek(input, 0, SEEK_END) != 0 || (file_size = ftell(input)) <= 0 ||
        fseek(input, 0, SEEK_SET) != 0) {
        set_error(error_text, error_text_size, "Input MP3 is empty or unreadable.");
        goto cleanup;
    }
    if ((unsigned long)file_size > MAX_MP3_SIZE) {
        set_error(error_text, error_text_size, "MP3 exceeds the 16 MiB safety limit.");
        goto cleanup;
    }
    compressed = (uint8_t *)malloc((size_t)file_size);
    if (compressed == NULL) {
        set_error(error_text, error_text_size, "Not enough memory to read the MP3.");
        goto cleanup;
    }
    read_count = fread(compressed, 1, (size_t)file_size, input);
    if (read_count != (size_t)file_size) {
        set_error(error_text, error_text_size, "Could not read the entire MP3.");
        goto cleanup;
    }

    mp3dec_init(&decoder);
    while (offset < file_size) {
        int samples;
        size_t needed;

        memset(&info, 0, sizeof(info));
        samples = mp3dec_decode_frame(&decoder, compressed + offset,
                                      (int)(file_size - offset), decoded, &info);
        if (info.frame_bytes <= 0) {
            offset++;
            continue;
        }
        offset += info.frame_bytes;
        if (samples <= 0 || info.channels <= 0 || info.channels > 2 || info.hz <= 0)
            continue;

        if (source_rate == 0)
            source_rate = info.hz;
        else if (source_rate != info.hz) {
            set_error(error_text, error_text_size, "MP3 sample rate changes mid-file; export with a constant sample rate.");
            goto cleanup;
        }

        needed = mono_count + (size_t)samples;
        if (needed > MAX_SOURCE_SAMPLES || needed > (size_t)source_rate * 120U) {
            set_error(error_text, error_text_size, "Audio is longer than the 120-second limit.");
            goto cleanup;
        }
        if (needed > mono_capacity) {
            size_t new_capacity = mono_capacity == 0 ? 32768U : mono_capacity;
            int16_t *grown;
            while (new_capacity < needed) {
                if (new_capacity > MAX_SOURCE_SAMPLES / 2U) {
                    new_capacity = MAX_SOURCE_SAMPLES;
                    break;
                }
                new_capacity *= 2U;
            }
            if (new_capacity < needed) {
                set_error(error_text, error_text_size, "Audio exceeds the conversion limit.");
                goto cleanup;
            }
            grown = (int16_t *)realloc(mono, new_capacity * sizeof(int16_t));
            if (grown == NULL) {
                set_error(error_text, error_text_size, "Not enough memory for decoded audio.");
                goto cleanup;
            }
            mono = grown;
            mono_capacity = new_capacity;
        }

        for (int i = 0; i < samples; i++) {
            int32_t left = decoded[i * info.channels];
            int32_t right = info.channels > 1 ? decoded[i * info.channels + 1] : left;
            mono[mono_count + (size_t)i] = (int16_t)((left + right) / 2);
        }
        mono_count = needed;
    }

    if (source_rate == 0 || mono_count == 0) {
        set_error(error_text, error_text_size, "No valid MP3 audio frames were decoded.");
        goto cleanup;
    }

    output = fopen(output_path, "wb");
    if (output == NULL) {
        set_error(error_text, error_text_size, "Could not create output RAW file.");
        goto cleanup;
    }
    memset(&writer, 0, sizeof(writer));
    writer.file = output;

    {
        uint64_t product = (uint64_t)mono_count * (uint64_t)TARGET_RATE;
        uint64_t output_count = (product + (uint64_t)source_rate / 2U) / (uint64_t)source_rate;

        for (uint64_t i = 0; i < output_count; i++) {
            uint64_t position = i * (uint64_t)source_rate;
            uint64_t source_index = position / TARGET_RATE;
            uint32_t fraction = (uint32_t)(position % TARGET_RATE);
            int32_t first;
            int32_t second;
            int64_t interpolated;

            if (source_index >= mono_count)
                source_index = mono_count - 1U;
            first = mono[source_index];
            second = source_index + 1U < mono_count ? mono[source_index + 1U] : first;
            interpolated = (int64_t)first +
                (((int64_t)second - (int64_t)first) * (int64_t)fraction) / (int64_t)TARGET_RATE;

            if (interpolated > 32767)
                interpolated = 32767;
            if (interpolated < -32768)
                interpolated = -32768;
            if (!write_sample(&writer, (int16_t)interpolated)) {
                set_error(error_text, error_text_size, "Writing RAW audio failed.");
                goto cleanup;
            }
        }
    }
    if (!flush_writer(&writer)) {
        set_error(error_text, error_text_size, "Could not finish writing RAW audio.");
        goto cleanup;
    }
    result = 0;
    set_error(error_text, error_text_size, "OK");

cleanup:
    if (output != NULL) {
        if (fclose(output) != 0 && result == 0) {
            result = -1;
            set_error(error_text, error_text_size, "Closing the output RAW file failed.");
        }
    }
    if (input != NULL)
        fclose(input);
    free(compressed);
    free(mono);
    if (result != 0 && output_path != NULL)
        (void)remove(output_path);
    return result;
}
