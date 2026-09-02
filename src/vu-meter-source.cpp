#include "vu-meter-source.hpp"

/* OBS VU Meter PRO 3.3 - visual refinement build. */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>

namespace {

constexpr float DEFAULT_MIN_DB = -60.0f;
constexpr float DEFAULT_WARN_DB = -18.0f;
constexpr float DEFAULT_ERROR_DB = -6.0f;

enum Layout {
    LAYOUT_BASIC = 0,
    LAYOUT_PRO_PANEL = 1,
    LAYOUT_SLIM_LED = 2,
    LAYOUT_ANALOG = 3,
    LAYOUT_DOUBLE_SCALE = 4,
    LAYOUT_SINGLE = 5,
    LAYOUT_SINGLE_SLIM = 6,
    LAYOUT_VERTICAL_PANEL = 7,
    LAYOUT_VERTICAL_LED = 8,
    LAYOUT_ANALOG_VERTICAL = 9
};

enum MeterMode {
    METER_STEREO = 0,
    METER_SINGLE = 1
};

enum VisualStyle {
    STYLE_PRO = 0,
    STYLE_LED = 1,
    STYLE_ANALOG = 2,
    STYLE_MINIMAL = 3,
    STYLE_NEON = 4,
    STYLE_WHITE = 5,
    STYLE_DARK = 6,
    STYLE_RETRO = 7
};

enum BackgroundMode {
    BG_NORMAL = 0,
    BG_TRANSPARENT = 1,
    BG_CHROMA_GREEN = 2,
    BG_CHROMA_BLUE = 3,
    BG_BLACK = 4
};

struct vu_meter {
    obs_source_t *source = nullptr;
    obs_volmeter_t *volmeter = nullptr;
    obs_source_t *attached_source = nullptr;

    std::mutex level_mutex;
    float magnitude[2] = {DEFAULT_MIN_DB, DEFAULT_MIN_DB};
    float peak[2] = {DEFAULT_MIN_DB, DEFAULT_MIN_DB};
    float hold[2] = {DEFAULT_MIN_DB, DEFAULT_MIN_DB};
    float hold_age[2] = {0.0f, 0.0f};

    float min_db = DEFAULT_MIN_DB;
    float warning_db = DEFAULT_WARN_DB;
    float error_db = DEFAULT_ERROR_DB;

    int segments = 32;
    int gap = 3;
    int thickness = 10;
    int layout = LAYOUT_BASIC;
    int meter_mode = METER_STEREO;
    int mono_channel = 0;
    int visual_style = STYLE_PRO;
    int background_mode = BG_NORMAL;

    bool show_peak = true;
    bool show_hold = true;
    float peak_hold_seconds = 1.5f;
    float peak_decay_db_per_sec = 18.0f;

