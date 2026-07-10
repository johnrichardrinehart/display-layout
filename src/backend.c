#include "backend.h"

#include <stdio.h>
#include <string.h>

int backend_niri_open(DisplayBackend *backend, char *error, size_t error_size);

int backend_open(const char *name, DisplayBackend *backend, char *error,
                 size_t error_size) {
  memset(backend, 0, sizeof(*backend));
  if (name == NULL || strcmp(name, "auto") == 0 || strcmp(name, "niri") == 0) {
    return backend_niri_open(backend, error, error_size);
  }
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
