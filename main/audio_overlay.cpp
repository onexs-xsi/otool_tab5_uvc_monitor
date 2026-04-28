// 自动从原 main.cpp 拆分而来。仅供 main 组件内部使用。
#include "app_internal.h"

// 文件内静态前向声明（播放/录音控制存在互相调用）
static bool start_uac_speaker_stream(app_t *a, const uac_host_stream_config_t *preferred);

// ===================================================================
// UAC 音频图谱叠加层
// ===================================================================
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static uint16_t blend_rgb565(uint16_t bg, uint16_t fg, uint8_t alpha)
{
    uint32_t br = ((bg >> 11) & 0x1F) << 3;
    uint32_t bgc = ((bg >> 5) & 0x3F) << 2;
    uint32_t bb = (bg & 0x1F) << 3;
    uint32_t fr = ((fg >> 11) & 0x1F) << 3;
    uint32_t fgc = ((fg >> 5) & 0x3F) << 2;
    uint32_t fb = (fg & 0x1F) << 3;
    uint32_t inv = 255 - alpha;
    uint8_t r = (uint8_t)((fr * alpha + br * inv) / 255);
    uint8_t g = (uint8_t)((fgc * alpha + bgc * inv) / 255);
    uint8_t b = (uint8_t)((fb * alpha + bb * inv) / 255);
    return rgb565(r, g, b);
}

static inline void put_pixel_logic(app_t *a, int x, int y, uint16_t color, uint8_t alpha)
{
    if (!a->panel_buf || x < 0 || y < 0 || x >= PANEL_LOGIC_W || y >= PANEL_LOGIC_H) return;
    int px = y;
    int py = PANEL_LOGIC_W - 1 - x;
    if (px < 0 || py < 0 || px >= PANEL_PHYS_W || py >= PANEL_PHYS_H) return;
    uint16_t *buf = (uint16_t *)a->panel_buf;
    uint16_t *dst = &buf[py * PANEL_PHYS_W + px];
    *dst = (alpha == 255) ? color : blend_rgb565(*dst, color, alpha);
}

static void fill_rect_logic(app_t *a, int x, int y, int w, int h, uint16_t color, uint8_t alpha)
{
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            put_pixel_logic(a, x + xx, y + yy, color, alpha);
        }
    }
}

static void draw_rect_border_logic(app_t *a, int x, int y, int w, int h, uint16_t color, uint8_t alpha)
{
    for (int xx = 0; xx < w; xx++) {
        put_pixel_logic(a, x + xx, y, color, alpha);
        put_pixel_logic(a, x + xx, y + h - 1, color, alpha);
    }
    for (int yy = 0; yy < h; yy++) {
        put_pixel_logic(a, x, y + yy, color, alpha);
        put_pixel_logic(a, x + w - 1, y + yy, color, alpha);
    }
}

static bool audio_record_lock(app_t *a, TickType_t timeout)
{
    return a->audio_rec_mutex && xSemaphoreTake(a->audio_rec_mutex, timeout) == pdTRUE;
}

static void audio_record_unlock(app_t *a)
{
    if (a->audio_rec_mutex) xSemaphoreGive(a->audio_rec_mutex);
}

static void close_uac_speaker_stream(app_t *a)
{
    if (!a->uac_spk_dev_hdl) {
        a->uac_tx_start_attempted = false;
        return;
    }

    esp_err_t err = uac_host_device_set_mute(a->uac_spk_dev_hdl, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "uac speaker mute failed: %s", esp_err_to_name(err));
    }
    err = uac_host_device_stop(a->uac_spk_dev_hdl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "uac speaker stop failed: %s", esp_err_to_name(err));
    }
    err = uac_host_device_close(a->uac_spk_dev_hdl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "uac speaker close failed: %s", esp_err_to_name(err));
    }
    a->uac_spk_dev_hdl = nullptr;
    a->uac_tx_start_attempted = false;
}

static void fill_circle_logic(app_t *a, int cx, int cy, int r, uint16_t color, uint8_t alpha)
{
    int rr = r * r;
    for (int yy = -r; yy <= r; yy++) {
        for (int xx = -r; xx <= r; xx++) {
            if (xx * xx + yy * yy <= rr) {
                put_pixel_logic(a, cx + xx, cy + yy, color, alpha);
            }
        }
    }
}

static void fill_play_triangle_logic(app_t *a, int x, int y, int w, int h, uint16_t color, uint8_t alpha)
{
    int mid = y + h / 2;
    for (int xx = 0; xx < w; xx++) {
        int half = (xx * h) / (2 * w);
        for (int yy = mid - half; yy <= mid + half; yy++) {
            put_pixel_logic(a, x + xx, yy, color, alpha);
        }
    }
}

static void audio_record_button_rect(int *x, int *y, int *w, int *h)
{
    *x = AUDIO_REC_BOX_X + 14;
    *y = AUDIO_REC_BOX_Y + (AUDIO_REC_BOX_H - AUDIO_REC_BTN_SIZE) / 2;
    *w = AUDIO_REC_BTN_SIZE;
    *h = AUDIO_REC_BTN_SIZE;
}

