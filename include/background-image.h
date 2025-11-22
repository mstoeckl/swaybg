#ifndef _SWAY_BACKGROUND_IMAGE_H
#define _SWAY_BACKGROUND_IMAGE_H

#include "cairo_util.h"
#if HAVE_GLYCIN
#include <glycin-2/glycin.h>
#endif
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

/** CICP (coding-independent code point) values */
struct cicp {
	uint8_t primaries;
	uint8_t transfer;
	uint8_t matrix;
	uint8_t range;
};

struct background_image {
	cairo_surface_t *cairo_surface;
#if HAVE_GLYCIN
	/* This field is only non-NULL if glycin loaded an image with a scalable
	 * mimetype; to be accurate in fit and fill modes, such images need to be
	 * rendered once per output. */
	GlyImage *scalable_image;
#endif
	struct cicp cicp;
	bool has_cicp;
};

enum background_mode parse_background_mode(const char *mode);
/** On success, this returns true and fills *image. */
bool load_background_image(const char *path, struct background_image *image);
#if HAVE_GLYCIN
/** Requires that image->scalable_image is set; on success, this returns true
 *  and fills the remaining fields of `image`. */
bool load_scalable_image(struct background_image *image,
	uint32_t output_w, uint32_t output_h, enum background_mode mode);
#endif
void render_background_image(cairo_t *cairo, cairo_surface_t *image,
		enum background_mode mode, int buffer_width, int buffer_height);

#endif
