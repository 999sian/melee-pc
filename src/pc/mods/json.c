/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "json.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 64

typedef struct Parser {
    const char* p;
    const char* end;
    const char* start;
    char* err;
    size_t err_size;
    bool failed;
} Parser;

static void fail(Parser* ps, const char* fmt, ...) {
    if (ps->failed)
        return;
    ps->failed = true;
    if (ps->err == NULL || ps->err_size == 0)
        return;
    int line = 1;
    for (const char* c = ps->start; c < ps->p && c < ps->end; ++c)
        if (*c == '\n')
            ++line;
    int n = snprintf(ps->err, ps->err_size, "line %d: ", line);
    if (n < 0 || (size_t)n >= ps->err_size)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ps->err + n, ps->err_size - (size_t)n, fmt, ap);
    va_end(ap);
}

static void skip_ws(Parser* ps) {
    while (ps->p < ps->end) {
        char c = *ps->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++ps->p;
        } else if (c == '/' && ps->p + 1 < ps->end && ps->p[1] == '/') {
            while (ps->p < ps->end && *ps->p != '\n')
                ++ps->p;
        } else if (c == '/' && ps->p + 1 < ps->end && ps->p[1] == '*') {
            ps->p += 2;
            while (ps->p + 1 < ps->end && !(ps->p[0] == '*' && ps->p[1] == '/'))
                ++ps->p;
            if (ps->p + 1 >= ps->end) {
                fail(ps, "unterminated comment");
                return;
            }
            ps->p += 2;
        } else {
            break;
        }
    }
}

static JsonValue* new_value(JsonType type) {
    JsonValue* v = (JsonValue*)calloc(1, sizeof(JsonValue));
    if (v)
        v->type = type;
    return v;
}

static void put_utf8(char** out, uint32_t cp) {
    char* o = *out;
    if (cp < 0x80) {
        *o++ = (char)cp;
    } else if (cp < 0x800) {
        *o++ = (char)(0xC0 | (cp >> 6));
        *o++ = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *o++ = (char)(0xE0 | (cp >> 12));
        *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *o++ = (char)(0xF0 | (cp >> 18));
        *o++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    }
    *out = o;
}

static int hex4(const char* s, uint32_t* out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= (uint32_t)(c - 'A' + 10);
        else
            return 0;
    }
    *out = v;
    return 1;
}

/* Called with ps->p on the opening quote. */
static char* parse_string_raw(Parser* ps) {
    ++ps->p;
    const char* s = ps->p;
    size_t raw = 0;
    while (s + raw < ps->end && s[raw] != '"') {
        if (s[raw] == '\\')
            ++raw;
        ++raw;
    }
    if (s + raw >= ps->end) {
        fail(ps, "unterminated string");
        return NULL;
    }
    /* Escapes only ever shrink, so the raw length bounds the output. */
    char* buf = (char*)malloc(raw + 1);
    if (buf == NULL) {
        fail(ps, "out of memory");
        return NULL;
    }
    char* o = buf;
    while (*ps->p != '"') {
        unsigned char c = (unsigned char)*ps->p;
        if (c < 0x20) {
            fail(ps, "control character in string");
            free(buf);
            return NULL;
        }
        if (c != '\\') {
            *o++ = (char)c;
            ++ps->p;
            continue;
        }
        ++ps->p;
        char e = *ps->p++;
        switch (e) {
        case '"':
            *o++ = '"';
            break;
        case '\\':
            *o++ = '\\';
            break;
        case '/':
            *o++ = '/';
            break;
        case 'b':
            *o++ = '\b';
            break;
        case 'f':
            *o++ = '\f';
            break;
        case 'n':
            *o++ = '\n';
            break;
        case 'r':
            *o++ = '\r';
            break;
        case 't':
            *o++ = '\t';
            break;
        case 'u': {
            uint32_t cp;
            if (ps->end - ps->p < 4 || !hex4(ps->p, &cp)) {
                fail(ps, "bad \\u escape");
                free(buf);
                return NULL;
            }
            ps->p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && ps->end - ps->p >= 6 && ps->p[0] == '\\' &&
                ps->p[1] == 'u')
            {
                uint32_t lo;
                if (hex4(ps->p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    ps->p += 6;
                }
            }
            put_utf8(&o, cp);
            break;
        }
        default:
            fail(ps, "bad escape '\\%c'", e);
            free(buf);
            return NULL;
        }
    }
    ++ps->p;
    *o = '\0';
    return buf;
}

static JsonValue* parse_value(Parser* ps, int depth);

