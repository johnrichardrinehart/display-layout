#define _POSIX_C_SOURCE 200809L

#include "backend.h"
#include "config.h"
#include "model.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xrender.h>
#include <X11/keysym.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "third_party/stb_truetype.h"

#ifndef DISPLAY_LAYOUT_VERSION
#define DISPLAY_LAYOUT_VERSION "development"
#endif

static const double PI = 3.14159265358979323846;
static const double DEGREES_TO_RADIANS = PI / 180.0;
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
  ROUNDED_CORNER_STEPS = 8,
};

typedef struct {
  int x;
  int y;
  int width;
  int height;
} Rect;

typedef struct {
  unsigned long pixel;
  Picture picture;
} UiColor;

typedef struct {
  Pixmap atlas_pixmap;
  Picture atlas_picture;
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
  Display *x_display;
  int screen;
  Window window;
  Pixmap buffer;
  Drawable target;
  Picture target_picture;
  Visual *visual;
  Colormap colormap;
  GC gc;
  FontRenderer font;
  Cursor normal_cursor;
  Cursor hand_cursor;
  Theme theme;
  int width;
  int height;
} Ui;

static void resize_buffer(Ui *ui, int width, int height) {
  if (ui->target_picture != 0) {
    XRenderFreePicture(ui->x_display, ui->target_picture);
  }
  if (ui->gc != 0) {
    XFreeGC(ui->x_display, ui->gc);
  }
  if (ui->buffer != 0) {
    XFreePixmap(ui->x_display, ui->buffer);
  }
  ui->width = width;
  ui->height = height;
  ui->buffer = XCreatePixmap(
      ui->x_display, ui->window, (unsigned int)width, (unsigned int)height,
      (unsigned int)DefaultDepth(ui->x_display, ui->screen));
  ui->target = ui->buffer;
  ui->gc = XCreateGC(ui->x_display, ui->buffer, 0, NULL);
  XRenderPictFormat *format =
      XRenderFindVisualFormat(ui->x_display, ui->visual);
  ui->target_picture =
      XRenderCreatePicture(ui->x_display, ui->buffer, format, 0, NULL);
}