static void audio_scope_reset(app_t *a)
{
    portENTER_CRITICAL(&s_audio_scope_lock);
    memset(a->audio_scope, 0, sizeof(a->audio_scope));
    portEXIT_CRITICAL(&s_audio_scope_lock);
}

static void audio_scope_push(app_t *a, uint8_t level)
{
    portENTER_CRITICAL(&s_audio_scope_lock);
    memmove(a->audio_scope, a->audio_scope + 1, AUDIO_SCOPE_W - 1);
    a->audio_scope[AUDIO_SCOPE_W - 1] = level;
    portEXIT_CRITICAL(&s_audio_scope_lock);
}

static uint8_t audio_level_from_pcm(const uint8_t *data, uint32_t len, uint8_t bits)
{
    uint64_t acc = 0;
    uint32_t samples = 0;
    uint64_t max_amp = 1;

    if (bits == 8) {
        max_amp = 128;
        for (uint32_t i = 0; i < len; i++) {
            int sample = (int)data[i] - 128;
            acc += (sample < 0) ? -sample : sample;
            samples++;
        }
    } else if (bits == 16) {
        max_amp = 32768;
        for (uint32_t i = 0; i + 1 < len; i += 2) {
            int16_t sample = (int16_t)((uint16_t)data[i] | ((uint16_t)data[i + 1] << 8));
            acc += (sample < 0) ? (uint32_t)(-sample) : (uint32_t)sample;
            samples++;
        }
    } else if (bits == 24) {
        max_amp = 8388608;
        for (uint32_t i = 0; i + 2 < len; i += 3) {
            int32_t sample = (int32_t)data[i] | ((int32_t)data[i + 1] << 8) | ((int32_t)data[i + 2] << 16);
            if (sample & 0x800000) sample |= 0xFF000000;
            int64_t mag = sample;
            acc += (mag < 0) ? (uint64_t)(-mag) : (uint64_t)mag;
            samples++;
        }
    } else if (bits == 32) {
        max_amp = 2147483648ULL;
        for (uint32_t i = 0; i + 3 < len; i += 4) {
            int32_t sample = (int32_t)((uint32_t)data[i] | ((uint32_t)data[i + 1] << 8) |
                                       ((uint32_t)data[i + 2] << 16) | ((uint32_t)data[i + 3] << 24));
            int64_t mag = sample;
            acc += (mag < 0) ? (uint64_t)(-mag) : (uint64_t)mag;
            samples++;
        }
    }

    if (samples == 0) return 0;
    uint64_t avg = acc / samples;
    uint64_t scaled = (avg * 1530) / max_amp;
    return (uint8_t)((scaled > 255) ? 255 : scaled);
}

