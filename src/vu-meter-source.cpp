#include "vu-meter-source.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>

namespace {
constexpr float DEFAULT_MIN_DB = -60.0f;
constexpr float DEFAULT_WARN_DB = -18.0f;
constexpr float DEFAULT_ERROR_DB = -6.0f;

struct vu_meter {
    obs_source_t *source = nullptr;
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
    int direction = 0;
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
};

static const char *vu_name(void *) { return "VU Meter PRO"; }

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
    return std::clamp((db - min_db) / (-min_db), 0.0f, 1.0f);
}

static uint32_t segment_color(const vu_meter *m, float db)
{
    if (db >= m->error_db) return m->color_red;
    if (db >= m->warning_db) return m->color_yellow;
    return m->color_green;
}

/* Direct OBS audio capture. This bypasses the previous volmeter path. */
static void audio_capture_callback(void *param, obs_source_t *source,
                                   const struct audio_data *audio_data, bool muted)
{
    auto *m = static_cast<vu_meter *>(param);
    if (!m || !audio_data || source != m->attached_source || !audio_data->frames)
        return;

    const float volume = std::max(0.0f, obs_source_get_volume(source));
    float rms_db[2] = {m->min_db, m->min_db};
    float peak_db[2] = {m->min_db, m->min_db};

    int ch = 0;
    for (int plane = 0; plane < MAX_AV_PLANES && ch < 2; ++plane) {
        if (!audio_data->data[plane])
            continue;

        const float *samples =
            reinterpret_cast<const float *>(audio_data->data[plane]);

        double sum = 0.0;
        float peak = 0.0f;

        for (uint32_t i = 0; i < audio_data->frames; ++i) {
            const float a = std::fabs(samples[i]);
            sum += static_cast<double>(a) * static_cast<double>(a);
            peak = std::max(peak, a);
        }

        if (!muted && !obs_source_muted(source)) {
            const float rms =
                static_cast<float>(std::sqrt(sum / audio_data->frames));
            rms_db[ch] = clamp_db(obs_mul_to_db(rms * volume), m->min_db);
            peak_db[ch] = clamp_db(obs_mul_to_db(peak * volume), m->min_db);
        }
        ++ch;
    }

    std::lock_guard<std::mutex> lock(m->level_mutex);
    for (int i = 0; i < 2; ++i) {
        m->magnitude[i] = rms_db[i];
        m->peak[i] = peak_db[i];
        if (peak_db[i] >= m->hold[i]) {
            m->hold[i] = peak_db[i];
            m->hold_age[i] = 0.0f;
        }
    }
}

static void detach_audio_source(vu_meter *m)
{
    if (!m || !m->attached_source)
        return;

    obs_source_remove_audio_capture_callback(
        m->attached_source, audio_capture_callback, m);
    obs_source_release(m->attached_source);
    m->attached_source = nullptr;

    std::lock_guard<std::mutex> lock(m->level_mutex);
    for (int i = 0; i < 2; ++i) {
        m->magnitude[i] = m->min_db;
        m->peak[i] = m->min_db;
        m->hold[i] = m->min_db;
        m->hold_age[i] = 0.0f;
    }
}

static void set_source(vu_meter *m, obs_source_t *src)
{
    if (m->attached_source == src)
        return;

    detach_audio_source(m);

    if (src) {
        m->attached_source = obs_source_get_ref(src);
        obs_source_add_audio_capture_callback(
            m->attached_source, audio_capture_callback, m);
        blog(LOG_INFO, "[OBS VU Meter PRO] audio attached: %s",
             obs_source_get_name(m->attached_source));
    } else {
        blog(LOG_INFO, "[OBS VU Meter PRO] no audio source selected");
    }
}

static void render_rect(gs_effect_t *effect, gs_eparam_t *color_param,
                        uint32_t color, float x, float y, float w, float h)
{
    if (w <= 0.0f || h <= 0.0f) return;
    gs_effect_set_color(color_param, color);
    gs_matrix_push();
    gs_matrix_translate3f(x, y, 0.0f);
    while (gs_effect_loop(effect, "Solid"))
        gs_draw_sprite(nullptr, 0, (uint32_t)std::ceil(w),
                       (uint32_t)std::ceil(h));
    gs_matrix_pop();
}

