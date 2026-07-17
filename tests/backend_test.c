#define _POSIX_C_SOURCE 200809L
#include "backend.h"
#include "backend_gnome.h"
#include "backend_hyprland.h"
#include "backend_kscreen.h"
#include "backend_niri.h"
#include "backend_sway.h"
#include "backend_wlr.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_niri(void) {
  const char *json =
      "{\"eDP-1\":{\"name\":\"eDP-1\",\"make\":\"BOE\",\"model\":\"Panel\","
      "\"physical_size\":[280,190],\"logical\":{\"x\":1920,\"y\":0,\"width\":"
      "1280,\"height\":800}},\"DP-1\":{\"name\":\"DP-1\",\"logical\":{\"x\":0,"
      "\"y\":0,\"width\":1920,\"height\":1080}}}";
  DisplayList list;
  char error[256] = {0};
  assert(niri_parse_outputs(json, &list, error, sizeof(error)) == 0);
  assert(list.count == 2);
  assert(strcmp(list.displays[0].connector, "DP-1") == 0);
  assert(list.displays[1].physical_width_mm == 280);
}
static void test_sway(void) {
  const char *json =
      "[{\"name\":\"eDP-1\",\"make\":\"BOE\",\"model\":\"Panel\",\"active\":"
      "true,\"physical_width\":280,\"physical_height\":190,\"rect\":{\"x\":"
      "1920,\"y\":0,\"width\":1280,\"height\":800}},{\"name\":\"DP-1\","
      "\"active\":true,\"rect\":{\"x\":0,\"y\":0,\"width\":1920,\"height\":"
      "1080}}]";
  DisplayList list;
  char error[256] = {0};
  assert(sway_parse_outputs(json, &list, error, sizeof(error)) == 0);
  assert(list.count == 2);
  assert(strcmp(list.displays[0].connector, "DP-1") == 0);
  assert(list.displays[1].width == 1280);
}
static void test_hyprland(void) {
  const char *json =
      "[{\"name\":\"DP-1\",\"make\":\"Dell\",\"model\":\"U2720Q\",\"width\":"
      "3840,\"height\":2160,\"refreshRate\":60.0,\"x\":0,\"y\":0,\"scale\":2.0,"
      "\"transform\":0,"
      "\"physicalWidth\":600,\"physicalHeight\":340,\"disabled\":false}]";
  DisplayList list;
  char error[256] = {0};
  assert(hyprland_parse_monitors(json, &list, error, sizeof(error)) == 0);
  assert(list.count == 1);
  assert(list.displays[0].width == 1920);
  assert(list.displays[0].height == 1080);
  assert(list.displays[0].transform == 0);
}
static void test_wlr(void) {
  const char *json =
      "[{\"name\":\"DP-1\",\"make\":\"Dell\",\"model\":\"U2720Q\",\"physical_"
      "size\":{\"width\":600,\"height\":340},\"enabled\":true,\"modes\":[{"
      "\"width\":3840,\"height\":2160,\"current\":true}],\"position\":{\"x\":0,"
      "\"y\":0},\"transform\":\"normal\",\"scale\":2.0}]";
  DisplayList list;
  char error[256] = {0};
  assert(wlr_parse_outputs(json, &list, error, sizeof(error)) == 0);
  assert(list.count == 1);
  assert(list.displays[0].width == 1920);
  assert(list.displays[0].physical_height_mm == 340);
}
static void test_kscreen(void) {
  const char *json =
      "{\"outputs\":[{\"name\":\"DP-1\",\"enabled\":true,\"pos\":{\"x\":0,"
      "\"y\":0},\"size\":{\"width\":3840,\"height\":2160},\"sizeMM\":{"
      "\"width\":600,\"height\":340},\"scale\":2.0}]}";
  DisplayList list;
  char error[256] = {0};
  assert(kscreen_parse_outputs(json, &list, error, sizeof(error)) == 0);
  assert(list.count == 1);
  assert(list.displays[0].width == 1920);
  assert(list.displays[0].physical_width_mm == 600);
}
static void test_gnome(void) {
  const char *text = "Monitors:\n"
                     "├──Monitor DP-1 (Dell 27 Inch)\n"
                     "│  ├──Vendor: Dell Inc.\n"
                     "│  ├──Product: U2720Q\n"
                     "│  └──Current mode\n"
                     "│      └──3840x2160@60.000\n"
                     "└──Monitor DP-2 (Laptop Panel)\n"
                     "   ├──Vendor: BOE\n"
                     "   ├──Product: Panel\n"
                     "   └──Current mode\n"
                     "       └──1920x1080@60.000\n"
                     "\nLogical monitors:\n"
                     "├──Logical monitor #1\n"
                     "│  ├──Position: (0, 0)\n"
                     "│  ├──Scale: 2.0\n"
                     "│  ├──Transform: normal\n"
                     "│  ├──Primary: yes\n"
                     "│  └──Monitors: (1)\n"
                     "│      └──DP-1 (Dell 27 Inch)\n"
                     "└──Logical monitor #2\n"
                     "   ├──Position: (1920, 20)\n"
                     "   ├──Scale: 1.0\n"
                     "   ├──Transform: normal\n"
                     "   ├──Primary: no\n"
                     "   └──Monitors: (1)\n"
                     "       └──DP-2 (Laptop Panel)\n";
  DisplayList list;
  char error[256] = {0};
  assert(gnome_parse_outputs(text, &list, error, sizeof(error)) == 0);
  assert(list.count == 2);
  assert(strcmp(list.displays[0].connector, "DP-1") == 0);
  assert(list.displays[0].width == 1920);
  assert(list.displays[0].primary);
  assert(list.displays[0].refresh_rate == 60.0f);
  assert(strcmp(list.displays[0].make, "Dell Inc.") == 0);
  assert(strcmp(list.displays[0].model, "U2720Q") == 0);
  assert(list.displays[1].x == 1920);
  assert(list.displays[1].height == 1080);
}
static void test_selection(void) {
  DisplayBackend backend;
  char error[256] = {0};
  assert(backend_open("river", &backend, error, sizeof(error)) == 0);
  assert(strcmp(backend.ops->name, "wlr") == 0);
  backend_close(&backend);
  setenv("XDG_CURRENT_DESKTOP", "GNOME", 1);
  unsetenv("NIRI_SOCKET");
  unsetenv("SWAYSOCK");
  unsetenv("HYPRLAND_INSTANCE_SIGNATURE");
  assert(backend_open("auto", &backend, error, sizeof(error)) == 0);
  assert(strcmp(backend.ops->name, "gnome") == 0);
  backend_close(&backend);
}
int main(void) {
  test_niri();
  test_sway();
  test_hyprland();
  test_wlr();
  test_kscreen();
  test_gnome();
  test_selection();
  puts("backend tests passed");
  return 0;
}
