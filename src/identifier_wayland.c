#define _POSIX_C_SOURCE 200809L

#include "identifier_wayland.h"

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "third_party/stb_truetype.h"

#define OVERLAY_SIZE 240
#define BUFFER_COUNT 3
#define FONT_SIZE 144
#define FADE_START 0.55

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

typedef struct IdentifierState IdentifierState;

typedef struct {
  struct wl_buffer *buffer;
  uint32_t *pixels;
  bool busy;
} FrameBuffer;

typedef struct {
  IdentifierState *state;
  struct wl_output *output;
  uint32_t registry_name;
  char name[128];
  struct wl_surface *surface;
  struct zwlr_layer_surface_v1 *layer_surface;
  FrameBuffer buffers[BUFFER_COUNT];
  int next_buffer;
  unsigned int number;
  bool configured;
  bool closed;
} IdentifierOutput;

struct IdentifierState {
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_compositor *compositor;
  struct wl_shm *shm;
  struct zwlr_layer_shell_v1 *layer_shell;
  struct wl_shm_pool *pool;
  void *pool_data;
  size_t pool_size;
  IdentifierOutput outputs[DISPLAY_LAYOUT_MAX_DISPLAYS];
  size_t output_count;
  stbtt_fontinfo font;
  unsigned char *font_data;
  char error[256];
  bool failed;
};

static void set_error(IdentifierState *state, const char *message) {
  if (!state->failed) {
    snprintf(state->error, sizeof(state->error), "%s", message);
  }
  state->failed = true;
}

static int anonymous_file(size_t size) {
  const char *runtime = getenv("XDG_RUNTIME_DIR");
  if (runtime == NULL || runtime[0] == '\0') {
    errno = ENOENT;
    return -1;
  }
  char path[4096];
  snprintf(path, sizeof(path), "%s/display-layout-shm-XXXXXX", runtime);
  int fd = mkstemp(path);
  if (fd < 0) {
    return -1;
  }
  unlink(path);
  int flags = fcntl(fd, F_GETFD);
  if (flags >= 0) {
    fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
  }
  if (ftruncate(fd, (off_t)size) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static void buffer_release(void *data, struct wl_buffer *buffer) {
  (void)buffer;
  ((FrameBuffer *)data)->busy = false;
}

static const struct wl_buffer_listener BUFFER_LISTENER = {
    .release = buffer_release,
};

static void output_geometry(void *data, struct wl_output *output, int32_t x,
                            int32_t y, int32_t physical_width,
                            int32_t physical_height, int32_t subpixel,
                            const char *make, const char *model,
                            int32_t transform) {
  (void)data;
  (void)output;
  (void)x;
  (void)y;
  (void)physical_width;
  (void)physical_height;
  (void)subpixel;
  (void)make;
  (void)model;
  (void)transform;
}

static void output_mode(void *data, struct wl_output *output, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh) {
  (void)data;
  (void)output;
  (void)flags;
  (void)width;
  (void)height;
  (void)refresh;
}

static void output_done(void *data, struct wl_output *output) {
  (void)data;
  (void)output;
}

static void output_scale(void *data, struct wl_output *output, int32_t factor) {
  (void)data;
  (void)output;
  (void)factor;
}

static void output_name(void *data, struct wl_output *output,
                        const char *name) {
  (void)output;
  IdentifierOutput *identifier = data;
  snprintf(identifier->name, sizeof(identifier->name), "%s", name);
}

static void output_description(void *data, struct wl_output *output,
                               const char *description) {
  (void)data;
  (void)output;
  (void)description;
}

static const struct wl_output_listener OUTPUT_LISTENER = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version) {
  IdentifierState *state = data;
  if (strcmp(interface, wl_compositor_interface.name) == 0) {
    uint32_t bind_version = version < 4 ? version : 4;
    state->compositor = wl_registry_bind(
        registry, name, &wl_compositor_interface, bind_version);
  } else if (strcmp(interface, wl_shm_interface.name) == 0) {
    state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
  } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
    uint32_t bind_version = version < 4 ? version : 4;
    state->layer_shell = wl_registry_bind(
        registry, name, &zwlr_layer_shell_v1_interface, bind_version);
  } else if (strcmp(interface, wl_output_interface.name) == 0 &&
             state->output_count < DISPLAY_LAYOUT_MAX_DISPLAYS) {
    IdentifierOutput *identifier = &state->outputs[state->output_count++];
    identifier->state = state;
    identifier->registry_name = name;
    uint32_t bind_version = version < 4 ? version : 4;
    identifier->output =
        wl_registry_bind(registry, name, &wl_output_interface, bind_version);
    wl_output_add_listener(identifier->output, &OUTPUT_LISTENER, identifier);
  }
}

