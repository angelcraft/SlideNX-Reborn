#pragma once
#include <stddef.h>

int preview_mp3(const char *input_path, int gain_db,
                char *error_text, size_t error_text_size);
