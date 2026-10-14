#pragma once
#include <stddef.h>

int convert_mp3_to_raw(const char *input_path, const char *output_path,
                       char *error_text, size_t error_text_size);