static void registry_remove(void *data, struct wl_registry *registry,
                            uint32_t name) {
  (void)registry;
  IdentifierState *state = data;
  for (size_t index = 0; index < state->output_count; index++) {
    if (state->outputs[index].registry_name == name) {
      state->outputs[index].closed = true;
    }
  }
}

static const struct wl_registry_listener REGISTRY_LISTENER = {
    .global = registry_global,
    .global_remove = registry_remove,
};

static void layer_configure(void *data,
                            struct zwlr_layer_surface_v1 *layer_surface,
                            uint32_t serial, uint32_t width, uint32_t height) {
  (void)width;
  (void)height;
  IdentifierOutput *output = data;
  zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
  output->configured = true;
}

static void layer_closed(void *data,
                         struct zwlr_layer_surface_v1 *layer_surface) {
  (void)layer_surface;
  ((IdentifierOutput *)data)->closed = true;
}

static const struct zwlr_layer_surface_v1_listener LAYER_LISTENER = {
    .configure = layer_configure,
    .closed = layer_closed,
};

static const char *font_path(char *buffer, size_t size) {
  ssize_t length = readlink("/proc/self/exe", buffer, size - 1);
  if (length > 0) {
    buffer[length] = '\0';
    char *slash = strrchr(buffer, '/');
    if (slash != NULL) {
      *slash = '\0';
      slash = strrchr(buffer, '/');
      if (slash != NULL) {
        *slash = '\0';
        const char *suffix = "/share/display-layout/DejaVuSansMono.ttf";
        size_t used = strlen(buffer);
        if (used + strlen(suffix) + 1 < size) {
          strcpy(buffer + used, suffix);
          if (access(buffer, R_OK) == 0) {
            return buffer;
          }
        }
      }
    }
  }
  return "assets/DejaVuSansMono.ttf";
}

static int load_font(IdentifierState *state) {
  char path[4096];
  const char *resolved = font_path(path, sizeof(path));
  FILE *file = fopen(resolved, "rb");
  if (file == NULL) {
    set_error(state, "cannot open identifier font");
    return -1;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    set_error(state, "cannot inspect identifier font");
    return -1;
  }
  long size = ftell(file);
  rewind(file);
  if (size <= 0) {
    fclose(file);
    set_error(state, "identifier font is empty");
    return -1;
  }
  state->font_data = malloc((size_t)size);
  if (state->font_data == NULL ||
      fread(state->font_data, 1, (size_t)size, file) != (size_t)size) {
    fclose(file);
    set_error(state, "cannot read identifier font");
    return -1;
  }
  fclose(file);
  if (!stbtt_InitFont(&state->font, state->font_data,
                      stbtt_GetFontOffsetForIndex(state->font_data, 0))) {
    set_error(state, "cannot parse identifier font");
    return -1;
  }
  return 0;
}

static uint32_t premultiplied(unsigned int red, unsigned int green,
                              unsigned int blue, unsigned int alpha) {
  red = red * alpha / 255U;
  green = green * alpha / 255U;
  blue = blue * alpha / 255U;
  return (alpha << 24U) | (red << 16U) | (green << 8U) | blue;
}

