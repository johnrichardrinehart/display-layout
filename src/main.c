#define _POSIX_C_SOURCE 200809L

#include "backend.h"
#include "config.h"
#include "model.h"

#include "xdg-shell-client-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "third_party/stb_truetype.h"

#ifndef DISPLAY_LAYOUT_VERSION
#define DISPLAY_LAYOUT_VERSION "development"
#endif

static const float MILLIMETERS_PER_INCH = 25.4F;
static const float LAYOUT_WIDTH_FRACTION = 0.84F;
static const float LAYOUT_HEIGHT_FRACTION = 0.76F;
static const float MONITOR_INSET_FRACTION = 0.045F;
static const float FONT_ASCENT_FRACTION = 0.82F;

enum {
  FONT_ATLAS_SIZE = 1024,
  UI_MARGIN = 32,
  UI_HEADER_HEIGHT = 52,
  UI_FOOTER_HEIGHT = 74,
  UI_BUTTON_HEIGHT = 36,
  UI_BUTTON_WIDTH = 104,
  UI_BUTTON_BOTTOM_MARGIN = 16,
  UI_BUTTON_GAP = 8,
  UI_CLOSE_CENTER = 24,
  UI_CLOSE_RADIUS = 16,
  UI_BADGE_SIZE = 34,
  UI_BADGE_MARGIN = 12,
  FRAME_BUFFER_COUNT = 2,
};

typedef struct {
  int x;
  int y;
  int width;
  int height;
} Rect;

typedef uint32_t UiColor;

typedef struct {
  unsigned char *atlas;
  stbtt_bakedchar glyphs[95];
  int ascent;
  int descent;
} FontRenderer;

typedef struct {
  UiColor backdrop;
  UiColor surface;
  UiColor canvas;
  UiColor grid;
  UiColor border;
  UiColor monitor;
  UiColor monitor_inner;
  UiColor text;
  UiColor muted;
  UiColor accent;
  UiColor button;
  UiColor button_hover;
  UiColor white;
} Theme;

typedef struct {
  Rect canvas;
  float scale;
  float origin_x;
  float origin_y;
} ViewTransform;

typedef struct {
  int value;
  int guide_coordinate;
  bool snapped;
  bool centerline;
} SnapResult;

typedef struct {
  struct wl_buffer *buffer;
  uint32_t *pixels;
  bool busy;
} FrameBuffer;

typedef struct {
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_compositor *compositor;
  struct wl_shm *shm;
  struct wl_seat *seat;
  struct wl_pointer *pointer;
  struct wl_keyboard *keyboard;
  struct xdg_wm_base *wm_base;
  struct wl_surface *surface;
  struct xdg_surface *xdg_surface;
  struct xdg_toplevel *toplevel;
  struct xkb_context *xkb_context;
  struct xkb_keymap *xkb_keymap;
  struct xkb_state *xkb_state;
  struct wl_shm_pool *pool;
  void *pool_data;
  size_t pool_size;
  FrameBuffer buffers[FRAME_BUFFER_COUNT];
  uint32_t *pixels;
  int current_buffer;
  FontRenderer font;
  Theme theme;
  int width;
  int height;
  int mouse_x;
  int mouse_y;
  xkb_keysym_t key;
  bool configured;
  bool close_requested;
  bool pointer_pressed;
  bool pointer_released;
  bool key_pressed;
  bool control_down;
  bool shift_down;
  bool needs_redraw;
} Ui;

