#ifndef DISPLAY_LAYOUT_MODEL_H
#define DISPLAY_LAYOUT_MODEL_H

#include <stdbool.h>
#include <stddef.h>

#define DISPLAY_LAYOUT_MAX_DISPLAYS 16
#define DISPLAY_LAYOUT_NAME_MAX 128

typedef struct {
  char connector[DISPLAY_LAYOUT_NAME_MAX];
  char make[DISPLAY_LAYOUT_NAME_MAX];
  char model[DISPLAY_LAYOUT_NAME_MAX];
  int physical_width_mm;
  int physical_height_mm;
  int x;
  int y;
  int width;
  int height;
  bool enabled;
} LayoutDisplay;

typedef struct {
  LayoutDisplay displays[DISPLAY_LAYOUT_MAX_DISPLAYS];
  size_t count;
} DisplayList;

#endif