    uint32_t color_green = 0xFF35E06F;
    uint32_t color_yellow = 0xFFFFD84D;
    uint32_t color_red = 0xFFFF3B30;
    uint32_t color_off = 0xFF20252B;
    uint32_t color_background = 0xB0101419;
    uint32_t color_peak = 0xFFFFFFFF;
    uint32_t color_hold = 0xFF00FFFF;
    uint32_t color_frame = 0xFF353B43;
    bool preserve_chroma = true;
};

static const char *vu_name(void *)
{
    return "VU Meter PRO";
}

static float clamp_db(float db, float min_db)
{
    if (!std::isfinite(db))
        return min_db;
    return std::clamp(db, min_db, 0.0f);
}

static float level_to_fraction(float db, float min_db)
{
    if (db <= min_db)
        return 0.0f;
    return std::clamp((db - min_db) / (0.0f - min_db), 0.0f, 1.0f);
}

/* OBS color properties are RGBA; the graphics effect consumes ARGB. */
static uint32_t color_property_to_argb(long long value)
{
    const uint32_t rgba = (uint32_t)value;
    const uint32_t r = rgba & 0xFFu;
    const uint32_t g = (rgba >> 8) & 0xFFu;
    const uint32_t b = (rgba >> 16) & 0xFFu;
    const uint32_t a = (rgba >> 24) & 0xFFu;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static uint32_t migrate_color(long long value, uint32_t legacy_argb)
{
    const uint32_t raw = (uint32_t)value;
    return raw == legacy_argb ? raw : color_property_to_argb(value);
}

static uint32_t segment_color(const vu_meter *m, float db)
{
    if (db >= m->error_db)
        return m->color_red;
    if (db >= m->warning_db)
        return m->color_yellow;
    return m->color_green;
}

static uint32_t style_frame_color(const vu_meter *m)
{
    switch (m->visual_style) {
    case STYLE_LED:     return 0xFF263238;
    case STYLE_ANALOG:  return 0xFF7B6A4A;
    case STYLE_MINIMAL: return 0xFF555B64;
    case STYLE_NEON:    return 0xFF00CFFF;
    case STYLE_WHITE:   return 0xFFD0D5DB;
    case STYLE_DARK:    return 0xFF161A1F;
    case STYLE_RETRO:   return 0xFFB79B61;
    default:            return m->color_frame;
    }
}

static uint32_t style_off_color(const vu_meter *m)
{
    switch (m->visual_style) {
    case STYLE_NEON:   return 0xFF101B22;
    case STYLE_WHITE:  return 0xFFCBD1D8;
    case STYLE_ANALOG: return 0xFF27221A;
    case STYLE_RETRO:  return 0xFF3A3022;
    default:           return m->color_off;
    }
}

static uint32_t style_background_color(const vu_meter *m)
{
    switch (m->visual_style) {
    case STYLE_WHITE:  return 0xE8E9ECEF;
    case STYLE_ANALOG: return 0xE0181714;
    case STYLE_RETRO:  return 0xE0221D16;
    case STYLE_NEON:   return 0xE8061118;
    case STYLE_DARK:   return 0xF0080A0D;
    default:           return m->color_background;
    }
}

static void set_source(vu_meter *m, obs_source_t *new_source)
{
    if (m->attached_source == new_source)
        return;

    if (m->volmeter)
        obs_volmeter_detach_source(m->volmeter);

    if (m->attached_source) {
        obs_source_release(m->attached_source);
        m->attached_source = nullptr;
    }

    if (new_source) {
        m->attached_source = obs_source_get_ref(new_source);
        if (m->volmeter)
            obs_volmeter_attach_source(m->volmeter, m->attached_source);
    }

    std::lock_guard<std::mutex> lock(m->level_mutex);
    m->magnitude[0] = m->magnitude[1] = m->min_db;
    m->peak[0] = m->peak[1] = m->min_db;
    m->hold[0] = m->hold[1] = m->min_db;
    m->hold_age[0] = m->hold_age[1] = 0.0f;
}

static void meter_callback(void *param,
                           const float magnitude[MAX_AUDIO_CHANNELS],
                           const float peak[MAX_AUDIO_CHANNELS],
                           const float input_peak[MAX_AUDIO_CHANNELS])
{
    (void)input_peak;
    auto *m = static_cast<vu_meter *>(param);
    if (!m)
        return;

    std::lock_guard<std::mutex> lock(m->level_mutex);
    for (int ch = 0; ch < 2; ++ch) {
        const float mag = clamp_db(magnitude[ch], m->min_db);
        const float p = clamp_db(peak[ch], m->min_db);
        m->magnitude[ch] = mag;
        m->peak[ch] = p;

        if (p >= m->hold[ch]) {
            m->hold[ch] = p;
            m->hold_age[ch] = 0.0f;
        }
    }
}

static void render_rect(gs_effect_t *effect, gs_eparam_t *color_param,
                        uint32_t color, float x, float y, float w, float h)
{
    if (!effect || !color_param || w <= 0.0f || h <= 0.0f)
        return;

    gs_effect_set_color(color_param, color);
    gs_matrix_push();
    gs_matrix_translate3f(x, y, 0.0f);
    while (gs_effect_loop(effect, "Solid"))
        gs_draw_quadf(nullptr, 0, w, h);
    gs_matrix_pop();
}

static void render_circle(gs_effect_t *effect, gs_eparam_t *color_param,
                           uint32_t color, float cx, float cy, float radius)
{
    if (radius <= 1.0f)
        return;
    /* A filled circle made from horizontal scanlines. This avoids relying on
       platform-specific vector primitives and renders consistently in OBS. */
    const int r = (int)std::ceil(radius);
    for (int iy = -r; iy <= r; ++iy) {
        const float yy = (float)iy;
        const float inside = radius * radius - yy * yy;
        if (inside < 0.0f)
            continue;
        const float half = std::sqrt(inside);
        render_rect(effect, color_param, color,
                    cx - half, cy + yy, std::max(1.0f, half * 2.0f), 1.2f);
    }
}

static void render_ring(gs_effect_t *effect, gs_eparam_t *color_param,
                        uint32_t color, float cx, float cy,
                        float outer, float inner)
{
    if (outer <= inner || inner < 0.0f)
        return;
    const int r = (int)std::ceil(outer);
    const float outer2 = outer * outer;
    const float inner2 = inner * inner;
    for (int iy = -r; iy <= r; ++iy) {
        const float yy = (float)iy;
        const float oy = outer2 - yy * yy;
        if (oy <= 0.0f)
            continue;
        const float half_outer = std::sqrt(oy);
        const float iy2 = yy * yy;
        const float half_inner = inner2 > iy2 ? std::sqrt(inner2 - iy2) : 0.0f;
        if (half_outer > half_inner) {
            render_rect(effect, color_param, color,
                        cx - half_outer, cy + yy,
                        std::max(1.0f, half_outer - half_inner), 1.2f);
            if (half_inner > 0.0f)
                render_rect(effect, color_param, color,
                            cx + half_inner, cy + yy,
                            std::max(1.0f, half_outer - half_inner), 1.2f);
        }
    }
}

static void render_dial_tick(gs_effect_t *effect, gs_eparam_t *color_param,
                             uint32_t color, float cx, float cy,
                             float angle, float r1, float r2, float width)
{
    /* Build a short angled tick from tiny overlapping rectangles. */
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const int steps = std::max(2, (int)std::ceil(std::abs(r2 - r1) / 2.0f));
    for (int i = 0; i <= steps; ++i) {
        const float t = (float)i / (float)steps;
        const float r = r1 + (r2 - r1) * t;
        const float px = cx + c * r;
        const float py = cy + s * r;
        render_rect(effect, color_param, color,
                    px - width * 0.5f, py - width * 0.5f,
                    width, width);
    }
}

static void render_needle(gs_effect_t *effect,
                          gs_eparam_t *color_param, float cx, float cy,
                          float angle, float length, uint32_t color)
{
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const int steps = std::max(4, (int)std::ceil(length / 2.0f));
    for (int i = 0; i <= steps; ++i) {
        const float t = (float)i / (float)steps;
        const float r = length * t;
        const float px = cx + c * r;
        const float py = cy + s * r;
        const float ww = (i < 2) ? 3.0f : 2.0f;
        render_rect(effect, color_param, color,
                    px - ww * 0.5f, py - ww * 0.5f, ww, ww);
    }
    render_circle(effect, color_param, 0xFF151515, cx, cy, 7.0f);
    render_ring(effect, color_param, 0xFFC8C8C8, cx, cy, 7.0f, 5.0f);
}

static void render_frame(gs_effect_t *effect, gs_eparam_t *color_param,
                         uint32_t color, float x, float y, float w, float h,
                         float border)
{
    const float b = std::max(1.0f, border);
    render_rect(effect, color_param, color, x, y, w, b);
    render_rect(effect, color_param, color, x, y + h - b, w, b);
    render_rect(effect, color_param, color, x, y, b, h);
    render_rect(effect, color_param, color, x + w - b, y, b, h);
}

static void render_segment_bar(vu_meter *m, gs_effect_t *effect,
                               gs_eparam_t *color_param,
                               float x, float y, float w, float h,
                               float db, bool vertical);

/* ---------- 3D chassis / professional hardware helpers (v3.2) ---------- */
static void render_bevel_panel(gs_effect_t *effect, gs_eparam_t *color_param,
                               float x, float y, float w, float h,
                               uint32_t body, uint32_t edge_light,
                               uint32_t edge_dark, float depth)
{
    if (w <= 2.0f * depth || h <= 2.0f * depth)
        return;

    /* Rear shadow / extrusion. */
    render_rect(effect, color_param, edge_dark,
                x + depth, y + depth, w, h);
    render_rect(effect, color_param, body, x, y, w, h);

    /* Top and left bevels. */
    render_rect(effect, color_param, edge_light, x, y, w, depth);
    render_rect(effect, color_param, edge_light, x, y, depth, h);

    /* Bottom and right bevels. */
    render_rect(effect, color_param, edge_dark,
                x, y + h - depth, w, depth);
    render_rect(effect, color_param, edge_dark,
                x + w - depth, y, depth, h);
}

static void render_screw(gs_effect_t *effect, gs_eparam_t *color_param,
                         float cx, float cy, float r)
{
    /* Pixel-art concentric screw: self-contained and deterministic. */
    render_rect(effect, color_param, 0xFF080A0D,
                cx - r, cy - r, 2.0f * r, 2.0f * r);
    render_rect(effect, color_param, 0xFF59616A,
                cx - r + 1.0f, cy - r + 1.0f,
                std::max(1.0f, 2.0f * r - 2.0f),
                std::max(1.0f, 2.0f * r - 2.0f));
    render_rect(effect, color_param, 0xFF171B20,
                cx - r + 3.0f, cy - 1.0f, 2.0f * r - 6.0f, 2.0f);
}

static void render_glass_bar(vu_meter *m, gs_effect_t *effect,
                             gs_eparam_t *color_param,
                             float x, float y, float w, float h,
                             float db, bool vertical)
{
    /* Deep well. */
    render_bevel_panel(effect, color_param, x, y, w, h,
                       0xFF0B0F13, 0xFF303840, 0xFF030405, 3.0f);
    render_segment_bar(m, effect, color_param,
                       x + 5.0f, y + 5.0f, w - 10.0f, h - 10.0f,
                       db, vertical);
    /* Glass highlight, deliberately subtle. */
    render_rect(effect, color_param, 0x18FFFFFF,
                x + 5.0f, y + 5.0f,
                std::max(1.0f, (w - 10.0f) * 0.10f),
                h - 10.0f);
}

static void render_segment_bar(vu_meter *m, gs_effect_t *effect,
                               gs_eparam_t *color_param,
                               float x, float y, float w, float h,
                               float db, bool vertical)
{
    if (!m || m->segments <= 0)
        return;

    const float frac = level_to_fraction(db, m->min_db);
    const float span = (vertical ? h : w) / (float)m->segments;
    const float gap = std::min((float)m->gap, std::max(0.0f, span - 1.0f));
    const uint32_t off = style_off_color(m);

    for (int i = 0; i < m->segments; ++i) {
        const float start = (float)i / (float)m->segments;
        const float end = (float)(i + 1) / (float)m->segments;
        const bool on = ((float)i + 1.0f) / (float)m->segments <= frac + 0.0001f;

        const float db_mid = m->min_db +
            ((float)i + 0.5f) / (float)m->segments * (-m->min_db);
        const uint32_t color = on ? segment_color(m, db_mid) : off;

        if (!vertical) {
            const float px = x + start * w;
            const float pw = std::max(1.0f, (end - start) * w - gap);
            render_rect(effect, color_param, color, px, y, pw, h);
        } else {
            /* Bottom-to-top: segment zero is at the bottom. */
            const float py = y + (1.0f - end) * h;
            const float ph = std::max(1.0f, (end - start) * h - gap);
            render_rect(effect, color_param, color, x, py, w, ph);
        }
    }
}

static void render_peak_segment(vu_meter *m, gs_effect_t *effect,
                                gs_eparam_t *color_param,
                                float x, float y, float w, float h,
                                float db, bool vertical, uint32_t color)
{
    if (!m || m->segments <= 0)
        return;

    const float frac = level_to_fraction(db, m->min_db);
    if (frac <= 0.0f)
        return;

    int index = (int)std::floor(frac * (float)m->segments);
    index = std::clamp(index, 0, m->segments - 1);

    const float start = (float)index / (float)m->segments;
    const float end = (float)(index + 1) / (float)m->segments;
    const float span = (vertical ? h : w) / (float)m->segments;
    const float gap = std::min((float)m->gap, std::max(0.0f, span - 1.0f));

    if (!vertical) {
        const float px = x + start * w;
        const float pw = std::max(1.0f, (end - start) * w - gap);
        render_rect(effect, color_param, color, px, y, pw, h);
    } else {
        const float py = y + (1.0f - end) * h;
        const float ph = std::max(1.0f, (end - start) * h - gap);
        render_rect(effect, color_param, color, x, py, w, ph);
    }
}

static void render_stereo_horizontal(vu_meter *m, gs_effect_t *effect,
                                      gs_eparam_t *color_param,
                                      float x, float y, float w, float h,
                                      bool framed, bool slim)
{
    const float pad = framed ? 18.0f : 6.0f;
    const float gap = slim ? 6.0f : 12.0f;
    const float bw = w - 2.0f * pad;
    const float bh = std::max(4.0f, (h - 2.0f * pad - gap) / 2.0f);

    if (framed)
        render_frame(effect, color_param, style_frame_color(m),
                     x, y, w, h, 3.0f);

    if (m->meter_mode == METER_SINGLE) {
        const int ch = m->mono_channel == 1 ? 1 : 0;
        render_segment_bar(m, effect, color_param,
                           x + pad, y + pad, bw, h - 2.0f * pad,
                           m->magnitude[ch], false);
        if (m->show_hold)
            render_peak_segment(m, effect, color_param,
                                x + pad, y + pad, bw, h - 2.0f * pad,
                                m->hold[ch], false, m->color_hold);
        if (m->show_peak)
            render_peak_segment(m, effect, color_param,
                                x + pad, y + pad, bw, h - 2.0f * pad,
                                m->peak[ch], false, m->color_peak);
        return;
    }

    render_segment_bar(m, effect, color_param,
                       x + pad, y + pad, bw, bh, m->magnitude[0], false);
    render_segment_bar(m, effect, color_param,
                       x + pad, y + pad + bh + gap, bw, bh, m->magnitude[1], false);

    if (m->show_hold) {
        render_peak_segment(m, effect, color_param,
                            x + pad, y + pad, bw, bh, m->hold[0], false, m->color_hold);
        render_peak_segment(m, effect, color_param,
                            x + pad, y + pad + bh + gap, bw, bh,
                            m->hold[1], false, m->color_hold);
    }

    if (m->show_peak) {
        render_peak_segment(m, effect, color_param,
                            x + pad, y + pad, bw, bh, m->peak[0], false, m->color_peak);
        render_peak_segment(m, effect, color_param,
                            x + pad, y + pad + bh + gap, bw, bh,
                            m->peak[1], false, m->color_peak);
    }
}

static void render_stereo_vertical(vu_meter *m, gs_effect_t *effect,
                                   gs_eparam_t *color_param,
                                   float x, float y, float w, float h,
                                   bool framed, bool slim)
{
    const float pad = framed ? 18.0f : 6.0f;
    const float gap = slim ? 6.0f : 12.0f;
    const float bw = std::max(4.0f, (w - 2.0f * pad - gap) / 2.0f);
    const float bh = h - 2.0f * pad;

    if (framed)
        render_frame(effect, color_param, style_frame_color(m),
                     x, y, w, h, 3.0f);

    if (m->meter_mode == METER_SINGLE) {
        const int ch = m->mono_channel == 1 ? 1 : 0;
        render_segment_bar(m, effect, color_param,
                           x + pad, y + pad, w - 2.0f * pad, bh,
                           m->magnitude[ch], true);
        if (m->show_hold)
            render_peak_segment(m, effect, color_param,
                                x + pad, y + pad, w - 2.0f * pad, bh,
                                m->hold[ch], true, m->color_hold);
        if (m->show_peak)
            render_peak_segment(m, effect, color_param,
                                x + pad, y + pad, w - 2.0f * pad, bh,
                                m->peak[ch], true, m->color_peak);
        return;
    }

    render_segment_bar(m, effect, color_param,
                       x + pad, y + pad, bw, bh, m->magnitude[0], true);
    render_segment_bar(m, effect, color_param,
                       x + pad + bw + gap, y + pad, bw, bh, m->magnitude[1], true);

    if (m->show_hold) {
        render_peak_segment(m, effect, color_param,
                            x + pad, y + pad, bw, bh, m->hold[0], true, m->color_hold);
        render_peak_segment(m, effect, color_param,
                            x + pad + bw + gap, y + pad, bw, bh,
                            m->hold[1], true, m->color_hold);
    }

    if (m->show_peak) {
        render_peak_segment(m, effect, color_param,
                            x + pad, y + pad, bw, bh, m->peak[0], true, m->color_peak);
        render_peak_segment(m, effect, color_param,
                            x + pad + bw + gap, y + pad, bw, bh,
                            m->peak[1], true, m->color_peak);
    }
}

/* ---------- Professional UI drawing helpers (v3.1) ---------- */
struct Glyph {
    char c;
    uint8_t rows[7];
};

/* Tiny 5x7 bitmap font. It keeps the plugin self-contained and avoids
   platform text APIs inside the OBS graphics render thread. */
static const Glyph FONT[] = {
    {'0',{14,17,19,21,25,17,14}}, {'1',{4,12,4,4,4,4,14}},
    {'2',{14,17,1,2,4,8,31}}, {'3',{30,1,1,14,1,1,30}},
    {'4',{2,6,10,18,31,2,2}}, {'5',{31,16,16,30,1,1,30}},
    {'6',{14,16,16,30,17,17,14}}, {'7',{31,1,2,4,8,8,8}},
    {'8',{14,17,17,14,17,17,14}}, {'9',{14,17,17,15,1,1,14}},
    {'A',{14,17,17,31,17,17,17}}, {'B',{30,17,17,30,17,17,30}},
    {'C',{14,17,16,16,16,17,14}}, {'D',{30,17,17,17,17,17,30}},
    {'E',{31,16,16,30,16,16,31}}, {'F',{31,16,16,30,16,16,16}},
    {'G',{14,17,16,23,17,17,14}}, {'H',{17,17,17,31,17,17,17}},
    {'I',{14,4,4,4,4,4,14}}, {'L',{16,16,16,16,16,16,31}},
    {'M',{17,27,21,21,17,17,17}}, {'N',{17,25,21,19,17,17,17}},
    {'O',{14,17,17,17,17,17,14}}, {'P',{30,17,17,30,16,16,16}},
    {'R',{30,17,17,30,20,18,17}}, {'S',{15,16,16,14,1,1,30}},
    {'T',{31,4,4,4,4,4,4}}, {'U',{17,17,17,17,17,17,14}},
    {'V',{17,17,17,17,17,10,4}}, {'W',{17,17,17,21,21,27,17}},
    {'X',{17,17,10,4,10,17,17}}, {'Y',{17,17,10,4,4,4,4}},
    {'Z',{31,1,2,4,8,16,31}},
    {'-',{0,0,0,31,0,0,0}}, {'+',{0,4,4,31,4,4,0}},
    {'.',{0,0,0,0,0,6,6}}, {':',{0,6,6,0,6,6,0}},
    {'/',{1,2,4,8,16,0,0}}, {' ',{0,0,0,0,0,0,0}}
};

static const Glyph *find_glyph(char c)
{
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    for (const auto &g : FONT)
        if (g.c == c)
            return &g;
    return find_glyph(' ');
}

static void draw_text(gs_effect_t *effect, gs_eparam_t *color_param,
                      const char *text, float x, float y, float scale,
                      uint32_t color)
{
    if (!text || scale <= 0.0f)
        return;
    const float advance = 6.0f * scale;
    for (const char *p = text; *p; ++p) {
        const Glyph *g = find_glyph(*p);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if (g->rows[row] & (1u << (4 - col)))
                    render_rect(effect, color_param, color,
                                x + col * scale, y + row * scale,
                                scale, scale);
            }
        }
        x += advance;
    }
}