static int anonymous_file(size_t size) {
  const char *runtime = getenv("XDG_RUNTIME_DIR");
  if (runtime == NULL || runtime[0] == '\0') {
    errno = ENOENT;
    return -1;
  }
  char path[4096];
  snprintf(path, sizeof(path), "%s/display-layout-ui-XXXXXX", runtime);
  int fd = mkstemp(path);
  if (fd < 0)
    return -1;
  unlink(path);
  int flags = fcntl(fd, F_GETFD);
  if (flags >= 0)
    fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
  if (ftruncate(fd, (off_t)size) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static void frame_buffer_release(void *data, struct wl_buffer *buffer) {
  (void)buffer;
  ((FrameBuffer *)data)->busy = false;
}

static const struct wl_buffer_listener FRAME_BUFFER_LISTENER = {
    .release = frame_buffer_release,
};

static void destroy_buffers(Ui *ui) {
  for (int i = 0; i < FRAME_BUFFER_COUNT; i++) {
    if (ui->buffers[i].buffer != NULL)
      wl_buffer_destroy(ui->buffers[i].buffer);
    ui->buffers[i] = (FrameBuffer){0};
  }
  if (ui->pool != NULL)
    wl_shm_pool_destroy(ui->pool);
  if (ui->pool_data != NULL)
    munmap(ui->pool_data, ui->pool_size);
  ui->pool = NULL;
  ui->pool_data = NULL;
  ui->pool_size = 0;
  ui->pixels = NULL;
}

static int resize_buffer(Ui *ui, int width, int height) {
  destroy_buffers(ui);
  ui->width = width;
  ui->height = height;
  size_t frame_size = (size_t)width * (size_t)height * sizeof(uint32_t);
  ui->pool_size = frame_size * FRAME_BUFFER_COUNT;
  int fd = anonymous_file(ui->pool_size);
  if (fd < 0)
    return -1;
  ui->pool_data =
      mmap(NULL, ui->pool_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (ui->pool_data == MAP_FAILED) {
    ui->pool_data = NULL;
    close(fd);
    return -1;
  }
  ui->pool = wl_shm_create_pool(ui->shm, fd, (int32_t)ui->pool_size);
  close(fd);
  for (int i = 0; i < FRAME_BUFFER_COUNT; i++) {
    size_t offset = frame_size * (size_t)i;
    ui->buffers[i].pixels =
        (uint32_t *)((unsigned char *)ui->pool_data + offset);
    ui->buffers[i].buffer =
        wl_shm_pool_create_buffer(ui->pool, (int32_t)offset, width, height,
                                  width * 4, WL_SHM_FORMAT_XRGB8888);
    wl_buffer_add_listener(ui->buffers[i].buffer, &FRAME_BUFFER_LISTENER,
                           &ui->buffers[i]);
  }
  return 0;
}

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base,
                         uint32_t serial) {
  (void)data;
  xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener WM_BASE_LISTENER = {
    .ping = wm_base_ping,
};

static void pointer_enter(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t x, wl_fixed_t y) {
  (void)pointer;
  (void)serial;
  (void)surface;
  Ui *ui = data;
  ui->mouse_x = wl_fixed_to_int(x);
  ui->mouse_y = wl_fixed_to_int(y);
  ui->needs_redraw = true;
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface) {
  (void)pointer;
  (void)serial;
  (void)surface;
  Ui *ui = data;
  ui->mouse_x = -1;
  ui->mouse_y = -1;
  ui->needs_redraw = true;
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
                           uint32_t time, wl_fixed_t x, wl_fixed_t y) {
  (void)pointer;
  (void)time;
  Ui *ui = data;
  ui->mouse_x = wl_fixed_to_int(x);
  ui->mouse_y = wl_fixed_to_int(y);
  ui->needs_redraw = true;
}

static void pointer_button(void *data, struct wl_pointer *pointer,
                           uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t state) {
  (void)pointer;
  (void)serial;
  (void)time;
  Ui *ui = data;
  if (button != 0x110)
    return;
  if (state == WL_POINTER_BUTTON_STATE_PRESSED)
    ui->pointer_pressed = true;
  else
    ui->pointer_released = true;
  ui->needs_redraw = true;
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
                         uint32_t axis, wl_fixed_t value) {
  (void)data;
  (void)pointer;
  (void)time;
  (void)axis;
  (void)value;
}

static void pointer_frame(void *data, struct wl_pointer *pointer) {
  (void)data;
  (void)pointer;
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer,
                                uint32_t axis_source) {
  (void)data;
  (void)pointer;
  (void)axis_source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer,
                              uint32_t time, uint32_t axis) {
  (void)data;
  (void)pointer;
  (void)time;
  (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer,
                                  uint32_t axis, int32_t discrete) {
  (void)data;
  (void)pointer;
  (void)axis;
  (void)discrete;
}

static void pointer_axis_value120(void *data, struct wl_pointer *pointer,
                                  uint32_t axis, int32_t value120) {
  (void)data;
  (void)pointer;
  (void)axis;
  (void)value120;
}

static void pointer_axis_relative_direction(void *data,
                                            struct wl_pointer *pointer,
                                            uint32_t axis, uint32_t direction) {
  (void)data;
  (void)pointer;
  (void)axis;
  (void)direction;
}

static const struct wl_pointer_listener POINTER_LISTENER = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
    .axis_value120 = pointer_axis_value120,
    .axis_relative_direction = pointer_axis_relative_direction,
};

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                            uint32_t format, int32_t fd, uint32_t size) {
  (void)keyboard;
  Ui *ui = data;
  if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
    close(fd);
    return;
  }
  char *keymap_text = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (keymap_text == MAP_FAILED)
    return;
  struct xkb_keymap *keymap = xkb_keymap_new_from_string(
      ui->xkb_context, keymap_text, XKB_KEYMAP_FORMAT_TEXT_V1,
      XKB_KEYMAP_COMPILE_NO_FLAGS);
  munmap(keymap_text, size);
  if (keymap == NULL)
    return;
  struct xkb_state *state = xkb_state_new(keymap);
  if (state == NULL) {
    xkb_keymap_unref(keymap);
    return;
  }
  if (ui->xkb_state != NULL)
    xkb_state_unref(ui->xkb_state);
  if (ui->xkb_keymap != NULL)
    xkb_keymap_unref(ui->xkb_keymap);
  ui->xkb_keymap = keymap;
  ui->xkb_state = state;
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys) {
  (void)data;
  (void)keyboard;
  (void)serial;
  (void)surface;
  (void)keys;
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface) {
  (void)data;
  (void)keyboard;
  (void)serial;
  (void)surface;
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
                         uint32_t serial, uint32_t time, uint32_t key,
                         uint32_t state) {
  (void)keyboard;
  (void)serial;
  (void)time;
  Ui *ui = data;
  if (ui->xkb_state == NULL)
    return;
  bool pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
  xkb_state_update_key(ui->xkb_state, key + 8,
                       pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
  if (pressed) {
    ui->key = xkb_state_key_get_one_sym(ui->xkb_state, key + 8);
    ui->key_pressed = true;
    ui->needs_redraw = true;
  }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                               uint32_t serial, uint32_t depressed,
                               uint32_t latched, uint32_t locked,
                               uint32_t group) {
  (void)keyboard;
  (void)serial;
  Ui *ui = data;
  if (ui->xkb_state == NULL)
    return;
  xkb_state_update_mask(ui->xkb_state, depressed, latched, locked, 0, 0, group);
  ui->control_down =
      xkb_state_mod_name_is_active(ui->xkb_state, XKB_MOD_NAME_CTRL,
                                   XKB_STATE_MODS_EFFECTIVE) > 0;
  ui->shift_down =
      xkb_state_mod_name_is_active(ui->xkb_state, XKB_MOD_NAME_SHIFT,
                                   XKB_STATE_MODS_EFFECTIVE) > 0;
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
                                 int32_t rate, int32_t delay) {
  (void)data;
  (void)keyboard;
  (void)rate;
  (void)delay;
}

static const struct wl_keyboard_listener KEYBOARD_LISTENER = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *seat,
                              uint32_t capabilities) {
  Ui *ui = data;
  if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0 && ui->pointer == NULL) {
    ui->pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(ui->pointer, &POINTER_LISTENER, ui);
  } else if ((capabilities & WL_SEAT_CAPABILITY_POINTER) == 0 &&
             ui->pointer != NULL) {
    wl_pointer_destroy(ui->pointer);
    ui->pointer = NULL;
  }
  if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0 &&
      ui->keyboard == NULL) {
    ui->keyboard = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(ui->keyboard, &KEYBOARD_LISTENER, ui);
  } else if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) == 0 &&
             ui->keyboard != NULL) {
    wl_keyboard_destroy(ui->keyboard);
    ui->keyboard = NULL;
  }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
  (void)data;
  (void)seat;
  (void)name;
}

static const struct wl_seat_listener SEAT_LISTENER = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version) {
  Ui *ui = data;
  if (strcmp(interface, wl_compositor_interface.name) == 0) {
    uint32_t bind_version = version < 4 ? version : 4;
    ui->compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
                                      bind_version);
  } else if (strcmp(interface, wl_shm_interface.name) == 0) {
    ui->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
  } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
    ui->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    xdg_wm_base_add_listener(ui->wm_base, &WM_BASE_LISTENER, ui);
  } else if (strcmp(interface, wl_seat_interface.name) == 0 &&
             ui->seat == NULL) {
    uint32_t bind_version = version < 5 ? version : 5;
    ui->seat =
        wl_registry_bind(registry, name, &wl_seat_interface, bind_version);
    wl_seat_add_listener(ui->seat, &SEAT_LISTENER, ui);
  }
}

static void registry_remove(void *data, struct wl_registry *registry,
                            uint32_t name) {
  (void)data;
  (void)registry;
  (void)name;
}

