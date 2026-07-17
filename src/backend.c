#include "backend.h"
#include "backend_gnome.h"
#include "backend_hyprland.h"
#include "backend_kscreen.h"
#include "backend_sway.h"
#include "backend_wlr.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int backend_niri_open(DisplayBackend *backend, char *error, size_t error_size);

static int contains_case_insensitive(const char *text, const char *needle) {
  if (text == NULL || needle == NULL) {
    return 0;
  }
  size_t length = strlen(needle);
  for (; *text != '\0'; text++) {
    size_t i = 0;
    while (i < length && text[i] != '\0' &&
           tolower((unsigned char)text[i]) ==
               tolower((unsigned char)needle[i])) {
      i++;
    }
    if (i == length) {
      return 1;
    }
  }
  return 0;
}

static const char *detect_backend(void) {
  const char *desktop = getenv("XDG_CURRENT_DESKTOP");
  if (getenv("NIRI_SOCKET") != NULL ||
      contains_case_insensitive(desktop, "niri"))
    return "niri";
  if (getenv("HYPRLAND_INSTANCE_SIGNATURE") != NULL ||
      contains_case_insensitive(desktop, "hyprland"))
    return "hyprland";
  if (getenv("SWAYSOCK") != NULL || contains_case_insensitive(desktop, "sway"))
    return "sway";
  if (contains_case_insensitive(desktop, "gnome"))
    return "gnome";
  if (contains_case_insensitive(desktop, "kde") ||
      contains_case_insensitive(desktop, "plasma"))
    return "kscreen";
  return "wlr";
}

int backend_open(const char *name, DisplayBackend *backend, char *error,
                 size_t error_size) {
  memset(backend, 0, sizeof(*backend));
  if (name == NULL || strcmp(name, "auto") == 0)
    name = detect_backend();
  if (strcmp(name, "niri") == 0)
    return backend_niri_open(backend, error, error_size);
  if (strcmp(name, "sway") == 0)
    return backend_sway_open(backend, error, error_size);
  if (strcmp(name, "hyprland") == 0 || strcmp(name, "hypr") == 0)
    return backend_hyprland_open(backend, error, error_size);
  if (strcmp(name, "gnome") == 0 || strcmp(name, "mutter") == 0)
    return backend_gnome_open(backend, error, error_size);
  if (strcmp(name, "kscreen") == 0 || strcmp(name, "kde") == 0 ||
      strcmp(name, "plasma") == 0)
    return backend_kscreen_open(backend, error, error_size);
  if (strcmp(name, "wlr") == 0 || strcmp(name, "wlroots") == 0 ||
      strcmp(name, "river") == 0 || strcmp(name, "wayfire") == 0 ||
      strcmp(name, "labwc") == 0)
    return backend_wlr_open(backend, error, error_size);
  snprintf(error, error_size, "unknown display backend: %s", name);
  return -1;
}

void backend_close(DisplayBackend *backend) {
  if (backend != NULL && backend->ops != NULL &&
      backend->ops->destroy != NULL) {
    backend->ops->destroy(backend);
  }
  if (backend != NULL) {
    memset(backend, 0, sizeof(*backend));
  }
}
