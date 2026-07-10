#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static char *trim(char *text) {
  while (isspace((unsigned char)*text)) {
    text++;
  }
  char *end = text + strlen(text);
  while (end > text && isspace((unsigned char)end[-1])) {
    *--end = '\0';
  }
  return text;
}

void config_defaults(AppConfig *config) {
  *config = (AppConfig){
      .width = {.value = 1060.0F, .percent = false},
      .height = {.value = 696.0F, .percent = false},
      .font_size = 16,
      .theme = THEME_SYSTEM,
      .snap_distance = 14,
      .identify_duration_ms = 2000,
      .font_path = "",
      .backend = "auto",
  };
}

int config_parse_dimension(const char *text, WindowDimension *dimension) {
  char *end = NULL;
  errno = 0;
  float value = strtof(text, &end);
  if (errno != 0 || end == text || value <= 0.0F) {
    return -1;
  }
  while (isspace((unsigned char)*end)) {
    end++;
  }
  bool percent = false;
  if (strcmp(end, "%") == 0) {
    percent = true;
    if (value > 100.0F) {
      return -1;
    }
  } else if (*end != '\0' && strcasecmp(end, "px") != 0) {
    return -1;
  }
  *dimension = (WindowDimension){.value = value, .percent = percent};
  return 0;
}

static int parse_positive_int(const char *text, int minimum, int maximum,
                              int *result) {
  char *end = NULL;
  errno = 0;
  long value = strtol(text, &end, 10);
  while (end != NULL && isspace((unsigned char)*end)) {
    end++;
  }
  if (errno != 0 || end == text || (end != NULL && *end != '\0') ||
      value < minimum || value > maximum) {
    return -1;
  }
  *result = (int)value;
  return 0;
}

const char *config_default_path(char *buffer, size_t size) {
  const char *config_home = getenv("XDG_CONFIG_HOME");
  if (config_home != NULL && config_home[0] != '\0') {
    snprintf(buffer, size, "%s/display-layout/config.ini", config_home);
  } else {
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
      return NULL;
    }
    snprintf(buffer, size, "%s/.config/display-layout/config.ini", home);
  }
  return buffer;
}

int config_load(AppConfig *config, const char *path, char *error,
                size_t error_size) {
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    if (errno == ENOENT) {
      return 0;
    }
    snprintf(error, error_size, "cannot open %s: %s", path, strerror(errno));
    return -1;
  }

  char line[1024];
  unsigned int line_number = 0;
  while (fgets(line, sizeof(line), file) != NULL) {
    line_number++;
    char *entry = trim(line);
    if (*entry == '\0' || *entry == '#' || *entry == ';' || *entry == '[') {
      continue;
    }
    char *separator = strchr(entry, '=');
    if (separator == NULL) {
      snprintf(error, error_size, "%s:%u: expected key = value", path,
               line_number);
      fclose(file);
      return -1;
    }
    *separator = '\0';
    char *key = trim(entry);
    char *value = trim(separator + 1);

    int invalid = 0;
    if (strcmp(key, "width") == 0) {
      invalid = config_parse_dimension(value, &config->width);
    } else if (strcmp(key, "height") == 0) {
      invalid = config_parse_dimension(value, &config->height);
    } else if (strcmp(key, "font-size") == 0) {
      invalid = parse_positive_int(value, 10, 48, &config->font_size);
    } else if (strcmp(key, "snap-distance") == 0) {
      invalid = parse_positive_int(value, 0, 64, &config->snap_distance);
    } else if (strcmp(key, "identify-duration-ms") == 0) {
      invalid =
          parse_positive_int(value, 500, 30000, &config->identify_duration_ms);
    } else if (strcmp(key, "theme") == 0) {
      if (strcasecmp(value, "dark") == 0) {
        config->theme = THEME_DARK;
      } else if (strcasecmp(value, "light") == 0) {
        config->theme = THEME_LIGHT;
      } else if (strcasecmp(value, "system") == 0) {
        config->theme = THEME_SYSTEM;
      } else {
        invalid = -1;
      }
    } else if (strcmp(key, "font") == 0) {
      snprintf(config->font_path, sizeof(config->font_path), "%s", value);
    } else if (strcmp(key, "backend") == 0) {
      snprintf(config->backend, sizeof(config->backend), "%s", value);
    } else {
      snprintf(error, error_size, "%s:%u: unknown setting '%s'", path,
               line_number, key);
      fclose(file);
      return -1;
    }

    if (invalid != 0) {
      snprintf(error, error_size, "%s:%u: invalid value for '%s'", path,
               line_number, key);
      fclose(file);
      return -1;
    }
  }

  if (ferror(file)) {
    snprintf(error, error_size, "cannot read %s", path);
    fclose(file);
    return -1;
  }
  fclose(file);
  return 0;
}
