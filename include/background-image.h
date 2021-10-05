#ifndef _SWAY_BACKGROUND_IMAGE_H
#define _SWAY_BACKGROUND_IMAGE_H
#include <gegl.h>

enum background_mode {
	BACKGROUND_MODE_STRETCH,
	BACKGROUND_MODE_FILL,
	BACKGROUND_MODE_FIT,
	BACKGROUND_MODE_CENTER,
	BACKGROUND_MODE_TILE,
	BACKGROUND_MODE_SOLID_COLOR,
	BACKGROUND_MODE_INVALID,
};

enum background_mode parse_background_mode(const char *mode);
GeglBuffer *load_background_image(const char *path);
GeglBuffer *render_background_image(GeglBuffer *image, GeglColor *bg_color,
	const Babl* output_fmt, enum background_mode mode,
	int buffer_width, int buffer_height);

#endif