static void draw_audio_scope(app_t *a)
{
    if (!a->uac_selected || !a->audio_active || !a->panel_buf) return;

    uint8_t levels[AUDIO_SCOPE_W];
    portENTER_CRITICAL(&s_audio_scope_lock);
    memcpy(levels, a->audio_scope, sizeof(levels));
    portEXIT_CRITICAL(&s_audio_scope_lock);

    esp_cache_msync(a->panel_buf, PANEL_BUF_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

    const int x = AUDIO_SCOPE_X;
    const int y = AUDIO_SCOPE_Y;
    const int w = AUDIO_SCOPE_W;
    const int h = AUDIO_SCOPE_H;
    const int mid = y + h / 2;
    const uint16_t bg = rgb565(0x02, 0x08, 0x12);
    const uint16_t border = rgb565(0x22, 0xD3, 0xEE);
    const uint16_t grid = rgb565(0x38, 0xB8, 0xDF);
    const uint16_t wave = rgb565(0x34, 0xD3, 0x99);

    fill_rect_logic(a, x, y, w, h, bg, 160);
    draw_rect_border_logic(a, x, y, w, h, border, 175);
    for (int xx = 8; xx < w - 8; xx++) {
        put_pixel_logic(a, x + xx, mid, grid, 55);
    }

    for (int xx = 2; xx < w - 2; xx++) {
        int amp = (levels[xx] * (h - 18)) / 255;
        if (amp < 2) amp = 2;
        int y0 = mid - amp / 2;
        int y1 = mid + amp / 2;
        for (int yy = y0; yy <= y1; yy++) {
            put_pixel_logic(a, x + xx, yy, wave, 210);
        }
    }
}

static uint32_t pick_uac_sample_freq(const uac_host_dev_alt_param_t *alt)
{
    if (alt->sample_freq_type > 0) {
        for (int i = 0; i < alt->sample_freq_type && i < UAC_FREQ_NUM_MAX; i++) {
            if (alt->sample_freq[i]) return alt->sample_freq[i];
        }
        return 0;
    }
    if (alt->sample_freq_lower <= 48000 && alt->sample_freq_upper >= 48000) return 48000;
    if (alt->sample_freq_lower <= 16000 && alt->sample_freq_upper >= 16000) return 16000;
    return alt->sample_freq_lower;
}

static bool uac_stream_cfg_equal(const uac_host_stream_config_t *a,
                                 const uac_host_stream_config_t *b)
{
    return a && b &&
           a->channels == b->channels &&
           a->bit_resolution == b->bit_resolution &&
           a->sample_freq == b->sample_freq;
}

static bool select_uac_stream_config(uac_host_device_handle_t handle,
                                     uac_host_stream_config_t *cfg,
                                     const uac_host_stream_config_t *preferred,
                                     bool exact_required)
{
    uac_host_dev_info_t info = {};
    if (uac_host_get_device_info(handle, &info) != ESP_OK) return false;

    bool found = false;
    uac_host_stream_config_t fallback = {};
    for (uint8_t alt_idx = 1; alt_idx <= info.iface_alt_num; alt_idx++) {
        uac_host_dev_alt_param_t alt = {};
        if (uac_host_get_device_alt_param(handle, alt_idx, &alt) != ESP_OK) continue;
        if (alt.format != 1 || alt.channels == 0 || alt.bit_resolution == 0) continue;
        uint32_t freqs[UAC_FREQ_NUM_MAX] = {};
        int freq_count = 0;
        if (alt.sample_freq_type > 0) {
            for (int i = 0; i < alt.sample_freq_type && i < UAC_FREQ_NUM_MAX; i++) {
                if (alt.sample_freq[i]) freqs[freq_count++] = alt.sample_freq[i];
            }
        } else {
            uint32_t freq = 0;
            if (preferred && preferred->sample_freq >= alt.sample_freq_lower &&
                preferred->sample_freq <= alt.sample_freq_upper) {
                freq = preferred->sample_freq;
            } else {
                freq = pick_uac_sample_freq(&alt);
            }
            if (freq) freqs[freq_count++] = freq;
        }

        for (int fi = 0; fi < freq_count; fi++) {
            uac_host_stream_config_t candidate = {};
            candidate.channels = alt.channels;
            candidate.bit_resolution = alt.bit_resolution;
            candidate.sample_freq = freqs[fi];
            candidate.flags = 0;
            if (preferred && uac_stream_cfg_equal(&candidate, preferred)) {
                *cfg = candidate;
                return true;
            }
            if (!found) {
                fallback = candidate;
                found = true;
            }
            if (candidate.bit_resolution == 16 && fallback.bit_resolution != 16) {
                fallback = candidate;
            }
        }
    }

    if (preferred && exact_required) return false;
    if (found) *cfg = fallback;
    return found;
}

static uint32_t audio_record_ms_from_size(const app_t *a, size_t bytes)
{
    if (a->audio_record_bytes_per_sec == 0) return 0;
    uint64_t ms = ((uint64_t)bytes * 1000ULL) / a->audio_record_bytes_per_sec;
    if (ms > AUDIO_REC_MAX_MS) ms = AUDIO_REC_MAX_MS;
    return (uint32_t)ms;
}

static void audio_record_set_idle_locked(app_t *a)
{
    a->audio_rec_state = AUDIO_REC_IDLE;
    a->audio_record_size = 0;
    a->audio_record_duration_ms = 0;
    a->audio_record_started_ms = 0;
    a->audio_play_started_ms = 0;
    memset(a->audio_record_wave, 0, sizeof(a->audio_record_wave));
}

static void audio_playback_task(void *arg)
{
    app_t *a = (app_t *)arg;
    size_t offset = 0;

    while (true) {
        if (a->audio_play_stop_requested || !a->audio_active || !a->uac_spk_dev_hdl) break;

        size_t total = 0;
        uint8_t *buf = nullptr;
        if (audio_record_lock(a, pdMS_TO_TICKS(20))) {
            total = a->audio_record_size;
            buf = a->audio_record_buf;
            audio_record_unlock(a);
        }
        if (!buf || total == 0 || offset >= total) break;

        size_t chunk = total - offset;
        if (chunk > UAC_WRITE_CHUNK_SIZE) chunk = UAC_WRITE_CHUNK_SIZE;
        esp_err_t err = uac_host_device_write(a->uac_spk_dev_hdl, buf + offset,
                                              (uint32_t)chunk, pdMS_TO_TICKS(UAC_WRITE_TIMEOUT_MS));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "uac speaker write failed: %s", esp_err_to_name(err));
            break;
        }
        offset += chunk;
    }

    if (audio_record_lock(a, pdMS_TO_TICKS(50))) {
        if (a->audio_rec_state == AUDIO_REC_PLAYING) {
            audio_record_set_idle_locked(a);
        }
        a->audio_play_stop_requested = false;
        a->audio_playback_task = nullptr;
        audio_record_unlock(a);
    } else {
        a->audio_playback_task = nullptr;
        a->audio_play_stop_requested = false;
    }

    close_uac_speaker_stream(a);

    ESP_LOGI(TAG, "Audio playback ended");
    vTaskDelete(nullptr);
}

