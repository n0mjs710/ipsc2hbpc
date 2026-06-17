/* toml.h — minimal TOML reader for the ipsc2hbpc config schema.
 * Supports: [section] / [a.b] tables, key = value, quoted strings (basic and
 * literal), integers, booleans, and single-line arrays of ints or strings.
 * Sufficient for this project's config; not a general TOML implementation. */
#ifndef TOML_H
#define TOML_H

#include <stddef.h>

typedef enum { TOML_NONE, TOML_STRING, TOML_INT, TOML_BOOL, TOML_ARRAY } toml_type;

typedef struct toml_value {
    toml_type type;
    char     *s;        /* string value (TOML_STRING) */
    long long i;        /* integer value (TOML_INT) */
    int       b;        /* boolean value (TOML_BOOL) */
    struct toml_value *arr;  /* array elements (TOML_ARRAY) */
    int       arrlen;
} toml_value;

typedef struct toml toml;

/* Parse a TOML file.  Returns NULL on open/parse error (err filled). */
toml *toml_parse_file(const char *path, char *err, size_t errlen);
void  toml_free(toml *t);

/* Look up section.key.  Returns NULL if absent. */
const toml_value *toml_get(const toml *t, const char *section, const char *key);

#endif