static void blend(uint32_t *destination, unsigned int red, unsigned int green,
                  unsigned int blue, unsigned int alpha) {
  unsigned int inverse = 255U - alpha;
  unsigned int old = *destination;
  unsigned int old_alpha = old >> 24U;
  unsigned int old_red = (old >> 16U) & 255U;
  unsigned int old_green = (old >> 8U) & 255U;
  unsigned int old_blue = old & 255U;
  unsigned int out_alpha = alpha + old_alpha * inverse / 255U;
  unsigned int out_red = red * alpha / 255U + old_red * inverse / 255U;
  unsigned int out_green = green * alpha / 255U + old_green * inverse / 255U;
  unsigned int out_blue = blue * alpha / 255U + old_blue * inverse / 255U;
  *destination =
      (out_alpha << 24U) | (out_red << 16U) | (out_green << 8U) | out_blue;
}

static void render_frame(IdentifierState *state, IdentifierOutput *output,
                         FrameBuffer *frame, double opacity) {
  memset(frame->pixels, 0, OVERLAY_SIZE * OVERLAY_SIZE * sizeof(uint32_t));
  int center = OVERLAY_SIZE / 2;
  int radius = 100;
  unsigned int background_alpha = (unsigned int)(190.0 * opacity);
  for (int y = center - radius; y < center + radius; y++) {
    for (int x = center - radius; x < center + radius; x++) {
      int dx = x - center;
      int dy = y - center;
      if (dx * dx + dy * dy <= radius * radius) {
        frame->pixels[y * OVERLAY_SIZE + x] =
            premultiplied(27, 31, 38, background_alpha);
      }
    }
  }

  char number[16];
  snprintf(number, sizeof(number), "%u", output->number);
  float scale = stbtt_ScaleForPixelHeight(&state->font, FONT_SIZE);
  int ascent;
  int descent;
  int line_gap;
  stbtt_GetFontVMetrics(&state->font, &ascent, &descent, &line_gap);
  (void)line_gap;
  int width = 0;
  for (size_t index = 0; number[index] != '\0'; index++) {
    int advance;
    int bearing;
    stbtt_GetCodepointHMetrics(&state->font, number[index], &advance, &bearing);
    (void)bearing;
    width += (int)lroundf((float)advance * scale);
  }
  int pen_x = (OVERLAY_SIZE - width) / 2;
  int baseline =
      center + (int)lroundf((float)(ascent + descent) * scale / 2.0F);
  unsigned int text_alpha = (unsigned int)(255.0 * opacity);
  for (size_t index = 0; number[index] != '\0'; index++) {
    int glyph_width;
    int glyph_height;
    int x_offset;
    int y_offset;
    unsigned char *bitmap = stbtt_GetCodepointBitmap(
        &state->font, 0.0F, scale, number[index], &glyph_width, &glyph_height,
        &x_offset, &y_offset);
    int advance;
    int bearing;
    stbtt_GetCodepointHMetrics(&state->font, number[index], &advance, &bearing);
    (void)bearing;
    for (int y = 0; y < glyph_height; y++) {
      int destination_y = baseline + y_offset + y;
      if (destination_y < 0 || destination_y >= OVERLAY_SIZE) {
        continue;
      }
      for (int x = 0; x < glyph_width; x++) {
        int destination_x = pen_x + x_offset + x;
        if (destination_x < 0 || destination_x >= OVERLAY_SIZE) {
          continue;
        }
        unsigned int coverage = bitmap[y * glyph_width + x];
        unsigned int alpha = coverage * text_alpha / 255U;
        blend(&frame->pixels[destination_y * OVERLAY_SIZE + destination_x], 246,
              247, 249, alpha);
      }
    }
    stbtt_FreeBitmap(bitmap, NULL);
    pen_x += (int)lroundf((float)advance * scale);
  }
}

static IdentifierOutput *find_output(IdentifierState *state,
                                     const char *connector) {
  for (size_t index = 0; index < state->output_count; index++) {
    if (strcmp(state->outputs[index].name, connector) == 0) {
      return &state->outputs[index];
    }
  }
  return NULL;
}

