#include <assert.h>
#include <png.h>
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


static void error_handler(png_structp png_ptr, png_const_charp msg) {
	// todo: handle libpng errors properly -- e.g., with longjmp
	// to png_get_error_ptr(png_ptr);
	abort();
}

struct write_cicp {
	uint8_t primaries;
	uint8_t transfer;
	uint8_t matrix;
	uint8_t range;
	bool found;
};
static int read_chunk_callback(png_structp png_ptr, png_unknown_chunkp chunk_ptr) {

	struct write_cicp *w = png_get_user_chunk_ptr(png_ptr);

	if (chunk_ptr->size != 4 || strcmp((char*)chunk_ptr->name, "cICP")) {
		swaybg_log(LOG_ERROR, "Unexpected chunk: %s, size %zu", chunk_ptr->name, chunk_ptr->size);
		return 1;
	}

	w->primaries = chunk_ptr->data[0];
	w->transfer = chunk_ptr->data[1];
	w->matrix = chunk_ptr->data[2];
	w->range = chunk_ptr->data[3];
	w->found = true;

	return 0;
}

/* TODO: longterm, PNG files should be loaded directly, not via cairo.
 * To make format conversion easier, Pixman should support u16 buffers,
 * since those are convenient to pre-multiply in-place. */
bool read_png_info(const char *path, struct write_cicp *cicp) {
	FILE *fp = fopen(path, "rb");
	if (!fp)
	{
		return false;
	}

	uint8_t header[8];
	if (fread(header, 1, 8, fp) != 8)
	{
		return false;
	}

	bool is_png = !png_sig_cmp(header, 0, 8);
	if (!is_png)
	{
		return false;
	}

	png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING,
		NULL, error_handler, NULL);
	if (!png_ptr)
		return false;

	png_infop info_ptr = png_create_info_struct(png_ptr);
	if (!info_ptr) {
		png_destroy_read_struct(&png_ptr, NULL, NULL);
		return false;
	}

	/* Always read and process the chunk ourselves, even if libpng gains
	 * support for it in the future. (the cICP chunk is in the v3 PNG draft
	 * at the moment). */
	uint8_t chunk_list[4] = {'c','I','C','P'};
	png_set_keep_unknown_chunks(png_ptr, PNG_HANDLE_CHUNK_ALWAYS, chunk_list, 1);

	struct write_cicp cicp_data = {
		.primaries = 0,
		.transfer = 0,
		.matrix = 0,
		.range = 0,
		.found = false
	};

	png_set_read_user_chunk_fn(png_ptr, &cicp_data, read_chunk_callback);

	png_init_io(png_ptr, fp);
	png_set_sig_bytes(png_ptr, 8);
	png_read_info(png_ptr, info_ptr);

	// todo: later, actually read the full image

	fclose(fp);
	png_destroy_read_struct(&png_ptr, &info_ptr, NULL);

	*cicp = cicp_data;
	return true;
}

