#define _POSIX_C_SOURCE 200809L

#include "backend_niri.h"

#include "backend.h"
#include "identifier_wayland.h"
#include "third_party/jsmn.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define JSON_TOKEN_LIMIT 4096
#define OUTPUT_JSON_LIMIT (4U * 1024U * 1024U)

static bool token_equals(const char *json, const jsmntok_t *token,
                         const char *text) {
  size_t length = (size_t)(token->end - token->start);
  return token->type == JSMN_STRING && strlen(text) == length &&
         strncmp(json + token->start, text, length) == 0;
}

static int token_skip(const jsmntok_t *tokens, int index) {
  int cursor = index + 1;
  if (tokens[index].type == JSMN_OBJECT) {
    for (int item = 0; item < tokens[index].size; item++) {
      cursor = token_skip(tokens, cursor);
      cursor = token_skip(tokens, cursor);
    }
  } else if (tokens[index].type == JSMN_ARRAY) {
    for (int item = 0; item < tokens[index].size; item++) {
      cursor = token_skip(tokens, cursor);
    }
  }
  return cursor;
}

static int object_get(const char *json, const jsmntok_t *tokens, int object,
                      const char *key) {
  if (object < 0 || tokens[object].type != JSMN_OBJECT) {
    return -1;
  }
  int cursor = object + 1;
  for (int item = 0; item < tokens[object].size; item++) {
    int key_token = cursor;
    int value_token = token_skip(tokens, key_token);
    if (token_equals(json, &tokens[key_token], key)) {
      return value_token;
    }
    cursor = token_skip(tokens, value_token);
  }
  return -1;
}

static void token_string(const char *json, const jsmntok_t *token, char *output,
                         size_t size) {
  if (token == NULL || token->type != JSMN_STRING || size == 0) {
    if (size > 0) {
      output[0] = '\0';
    }
    return;
  }
  size_t length = (size_t)(token->end - token->start);
  if (length >= size) {
    length = size - 1;
  }
  memcpy(output, json + token->start, length);
  output[length] = '\0';
}

static int token_integer(const char *json, const jsmntok_t *token,
                         int fallback) {
  if (token == NULL || token->type != JSMN_PRIMITIVE) {
    return fallback;
  }
  char buffer[32];
  size_t length = (size_t)(token->end - token->start);
  if (length == 0 || length >= sizeof(buffer)) {
    return fallback;
  }
  memcpy(buffer, json + token->start, length);
  buffer[length] = '\0';
  char *end = NULL;
  long value = strtol(buffer, &end, 10);
  return end != buffer && *end == '\0' ? (int)value : fallback;
}

static int compare_display_positions(const void *left, const void *right) {
  const LayoutDisplay *a = left;
  const LayoutDisplay *b = right;
  if (a->x != b->x) {
    return a->x < b->x ? -1 : 1;
  }
  if (a->y != b->y) {
    return a->y < b->y ? -1 : 1;
  }
  return strcmp(a->connector, b->connector);
}

int niri_parse_outputs(const char *json, DisplayList *list, char *error,
                       size_t error_size) {
  jsmn_parser parser;
  jsmntok_t *tokens = calloc(JSON_TOKEN_LIMIT, sizeof(*tokens));
  if (tokens == NULL) {
    snprintf(error, error_size, "out of memory while parsing niri output data");
    return -1;
  }
  jsmn_init(&parser);
  int token_count =
      jsmn_parse(&parser, json, strlen(json), tokens, JSON_TOKEN_LIMIT);
  if (token_count < 1 || tokens[0].type != JSMN_OBJECT) {
    snprintf(error, error_size, "niri returned invalid output JSON");
    free(tokens);
    return -1;
  }

  *list = (DisplayList){0};
  int cursor = 1;
  for (int item = 0;
       item < tokens[0].size && list->count < DISPLAY_LAYOUT_MAX_DISPLAYS;
       item++) {
    int connector_token = cursor;
    int output_token = token_skip(tokens, connector_token);
    cursor = token_skip(tokens, output_token);
    if (tokens[connector_token].type != JSMN_STRING ||
        tokens[output_token].type != JSMN_OBJECT) {
      continue;
    }

    int logical_token = object_get(json, tokens, output_token, "logical");
    if (logical_token < 0 || tokens[logical_token].type != JSMN_OBJECT) {
      continue;
    }

    LayoutDisplay *display = &list->displays[list->count];
    token_string(json, &tokens[connector_token], display->connector,
                 sizeof(display->connector));
    int value = object_get(json, tokens, output_token, "name");
    if (value >= 0) {
      token_string(json, &tokens[value], display->connector,
                   sizeof(display->connector));
    }
    value = object_get(json, tokens, output_token, "make");
    if (value >= 0) {
      token_string(json, &tokens[value], display->make, sizeof(display->make));
    }
    value = object_get(json, tokens, output_token, "model");
    if (value >= 0) {
      token_string(json, &tokens[value], display->model,
                   sizeof(display->model));
    }

    int physical = object_get(json, tokens, output_token, "physical_size");
    if (physical >= 0 && tokens[physical].type == JSMN_ARRAY &&
        tokens[physical].size >= 2) {
      display->physical_width_mm =
          token_integer(json, &tokens[physical + 1], 0);
      int second = token_skip(tokens, physical + 1);
      display->physical_height_mm = token_integer(json, &tokens[second], 0);
    }

    int token = object_get(json, tokens, logical_token, "x");
    display->x = token >= 0 ? token_integer(json, &tokens[token], 0) : 0;
    token = object_get(json, tokens, logical_token, "y");
    display->y = token >= 0 ? token_integer(json, &tokens[token], 0) : 0;
    token = object_get(json, tokens, logical_token, "width");
    display->width = token >= 0 ? token_integer(json, &tokens[token], 0) : 0;
    token = object_get(json, tokens, logical_token, "height");
    display->height = token >= 0 ? token_integer(json, &tokens[token], 0) : 0;
    display->enabled = display->width > 0 && display->height > 0;
    if (display->enabled) {
      list->count++;
    }
  }

  free(tokens);
  if (list->count == 0) {
    snprintf(error, error_size, "niri reported no active displays");
    return -1;
  }
  qsort(list->displays, list->count, sizeof(list->displays[0]),
        compare_display_positions);
  return 0;
}

