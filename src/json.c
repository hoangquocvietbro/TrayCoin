/*
 * TrayCoin - Taskbar & Widget Crypto Tracker
 * Copyright (C) 2026 hoangquocvietbro (@hoangquocvietbro)
 * Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
 */
#include "json.h"
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

static void *jalloc(size_t n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n ? n : 1); }
static void *jrealloc(void *p, size_t n) { return p ? HeapReAlloc(GetProcessHeap(), 0, p, n) : jalloc(n); }
static void  jfree(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

static char *jstrdup_n(const char *s, size_t n)
{
    char *r = (char *)jalloc(n + 1);
    if (!r) return NULL;
    if (n) memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

static char *jstrdup(const char *s)
{
    return s ? jstrdup_n(s, strlen(s)) : jstrdup_n("", 0);
}

static JsonVal *jnew(JsonType t)
{
    JsonVal *v = (JsonVal *)jalloc(sizeof(JsonVal));
    if (v) v->type = t;
    return v;
}

JsonVal *json_new_obj(void)          { return jnew(JSON_OBJECT); }
JsonVal *json_new_arr(void)          { return jnew(JSON_ARRAY); }
JsonVal *json_new_null(void)         { return jnew(JSON_NULL); }
JsonVal *json_new_bool(int b)        { JsonVal *v = jnew(JSON_BOOL); if (v) v->boolean = b ? 1 : 0; return v; }
JsonVal *json_new_num(double n)      { JsonVal *v = jnew(JSON_NUMBER); if (v) v->num = n; return v; }
JsonVal *json_new_str(const char *s) { JsonVal *v = jnew(JSON_STRING); if (v) v->str = jstrdup(s); return v; }

void json_obj_add(JsonVal *obj, const char *key, JsonVal *val)
{
    if (!obj || obj->type != JSON_OBJECT || !val || !key) return;
    val->key = jstrdup(key);
    val->next = NULL;
    if (!obj->child) { obj->child = obj->tail = val; }
    else { obj->tail->next = val; obj->tail = val; }
}

void json_arr_add(JsonVal *arr, JsonVal *val)
{
    if (!arr || arr->type != JSON_ARRAY || !val) return;
    val->next = NULL;
    if (!arr->child) { arr->child = arr->tail = val; }
    else { arr->tail->next = val; arr->tail = val; }
}

void json_free(JsonVal *v)
{
    while (v) {
        JsonVal *next = v->next;
        if (v->child) json_free(v->child);
        if (v->key) jfree(v->key);
        if (v->str) jfree(v->str);
        jfree(v);
        v = next;
    }
}

/* ---- Parser ---- */

typedef struct {
    const char *p;
    int depth;
} JParser;

static void skip_ws(JParser *jp)
{
    while (*jp->p == ' ' || *jp->p == '\t' || *jp->p == '\r' || *jp->p == '\n') jp->p++;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex4(const char *s, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hex_val(s[i]);
        if (h < 0) return 0;
        v = (v << 4) | (unsigned)h;
    }
    *out = v;
    return 1;
}

static void append_utf8(char **buf, size_t *len, size_t *cap, unsigned cp)
{
    if (*len + 8 >= *cap) {
        size_t nc = (*cap < 64) ? 64 : (*cap * 2);
        char *nb = (char *)jrealloc(*buf, nc);
        if (!nb) return;
        *buf = nb; *cap = nc;
    }
    char *o = *buf + *len;
    if (cp < 0x80) {
        o[0] = (char)cp; *len += 1;
    } else if (cp < 0x800) {
        o[0] = (char)(0xC0 | (cp >> 6));
        o[1] = (char)(0x80 | (cp & 0x3F));
        *len += 2;
    } else if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12));
        o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        *len += 3;
    } else {
        o[0] = (char)(0xF0 | (cp >> 18));
        o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[3] = (char)(0x80 | (cp & 0x3F));
        *len += 4;
    }
}

