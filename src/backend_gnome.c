#define _POSIX_C_SOURCE 200809L
#include "backend_gnome.h"
#include "backend_common.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_text(char *output, size_t output_size, const char *text) {
  if (output_size == 0)
    return;
  snprintf(output, output_size, "%s", text);
}

static bool connector_on_line(const char *line, const char *connector) {
  const char *match = strstr(line, connector);
  if (match == NULL)
    return false;
  char following = match[strlen(connector)];
  return following == '\0' || following == '(' ||
         isspace((unsigned char)following);
}

static int parse_transform(const char *name) {
  static const char *const names[] = {"normal",      "90",         "180",
                                      "270",         "flipped",    "flipped-90",
                                      "flipped-180", "flipped-270"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    if (strcmp(name, names[i]) == 0)
      return (int)i;
  }
  return 0;
}

static const char *transform_name(int transform) {
  static const char *const names[] = {"normal",      "90",         "180",
                                      "270",         "flipped",    "flipped-90",
                                      "flipped-180", "flipped-270"};
  if (transform < 0 || transform >= (int)(sizeof(names) / sizeof(names[0])))
    return "normal";
  return names[transform];
}

int gnome_parse_outputs(const char *text, DisplayList *list, char *error,
                        size_t error_size) {
  *list = (DisplayList){0};
  DisplayList physical = {0};
  LayoutDisplay *monitor = NULL;
  int x = 0;
  int y = 0;
  double scale = 1.0;
  int transform = 0;
  bool primary = false;
  bool logical_section = false;
  bool awaiting_current_mode = false;

  char *copy = strdup(text);
  if (copy == NULL) {
    snprintf(error, error_size, "out of memory while parsing GNOME displays");
    return -1;
  }

  char *save = NULL;
  for (char *line = strtok_r(copy, "\n", &save); line;
       line = strtok_r(NULL, "\n", &save)) {
    if (strstr(line, "Logical monitors:") != NULL) {
      logical_section = true;
      monitor = NULL;
      continue;
    }

    if (!logical_section) {
      char *field = strstr(line, "Monitor ");
      if (field != NULL && physical.count < DISPLAY_LAYOUT_MAX_DISPLAYS) {
        field += 8;
        char *space = strchr(field, ' ');
        if (space == NULL)
          continue;
        monitor = &physical.displays[physical.count++];
        size_t length = (size_t)(space - field);
        if (length >= sizeof(monitor->connector))
          length = sizeof(monitor->connector) - 1;
        memcpy(monitor->connector, field, length);
        monitor->connector[length] = '\0';
        char *open = strchr(space, '(');
        char *close = open != NULL ? strrchr(open, ')') : NULL;
        if (open != NULL && close != NULL && close > open + 1) {
          length = (size_t)(close - open - 1);
          if (length >= sizeof(monitor->model))
            length = sizeof(monitor->model) - 1;
          memcpy(monitor->model, open + 1, length);
          monitor->model[length] = '\0';
        }
        awaiting_current_mode = false;
        continue;
      }
      field = strstr(line, "Vendor: ");
      if (field != NULL && monitor != NULL) {
        copy_text(monitor->make, sizeof(monitor->make), field + 8);
        continue;
      }
      field = strstr(line, "Product: ");
      if (field != NULL && monitor != NULL) {
        copy_text(monitor->model, sizeof(monitor->model), field + 9);
        continue;
      }
      if (strstr(line, "Current mode") != NULL && monitor != NULL) {
        awaiting_current_mode = true;
        continue;
      }
      if (awaiting_current_mode && monitor != NULL) {
        char *mode = line;
        while (*mode != '\0' && !isdigit((unsigned char)*mode))
          mode++;
        if (sscanf(mode, "%dx%d@%f", &monitor->mode_width,
                   &monitor->mode_height, &monitor->refresh_rate) == 3)
          awaiting_current_mode = false;
      }
      continue;
    }

    char *field = strstr(line, "Position: (");
    if (field != NULL) {
      if (sscanf(field, "Position: (%d, %d)", &x, &y) != 2) {
        x = 0;
        y = 0;
      }
      continue;
    }
    field = strstr(line, "Scale: ");
    if (field != NULL) {
      scale = strtod(field + 7, NULL);
      if (scale <= 0.0)
        scale = 1.0;
      continue;
    }
    field = strstr(line, "Transform: ");
    if (field != NULL) {
      transform = parse_transform(field + 11);
      continue;
    }
    field = strstr(line, "Primary: ");
    if (field != NULL) {
      primary = strcmp(field + 9, "yes") == 0;
      continue;
    }

    for (size_t i = 0;
         i < physical.count && list->count < DISPLAY_LAYOUT_MAX_DISPLAYS; i++) {
      if (!connector_on_line(line, physical.displays[i].connector))
        continue;
      LayoutDisplay *display = &list->displays[list->count];
      *display = physical.displays[i];
      display->x = x;
      display->y = y;
      display->scale = (float)scale;
      display->transform = transform;
      display->primary = primary;
      display->width = (int)lround(display->mode_width / scale);
      display->height = (int)lround(display->mode_height / scale);
      if (transform == 1 || transform == 3 || transform == 5 ||
          transform == 7) {
        int swap = display->width;
        display->width = display->height;
        display->height = swap;
      }
      display->enabled = display->connector[0] != '\0' && display->width > 0 &&
                         display->height > 0;
      if (display->enabled)
        list->count++;
      break;
    }
  }

  free(copy);
  if (list->count == 0) {
    snprintf(error, error_size, "GNOME reported no active displays");
    return -1;
  }
  backend_sort_displays(list);
  return 0;
}

