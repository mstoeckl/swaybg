#ifndef _SWAY_BACKGROUND_IMAGE_H
#define _SWAY_BACKGROUND_IMAGE_H

#include <gegl.h>
#include <stdbool.h>

enum background_mode {
	BACKGROUND_MODE_STRETCH,
	BACKGROUND_MODE_FILL,
	BACKGROUND_MODE_FIT,
	BACKGROUND_MODE_CENTER,
	BACKGROUND_MODE_TILE,
	BACKGROUND_MODE_SOLID_COLOR,
	BACKGROUND_MODE_INVALID,
};

void load_gegl_module_library(const char *name);

enum background_mode parse_background_mode(const char *mode);
GeglBuffer *load_background_image(const char *path);
bool render_background_image(GeglBuffer *out, GeglBuffer *image,
	GeglColor *bg_color, enum background_mode mode);

#endif