static void stop_audio_playback(app_t *a, bool wait_done)
{
    if (!a->audio_playback_task) return;
    if (a->uac_spk_dev_hdl) {
        uac_host_device_set_mute(a->uac_spk_dev_hdl, true);
    }
    a->audio_play_stop_requested = true;
    if (wait_done) {
        for (int i = 0; a->audio_playback_task && i < 160; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

static bool start_audio_playback(app_t *a)
{
    if (!a->uac_tx_available) {
        ESP_LOGW(TAG, "UAC speaker is not available");
        return false;
    }

    if (!a->uac_spk_dev_hdl) {
        uac_host_stream_config_t preferred = {};
        preferred.channels = a->audio_channels;
        preferred.bit_resolution = a->audio_bit_resolution;
        preferred.sample_freq = a->audio_sample_freq;
        if (!start_uac_speaker_stream(a, &preferred)) {
            ESP_LOGW(TAG, "UAC speaker could not be started for playback");
            return false;
        }
    }

    if (a->audio_playback_task) return true;

    bool can_start = false;
    if (audio_record_lock(a, pdMS_TO_TICKS(20))) {
        can_start = (a->audio_rec_state == AUDIO_REC_READY &&
                     a->audio_record_buf && a->audio_record_size > 0);
        if (can_start) {
            a->audio_rec_state = AUDIO_REC_PLAYING;
            a->audio_play_stop_requested = false;
            a->audio_play_started_ms = (uint32_t)esp_log_timestamp();
        }
        audio_record_unlock(a);
    }
    if (!can_start) return false;

    esp_err_t mute_err = uac_host_device_set_mute(a->uac_spk_dev_hdl, false);
    if (mute_err != ESP_OK) {
        ESP_LOGW(TAG, "uac speaker unmute failed: %s", esp_err_to_name(mute_err));
    }

    BaseType_t ok = xTaskCreatePinnedToCore(audio_playback_task, "uac_play", 4096, a,
                                            UAC_AUDIO_TASK_PRIO, &a->audio_playback_task,
                                            tskNO_AFFINITY);
    if (ok != pdTRUE) {
        ESP_LOGW(TAG, "uac playback task create failed");
        if (audio_record_lock(a, pdMS_TO_TICKS(20))) {
            a->audio_rec_state = AUDIO_REC_READY;
            a->audio_play_started_ms = 0;
            audio_record_unlock(a);
        }
        close_uac_speaker_stream(a);
        return false;
    }

    ESP_LOGI(TAG, "Audio playback started");
    return true;
}

static bool audio_record_prepare(app_t *a, const uac_host_stream_config_t *cfg)
{
    if (!cfg || cfg->channels == 0 || cfg->bit_resolution == 0 || cfg->sample_freq == 0) return false;
    uint32_t bytes_per_sample = cfg->bit_resolution / 8;
    if (bytes_per_sample == 0) return false;
    uint32_t bytes_per_sec = cfg->sample_freq * cfg->channels * bytes_per_sample;
    size_t capacity = ((size_t)bytes_per_sec * AUDIO_REC_MAX_MS) / 1000;
    if (capacity == 0) return false;

    if (a->audio_record_buf && a->audio_record_capacity != capacity) {
        free(a->audio_record_buf);
        a->audio_record_buf = nullptr;
    }
    if (!a->audio_record_buf) {
        a->audio_record_buf = (uint8_t *)heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!a->audio_record_buf) a->audio_record_buf = (uint8_t *)malloc(capacity);
        if (!a->audio_record_buf) {
            ESP_LOGW(TAG, "audio record buf alloc failed size=%u", (unsigned)capacity);
            a->audio_record_capacity = 0;
            return false;
        }
    }

    memset(a->audio_record_buf, 0, capacity);
    a->audio_record_capacity = capacity;
    a->audio_record_bytes_per_sec = bytes_per_sec;
    if (audio_record_lock(a, pdMS_TO_TICKS(20))) {
        audio_record_set_idle_locked(a);
        audio_record_unlock(a);
    }
    ESP_LOGI(TAG, "Audio record buffer ready: %u bytes", (unsigned)capacity);
    return true;
}

static void audio_record_release(app_t *a)
{
    stop_audio_playback(a, true);
    if (audio_record_lock(a, pdMS_TO_TICKS(50))) {
        audio_record_set_idle_locked(a);
        audio_record_unlock(a);
    }
    if (a->audio_record_buf) {
        free(a->audio_record_buf);
        a->audio_record_buf = nullptr;
    }
    a->audio_record_capacity = 0;
    a->audio_record_bytes_per_sec = 0;
    a->audio_rec_touch_down = false;
    a->audio_rec_long_sent = false;
    close_uac_speaker_stream(a);
}

static void audio_record_start(app_t *a)
{
    if (!a->audio_record_buf || a->audio_record_capacity == 0) return;
    if (!a->uac_tx_available) {
        ESP_LOGW(TAG, "UAC speaker is not available");
        return;
    }
    if (audio_record_lock(a, pdMS_TO_TICKS(20))) {
        if (a->audio_rec_state == AUDIO_REC_IDLE) {
            a->audio_record_size = 0;
            a->audio_record_duration_ms = 0;
            a->audio_record_started_ms = (uint32_t)esp_log_timestamp();
            a->audio_play_started_ms = 0;
            a->audio_rec_state = AUDIO_REC_RECORDING;
            memset(a->audio_record_wave, 0, sizeof(a->audio_record_wave));
            ESP_LOGI(TAG, "Audio recording started");
        }
        audio_record_unlock(a);
    }
}

static void audio_record_stop(app_t *a)
{
    if (audio_record_lock(a, pdMS_TO_TICKS(20))) {
        if (a->audio_rec_state == AUDIO_REC_RECORDING) {
            a->audio_record_duration_ms = audio_record_ms_from_size(a, a->audio_record_size);
            if (a->audio_record_size > 0) {
                a->audio_rec_state = AUDIO_REC_READY;
                ESP_LOGI(TAG, "Audio recording stopped: %u bytes", (unsigned)a->audio_record_size);
            } else {
                audio_record_set_idle_locked(a);
            }
        }
        audio_record_unlock(a);
    }
}

static void audio_record_discard(app_t *a)
{
    stop_audio_playback(a, false);
    if (audio_record_lock(a, pdMS_TO_TICKS(20))) {
        audio_record_set_idle_locked(a);
        audio_record_unlock(a);
    }
    ESP_LOGI(TAG, "Audio recording discarded");
}

static void audio_record_click(app_t *a)
{
    audio_rec_state_t state = AUDIO_REC_IDLE;
    if (audio_record_lock(a, pdMS_TO_TICKS(20))) {
        state = a->audio_rec_state;
        audio_record_unlock(a);
    }

    switch (state) {
    case AUDIO_REC_IDLE:
        audio_record_start(a);
        break;
    case AUDIO_REC_RECORDING:
        audio_record_stop(a);
        break;
    case AUDIO_REC_READY:
        start_audio_playback(a);
        break;
    case AUDIO_REC_PLAYING:
        audio_record_discard(a);
        break;
    default:
        break;
    }
}

static void audio_record_append(app_t *a, const uint8_t *data, uint32_t len)
{
    if (!data || len == 0 || !a->audio_record_buf || a->audio_record_capacity == 0) return;

    bool reached_max = false;
    if (audio_record_lock(a, 0)) {
        if (a->audio_rec_state == AUDIO_REC_RECORDING &&
            a->audio_record_size < a->audio_record_capacity) {
            size_t old_size = a->audio_record_size;
            size_t room = a->audio_record_capacity - a->audio_record_size;
            size_t copy_len = (len < room) ? len : room;
            memcpy(a->audio_record_buf + a->audio_record_size, data, copy_len);
            a->audio_record_size += copy_len;
            a->audio_record_duration_ms = audio_record_ms_from_size(a, a->audio_record_size);
            uint8_t level = audio_level_from_pcm(data, (uint32_t)copy_len, a->audio_bit_resolution);
            int start_idx = (int)(((uint64_t)old_size * AUDIO_SCOPE_W) / a->audio_record_capacity);
            int end_idx = (int)((((uint64_t)(a->audio_record_size ? (a->audio_record_size - 1) : 0)) * AUDIO_SCOPE_W) /
                                a->audio_record_capacity);
            if (start_idx < 0) start_idx = 0;
            if (end_idx >= AUDIO_SCOPE_W) end_idx = AUDIO_SCOPE_W - 1;
            for (int i = start_idx; i <= end_idx; i++) {
                if (level > a->audio_record_wave[i]) a->audio_record_wave[i] = level;
            }
            if (a->audio_record_size >= a->audio_record_capacity ||
                a->audio_record_duration_ms >= AUDIO_REC_MAX_MS) {
                a->audio_rec_state = AUDIO_REC_READY;
                a->audio_record_duration_ms = AUDIO_REC_MAX_MS;
                reached_max = true;
            }
        }
        audio_record_unlock(a);
    }
    if (reached_max) ESP_LOGI(TAG, "Audio recording reached max length");
}

static bool start_uac_speaker_stream(app_t *a, const uac_host_stream_config_t *preferred)
{
    if (!a->uac_tx_available) return false;
    if (a->uac_spk_dev_hdl) return true;
    a->uac_tx_start_attempted = true;

    uac_host_device_config_t dev_cfg = {};
    dev_cfg.addr = a->uac_tx_addr;
    dev_cfg.iface_num = a->uac_tx_iface_num;
    dev_cfg.buffer_size = UAC_AUDIO_BUF_SIZE;
    dev_cfg.buffer_threshold = UAC_AUDIO_THRESHOLD;
    dev_cfg.callback = uac_device_event_cb;
    dev_cfg.callback_arg = a;

    uac_host_device_handle_t handle = nullptr;
    esp_err_t err = uac_host_device_open(&dev_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "uac speaker open failed: %s", esp_err_to_name(err));
        a->uac_tx_start_attempted = false;
        return false;
    }

    uac_host_stream_config_t stream_cfg = {};
    if (!select_uac_stream_config(handle, &stream_cfg, preferred, true)) {
        ESP_LOGW(TAG, "uac speaker exact stream config not found");
        uac_host_device_close(handle);
        a->uac_tx_start_attempted = false;
        return false;
    }

    err = uac_host_device_start(handle, &stream_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "uac speaker start failed: %s", esp_err_to_name(err));
        uac_host_device_close(handle);
        a->uac_tx_start_attempted = false;
        return false;
    }

    a->uac_spk_dev_hdl = handle;
    a->speaker_channels = stream_cfg.channels;
    a->speaker_bit_resolution = stream_cfg.bit_resolution;
    a->speaker_sample_freq = stream_cfg.sample_freq;
    uac_host_device_set_volume(handle, 70);
    uac_host_device_set_mute(handle, true);
    ESP_LOGI(TAG, "UAC speaker started: %lu Hz %u bit channels=%u",
             (unsigned long)a->speaker_sample_freq, a->speaker_bit_resolution, a->speaker_channels);
    return true;
}