static int capture_command(char *const argv[], char **output, char *error,
                           size_t error_size) {
  int descriptors[2];
  if (pipe(descriptors) != 0) {
    snprintf(error, error_size, "cannot create backend pipe: %s",
             strerror(errno));
    return -1;
  }
  pid_t child = fork();
  if (child < 0) {
    close(descriptors[0]);
    close(descriptors[1]);
    snprintf(error, error_size, "cannot start backend: %s", strerror(errno));
    return -1;
  }
  if (child == 0) {
    close(descriptors[0]);
    dup2(descriptors[1], STDOUT_FILENO);
    close(descriptors[1]);
    execvp(argv[0], argv);
    _exit(127);
  }

  close(descriptors[1]);
  char *buffer = malloc(OUTPUT_JSON_LIMIT + 1U);
  if (buffer == NULL) {
    close(descriptors[0]);
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    snprintf(error, error_size, "out of memory while reading backend output");
    return -1;
  }
  size_t used = 0;
  ssize_t amount;
  while (used < OUTPUT_JSON_LIMIT &&
         (amount = read(descriptors[0], buffer + used,
                        OUTPUT_JSON_LIMIT - used)) > 0) {
    used += (size_t)amount;
  }
  close(descriptors[0]);
  int status = 0;
  waitpid(child, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    free(buffer);
    snprintf(error, error_size, "niri IPC command failed");
    return -1;
  }
  buffer[used] = '\0';
  *output = buffer;
  return 0;
}

static int run_command(char *const argv[], char *error, size_t error_size) {
  pid_t child = fork();
  if (child < 0) {
    snprintf(error, error_size, "cannot start backend: %s", strerror(errno));
    return -1;
  }
  if (child == 0) {
    execvp(argv[0], argv);
    _exit(127);
  }
  int status = 0;
  waitpid(child, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    snprintf(error, error_size, "niri rejected the display layout");
    return -1;
  }
  return 0;
}

static int niri_load(DisplayBackend *backend, DisplayList *list, char *error,
                     size_t error_size) {
  (void)backend;
  char *argv[] = {"niri", "msg", "--json", "outputs", NULL};
  char *json = NULL;
  if (capture_command(argv, &json, error, error_size) != 0) {
    return -1;
  }
  int result = niri_parse_outputs(json, list, error, error_size);
  free(json);
  return result;
}

static int niri_apply(DisplayBackend *backend, const DisplayList *list,
                      char *error, size_t error_size) {
  (void)backend;
  for (size_t index = 0; index < list->count; index++) {
    const LayoutDisplay *display = &list->displays[index];
    char x[32];
    char y[32];
    snprintf(x, sizeof(x), "%d", display->x);
    snprintf(y, sizeof(y), "%d", display->y);
    char *argv[] = {"niri",     "msg", "output", (char *)display->connector,
                    "position", "set", "--",     x,
                    y,          NULL};
    if (run_command(argv, error, error_size) != 0) {
      return -1;
    }
  }
  return 0;
}

static int niri_identify(DisplayBackend *backend, const DisplayList *list,
                         unsigned int duration_ms, char *error,
                         size_t error_size) {
  (void)backend;
  const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
  if (runtime_dir == NULL || runtime_dir[0] == '\0') {
    runtime_dir = "/tmp";
  }
  char lock_path[4096];
  snprintf(lock_path, sizeof(lock_path), "%s/display-layout-identify-%u.lock",
           runtime_dir, (unsigned int)getuid());
  int lock_fd = open(lock_path, O_CREAT | O_CLOEXEC | O_RDWR, 0600);
  if (lock_fd < 0) {
    snprintf(error, error_size, "cannot open identifier lock: %s",
             strerror(errno));
    return -1;
  }
  if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
    close(lock_fd);
    return 0;
  }

  pid_t launcher = fork();
  if (launcher < 0) {
    snprintf(error, error_size, "cannot launch display identifiers: %s",
             strerror(errno));
    close(lock_fd);
    return -1;
  }
  if (launcher == 0) {
    pid_t worker = fork();
    if (worker < 0) {
      _exit(1);
    }
    if (worker > 0) {
      _exit(0);
    }
    setsid();
    char identify_error[256];
    int result = identifier_wayland_show(list, duration_ms, identify_error,
                                         sizeof(identify_error));
    if (result != 0) {
      fprintf(stderr, "display-layout: %s\n", identify_error);
    }
    close(lock_fd);
    _exit(result == 0 ? 0 : 1);
  }
  waitpid(launcher, NULL, 0);
  close(lock_fd);
  return 0;
}

static void niri_destroy(DisplayBackend *backend) { (void)backend; }

static const DisplayBackendOps NIRI_OPS = {
    .name = "niri",
    .load = niri_load,
    .apply = niri_apply,
    .identify = niri_identify,
    .destroy = niri_destroy,
};

int backend_niri_open(DisplayBackend *backend, char *error, size_t error_size) {
  (void)error;
  (void)error_size;
  backend->ops = &NIRI_OPS;
  backend->state = NULL;
  return 0;
}