static const struct wl_registry_listener REGISTRY_LISTENER = {
    .global = registry_global,
    .global_remove = registry_remove,
};

static void xdg_surface_configure(void *data, struct xdg_surface *surface,
                                  uint32_t serial) {
  Ui *ui = data;
  xdg_surface_ack_configure(surface, serial);
  ui->configured = true;
  ui->needs_redraw = true;
}

static const struct xdg_surface_listener XDG_SURFACE_LISTENER = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                               int32_t width, int32_t height,
                               struct wl_array *states) {
  (void)data;
  (void)toplevel;
  (void)width;
  (void)height;
  (void)states;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
  (void)toplevel;
  ((Ui *)data)->close_requested = true;
}

static const struct xdg_toplevel_listener TOPLEVEL_LISTENER = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

static int ui_init(Ui *ui, int width, int height, char *error,
                   size_t error_size) {
  *ui = (Ui){.width = width, .height = height, .mouse_x = -1, .mouse_y = -1};
  ui->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (ui->xkb_context == NULL) {
    snprintf(error, error_size, "cannot initialize Wayland keyboard support");
    return -1;
  }
  ui->display = wl_display_connect(NULL);
  if (ui->display == NULL) {
    snprintf(error, error_size, "cannot connect to Wayland compositor");
    return -1;
  }
  ui->registry = wl_display_get_registry(ui->display);
  wl_registry_add_listener(ui->registry, &REGISTRY_LISTENER, ui);
  if (wl_display_roundtrip(ui->display) < 0 || ui->compositor == NULL ||
      ui->shm == NULL || ui->wm_base == NULL) {
    snprintf(
        error, error_size,
        "Wayland compositor is missing xdg-shell or shared-memory support");
    return -1;
  }
  wl_display_roundtrip(ui->display);
  ui->surface = wl_compositor_create_surface(ui->compositor);
  ui->xdg_surface = xdg_wm_base_get_xdg_surface(ui->wm_base, ui->surface);
  xdg_surface_add_listener(ui->xdg_surface, &XDG_SURFACE_LISTENER, ui);
  ui->toplevel = xdg_surface_get_toplevel(ui->xdg_surface);
  xdg_toplevel_add_listener(ui->toplevel, &TOPLEVEL_LISTENER, ui);
  xdg_toplevel_set_title(ui->toplevel, "Display Layout Editor");
  xdg_toplevel_set_app_id(ui->toplevel, "display-layout-editor");
  xdg_toplevel_set_min_size(ui->toplevel, width, height);
  xdg_toplevel_set_max_size(ui->toplevel, width, height);
  wl_surface_commit(ui->surface);
  while (!ui->configured) {
    if (wl_display_dispatch(ui->display) < 0) {
      snprintf(error, error_size, "Wayland compositor closed the connection");
      return -1;
    }
  }
  if (resize_buffer(ui, width, height) != 0) {
    snprintf(error, error_size, "cannot allocate Wayland drawing buffers: %s",
             strerror(errno));
    return -1;
  }
  return 0;
}

static int begin_frame(Ui *ui) {
  for (;;) {
    for (int i = 0; i < FRAME_BUFFER_COUNT; i++) {
      if (!ui->buffers[i].busy) {
        ui->current_buffer = i;
        ui->pixels = ui->buffers[i].pixels;
        return 0;
      }
    }
    if (wl_display_dispatch(ui->display) < 0)
      return -1;
  }
}

static void present_frame(Ui *ui) {
  FrameBuffer *frame = &ui->buffers[ui->current_buffer];
  frame->busy = true;
  wl_surface_attach(ui->surface, frame->buffer, 0, 0);
  wl_surface_damage_buffer(ui->surface, 0, 0, ui->width, ui->height);
  wl_surface_commit(ui->surface);
  wl_display_flush(ui->display);
  ui->pixels = NULL;
}

static void ui_destroy(Ui *ui) {
  free(ui->font.atlas);
  destroy_buffers(ui);
  if (ui->xkb_state != NULL)
    xkb_state_unref(ui->xkb_state);
  if (ui->xkb_keymap != NULL)
    xkb_keymap_unref(ui->xkb_keymap);
  if (ui->xkb_context != NULL)
    xkb_context_unref(ui->xkb_context);
  if (ui->keyboard != NULL)
    wl_keyboard_destroy(ui->keyboard);
  if (ui->pointer != NULL)
    wl_pointer_destroy(ui->pointer);
  if (ui->seat != NULL)
    wl_seat_destroy(ui->seat);
  if (ui->toplevel != NULL)
    xdg_toplevel_destroy(ui->toplevel);
  if (ui->xdg_surface != NULL)
    xdg_surface_destroy(ui->xdg_surface);
  if (ui->surface != NULL)
    wl_surface_destroy(ui->surface);
  if (ui->wm_base != NULL)
    xdg_wm_base_destroy(ui->wm_base);
  if (ui->shm != NULL)
    wl_shm_destroy(ui->shm);
  if (ui->compositor != NULL)
    wl_compositor_destroy(ui->compositor);
  if (ui->registry != NULL)
    wl_registry_destroy(ui->registry);
  if (ui->display != NULL)
    wl_display_disconnect(ui->display);
}

static UiColor color(Ui *ui, unsigned short red, unsigned short green,
                     unsigned short blue) {
  (void)ui;
  return 0xff000000U | ((uint32_t)(red >> 8) << 16) |
         ((uint32_t)(green >> 8) << 8) | (uint32_t)(blue >> 8);
}

static Theme dark_theme(Ui *ui) {
  return (Theme){
      .backdrop = color(ui, 0x0c0c, 0x0f0f, 0x1414),
      .surface = color(ui, 0x1b1b, 0x1f1f, 0x2626),
      .canvas = color(ui, 0x1111, 0x1515, 0x1a1a),
      .grid = color(ui, 0x2020, 0x2525, 0x2d2d),
      .border = color(ui, 0x3535, 0x3b3b, 0x4646),
      .monitor = color(ui, 0x2525, 0x2b2b, 0x3434),
      .monitor_inner = color(ui, 0x2020, 0x2525, 0x2d2d),
      .text = color(ui, 0xf6f6, 0xf7f7, 0xf9f9),
      .muted = color(ui, 0x9c9c, 0xa5a5, 0xb4b4),
      .accent = color(ui, 0x6e6e, 0xd6d6, 0xffff),
      .button = color(ui, 0x2525, 0x2b2b, 0x3434),
      .button_hover = color(ui, 0x2f2f, 0x3737, 0x4242),
      .white = color(ui, 0xffff, 0xffff, 0xffff),
  };
}

