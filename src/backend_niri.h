#ifndef DISPLAY_LAYOUT_BACKEND_NIRI_H
#define DISPLAY_LAYOUT_BACKEND_NIRI_H

#include "model.h"

#include <stddef.h>

int niri_parse_outputs(const char *json, DisplayList *list, char *error,
                       size_t error_size);

#endif
