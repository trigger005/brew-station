// SPDX-License-Identifier: AGPL-3.0-or-later
// Ported from gbs-anywhere src/protocol/wire.rs.
#include "machine_wire.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"

// ------------------------------------------------------------------ numbers

int wire_fmt_f64(double v, char *buf, size_t size) {
    if (!isfinite(v)) return snprintf(buf, size, "null");  // serde_json: NaN/inf -> null
    char tmp[40];
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(tmp, sizeof tmp, "%.*g", prec, v);
        if (strtod(tmp, NULL) == v) break;
    }
    if (!strpbrk(tmp, ".eEn")) strcat(tmp, ".0");
    return snprintf(buf, size, "%s", tmp);
}

// wire.rs:314-320 number_like: a JSON number, or a string holding one
static bool number_like(const cJSON *v, double *out) {
    if (cJSON_IsNumber(v)) {
        *out = v->valuedouble;
        return true;
    }
    if (cJSON_IsString(v) && v->valuestring) {
        const char *s = v->valuestring;
        while (isspace((unsigned char)*s)) s++;
        if (!*s) return false;
        char *end;
        double d = strtod(s, &end);
        while (isspace((unsigned char)*end)) end++;
        if (end == s || *end) return false;
        *out = d;
        return true;
    }
    return false;
}

