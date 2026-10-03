/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Minimal JSON DOM for mod manifests (mod.json) and user mod settings.
 * Strict RFC 8259 apart from two conveniences for hand-written files:
 * // and slash-star comments, and a trailing comma before ] or }. */
#ifndef PC_MODS_JSON_H
#define PC_MODS_JSON_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum JsonType {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
} JsonType;

typedef struct JsonValue JsonValue;
struct JsonValue {
    JsonType type;
    bool boolean;
    double number;
    char* string;     /* JSON_STRING: UTF-8, NUL-terminated */
    char* key;        /* set when this value is an object member */
    JsonValue* child; /* first element/member of an array/object */
    JsonValue* next;  /* next sibling */
    size_t count;     /* array/object element count */
};

/* Parses `text` (len bytes). Returns NULL on error and, if err is non-NULL,
 * writes a "line N: reason" message into it. Free with json_free. */
JsonValue* json_parse(const char* text, size_t len, char* err, size_t err_size);
/* Reads and parses a whole file. */
JsonValue* json_parse_file(const char* path, char* err, size_t err_size);
void json_free(JsonValue* v);

/* Accessors. All are NULL-safe and return the fallback on a type mismatch. */
const JsonValue* json_get(const JsonValue* obj, const char* key);
const JsonValue* json_at(const JsonValue* arr, size_t index);
const char* json_string(const JsonValue* v, const char* fallback);
double json_number(const JsonValue* v, double fallback);
bool json_bool(const JsonValue* v, bool fallback);

#define JSON_FOREACH(it, parent)                                                                   \
    for (const JsonValue* it = (parent) ? (parent)->child : NULL; it != NULL; it = it->next)

#ifdef __cplusplus
}
#endif

#endif
