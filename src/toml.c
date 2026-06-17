/* toml.c — minimal TOML reader (see toml.h). */
#include "toml.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_ENTRIES 256

typedef struct {
    char       section[64];
    char       key[64];
    toml_value val;
} entry;

struct toml {
    entry entries[MAX_ENTRIES];
    int   n;
};

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) *e-- = 0;
    return s;
}

/* Truncate the line at the first '#' that is outside a quoted string. */
static void strip_comment(char *line)
{
    int in_basic = 0, in_lit = 0;
    for (char *p = line; *p; p++) {
        if (in_basic) { if (*p == '"' ) in_basic = 0; }
        else if (in_lit) { if (*p == '\'') in_lit = 0; }
        else if (*p == '"') in_basic = 1;
        else if (*p == '\'') in_lit = 1;
        else if (*p == '#') { *p = 0; return; }
    }
}

/* Parse a basic-string body starting after the opening quote; writes to out. */
static void parse_basic_string(const char *p, char *out, size_t cap)
{
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': out[n++] = '\n'; break;
                case 't': out[n++] = '\t'; break;
                case 'r': out[n++] = '\r'; break;
                case '"': out[n++] = '"';  break;
                case '\\': out[n++] = '\\'; break;
                default:  out[n++] = *p;   break;
            }
            p++;
        } else {
            out[n++] = *p++;
        }
    }
    out[n] = 0;
}

/* Parse a scalar value token into v.  Returns 0 on success, -1 on error. */
static int parse_scalar(const char *tok, toml_value *v)
{
    if (tok[0] == '"') {
        char buf[512];
        parse_basic_string(tok + 1, buf, sizeof buf);
        v->type = TOML_STRING;
        v->s = strdup(buf);
        return 0;
    }
    if (tok[0] == '\'') {
        char buf[512];
        size_t n = 0; const char *p = tok + 1;
        while (*p && *p != '\'' && n + 1 < sizeof buf) buf[n++] = *p++;
        buf[n] = 0;
        v->type = TOML_STRING;
        v->s = strdup(buf);
        return 0;
    }
    if (!strcmp(tok, "true") || !strcmp(tok, "false")) {
        v->type = TOML_BOOL;
        v->b = tok[0] == 't';
        return 0;
    }
    /* integer (allow +/- and underscore separators) */
    char clean[64]; size_t n = 0;
    for (const char *p = tok; *p && n + 1 < sizeof clean; p++)
        if (*p != '_') clean[n++] = *p;
    clean[n] = 0;
    if (clean[0] == 0) return -1;
    char *end;
    long long iv = strtoll(clean, &end, 10);
    if (*end != 0) return -1;
    v->type = TOML_INT;
    v->i = iv;
    return 0;
}

/* Parse a single-line array body (without the surrounding brackets). */
static int parse_array(const char *body, toml_value *v)
{
    v->type = TOML_ARRAY;
    v->arr = NULL;
    v->arrlen = 0;
    int cap = 0;
    const char *p = body;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p) break;
        /* find end of this element */
        char tok[256]; size_t tn = 0;
        if (*p == '"' || *p == '\'') {
            char q = *p; tok[tn++] = *p++;
            while (*p && tn + 1 < sizeof tok) {
                tok[tn++] = *p;
                if (*p == q) { p++; break; }
                p++;
            }
        } else {
            while (*p && *p != ',' && tn + 1 < sizeof tok) tok[tn++] = *p++;
        }
        tok[tn] = 0;
        char *t = trim(tok);
        if (!*t) continue;
        toml_value ev; memset(&ev, 0, sizeof ev);
        if (parse_scalar(t, &ev) != 0) return -1;
        if (v->arrlen >= cap) {
            cap = cap ? cap * 2 : 8;
            v->arr = realloc(v->arr, (size_t)cap * sizeof(toml_value));
        }
        v->arr[v->arrlen++] = ev;
    }
    return 0;
}

toml *toml_parse_file(const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(err, errlen, "Config file not found: %s", path);
        return NULL;
    }
    toml *t = calloc(1, sizeof *t);
    char cur_section[64] = "";
    char line[1024];
    int lineno = 0;

    while (fgets(line, sizeof line, f)) {
        lineno++;
        strip_comment(line);
        char *s = trim(line);
        if (!*s) continue;

        if (*s == '[') {
            char *close = strchr(s, ']');
            if (!close) { snprintf(err, errlen, "line %d: malformed section", lineno); goto fail; }
            *close = 0;
            char *name = trim(s + 1);
            snprintf(cur_section, sizeof cur_section, "%s", name);
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) { snprintf(err, errlen, "line %d: expected key = value", lineno); goto fail; }
        *eq = 0;
        char *key = trim(s);
        char *val = trim(eq + 1);
        if (!*key) { snprintf(err, errlen, "line %d: empty key", lineno); goto fail; }

        if (t->n >= MAX_ENTRIES) { snprintf(err, errlen, "too many config entries"); goto fail; }
        entry *e = &t->entries[t->n];
        snprintf(e->section, sizeof e->section, "%s", cur_section);
        snprintf(e->key, sizeof e->key, "%s", key);
        memset(&e->val, 0, sizeof e->val);

        if (*val == '[') {
            char *close = strrchr(val, ']');
            if (!close) { snprintf(err, errlen, "line %d: unterminated array", lineno); goto fail; }
            *close = 0;
            if (parse_array(val + 1, &e->val) != 0) {
                snprintf(err, errlen, "line %d: bad array value", lineno); goto fail;
            }
        } else {
            if (parse_scalar(val, &e->val) != 0) {
                snprintf(err, errlen, "line %d: bad value for %s", lineno, key); goto fail;
            }
        }
        t->n++;
    }
    fclose(f);
    return t;

fail:
    fclose(f);
    toml_free(t);
    return NULL;
}

void toml_free(toml *t)
{
    if (!t) return;
    for (int i = 0; i < t->n; i++) {
        toml_value *v = &t->entries[i].val;
        if (v->type == TOML_STRING) free(v->s);
        if (v->type == TOML_ARRAY) {
            for (int j = 0; j < v->arrlen; j++)
                if (v->arr[j].type == TOML_STRING) free(v->arr[j].s);
            free(v->arr);
        }
    }
    free(t);
}

const toml_value *toml_get(const toml *t, const char *section, const char *key)
{
    for (int i = 0; i < t->n; i++)
        if (!strcmp(t->entries[i].section, section) && !strcmp(t->entries[i].key, key))
            return &t->entries[i].val;
    return NULL;
}