static int create_buffers(IdentifierState *state) {
  size_t frame_size = OVERLAY_SIZE * OVERLAY_SIZE * sizeof(uint32_t);
  state->pool_size = state->output_count * BUFFER_COUNT * frame_size;
  int fd = anonymous_file(state->pool_size);
  if (fd < 0) {
    set_error(state, "cannot allocate Wayland identifier buffers");
    return -1;
  }
  state->pool_data =
      mmap(NULL, state->pool_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (state->pool_data == MAP_FAILED) {
    close(fd);
    state->pool_data = NULL;
    set_error(state, "cannot map Wayland identifier buffers");
    return -1;
  }
  state->pool = wl_shm_create_pool(state->shm, fd, (int32_t)state->pool_size);
  close(fd);
  for (size_t output_index = 0; output_index < state->output_count;
       output_index++) {
    for (int buffer_index = 0; buffer_index < BUFFER_COUNT; buffer_index++) {
      size_t frame_index = output_index * BUFFER_COUNT + (size_t)buffer_index;
      FrameBuffer *frame = &state->outputs[output_index].buffers[buffer_index];
      frame->pixels = (uint32_t *)((unsigned char *)state->pool_data +
                                   frame_index * frame_size);
      frame->buffer = wl_shm_pool_create_buffer(
          state->pool, (int32_t)(frame_index * frame_size), OVERLAY_SIZE,
          OVERLAY_SIZE, OVERLAY_SIZE * 4, WL_SHM_FORMAT_ARGB8888);
      wl_buffer_add_listener(frame->buffer, &BUFFER_LISTENER, frame);
    }
  }
  return 0;
}

static int create_surface(IdentifierState *state, IdentifierOutput *output) {
  output->surface = wl_compositor_create_surface(state->compositor);
  output->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
      state->layer_shell, output->surface, output->output,
      ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "display-layout-identifier");
  if (output->surface == NULL || output->layer_surface == NULL) {
    set_error(state, "cannot create Wayland identifier surface");
    return -1;
  }
  zwlr_layer_surface_v1_add_listener(output->layer_surface, &LAYER_LISTENER,
                                     output);
  zwlr_layer_surface_v1_set_size(output->layer_surface, OVERLAY_SIZE,
                                 OVERLAY_SIZE);
  zwlr_layer_surface_v1_set_exclusive_zone(output->layer_surface, -1);
  zwlr_layer_surface_v1_set_keyboard_interactivity(
      output->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
  struct wl_region *empty = wl_compositor_create_region(state->compositor);
  wl_surface_set_input_region(output->surface, empty);
  wl_region_destroy(empty);
  wl_surface_commit(output->surface);
  return 0;
}

static void cleanup(IdentifierState *state) {
  for (size_t index = 0; index < state->output_count; index++) {
    IdentifierOutput *output = &state->outputs[index];
    for (int buffer = 0; buffer < BUFFER_COUNT; buffer++) {
      if (output->buffers[buffer].buffer != NULL) {
        wl_buffer_destroy(output->buffers[buffer].buffer);
      }
    }
    if (output->layer_surface != NULL) {
      zwlr_layer_surface_v1_destroy(output->layer_surface);
    }
    if (output->surface != NULL) {
      wl_surface_destroy(output->surface);
    }
    if (output->output != NULL) {
      wl_output_destroy(output->output);
    }
  }
  if (state->pool != NULL) {
    wl_shm_pool_destroy(state->pool);
  }
  if (state->pool_data != NULL) {
    munmap(state->pool_data, state->pool_size);
  }
  if (state->layer_shell != NULL) {
    zwlr_layer_shell_v1_destroy(state->layer_shell);
  }
  if (state->shm != NULL) {
    wl_shm_destroy(state->shm);
  }
  if (state->compositor != NULL) {
    wl_compositor_destroy(state->compositor);
  }
  if (state->registry != NULL) {
    wl_registry_destroy(state->registry);
  }
  if (state->display != NULL) {
    wl_display_disconnect(state->display);
  }
  free(state->font_data);
}

static long long elapsed_milliseconds(struct timespec start,
                                      struct timespec now) {
  return (long long)(now.tv_sec - start.tv_sec) * 1000LL +
         (long long)(now.tv_nsec - start.tv_nsec) / 1000000LL;
}

int identifier_wayland_show(const DisplayList *list, unsigned int duration_ms,
                            char *error, size_t error_size) {
  IdentifierState state = {0};
  state.display = wl_display_connect(NULL);
  if (state.display == NULL) {
    snprintf(error, error_size, "cannot connect to the Wayland display");
    return -1;
  }
  state.registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(state.registry, &REGISTRY_LISTENER, &state);
  wl_display_roundtrip(state.display);
  wl_display_roundtrip(state.display);
  if (state.compositor == NULL || state.shm == NULL ||
      state.layer_shell == NULL) {
    set_error(&state, "compositor does not provide the layer-shell protocol");
  }
  if (!state.failed && load_font(&state) != 0) {
    state.failed = true;
  }
  if (!state.failed && create_buffers(&state) != 0) {
    state.failed = true;
  }

  IdentifierOutput *active[DISPLAY_LAYOUT_MAX_DISPLAYS] = {0};
  size_t active_count = 0;
  if (!state.failed) {
    for (size_t index = 0; index < list->count; index++) {
      IdentifierOutput *output =
          find_output(&state, list->displays[index].connector);
      if (output == NULL) {
        set_error(&state, "cannot match a niri output to a Wayland output");
        break;
      }
      output->number = (unsigned int)index + 1U;
      if (create_surface(&state, output) != 0) {
        break;
      }
      active[active_count++] = output;
    }
  }
  while (!state.failed) {
    bool configured = true;
    for (size_t index = 0; index < active_count; index++) {
      configured = configured && active[index]->configured;
    }
    if (configured) {
      break;
    }
    if (wl_display_dispatch(state.display) < 0) {
      set_error(&state, "Wayland disconnected while configuring identifiers");
    }
  }

  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);
  while (!state.failed) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long elapsed = elapsed_milliseconds(start, now);
    if ((unsigned long long)elapsed >= duration_ms) {
      break;
    }
    double progress = duration_ms == 0 ? 1.0 : (double)elapsed / duration_ms;
    double opacity =
        progress < FADE_START ? 1.0 : (1.0 - progress) / (1.0 - FADE_START);
    bool committed = false;
    for (size_t index = 0; index < active_count; index++) {
      IdentifierOutput *output = active[index];
      FrameBuffer *frame = NULL;
      for (int attempt = 0; attempt < BUFFER_COUNT; attempt++) {
        int candidate = (output->next_buffer + attempt) % BUFFER_COUNT;
        if (!output->buffers[candidate].busy) {
          frame = &output->buffers[candidate];
          output->next_buffer = (candidate + 1) % BUFFER_COUNT;
          break;
        }
      }
      if (frame == NULL || output->closed) {
        continue;
      }
      render_frame(&state, output, frame, opacity);
      frame->busy = true;
      wl_surface_attach(output->surface, frame->buffer, 0, 0);
      wl_surface_damage_buffer(output->surface, 0, 0, OVERLAY_SIZE,
                               OVERLAY_SIZE);
      wl_surface_commit(output->surface);
      committed = true;
    }
    if (committed && wl_display_flush(state.display) < 0 && errno != EAGAIN) {
      set_error(&state, "cannot update Wayland identifiers");
      break;
    }
    wl_display_dispatch_pending(state.display);
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 16000000L};
    nanosleep(&pause, NULL);
    wl_display_roundtrip(state.display);
  }

  if (state.failed) {
    snprintf(error, error_size, "%s", state.error);
  }
  cleanup(&state);
  return state.failed ? -1 : 0;
}
