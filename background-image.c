#include <assert.h>
#include <png.h>
#include <stdbool.h>

#include "background-image.h"
#include "cairo_util.h"
#include "log.h"

#include "color-management-v1-client-protocol.h"

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

/** Map CICP transfer code to the WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION enum
 * (or 0 if no match). */
uint32_t cicp_to_wl_tf(uint8_t transfer) {
	switch (transfer) {
	case 1:
	case 6:
	case 14:
	case 15:
		return WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886;
	case 2: // unspecified
		return 0;
	case 4:
		return WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22;
	case 5:
		return WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA28;
	case 7:
		return WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST240;
	case 8: // this is an extension of tf=8 and will work on all tf=8 images
		return WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR;
	case 9:
		return WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_LOG_100;
	case 10:
		return WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_LOG_316;
	case 12:
		return WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_XVYCC;
	case 13: // swaybg only supports MatrixCoefficients = 0
		return WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB;
	case 16: // with reference white specified
		return WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ;
	case 17:
		return WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST428;
	case 18: // with reference white specified
		return WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG;
	default:
		return 0;
	}

}
/** Map CICP primary code to the WP_COLOR_MANAGER_V1_PRIMARIES enum
 * (or 0 if no match). */
uint32_t cicp_to_wl_primaries(uint8_t primaries) {
	switch (primaries) {
	case 1:
		return WP_COLOR_MANAGER_V1_PRIMARIES_SRGB;
	case 2: // unspecified
		return 0;
	case 4:
		return WP_COLOR_MANAGER_V1_PRIMARIES_PAL_M;
	case 5:
		return WP_COLOR_MANAGER_V1_PRIMARIES_PAL;
	case 6:
	case 7:
		return WP_COLOR_MANAGER_V1_PRIMARIES_NTSC;
	case 8:
		return WP_COLOR_MANAGER_V1_PRIMARIES_GENERIC_FILM;
	case 9:
		return WP_COLOR_MANAGER_V1_PRIMARIES_BT2020;
	case 10:
		return WP_COLOR_MANAGER_V1_PRIMARIES_CIE1931_XYZ;
	case 11:
		return WP_COLOR_MANAGER_V1_PRIMARIES_DCI_P3;
	case 12:
		return WP_COLOR_MANAGER_V1_PRIMARIES_DISPLAY_P3;
	default:
		return 0;
	}
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
	// matrix and range are always the same, 0 and 1, for normal RGB PNG images
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
	 * at the moment).
	 *
	 * Also: v3 PNG draft color precedence order is cICP > iCCP > sRGB > cHRM / gAMA
	 */
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

	// todo: later, actually read the full image, if only to avoid race conditions

	fclose(fp);
	png_destroy_read_struct(&png_ptr, &info_ptr, NULL);

	*cicp = cicp_data;
	return true;
}

cairo_surface_t *load_background_image(const char *path, struct cicp *color_info) {
	cairo_surface_t *image = NULL;

	struct write_cicp cicp = {
		.found = false
	};
#if HAVE_GDK_PIXBUF
	// Prefer to load PNG images with Cairo, since it can load images with
	// higher bit depths at full precision
	const char *suffix = strrchr(path, '.');
	if (suffix && (!strcmp(suffix, ".png") || !strcmp(suffix, ".PNG"))) {
		read_png_info(path, &cicp);
		image = cairo_image_surface_create_from_png(path);
	}

	// if not a PNG image, try to load with gdk-pixbuf
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
#else // !HAVE_GDK_PIXBUF
	image = cairo_image_surface_create_from_png(path);
#endif

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
		color_info->present = true;
		color_info->primaries = cicp.primaries;
		color_info->transfer = cicp.transfer;
		color_info->matrix = cicp.matrix;
		color_info->range = cicp.range;
	} else {
		color_info->present = false;
		color_info->primaries = 1; // sRGB primaries
		color_info->transfer = 13; // sRGB transfer function
		color_info->matrix = 0; // RGB
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