static void draw_audio_record_panel(app_t *a)
{
    if (!a->uac_selected || !a->audio_active || !a->panel_buf) return;

    audio_rec_state_t state = AUDIO_REC_IDLE;
    size_t rec_size = 0;
    size_t rec_capacity = 0;
    uint32_t rec_ms = 0;
    uint32_t play_started = 0;
    bool enabled = false;
    uint8_t wave[AUDIO_SCOPE_W];
    memset(wave, 0, sizeof(wave));

    if (audio_record_lock(a, 0)) {
        state = a->audio_rec_state;
        rec_size = a->audio_record_size;
        rec_capacity = a->audio_record_capacity;
        rec_ms = a->audio_record_duration_ms;
        if (state == AUDIO_REC_RECORDING) rec_ms = audio_record_ms_from_size(a, rec_size);
        play_started = a->audio_play_started_ms;
        enabled = (a->audio_record_buf != nullptr && rec_capacity > 0 && a->uac_tx_available);
        memcpy(wave, a->audio_record_wave, sizeof(wave));
        audio_record_unlock(a);
    }

    if (state == AUDIO_REC_PLAYING && rec_ms > 0 && play_started > 0) {
        uint32_t now = (uint32_t)esp_log_timestamp();
        uint32_t elapsed = now - play_started;
        if (elapsed < rec_ms) rec_ms = elapsed;
    }

    const int x = AUDIO_REC_BOX_X;
    const int y = AUDIO_REC_BOX_Y;
    const int w = AUDIO_REC_BOX_W;
    const int h = AUDIO_REC_BOX_H;
    const uint16_t bg = rgb565(0x02, 0x08, 0x12);
    const uint16_t border = rgb565(0x22, 0xD3, 0xEE);
    const uint16_t grid = rgb565(0x38, 0xB8, 0xDF);
    const uint16_t btn = enabled ? rgb565(0x0B, 0x4A, 0x82) : rgb565(0x13, 0x2B, 0x4C);
    const uint16_t btn_active = rgb565(0x10, 0x6A, 0xB5);
    const uint16_t wave_idle = rgb565(0x4B, 0x75, 0x9A);
    const uint16_t wave_fill = rgb565(0x4D, 0xC4, 0xFF);
    const uint16_t wave_played = rgb565(0x34, 0xD3, 0x99);
    const uint16_t white = rgb565(0xFF, 0xFF, 0xFF);

    fill_rect_logic(a, x, y, w, h, bg, 160);
    draw_rect_border_logic(a, x, y, w, h, border, 175);

    int bx, by, bw, bh;
    audio_record_button_rect(&bx, &by, &bw, &bh);
    uint16_t btn_color = (state == AUDIO_REC_RECORDING || state == AUDIO_REC_PLAYING) ? btn_active : btn;
    fill_rect_logic(a, bx, by, bw, bh, btn_color, 215);
    draw_rect_border_logic(a, bx, by, bw, bh, border, 180);

    int cx = bx + bw / 2;
    int cy = by + bh / 2;
    if (state == AUDIO_REC_READY) {
        fill_play_triangle_logic(a, cx - 12, cy - 18, 30, 36, white, 255);
    } else if (state == AUDIO_REC_RECORDING || state == AUDIO_REC_PLAYING) {
        fill_rect_logic(a, cx - 13, cy - 13, 26, 26, white, 255);
    } else {
        fill_circle_logic(a, cx, cy, 18, white, 255);
        fill_circle_logic(a, cx, cy, 10, enabled ? btn_color : rgb565(0x60, 0xA5, 0xFA), 255);
    }

    const int rx = bx + bw + 18;
    const int rw = x + w - rx - 16;
    const int plot_y = y + 10;
    const int plot_h = h - 20;
    const int mid = plot_y + plot_h / 2;

    for (int xx = 0; xx < rw; xx++) {
        put_pixel_logic(a, rx + xx, mid, grid, 55);
    }

    uint32_t progress_ms = 0;
    if (state == AUDIO_REC_RECORDING || state == AUDIO_REC_READY) {
        progress_ms = rec_ms;
    } else if (state == AUDIO_REC_PLAYING && play_started > 0) {
        uint32_t elapsed = (uint32_t)esp_log_timestamp() - play_started;
        progress_ms = elapsed;
        if (progress_ms > rec_ms && rec_ms > 0) progress_ms = rec_ms;
    }
    if (progress_ms > AUDIO_REC_MAX_MS) progress_ms = AUDIO_REC_MAX_MS;

    int recorded_px = 0;
    if (rec_capacity > 0) {
        recorded_px = (int)(((uint64_t)rec_size * rw) / rec_capacity);
        if (recorded_px > rw) recorded_px = rw;
    }
    int play_px = (int)(((uint64_t)progress_ms * rw) / AUDIO_REC_MAX_MS);
    if (play_px > rw) play_px = rw;

    for (int xx = 0; xx < rw; xx++) {
        int idx = (xx * AUDIO_SCOPE_W) / rw;
        int amp = (wave[idx] * (plot_h - 14)) / 255;
        if (amp < 2 && xx < recorded_px) amp = 2;
        if (amp <= 0) continue;

        uint16_t color = wave_idle;
        uint8_t alpha = 105;
        if (xx < recorded_px) {
            color = wave_fill;
            alpha = 205;
        }
        if (state == AUDIO_REC_PLAYING && xx < play_px) {
            color = wave_played;
            alpha = 225;
        }

        int y0 = mid - amp / 2;
        int y1 = mid + amp / 2;
        for (int yy = y0; yy <= y1; yy++) {
            put_pixel_logic(a, rx + xx, yy, color, alpha);
        }
    }

    if (state == AUDIO_REC_READY) {
        int mark_x = rx + recorded_px;
        fill_rect_logic(a, mark_x, plot_y - 2, 2, plot_h + 4, border, 170);
    } else if (state == AUDIO_REC_PLAYING) {
        int px = rx + play_px;
        fill_rect_logic(a, px, plot_y - 4, 3, plot_h + 8, white, 230);
    }
}