static void render_bar(vu_meter *m, gs_effect_t *effect,
                       gs_eparam_t *color_param, float x, float y,
                       float w, float h, float db)
{
    const float frac = level_to_fraction(db, m->min_db);

    for (int i = 0; i < m->segments; ++i) {
        const float start = (float)i / m->segments;
        const float end = (float)(i + 1) / m->segments;
        const float p0 = m->direction == 0 ? x + start * w : y + start * h;
        const float p1 = m->direction == 0 ? x + end * w : y + end * h;
        const float size = std::max(0.0f, p1 - p0 - (float)m->gap);
        const bool on = (float)i < frac * m->segments;
        const float sdb = m->min_db +
            ((float)i + 0.5f) / m->segments * (-m->min_db);
        const uint32_t color = on ? segment_color(m, sdb) : m->color_off;

        if (m->direction == 0)
            render_rect(effect, color_param, color, p0, y, size, h);
        else
            render_rect(effect, color_param, color, x, p0, w, size);
    }
}

static void vu_render(void *data, gs_effect_t *)
{
    auto *m = static_cast<vu_meter *>(data);
    if (!m) return;

    const uint32_t cx = obs_source_get_width(m->source);
    const uint32_t cy = obs_source_get_height(m->source);
    if (!cx || !cy) return;

    float peak[2], hold[2];
    {
        std::lock_guard<std::mutex> lock(m->level_mutex);
        peak[0] = m->peak[0]; peak[1] = m->peak[1];
        hold[0] = m->hold[0]; hold[1] = m->hold[1];
    }

    gs_set_2d_mode();
    gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
    if (!effect) return;
    gs_eparam_t *color_param =
        gs_effect_get_param_by_name(effect, "color");
    if (!color_param) return;

    render_rect(effect, color_param, m->color_background,
                0, 0, (float)cx, (float)cy);

    const float outer = 6.0f;
    if (m->direction == 0) {
        const float bar_h =
            std::max(1.0f, ((float)cy - outer * 3.0f) / 2.0f);
        const float bar_w = (float)cx - outer * 2.0f;

        render_bar(m, effect, color_param, outer, outer,
                   bar_w, bar_h, peak[0]);
        render_bar(m, effect, color_param, outer,
                   outer * 2.0f + bar_h, bar_w, bar_h, peak[1]);

        if (m->show_hold) {
            const float hw = bar_w * level_to_fraction(hold[0], m->min_db);
            const float hw2 = bar_w * level_to_fraction(hold[1], m->min_db);
            render_rect(effect, color_param, m->color_peak,
                        outer + hw - 1.0f, outer + bar_h - 2.0f, 2, 2);
            render_rect(effect, color_param, m->color_peak,
                        outer + hw2 - 1.0f,
                        outer * 2.0f + bar_h + bar_h - 2.0f, 2, 2);
        }
    } else {
        const float bar_w =
            std::max(1.0f, ((float)cx - outer * 3.0f) / 2.0f);
        const float bar_h = (float)cy - outer * 2.0f;
        render_bar(m, effect, color_param, outer, outer,
                   bar_w, bar_h, peak[0]);
        render_bar(m, effect, color_param,
                   outer * 2.0f + bar_w, outer,
                   bar_w, bar_h, peak[1]);
    }
}

static void vu_tick(void *data, float seconds)
{
    auto *m = static_cast<vu_meter *>(data);
    if (!m) return;

    const float dt = std::clamp(seconds, 0.0f, 0.25f);
    std::lock_guard<std::mutex> lock(m->level_mutex);

    for (int i = 0; i < 2; ++i) {
        m->hold_age[i] += dt;
        if (m->hold_age[i] > m->peak_hold_seconds) {
            m->hold[i] = std::max(m->hold[i] -
                m->peak_decay_db_per_sec * dt, m->peak[i]);
            m->hold[i] = std::max(m->hold[i], m->min_db);
        }
    }
}