static void draw_centered_text(gs_effect_t *effect, gs_eparam_t *color_param,
                               const char *text, float cx, float y,
                               float scale, uint32_t color)
{
    if (!text)
        return;
    size_t n = 0;
    while (text[n]) ++n;
    draw_text(effect, color_param, text,
              cx - ((float)n * 6.0f * scale - scale) * 0.5f,
              y, scale, color);
}

static bool layout_draws_background(const vu_meter *m)
{
    if (!m)
        return false;

    // In transparent/chroma modes the OBS-level background must remain
    // visible so the user can key it out. Professional/analog layouts
    // otherwise draw their own 3D chassis.
    if (m->background_mode == BG_TRANSPARENT ||
        m->background_mode == BG_CHROMA_GREEN ||
        m->background_mode == BG_CHROMA_BLUE)
        return false;

    return m->layout == LAYOUT_PRO_PANEL ||
           m->layout == LAYOUT_DOUBLE_SCALE ||
           m->layout == LAYOUT_VERTICAL_PANEL ||
           m->layout == LAYOUT_ANALOG ||
           m->layout == LAYOUT_ANALOG_VERTICAL;
}

static void render_professional_panel(vu_meter *m, gs_effect_t *effect,
                                       gs_eparam_t *color_param,
                                       float w, float h)
{
    /* v3.3: layered hardware chassis.  The audio path is untouched; this
       renderer only changes presentation. */
    uint32_t body = 0xFF20262C;
    uint32_t hi = 0xFF69727C;
    uint32_t dark = 0xFF07090B;
    uint32_t face = 0xFF11161B;
    uint32_t accent = 0xFF35E06F;

    switch (m->visual_style) {
    case STYLE_WHITE:
        body = 0xFFD5D9DD; hi = 0xFFF7F8F9; dark = 0xFF777D83; face = 0xFFEEF0F2;
        break;
    case STYLE_RETRO:
        body = 0xFF463A2A; hi = 0xFFC2A66F; dark = 0xFF17120C; face = 0xFF211A12;
        accent = 0xFFE0B85A;
        break;
    case STYLE_NEON:
        body = 0xFF111A22; hi = 0xFF00D9FF; dark = 0xFF030609; face = 0xFF071018;
        accent = 0xFF00D9FF;
        break;
    case STYLE_ANALOG:
        body = 0xFF403A31; hi = 0xFFB6A27E; dark = 0xFF110F0B; face = 0xFF211D17;
        accent = 0xFFD6B56A;
        break;
    case STYLE_DARK:
        body = 0xFF15191E; hi = 0xFF424A53; dark = 0xFF020304; face = 0xFF090D11;
        break;
    case STYLE_MINIMAL:
        body = 0xFF2B3036; hi = 0xFF8B939C; dark = 0xFF111418; face = 0xFF171B20;
        break;
    default:
        break;
    }

    if (layout_draws_background(m)) {
        /* Extruded chassis + front plate + recessed inner plate. */
        render_bevel_panel(effect, color_param, 3.0f, 4.0f, w - 6.0f, h - 6.0f,
                           dark, dark, 0xFF020304, 6.0f);
        render_bevel_panel(effect, color_param, 0.0f, 0.0f, w - 7.0f, h - 7.0f,
                           body, hi, dark, 5.0f);
        render_bevel_panel(effect, color_param, 14.0f, 15.0f, w - 35.0f, h - 35.0f,
                           face, 0xFF3D464F, 0xFF050608, 4.0f);
    } else {
        render_frame(effect, color_param, hi, 2.0f, 2.0f, w - 4.0f, h - 4.0f, 2.0f);
        render_frame(effect, color_param, 0xFF303840, 9.0f, 9.0f, w - 18.0f, h - 18.0f, 1.0f);
    }

    /* Realistic mounting screws. */
    render_screw(effect, color_param, 25.0f, 25.0f, 6.0f);
    render_screw(effect, color_param, w - 25.0f, 25.0f, 6.0f);
    render_screw(effect, color_param, 25.0f, h - 25.0f, 6.0f);
    render_screw(effect, color_param, w - 25.0f, h - 25.0f, 6.0f);

    /* Header strip and status LEDs. */
    render_bevel_panel(effect, color_param, 34.0f, 30.0f, w - 68.0f, 25.0f,
                       0xFF0A0E12, 0xFF343C44, 0xFF030405, 2.0f);
    draw_text(effect, color_param, "VU METER", 47.0f, 36.0f, 0.95f, 0xFFE8EBEE);
    draw_text(effect, color_param, "STUDIO", w - 103.0f, 37.0f, 0.65f, 0xFF89939D);
    render_rect(effect, color_param, accent, w - 54.0f, 38.0f, 7.0f, 7.0f);

    const float top = 69.0f;
    const float bottom = h - 48.0f;
    const float gap = 24.0f;
    const float left = 34.0f;
    const float usable = w - 2.0f * left - gap;
    const float cw = (m->meter_mode == METER_SINGLE) ? std::min(220.0f, usable) : usable * 0.5f;

    auto draw_channel = [&](float x, float db, float peak, float hold, const char *label) {
        const float ph = bottom - top;
        render_bevel_panel(effect, color_param, x, top, cw, ph,
                           0xFF0D1217, 0xFF4A545E, 0xFF030405, 3.0f);
        render_bevel_panel(effect, color_param, x + 8.0f, top + 8.0f, cw - 16.0f, ph - 16.0f,
                           0xFF070B0F, 0xFF252D35, 0xFF020304, 2.0f);

        draw_centered_text(effect, color_param, label, x + cw * 0.5f, top + 16.0f,
                           0.95f, 0xFFE5E9ED);

        /* Meter well is deliberately narrower and deeper, like a rack unit. */
        const float bar_w = std::max(30.0f, std::min(62.0f, cw * 0.24f));
        const float bar_x = x + (cw - bar_w) * 0.5f;
        const float bar_y = top + 48.0f;
        const float bar_h = ph - 93.0f;
        render_glass_bar(m, effect, color_param, bar_x, bar_y, bar_w, bar_h, db, true);

        if (m->show_hold)
            render_peak_segment(m, effect, color_param, bar_x + 5.0f, bar_y + 5.0f,
                                bar_w - 10.0f, bar_h - 10.0f, hold, true, m->color_hold);
        if (m->show_peak)
            render_peak_segment(m, effect, color_param, bar_x + 5.0f, bar_y + 5.0f,
                                bar_w - 10.0f, bar_h - 10.0f, peak, true, m->color_peak);

        char buf[40];
        std::snprintf(buf, sizeof(buf), "%+.1f dB", db);
        draw_centered_text(effect, color_param, buf, x + cw * 0.5f, bottom - 34.0f,
                           0.85f, 0xFFD5DADF);
        std::snprintf(buf, sizeof(buf), "PEAK %+.1f", peak);
        draw_centered_text(effect, color_param, buf, x + cw * 0.5f, bottom - 19.0f,
                           0.62f, 0xFF8F9AA5);

        /* Small physical indicator lamps. */
        render_rect(effect, color_param, segment_color(m, db), x + 14.0f, bottom - 22.0f, 5.0f, 5.0f);
        render_rect(effect, color_param, 0xFF252B31, x + 23.0f, bottom - 22.0f, 5.0f, 5.0f);
    };

    if (m->meter_mode == METER_SINGLE) {
        const int ch = m->mono_channel == 1 ? 1 : 0;
        draw_channel((w - cw) * 0.5f, m->magnitude[ch], m->peak[ch], m->hold[ch],
                     ch ? "RIGHT" : "LEFT");
    } else {
        draw_channel(left, m->magnitude[0], m->peak[0], m->hold[0], "LEFT");
        draw_channel(left + cw + gap, m->magnitude[1], m->peak[1], m->hold[1], "RIGHT");
    }

    /* Bottom status rail. */
    render_rect(effect, color_param, 0xFF080B0E, 34.0f, h - 35.0f, w - 68.0f, 12.0f);
    draw_text(effect, color_param, "LEVEL", 42.0f, h - 31.0f, 0.58f, 0xFF77818B);
    draw_text(effect, color_param, "dBFS", w - 75.0f, h - 31.0f, 0.58f, 0xFF77818B);
}


