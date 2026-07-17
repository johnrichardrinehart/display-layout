#ifndef DISPLAY_LAYOUT_BACKEND_SWAY_H
#define DISPLAY_LAYOUT_BACKEND_SWAY_H
#include "backend.h"
int sway_parse_outputs(const char *json, DisplayList *list, char *error,
                       size_t error_size);
int backend_sway_open(DisplayBackend *backend, char *error, size_t error_size);
#endif