static Theme light_theme(Ui *ui) {
  return (Theme){
      .backdrop = color(ui, 0xe3e3, 0xe7e7, 0xeded),
      .surface = color(ui, 0xf7f7, 0xf8f8, 0xfafa),
      .canvas = color(ui, 0xebeb, 0xeeee, 0xf2f2),
      .grid = color(ui, 0xd7d7, 0xdcdc, 0xe3e3),
      .border = color(ui, 0xbaba, 0xc2c2, 0xcdcd),
      .monitor = color(ui, 0xdada, 0xdfdf, 0xe6e6),
      .monitor_inner = color(ui, 0xf4f4, 0xf6f6, 0xf8f8),
      .text = color(ui, 0x1f1f, 0x2424, 0x2c2c),
      .muted = color(ui, 0x5b5b, 0x6565, 0x7474),
      .accent = color(ui, 0x1212, 0x8b8b, 0xbaba),
      .button = color(ui, 0xe5e5, 0xe9e9, 0xeeee),
      .button_hover = color(ui, 0xd8d8, 0xdede, 0xe5e5),
      .white = color(ui, 0xffff, 0xffff, 0xffff),
  };
}

static bool point_in_rect(int x, int y, Rect rectangle) {
  return x >= rectangle.x && y >= rectangle.y &&
         x < rectangle.x + rectangle.width &&
         y < rectangle.y + rectangle.height;
}

static void put_pixel(Ui *ui, int x, int y, UiColor color_value) {
  if (x >= 0 && y >= 0 && x < ui->width && y < ui->height)
    ui->pixels[(size_t)y * (size_t)ui->width + (size_t)x] = color_value;
}

static void blend_pixel(Ui *ui, int x, int y, UiColor color_value,
                        unsigned int alpha) {
  if (x < 0 || y < 0 || x >= ui->width || y >= ui->height || alpha == 0)
    return;
  uint32_t *target = &ui->pixels[(size_t)y * (size_t)ui->width + (size_t)x];
  unsigned int inverse = 255U - alpha;
  unsigned int red = (((color_value >> 16) & 0xffU) * alpha +
                      ((*target >> 16) & 0xffU) * inverse) /
                     255U;
  unsigned int green = (((color_value >> 8) & 0xffU) * alpha +
                        ((*target >> 8) & 0xffU) * inverse) /
                       255U;
  unsigned int blue =
      ((color_value & 0xffU) * alpha + (*target & 0xffU) * inverse) / 255U;
  *target = 0xff000000U | (red << 16) | (green << 8) | blue;
}

static void fill_rect(Ui *ui, Rect rectangle, UiColor fill) {
  int start_x = rectangle.x < 0 ? 0 : rectangle.x;
  int start_y = rectangle.y < 0 ? 0 : rectangle.y;
  int end_x = rectangle.x + rectangle.width;
  int end_y = rectangle.y + rectangle.height;
  if (end_x > ui->width)
    end_x = ui->width;
  if (end_y > ui->height)
    end_y = ui->height;
  for (int y = start_y; y < end_y; y++) {
    uint32_t *row = ui->pixels + (size_t)y * (size_t)ui->width;
    for (int x = start_x; x < end_x; x++)
      row[x] = fill;
  }
}

static void fill_rounded(Ui *ui, Rect rectangle, int radius, UiColor fill) {
  if (radius <= 0 || rectangle.width < radius * 2 ||
      rectangle.height < radius * 2) {
    fill_rect(ui, rectangle, fill);
    return;
  }
  for (int y = rectangle.y; y < rectangle.y + rectangle.height; y++) {
    for (int x = rectangle.x; x < rectangle.x + rectangle.width; x++) {
      int corner_x = x < rectangle.x + radius
                         ? rectangle.x + radius
                         : (x >= rectangle.x + rectangle.width - radius
                                ? rectangle.x + rectangle.width - radius - 1
                                : x);
      int corner_y = y < rectangle.y + radius
                         ? rectangle.y + radius
                         : (y >= rectangle.y + rectangle.height - radius
                                ? rectangle.y + rectangle.height - radius - 1
                                : y);
      int dx = x - corner_x;
      int dy = y - corner_y;
      if (dx * dx + dy * dy <= radius * radius)
        put_pixel(ui, x, y, fill);
    }
  }
}

static void draw_line(Ui *ui, int x0, int y0, int x1, int y1, int width,
                      bool dashed, UiColor color_value) {
  int dx = abs(x1 - x0);
  int sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0);
  int sy = y0 < y1 ? 1 : -1;
  int error = dx + dy;
  int step = 0;
  for (;;) {
    if (!dashed || step % 11 < 6) {
      int half = width / 2;
      fill_rect(ui, (Rect){x0 - half, y0 - half, width, width}, color_value);
    }
    if (x0 == x1 && y0 == y1)
      break;
    int twice = error * 2;
    if (twice >= dy) {
      error += dy;
      x0 += sx;
    }
    if (twice <= dx) {
      error += dx;
      y0 += sy;
    }
    step++;
  }
}

static void stroke_rect(Ui *ui, Rect rectangle, int width, UiColor stroke) {
  fill_rect(ui, (Rect){rectangle.x, rectangle.y, rectangle.width, width},
            stroke);
  fill_rect(ui,
            (Rect){rectangle.x, rectangle.y + rectangle.height - width,
                   rectangle.width, width},
            stroke);
  fill_rect(ui, (Rect){rectangle.x, rectangle.y, width, rectangle.height},
            stroke);
  fill_rect(ui,
            (Rect){rectangle.x + rectangle.width - width, rectangle.y, width,
                   rectangle.height},
            stroke);
}

static int load_font(Ui *ui, const char *path, int pixel_size) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    return -1;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return -1;
  }
  long file_size = ftell(file);
  rewind(file);
  unsigned char *font_data = malloc((size_t)file_size);
  unsigned char *bitmap = calloc((size_t)FONT_ATLAS_SIZE * FONT_ATLAS_SIZE, 1U);
  if (font_data == NULL || bitmap == NULL ||
      fread(font_data, 1, (size_t)file_size, file) != (size_t)file_size) {
    free(font_data);
    free(bitmap);
    fclose(file);
    return -1;
  }
  fclose(file);
  int baked = stbtt_BakeFontBitmap(font_data, 0, (float)pixel_size, bitmap,
                                   FONT_ATLAS_SIZE, FONT_ATLAS_SIZE, 32, 95,
                                   ui->font.glyphs);
  free(font_data);
  if (baked <= 0) {
    free(bitmap);
    return -1;
  }

  ui->font.atlas = bitmap;
  ui->font.ascent = (int)roundf((float)pixel_size * FONT_ASCENT_FRACTION);
  ui->font.descent = pixel_size - ui->font.ascent;
  return 0;
}