static void render_analog_professional(vu_meter *m, gs_effect_t *effect,
                                        gs_eparam_t *color_param,
                                        float x, float y, float w, float h,
                                        float db, const char *label)
{
    /* A true hardware-style analog meter: textured chassis, recessed cream
       dial, circular scale, major/minor ticks, red zone, needle and hub. */
    render_bevel_panel(effect, color_param, x, y, w, h,
                       0xFF25282B, 0xFF777D82, 0xFF070809, 6.0f);
    render_bevel_panel(effect, color_param, x + 7.0f, y + 7.0f,
                       w - 14.0f, h - 14.0f,
                       0xFF4A4C4C, 0xFF9A9D9C, 0xFF171818, 4.0f);

    const float ix = x + 16.0f;
    const float iy = y + 16.0f;
    const float iw = std::max(20.0f, w - 32.0f);
    const float ih = std::max(20.0f, h - 32.0f);
    render_bevel_panel(effect, color_param, ix, iy, iw, ih,
                       0xFFD8CCAA, 0xFFF7EBC5, 0xFF655B48, 3.0f);

    const float cx = x + w * 0.5f;
    const float cy = y + h * 0.70f;
    const float radius = std::max(20.0f, std::min(w * 0.43f, h * 0.49f));

    /* Dial face with subtle concentric depth. */
    render_circle(effect, color_param, 0xFFB8AC8E, cx + 2.0f, cy + 2.0f, radius + 2.0f);
    render_circle(effect, color_param, 0xFFE8DDBA, cx, cy, radius);
    render_ring(effect, color_param, 0xFF8B8068, cx, cy, radius, radius - 2.0f);
    render_ring(effect, color_param, 0xFFF5EBCB, cx, cy, radius - 4.0f, radius - 5.0f);

    draw_centered_text(effect, color_param, label, cx, y + 28.0f,
                       1.15f, 0xFF211D16);
    draw_centered_text(effect, color_param, "VU", cx, y + 45.0f,
                       0.72f, 0xFF756A54);

    /* Scale: -60 to 0 dB, with a clear red overload region. */
    const float start_angle = -2.52f;
    const float end_angle = -0.62f;
    for (int i = 0; i <= 30; ++i) {
        const float a = start_angle + (end_angle - start_angle) *
                        ((float)i / 30.0f);
        const bool major = (i % 5 == 0);
        const bool red = i >= 25;
        render_dial_tick(effect, color_param,
                         red ? 0xFFB32027 : 0xFF332D23,
                         cx, cy, a,
                         radius - 9.0f,
                         radius - (major ? 27.0f : 20.0f),
                         major ? 3.0f : 2.0f);
    }

    /* Numeric scale labels. */
    const char *labels[] = {"-60", "-40", "-20", "-10", "-5", "0"};
    for (int i = 0; i < 6; ++i) {
        const float a = start_angle + (end_angle - start_angle) *
                        ((float)i / 5.0f);
        const float lr = radius - 42.0f;
        draw_centered_text(effect, color_param, labels[i],
                           cx + std::cos(a) * lr,
                           cy + std::sin(a) * lr - 4.0f,
                           0.62f,
                           (i >= 4) ? 0xFF9C242A : 0xFF433A2B);
    }

    /* Red overload arc made from small illuminated marks. */
    for (int i = 25; i <= 30; ++i) {
        const float a = start_angle + (end_angle - start_angle) *
                        ((float)i / 30.0f);
        render_dial_tick(effect, color_param, 0xFFD83A40,
                         cx, cy, a, radius - 12.0f, radius - 7.0f, 3.0f);
    }

    const float frac = level_to_fraction(db, m->min_db);
    const float angle = start_angle + frac * (end_angle - start_angle);
    render_needle(effect, color_param, cx, cy, angle,
                  radius - 18.0f, 0xFF9D2027);

    /* Mechanical scale plate and readout. */
    render_bevel_panel(effect, color_param,
                       cx - 43.0f, y + h - 38.0f, 86.0f, 20.0f,
                       0xFF2A2721, 0xFF716A5B, 0xFF11100D, 2.0f);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%+.1f dB", db);
    draw_centered_text(effect, color_param, buf, cx, y + h - 33.0f,
                       0.68f, 0xFFF0E5C5);
}

