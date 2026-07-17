#ifndef DISPLAY_LAYOUT_BACKEND_GNOME_H
#define DISPLAY_LAYOUT_BACKEND_GNOME_H
#include "backend.h"
int gnome_parse_outputs(const char *text, DisplayList *list, char *error,
                        size_t error_size);
int backend_gnome_open(DisplayBackend *backend, char *error, size_t error_size);
#endif
