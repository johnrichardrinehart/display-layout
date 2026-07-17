#include "backend_kscreen.h"
#include "backend_common.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int kscreen_parse_outputs(const char *json, DisplayList *list, char *error,
                          size_t error_size) {
  jsmntok_t *tokens = NULL;
  if (json_parse(json, &tokens, error, error_size, "KScreen") < 0)
    return -1;
  *list = (DisplayList){0};
  if (tokens[0].type != JSMN_OBJECT)
    goto invalid;
  int outputs = json_object_get(json, tokens, 0, "outputs");
  if (outputs < 0 || tokens[outputs].type != JSMN_ARRAY)
    goto invalid;
  int cursor = outputs + 1;
  for (int i = 0;
       i < tokens[outputs].size && list->count < DISPLAY_LAYOUT_MAX_DISPLAYS;
       i++) {
    int object = cursor;
    cursor = json_token_skip(tokens, cursor);
    int token = json_object_get(json, tokens, object, "enabled");
    if (token < 0 || !json_token_bool(json, &tokens[token], false))
      continue;
    LayoutDisplay *d = &list->displays[list->count];
    token = json_object_get(json, tokens, object, "name");
    if (token >= 0)
      json_token_string(json, &tokens[token], d->connector,
                        sizeof(d->connector));
    token = json_object_get(json, tokens, object, "manufacturer");
    if (token >= 0)
      json_token_string(json, &tokens[token], d->make, sizeof(d->make));
    token = json_object_get(json, tokens, object, "model");
    if (token >= 0)
      json_token_string(json, &tokens[token], d->model, sizeof(d->model));
    int pos = json_object_get(json, tokens, object, "pos");
    if (pos >= 0) {
      token = json_object_get(json, tokens, pos, "x");
      d->x = token >= 0 ? json_token_int(json, &tokens[token], 0) : 0;
      token = json_object_get(json, tokens, pos, "y");
      d->y = token >= 0 ? json_token_int(json, &tokens[token], 0) : 0;
    }
    int size = json_object_get(json, tokens, object, "size");
    double scale = 1.0;
    token = json_object_get(json, tokens, object, "scale");
    if (token >= 0)
      scale = json_token_double(json, &tokens[token], 1.0);
    if (scale <= 0)
      scale = 1.0;
    if (size >= 0) {
      token = json_object_get(json, tokens, size, "width");
      d->width =
          token >= 0
              ? (int)lround(json_token_double(json, &tokens[token], 0) / scale)
              : 0;
      token = json_object_get(json, tokens, size, "height");
      d->height =
          token >= 0
              ? (int)lround(json_token_double(json, &tokens[token], 0) / scale)
              : 0;
    }
    int physical = json_object_get(json, tokens, object, "sizeMM");
    if (physical >= 0) {
      token = json_object_get(json, tokens, physical, "width");
      d->physical_width_mm =
          token >= 0 ? json_token_int(json, &tokens[token], 0) : 0;
      token = json_object_get(json, tokens, physical, "height");
      d->physical_height_mm =
          token >= 0 ? json_token_int(json, &tokens[token], 0) : 0;
    }
    d->enabled = d->connector[0] && d->width > 0 && d->height > 0;
    if (d->enabled)
      list->count++;
  }
  free(tokens);
  if (!list->count) {
    snprintf(error, error_size, "KScreen reported no active displays");
    return -1;
  }
  backend_sort_displays(list);
  return 0;
invalid:
  free(tokens);
  snprintf(error, error_size, "KScreen returned invalid output JSON");
  return -1;
}
static int kscreen_load(DisplayBackend *backend, DisplayList *list, char *error,
                        size_t error_size) {
  (void)backend;
  char *json = NULL;
  char *argv[] = {"kscreen-doctor", "--json", NULL};
  if (backend_capture(argv, &json, error, error_size) != 0)
    return -1;
  int result = kscreen_parse_outputs(json, list, error, error_size);
  free(json);
  return result;
}
static int kscreen_apply(DisplayBackend *backend, const DisplayList *list,
                         char *error, size_t error_size) {
  (void)backend;
  char settings[DISPLAY_LAYOUT_MAX_DISPLAYS][384];
  char *argv[DISPLAY_LAYOUT_MAX_DISPLAYS + 2];
  size_t a = 0;
  argv[a++] = "kscreen-doctor";
  for (size_t i = 0; i < list->count; i++) {
    snprintf(settings[i], sizeof(settings[i]), "output.%s.position.%d,%d",
             list->displays[i].connector, list->displays[i].x,
             list->displays[i].y);
    argv[a++] = settings[i];
  }
  argv[a] = NULL;
  return backend_run(argv, "KScreen", error, error_size);
}
static const DisplayBackendOps OPS = {.name = "kscreen",
                                      .load = kscreen_load,
                                      .apply = kscreen_apply,
                                      .identify = backend_identify,
                                      .destroy = backend_noop_destroy};
int backend_kscreen_open(DisplayBackend *backend, char *error,
                         size_t error_size) {
  (void)error;
  (void)error_size;
  backend->ops = &OPS;
  backend->state = NULL;
  return 0;
}
