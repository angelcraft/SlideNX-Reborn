#include "gui.h"

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define GUI_WIDTH 1280
#define GUI_HEIGHT 720
#define GUI_MAX_FILES_VISIBLE 14

typedef struct { Uint8 r, g, b, a; } GuiColor;

static SDL_Window *g_window;
static SDL_Renderer *g_renderer;

static const GuiColor C_BG       = {  9,  14,  27, 255};
static const GuiColor C_SIDEBAR  = { 16,  23,  40, 255};
static const GuiColor C_PANEL    = { 21,  31,  51, 255};
static const GuiColor C_PANEL_HI = { 26,  54,  69, 255};
static const GuiColor C_BORDER   = { 42,  57,  79, 255};
static const GuiColor C_ACCENT   = { 67, 222, 190, 255};
static const GuiColor C_TEXT     = {239, 245, 252, 255};
static const GuiColor C_MUTED    = {139, 157, 181, 255};
static const GuiColor C_GREEN    = { 83, 220, 151, 255};
static const GuiColor C_AMBER    = {255, 198,  92, 255};
static const GuiColor C_RED      = {246, 113, 123, 255};

/* Compact embedded 5x7 bitmap font. Text is drawn in uppercase so no external
   font file or extra runtime asset is needed on the SD card. */
#define GLYPH(ch,a,b,c,d,e,f,g) [ch] = {a,b,c,d,e,f,g}
static const Uint8 FONT[128][7] = {
    GLYPH('0',14,17,19,21,25,17,14),
    GLYPH('1', 4,12, 4, 4, 4, 4,14),
    GLYPH('2',14,17, 1, 2, 4, 8,31),
    GLYPH('3',30, 1, 1,14, 1, 1,30),
    GLYPH('4', 2, 6,10,18,31, 2, 2),
    GLYPH('5',31,16,16,30, 1, 1,30),
    GLYPH('6',14,16,16,30,17,17,14),
    GLYPH('7',31, 1, 2, 4, 8, 8, 8),
    GLYPH('8',14,17,17,14,17,17,14),
    GLYPH('9',14,17,17,15, 1, 1,14),
    GLYPH('A',14,17,17,31,17,17,17),
    GLYPH('B',30,17,17,30,17,17,30),
    GLYPH('C',14,17,16,16,16,17,14),
    GLYPH('D',30,17,17,17,17,17,30),
    GLYPH('E',31,16,16,30,16,16,31),
    GLYPH('F',31,16,16,30,16,16,16),
    GLYPH('G',14,17,16,23,17,17,15),
    GLYPH('H',17,17,17,31,17,17,17),
    GLYPH('I',14, 4, 4, 4, 4, 4,14),
    GLYPH('J', 7, 2, 2, 2,18,18,12),
    GLYPH('K',17,18,20,24,20,18,17),
    GLYPH('L',16,16,16,16,16,16,31),
    GLYPH('M',17,27,21,21,17,17,17),
    GLYPH('N',17,25,21,19,17,17,17),
    GLYPH('O',14,17,17,17,17,17,14),
    GLYPH('P',30,17,17,30,16,16,16),
    GLYPH('Q',14,17,17,17,21,18,13),
    GLYPH('R',30,17,17,30,20,18,17),
    GLYPH('S',15,16,16,14, 1, 1,30),
    GLYPH('T',31, 4, 4, 4, 4, 4, 4),
    GLYPH('U',17,17,17,17,17,17,14),
    GLYPH('V',17,17,17,17,17,10, 4),
    GLYPH('W',17,17,17,21,21,21,10),
    GLYPH('X',17,17,10, 4,10,17,17),
    GLYPH('Y',17,17,10, 4, 4, 4, 4),
    GLYPH('Z',31, 1, 2, 4, 8,16,31),
    GLYPH(':', 0, 4, 4, 0, 4, 4, 0),
    GLYPH('.', 0, 0, 0, 0, 0, 6, 6),
    GLYPH(',', 0, 0, 0, 0, 6, 6, 4),
    GLYPH(';', 0, 4, 4, 0, 4, 4, 8),
    GLYPH('-', 0, 0, 0,31, 0, 0, 0),
    GLYPH('_', 0, 0, 0, 0, 0, 0,31),
    GLYPH('+', 0, 4, 4,31, 4, 4, 0),
    GLYPH('/', 1, 1, 2, 4, 8,16,16),
    GLYPH('\\',16,16, 8, 4, 2, 1, 1),
    GLYPH('(', 2, 4, 8, 8, 8, 4, 2),
    GLYPH(')', 8, 4, 2, 2, 2, 4, 8),
    GLYPH('[',14, 8, 8, 8, 8, 8,14),
    GLYPH(']',14, 2, 2, 2, 2, 2,14),
    GLYPH('!', 4, 4, 4, 4, 4, 0, 4),
    GLYPH('?',14,17, 1, 2, 4, 0, 4),
    GLYPH('=', 0,31, 0,31, 0, 0, 0),
    GLYPH(39, 4, 4, 2, 0, 0, 0, 0),
    GLYPH('"',10,10, 5, 0, 0, 0, 0),
    GLYPH('%',17, 2, 4, 8,17, 0, 0),
    GLYPH('*', 0,21,14,31,14,21, 0),
    GLYPH('#',10,31,10,10,31,10, 0),
    GLYPH('@',14,17,23,21,23,16,14),
    GLYPH('&',12,18,20, 8,21,18,13),
    GLYPH('<', 2, 4, 8,16, 8, 4, 2),
    GLYPH('>', 8, 4, 2, 1, 2, 4, 8),
    GLYPH('|', 4, 4, 4, 4, 4, 4, 4),
    GLYPH('~', 0, 0, 9,22, 0, 0, 0)
};
#undef GLYPH