static int gnome_load(DisplayBackend *backend, DisplayList *list, char *error,
                      size_t error_size) {
  (void)backend;
  char *text = NULL;
  char *argv[] = {"gdctl", "show", NULL};
  if (backend_capture(argv, &text, error, error_size) != 0)
    return -1;
  int result = gnome_parse_outputs(text, list, error, error_size);
  free(text);
  return result;
}

static int gnome_apply(DisplayBackend *backend, const DisplayList *list,
                       char *error, size_t error_size) {
  (void)backend;
  char xs[DISPLAY_LAYOUT_MAX_DISPLAYS][32];
  char ys[DISPLAY_LAYOUT_MAX_DISPLAYS][32];
  char scales[DISPLAY_LAYOUT_MAX_DISPLAYS][32];
  char modes[DISPLAY_LAYOUT_MAX_DISPLAYS][64];
  char *argv[2 + DISPLAY_LAYOUT_MAX_DISPLAYS * 14 + 1];
  size_t a = 0;
  argv[a++] = "gdctl";
  argv[a++] = "set";
  for (size_t i = 0; i < list->count; i++) {
    const LayoutDisplay *display = &list->displays[i];
    snprintf(xs[i], sizeof(xs[i]), "%d", display->x);
    snprintf(ys[i], sizeof(ys[i]), "%d", display->y);
    snprintf(scales[i], sizeof(scales[i]), "%.8g", display->scale);
    snprintf(modes[i], sizeof(modes[i]), "%dx%d@%.3f", display->mode_width,
             display->mode_height, display->refresh_rate);
    argv[a++] = "--logical-monitor";
    if (display->primary)
      argv[a++] = "--primary";
    argv[a++] = "--x";
    argv[a++] = xs[i];
    argv[a++] = "--y";
    argv[a++] = ys[i];
    argv[a++] = "--scale";
    argv[a++] = scales[i];
    argv[a++] = "--transform";
    argv[a++] = (char *)transform_name(display->transform);
    argv[a++] = "--monitor";
    argv[a++] = (char *)display->connector;
    argv[a++] = "--mode";
    argv[a++] = modes[i];
  }
  argv[a] = NULL;
  return backend_run(argv, "GNOME", error, error_size);
}

static const DisplayBackendOps OPS = {.name = "gnome",
                                      .load = gnome_load,
                                      .apply = gnome_apply,
                                      .identify = backend_identify,
                                      .destroy = backend_noop_destroy};

int backend_gnome_open(DisplayBackend *backend, char *error,
                       size_t error_size) {
  (void)error;
  (void)error_size;
  backend->ops = &OPS;
  backend->state = NULL;
  return 0;
}
