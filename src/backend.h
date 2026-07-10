#ifndef DISPLAY_LAYOUT_BACKEND_H
#define DISPLAY_LAYOUT_BACKEND_H

#include "model.h"

#include <stddef.h>

typedef struct DisplayBackend DisplayBackend;

typedef struct {
  const char *name;
  int (*load)(DisplayBackend *backend, DisplayList *list, char *error,
              size_t error_size);
  int (*apply)(DisplayBackend *backend, const DisplayList *list, char *error,
               size_t error_size);
  int (*identify)(DisplayBackend *backend, const DisplayList *list,
                  unsigned int duration_ms, char *error, size_t error_size);
  void (*destroy)(DisplayBackend *backend);
} DisplayBackendOps;

struct DisplayBackend {
  const DisplayBackendOps *ops;
  void *state;
};

int backend_open(const char *name, DisplayBackend *backend, char *error,
                 size_t error_size);
void backend_close(DisplayBackend *backend);

#endif