static void render_layout(vu_meter *m, gs_effect_t *effect,
                          gs_eparam_t *color_param, float w, float h)
{
    switch (m->layout) {
    case LAYOUT_PRO_PANEL:
        render_professional_panel(m, effect, color_param, w, h);
        break;
    case LAYOUT_ANALOG:
        if (m->meter_mode == METER_SINGLE) {
            const int ch = m->mono_channel == 1 ? 1 : 0;
            render_analog_professional(m, effect, color_param, 0, 0, w, h,
                                       m->magnitude[ch], ch ? "RIGHT" : "LEFT");
        } else {
            const float gap = 12.0f, cw = (w - gap) * 0.5f;
            render_analog_professional(m, effect, color_param, 0, 0, cw, h,
                                       m->magnitude[0], "LEFT");
            render_analog_professional(m, effect, color_param, cw + gap, 0, cw, h,
                                       m->magnitude[1], "RIGHT");
        }
        break;
    case LAYOUT_DOUBLE_SCALE:
        render_professional_panel(m, effect, color_param, w, h);
        render_frame(effect, color_param, 0xFF6B7280, 10, 10, w - 20, h - 20, 1.0f);
        break;
    case LAYOUT_VERTICAL_PANEL:
        render_professional_panel(m, effect, color_param, w, h);
        break;
    case LAYOUT_ANALOG_VERTICAL:
        if (m->meter_mode == METER_SINGLE) {
            const int ch = m->mono_channel == 1 ? 1 : 0;
            render_analog_professional(m, effect, color_param, 0, 0, w, h,
                                       m->magnitude[ch], ch ? "RIGHT" : "LEFT");
        } else {
            const float gap = 12.0f, cw = (w - gap) * 0.5f;
            render_analog_professional(m, effect, color_param, 0, 0, cw, h,
                                       m->magnitude[0], "LEFT");
            render_analog_professional(m, effect, color_param, cw + gap, 0, cw, h,
                                       m->magnitude[1], "RIGHT");
        }
        break;
    case LAYOUT_SLIM_LED:
    case LAYOUT_SINGLE_SLIM:
    case LAYOUT_VERTICAL_LED:
    case LAYOUT_SINGLE:
    case LAYOUT_BASIC:
    default:
        /* The original v2.x LED renderer remains available as the compatibility
           layout. This is intentionally not reused by the Professional Panel. */
        if (m->layout == LAYOUT_VERTICAL_LED) {
            render_stereo_vertical(m, effect, color_param, 0, 0, w, h, true, true);
        } else if (m->layout == LAYOUT_SINGLE || m->layout == LAYOUT_SINGLE_SLIM) {
            render_stereo_horizontal(m, effect, color_param, 0, 0, w, h,
                                     true, m->layout == LAYOUT_SINGLE_SLIM);
        } else {
            render_stereo_horizontal(m, effect, color_param, 0, 0, w, h,
                                     true, m->layout == LAYOUT_SLIM_LED);
        }
        break;
    }
}