static void color(GuiColor value)
{
    SDL_SetRenderDrawColor(g_renderer, value.r, value.g, value.b, value.a);
}

static void fill_rect(int x, int y, int w, int h, GuiColor c)
{
    SDL_Rect r = {x, y, w, h};
    color(c);
    SDL_RenderFillRect(g_renderer, &r);
}

static void outline_rect(int x, int y, int w, int h, GuiColor c)
{
    SDL_Rect r = {x, y, w, h};
    color(c);
    SDL_RenderDrawRect(g_renderer, &r);
}

static void panel(int x, int y, int w, int h, GuiColor fill, GuiColor border)
{
    fill_rect(x, y, w, h, fill);
    outline_rect(x, y, w, h, border);
}

static void line(int x1, int y1, int x2, int y2, GuiColor c)
{
    color(c);
    SDL_RenderDrawLine(g_renderer, x1, y1, x2, y2);
}

static void text_draw(const char *text, int x, int y, int scale, GuiColor c, int max_chars)
{
    if (!text) return;
    color(c);
    int used = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p && used < max_chars; ++p, ++used) {
        unsigned char ch = *p;
        if (ch < 128) ch = (unsigned char)toupper(ch);
        if (ch >= 128 || ch == '\n' || ch == '\r') ch = '?';
        for (int row = 0; row < 7; ++row) {
            Uint8 bits = FONT[ch][row];
            for (int col = 0; col < 5; ++col) {
                if (bits & (1u << (4 - col))) {
                    SDL_Rect px = {x + (used * 6 + col) * scale, y + row * scale, scale, scale};
                    SDL_RenderFillRect(g_renderer, &px);
                }
            }
        }
    }
}

static int char_count_limit(int pixel_width, int scale)
{
    int chars = pixel_width / (6 * scale);
    return chars > 0 ? chars : 1;
}

static void draw_truncated(const char *value, int x, int y, int scale,
                           GuiColor c, int max_pixel_width)
{
    int max_chars = char_count_limit(max_pixel_width, scale);
    size_t len = value ? strlen(value) : 0;
    if (len <= (size_t)max_chars) {
        text_draw(value ? value : "", x, y, scale, c, max_chars);
        return;
    }
    char local[128];
    size_t copy_len = (size_t)max_chars;
    if (copy_len > sizeof(local) - 1) copy_len = sizeof(local) - 1;
    if (copy_len > 3) copy_len -= 3;
    memcpy(local, value, copy_len);
    local[copy_len++] = '.';
    local[copy_len++] = '.';
    local[copy_len++] = '.';
    local[copy_len] = '\0';
    text_draw(local, x, y, scale, c, max_chars);
}

