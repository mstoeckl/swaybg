#include <assert.h>
#include <stdbool.h>
#include "background-image.h"
#include "cairo_util.h"
#include "log.h"

enum background_mode parse_background_mode(const char *mode) {
	if (strcmp(mode, "stretch") == 0) {
		return BACKGROUND_MODE_STRETCH;
	} else if (strcmp(mode, "fill") == 0) {
		return BACKGROUND_MODE_FILL;
	} else if (strcmp(mode, "fit") == 0) {
		return BACKGROUND_MODE_FIT;
	} else if (strcmp(mode, "center") == 0) {
		return BACKGROUND_MODE_CENTER;
	} else if (strcmp(mode, "tile") == 0) {
		return BACKGROUND_MODE_TILE;
	} else if (strcmp(mode, "solid_color") == 0) {
		return BACKGROUND_MODE_SOLID_COLOR;
	}
	swaybg_log(LOG_ERROR, "Unsupported background mode: %s", mode);
	return BACKGROUND_MODE_INVALID;
}

cairo_surface_t *load_background_image(const char *path) {
	cairo_surface_t *image;
#if HAVE_GDK_PIXBUF
	GError *err = NULL;
	GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(path, &err);
	if (!pixbuf) {
		swaybg_log(LOG_ERROR, "Failed to load background image (%s).",
				err->message);
		return NULL;
	}
	image = gdk_cairo_image_surface_create_from_pixbuf(pixbuf);
	g_object_unref(pixbuf);
#else
	image = cairo_image_surface_create_from_png(path);
#endif // HAVE_GDK_PIXBUF
	if (!image) {
		swaybg_log(LOG_ERROR, "Failed to read background image.");
		return NULL;
	}
	if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
		swaybg_log(LOG_ERROR, "Failed to read background image: %s."
#if !HAVE_GDK_PIXBUF
				"\nSway was compiled without gdk_pixbuf support, so only"
				"\nPNG images can be loaded. This is the likely cause."
#endif // !HAVE_GDK_PIXBUF
				, cairo_status_to_string(cairo_surface_status(image)));
		return NULL;
	}
	return image;
}

void render_background_image(pixman_image_t *dest, pixman_image_t *image,
		enum background_mode mode, int buffer_width, int buffer_height) {
	int image_width = pixman_image_get_width(image);
	int image_height = pixman_image_get_height(image);
	int dest_width = pixman_image_get_width(dest);
	int dest_height = pixman_image_get_height(dest);
	bool image_is_taller = (int64_t)image_height * dest_width >=
		(int64_t)image_width * dest_height;

	int src_x = 0;
	int src_y = 0;
	int dst_x = 0;
	int dst_y = 0;
	double scale_x = 1.;
	double scale_y = 1.;
	pixman_repeat_t repeat = PIXMAN_REPEAT_NONE;

	switch (mode) {
	case BACKGROUND_MODE_STRETCH:
		scale_x = (double)buffer_width / image_width;
		scale_y = (double)buffer_height / image_height;
		break;
	case BACKGROUND_MODE_FILL:
		if (image_is_taller) {
			scale_x = scale_y = (double)dest_width / image_width;
			src_y = (scale_x * image_height - buffer_height) / 2;
		} else {
			scale_x = scale_y = (double)dest_height / image_height;
			src_x = (scale_y * image_width - buffer_width) / 2;
		}
		break;
	case BACKGROUND_MODE_FIT:
		if (image_is_taller) {
			scale_x = scale_y = (double)dest_height / image_height;
			dst_x = (buffer_width - image_width * scale_y) / 2;
		} else {
			scale_x = scale_y = (double)dest_width / image_width;
			dst_y = (buffer_height - image_height * scale_x) / 2;
		}
		break;
	case BACKGROUND_MODE_CENTER:
		if (dest_width >= image_width) {
			dst_x = (dest_width - image_width) / 2;
		} else {
			src_x = (image_width - dest_width) / 2;
		}
		if (dest_height >= image_height) {
			dst_y = (dest_height - image_height) / 2;
		} else {
			src_y = (image_height - dest_height) / 2;
		}
		break;
	case BACKGROUND_MODE_TILE: {
		repeat = PIXMAN_REPEAT_NORMAL;
		break;
	}
	case BACKGROUND_MODE_SOLID_COLOR:
	case BACKGROUND_MODE_INVALID:
		assert(0);
		break;
	}

	if (scale_x >= 0.75 && scale_y >= 0.75) {
		// Bilinear scaling is relatively fast and gives decent
		// results for upscaling and light downscaling
		pixman_image_set_filter(image, PIXMAN_FILTER_BILINEAR, NULL, 0);
	} else {
		// When downscaling, convolve the output_image so that each
		// pixel in the common_image collects colors from a region
		// of size roughly 1/x_scale*1/y_scale in the output_image
		int n_values = 0;
		pixman_fixed_t *conv = pixman_filter_create_separable_convolution(
			&n_values,
			pixman_double_to_fixed(scale_x > 1. ? 1. : 1. / scale_x),
			pixman_double_to_fixed(scale_y > 1. ? 1. : 1. / scale_y),
			PIXMAN_KERNEL_IMPULSE, PIXMAN_KERNEL_IMPULSE,
			PIXMAN_KERNEL_LANCZOS2, PIXMAN_KERNEL_LANCZOS2,
			2, 2);
		pixman_image_set_filter(image,
			PIXMAN_FILTER_SEPARABLE_CONVOLUTION, conv, n_values);
		free(conv);
	}

	pixman_transform_t transform;
	pixman_transform_init_scale(&transform,
		pixman_double_to_fixed(1. / scale_x),
		pixman_double_to_fixed(1. / scale_y));
	pixman_image_set_transform(image, &transform);
	pixman_image_set_repeat(image, repeat);
	pixman_image_composite32(PIXMAN_OP_OVER, image, NULL, dest,
		src_x, src_y, 0, 0, dst_x, dst_y, buffer_width, buffer_height);
}