cairo_surface_t *load_background_image(const char *path, struct cicp *color_info) {
	cairo_surface_t *image = NULL;

	// Prefer to load PNG images with Cairo, since it can load images with
	// higher bit depths at full precision
	struct write_cicp cicp = {
		.found = false
	};
	const char *suffix = strrchr(path, '.');
	if (suffix && (!strcmp(suffix, ".png") || !strcmp(suffix, ".PNG"))) {
		read_png_info(path, &cicp);
		image = cairo_image_surface_create_from_png(path);
	}

	// if not a PNG image, try to load with gdk-pixbuf
#if HAVE_GDK_PIXBUF
	if (!image) {
		GError *err = NULL;
		GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(path, &err);
		if (!pixbuf) {
			swaybg_log(LOG_ERROR, "Failed to load background image (%s).",
					err->message);
			return NULL;
		}

		// Correct for embedded image orientation; typical images are not
		// rotated and will be handled efficiently
		GdkPixbuf *oriented = gdk_pixbuf_apply_embedded_orientation(pixbuf);
		g_object_unref(pixbuf);
		image = gdk_cairo_image_surface_create_from_pixbuf(oriented);
		g_object_unref(oriented);
	}
#endif // HAVE_GDK_PIXBUF

	if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
		swaybg_log(LOG_ERROR, "Failed to read background image: %s."
#if !HAVE_GDK_PIXBUF
				"\nSway was compiled without gdk_pixbuf support, so only"
				"\nPNG images can be loaded. This is the likely cause."
#endif // !HAVE_GDK_PIXBUF
				, cairo_status_to_string(cairo_surface_status(image)));
		return NULL;
	}

	if (cicp.found) {
		/* ignore unless reasonable */
		if (cicp.primaries != 1 && cicp.primaries != 9) {
			swaybg_log(LOG_ERROR, "Received image with CICP primaries = %d; only rec709=1 and rec2020=9 supported", cicp.primaries);
			cicp.found = false;
		}
		if (cicp.transfer != 4 && cicp.transfer != 8 && cicp.transfer != 13  && cicp.transfer != 16) {
			swaybg_log(LOG_ERROR, "Received image with CICP transfer = %d; only srgb=13, gamma22=4, st2084=16, and linear=8 supported", cicp.transfer);
			cicp.found = false;
		}
		if (cicp.range != 1) {
			swaybg_log(LOG_ERROR, "Received image with CICP range = %d; only full range (1) supported", cicp.range);
			cicp.found = false;
		}
		if (cicp.matrix != 0) {
			swaybg_log(LOG_ERROR, "Received image with CICP matrix = %d; only RGB (0) supported", cicp.matrix);
			cicp.found = false;
		}

	}

	if (cicp.found) {
		color_info->primaries = cicp.primaries;
		color_info->transfer = cicp.transfer;
		color_info->matrix = cicp.matrix;
		color_info->range = cicp.range;
	} else {
		color_info->primaries = 1; // sRGB primaries
		color_info->transfer = 13; // sRGB transfer function
		color_info->matrix = 0;
		color_info->range = 1; // full range
	}

	return image;
}

void render_background_image(cairo_t *cairo, cairo_surface_t *image,
		enum background_mode mode, int buffer_width, int buffer_height) {
	double width = cairo_image_surface_get_width(image);
	double height = cairo_image_surface_get_height(image);

	cairo_save(cairo);
	switch (mode) {
	case BACKGROUND_MODE_STRETCH:
		cairo_scale(cairo,
				(double)buffer_width / width,
				(double)buffer_height / height);
		cairo_set_source_surface(cairo, image, 0, 0);
		break;
	case BACKGROUND_MODE_FILL: {
		double window_ratio = (double)buffer_width / buffer_height;
		double bg_ratio = width / height;

		if (window_ratio > bg_ratio) {
			double scale = (double)buffer_width / width;
			cairo_scale(cairo, scale, scale);
			cairo_set_source_surface(cairo, image,
					0, (double)buffer_height / 2 / scale - height / 2);
		} else {
			double scale = (double)buffer_height / height;
			cairo_scale(cairo, scale, scale);
			cairo_set_source_surface(cairo, image,
					(double)buffer_width / 2 / scale - width / 2, 0);
		}
		break;
	}
	case BACKGROUND_MODE_FIT: {
		double window_ratio = (double)buffer_width / buffer_height;
		double bg_ratio = width / height;

		if (window_ratio > bg_ratio) {
			double scale = (double)buffer_height / height;
			cairo_scale(cairo, scale, scale);
			cairo_set_source_surface(cairo, image,
					(double)buffer_width / 2 / scale - width / 2, 0);
		} else {
			double scale = (double)buffer_width / width;
			cairo_scale(cairo, scale, scale);
			cairo_set_source_surface(cairo, image,
					0, (double)buffer_height / 2 / scale - height / 2);
		}
		break;
	}
	case BACKGROUND_MODE_CENTER:
		cairo_set_source_surface(cairo, image,
				(double)buffer_width / 2 - width / 2,
				(double)buffer_height / 2 - height / 2);
		break;
	case BACKGROUND_MODE_TILE: {
		cairo_pattern_t *pattern = cairo_pattern_create_for_surface(image);
		cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
		cairo_set_source(cairo, pattern);
		cairo_pattern_destroy(pattern);
		break;
	}
	case BACKGROUND_MODE_SOLID_COLOR:
	case BACKGROUND_MODE_INVALID:
		assert(0);
		break;
	}
	cairo_paint(cairo);
	cairo_restore(cairo);
}
