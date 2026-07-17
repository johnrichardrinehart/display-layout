#include "backend_niri.h"
#include "backend_common.h"

#include <stdio.h>
#include <stdlib.h>

int niri_parse_outputs(const char *json, DisplayList *list, char *error,
                       size_t error_size) {
  jsmntok_t *tokens = NULL;
  if (json_parse(json, &tokens, error, error_size, "niri") < 0)
    return -1;
  if (tokens[0].type != JSMN_OBJECT)
    goto invalid;

  *list = (DisplayList){0};
  int cursor = 1;
  for (int item = 0;
       item < tokens[0].size && list->count < DISPLAY_LAYOUT_MAX_DISPLAYS;
       item++) {
    int connector_token = cursor;
    int output_token = json_token_skip(tokens, connector_token);
    cursor = json_token_skip(tokens, output_token);
    if (tokens[connector_token].type != JSMN_STRING ||
        tokens[output_token].type != JSMN_OBJECT)
      continue;

    int logical_token = json_object_get(json, tokens, output_token, "logical");
    if (logical_token < 0 || tokens[logical_token].type != JSMN_OBJECT)
      continue;

    LayoutDisplay *display = &list->displays[list->count];
    json_token_string(json, &tokens[connector_token], display->connector,
                      sizeof(display->connector));
    int value = json_object_get(json, tokens, output_token, "name");
    if (value >= 0)
      json_token_string(json, &tokens[value], display->connector,
                        sizeof(display->connector));
    value = json_object_get(json, tokens, output_token, "make");
    if (value >= 0)
      json_token_string(json, &tokens[value], display->make,
                        sizeof(display->make));
    value = json_object_get(json, tokens, output_token, "model");
    if (value >= 0)
      json_token_string(json, &tokens[value], display->model,
                        sizeof(display->model));

    int physical = json_object_get(json, tokens, output_token, "physical_size");
    if (physical >= 0 && tokens[physical].type == JSMN_ARRAY &&
        tokens[physical].size >= 2) {
      display->physical_width_mm =
          json_token_int(json, &tokens[physical + 1], 0);
      int second = json_token_skip(tokens, physical + 1);
      display->physical_height_mm = json_token_int(json, &tokens[second], 0);
    }

    int token = json_object_get(json, tokens, logical_token, "x");
    display->x = token >= 0 ? json_token_int(json, &tokens[token], 0) : 0;
    token = json_object_get(json, tokens, logical_token, "y");
    display->y = token >= 0 ? json_token_int(json, &tokens[token], 0) : 0;
    token = json_object_get(json, tokens, logical_token, "width");
    display->width = token >= 0 ? json_token_int(json, &tokens[token], 0) : 0;
    token = json_object_get(json, tokens, logical_token, "height");
    display->height = token >= 0 ? json_token_int(json, &tokens[token], 0) : 0;
    display->enabled = display->connector[0] != '\0' && display->width > 0 &&
                       display->height > 0;
    if (display->enabled)
      list->count++;
  }

  free(tokens);
  if (list->count == 0) {
    snprintf(error, error_size, "niri reported no active displays");
    return -1;
  }
  backend_sort_displays(list);
  return 0;

invalid:
  free(tokens);
  snprintf(error, error_size, "niri returned invalid output JSON");
  return -1;
}

static int niri_load(DisplayBackend *backend, DisplayList *list, char *error,
                     size_t error_size) {
  (void)backend;
  char *argv[] = {"niri", "msg", "--json", "outputs", NULL};
  char *json = NULL;
  if (backend_capture(argv, &json, error, error_size) != 0)
    return -1;
  int result = niri_parse_outputs(json, list, error, error_size);
  free(json);
  return result;
}

static int niri_apply(DisplayBackend *backend, const DisplayList *list,
                      char *error, size_t error_size) {
  (void)backend;
  for (size_t index = 0; index < list->count; index++) {
    const LayoutDisplay *display = &list->displays[index];
    char x[32];
    char y[32];
    snprintf(x, sizeof(x), "%d", display->x);
    snprintf(y, sizeof(y), "%d", display->y);
    char *argv[] = {"niri",     "msg", "output", (char *)display->connector,
                    "position", "set", "--",     x,
                    y,          NULL};
    if (backend_run(argv, "niri", error, error_size) != 0)
      return -1;
  }
  return 0;
}

static const DisplayBackendOps NIRI_OPS = {
    .name = "niri",
    .load = niri_load,
    .apply = niri_apply,
    .identify = backend_identify,
    .destroy = backend_noop_destroy,
};

int backend_niri_open(DisplayBackend *backend, char *error, size_t error_size) {
  (void)error;
  (void)error_size;
  backend->ops = &NIRI_OPS;
  backend->state = NULL;
  return 0;
}