static void vu_render(void *data, gs_effect_t *)
{
    auto *m = static_cast<vu_meter *>(data);
    if (!m || !m->source)
        return;

    const uint32_t cx = obs_source_get_width(m->source);
    const uint32_t cy = obs_source_get_height(m->source);
    if (!cx || !cy)
        return;

    float magnitude[2], peak[2], hold[2];
    {
        std::lock_guard<std::mutex> lock(m->level_mutex);
        magnitude[0] = m->magnitude[0];
        magnitude[1] = m->magnitude[1];
        peak[0] = m->peak[0];
        peak[1] = m->peak[1];
        hold[0] = m->hold[0];
        hold[1] = m->hold[1];
    }

    /* Copy the current values back into local rendering state so the
       renderer remains stable even if the audio callback runs concurrently. */
    m->magnitude[0] = magnitude[0];
    m->magnitude[1] = magnitude[1];
    m->peak[0] = peak[0];
    m->peak[1] = peak[1];
    m->hold[0] = hold[0];
    m->hold[1] = hold[1];

    gs_blend_state_push();
    gs_enable_blending(true);
    gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

    gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
    if (!effect) {
        gs_blend_state_pop();
        return;
    }

    gs_eparam_t *color_param = gs_effect_get_param_by_name(effect, "color");
    if (!color_param) {
        gs_blend_state_pop();
        return;
    }

    if (m->background_mode != BG_TRANSPARENT) {
        uint32_t bg = style_background_color(m);
        if (m->background_mode == BG_CHROMA_GREEN)
            bg = 0xFF00FF00;
        else if (m->background_mode == BG_CHROMA_BLUE)
            bg = 0xFF0000FF;
        else if (m->background_mode == BG_BLACK)
            bg = 0xFF000000;
        render_rect(effect, color_param, bg, 0, 0, (float)cx, (float)cy);
    }

    render_layout(m, effect, color_param, (float)cx, (float)cy);
    gs_blend_state_pop();
}

