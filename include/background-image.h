#ifndef _SWAY_BACKGROUND_IMAGE_H
#define _SWAY_BACKGROUND_IMAGE_H

#include <pixman.h>

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
pixman_image_t *load_background_image(const char *path);
void render_background_image(pixman_image_t *dest, pixman_image_t *image,
		enum background_mode mode, int buffer_width, int buffer_height);

#endif