// wire.rs:325-339 text_like: strings lose control characters and are cut to
// WIRE_MAX_TEXT_CHARS characters; numbers and booleans become their text.
static bool text_like(const cJSON *v, char *out, size_t size) {
    if (cJSON_IsString(v) && v->valuestring) {
        const unsigned char *p = (const unsigned char *)v->valuestring;
        size_t n = 0;
        int chars = 0;
        while (*p && chars < WIRE_MAX_TEXT_CHARS) {
            size_t clen = *p < 0x80 ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : 4;
            for (size_t i = 1; i < clen; i++)
                if (!p[i]) clen = i;  // truncated sequence: stop at the NUL
            bool control = *p < 0x20 || *p == 0x7F || (clen == 2 && p[0] == 0xC2 && p[1] < 0xA0);
            if (!control) {
                if (n + clen >= size) break;
                memcpy(out + n, p, clen);
                n += clen;
                chars++;
            }
            p += clen;
        }
        out[n] = 0;
        return true;
    }
    if (cJSON_IsNumber(v)) {
        double d = v->valuedouble;
        // serde_json keeps integers as integers, floats as f64 text
        if (d == floor(d) && fabs(d) < 9.0e15) snprintf(out, size, "%lld", (long long)d);
        else wire_fmt_f64(d, out, size);
        return true;
    }
    if (cJSON_IsBool(v)) {
        snprintf(out, size, "%s", cJSON_IsTrue(v) ? "true" : "false");
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ requests

bool wire_grinder_path(const char *path) {
    size_t n = strlen(path);
    while (n && path[n - 1] == '/') n--;
    static const char prefix[] = "/api/v2";
    size_t p = sizeof prefix - 1;
    return n >= p && !strncmp(path, prefix, p) && (n == p || path[p] == '/');
}

void wire_classify(const char *method, const char *path, const uint8_t *body, size_t len,
                    wire_request_t *out) {
    memset(out, 0, sizeof *out);
    // wire.rs:292: path.trim_end_matches('/')
    char p[96];
    size_t n = strlen(path);
    while (n && path[n - 1] == '/') n--;
    if (n >= sizeof p) {
        out->kind = WREQ_OTHER;
        return;
    }
    memcpy(p, path, n);
    p[n] = 0;

    bool get = !strcasecmp(method, "GET"), post = !strcasecmp(method, "POST");
    if (get && !strcmp(p, WIRE_PATH_MACHINE)) {
        out->kind = WREQ_IDENTITY;
    } else if (get && !strcmp(p, WIRE_PATH_MAKO)) {
        out->kind = WREQ_POLL_MAKO;
    } else if (post && (!strcmp(p, WIRE_PATH_BREWRATIO) || !strcmp(p, WIRE_PATH_SCRIPTS_EXECUTE))) {
        cJSON *root = cJSON_ParseWithLength((const char *)body, len);
        if (!root) {
            out->kind = WREQ_MALFORMED;
            return;
        }
        if (!strcmp(p, WIRE_PATH_BREWRATIO)) {
            // wire.rs:245-252
            out->kind = WREQ_GRIND_RESULT;
            out->grind.has_target =
                number_like(cJSON_GetObjectItemCaseSensitive(root, "SYNC_BEVERAGE_WEIGHT"),
                            &out->grind.target_weight_g);
            out->grind.has_filter = text_like(cJSON_GetObjectItemCaseSensitive(root, "SYNC_FILTER"),
                                              out->grind.filter, sizeof out->grind.filter);
        } else {
            // wire.rs:263-269: ID as number_like, then `as i64` (truncates)
            out->kind = WREQ_START_BREW;
            double id;
            if (number_like(cJSON_GetObjectItemCaseSensitive(root, "ID"), &id) && isfinite(id)) {
                out->has_script_id = true;
                out->script_id = (int64_t)id;
            }
        }
        cJSON_Delete(root);
    } else {
        out->kind = WREQ_OTHER;
    }
}

// ------------------------------------------------------------------ replies

typedef struct {
    char *buf;
    size_t size;
    size_t len;  // may exceed size (then the output is truncated)
} jw_t;

static void jw_raw(jw_t *w, const char *s) {
    size_t n = strlen(s);
    if (w->len < w->size) {
        size_t room = w->size - w->len - 1;
        memcpy(w->buf + w->len, s, n < room ? n : room);
        w->buf[w->len + (n < room ? n : room)] = 0;
    }
    w->len += n;
}

static void jw_key(jw_t *w, const char *key) {
    jw_raw(w, w->len > 1 ? ",\"" : "\"");
    jw_raw(w, key);
    jw_raw(w, "\":");
}

static void jw_int(jw_t *w, const char *key, long long v) {
    char t[24];
    snprintf(t, sizeof t, "%lld", v);
    jw_key(w, key);
    jw_raw(w, t);
}

static void jw_f64(jw_t *w, const char *key, double v) {
    char t[40];
    wire_fmt_f64(v, t, sizeof t);
    jw_key(w, key);
    jw_raw(w, t);
}

static void jw_str(jw_t *w, const char *key, const char *s) {
    jw_key(w, key);
    jw_raw(w, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char t[8];
        if (*p == '"' || *p == '\\') {
            t[0] = '\\', t[1] = (char)*p, t[2] = 0;
        } else if (*p < 0x20) {
            snprintf(t, sizeof t, "\\u%04x", *p);
        } else {
            t[0] = (char)*p, t[1] = 0;
        }
        jw_raw(w, t);
    }
    jw_raw(w, "\"");
}

int wire_mako_json(const mako_state_t *s, char *buf, size_t size) {
    jw_t w = {buf, size, 0};
    if (size) buf[0] = 0;
    jw_raw(&w, "{");
    // Sorted by key (byte order: upper case before "script").
    jw_f64(&w, "BG_SENS_TEMP", s->bg_sens_temp);
    jw_f64(&w, "BG_SET_TEMP", s->bg_set_temp);
    jw_int(&w, "MA_EXTRACTION_STATUS", s->extraction_status);
    jw_int(&w, "MA_STATUS", s->status);
    jw_f64(&w, "PU_SENS_FLOW_METER_VOLUME", s->flow_meter_volume_ml);
    jw_int(&w, "REAL_EXTRACTION_TIME_MS", s->real_extraction_time_ms);
    jw_int(&w, "TANK_LEVEL", s->tank_level);
    jw_str(&w, "TIMESTAMP", s->timestamp);
    jw_str(&w, "script", s->script);
    jw_raw(&w, "}");
    return (int)w.len;
}

int wire_identity_json(const machine_identity_t *id, char *buf, size_t size) {
    jw_t w = {buf, size, 0};
    if (size) buf[0] = 0;
    jw_raw(&w, "{");
    jw_int(&w, "BLE", id->ble);
    jw_int(&w, "ESP_FW_MAJOR", id->esp_fw_major);
    jw_int(&w, "ESP_FW_MINOR", id->esp_fw_minor);
    jw_int(&w, "FW_VERSION_MAJOR", id->fw_major);
    jw_int(&w, "FW_VERSION_MINOR", id->fw_minor);
    jw_int(&w, "MA_DELAY", id->delay);
    jw_int(&w, "MA_FIX_WATER_SUPPLY", id->fix_water_supply);
    jw_int(&w, "MA_MAX_AMPERE", id->max_ampere);
    jw_str(&w, "MA_SN", id->serial);
    jw_int(&w, "MA_SUBTYPE", id->machine_subtype);
    jw_int(&w, "MA_TYPE", id->machine_type);
    jw_raw(&w, "}");
    return (int)w.len;
}