bool gui_init(void)
{
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL video init failed: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    g_window = SDL_CreateWindow("SlideNX Reborn", SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED, GUI_WIDTH, GUI_HEIGHT,
                                SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN);
    if (!g_window) {
        fprintf(stderr, "SDL window creation failed: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }

    g_renderer = SDL_CreateRenderer(g_window, -1,
                                    SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_renderer)
        g_renderer = SDL_CreateRenderer(g_window, -1, 0);
    if (!g_renderer) {
        fprintf(stderr, "SDL renderer creation failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_window);
        g_window = NULL;
        SDL_Quit();
        return false;
    }

    if (SDL_RenderSetLogicalSize(g_renderer, GUI_WIDTH, GUI_HEIGHT) != 0) {
        fprintf(stderr, "SDL logical resolution setup failed: %s\n", SDL_GetError());
    }
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
    return true;
}

void gui_exit(void)
{
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window) SDL_DestroyWindow(g_window);
    g_renderer = NULL;
    g_window = NULL;
    SDL_Quit();
}

static void frame_begin(void)
{
    color(C_BG);
    SDL_RenderClear(g_renderer);
}

static void frame_end(void)
{
    SDL_RenderPresent(g_renderer);
}

static void draw_header(const char *title, const char *subtitle)
{
    text_draw("SLIDENX REBORN 2.5", 34, 22, 3, C_TEXT, 36);
    text_draw(title, 35, 64, 2, C_ACCENT, 36);
    draw_truncated(subtitle, 800, 55, 2, C_MUTED, 440);
    line(30, 94, 1250, 94, C_BORDER);
}

static void draw_sound_card(int x, int y, int w, const char *label,
                            const char *filename, bool selected, bool connecting)
{
    GuiColor border = selected ? C_ACCENT : C_BORDER;
    panel(x, y, w, 164, C_PANEL, border);
    fill_rect(x, y, 6, 164, connecting ? C_ACCENT : C_AMBER);
    text_draw(label, x + 22, y + 18, 2, C_MUTED, 28);
    draw_truncated(filename && filename[0] ? filename : "(NOT SELECTED)",
                   x + 22, y + 60, 2, C_TEXT, w - 42);
    text_draw(selected ? "SELECTED IN MENU" : "MP3 SOURCE",
              x + 22, y + 119, 1, selected ? C_ACCENT : C_MUTED, 35);
}

void gui_draw_main(int selected, const char *connect_name, const char *disconnect_name,
                   int gain_db, const char *status, bool outputs_ready, bool sd_ready)
{
    static const char *items[] = {
        "CHOOSE CONNECT SOUND", "CHOOSE DISCONNECT SOUND",
        "PREVIEW CONNECT SOUND", "PREVIEW DISCONNECT",
        "CONVERSION GAIN", "CONVERT SELECTED SOUNDS",
        "INSTALL TO LAYEREDFS", "RESTORE PREVIOUS SOUNDS",
        "REFRESH MP3 LIST", "AUDIO LAB / TRIM", "EXIT SLIDENX"
    };

    frame_begin();
    draw_header("SOUND STUDIO", "CUSTOM JOY-CON CONNECT / DISCONNECT EFFECTS");

    panel(28, 112, 376, 566, C_SIDEBAR, C_BORDER);
    text_draw("MENU", 48, 130, 2, C_MUTED, 20);
    line(46, 153, 386, 153, C_BORDER);

    for (int i = 0; i < 11; ++i) {
        int y = 164 + i * 44;
        bool active = (i == selected);
        if (active) {
            fill_rect(40, y, 352, 39, C_PANEL_HI);
            fill_rect(40, y, 5, 39, C_ACCENT);
        }
        draw_truncated(items[i], 56, y + 13, 2,
                       active ? C_TEXT : C_MUTED, 323);
    }

    draw_sound_card(428, 112, 392, "CONNECT SOUND", connect_name, selected == 0 || selected == 2, true);
    draw_sound_card(836, 112, 416, "DISCONNECT SOUND", disconnect_name, selected == 1 || selected == 3, false);

    panel(428, 290, 824, 97, C_PANEL, C_BORDER);
    text_draw("AUTOMATIC LEVELING", 448, 305, 2, C_ACCENT, 30);
    text_draw("MANUAL GAIN", 448, 343, 2, C_MUTED, 20);
    char gain[32];
    snprintf(gain, sizeof(gain), "%+d DB", gain_db);
    text_draw(gain, 620, 339, 3, C_TEXT, 16);
    text_draw("LEFT/RIGHT TO ADJUST", 820, 344, 1, C_MUTED, 42);
    fill_rect(1118, 331, 112, 29, outputs_ready ? C_GREEN : C_BORDER);
    text_draw(outputs_ready ? "RAW READY" : "CONVERT FIRST", 1124, 341, 1,
              outputs_ready ? C_BG : C_MUTED, 20);

    panel(428, 401, 824, 133, C_PANEL, C_BORDER);
    text_draw("STATUS", 448, 418, 2, C_MUTED, 18);
    fill_rect(448, 448, 8, 8, sd_ready ? C_GREEN : C_RED);
    draw_truncated(status && status[0] ? status : "READY", 470, 442, 2, C_TEXT, 750);
    text_draw(sd_ready ? "SD CARD AVAILABLE" : "SD CARD UNAVAILABLE",
              448, 491, 1, sd_ready ? C_GREEN : C_RED, 30);

    panel(428, 548, 824, 130, C_PANEL, C_BORDER);
    text_draw("QUICK GUIDE", 448, 566, 2, C_ACCENT, 20);
    text_draw("A  SELECT   Y  INSTALL   X  RESTORE", 448, 599, 2, C_TEXT, 42);
    text_draw("B  REFRESH   PLUS  EXIT", 448, 632, 2, C_MUTED, 35);

    frame_end();
}

void gui_draw_picker(bool connecting, size_t count, const char *const *files, int selected)
{
    size_t start = selected >= GUI_MAX_FILES_VISIBLE
                 ? (size_t)(selected - GUI_MAX_FILES_VISIBLE + 1) : 0;

    frame_begin();
    draw_header(connecting ? "CHOOSE CONNECT SOUND" : "CHOOSE DISCONNECT SOUND",
                "SELECT AN MP3 FROM THE SD CARD");

    panel(28, 112, 1224, 566, C_SIDEBAR, C_BORDER);
    text_draw("FOLDER  SD:/SLIDENX/SOUNDS/", 48, 130, 2, C_MUTED, 42);
    char count_label[48];
    snprintf(count_label, sizeof(count_label), "%lu MP3 FILE(S)", (unsigned long)count);
    text_draw(count_label, 930, 130, 2, C_ACCENT, 28);
    line(46, 157, 1234, 157, C_BORDER);

    if (count == 0) {
        text_draw("NO MP3 FILES FOUND", 72, 205, 3, C_TEXT, 30);
        text_draw("COPY MP3 FILES TO SD:/SLIDENX/SOUNDS/", 72, 260, 2, C_MUTED, 50);
    } else {
        size_t end = start + GUI_MAX_FILES_VISIBLE;
        if (end > count) end = count;
        int row = 0;
        for (size_t i = start; i < end; ++i, ++row) {
            int y = 169 + row * 33;
            bool active = ((int)i == selected);
            if (active) {
                fill_rect(44, y, 1190, 31, C_PANEL_HI);
                fill_rect(44, y, 5, 31, C_ACCENT);
                text_draw(">", 58, y + 7, 2, C_ACCENT, 2);
            }
            draw_truncated(files && files[i] ? files[i] : "(INVALID NAME)",
                           active ? 84 : 68, y + 7, 2,
                           active ? C_TEXT : C_MUTED, 1110);
        }
    }

    fill_rect(28, 637, 1224, 41, C_PANEL);
    text_draw("UP/DOWN  NAVIGATE", 48, 650, 2, C_TEXT, 25);
    text_draw("A  CHOOSE", 450, 650, 2, C_ACCENT, 18);
    text_draw("B  CANCEL", 650, 650, 2, C_MUTED, 18);
    text_draw("PLUS  EXIT", 850, 650, 2, C_MUTED, 18);
    frame_end();
}

static void format_time(uint32_t milliseconds, char *buffer, size_t size)
{
    unsigned long total_seconds = (unsigned long)(milliseconds / 1000U);
    unsigned long minutes = total_seconds / 60U;
    unsigned long seconds = total_seconds % 60U;
    unsigned long millis = (unsigned long)(milliseconds % 1000U);
    (void)snprintf(buffer, size, "%02lu:%02lu.%03lu", minutes, seconds, millis);
}

void gui_draw_audio_lab(bool connecting, const char *filename,
                        const AudioLabInfo *info, const AudioTrimOptions *options,
                        int selected_row, const char *status)
{
    static const char *row_names[] = {"TRIM START", "TRIM END", "FADE IN", "FADE OUT"};
    char duration[32], start[32], end[32], fade_in[32], fade_out[32], metric[80];
    uint32_t dur = info && info->valid ? info->duration_ms : 0U;
    uint32_t start_ms = options ? options->trim_start_ms : 0U;
    uint32_t end_ms = options && options->trim_end_ms ? options->trim_end_ms : dur;
    uint32_t fi_ms = options ? options->fade_in_ms : 0U;
    uint32_t fo_ms = options ? options->fade_out_ms : 0U;
    const int wave_x = 72, wave_y = 270, wave_width = 1136;
    const int center_y = wave_y + 30;

    format_time(dur, duration, sizeof(duration));
    format_time(start_ms, start, sizeof(start));
    if (options && options->trim_end_ms) format_time(end_ms, end, sizeof(end));
    else (void)snprintf(end, sizeof(end), "AUTO / EOF");
    format_time(fi_ms, fade_in, sizeof(fade_in));
    format_time(fo_ms, fade_out, sizeof(fade_out));

    frame_begin();
    draw_header("AUDIO LAB", connecting ? "EDITING CONNECT SOUND" : "EDITING DISCONNECT SOUND");
    panel(28, 112, 1224, 566, C_SIDEBAR, C_BORDER);

    text_draw("SOURCE", 52, 128, 1, C_MUTED, 16);
    draw_truncated(filename && filename[0] ? filename : "(NO MP3 SELECTED)",
                   52, 146, 2, C_TEXT, 690);
    text_draw("L / R  SWITCH SOURCE", 906, 145, 2, C_ACCENT, 30);

    if (!info || !info->valid) {
        text_draw("ANALYSIS UNAVAILABLE", 70, 214, 3, C_RED, 30);
        draw_truncated(status ? status : "CHECK MP3 FILE", 70, 266, 2, C_TEXT, 1000);
    } else {
        (void)snprintf(metric, sizeof(metric), "DURATION  %s", duration);
        text_draw(metric, 52, 190, 2, C_TEXT, 27);
        (void)snprintf(metric, sizeof(metric), "PEAK  %.1f DBFS", info->peak_dbfs);
        text_draw(metric, 455, 190, 2, C_ACCENT, 28);
        (void)snprintf(metric, sizeof(metric), "RMS  %.1f DBFS", info->rms_dbfs);
        text_draw(metric, 830, 190, 2, C_AMBER, 25);

        panel(52, 222, 1176, 128, C_PANEL, C_BORDER);
        line(wave_x, center_y, wave_x + wave_width, center_y, C_BORDER);
        uint32_t effective_end = end_ms > 0 ? end_ms : dur;
        int start_x = wave_x + (dur ? (int)(((uint64_t)start_ms * wave_width) / dur) : 0);
        int end_x = wave_x + (dur ? (int)(((uint64_t)effective_end * wave_width) / dur) : wave_width);
        if (start_x < wave_x) start_x = wave_x;
        if (start_x > wave_x + wave_width) start_x = wave_x + wave_width;
        if (end_x < start_x) end_x = start_x;
        if (end_x > wave_x + wave_width) end_x = wave_x + wave_width;

        for (int i = 0; i < AUDIO_LAB_WAVE_SAMPLES; ++i) {
            int x = wave_x + (int)(((int64_t)i * wave_width) / AUDIO_LAB_WAVE_SAMPLES);
            int bar_w = 5;
            int h = (int)(((uint32_t)info->waveform[i] * 54U) / 1000U);
            int sample_mid = wave_x + (int)(((int64_t)(i * 2 + 1) * wave_width) /
                                            (AUDIO_LAB_WAVE_SAMPLES * 2));
            bool included = sample_mid >= start_x && sample_mid < end_x;
            GuiColor bar = included ? (connecting ? C_ACCENT : C_AMBER) : C_BORDER;
            if (h < 2) h = 2;
            fill_rect(x, center_y - h, bar_w, h * 2, bar);
        }
        line(start_x, 232, start_x, 343, C_TEXT);
        line(end_x, 232, end_x, 343, C_TEXT);
        text_draw("START", start_x > wave_x + 70 ? start_x - 42 : start_x + 3, 334, 1, C_TEXT, 8);
        text_draw("END", end_x > wave_x + wave_width - 80 ? end_x - 26 : end_x + 3, 334, 1, C_TEXT, 6);
    }

    for (int row = 0; row < 4; ++row) {
        int y = 369 + row * 52;
        bool active = row == selected_row;
        if (active) {
            fill_rect(50, y, 1178, 44, C_PANEL_HI);
            fill_rect(50, y, 5, 44, C_ACCENT);
        }
        text_draw(row_names[row], 72, y + 13, 2, active ? C_TEXT : C_MUTED, 22);
        const char *value = row == 0 ? start : row == 1 ? end : row == 2 ? fade_in : fade_out;
        draw_truncated(value, 500, y + 12, 2, active ? C_ACCENT : C_TEXT, 320);
        if (row < 2)
            text_draw("250 MS STEPS", 900, y + 15, 1, C_MUTED, 22);
        else
            text_draw("FADE DURATION", 900, y + 15, 1, C_MUTED, 22);
    }

    fill_rect(28, 581, 1224, 97, C_PANEL);
    draw_truncated(status && status[0] ? status : "ANALYSIS READY", 48, 592, 1, C_MUTED, 1140);
    text_draw("UP/DOWN ROW   LEFT/RIGHT ADJUST   A PREVIEW", 48, 615, 2, C_TEXT, 43);
    text_draw("L/R SOURCE   X RESET   B BACK   PLUS EXIT", 48, 649, 2, C_ACCENT, 42);
    frame_end();
}
