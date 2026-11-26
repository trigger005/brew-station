// SPDX-License-Identifier: AGPL-3.0-or-later
// Glue between the scale, the detector and the machine. The detector itself
// is pure (components/shot); this only feeds it and acts on what it says.
//
// Readings are stamped when they arrive (the AKU sends no clock of its own),
// on the same esp_timer clock as the knob press, so timing mode 1 is
// "knob press to end of flow" on one clock. The BLE delay shifts both the
// first drops and the end alike; it hasn't been measured yet (M2).
#include "autoshot.h"

#include "shot_decide.h"

#include <math.h>
#include <string.h>

#include "aku.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "web.h"
#include "grinder_api.h"

static const char *TAG = "autoshot";

#define NVS_NS "cs"
#define NVS_KEY_MODE "report_mode"

#if CONFIG_CS_REPORT_DEFAULT_TARGET
#define DEFAULT_MODE REPORT_TARGET
#elif CONFIG_CS_REPORT_DEFAULT_MANUAL
#define DEFAULT_MODE REPORT_MANUAL
#else
#define DEFAULT_MODE REPORT_AUTO
#endif

#if CONFIG_CS_TIMING_MODE_DROPS
#define TIMING_MODE 3
#else
#define TIMING_MODE 1
#endif

static SemaphoreHandle_t s_lock;
static shot_detector_t *s_det;
static autoshot_status_t s_st;
static int64_t s_knob_ms;

const char *report_mode_code(report_mode_t m) {
    return m == REPORT_TARGET ? "target" : m == REPORT_MANUAL ? "manual" : "auto";
}

bool report_mode_parse(const char *s, report_mode_t *out) {
    if (!strcmp(s, "auto")) *out = REPORT_AUTO;
    else if (!strcmp(s, "target")) *out = REPORT_TARGET;
    else if (!strcmp(s, "manual")) *out = REPORT_MANUAL;
    else return false;
    return true;
}

static report_mode_t load_mode(void) {
    nvs_handle_t h;
    uint8_t v = DEFAULT_MODE;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_MODE, &v);
        nvs_close(h);
    }
    return v <= REPORT_MANUAL ? (report_mode_t)v : DEFAULT_MODE;
}

esp_err_t autoshot_start(void) {
    s_lock = xSemaphoreCreateMutex();
    s_st.mode = load_mode();
    s_st.timing_mode = TIMING_MODE;
    // ~140 KB: PSRAM, it's only touched a few times a second.
    s_det = heap_caps_malloc(sizeof *s_det, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_det) s_det = malloc(sizeof *s_det);
    if (!s_det) {
        ESP_LOGE(TAG, "No memory for the shot detector: reporting stays manual");
        s_st.mode = REPORT_MANUAL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Report mode %s, timing mode %d", report_mode_code(s_st.mode), TIMING_MODE);
    return ESP_OK;
}

esp_err_t autoshot_set_mode(report_mode_t mode) {
    if (!s_lock || mode > REPORT_MANUAL) return ESP_ERR_INVALID_ARG;
    if (!s_det && mode != REPORT_MANUAL) return ESP_ERR_NO_MEM;
    // Armed with no shot running: the event that should have disarmed it was
    // lost (machine event queue full). Don't let that lock the mode.
    grinder_api_status_t x;
    grinder_api_get_status(&x);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_st.armed && x.phase != MACHINE_RUNNING) {
        s_st.armed = false;
        s_st.seq++;
    }
    if (s_st.armed) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    bool changed = s_st.mode != mode;
    s_st.mode = mode;
    if (changed) s_st.seq++;
    xSemaphoreGive(s_lock);
    if (!changed) return ESP_OK;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, NVS_KEY_MODE, (uint8_t)mode);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGW(TAG, "Saving the report mode: %s", esp_err_to_name(err));
    printf("AUTO mode %s\n", report_mode_code(mode));
    return ESP_OK;
}

void autoshot_arm(int64_t knob_ms, bool tared) {
    if (!s_lock || !s_det) return;
    grinder_api_status_t x;
    grinder_api_get_status(&x);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_st.mode == REPORT_MANUAL) {
        xSemaphoreGive(s_lock);
        return;
    }
    // gbs-anywhere half_decent/run.rs:275-281: after a tare, wait for it to
    // land; without one the shot may already be running.
    if (tared) shot_init_after_tare(s_det);
    else shot_init(s_det);
    shot_expecting(s_det, x.has_recipe, x.recipe_g);
    s_knob_ms = knob_ms;
    // Target needs the grinder's recipe weight; without one it works as auto.
    bool target = s_st.mode == REPORT_TARGET && x.has_recipe && isfinite(x.recipe_g) && x.recipe_g > 0;
    s_st.watching = target ? REPORT_TARGET : REPORT_AUTO;
    s_st.target_g = target ? x.recipe_g : 0;
    s_st.armed = true;
    s_st.progress = SHOT_SETTLING;
    s_st.grams = 0;
    s_st.seq++;
    report_mode_t mode = s_st.mode;
    xSemaphoreGive(s_lock);
    printf("AUTO %lld armed %s tared=%d recipe=%.1f\n", (long long)knob_ms, target ? "target" : "auto", tared,
           x.has_recipe ? x.recipe_g : 0.0);
    if (mode == REPORT_TARGET && !target) web_log("Target mode, but no recipe weight from the grinder: reporting at the end of flow");
}