void draw_audio_overlay(app_t *a)
{
    draw_audio_scope(a);
    draw_audio_record_panel(a);
}

static bool read_touch_logic(app_t *a, int *logic_x, int *logic_y)
{
    if (!a->touch_handle || !logic_x || !logic_y) return false;

    esp_lcd_touch_point_data_t point = {};
    uint8_t cnt = 0;
    if (esp_lcd_touch_read_data(a->touch_handle) != ESP_OK) return false;
    if (esp_lcd_touch_get_data(a->touch_handle, &point, &cnt, 1) != ESP_OK || cnt == 0) return false;

    // 触摸原始坐标是 720x1280 物理坐标，这里转换为 1280x720 逻辑坐标。
    int x = PANEL_LOGIC_W - 1 - (int)point.y;
    int y = (int)point.x;
    if (x < 0 || y < 0 || x >= PANEL_LOGIC_W || y >= PANEL_LOGIC_H) return false;
    *logic_x = x;
    *logic_y = y;
    return true;
}

static bool audio_record_button_hit(int x, int y)
{
    int bx, by, bw, bh;
    audio_record_button_rect(&bx, &by, &bw, &bh);
    return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

void audio_record_touch_poll(app_t *a)
{
    if (!a->uac_selected || !a->audio_active || !a->audio_record_buf) {
        a->audio_rec_touch_down = false;
        a->audio_rec_long_sent = false;
        return;
    }

    int x = 0, y = 0;
    bool pressed = read_touch_logic(a, &x, &y);
    bool hit = pressed && audio_record_button_hit(x, y);
    uint32_t now = (uint32_t)esp_log_timestamp();

    if (hit) {
        if (!a->audio_rec_touch_down) {
            a->audio_rec_touch_down = true;
            a->audio_rec_long_sent = false;
            a->audio_rec_touch_start_ms = now;
        } else if (!a->audio_rec_long_sent &&
                   now - a->audio_rec_touch_start_ms >= AUDIO_REC_LONG_MS) {
            audio_rec_state_t state = AUDIO_REC_IDLE;
            if (audio_record_lock(a, 0)) {
                state = a->audio_rec_state;
                audio_record_unlock(a);
            }
            if (state == AUDIO_REC_READY) {
                audio_record_discard(a);
                a->audio_rec_long_sent = true;
            }
        }
        return;
    }

    if (a->audio_rec_touch_down) {
        if (!a->audio_rec_long_sent) {
            audio_record_click(a);
        }
        a->audio_rec_touch_down = false;
        a->audio_rec_long_sent = false;
    }
}

void uac_device_event_cb(uac_host_device_handle_t uac_device_handle,
                                const uac_host_device_event_t event, void *arg)
{
    app_t *a = (app_t *)arg;
    if (event == UAC_HOST_DEVICE_EVENT_RX_DONE) {
        if (a->audio_task) xTaskNotifyGive(a->audio_task);
    } else if (event == UAC_HOST_DEVICE_EVENT_TX_DONE) {
        // 扬声器TX缓冲有空间；播放任务使用阻塞写入，这里无需额外通知。
    } else if (event == UAC_HOST_DRIVER_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "UAC device disconnected");
        if (uac_device_handle == a->uac_dev_hdl) {
            a->audio_active = false;
            a->uac_rx_available = false;
            a->uac_selected = false;
            if (a->audio_task) xTaskNotifyGive(a->audio_task);
        }
        if (uac_device_handle == a->uac_spk_dev_hdl) {
            a->audio_play_stop_requested = true;
            a->uac_tx_available = false;
            a->uac_spk_dev_hdl = nullptr;
        }
        xEventGroupSetBits(a->events, EV_UAC_CONNECTED);
    } else if (event == UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR) {
        ESP_LOGW(TAG, "UAC transfer error");
    }
}