static void vu_tick(void *data, float seconds)
{
    auto *m = static_cast<vu_meter *>(data);
    if (!m)
        return;

    const float dt = std::clamp(seconds, 0.0f, 0.25f);
    std::lock_guard<std::mutex> lock(m->level_mutex);

    for (int ch = 0; ch < 2; ++ch) {
        m->hold_age[ch] += dt;
        if (m->hold_age[ch] > m->peak_hold_seconds) {
            m->hold[ch] -= m->peak_decay_db_per_sec * dt;
            m->hold[ch] = std::max(m->hold[ch], m->peak[ch]);
        }
    }
}

static void *vu_create(obs_data_t *, obs_source_t *source)
{
    auto *m = new vu_meter();
    m->source = source;
    m->volmeter = obs_volmeter_create(OBS_FADER_LOG);

    if (m->volmeter)
        obs_volmeter_add_callback(m->volmeter, meter_callback, m);

    return m;
}

static void vu_destroy(void *data)
{
    auto *m = static_cast<vu_meter *>(data);
    if (!m)
        return;

    if (m->volmeter) {
        obs_volmeter_remove_callback(m->volmeter, meter_callback, m);
        obs_volmeter_detach_source(m->volmeter);
        obs_volmeter_destroy(m->volmeter);
        m->volmeter = nullptr;
    }

    if (m->attached_source) {
        obs_source_release(m->attached_source);
        m->attached_source = nullptr;
    }

    delete m;
}

static void vu_update(void *data, obs_data_t *settings)
{
    auto *m = static_cast<vu_meter *>(data);
    if (!m || !settings)
        return;

    const char *name = obs_data_get_string(settings, "source_name");
    obs_source_t *src = (name && *name) ? obs_get_source_by_name(name) : nullptr;
    set_source(m, src);
    if (src)
        obs_source_release(src);

    m->min_db = (float)obs_data_get_double(settings, "min_db");
    m->warning_db = (float)obs_data_get_double(settings, "warning_db");
    m->error_db = (float)obs_data_get_double(settings, "error_db");
    m->segments = std::clamp((int)obs_data_get_int(settings, "segments"), 8, 96);
    m->gap = std::clamp((int)obs_data_get_int(settings, "gap"), 0, 12);
    m->thickness = std::clamp((int)obs_data_get_int(settings, "thickness"), 2, 40);
    m->layout = std::clamp((int)obs_data_get_int(settings, "layout"), 0, 9);
    m->meter_mode = std::clamp((int)obs_data_get_int(settings, "meter_mode"), 0, 1);
    m->mono_channel = std::clamp((int)obs_data_get_int(settings, "mono_channel"), 0, 1);
    m->visual_style = std::clamp((int)obs_data_get_int(settings, "visual_style"), 0, 7);
    m->background_mode = std::clamp((int)obs_data_get_int(settings, "background_mode"), 0, 4);
    m->show_peak = obs_data_get_bool(settings, "show_peak");
    m->show_hold = obs_data_get_bool(settings, "show_hold");
    m->peak_hold_seconds = (float)obs_data_get_double(settings, "peak_hold");
    m->peak_decay_db_per_sec = (float)obs_data_get_double(settings, "peak_decay");

    m->color_green = migrate_color(obs_data_get_int(settings, "color_green"), 0xFF35E06F);
    m->color_yellow = migrate_color(obs_data_get_int(settings, "color_yellow"), 0xFFFFD84D);
    m->color_red = migrate_color(obs_data_get_int(settings, "color_red"), 0xFFFF3B30);
    m->color_off = migrate_color(obs_data_get_int(settings, "color_off"), 0xFF20252B);
    m->color_background = migrate_color(obs_data_get_int(settings, "color_background"), 0xB0101419);
    m->color_peak = migrate_color(obs_data_get_int(settings, "color_peak"), 0xFFFFFFFF);
    m->color_hold = migrate_color(obs_data_get_int(settings, "color_hold"), 0xFF00FFFF);
    m->color_frame = migrate_color(obs_data_get_int(settings, "color_frame"), 0xFF353B43);
}

static bool enum_source(void *param, obs_source_t *source)
{
    auto *list = static_cast<obs_property_t *>(param);
    if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) == 0)
        return true;

    const char *name = obs_source_get_name(source);
    if (name && *name)
        obs_property_list_add_string(list, name, name);

    return true;
}

static bool properties_modified(obs_properties_t *props,
                                 obs_property_t *,
                                 obs_data_t *settings)
{
    if (!props || !settings)
        return false;

    obs_property_t *channel = obs_properties_get(props, "mono_channel");
    if (channel)
        obs_property_set_visible(channel,
                                 obs_data_get_int(settings, "meter_mode") == METER_SINGLE);

    return true;
}

