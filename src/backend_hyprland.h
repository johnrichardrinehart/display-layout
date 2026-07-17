#ifndef DISPLAY_LAYOUT_BACKEND_HYPRLAND_H
#define DISPLAY_LAYOUT_BACKEND_HYPRLAND_H
#include "backend.h"
int hyprland_parse_monitors(const char *json, DisplayList *list, char *error,
                            size_t error_size);
int backend_hyprland_open(DisplayBackend *backend, char *error,
                          size_t error_size);
#endif
