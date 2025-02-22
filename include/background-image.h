#ifndef _SWAY_BACKGROUND_IMAGE_H
#define _SWAY_BACKGROUND_IMAGE_H
#include <stdbool.h>
#include "cairo_util.h"

enum background_mode {
	BACKGROUND_MODE_STRETCH,
	BACKGROUND_MODE_FILL,
	BACKGROUND_MODE_FIT,
	BACKGROUND_MODE_CENTER,
	BACKGROUND_MODE_TILE,
	BACKGROUND_MODE_SOLID_COLOR,
	BACKGROUND_MODE_INVALID,
};

struct cicp {
	/* `present` is only true if a CICP chunk was found */
	bool present;
	uint8_t primaries;
	uint8_t transfer;
	uint8_t matrix;
	uint8_t range;
};

uint32_t cicp_to_wl_tf(uint8_t transfer);
uint32_t cicp_to_wl_primaries(uint8_t primaries);

enum background_mode parse_background_mode(const char *mode);
cairo_surface_t *load_background_image(const char *path, struct cicp *color_info);
void render_background_image(cairo_t *cairo, cairo_surface_t *image,
		enum background_mode mode, int buffer_width, int buffer_height);

#endif
