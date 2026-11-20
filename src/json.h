#ifndef TRAYCOIN_JSON_H
#define TRAYCOIN_JSON_H

#include <stddef.h>

typedef enum {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonVal JsonVal;

struct JsonVal {
    JsonType type;
    char    *key;      /* Non-null if member of an object */
    JsonVal *next;     /* Sibling in array or object */
    JsonVal *child;    /* First child for array or object */
    JsonVal *tail;     /* Last child for O(1) append */
    char    *str;      /* For JSON_STRING */
    double   num;      /* For JSON_NUMBER */
    int      boolean;  /* For JSON_BOOL */
};

/* Parse UTF-8 JSON text into a tree. Returns NULL on error. */
JsonVal *json_parse(const char *text);

/* Recursively free a JSON tree. */
void json_free(JsonVal *v);

/* Construction helpers for serialization */
JsonVal *json_new_obj(void);
JsonVal *json_new_arr(void);
JsonVal *json_new_str(const char *s);
JsonVal *json_new_num(double n);
JsonVal *json_new_bool(int b);
JsonVal *json_new_null(void);

void json_obj_add(JsonVal *obj, const char *key, JsonVal *val);
void json_arr_add(JsonVal *arr, JsonVal *val);

/* Serialize to newly heap-allocated UTF-8 string (free with json_str_free). */
char *json_stringify(const JsonVal *v, int pretty);
void  json_str_free(char *s);

/* Query helpers */
const JsonVal *json_get(const JsonVal *obj, const char *key);
const JsonVal *json_get_ci(const JsonVal *obj, const char *key);
const JsonVal *json_first_child(const JsonVal *v);
const JsonVal *json_at(const JsonVal *arr, int index);
int            json_len(const JsonVal *v);

/* Value extraction helpers */
const char *json_as_str(const JsonVal *v, const char *fallback);
int         json_as_double(const JsonVal *v, double *out);
int         json_as_int(const JsonVal *v, int fallback);
int         json_as_bool(const JsonVal *v, int fallback);

#endif /* TRAYCOIN_JSON_H */
