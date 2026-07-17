#ifndef DISPLAY_LAYOUT_BACKEND_COMMON_H
#define DISPLAY_LAYOUT_BACKEND_COMMON_H

#include "backend.h"
#define JSMN_HEADER
#include "third_party/jsmn.h"
#undef JSMN_HEADER

#include <stdbool.h>
#include <stddef.h>

#define BACKEND_JSON_TOKENS 8192

int backend_capture(char *const argv[], char **output, char *error,
                    size_t error_size);
int backend_run(char *const argv[], const char *backend_name, char *error,
                size_t error_size);
int backend_identify(DisplayBackend *backend, const DisplayList *list,
                     unsigned int duration_ms, char *error, size_t error_size);
void backend_noop_destroy(DisplayBackend *backend);
void backend_sort_displays(DisplayList *list);

bool json_token_equals(const char *json, const jsmntok_t *token,
                       const char *text);
int json_token_skip(const jsmntok_t *tokens, int index);
int json_object_get(const char *json, const jsmntok_t *tokens, int object,
                    const char *key);
void json_token_string(const char *json, const jsmntok_t *token, char *output,
                       size_t size);
int json_token_int(const char *json, const jsmntok_t *token, int fallback);
double json_token_double(const char *json, const jsmntok_t *token,
                         double fallback);
bool json_token_bool(const char *json, const jsmntok_t *token, bool fallback);
int json_parse(const char *json, jsmntok_t **tokens, char *error,
               size_t error_size, const char *backend_name);

#endif
