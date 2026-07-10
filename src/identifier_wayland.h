#ifndef DISPLAY_LAYOUT_IDENTIFIER_WAYLAND_H
#define DISPLAY_LAYOUT_IDENTIFIER_WAYLAND_H

#include "model.h"

#include <stddef.h>

int identifier_wayland_show(const DisplayList *list, unsigned int duration_ms,
                            char *error, size_t error_size);

#endif