static obs_properties_t *vu_properties(void *)
{
    obs_properties_t *props = obs_properties_create();

    obs_property_t *source_list =
        obs_properties_add_list(props, "source_name", "Fonte de áudio",
                                OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(source_list, "-- nenhuma --", "");
    obs_enum_sources(enum_source, source_list);

    obs_property_t *layout =
        obs_properties_add_list(props, "layout", "Layout",
                                OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(layout, "1. Classic LED (base)", LAYOUT_BASIC);
    obs_property_list_add_int(layout, "2. Console 3D", LAYOUT_PRO_PANEL);
    obs_property_list_add_int(layout, "3. Slim LED 3D", LAYOUT_SLIM_LED);
    obs_property_list_add_int(layout, "4. Vintage Analog", LAYOUT_ANALOG);
    obs_property_list_add_int(layout, "5. Dual Scale Rack", LAYOUT_DOUBLE_SCALE);
    obs_property_list_add_int(layout, "6. Single Meter 3D", LAYOUT_SINGLE);
    obs_property_list_add_int(layout, "7. Single Slim 3D", LAYOUT_SINGLE_SLIM);
    obs_property_list_add_int(layout, "8. Vertical Console 3D", LAYOUT_VERTICAL_PANEL);
    obs_property_list_add_int(layout, "9. Vertical LED 3D", LAYOUT_VERTICAL_LED);
    obs_property_list_add_int(layout, "10. Vintage Analog Vertical", LAYOUT_ANALOG_VERTICAL);

    obs_property_t *mode =
        obs_properties_add_list(props, "meter_mode", "Modo do medidor",
                                OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(mode, "Estéreo — L + R", METER_STEREO);
    obs_property_list_add_int(mode, "Barra única", METER_SINGLE);

    obs_property_t *channel =
        obs_properties_add_list(props, "mono_channel", "Canal da barra única",
                                OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(channel, "L — Esquerdo", 0);
    obs_property_list_add_int(channel, "R — Direito", 1);
    obs_property_set_modified_callback(mode, properties_modified);
    obs_property_set_visible(channel, false);

    obs_property_t *style =
        obs_properties_add_list(props, "visual_style", "Estilo visual",
                                OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(style, "Chassi Grafite", STYLE_PRO);
    obs_property_list_add_int(style, "LED Moderno", STYLE_LED);
    obs_property_list_add_int(style, "Painel Analógico", STYLE_ANALOG);
    obs_property_list_add_int(style, "Minimalista", STYLE_MINIMAL);
    obs_property_list_add_int(style, "Neon / Cyan", STYLE_NEON);
    obs_property_list_add_int(style, "Console Prata", STYLE_WHITE);
    obs_property_list_add_int(style, "Dark Rack", STYLE_DARK);
    obs_property_list_add_int(style, "Vintage", STYLE_RETRO);

    obs_property_t *bg =
        obs_properties_add_list(props, "background_mode", "Fundo / Chroma Key",
                                OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(bg, "Normal", BG_NORMAL);
    obs_property_list_add_int(bg, "Transparente", BG_TRANSPARENT);
    obs_property_list_add_int(bg, "Chroma Key Verde", BG_CHROMA_GREEN);
    obs_property_list_add_int(bg, "Chroma Key Azul", BG_CHROMA_BLUE);
    obs_property_list_add_int(bg, "Preto", BG_BLACK);

    obs_properties_add_int(props, "segments", "Segmentos LED", 8, 96, 1);
    obs_properties_add_float_slider(props, "min_db", "Escala mínima (dB)", -80.0, -20.0, 1.0);
    obs_properties_add_float_slider(props, "warning_db", "Início do amarelo (dB)", -40.0, -1.0, 1.0);
    obs_properties_add_float_slider(props, "error_db", "Início do vermelho (dB)", -20.0, 0.0, 1.0);
    obs_properties_add_int(props, "gap", "Espaçamento entre segmentos", 0, 12, 1);
    obs_properties_add_int(props, "thickness", "Espessura visual", 2, 40, 1);

    obs_properties_add_bool(props, "show_peak", "Mostrar Peak");
    obs_properties_add_bool(props, "show_hold", "Mostrar Peak Hold");
    obs_properties_add_float_slider(props, "peak_hold", "Tempo do Peak Hold (s)", 0.1, 10.0, 0.1);
    obs_properties_add_float_slider(props, "peak_decay", "Queda do Peak (dB/s)", 1.0, 60.0, 1.0);

    obs_properties_add_color(props, "color_green", "Cor nível verde");
    obs_properties_add_color(props, "color_yellow", "Cor nível amarelo");
    obs_properties_add_color(props, "color_red", "Cor nível vermelho");
    obs_properties_add_color(props, "color_off", "Cor LED desligado");
    obs_properties_add_color_alpha(props, "color_background", "Cor de fundo");
    obs_properties_add_color(props, "color_peak", "Cor Peak");
    obs_properties_add_color(props, "color_hold", "Cor Peak Hold");
    obs_properties_add_color(props, "color_frame", "Cor moldura");

    return props;
}

static void vu_defaults(obs_data_t *settings)
{
    obs_data_set_default_string(settings, "source_name", "");
    obs_data_set_default_int(settings, "layout", LAYOUT_BASIC);
    obs_data_set_default_int(settings, "meter_mode", METER_STEREO);
    obs_data_set_default_int(settings, "mono_channel", 0);
    obs_data_set_default_int(settings, "visual_style", STYLE_PRO);
    obs_data_set_default_int(settings, "background_mode", BG_NORMAL);

    obs_data_set_default_int(settings, "segments", 32);
    obs_data_set_default_double(settings, "min_db", DEFAULT_MIN_DB);
    obs_data_set_default_double(settings, "warning_db", DEFAULT_WARN_DB);
    obs_data_set_default_double(settings, "error_db", DEFAULT_ERROR_DB);
    obs_data_set_default_int(settings, "gap", 3);
    obs_data_set_default_int(settings, "thickness", 10);

    obs_data_set_default_bool(settings, "show_peak", true);
    obs_data_set_default_bool(settings, "show_hold", true);
    obs_data_set_default_double(settings, "peak_hold", 1.5);
    obs_data_set_default_double(settings, "peak_decay", 18.0);

    obs_data_set_default_int(settings, "color_green", 0xFF35E06F);
    obs_data_set_default_int(settings, "color_yellow", 0xFFFFD84D);
    obs_data_set_default_int(settings, "color_red", 0xFFFF3B30);
    obs_data_set_default_int(settings, "color_off", 0xFF20252B);
    obs_data_set_default_int(settings, "color_background", 0xB0101419);
    obs_data_set_default_int(settings, "color_peak", 0xFFFFFFFF);
    obs_data_set_default_int(settings, "color_hold", 0xFF00FFFF);
    obs_data_set_default_int(settings, "color_frame", 0xFF353B43);
}

static uint32_t vu_width(void *data)
{
    auto *m = static_cast<vu_meter *>(data);
    switch (m ? m->layout : LAYOUT_BASIC) {
    case LAYOUT_VERTICAL_PANEL:
    case LAYOUT_VERTICAL_LED:
        return 220;
    case LAYOUT_ANALOG_VERTICAL:
        return 300;
    case LAYOUT_PRO_PANEL:
    case LAYOUT_ANALOG:
    case LAYOUT_DOUBLE_SCALE:
        return 760;
    default:
        return 640;
    }
}

static uint32_t vu_height(void *data)
{
    auto *m = static_cast<vu_meter *>(data);
    switch (m ? m->layout : LAYOUT_BASIC) {
    case LAYOUT_VERTICAL_PANEL:
        return 520;
    case LAYOUT_VERTICAL_LED:
        return 420;
    case LAYOUT_ANALOG_VERTICAL:
        return 520;
    case LAYOUT_PRO_PANEL:
        return 520;
    case LAYOUT_ANALOG:
        return 300;
    case LAYOUT_DOUBLE_SCALE:
        return 180;
    case LAYOUT_SINGLE:
    case LAYOUT_SINGLE_SLIM:
        return 120;
    default:
        return 120;
    }
}

} // namespace

struct obs_source_info vu_meter_source_info = {
    .id = "obs_vu_meter_pro",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,

    .get_name = vu_name,
    .create = vu_create,
    .destroy = vu_destroy,

    .get_width = vu_width,
    .get_height = vu_height,

    .get_defaults = vu_defaults,
    .get_properties = vu_properties,
    .update = vu_update,

    .video_tick = vu_tick,
    .video_render = vu_render,

    .icon_type = OBS_ICON_TYPE_CUSTOM,
};
