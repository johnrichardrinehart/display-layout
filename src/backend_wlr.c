#include "backend_wlr.h"
#include "backend_common.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int wlr_parse_outputs(const char *json, DisplayList *list, char *error,
                      size_t error_size) {
  jsmntok_t *tokens = NULL;
  if (json_parse(json, &tokens, error, error_size, "wlr-randr") < 0)
    return -1;
  *list = (DisplayList){0};
  if (tokens[0].type != JSMN_ARRAY)
    goto invalid;
  int cursor = 1;
  for (int i = 0;
       i < tokens[0].size && list->count < DISPLAY_LAYOUT_MAX_DISPLAYS; i++) {
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
    token = json_object_get(json, tokens, object, "make");
    if (token >= 0)
      json_token_string(json, &tokens[token], d->make, sizeof(d->make));
    token = json_object_get(json, tokens, object, "model");
    if (token >= 0)
      json_token_string(json, &tokens[token], d->model, sizeof(d->model));
    int physical = json_object_get(json, tokens, object, "physical_size");
    if (physical >= 0) {
      token = json_object_get(json, tokens, physical, "width");
      d->physical_width_mm =
          token >= 0 ? json_token_int(json, &tokens[token], 0) : 0;
      token = json_object_get(json, tokens, physical, "height");
      d->physical_height_mm =
          token >= 0 ? json_token_int(json, &tokens[token], 0) : 0;
    }
    int position = json_object_get(json, tokens, object, "position");
    if (position >= 0) {
      token = json_object_get(json, tokens, position, "x");
      d->x = token >= 0 ? json_token_int(json, &tokens[token], 0) : 0;
      token = json_object_get(json, tokens, position, "y");
      d->y = token >= 0 ? json_token_int(json, &tokens[token], 0) : 0;
    }
    token = json_object_get(json, tokens, object, "scale");
    double scale =
        token >= 0 ? json_token_double(json, &tokens[token], 1.0) : 1.0;
    if (scale <= 0)
      scale = 1.0;
    int modes = json_object_get(json, tokens, object, "modes");
    if (modes >= 0 && tokens[modes].type == JSMN_ARRAY) {
      int mode = modes + 1;
      for (int m = 0; m < tokens[modes].size; m++) {
        int current = json_object_get(json, tokens, mode, "current");
        if (current >= 0 && json_token_bool(json, &tokens[current], false)) {
          token = json_object_get(json, tokens, mode, "width");
          d->width =
              token >= 0
                  ? (int)lround(json_token_double(json, &tokens[token], 0) /
                                scale)
                  : 0;
          token = json_object_get(json, tokens, mode, "height");
          d->height =
              token >= 0
                  ? (int)lround(json_token_double(json, &tokens[token], 0) /
                                scale)
                  : 0;
          break;
        }
        mode = json_token_skip(tokens, mode);
      }
    }
    token = json_object_get(json, tokens, object, "transform");
    char transform[32] = {0};
    if (token >= 0)
      json_token_string(json, &tokens[token], transform, sizeof(transform));
    if (transform[0] &&
        (!strcmp(transform, "90") || !strcmp(transform, "270") ||
         !strcmp(transform, "flipped-90") ||
         !strcmp(transform, "flipped-270"))) {
      int swap = d->width;
      d->width = d->height;
      d->height = swap;
    }
    d->enabled = d->connector[0] && d->width > 0 && d->height > 0;
    if (d->enabled)
      list->count++;
  }
  free(tokens);
  if (!list->count) {
    snprintf(
        error, error_size,
        "compositor reported no active displays through wlr output management");
    return -1;
  }
  backend_sort_displays(list);
  return 0;
invalid:
  free(tokens);
  snprintf(error, error_size, "wlr-randr returned invalid output JSON");
  return -1;
}
static int wlr_load(DisplayBackend *backend, DisplayList *list, char *error,
                    size_t error_size) {
  (void)backend;
  char *json = NULL;
  char *argv[] = {"wlr-randr", "--json", NULL};
  if (backend_capture(argv, &json, error, error_size) != 0)
    return -1;
  int result = wlr_parse_outputs(json, list, error, error_size);
  free(json);
  return result;
}
static int wlr_apply(DisplayBackend *backend, const DisplayList *list,
                     char *error, size_t error_size) {
  (void)backend;
  char positions[DISPLAY_LAYOUT_MAX_DISPLAYS][64];
  char *argv[2 + DISPLAY_LAYOUT_MAX_DISPLAYS * 4 + 1];
  size_t a = 0;
  argv[a++] = "wlr-randr";
  for (size_t i = 0; i < list->count; i++) {
    snprintf(positions[i], sizeof(positions[i]), "%d,%d", list->displays[i].x,
             list->displays[i].y);
    argv[a++] = "--output";
    argv[a++] = (char *)list->displays[i].connector;
    argv[a++] = "--pos";
    argv[a++] = positions[i];
  }
  argv[a] = NULL;
  return backend_run(argv, "wlroots compositor", error, error_size);
}
static const DisplayBackendOps OPS = {.name = "wlr",
                                      .load = wlr_load,
                                      .apply = wlr_apply,
                                      .identify = backend_identify,
                                      .destroy = backend_noop_destroy};
int backend_wlr_open(DisplayBackend *backend, char *error, size_t error_size) {
  (void)error;
  (void)error_size;
  backend->ops = &OPS;
  backend->state = NULL;
  return 0;
}