static UiColor color(Ui *ui, unsigned short red, unsigned short green,
                     unsigned short blue) {
  XColor xcolor = {.red = red,
                   .green = green,
                   .blue = blue,
                   .flags = DoRed | DoGreen | DoBlue};
  XAllocColor(ui->x_display, ui->colormap, &xcolor);
  XRenderColor render = {
      .red = red, .green = green, .blue = blue, .alpha = 65535};
  return (UiColor){.pixel = xcolor.pixel,
                   .picture = XRenderCreateSolidFill(ui->x_display, &render)};
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

static void set_gc_color(Ui *ui, UiColor color_value) {
  XSetForeground(ui->x_display, ui->gc, color_value.pixel);
}

static void fill_rect(Ui *ui, Rect rectangle, UiColor fill) {
  set_gc_color(ui, fill);
  XFillRectangle(ui->x_display, ui->target, ui->gc, rectangle.x, rectangle.y,
                 (unsigned int)rectangle.width, (unsigned int)rectangle.height);
}

static void fill_rounded(Ui *ui, Rect rectangle, int radius, UiColor fill) {
  if (radius <= 0 || rectangle.width < radius * 2 ||
      rectangle.height < radius * 2) {
    fill_rect(ui, rectangle, fill);
    return;
  }
  XPointDouble points[4 * (ROUNDED_CORNER_STEPS + 1)];
  int count = 0;
  const double centers[4][3] = {
      {rectangle.x + rectangle.width - radius, rectangle.y + radius, -90.0},
      {rectangle.x + rectangle.width - radius,
       rectangle.y + rectangle.height - radius, 0.0},
      {rectangle.x + radius, rectangle.y + rectangle.height - radius, 90.0},
      {rectangle.x + radius, rectangle.y + radius, 180.0},
  };
  for (int corner = 0; corner < 4; corner++) {
    for (int step = 0; step <= ROUNDED_CORNER_STEPS; step++) {
      double angle = (centers[corner][2] +
                      (double)step * (90.0 / (double)ROUNDED_CORNER_STEPS)) *
                     DEGREES_TO_RADIANS;
      points[count++] = (XPointDouble){
          .x = centers[corner][0] + cos(angle) * (double)radius,
          .y = centers[corner][1] + sin(angle) * (double)radius,
      };
    }
  }
  XRenderPictFormat *mask =
      XRenderFindStandardFormat(ui->x_display, PictStandardA8);
  XRenderCompositeDoublePoly(ui->x_display, PictOpOver, fill.picture,
                             ui->target_picture, mask, 0, 0, 0, 0, points,
                             count, EvenOddRule);
}

static void stroke_rect(Ui *ui, Rect rectangle, int width, UiColor stroke) {
  set_gc_color(ui, stroke);
  XSetLineAttributes(ui->x_display, ui->gc, (unsigned int)width, LineSolid,
                     CapRound, JoinRound);
  XDrawRectangle(ui->x_display, ui->target, ui->gc, rectangle.x, rectangle.y,
                 (unsigned int)(rectangle.width - 1),
                 (unsigned int)(rectangle.height - 1));
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

  ui->font.atlas_pixmap = XCreatePixmap(ui->x_display, ui->window,
                                        FONT_ATLAS_SIZE, FONT_ATLAS_SIZE, 8);
  GC atlas_gc = XCreateGC(ui->x_display, ui->font.atlas_pixmap, 0, NULL);
  XImage *image =
      XCreateImage(ui->x_display, ui->visual, 8, ZPixmap, 0, (char *)bitmap,
                   FONT_ATLAS_SIZE, FONT_ATLAS_SIZE, 8, FONT_ATLAS_SIZE);
  XPutImage(ui->x_display, ui->font.atlas_pixmap, atlas_gc, image, 0, 0, 0, 0,
            FONT_ATLAS_SIZE, FONT_ATLAS_SIZE);
  image->data = NULL;
  XDestroyImage(image);
  free(bitmap);
  XFreeGC(ui->x_display, atlas_gc);
  XRenderPictFormat *alpha_format =
      XRenderFindStandardFormat(ui->x_display, PictStandardA8);
  ui->font.atlas_picture = XRenderCreatePicture(
      ui->x_display, ui->font.atlas_pixmap, alpha_format, 0, NULL);
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
    XRenderComposite(ui->x_display, PictOpOver, color_value.picture,
                     ui->font.atlas_picture, ui->target_picture, 0, 0,
                     glyph->x0, glyph->y0, (int)roundf(cursor + glyph->xoff),
                     (int)roundf((float)baseline + glyph->yoff),
                     (unsigned int)width, (unsigned int)height);
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
  set_gc_color(ui, ui->theme.grid);
  for (int x = canvas.x; x <= canvas.x + canvas.width; x += 24) {
    XDrawLine(ui->x_display, ui->target, ui->gc, x, canvas.y, x,
              canvas.y + canvas.height);
  }
  for (int y = canvas.y; y <= canvas.y + canvas.height; y += 24) {
    XDrawLine(ui->x_display, ui->target, ui->gc, canvas.x, y,
              canvas.x + canvas.width, y);
  }
}

static void draw_guide(Ui *ui, ViewTransform transform, SnapResult snap,
                       bool vertical) {
  int coordinate =
      vertical ? (int)roundf(transform.origin_x +
                             (float)snap.guide_coordinate * transform.scale)
               : (int)roundf(transform.origin_y +
                             (float)snap.guide_coordinate * transform.scale);
  set_gc_color(ui, ui->theme.accent);
  XSetLineAttributes(ui->x_display, ui->gc, snap.centerline ? 2U : 1U,
                     LineOnOffDash, CapRound, JoinRound);
  char dash[] = {6, 5};
  XSetDashes(ui->x_display, ui->gc, 0, dash, 2);
  if (vertical) {
    XDrawLine(ui->x_display, ui->target, ui->gc, coordinate, transform.canvas.y,
              coordinate, transform.canvas.y + transform.canvas.height);
  } else {
    XDrawLine(ui->x_display, ui->target, ui->gc, transform.canvas.x, coordinate,
              transform.canvas.x + transform.canvas.width, coordinate);
  }
  XSetLineAttributes(ui->x_display, ui->gc, 1U, LineSolid, CapRound, JoinRound);
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

static bool draw_close_button(Ui *ui, int center_x, int center_y, bool focused,
                              int mouse_x, int mouse_y, bool released) {
  int radius = 16;
  int dx = mouse_x - center_x;
  int dy = mouse_y - center_y;
  bool hovered = dx * dx + dy * dy <= radius * radius;
  set_gc_color(ui, hovered ? ui->theme.button_hover : ui->theme.button);
  XFillArc(ui->x_display, ui->target, ui->gc, center_x - radius,
           center_y - radius, (unsigned int)(radius * 2),
           (unsigned int)(radius * 2), 0, 360 * 64);
  if (focused) {
    set_gc_color(ui, ui->theme.accent);
    XSetLineAttributes(ui->x_display, ui->gc, 2U, LineSolid, CapRound,
                       JoinRound);
    XDrawArc(ui->x_display, ui->target, ui->gc, center_x - radius,
             center_y - radius, (unsigned int)(radius * 2),
             (unsigned int)(radius * 2), 0, 360 * 64);
  }
  set_gc_color(ui, ui->theme.muted);
  XSetLineAttributes(ui->x_display, ui->gc, 2U, LineSolid, CapRound, JoinRound);
  XDrawLine(ui->x_display, ui->target, ui->gc, center_x - 5, center_y - 5,
            center_x + 5, center_y + 5);
  XDrawLine(ui->x_display, ui->target, ui->gc, center_x + 5, center_y - 5,
            center_x - 5, center_y + 5);
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

static void configure_dialog_window(Ui *ui) {
  typedef struct {
    unsigned long flags;
    unsigned long functions;
    unsigned long decorations;
    long input_mode;
    unsigned long status;
  } MotifHints;
  MotifHints motif = {.flags = 2, .decorations = 0};
  Atom motif_property = XInternAtom(ui->x_display, "_MOTIF_WM_HINTS", False);
  XChangeProperty(ui->x_display, ui->window, motif_property, motif_property, 32,
                  PropModeReplace, (unsigned char *)&motif, 5);

  Atom window_type = XInternAtom(ui->x_display, "_NET_WM_WINDOW_TYPE", False);
  Atom dialog_type =
      XInternAtom(ui->x_display, "_NET_WM_WINDOW_TYPE_DIALOG", False);
  XChangeProperty(ui->x_display, ui->window, window_type, XA_ATOM, 32,
                  PropModeReplace, (unsigned char *)&dialog_type, 1);
  XSetTransientForHint(ui->x_display, ui->window,
                       RootWindow(ui->x_display, ui->screen));

  XSizeHints size_hints = {
      .flags = PMinSize | PMaxSize,
      .min_width = ui->width,
      .min_height = ui->height,
      .max_width = ui->width,
      .max_height = ui->height,
  };
  XSetWMNormalHints(ui->x_display, ui->window, &size_hints);
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

  Ui ui = {0};
  ui.x_display = XOpenDisplay(NULL);
  if (ui.x_display == NULL) {
    fprintf(stderr, "display-layout: cannot open X display\n");
    backend_close(&backend);
    return 1;
  }
  ui.screen = DefaultScreen(ui.x_display);
  ui.visual = DefaultVisual(ui.x_display, ui.screen);
  ui.colormap = DefaultColormap(ui.x_display, ui.screen);
  int screen_width = DisplayWidth(ui.x_display, ui.screen);
  int screen_height = DisplayHeight(ui.x_display, ui.screen);
  ui.width = configured_dimension(config.width, screen_width, 720);
  ui.height = configured_dimension(config.height, screen_height, 520);
  ui.window = XCreateSimpleWindow(
      ui.x_display, RootWindow(ui.x_display, ui.screen),
      (screen_width - ui.width) / 2, (screen_height - ui.height) / 2,
      (unsigned int)ui.width, (unsigned int)ui.height, 0, 0, 0);
  XStoreName(ui.x_display, ui.window, "Display Layout Editor");
  XClassHint class_hint = {.res_name = "display-layout-editor",
                           .res_class = "DisplayLayoutEditor"};
  XSetClassHint(ui.x_display, ui.window, &class_hint);
  configure_dialog_window(&ui);
  XSelectInput(ui.x_display, ui.window,
               ExposureMask | ButtonPressMask | ButtonReleaseMask |
                   PointerMotionMask | KeyPressMask | StructureNotifyMask);
  Atom wm_delete = XInternAtom(ui.x_display, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(ui.x_display, ui.window, &wm_delete, 1);
  resize_buffer(&ui, ui.width, ui.height);
  char font_path[1024];
  const char *resolved_font =
      resolve_font_path(&config, font_path, sizeof(font_path));
  if (load_font(&ui, resolved_font, config.font_size) != 0) {
    fprintf(stderr, "display-layout: cannot load vector font: %s\n",
            resolved_font);
    XCloseDisplay(ui.x_display);
    backend_close(&backend);
    return 1;
  }
  ui.normal_cursor = XCreateFontCursor(ui.x_display, XC_left_ptr);
  ui.hand_cursor = XCreateFontCursor(ui.x_display, XC_hand2);
  XDefineCursor(ui.x_display, ui.window, ui.normal_cursor);
  ui.theme = (config.theme == THEME_LIGHT ||
              (config.theme == THEME_SYSTEM && system_prefers_light()))
                 ? light_theme(&ui)
                 : dark_theme(&ui);
  XMapRaised(ui.x_display, ui.window);

  int selected = displays.count > 0 ? 0 : -1;
  bool dragging = false;
  int drag_mouse_x = 0;
  int drag_mouse_y = 0;
  int drag_display_x = 0;
  int drag_display_y = 0;
  int mouse_x = -1;
  int mouse_y = -1;
  bool mouse_released = false;
  SnapResult horizontal_snap = {0};
  SnapResult vertical_snap = {0};
  bool running = true;
  bool redraw = true;
  int focused_control = 2;
  bool keyboard_activate = false;

  while (running) {
    mouse_released = false;
    keyboard_activate = false;
    while (XPending(ui.x_display) > 0) {
      XEvent event;
      XNextEvent(ui.x_display, &event);
      if (event.type == Expose) {
        redraw = true;
      } else if (event.type == ConfigureNotify) {
        if (ui.width != event.xconfigure.width ||
            ui.height != event.xconfigure.height) {
          resize_buffer(&ui, event.xconfigure.width, event.xconfigure.height);
        }
        redraw = true;
      } else if (event.type == MotionNotify) {
        mouse_x = event.xmotion.x;
        mouse_y = event.xmotion.y;
        redraw = true;
      } else if (event.type == ButtonPress && event.xbutton.button == Button1) {
        mouse_x = event.xbutton.x;
        mouse_y = event.xbutton.y;
        int margin = UI_MARGIN;
        int button_y = ui.height - UI_BUTTON_HEIGHT - UI_BUTTON_BOTTOM_MARGIN;
        Rect identify_control = {margin, button_y, 114, UI_BUTTON_HEIGHT};
        Rect apply_control = {ui.width - margin - UI_BUTTON_WIDTH, button_y,
                              UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
        Rect reset_control = {apply_control.x - UI_BUTTON_WIDTH - UI_BUTTON_GAP,
                              button_y, UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
        int close_dx = mouse_x - (ui.width - UI_CLOSE_CENTER);
        int close_dy = mouse_y - UI_CLOSE_CENTER;
        if (point_in_rect(mouse_x, mouse_y, identify_control)) {
          focused_control = 0;
        } else if (point_in_rect(mouse_x, mouse_y, reset_control)) {
          focused_control = 1;
        } else if (point_in_rect(mouse_x, mouse_y, apply_control)) {
          focused_control = 2;
        } else if (close_dx * close_dx + close_dy * close_dy <=
                   UI_CLOSE_RADIUS * UI_CLOSE_RADIUS) {
          focused_control = 3;
        }
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
        redraw = true;
      } else if (event.type == ButtonRelease &&
                 event.xbutton.button == Button1) {
        mouse_x = event.xbutton.x;
        mouse_y = event.xbutton.y;
        dragging = false;
        mouse_released = true;
        redraw = true;
      } else if (event.type == KeyPress) {
        KeySym key = XLookupKeysym(&event.xkey, 0);
        if (key == XK_Escape) {
          running = false;
        } else if (key == XK_Tab) {
          int direction = (event.xkey.state & ShiftMask) != 0 ? -1 : 1;
          focused_control = (focused_control + direction + 4) % 4;
          redraw = true;
        } else if (key == XK_Return || key == XK_KP_Enter || key == XK_space) {
          keyboard_activate = true;
          redraw = true;
        } else if ((key == XK_r || key == XK_R) &&
                   (event.xkey.state & ControlMask) != 0) {
          displays = original;
          redraw = true;
        }
      } else if (event.type == ClientMessage &&
                 (Atom)event.xclient.data.l[0] == wm_delete) {
        running = false;
      }
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

    if (redraw) {
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
      if (dragging && horizontal_snap.snapped) {
        draw_guide(&ui, transform, horizontal_snap, true);
      }
      if (dragging && vertical_snap.snapped) {
        draw_guide(&ui, transform, vertical_snap, false);
      }

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
      int close_dx = mouse_x - (ui.width - UI_CLOSE_CENTER);
      int close_dy = mouse_y - UI_CLOSE_CENTER;
      bool over_close = close_dx * close_dx + close_dy * close_dy <=
                        UI_CLOSE_RADIUS * UI_CLOSE_RADIUS;
      bool over_button = point_in_rect(mouse_x, mouse_y, identify_button) ||
                         point_in_rect(mouse_x, mouse_y, reset_button) ||
                         point_in_rect(mouse_x, mouse_y, apply_button);
      XDefineCursor(ui.x_display, ui.window,
                    over_close || over_button ? ui.hand_cursor
                                              : ui.normal_cursor);
      set_gc_color(&ui, ui.theme.border);
      XDrawLine(ui.x_display, ui.buffer, ui.gc, 0, ui.height - UI_FOOTER_HEIGHT,
                ui.width, ui.height - UI_FOOTER_HEIGHT);

      if (close_clicked) {
        running = false;
      }
      if (identify_clicked && backend.ops->identify != NULL &&
          backend.ops->identify(&backend, &displays,
                                (unsigned int)config.identify_duration_ms,
                                error, sizeof(error)) != 0) {
        fprintf(stderr, "display-layout: %s\n", error);
      }
      if (reset_clicked) {
        displays = original;
      }
      if (apply_clicked) {
        if (backend.ops->apply(&backend, &displays, error, sizeof(error)) ==
            0) {
          running = false;
        } else {
          fprintf(stderr, "display-layout: %s\n", error);
        }
      }
      XCopyArea(ui.x_display, ui.buffer, ui.window, ui.gc, 0, 0,
                (unsigned int)ui.width, (unsigned int)ui.height, 0, 0);
      XFlush(ui.x_display);
      redraw = dragging;
    }

    if (!redraw && running) {
      struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000L};
      nanosleep(&pause, NULL);
    }
  }

  XRenderFreePicture(ui.x_display, ui.font.atlas_picture);
  XFreePixmap(ui.x_display, ui.font.atlas_pixmap);
  XRenderFreePicture(ui.x_display, ui.target_picture);
  XFreeCursor(ui.x_display, ui.hand_cursor);
  XFreeCursor(ui.x_display, ui.normal_cursor);
  XFreeGC(ui.x_display, ui.gc);
  XFreePixmap(ui.x_display, ui.buffer);
  XDestroyWindow(ui.x_display, ui.window);
  XCloseDisplay(ui.x_display);
  backend_close(&backend);
  return 0;
}
