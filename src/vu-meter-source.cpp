#include "vu-meter-source.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr float DEFAULT_MIN_DB = -60.0f;
constexpr float DEFAULT_WARN_DB = -18.0f;
constexpr float DEFAULT_ERROR_DB = -6.0f;
constexpr float DEFAULT_CLIP_DB = 0.0f;

struct vu_meter {
    obs_source_t *source = nullptr;
    obs_volmeter_t *volmeter = nullptr;
    obs_source_t *attached_source = nullptr;

    std::mutex level_mutex;
    float magnitude[2] = {DEFAULT_MIN_DB, DEFAULT_MIN_DB};
    float peak[2] = {DEFAULT_MIN_DB, DEFAULT_MIN_DB};
    float hold[2] = {DEFAULT_MIN_DB, DEFAULT_MIN_DB};
    bool clipped[2] = {false, false};

    float min_db = DEFAULT_MIN_DB;
    float warning_db = DEFAULT_WARN_DB;
    float error_db = DEFAULT_ERROR_DB;
    float clip_db = DEFAULT_CLIP_DB;

    int segments = 32;
    int gap = 3;
    int thickness = 10;
    int direction = 0; // 0 horizontal, 1 vertical
    bool show_peak = true;
    bool show_hold = true;
    float peak_hold_seconds = 1.5f;
    float peak_decay_db_per_sec = 18.0f;
    bool rounded = false; // reserved for v1.1 renderer

    uint32_t color_green = 0xFF35E06F;
    uint32_t color_yellow = 0xFFFFD84D;
    uint32_t color_red = 0xFFFF3B30;
    uint32_t color_off = 0xFF20252B;
    uint32_t color_background = 0xB0101419;
    uint32_t color_peak = 0xFFFFFFFF;

    float hold_age[2] = {0.0f, 0.0f};
    float last_tick = 0.0f;
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

static uint32_t segment_color(const vu_meter *m, float segment_db)
{
    if (segment_db >= m->error_db)
        return m->color_red;
    if (segment_db >= m->warning_db)
        return m->color_yellow;
    return m->color_green;
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
        const float p = clamp_db(peak[ch], m->min_db);
        const float mag = clamp_db(magnitude[ch], m->min_db);
        m->peak[ch] = p;
        m->magnitude[ch] = mag;
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

    /*
     * OBS's OBS_EFFECT_SOLID is the same solid-color technique used by
     * OBS itself for simple rectangles.  Keep the draw operation isolated
     * so every rectangle gets its own color and transform.
     */
    gs_effect_set_color(color_param, color);

    gs_matrix_push();
    gs_matrix_translate3f(x, y, 0.0f);

    while (gs_effect_loop(effect, "Solid"))
        gs_draw_quadf(nullptr, 0, w, h);

    gs_matrix_pop();
}

static void render_bar(vu_meter *m, gs_effect_t *effect, gs_eparam_t *color_param,
                       float x, float y, float w, float h, float db)
{
    if (!m || !effect || !color_param || m->segments <= 0)
        return;

    const float frac = level_to_fraction(db, m->min_db);
    const float segment_width = w / (float)m->segments;

    for (int i = 0; i < m->segments; ++i) {
        const float start = (float)i / (float)m->segments;
        const float end = (float)(i + 1) / (float)m->segments;

        const float pos0 = (m->direction == 0) ? x + start * w : y + start * h;
        const float pos1 = (m->direction == 0) ? x + end * w : y + end * h;

        const float size = std::max(1.0f, pos1 - pos0 - (float)m->gap);

        const bool on =
            ((float)i + 1.0f) / (float)m->segments <= frac + 0.0001f;

        const float segment_db =
            m->min_db +
            ((float)i + 0.5f) / (float)m->segments * (-m->min_db);

        const uint32_t color =
            on ? segment_color(m, segment_db) : m->color_off;

        if (m->direction == 0)
            render_rect(effect, color_param, color, pos0, y, size, h);
        else
            render_rect(effect, color_param, color, x, pos0, w, size);
    }

    (void)segment_width;
}

static void vu_render(void *data, gs_effect_t *effect_unused)
{
    auto *m = static_cast<vu_meter *>(data);
    if (!m || !m->source)
        return;

    const uint32_t cx = obs_source_get_width(m->source);
    const uint32_t cy = obs_source_get_height(m->source);

    if (!cx || !cy)
        return;

    float magnitude[2];
    float peak[2];
    float hold[2];
    int direction;
    bool show_hold;
    uint32_t color_background;
    uint32_t color_peak;
    float min_db;

    {
        std::lock_guard<std::mutex> lock(m->level_mutex);

        magnitude[0] = m->magnitude[0];
        magnitude[1] = m->magnitude[1];
        peak[0] = m->peak[0];
        peak[1] = m->peak[1];
        hold[0] = m->hold[0];
        hold[1] = m->hold[1];

        direction = m->direction;
        show_hold = m->show_hold;
        color_background = m->color_background;
        color_peak = m->color_peak;
        min_db = m->min_db;
    }

    (void)effect_unused;
    (void)peak;

    /*
     * IMPORTANT:
     * A custom-draw source is rendered inside the graphics state prepared
     * by OBS.  Do not assume that the projection, model matrix or viewport
     * already correspond to this source's own dimensions.
     *
     * Version 2.1 explicitly establishes a source-local 2D rendering space.
     * This avoids the situation where the render callback is receiving and
     * calculating audio correctly, but the rectangles are transformed
     * outside the source texture.
     */
    gs_viewport_push();
    gs_projection_push();
    gs_matrix_push();
    gs_matrix_identity();

    gs_set_viewport(0, 0, (int)cx, (int)cy);
    gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);

