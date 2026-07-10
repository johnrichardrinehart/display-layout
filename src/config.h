#ifndef DISPLAY_LAYOUT_CONFIG_H
#define DISPLAY_LAYOUT_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
  float value;
  bool percent;
} WindowDimension;

typedef enum {
  THEME_DARK,
  THEME_LIGHT,
  THEME_SYSTEM,
} ThemeMode;

typedef struct {
  WindowDimension width;
  WindowDimension height;
  int font_size;
  ThemeMode theme;
  int snap_distance;
  int identify_duration_ms;
  char font_path[512];
  char backend[32];
} AppConfig;

void config_defaults(AppConfig *config);
int config_load(AppConfig *config, const char *path, char *error,
                size_t error_size);
int config_parse_dimension(const char *text, WindowDimension *dimension);
const char *config_default_path(char *buffer, size_t size);

#endif