static JsonValue* parse_container(Parser* ps, int depth, bool object) {
    const char close = object ? '}' : ']';
    JsonValue* v = new_value(object ? JSON_OBJECT : JSON_ARRAY);
    if (v == NULL) {
        fail(ps, "out of memory");
        return NULL;
    }
    ++ps->p;
    JsonValue** tail = &v->child;
    for (;;) {
        skip_ws(ps);
        if (ps->failed)
            break;
        if (ps->p < ps->end && *ps->p == close) {
            ++ps->p;
            return v;
        }
        char* key = NULL;
        if (object) {
            if (ps->p >= ps->end || *ps->p != '"') {
                fail(ps, "expected a quoted key");
                break;
            }
            key = parse_string_raw(ps);
            if (key == NULL)
                break;
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') {
                fail(ps, "expected ':' after key \"%s\"", key);
                free(key);
                break;
            }
            ++ps->p;
        }
        JsonValue* item = parse_value(ps, depth + 1);
        if (item == NULL) {
            free(key);
            break;
        }
        item->key = key;
        *tail = item;
        tail = &item->next;
        ++v->count;
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ++ps->p;
            continue;
        }
        if (ps->p < ps->end && *ps->p == close) {
            ++ps->p;
            return v;
        }
        fail(ps, "expected ',' or '%c'", close);
        break;
    }
    json_free(v);
    return NULL;
}

static JsonValue* parse_value(Parser* ps, int depth) {
    if (depth > JSON_MAX_DEPTH) {
        fail(ps, "nesting too deep");
        return NULL;
    }
    skip_ws(ps);
    if (ps->failed)
        return NULL;
    if (ps->p >= ps->end) {
        fail(ps, "unexpected end of input");
        return NULL;
    }
    char c = *ps->p;
    if (c == '{' || c == '[')
        return parse_container(ps, depth, c == '{');
    if (c == '"') {
        char* s = parse_string_raw(ps);
        if (s == NULL)
            return NULL;
        JsonValue* v = new_value(JSON_STRING);
        if (v == NULL) {
            free(s);
            fail(ps, "out of memory");
            return NULL;
        }
        v->string = s;
        return v;
    }
    static const struct {
        const char* word;
        JsonType type;
        bool value;
    } words[] = {
        {"true", JSON_BOOL, true}, {"false", JSON_BOOL, false}, {"null", JSON_NULL, false}};
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
        size_t n = strlen(words[i].word);
        if ((size_t)(ps->end - ps->p) >= n && memcmp(ps->p, words[i].word, n) == 0) {
            ps->p += n;
            JsonValue* v = new_value(words[i].type);
            if (v)
                v->boolean = words[i].value;
            return v;
        }
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char tmp[64];
        size_t n = 0;
        while (ps->p + n < ps->end && n < sizeof(tmp) - 1 &&
               (isdigit((unsigned char)ps->p[n]) || strchr("+-.eE", ps->p[n]) != NULL))
        {
            tmp[n] = ps->p[n];
            ++n;
        }
        tmp[n] = '\0';
        char* endp = NULL;
        double d = strtod(tmp, &endp);
        if (endp == tmp || (size_t)(endp - tmp) != n) {
            fail(ps, "bad number '%s'", tmp);
            return NULL;
        }
        ps->p += n;
        JsonValue* v = new_value(JSON_NUMBER);
        if (v)
            v->number = d;
        return v;
    }
    fail(ps, "unexpected character '%c'", c);
    return NULL;
}

JsonValue* json_parse(const char* text, size_t len, char* err, size_t err_size) {
    Parser ps = {text, text + len, text, err, err_size, false};
    if (err && err_size)
        err[0] = '\0';
    /* UTF-8 BOM, which Windows editors like to add. */
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        ps.p += 3;
    JsonValue* v = parse_value(&ps, 0);
    if (v == NULL)
        return NULL;
    skip_ws(&ps);
    if (!ps.failed && ps.p != ps.end)
        fail(&ps, "trailing characters after the document");
    if (ps.failed) {
        json_free(v);
        return NULL;
    }
    return v;
}

JsonValue* json_parse_file(const char* path, char* err, size_t err_size) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        if (err && err_size)
            snprintf(err, err_size, "cannot open %s", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || size > 16 * 1024 * 1024) {
        fclose(f);
        if (err && err_size)
            snprintf(err, err_size, "%s is not a readable size", path);
        return NULL;
    }
    char* buf = (char*)malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    JsonValue* v = json_parse(buf, got, err, err_size);
    free(buf);
    return v;
}

void json_free(JsonValue* v) {
    while (v != NULL) {
        JsonValue* next = v->next;
        json_free(v->child);
        free(v->string);
        free(v->key);
        free(v);
        v = next;
    }
}

const JsonValue* json_get(const JsonValue* obj, const char* key) {
    if (obj == NULL || obj->type != JSON_OBJECT || key == NULL)
        return NULL;
    JSON_FOREACH(it, obj)
    if (it->key && strcmp(it->key, key) == 0)
        return it;
    return NULL;
}

const JsonValue* json_at(const JsonValue* arr, size_t index) {
    if (arr == NULL || arr->type != JSON_ARRAY)
        return NULL;
    JSON_FOREACH(it, arr) {
        if (index == 0)
            return it;
        --index;
    }
    return NULL;
}

const char* json_string(const JsonValue* v, const char* fallback) {
    return v && v->type == JSON_STRING ? v->string : fallback;
}

double json_number(const JsonValue* v, double fallback) {
    return v && v->type == JSON_NUMBER ? v->number : fallback;
}

bool json_bool(const JsonValue* v, bool fallback) {
    return v && v->type == JSON_BOOL ? v->boolean : fallback;
}
