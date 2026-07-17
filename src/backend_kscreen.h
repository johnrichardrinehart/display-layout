#ifndef DISPLAY_LAYOUT_BACKEND_KSCREEN_H
#define DISPLAY_LAYOUT_BACKEND_KSCREEN_H
#include "backend.h"
int kscreen_parse_outputs(const char *json, DisplayList *list, char *error,
                          size_t error_size);
int backend_kscreen_open(DisplayBackend *backend, char *error,
                         size_t error_size);
#endif