static void draw_text(Ui *ui, const char *text, int x, int baseline,
                      UiColor color_value) {
  float cursor = (float)x;
  for (const unsigned char *character = (const unsigned char *)text;
       *character != '\0'; character++) {
    if (*character < 32 || *character > 126) {
      continue;
    }
    const stbtt_bakedchar *glyph = &ui->font.glyphs[*character - 32];
    int width = glyph->x1 - glyph->x0;
    int height = glyph->y1 - glyph->y0;
    int target_x = (int)roundf(cursor + glyph->xoff);
    int target_y = (int)roundf((float)baseline + glyph->yoff);
    for (int row = 0; row < height; row++) {
      for (int column = 0; column < width; column++) {
        unsigned int alpha =
            ui->font.atlas[(size_t)(glyph->y0 + row) * FONT_ATLAS_SIZE +
                           (size_t)(glyph->x0 + column)];
        blend_pixel(ui, target_x + column, target_y + row, color_value, alpha);
      }
    }
    cursor += glyph->xadvance;
  }
}

static int text_width(Ui *ui, const char *text) {
  float width = 0.0F;
  for (const unsigned char *character = (const unsigned char *)text;
       *character != '\0'; character++) {
    if (*character >= 32 && *character <= 126) {
      width += ui->font.glyphs[*character - 32].xadvance;
    }
  }
  return (int)roundf(width);
}

static void draw_text_centered(Ui *ui, const char *text, Rect bounds,
                               UiColor color_value) {
  int width = text_width(ui, text);
  int baseline = bounds.y +
                 (bounds.height - (ui->font.ascent + ui->font.descent)) / 2 +
                 ui->font.ascent;
  draw_text(ui, text, bounds.x + (bounds.width - width) / 2, baseline,
            color_value);
}

static int configured_dimension(WindowDimension dimension, int available,
                                int minimum) {
  int result = dimension.percent
                   ? (int)roundf(dimension.value * (float)available / 100.0F)
                   : (int)roundf(dimension.value);
  if (result < minimum) {
    result = minimum;
  }
  if (result > available) {
    result = available;
  }
  return result;
}

static void display_bounds(const DisplayList *list, int *minimum_x,
                           int *minimum_y, int *maximum_x, int *maximum_y) {
  *minimum_x = list->displays[0].x;
  *minimum_y = list->displays[0].y;
  *maximum_x = list->displays[0].x + list->displays[0].width;
  *maximum_y = list->displays[0].y + list->displays[0].height;
  for (size_t index = 1; index < list->count; index++) {
    const LayoutDisplay *display = &list->displays[index];
    *minimum_x = display->x < *minimum_x ? display->x : *minimum_x;
    *minimum_y = display->y < *minimum_y ? display->y : *minimum_y;
    *maximum_x = display->x + display->width > *maximum_x
                     ? display->x + display->width
                     : *maximum_x;
    *maximum_y = display->y + display->height > *maximum_y
                     ? display->y + display->height
                     : *maximum_y;
  }
}

static ViewTransform view_transform(const DisplayList *list, Rect canvas) {
  int minimum_x;
  int minimum_y;
  int maximum_x;
  int maximum_y;
  display_bounds(list, &minimum_x, &minimum_y, &maximum_x, &maximum_y);
  float layout_width = (float)(maximum_x - minimum_x);
  float layout_height = (float)(maximum_y - minimum_y);
  float scale =
      fminf((float)canvas.width * LAYOUT_WIDTH_FRACTION / layout_width,
            (float)canvas.height * LAYOUT_HEIGHT_FRACTION / layout_height);
  float rendered_width = layout_width * scale;
  float rendered_height = layout_height * scale;
  return (ViewTransform){
      .canvas = canvas,
      .scale = scale,
      .origin_x = (float)canvas.x +
                  ((float)canvas.width - rendered_width) / 2.0F -
                  (float)minimum_x * scale,
      .origin_y = (float)canvas.y +
                  ((float)canvas.height - rendered_height) / 2.0F -
                  (float)minimum_y * scale,
  };
}

static Rect display_rect(const LayoutDisplay *display,
                         ViewTransform transform) {
  return (Rect){
      .x =
          (int)roundf(transform.origin_x + (float)display->x * transform.scale),
      .y =
          (int)roundf(transform.origin_y + (float)display->y * transform.scale),
      .width = (int)roundf((float)display->width * transform.scale),
      .height = (int)roundf((float)display->height * transform.scale),
  };
}

static void friendly_name(const LayoutDisplay *display, char *buffer,
                          size_t size) {
  if (strncmp(display->connector, "eDP-", 4) == 0 ||
      strncmp(display->connector, "LVDS-", 5) == 0 ||
      strncmp(display->connector, "DSI-", 4) == 0) {
    snprintf(buffer, size, "Built-in Display");
    return;
  }
  const char *model = strncmp(display->model, "DELL ", 5) == 0
                          ? display->model + 5
                          : display->model;
  if (display->physical_width_mm > 0 && display->physical_height_mm > 0) {
    float diagonal =
        sqrtf((float)(display->physical_width_mm * display->physical_width_mm +
                      display->physical_height_mm *
                          display->physical_height_mm)) /
        MILLIMETERS_PER_INCH;
    snprintf(buffer, size, "%.0f\" %s", diagonal,
             model[0] != '\0' ? model : display->connector);
  } else {
    snprintf(buffer, size, "%s", model[0] != '\0' ? model : display->connector);
  }
}