    /*
     * Make the custom source independent of the blend state inherited from
     * whatever source was rendered before it.
     */
    gs_blend_state_push();
    gs_enable_blending(true);
    gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

    gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
    if (!effect) {
        gs_blend_state_pop();
        gs_matrix_pop();
        gs_projection_pop();
        gs_viewport_pop();
        return;
    }

    gs_eparam_t *color_param =
        gs_effect_get_param_by_name(effect, "color");

    if (!color_param) {
        gs_blend_state_pop();
        gs_matrix_pop();
        gs_projection_pop();
        gs_viewport_pop();
        return;
    }

    /*
     * Diagnostic: prove that the render thread sees the same changing
     * levels received by the audio callback.
     */
    static float debug_elapsed = 0.0f;
    debug_elapsed += 1.0f / 60.0f;

    if (debug_elapsed >= 1.0f) {
        debug_elapsed = 0.0f;

        blog(LOG_INFO,
             "[OBS VU Meter PRO 2.1] render L=%.1f dB (%.3f) "
             "R=%.1f dB (%.3f)",
             magnitude[0], level_to_fraction(magnitude[0], min_db),
             magnitude[1], level_to_fraction(magnitude[1], min_db));
    }

    const float outer = 6.0f;

    /* Draw the source background first. */
    render_rect(effect, color_param, color_background,
                0.0f, 0.0f, (float)cx, (float)cy);

    if (direction == 0) {
        const float bar_h =
            std::max(1.0f, ((float)cy - outer * 3.0f) / 2.0f);
        const float bar_w =
            std::max(1.0f, (float)cx - outer * 2.0f);

        render_bar(m, effect, color_param,
                   outer, outer, bar_w, bar_h, magnitude[0]);

        render_bar(m, effect, color_param,
                   outer, outer * 2.0f + bar_h,
                   bar_w, bar_h, magnitude[1]);

        if (show_hold) {
            const float hold_w1 =
                bar_w * level_to_fraction(hold[0], min_db);
            const float hold_w2 =
                bar_w * level_to_fraction(hold[1], min_db);

            const float hold_y1 = outer + bar_h - 2.0f;
            const float hold_y2 =
                outer * 2.0f + bar_h + bar_h - 2.0f;

            render_rect(effect, color_param, color_peak,
                        outer + std::max(0.0f, hold_w1 - 1.0f),
                        hold_y1, 2.0f, 2.0f);

            render_rect(effect, color_param, color_peak,
                        outer + std::max(0.0f, hold_w2 - 1.0f),
                        hold_y2, 2.0f, 2.0f);
        }
    } else {
        const float bar_w =
            std::max(1.0f, ((float)cx - outer * 3.0f) / 2.0f);
        const float bar_h =
            std::max(1.0f, (float)cy - outer * 2.0f);

        render_bar(m, effect, color_param,
                   outer, outer, bar_w, bar_h, magnitude[0]);

        render_bar(m, effect, color_param,
                   outer * 2.0f + bar_w, outer,
                   bar_w, bar_h, magnitude[1]);
    }