static char *parse_string(JParser *jp)
{
    if (*jp->p != '"') return NULL;
    jp->p++;
    size_t cap = 64, len = 0;
    char *buf = (char *)jalloc(cap);
    if (!buf) return NULL;

    while (*jp->p && *jp->p != '"') {
        unsigned char c = (unsigned char)*jp->p++;
        if (c == '\\') {
            char e = *jp->p++;
            if (!e) { jfree(buf); return NULL; }
            switch (e) {
            case '"':  c = '"';  break;
            case '\\': c = '\\'; break;
            case '/':  c = '/';  break;
            case 'b':  c = '\b'; break;
            case 'f':  c = '\f'; break;
            case 'n':  c = '\n'; break;
            case 'r':  c = '\r'; break;
            case 't':  c = '\t'; break;
            case 'u': {
                unsigned cp = 0;
                if (!parse_hex4(jp->p, &cp)) { jfree(buf); return NULL; }
                jp->p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && jp->p[0] == '\\' && jp->p[1] == 'u') {
                    unsigned lo = 0;
                    if (parse_hex4(jp->p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        jp->p += 6;
                        cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
                    }
                }
                append_utf8(&buf, &len, &cap, cp);
                continue;
            }
            default: c = (unsigned char)e; break;
            }
        }
        if (len + 2 >= cap) {
            size_t nc = cap * 2;
            char *nb = (char *)jrealloc(buf, nc);
            if (!nb) { jfree(buf); return NULL; }
            buf = nb; cap = nc;
        }
        buf[len++] = (char)c;
    }
    if (*jp->p != '"') { jfree(buf); return NULL; }
    jp->p++;
    buf[len] = '\0';
    return buf;
}

static JsonVal *parse_value(JParser *jp);

static JsonVal *parse_array(JParser *jp)
{
    jp->p++; /* skip '[' */
    JsonVal *arr = json_new_arr();
    if (!arr) return NULL;
    skip_ws(jp);
    if (*jp->p == ']') { jp->p++; return arr; }

    for (;;) {
        skip_ws(jp);
        JsonVal *el = parse_value(jp);
        if (!el) { json_free(arr); return NULL; }
        json_arr_add(arr, el);
        skip_ws(jp);
        if (*jp->p == ',') { jp->p++; continue; }
        if (*jp->p == ']') { jp->p++; return arr; }
        json_free(arr);
        return NULL;
    }
}

static JsonVal *parse_object(JParser *jp)
{
    jp->p++; /* skip '{' */
    JsonVal *obj = json_new_obj();
    if (!obj) return NULL;
    skip_ws(jp);
    if (*jp->p == '}') { jp->p++; return obj; }

    for (;;) {
        skip_ws(jp);
        char *key = parse_string(jp);
        if (!key) { json_free(obj); return NULL; }
        skip_ws(jp);
        if (*jp->p != ':') { jfree(key); json_free(obj); return NULL; }
        jp->p++;
        skip_ws(jp);
        JsonVal *val = parse_value(jp);
        if (!val) { jfree(key); json_free(obj); return NULL; }
        val->key = key;
        val->next = NULL;
        if (!obj->child) { obj->child = obj->tail = val; }
        else { obj->tail->next = val; obj->tail = val; }

        skip_ws(jp);
        if (*jp->p == ',') { jp->p++; continue; }
        if (*jp->p == '}') { jp->p++; return obj; }
        json_free(obj);
        return NULL;
    }
}

static JsonVal *parse_number(JParser *jp)
{
    char *end = NULL;
    double n = strtod(jp->p, &end);
    if (end == jp->p) return NULL;
    jp->p = end;
    return json_new_num(n);
}