static void audio_capture_task(void *arg)
{
    app_t *a = (app_t *)arg;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(UAC_READ_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) buf = (uint8_t *)malloc(UAC_READ_BUF_SIZE);

    while (a->audio_task_running) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        if (!buf || !a->audio_active || !a->uac_dev_hdl) continue;

        for (int i = 0; i < 4 && a->audio_active; i++) {
            uint32_t got = 0;
            esp_err_t err = uac_host_device_read(a->uac_dev_hdl, buf, UAC_READ_BUF_SIZE, &got, 0);
            if (err != ESP_OK || got == 0) break;
            audio_scope_push(a, audio_level_from_pcm(buf, got, a->audio_bit_resolution));
            audio_record_append(a, buf, got);
        }
    }

    if (buf) free(buf);
    a->audio_task = nullptr;
    vTaskDelete(nullptr);
}

bool start_uac_stream(app_t *a)
{
    if (!a->uac_selected || !a->uac_driver_ready || !a->uac_rx_available) return false;
    if (a->uac_dev_hdl) return true;

    audio_scope_reset(a);

    uac_host_device_config_t dev_cfg = {};
    dev_cfg.addr = a->uac_addr;
    dev_cfg.iface_num = a->uac_iface_num;
    dev_cfg.buffer_size = UAC_AUDIO_BUF_SIZE;
    dev_cfg.buffer_threshold = UAC_AUDIO_THRESHOLD;
    dev_cfg.callback = uac_device_event_cb;
    dev_cfg.callback_arg = a;

    uac_host_device_handle_t handle = nullptr;
    esp_err_t err = uac_host_device_open(&dev_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "uac open failed: %s", esp_err_to_name(err));
        return false;
    }

    uac_host_stream_config_t stream_cfg = {};
    if (!select_uac_stream_config(handle, &stream_cfg, nullptr, false)) {
        ESP_LOGW(TAG, "uac stream config not found");
        uac_host_device_close(handle);
        return false;
    }

    a->uac_dev_hdl = handle;
    a->audio_channels = stream_cfg.channels;
    a->audio_bit_resolution = stream_cfg.bit_resolution;
    a->audio_sample_freq = stream_cfg.sample_freq;
    audio_record_prepare(a, &stream_cfg);
    a->audio_task_running = true;
    BaseType_t task_ok = xTaskCreatePinnedToCore(audio_capture_task, "uac_audio", 4096, a,
                                                 UAC_AUDIO_TASK_PRIO, &a->audio_task, tskNO_AFFINITY);
    if (task_ok != pdTRUE) {
        ESP_LOGW(TAG, "uac audio task create failed");
        a->audio_task_running = false;
        a->uac_dev_hdl = nullptr;
        uac_host_device_close(handle);
        return false;
    }

    err = uac_host_device_start(handle, &stream_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "uac start failed: %s", esp_err_to_name(err));
        a->audio_task_running = false;
        if (a->audio_task) xTaskNotifyGive(a->audio_task);
        for (int i = 0; a->audio_task && i < 50; i++) vTaskDelay(pdMS_TO_TICKS(10));
        a->uac_dev_hdl = nullptr;
        uac_host_device_close(handle);
        return false;
    }

    a->audio_active = true;
    xTaskNotifyGive(a->audio_task);
    ESP_LOGI(TAG, "UAC audio started: %lu Hz %u bit channels=%u",
             (unsigned long)a->audio_sample_freq, a->audio_bit_resolution, a->audio_channels);
    return true;
}

void stop_uac_stream(app_t *a)
{
    audio_record_release(a);
    a->audio_active = false;
    if (a->audio_task) {
        a->audio_task_running = false;
        xTaskNotifyGive(a->audio_task);
        for (int i = 0; a->audio_task && i < 50; i++) vTaskDelay(pdMS_TO_TICKS(10));
    }

    close_uac_speaker_stream(a);

    if (a->uac_dev_hdl) {
        esp_err_t err = uac_host_device_stop(a->uac_dev_hdl);
        if (err != ESP_OK) ESP_LOGW(TAG, "uac stop failed: %s", esp_err_to_name(err));
        err = uac_host_device_close(a->uac_dev_hdl);
        if (err != ESP_OK) ESP_LOGW(TAG, "uac close failed: %s", esp_err_to_name(err));
        a->uac_dev_hdl = nullptr;
    }
    a->uac_selected = false;
    a->audio_task_running = false;
    audio_scope_reset(a);
}

