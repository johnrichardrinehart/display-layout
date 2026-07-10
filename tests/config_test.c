#include "config.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
  WindowDimension dimension;
  assert(config_parse_dimension("1060px", &dimension) == 0);
  assert(!dimension.percent && dimension.value == 1060.0F);
  assert(config_parse_dimension("88%", &dimension) == 0);
  assert(dimension.percent && dimension.value == 88.0F);
  assert(config_parse_dimension("0", &dimension) != 0);
  assert(config_parse_dimension("101%", &dimension) != 0);
  assert(config_parse_dimension("wide", &dimension) != 0);
  puts("config tests passed");
  return 0;
}