static void *vu_create(obs_data_t *, obs_source_t *source)
{
    auto *m = new vu_meter();
    m->source = source;
    return m;
}

static void vu_destroy(void *data)
{
    auto *m = static_cast<vu_meter *>(data);
    if (!m) return;
    detach_audio_source(m);
    delete m;
}

static void vu_update(void *data, obs_data_t *settings)
{
    auto *m = static_cast<vu_meter *>(data);
    if (!m || !settings) return;

    const char *name = obs_data_get_string(settings, "source_name");
    obs_source_t *src =
        (name && *name) ? obs_get_source_by_name(name) : nullptr;

    set_source(m, src);
    if (src) obs_source_release(src);

    m->min_db = (float)obs_data_get_double(settings, "min_db");
    m->warning_db = (float)obs_data_get_double(settings, "warning_db");
    m->error_db = (float)obs_data_get_double(settings, "error_db");
    m->segments = std::clamp((int)obs_data_get_int(settings, "segments"), 8, 96);
    m->gap = std::clamp((int)obs_data_get_int(settings, "gap"), 0, 12);
    m->thickness = std::clamp((int)obs_data_get_int(settings, "thickness"), 2, 40);
    m->direction = std::clamp((int)obs_data_get_int(settings, "direction"), 0, 1);
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
    if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) == 0)
        return true;

    const char *name = obs_source_get_name(source);
    if (name && *name)
        obs_property_list_add_string(list, name, name);
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

    obs_properties_add_int(props, "segments", "Segmentos", 8, 96, 1);
    obs_properties_add_float_slider(props, "min_db", "Escala mínima (dB)",
                                     -80.0, -20.0, 1.0);
    obs_properties_add_float_slider(props, "warning_db", "Início do amarelo (dB)",
                                     -40.0, -1.0, 1.0);
    obs_properties_add_float_slider(props, "error_db", "Início do vermelho (dB)",
                                     -20.0, 0.0, 1.0);
    obs_properties_add_int(props, "gap", "Espaçamento", 0, 12, 1);
    obs_properties_add_int(props, "thickness", "Espessura", 2, 40, 1);

    obs_property_t *direction =
        obs_properties_add_list(props, "direction", "Orientação",
                                OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(direction, "Horizontal", 0);
    obs_property_list_add_int(direction, "Vertical", 1);

    obs_properties_add_bool(props, "show_peak", "Mostrar Peak");
    obs_properties_add_bool(props, "show_hold", "Mostrar Peak Hold");
    obs_properties_add_float_slider(props, "peak_hold",
                                     "Tempo do Peak Hold (s)", 0.1, 10.0, 0.1);
    obs_properties_add_float_slider(props, "peak_decay",
                                     "Queda do Peak (dB/s)", 1.0, 60.0, 1.0);

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

/*
 * Initialized field-by-field to avoid C++20 designated-initializer ordering
 * problems with OBS 31.x headers.
 */
struct obs_source_info vu_meter_source_info = {};

static void init_source_info()
{
    vu_meter_source_info.id = "obs_vu_meter_pro";
    vu_meter_source_info.type = OBS_SOURCE_TYPE_INPUT;
    vu_meter_source_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
    vu_meter_source_info.get_name = vu_name;
    vu_meter_source_info.create = vu_create;
    vu_meter_source_info.destroy = vu_destroy;
    vu_meter_source_info.update = vu_update;
    vu_meter_source_info.video_tick = vu_tick;
    vu_meter_source_info.video_render = vu_render;
    vu_meter_source_info.get_width = vu_width;
    vu_meter_source_info.get_height = vu_height;
    vu_meter_source_info.get_defaults = vu_defaults;
    vu_meter_source_info.get_properties = vu_properties;
    vu_meter_source_info.icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT;
}

struct source_info_initializer {
    source_info_initializer() { init_source_info(); }
} source_info_initializer;
