#include "backend_hyprland.h"
#include "backend_common.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int hyprland_parse_monitors(const char *json, DisplayList *list, char *error,
                            size_t error_size) {
  jsmntok_t *tokens = NULL;
  if (json_parse(json, &tokens, error, error_size, "Hyprland") < 0)
    return -1;
  *list = (DisplayList){0};
  if (tokens[0].type != JSMN_ARRAY)
    goto invalid;
  int cursor = 1;
  for (int i = 0;
       i < tokens[0].size && list->count < DISPLAY_LAYOUT_MAX_DISPLAYS; i++) {
    int object = cursor;
    cursor = json_token_skip(tokens, cursor);
    int token = json_object_get(json, tokens, object, "disabled");
    if (token >= 0 && json_token_bool(json, &tokens[token], false))
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
    token = json_object_get(json, tokens, object, "x");
    d->x = token >= 0 ? json_token_int(json, &tokens[token], 0) : 0;
    token = json_object_get(json, tokens, object, "y");
    d->y = token >= 0 ? json_token_int(json, &tokens[token], 0) : 0;
    int width_token = json_object_get(json, tokens, object, "width");
    int height_token = json_object_get(json, tokens, object, "height");
    d->mode_width =
        width_token >= 0 ? json_token_int(json, &tokens[width_token], 0) : 0;
    d->mode_height =
        height_token >= 0 ? json_token_int(json, &tokens[height_token], 0) : 0;
    token = json_object_get(json, tokens, object, "refreshRate");
    d->refresh_rate =
        token >= 0 ? (float)json_token_double(json, &tokens[token], 0.0) : 0.0f;
    token = json_object_get(json, tokens, object, "scale");
    double scale =
        token >= 0 ? json_token_double(json, &tokens[token], 1.0) : 1.0;
    if (scale <= 0.0)
      scale = 1.0;
    d->scale = (float)scale;
    d->width = (int)lround(d->mode_width / scale);
    d->height = (int)lround(d->mode_height / scale);
    token = json_object_get(json, tokens, object, "transform");
    d->transform = token >= 0 ? json_token_int(json, &tokens[token], 0) : 0;
    if (d->transform == 1 || d->transform == 3 || d->transform == 5 ||
        d->transform == 7) {
      int swap = d->width;
      d->width = d->height;
      d->height = swap;
    }
    token = json_object_get(json, tokens, object, "physicalWidth");
    d->physical_width_mm =
        token >= 0 ? json_token_int(json, &tokens[token], 0) : 0;
    token = json_object_get(json, tokens, object, "physicalHeight");
    d->physical_height_mm =
        token >= 0 ? json_token_int(json, &tokens[token], 0) : 0;
    d->enabled = d->connector[0] != '\0' && d->width > 0 && d->height > 0;
    if (d->enabled)
      list->count++;
  }
  free(tokens);
  if (list->count == 0) {
    snprintf(error, error_size, "Hyprland reported no active displays");
    return -1;
  }
  backend_sort_displays(list);
  return 0;
invalid:
  free(tokens);
  snprintf(error, error_size, "Hyprland returned invalid monitor JSON");
  return -1;
}

static int hyprland_load(DisplayBackend *backend, DisplayList *list,
                         char *error, size_t error_size) {
  (void)backend;
  char *json = NULL;
  char *argv[] = {"hyprctl", "-j", "monitors", NULL};
  if (backend_capture(argv, &json, error, error_size) != 0)
    return -1;
  int result = hyprland_parse_monitors(json, list, error, error_size);
  free(json);
  return result;
}
static int hyprland_apply(DisplayBackend *backend, const DisplayList *list,
                          char *error, size_t error_size) {
  (void)backend;
  for (size_t i = 0; i < list->count; i++) {
    const LayoutDisplay *display = &list->displays[i];
    char rule[512];
    if (display->mode_width > 0 && display->mode_height > 0 &&
        display->refresh_rate > 0.0f && display->scale > 0.0f) {
      snprintf(rule, sizeof(rule), "%s,%dx%d@%.3f,%dx%d,%.3f,transform,%d",
               display->connector, display->mode_width, display->mode_height,
               display->refresh_rate, display->x, display->y, display->scale,
               display->transform);
    } else {
      snprintf(rule, sizeof(rule), "%s,preferred,%dx%d,auto",
               display->connector, display->x, display->y);
    }
    char *argv[] = {"hyprctl", "keyword", "monitor", rule, NULL};
    if (backend_run(argv, "Hyprland", error, error_size) != 0)
      return -1;
  }
  return 0;
}
static const DisplayBackendOps OPS = {.name = "hyprland",
                                      .load = hyprland_load,
                                      .apply = hyprland_apply,
                                      .identify = backend_identify,
                                      .destroy = backend_noop_destroy};
int backend_hyprland_open(DisplayBackend *backend, char *error,
                          size_t error_size) {
  (void)error;
  (void)error_size;
  backend->ops = &OPS;
  backend->state = NULL;
  return 0;
}
