#include "backend_sway.h"
#include "backend_common.h"

#include <stdio.h>
#include <stdlib.h>

int sway_parse_outputs(const char *json, DisplayList *list, char *error,
                       size_t error_size) {
  jsmntok_t *tokens = NULL;
  if (json_parse(json, &tokens, error, error_size, "sway") < 0)
    return -1;
  *list = (DisplayList){0};
  if (tokens[0].type != JSMN_ARRAY)
    goto invalid;
  int cursor = 1;
  for (int i = 0;
       i < tokens[0].size && list->count < DISPLAY_LAYOUT_MAX_DISPLAYS; i++) {
    int object = cursor;
    cursor = json_token_skip(tokens, cursor);
    int active = json_object_get(json, tokens, object, "active");
    if (active < 0 || !json_token_bool(json, &tokens[active], false))
      continue;
    LayoutDisplay *d = &list->displays[list->count];
    int token = json_object_get(json, tokens, object, "name");
    if (token >= 0)
      json_token_string(json, &tokens[token], d->connector,
                        sizeof(d->connector));
    token = json_object_get(json, tokens, object, "make");
    if (token >= 0)
      json_token_string(json, &tokens[token], d->make, sizeof(d->make));
    token = json_object_get(json, tokens, object, "model");
    if (token >= 0)
      json_token_string(json, &tokens[token], d->model, sizeof(d->model));
    int rect = json_object_get(json, tokens, object, "rect");
    if (rect < 0)
      continue;
    token = json_object_get(json, tokens, rect, "x");
    d->x = token >= 0 ? json_token_int(json, &tokens[token], 0) : 0;
    token = json_object_get(json, tokens, rect, "y");
    d->y = token >= 0 ? json_token_int(json, &tokens[token], 0) : 0;
    token = json_object_get(json, tokens, rect, "width");
    d->width = token >= 0 ? json_token_int(json, &tokens[token], 0) : 0;
    token = json_object_get(json, tokens, rect, "height");
    d->height = token >= 0 ? json_token_int(json, &tokens[token], 0) : 0;
    token = json_object_get(json, tokens, object, "physical_width");
    d->physical_width_mm =
        token >= 0 ? json_token_int(json, &tokens[token], 0) : 0;
    token = json_object_get(json, tokens, object, "physical_height");
    d->physical_height_mm =
        token >= 0 ? json_token_int(json, &tokens[token], 0) : 0;
    d->enabled = d->width > 0 && d->height > 0;
    if (d->enabled)
      list->count++;
  }
  free(tokens);
  if (list->count == 0) {
    snprintf(error, error_size, "sway reported no active displays");
    return -1;
  }
  backend_sort_displays(list);
  return 0;
invalid:
  free(tokens);
  snprintf(error, error_size, "sway returned invalid output JSON");
  return -1;
}

static int sway_load(DisplayBackend *backend, DisplayList *list, char *error,
                     size_t error_size) {
  (void)backend;
  char *json = NULL;
  char *argv[] = {"swaymsg", "-r", "-t", "get_outputs", NULL};
  if (backend_capture(argv, &json, error, error_size) != 0)
    return -1;
  int result = sway_parse_outputs(json, list, error, error_size);
  free(json);
  return result;
}

static int sway_apply(DisplayBackend *backend, const DisplayList *list,
                      char *error, size_t error_size) {
  (void)backend;
  for (size_t i = 0; i < list->count; i++) {
    char position[64];
    snprintf(position, sizeof(position), "%d %d", list->displays[i].x,
             list->displays[i].y);
    char *argv[] = {"swaymsg", "output", (char *)list->displays[i].connector,
                    "pos",     position, NULL};
    if (backend_run(argv, "sway", error, error_size) != 0)
      return -1;
  }
  return 0;
}

static const DisplayBackendOps OPS = {.name = "sway",
                                      .load = sway_load,
                                      .apply = sway_apply,
                                      .identify = backend_identify,
                                      .destroy = backend_noop_destroy};
int backend_sway_open(DisplayBackend *backend, char *error, size_t error_size) {
  (void)error;
  (void)error_size;
  backend->ops = &OPS;
  backend->state = NULL;
  return 0;
}