static void draw_monitor(Ui *ui, const LayoutDisplay *display, Rect rectangle,
                         bool selected, size_t index) {
  fill_rounded(ui, rectangle, 7, ui->theme.monitor);
  stroke_rect(ui, rectangle, selected ? 3 : 2,
              selected ? ui->theme.accent : ui->theme.muted);
  int inset = (int)fmaxf(8.0F, (float)(rectangle.width < rectangle.height
                                           ? rectangle.width
                                           : rectangle.height) *
                                   MONITOR_INSET_FRACTION);
  Rect inner = {rectangle.x + inset, rectangle.y + inset,
                rectangle.width - inset * 2, rectangle.height - inset * 2};
  fill_rounded(ui, inner, 4, ui->theme.monitor_inner);
  stroke_rect(ui, inner, 1, selected ? ui->theme.accent : ui->theme.border);

  char name[DISPLAY_LAYOUT_NAME_MAX + 32];
  char details[256];
  friendly_name(display, name, sizeof(name));
  snprintf(details, sizeof(details), "%s  |  %d x %d", display->connector,
           display->width, display->height);
  int line_height = ui->font.ascent + ui->font.descent;
  Rect title = {inner.x, inner.y + inner.height / 2 - line_height, inner.width,
                line_height};
  Rect detail = {inner.x, title.y + line_height + 3, inner.width, line_height};
  draw_text_centered(ui, name, title, ui->theme.text);
  draw_text_centered(ui, details, detail,
                     selected ? ui->theme.accent : ui->theme.muted);

  int stand_width = rectangle.width / 6;
  if (stand_width > 82) {
    stand_width = 82;
  }
  fill_rounded(ui,
               (Rect){rectangle.x + (rectangle.width - stand_width) / 2,
                      rectangle.y + rectangle.height - inset, stand_width, 5},
               3, selected ? ui->theme.accent : ui->theme.muted);

  Rect badge = {rectangle.x + UI_BADGE_MARGIN, rectangle.y + UI_BADGE_MARGIN,
                UI_BADGE_SIZE, UI_BADGE_SIZE};
  fill_rounded(ui, badge, UI_BADGE_SIZE / 2, ui->theme.accent);
  char number[16];
  snprintf(number, sizeof(number), "%u", (unsigned int)index + 1U);
  draw_text_centered(ui, number, badge, ui->theme.white);
}

static SnapResult nearest_snap(int value, int size, const DisplayList *list,
                               size_t moving, bool horizontal,
                               float threshold) {
  SnapResult result = {.value = value};
  float best_distance = threshold + 1.0F;
  int moving_points[3] = {value, value + size / 2, value + size};
  for (size_t index = 0; index < list->count; index++) {
    if (index == moving) {
      continue;
    }
    const LayoutDisplay *other = &list->displays[index];
    int start = horizontal ? other->x : other->y;
    int other_size = horizontal ? other->width : other->height;
    int target_points[3] = {start, start + other_size / 2, start + other_size};
    for (int moving_point = 0; moving_point < 3; moving_point++) {
      for (int target_point = 0; target_point < 3; target_point++) {
        bool centers = moving_point == 1 && target_point == 1;
        bool edges = moving_point != 1 && target_point != 1;
        if (!centers && !edges) {
          continue;
        }
        float distance = fabsf(
            (float)(moving_points[moving_point] - target_points[target_point]));
        if (distance <= threshold && distance < best_distance) {
          int offset =
              moving_point == 0 ? 0 : (moving_point == 1 ? size / 2 : size);
          result.value = target_points[target_point] - offset;
          result.guide_coordinate = target_points[target_point];
          result.snapped = true;
          result.centerline = centers;
          best_distance = distance;
        }
      }
    }
  }
  return result;
}

static void draw_grid(Ui *ui, Rect canvas) {
  for (int x = canvas.x; x <= canvas.x + canvas.width; x += 24)
    draw_line(ui, x, canvas.y, x, canvas.y + canvas.height, 1, false,
              ui->theme.grid);
  for (int y = canvas.y; y <= canvas.y + canvas.height; y += 24)
    draw_line(ui, canvas.x, y, canvas.x + canvas.width, y, 1, false,
              ui->theme.grid);
}

static void draw_guide(Ui *ui, ViewTransform transform, SnapResult snap,
                       bool vertical) {
  int coordinate =
      vertical ? (int)roundf(transform.origin_x +
                             (float)snap.guide_coordinate * transform.scale)
               : (int)roundf(transform.origin_y +
                             (float)snap.guide_coordinate * transform.scale);
  if (vertical)
    draw_line(ui, coordinate, transform.canvas.y, coordinate,
              transform.canvas.y + transform.canvas.height,
              snap.centerline ? 2 : 1, true, ui->theme.accent);
  else
    draw_line(ui, transform.canvas.x, coordinate,
              transform.canvas.x + transform.canvas.width, coordinate,
              snap.centerline ? 2 : 1, true, ui->theme.accent);
}

static bool draw_button(Ui *ui, Rect rectangle, const char *label, bool primary,
                        bool focused, int mouse_x, int mouse_y, bool released) {
  bool hovered = point_in_rect(mouse_x, mouse_y, rectangle);
  fill_rounded(ui, rectangle, 8,
               primary ? ui->theme.accent
                       : (hovered ? ui->theme.button_hover : ui->theme.button));
  if (!primary || focused) {
    stroke_rect(ui, rectangle, focused ? 2 : 1,
                focused ? ui->theme.accent : ui->theme.border);
  }
  draw_text_centered(ui, label, rectangle,
                     primary ? ui->theme.white : ui->theme.text);
  return hovered && released;
}

static void fill_circle(Ui *ui, int center_x, int center_y, int radius,
                        UiColor color_value) {
  for (int y = -radius; y <= radius; y++) {
    int extent = (int)sqrt((double)(radius * radius - y * y));
    fill_rect(ui, (Rect){center_x - extent, center_y + y, extent * 2 + 1, 1},
              color_value);
  }
}

static bool draw_close_button(Ui *ui, int center_x, int center_y, bool focused,
                              int mouse_x, int mouse_y, bool released) {
  int radius = 16;
  int dx = mouse_x - center_x;
  int dy = mouse_y - center_y;
  bool hovered = dx * dx + dy * dy <= radius * radius;
  fill_circle(ui, center_x, center_y, radius,
              hovered ? ui->theme.button_hover : ui->theme.button);
  if (focused) {
    for (int angle = 0; angle < 360; angle++) {
      double radians = (double)angle * 3.14159265358979323846 / 180.0;
      put_pixel(ui, center_x + (int)round(cos(radians) * radius),
                center_y + (int)round(sin(radians) * radius), ui->theme.accent);
    }
  }
  draw_line(ui, center_x - 5, center_y - 5, center_x + 5, center_y + 5, 2,
            false, ui->theme.muted);
  draw_line(ui, center_x + 5, center_y - 5, center_x - 5, center_y + 5, 2,
            false, ui->theme.muted);
  return hovered && released;
}

static bool file_contains(const char *path, const char *needle) {
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return false;
  }
  char line[512];
  bool found = false;
  while (!found && fgets(line, sizeof(line), file) != NULL) {
    found = strstr(line, needle) != NULL;
  }
  fclose(file);
  return found;
}

