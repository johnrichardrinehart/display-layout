#include "backend_niri.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  const char *json =
      "{"
      "\"eDP-1\":{"
      "\"name\":\"eDP-1\",\"make\":\"BOE\",\"model\":\"0x095F\","
      "\"physical_size\":[280,190],"
      "\"logical\":{\"x\":3840,\"y\":1800,\"width\":2256,\"height\":1504,"
      "\"scale\":1.0,\"transform\":\"Normal\"}},"
      "\"DP-5\":{"
      "\"name\":\"DP-5\",\"make\":\"Dell Inc.\",\"model\":\"DELL U3225QE\","
      "\"physical_size\":[700,390],"
      "\"logical\":{\"x\":0,\"y\":0,\"width\":3840,\"height\":2160,"
      "\"scale\":1.0,\"transform\":\"Normal\"}},"
      "\"DP-9\":{\"name\":\"DP-9\",\"logical\":null}"
      "}";
  DisplayList list;
  char error[256] = {0};
  assert(niri_parse_outputs(json, &list, error, sizeof(error)) == 0);
  assert(list.count == 2);
  assert(strcmp(list.displays[0].model, "DELL U3225QE") == 0);
  assert(list.displays[0].width == 3840);
  assert(strcmp(list.displays[1].connector, "eDP-1") == 0);
  assert(list.displays[1].x == 3840);
  assert(list.displays[1].physical_width_mm == 280);
  puts("niri backend tests passed");
  return 0;
}
