#ifndef DISPLAY_LAYOUT_BACKEND_WLR_H
#define DISPLAY_LAYOUT_BACKEND_WLR_H
#include "backend.h"
int wlr_parse_outputs(const char *json, DisplayList *list, char *error,
                      size_t error_size);
int backend_wlr_open(DisplayBackend *backend, char *error, size_t error_size);
#endif