static JsonVal *parse_value(JParser *jp)
{
    if (jp->depth > 64) return NULL;
    skip_ws(jp);
    char c = *jp->p;
    if (!c) return NULL;
    if (c == '{') {
        jp->depth++;
        JsonVal *v = parse_object(jp);
        jp->depth--;
        return v;
    }
    if (c == '[') {
        jp->depth++;
        JsonVal *v = parse_array(jp);
        jp->depth--;
        return v;
    }
    if (c == '"') {
        char *s = parse_string(jp);
        if (!s) return NULL;
        JsonVal *v = jnew(JSON_STRING);
        if (!v) { jfree(s); return NULL; }
        v->str = s;
        return v;
    }
    if (c == 't' && strncmp(jp->p, "true", 4) == 0)  { jp->p += 4; return json_new_bool(1); }
    if (c == 'f' && strncmp(jp->p, "false", 5) == 0) { jp->p += 5; return json_new_bool(0); }
    if (c == 'n' && strncmp(jp->p, "null", 4) == 0)  { jp->p += 4; return json_new_null(); }
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(jp);
    return NULL;
}

JsonVal *json_parse(const char *text)
{
    if (!text) return NULL;
    /* Skip optional UTF-8 BOM */
    if ((unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text += 3;
    JParser jp = { text, 0 };
    return parse_value(&jp);
}

/* ---- Serializer ---- */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    int    err;
} StrBuf;

static void sb_reserve(StrBuf *sb, size_t add)
{
    if (sb->err) return;
    if (sb->len + add + 1 <= sb->cap) return;
    size_t nc = sb->cap ? sb->cap * 2 : 256;
    while (nc < sb->len + add + 1) nc *= 2;
    char *nb = (char *)jrealloc(sb->buf, nc);
    if (!nb) { sb->err = 1; return; }
    sb->buf = nb;
    sb->cap = nc;
}

static void sb_append(StrBuf *sb, const char *s, size_t n)
{
    sb_reserve(sb, n);
    if (sb->err) return;
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static void sb_puts(StrBuf *sb, const char *s) { sb_append(sb, s, strlen(s)); }
static void sb_putc(StrBuf *sb, char c)        { sb_append(sb, &c, 1); }

static void sb_indent(StrBuf *sb, int level)
{
    for (int i = 0; i < level; i++) sb_puts(sb, "  ");
}

static void sb_escape_str(StrBuf *sb, const char *s)
{
    sb_putc(sb, '"');
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  sb_puts(sb, "\\\""); break;
        case '\\': sb_puts(sb, "\\\\"); break;
        case '\b': sb_puts(sb, "\\b"); break;
        case '\f': sb_puts(sb, "\\f"); break;
        case '\n': sb_puts(sb, "\\n"); break;
        case '\r': sb_puts(sb, "\\r"); break;
        case '\t': sb_puts(sb, "\\t"); break;
        default:
            if (c < 0x20) {
                char u[8];
                _snprintf(u, sizeof(u), "\\u%04x", (unsigned)c);
                sb_puts(sb, u);
            } else {
                sb_putc(sb, (char)c);
            }
            break;
        }
    }
    sb_putc(sb, '"');
}