static bool system_prefers_light(void) {
  const char *theme = getenv("GTK_THEME");
  if (theme != NULL) {
    return strstr(theme, "dark") == NULL && strstr(theme, "Dark") == NULL;
  }
  const char *scheme = getenv("COLOR_SCHEME");
  if (scheme != NULL) {
    return strcmp(scheme, "light") == 0 || strcmp(scheme, "prefer-light") == 0;
  }
  const char *home = getenv("HOME");
  if (home != NULL) {
    const char *relative_paths[] = {"/.config/gtk-4.0/settings.ini",
                                    "/.config/gtk-3.0/settings.ini"};
    char path[1024];
    for (size_t index = 0; index < 2; index++) {
      snprintf(path, sizeof(path), "%s%s", home, relative_paths[index]);
      if (file_contains(path, "gtk-application-prefer-dark-theme=0")) {
        return true;
      }
      if (file_contains(path, "gtk-application-prefer-dark-theme=1") ||
          file_contains(path, "dark") || file_contains(path, "Dark")) {
        return false;
      }
    }
  }
  return false;
}

static const char *resolve_font_path(const AppConfig *config, char *buffer,
                                     size_t size) {
  if (config->font_path[0] != '\0') {
    return config->font_path;
  }
  char executable[1024];
  ssize_t length =
      readlink("/proc/self/exe", executable, sizeof(executable) - 1);
  if (length > 0) {
    executable[length] = '\0';
    char *slash = strrchr(executable, '/');
    if (slash != NULL) {
      *slash = '\0';
      slash = strrchr(executable, '/');
      if (slash != NULL) {
        *slash = '\0';
        const char *suffix = "/share/display-layout/DejaVuSansMono.ttf";
        size_t prefix_length = strlen(executable);
        size_t suffix_length = strlen(suffix);
        if (prefix_length + suffix_length + 1 <= size) {
          memcpy(buffer, executable, prefix_length);
          memcpy(buffer + prefix_length, suffix, suffix_length + 1);
          if (access(buffer, R_OK) == 0) {
            return buffer;
          }
        }
      }
    }
  }
  return "assets/DejaVuSansMono.ttf";
}

static void print_usage(FILE *stream) {
  fprintf(
      stream,
      "Usage: display-layout [--backend NAME] [--config PATH] [--identify]\n"
      "       display-layout --help | --version\n");
}

