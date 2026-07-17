#define _POSIX_C_SOURCE 200809L

#include "backend_common.h"

#include "identifier_wayland.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define COMMAND_OUTPUT_LIMIT (4U * 1024U * 1024U)

int backend_capture(char *const argv[], char **output, char *error,
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
  char *buffer = malloc(COMMAND_OUTPUT_LIMIT + 1U);
  if (buffer == NULL) {
    close(descriptors[0]);
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    snprintf(error, error_size, "out of memory while reading backend output");
    return -1;
  }
  size_t used = 0;
  ssize_t amount;
  while (used < COMMAND_OUTPUT_LIMIT &&
         (amount = read(descriptors[0], buffer + used,
                        COMMAND_OUTPUT_LIMIT - used)) > 0) {
    used += (size_t)amount;
  }
  close(descriptors[0]);
  int status = 0;
  waitpid(child, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    free(buffer);
    snprintf(error, error_size, "%s command failed", argv[0]);
    return -1;
  }
  buffer[used] = '\0';
  *output = buffer;
  return 0;
}

int backend_run(char *const argv[], const char *backend_name, char *error,
                size_t error_size) {
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
    snprintf(error, error_size, "%s rejected the display layout", backend_name);
    return -1;
  }
  return 0;
}

int backend_identify(DisplayBackend *backend, const DisplayList *list,
                     unsigned int duration_ms, char *error, size_t error_size) {
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

void backend_noop_destroy(DisplayBackend *backend) { (void)backend; }

static int compare_positions(const void *left, const void *right) {
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

void backend_sort_displays(DisplayList *list) {
  qsort(list->displays, list->count, sizeof(list->displays[0]),
        compare_positions);
}

bool json_token_equals(const char *json, const jsmntok_t *token,
                       const char *text) {
  size_t length = (size_t)(token->end - token->start);
  return token->type == JSMN_STRING && strlen(text) == length &&
         strncmp(json + token->start, text, length) == 0;
}

int json_token_skip(const jsmntok_t *tokens, int index) {
  int cursor = index + 1;
  if (tokens[index].type == JSMN_OBJECT) {
    for (int item = 0; item < tokens[index].size; item++) {
      cursor = json_token_skip(tokens, cursor);
      cursor = json_token_skip(tokens, cursor);
    }
  } else if (tokens[index].type == JSMN_ARRAY) {
    for (int item = 0; item < tokens[index].size; item++) {
      cursor = json_token_skip(tokens, cursor);
    }
  }
  return cursor;
}

int json_object_get(const char *json, const jsmntok_t *tokens, int object,
                    const char *key) {
  if (object < 0 || tokens[object].type != JSMN_OBJECT) {
    return -1;
  }
  int cursor = object + 1;
  for (int item = 0; item < tokens[object].size; item++) {
    int key_token = cursor;
    int value_token = json_token_skip(tokens, key_token);
    if (json_token_equals(json, &tokens[key_token], key)) {
      return value_token;
    }
    cursor = json_token_skip(tokens, value_token);
  }
  return -1;
}

void json_token_string(const char *json, const jsmntok_t *token, char *output,
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

static double token_number(const char *json, const jsmntok_t *token,
                           double fallback) {
  if (token == NULL || token->type != JSMN_PRIMITIVE) {
    return fallback;
  }
  char buffer[64];
  size_t length = (size_t)(token->end - token->start);
  if (length == 0 || length >= sizeof(buffer)) {
    return fallback;
  }
  memcpy(buffer, json + token->start, length);
  buffer[length] = '\0';
  char *end = NULL;
  double value = strtod(buffer, &end);
  return end != buffer && *end == '\0' ? value : fallback;
}

int json_token_int(const char *json, const jsmntok_t *token, int fallback) {
  return (int)token_number(json, token, fallback);
}

double json_token_double(const char *json, const jsmntok_t *token,
                         double fallback) {
  return token_number(json, token, fallback);
}

bool json_token_bool(const char *json, const jsmntok_t *token, bool fallback) {
  if (token == NULL || token->type != JSMN_PRIMITIVE) {
    return fallback;
  }
  if ((token->end - token->start) == 4 &&
      strncmp(json + token->start, "true", 4) == 0) {
    return true;
  }
  if ((token->end - token->start) == 5 &&
      strncmp(json + token->start, "false", 5) == 0) {
    return false;
  }
  return fallback;
}

int json_parse(const char *json, jsmntok_t **tokens, char *error,
               size_t error_size, const char *backend_name) {
  *tokens = calloc(BACKEND_JSON_TOKENS, sizeof(**tokens));
  if (*tokens == NULL) {
    snprintf(error, error_size, "out of memory while parsing %s output",
             backend_name);
    return -1;
  }
  jsmn_parser parser;
  jsmn_init(&parser);
  int count =
      jsmn_parse(&parser, json, strlen(json), *tokens, BACKEND_JSON_TOKENS);
  if (count < 1) {
    snprintf(error, error_size, "%s returned invalid JSON", backend_name);
    free(*tokens);
    *tokens = NULL;
    return -1;
  }
  return count;
}
