#ifndef _SWAY_CAIRO_UTIL_H
#define _SWAY_CAIRO_UTIL_H

#include <stdint.h>
#include <cairo.h>
#include <wayland-client.h>

void cairo_set_source_u32(cairo_t *cairo, uint32_t color);

#endif