int main(int argc, char **argv) {
  AppConfig config;
  config_defaults(&config);
  char config_path[1024];
  const char *requested_config =
      config_default_path(config_path, sizeof(config_path));
  const char *backend_override = NULL;
  bool identify_only = false;
  for (int index = 1; index < argc; index++) {
    if (strcmp(argv[index], "--help") == 0 || strcmp(argv[index], "-h") == 0) {
      print_usage(stdout);
      return 0;
    }
    if (strcmp(argv[index], "--version") == 0) {
      printf("display-layout %s\n", DISPLAY_LAYOUT_VERSION);
      return 0;
    }
    if (strcmp(argv[index], "--config") == 0 && index + 1 < argc) {
      requested_config = argv[++index];
    } else if (strcmp(argv[index], "--backend") == 0 && index + 1 < argc) {
      backend_override = argv[++index];
    } else if (strcmp(argv[index], "--identify") == 0) {
      identify_only = true;
    } else {
      print_usage(stderr);
      return 2;
    }
  }

  char error[512] = {0};
  if (requested_config != NULL &&
      config_load(&config, requested_config, error, sizeof(error)) != 0) {
    fprintf(stderr, "display-layout: %s\n", error);
    return 1;
  }
  if (backend_override != NULL) {
    snprintf(config.backend, sizeof(config.backend), "%s", backend_override);
  }

  DisplayBackend backend;
  if (backend_open(config.backend, &backend, error, sizeof(error)) != 0) {
    fprintf(stderr, "display-layout: %s\n", error);
    return 1;
  }
  DisplayList displays;
  if (backend.ops->load(&backend, &displays, error, sizeof(error)) != 0) {
    fprintf(stderr, "display-layout: %s\n", error);
    backend_close(&backend);
    return 1;
  }
  DisplayList original = displays;
  DisplayList drag_view = displays;
  if (identify_only) {
    int result =
        backend.ops->identify != NULL
            ? backend.ops->identify(&backend, &displays,
                                    (unsigned int)config.identify_duration_ms,
                                    error, sizeof(error))
            : -1;
    if (result != 0) {
      fprintf(stderr, "display-layout: %s\n",
              error[0] != '\0' ? error : "backend cannot identify displays");
    }
    backend_close(&backend);
    return result == 0 ? 0 : 1;
  }

  int width = configured_dimension(config.width, 1920, 720);
  int height = configured_dimension(config.height, 1080, 520);
  Ui ui;
  if (ui_init(&ui, width, height, error, sizeof(error)) != 0) {
    fprintf(stderr, "display-layout: %s\n", error);
    ui_destroy(&ui);
    backend_close(&backend);
    return 1;
  }
  char font_path[1024];
  const char *resolved_font =
      resolve_font_path(&config, font_path, sizeof(font_path));
  if (load_font(&ui, resolved_font, config.font_size) != 0) {
    fprintf(stderr, "display-layout: cannot load vector font: %s\n",
            resolved_font);
    ui_destroy(&ui);
    backend_close(&backend);
    return 1;
  }
  ui.theme = (config.theme == THEME_LIGHT ||
              (config.theme == THEME_SYSTEM && system_prefers_light()))
                 ? light_theme(&ui)
                 : dark_theme(&ui);

  int selected = displays.count > 0 ? 0 : -1;
  bool dragging = false;
  int drag_mouse_x = 0;
  int drag_mouse_y = 0;
  int drag_display_x = 0;
  int drag_display_y = 0;
  SnapResult horizontal_snap = {0};
  SnapResult vertical_snap = {0};
  bool running = true;
  bool redraw = true;
  int focused_control = 2;

  while (running) {
    if (!redraw && !ui.needs_redraw) {
      if (wl_display_dispatch(ui.display) < 0) {
        fprintf(stderr,
                "display-layout: Wayland compositor closed the connection\n");
        break;
      }
    } else {
      wl_display_dispatch_pending(ui.display);
    }
    if (ui.close_requested)
      running = false;

    int mouse_x = ui.mouse_x;
    int mouse_y = ui.mouse_y;
    bool mouse_released = ui.pointer_released;
    bool keyboard_activate = false;
    redraw = redraw || ui.needs_redraw;

    if (ui.pointer_pressed) {
      int margin = UI_MARGIN;
      int button_y = ui.height - UI_BUTTON_HEIGHT - UI_BUTTON_BOTTOM_MARGIN;
      Rect identify_control = {margin, button_y, 114, UI_BUTTON_HEIGHT};
      Rect apply_control = {ui.width - margin - UI_BUTTON_WIDTH, button_y,
                            UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
      Rect reset_control = {apply_control.x - UI_BUTTON_WIDTH - UI_BUTTON_GAP,
                            button_y, UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
      int close_dx = mouse_x - (ui.width - UI_CLOSE_CENTER);
      int close_dy = mouse_y - UI_CLOSE_CENTER;
      if (point_in_rect(mouse_x, mouse_y, identify_control))
        focused_control = 0;
      else if (point_in_rect(mouse_x, mouse_y, reset_control))
        focused_control = 1;
      else if (point_in_rect(mouse_x, mouse_y, apply_control))
        focused_control = 2;
      else if (close_dx * close_dx + close_dy * close_dy <=
               UI_CLOSE_RADIUS * UI_CLOSE_RADIUS)
        focused_control = 3;

      Rect canvas = {margin, UI_HEADER_HEIGHT, ui.width - margin * 2,
                     ui.height - UI_HEADER_HEIGHT - UI_FOOTER_HEIGHT};
      ViewTransform transform = view_transform(&displays, canvas);
      if (point_in_rect(mouse_x, mouse_y, canvas)) {
        for (int index = (int)displays.count - 1; index >= 0; index--) {
          if (point_in_rect(
                  mouse_x, mouse_y,
                  display_rect(&displays.displays[index], transform))) {
            selected = index;
            dragging = true;
            drag_view = displays;
            drag_mouse_x = mouse_x;
            drag_mouse_y = mouse_y;
            drag_display_x = displays.displays[index].x;
            drag_display_y = displays.displays[index].y;
            break;
          }
        }
      }
    }
    if (ui.pointer_released)
      dragging = false;

    if (ui.key_pressed) {
      if (ui.key == XKB_KEY_Escape) {
        running = false;
      } else if (ui.key == XKB_KEY_Tab) {
        int direction = ui.shift_down ? -1 : 1;
        focused_control = (focused_control + direction + 4) % 4;
      } else if (ui.key == XKB_KEY_Return || ui.key == XKB_KEY_KP_Enter ||
                 ui.key == XKB_KEY_space) {
        keyboard_activate = true;
      } else if ((ui.key == XKB_KEY_r || ui.key == XKB_KEY_R) &&
                 ui.control_down) {
        displays = original;
      }
      redraw = true;
    }

    int margin = UI_MARGIN;
    Rect canvas = {margin, UI_HEADER_HEIGHT, ui.width - margin * 2,
                   ui.height - UI_HEADER_HEIGHT - UI_FOOTER_HEIGHT};
    ViewTransform transform =
        view_transform(dragging ? &drag_view : &displays, canvas);
    horizontal_snap.snapped = false;
    vertical_snap.snapped = false;
    if (dragging && selected >= 0) {
      LayoutDisplay *display = &displays.displays[selected];
      int x = drag_display_x +
              (int)roundf((float)(mouse_x - drag_mouse_x) / transform.scale);
      int y = drag_display_y +
              (int)roundf((float)(mouse_y - drag_mouse_y) / transform.scale);
      float threshold = (float)config.snap_distance / transform.scale;
      horizontal_snap = nearest_snap(x, display->width, &displays,
                                     (size_t)selected, true, threshold);
      vertical_snap = nearest_snap(y, display->height, &displays,
                                   (size_t)selected, false, threshold);
      display->x = horizontal_snap.value;
      display->y = vertical_snap.value;
      redraw = true;
    }

    if (redraw && running) {
      if (begin_frame(&ui) != 0) {
        fprintf(stderr,
                "display-layout: cannot acquire Wayland frame buffer\n");
        break;
      }
      fill_rect(&ui, (Rect){0, 0, ui.width, ui.height}, ui.theme.surface);
      bool close_clicked = draw_close_button(&ui, ui.width - UI_CLOSE_CENTER,
                                             24, focused_control == 3, mouse_x,
                                             mouse_y, mouse_released);
      close_clicked =
          close_clicked || (keyboard_activate && focused_control == 3);

      fill_rounded(&ui, canvas, 12, ui.theme.canvas);
      stroke_rect(&ui, canvas, 1, ui.theme.border);
      draw_grid(&ui, canvas);
      for (size_t index = 0; index < displays.count; index++) {
        if ((int)index != selected) {
          draw_monitor(&ui, &displays.displays[index],
                       display_rect(&displays.displays[index], transform),
                       false, index);
        }
      }
      if (selected >= 0) {
        draw_monitor(&ui, &displays.displays[selected],
                     display_rect(&displays.displays[selected], transform),
                     true, (size_t)selected);
      }
      if (dragging && horizontal_snap.snapped)
        draw_guide(&ui, transform, horizontal_snap, true);
      if (dragging && vertical_snap.snapped)
        draw_guide(&ui, transform, vertical_snap, false);

      int button_y = ui.height - UI_BUTTON_HEIGHT - UI_BUTTON_BOTTOM_MARGIN;
      Rect identify_button = {margin, button_y, 114, UI_BUTTON_HEIGHT};
      Rect apply_button = {ui.width - margin - UI_BUTTON_WIDTH, button_y,
                           UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
      Rect reset_button = {apply_button.x - UI_BUTTON_WIDTH - UI_BUTTON_GAP,
                           button_y, UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
      bool identify_clicked =
          draw_button(&ui, identify_button, "Identify", false,
                      focused_control == 0, mouse_x, mouse_y, mouse_released);
      bool reset_clicked =
          draw_button(&ui, reset_button, "Reset", false, focused_control == 1,
                      mouse_x, mouse_y, mouse_released);
      bool apply_clicked =
          draw_button(&ui, apply_button, "Apply", true, focused_control == 2,
                      mouse_x, mouse_y, mouse_released);
      identify_clicked =
          identify_clicked || (keyboard_activate && focused_control == 0);
      reset_clicked =
          reset_clicked || (keyboard_activate && focused_control == 1);
      apply_clicked =
          apply_clicked || (keyboard_activate && focused_control == 2);
      draw_line(&ui, 0, ui.height - UI_FOOTER_HEIGHT, ui.width,
                ui.height - UI_FOOTER_HEIGHT, 1, false, ui.theme.border);

      if (close_clicked)
        running = false;
      if (identify_clicked && backend.ops->identify != NULL &&
          backend.ops->identify(&backend, &displays,
                                (unsigned int)config.identify_duration_ms,
                                error, sizeof(error)) != 0)
        fprintf(stderr, "display-layout: %s\n", error);
      if (reset_clicked)
        displays = original;
      if (apply_clicked) {
        if (backend.ops->apply(&backend, &displays, error, sizeof(error)) == 0)
          running = false;
        else
          fprintf(stderr, "display-layout: %s\n", error);
      }
      present_frame(&ui);
      redraw = false;
    }

    ui.pointer_pressed = false;
    ui.pointer_released = false;
    ui.key_pressed = false;
    ui.needs_redraw = false;
  }

  ui_destroy(&ui);
  backend_close(&backend);
  return 0;
}