void autoshot_disarm(void) {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_st.armed) {
        s_st.armed = false;
        s_st.seq++;
    }
    xSemaphoreGive(s_lock);
}

static void report(uint32_t time_ms, double grams, report_mode_t by, const char *what) {
    machine_err_t e = grinder_api_report_shot(time_ms, grams);
    if (e != MACHINE_OK) {
        web_log("Auto: %s (%.1f s, %.1f g) but not sent: %s", what, time_ms / 1000.0, grams, machine_err_code(e));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.has_last = true;
    s_st.last_time_ms = time_ms;
    s_st.last_grams = grams;
    s_st.last_by = by;
    s_st.seq++;
    xSemaphoreGive(s_lock);
    web_log("Auto: %s, sent %.1f s, %.1f g", what, time_ms / 1000.0, grams);
}

void autoshot_weight(int64_t t_ms, double grams) {
    if (!s_lock || !s_det) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_st.armed) {
        xSemaphoreGive(s_lock);
        return;
    }
    // Several frames of one notification can share its arrival time, and two
    // notifications can land in the same ms (notif_ms min 0 on hardware). The
    // detector takes a time that doesn't move on as the scale's clock
    // restarting and starts over (shot.rs:175); this clock never goes back.
    static int64_t last_ms;
    t_ms = shot_monotonic_ms(&last_ms, t_ms);
    shot_progress_t p = shot_push(s_det, (shot_reading_t){t_ms, grams});
    shot_decide_cfg_t cfg = {
        .target = s_st.watching == REPORT_TARGET,
        .target_g = s_st.target_g,
        .knob_ms = s_knob_ms,
        .timing_mode = TIMING_MODE,
#if CONFIG_CS_TIMING_MODE_DROPS
        .preinfusion_ms = CONFIG_CS_PREINFUSION_S * 1000u,
#endif
    };
    shot_decision_t dec = shot_decide(&p, &cfg);
    shot_progress_kind_t was = s_st.progress;
    bool first_drops = p.kind == SHOT_FLOWING && was != SHOT_FLOWING;
    s_st.progress = p.kind;
    if (p.kind == SHOT_FLOWING) s_st.grams = p.grams;
    if (p.kind != was) s_st.seq++;
    if (dec.kind != SHOT_REPORT_NONE) s_st.armed = false;  // the rest of the pour is ignored
    xSemaphoreGive(s_lock);

    if (p.kind != was && p.kind != SHOT_DONE)
        printf("AUTO %lld %s %.1f g\n", (long long)t_ms, shot_progress_name(p.kind), p.grams);

    if (first_drops) {
        web_log("Auto: first drops at %.1f s", (p.start_ms - cfg.knob_ms) / 1000.0);
#if CONFIG_CS_TIMING_MODE_DROPS && CONFIG_CS_KNOB_SCALE_ACTIONS
        // Timing mode 3: the AKU timer runs from the first drops (display only).
        aku_send(AKU_CMD_TIMER_START);
#endif
    }
    if (dec.kind == SHOT_REPORT_NONE) return;

    if (dec.kind == SHOT_REPORT_TARGET)
        printf("AUTO %lld target %.1f g reached at %lld (drops %lld, knob %lld) -> report %lu ms %.1f g\n",
               (long long)t_ms, cfg.target_g, (long long)p.ms, (long long)p.start_ms, (long long)cfg.knob_ms,
               (unsigned long)dec.time_ms, dec.grams);
    else
        printf("AUTO %lld done drops %lld..%lld (%lu ms) knob %lld -> report %lu ms %.1f g\n", (long long)t_ms,
               (long long)p.shot.start_ms, (long long)p.shot.end_ms, (unsigned long)p.shot.time_ms,
               (long long)cfg.knob_ms, (unsigned long)dec.time_ms, dec.grams);
    report(dec.time_ms, dec.grams, dec.kind == SHOT_REPORT_TARGET ? REPORT_TARGET : REPORT_AUTO,
           dec.kind == SHOT_REPORT_TARGET           ? "target reached"
           : dec.kind == SHOT_REPORT_END_BEFORE_TARGET ? "flow stopped before the target"
                                                        : "flow stopped");
}

void autoshot_get_status(autoshot_status_t *out) {
    if (!s_lock) {
        memset(out, 0, sizeof *out);
        out->mode = REPORT_MANUAL;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st;
    xSemaphoreGive(s_lock);
}