    gs_blend_state_pop();
    gs_matrix_pop();
    gs_projection_pop();
    gs_viewport_pop();
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

static void *vu_create(obs_data_t *settings, obs_source_t *source)
{
    (void)settings;

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
    m->direction = (int)obs_data_get_int(settings, "direction");
    m->show_peak = obs_data_get_bool(settings, "show_peak");
    m->show_hold = obs_data_get_bool(settings, "show_hold");
    m->peak_hold_seconds = (float)obs_data_get_double(settings, "peak_hold");
    m->peak_decay_db_per_sec = (float)obs_data_get_double(settings, "peak_decay");

    m->color_green = (uint32_t)obs_data_get_int(settings, "color_green");
    m->color_yellow = (uint32_t)obs_data_get_int(settings, "color_yellow");
    m->color_red = (uint32_t)obs_data_get_int(settings, "color_red");
    m->color_off = (uint32_t)obs_data_get_int(settings, "color_off");
    m->color_background = (uint32_t)obs_data_get_int(settings, "color_background");
    m->color_peak = (uint32_t)obs_data_get_int(settings, "color_peak");
}

static bool enum_source(void *param, obs_source_t *source)
{
    auto *list = static_cast<obs_property_t *>(param);
    const uint32_t flags = obs_source_get_output_flags(source);
    if ((flags & OBS_SOURCE_AUDIO) != 0) {
        const char *name = obs_source_get_name(source);
        if (name && *name)
            obs_property_list_add_string(list, name, name);
    }
    return true;
}

static obs_properties_t *vu_properties(void *data)
{
    (void)data;
    obs_properties_t *props = obs_properties_create();

    obs_property_t *source_list = obs_properties_add_list(props, "source_name", "Fonte de áudio",
                                                            OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(source_list, "-- nenhuma --", "");
    obs_enum_sources(enum_source, source_list);

    obs_properties_add_int(props, "segments", "Segmentos", 8, 96, 1);
    obs_properties_add_float_slider(props, "min_db", "Escala mínima (dB)", -80.0, -20.0, 1.0);
    obs_properties_add_float_slider(props, "warning_db", "Início do amarelo (dB)", -40.0, -1.0, 1.0);
    obs_properties_add_float_slider(props, "error_db", "Início do vermelho (dB)", -20.0, 0.0, 1.0);
    obs_properties_add_int(props, "gap", "Espaçamento", 0, 12, 1);
    obs_properties_add_int(props, "thickness", "Espessura", 2, 40, 1);

    obs_property_t *direction = obs_properties_add_list(props, "direction", "Orientação",
                                                          OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(direction, "Horizontal", 0);
    obs_property_list_add_int(direction, "Vertical", 1);

    obs_properties_add_bool(props, "show_peak", "Mostrar Peak");
    obs_properties_add_bool(props, "show_hold", "Mostrar Peak Hold");
    obs_properties_add_float_slider(props, "peak_hold", "Tempo do Peak Hold (s)", 0.1, 10.0, 0.1);
    obs_properties_add_float_slider(props, "peak_decay", "Queda do Peak (dB/s)", 1.0, 60.0, 1.0);

    obs_properties_add_color(props, "color_green", "Cor verde");
    obs_properties_add_color(props, "color_yellow", "Cor amarela");
    obs_properties_add_color(props, "color_red", "Cor vermelha");
    obs_properties_add_color(props, "color_off", "LED apagado");
    obs_properties_add_color_alpha(props, "color_background", "Fundo");
    obs_properties_add_color(props, "color_peak", "Peak Hold");

    return props;
}

static void vu_defaults(obs_data_t *settings)
{
    obs_data_set_default_string(settings, "source_name", "");
    obs_data_set_default_int(settings, "segments", 32);
    obs_data_set_default_double(settings, "min_db", DEFAULT_MIN_DB);
    obs_data_set_default_double(settings, "warning_db", DEFAULT_WARN_DB);
    obs_data_set_default_double(settings, "error_db", DEFAULT_ERROR_DB);
    obs_data_set_default_int(settings, "gap", 3);
    obs_data_set_default_int(settings, "thickness", 10);
    obs_data_set_default_int(settings, "direction", 0);
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
}

static uint32_t vu_width(void *data)
{
    auto *m = static_cast<vu_meter *>(data);
    return m && m->direction == 1 ? 180 : 640;
}

static uint32_t vu_height(void *data)
{
    auto *m = static_cast<vu_meter *>(data);
    return m && m->direction == 1 ? 420 : 120;
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
