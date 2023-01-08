#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include "background-image.h"
#include "log.h"
#if HAVE_GDK_PIXBUF
#include <gdk-pixbuf/gdk-pixbuf.h>
#else
#include <cairo.h>
#endif

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

#if HAVE_GDK_PIXBUF
static pixman_image_t* pixman_image_surface_create_from_pixbuf(const GdkPixbuf *gdkbuf) {
	int chan = gdk_pixbuf_get_n_channels(gdkbuf);
	if (chan < 3) {
		return NULL;
	}

	const guint8* gdkpix = gdk_pixbuf_read_pixels(gdkbuf);
	if (!gdkpix) {
		return NULL;
	}
	gint w = gdk_pixbuf_get_width(gdkbuf);
	gint h = gdk_pixbuf_get_height(gdkbuf);
	int stride = gdk_pixbuf_get_rowstride(gdkbuf);

	pixman_format_code_t fmt = (chan == 3) ? PIXMAN_x8r8g8b8 : PIXMAN_a8r8g8b8;
	pixman_image_t *img = pixman_image_create_bits(fmt, w, h, NULL, 0);
	if (!img) {
		return NULL;
	}

	int cstride = pixman_image_get_stride(img);
	unsigned char *cpix = (unsigned char *)pixman_image_get_data(img);

	if (chan == 3) {
		int i;
		for (i = h; i; --i) {
			const guint8 *gp = gdkpix;
			unsigned char *cp = cpix;
			const guint8* end = gp + 3*w;
			while (gp < end) {
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
				cp[0] = gp[2];
				cp[1] = gp[1];
				cp[2] = gp[0];
#else
				cp[1] = gp[0];
				cp[2] = gp[1];
				cp[3] = gp[2];
#endif
				gp += 3;
				cp += 4;
			}
			gdkpix += stride;
			cpix += cstride;
		}
	} else {
		/* premul-color = alpha/255 * color/255 * 255 = (alpha*color)/255
		 * (z/255) = z/256 * 256/255     = z/256 (1 + 1/255)
		 *         = z/256 + (z/256)/255 = (z + z/255)/256
		 *         # recurse once
		 *         = (z + (z + z/255)/256)/256
		 *         = (z + z/256 + z/256/255) / 256
		 *         # only use 16bit uint operations, loose some precision,
		 *         # result is floored.
		 *       ->  (z + z>>8)>>8
		 *         # add 0x80/255 = 0.5 to convert floor to round
		 *       =>  (z+0x80 + (z+0x80)>>8 ) >> 8
		 * ------
		 * tested as equal to lround(z/255.0) for uint z in [0..0xfe02]
		 */
#define PREMUL_ALPHA(x,a,b,z) \
		G_STMT_START { z = a * b + 0x80; x = (z + (z >> 8)) >> 8; } \
		G_STMT_END
		int i;
		for (i = h; i; --i) {
			const guint8 *gp = gdkpix;
			unsigned char *cp = cpix;
			const guint8* end = gp + 4*w;
			guint z1, z2, z3;
			while (gp < end) {
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
				PREMUL_ALPHA(cp[0], gp[2], gp[3], z1);
				PREMUL_ALPHA(cp[1], gp[1], gp[3], z2);
				PREMUL_ALPHA(cp[2], gp[0], gp[3], z3);
				cp[3] = gp[3];
#else
				PREMUL_ALPHA(cp[1], gp[0], gp[3], z1);
				PREMUL_ALPHA(cp[2], gp[1], gp[3], z2);
				PREMUL_ALPHA(cp[3], gp[2], gp[3], z3);
				cp[0] = gp[3];
#endif
				gp += 4;
				cp += 4;
			}
			gdkpix += stride;
			cpix += cstride;
		}
#undef PREMUL_ALPHA
	}
	return img;
}
#endif // !HAVE_GDK_PIXBUF

pixman_image_t *load_background_image(const char *path) {
	pixman_image_t *image;
#if HAVE_GDK_PIXBUF
	GError *err = NULL;
	GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(path, &err);
	if (!pixbuf) {
		swaybg_log(LOG_ERROR, "Failed to load background image (%s).",
				err->message);
		return NULL;
	}
	image = pixman_image_surface_create_from_pixbuf(pixbuf);
	g_object_unref(pixbuf);
#else
	cairo_surface_t *surface = cairo_image_surface_create_from_png(path);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		swaybg_log(LOG_ERROR, "Failed to load background image: %s"
			"\nSway was compiled without gdk_pixbuf support, so only"
			"\nPNG images can be loaded. This is the likely cause.",
			cairo_status_to_string(cairo_surface_status(surface)));
		return NULL;
	}
	image = pixman_image_create_bits(PIXMAN_a8r8g8b8,
		cairo_image_surface_get_width(surface),
		cairo_image_surface_get_height(surface),
		NULL, 0);
	if (image) {
		// convert format by blitting surface onto image
		cairo_surface_t *dst = cairo_image_surface_create_for_data(
			(unsigned char *)pixman_image_get_data(image),
			CAIRO_FORMAT_ARGB32,
			pixman_image_get_width(image),
			pixman_image_get_height(image),
			pixman_image_get_stride(image));
		cairo_t *cairo = cairo_create(dst);
		cairo_set_source_surface(cairo, surface, 0, 0);
		cairo_paint(cairo);
		cairo_destroy(cairo);

		cairo_surface_flush(dst);
	}
	cairo_surface_destroy(surface);

#endif // HAVE_GDK_PIXBUF

	if (!image) {
		swaybg_log(LOG_ERROR, "Failed to read background image.");
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