static void emit_val(StrBuf *sb, const JsonVal *v, int pretty, int depth)
{
    if (!v) { sb_puts(sb, "null"); return; }
    switch (v->type) {
    case JSON_NULL:
        sb_puts(sb, "null");
        break;
    case JSON_BOOL:
        sb_puts(sb, v->boolean ? "true" : "false");
        break;
    case JSON_NUMBER: {
        char tmp[64];
        if (isnan(v->num) || isinf(v->num)) sb_puts(sb, "0");
        else if (v->num == (double)(long long)v->num && fabs(v->num) < 1e15) {
            _snprintf(tmp, sizeof(tmp), "%lld", (long long)v->num);
            sb_puts(sb, tmp);
        } else {
            _snprintf(tmp, sizeof(tmp), "%.12g", v->num);
            sb_puts(sb, tmp);
        }
        break;
    }
    case JSON_STRING:
        sb_escape_str(sb, v->str ? v->str : "");
        break;
    case JSON_ARRAY: {
        sb_putc(sb, '[');
        const JsonVal *c = v->child;
        if (c && pretty) sb_putc(sb, '\n');
        while (c) {
            if (pretty) sb_indent(sb, depth + 1);
            emit_val(sb, c, pretty, depth + 1);
            if (c->next) sb_putc(sb, ',');
            if (pretty) sb_putc(sb, '\n');
            c = c->next;
        }
        if (v->child && pretty) sb_indent(sb, depth);
        sb_putc(sb, ']');
        break;
    }
    case JSON_OBJECT: {
        sb_putc(sb, '{');
        const JsonVal *c = v->child;
        if (c && pretty) sb_putc(sb, '\n');
        while (c) {
            if (pretty) sb_indent(sb, depth + 1);
            sb_escape_str(sb, c->key ? c->key : "");
            sb_puts(sb, pretty ? ": " : ":");
            emit_val(sb, c, pretty, depth + 1);
            if (c->next) sb_putc(sb, ',');
            if (pretty) sb_putc(sb, '\n');
            c = c->next;
        }
        if (v->child && pretty) sb_indent(sb, depth);
        sb_putc(sb, '}');
        break;
    }
    }
}

char *json_stringify(const JsonVal *v, int pretty)
{
    StrBuf sb = { NULL, 0, 0, 0 };
    sb_reserve(&sb, 256);
    emit_val(&sb, v, pretty, 0);
    if (sb.err) { jfree(sb.buf); return NULL; }
    return sb.buf;
}

void json_str_free(char *s) { jfree(s); }

/* ---- Query helpers ---- */

const JsonVal *json_get(const JsonVal *obj, const char *key)
{
    if (!obj || obj->type != JSON_OBJECT || !key) return NULL;
    for (const JsonVal *c = obj->child; c; c = c->next)
        if (c->key && strcmp(c->key, key) == 0) return c;
    return NULL;
}

const JsonVal *json_get_ci(const JsonVal *obj, const char *key)
{
    if (!obj || obj->type != JSON_OBJECT || !key) return NULL;
    for (const JsonVal *c = obj->child; c; c = c->next)
        if (c->key && _stricmp(c->key, key) == 0) return c;
    return NULL;
}

const JsonVal *json_first_child(const JsonVal *v)
{
    return v ? v->child : NULL;
}

const JsonVal *json_at(const JsonVal *arr, int index)
{
    if (!arr || arr->type != JSON_ARRAY || index < 0) return NULL;
    const JsonVal *c = arr->child;
    while (c && index > 0) { c = c->next; index--; }
    return c;
}

int json_len(const JsonVal *v)
{
    if (!v || (v->type != JSON_ARRAY && v->type != JSON_OBJECT)) return 0;
    int n = 0;
    for (const JsonVal *c = v->child; c; c = c->next) n++;
    return n;
}

const char *json_as_str(const JsonVal *v, const char *fallback)
{
    if (!v || v->type != JSON_STRING || !v->str) return fallback;
    return v->str;
}

int json_as_double(const JsonVal *v, double *out)
{
    if (!v || !out) return 0;
    if (v->type == JSON_NUMBER) {
        if (isnan(v->num) || isinf(v->num)) return 0;
        *out = v->num;
        return 1;
    }
    if (v->type == JSON_STRING && v->str && v->str[0]) {
        char *end = NULL;
        double d = strtod(v->str, &end);
        if (end != v->str && !isnan(d) && !isinf(d)) {
            *out = d;
            return 1;
        }
    }
    return 0;
}

int json_as_int(const JsonVal *v, int fallback)
{
    double d = 0;
    if (json_as_double(v, &d)) return (int)d;
    if (v && v->type == JSON_BOOL) return v->boolean;
    return fallback;
}

int json_as_bool(const JsonVal *v, int fallback)
{
    if (!v) return fallback;
    if (v->type == JSON_BOOL) return v->boolean;
    if (v->type == JSON_NUMBER) return v->num != 0.0;
    return fallback;
}
