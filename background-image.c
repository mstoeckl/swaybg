#include <assert.h>
#include "background-image.h"
#include "cairo_util.h"
#include "log.h"

#include <openexr.h>

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
	char *suffix = strrchr(path, '.');
	if (suffix && (strcmp(suffix, ".exr") == 0 || strcmp(suffix, ".EXR") == 0)) {
		// TODO: screw on an OpenEXR loader, so we can get full precision
		// out of CAIRO RGBA128F
		exr_context_initializer_t ctx_init = EXR_DEFAULT_CONTEXT_INITIALIZER;
		// ^^ set user data, and do custom read handler?

		exr_context_t ctxt;
		exr_result_t res;
		if ((res = exr_start_read (&ctxt, path, &ctx_init))) {
			swaybg_log(LOG_ERROR, "Failed to start reading background image: %s.",
				exr_get_error_code_as_string(res));
			return NULL;
		}
		int parts = 0;
		if ((res = exr_get_count(ctxt, &parts))) {
			swaybg_log(LOG_ERROR, "Failed to start get image part count: %s.",
				exr_get_error_code_as_string(res));
			return NULL;
		}
		if (parts != 1) {
			swaybg_log(LOG_ERROR, "Multi-(%d)-part EXR not supported", parts);
			return NULL;
		}
		const int the_part = 0;

		/* technically we should be only extracting the display
		 * window portion here */
		exr_attr_box2i_t data_window;
		if ((res = exr_get_data_window(ctxt, the_part, &data_window))) {
			swaybg_log(LOG_ERROR, "Failed to get data window: %s.",
				exr_get_error_code_as_string(res));
			return NULL;
		}
		int width = data_window.max.x - data_window.min.x + 1;
		int height = data_window.max.y - data_window.min.y + 1;

		const exr_attr_chlist_t *chlist = NULL;
		if ((res = exr_get_channels(ctxt, the_part, &chlist))) {
			swaybg_log(LOG_ERROR, "Failed to get channels: %s.",
				exr_get_error_code_as_string(res));
			return NULL;
		}
		/* misc note: EXR images are, iirc, alpha-premultiplied by default;
		 * also, color curve is linear */
		cairo_format_t target = chlist->num_channels == 4 ? CAIRO_FORMAT_RGBA128F : CAIRO_FORMAT_RGB96F;
		image = cairo_image_surface_create(target, width, height);
		if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
			swaybg_log(LOG_ERROR, "Failed to create background image: %s."
					, cairo_status_to_string(cairo_surface_status(image)));
			return NULL;
		}

		int chunk_count = 0;
		if ((res = exr_get_chunk_count(ctxt, the_part, &chunk_count))) {
			swaybg_log(LOG_ERROR, "Failed to get chunk count: %s.",
				exr_get_error_code_as_string(res));
			return NULL;
		}
		exr_storage_t storage = EXR_STORAGE_LAST_TYPE;
		if ((res = exr_get_storage(ctxt, the_part, &storage))) {
			swaybg_log(LOG_ERROR, "Failed to get storage type: %s.",
				exr_get_error_code_as_string(res));
			return NULL;
		}

		if (storage != EXR_STORAGE_SCANLINE) {
			swaybg_log(LOG_ERROR, "Non-scanline storage not supported");
		}

		int32_t scanlines_per_chunk = 0;
		if ((res = exr_get_scanlines_per_chunk(ctxt, the_part, &scanlines_per_chunk))) {
			swaybg_log(LOG_ERROR, "Failed to get scanlines per chunk: %s.",
				exr_get_error_code_as_string(res));
			return NULL;
		}

		exr_chunk_info_t chunk = {0};
		if ((res = exr_read_scanline_chunk_info(ctxt, the_part, data_window.min.y, &chunk))) {
			swaybg_log(LOG_ERROR, "Failed to get chunk info: %s.",
				exr_get_error_code_as_string(res));
			return NULL;
		}

		exr_decode_pipeline_t decode_pipeline = {0};
		if ((res = exr_decoding_initialize(ctxt, the_part, &chunk, &decode_pipeline))) {
			swaybg_log(LOG_ERROR, "Failed to get chunk info: %s.",
				exr_get_error_code_as_string(res));
			return NULL;
		}
		size_t offsets[4] = {0, 0, 0, 0};

		for (int i = 0; i < decode_pipeline.channel_count; i++) {
			decode_pipeline.channels[i].data_type = EXR_PIXEL_FLOAT;
			if (!strcmp(decode_pipeline.channels[i].channel_name, "R")) {
				offsets[i] = 0;
			} else if (!strcmp(decode_pipeline.channels[i].channel_name, "G")) {
				offsets[i] = 1;
			} else if (!strcmp(decode_pipeline.channels[i].channel_name, "B")) {
				offsets[i] = 2;
			} else if (!strcmp(decode_pipeline.channels[i].channel_name, "A")) {
				offsets[i] = 3;
			} else {
				swaybg_log(LOG_ERROR, "Unexpected channel: %s.",
					decode_pipeline.channels[i].channel_name);
				return NULL;
			}
		}

		int stride = cairo_image_surface_get_stride(image);
		uint8_t *data = cairo_image_surface_get_data(image);
		int pixel_stride = chlist->num_channels == 4 ? 16 : 12;
		for (int y = data_window.min.y; y <= data_window.max.y; y += scanlines_per_chunk) {
			if (y > data_window.min.y) {
				if ((res = exr_read_scanline_chunk_info(ctxt, the_part, y, &chunk))) {
					swaybg_log(LOG_ERROR, "Failed to get chunk info: %s.",
						exr_get_error_code_as_string(res));
					return NULL;
				}
				if ((res = exr_decoding_update(ctxt, the_part, &chunk, &decode_pipeline))) {
					swaybg_log(LOG_ERROR, "Failed to get update decoder: %s.",
						exr_get_error_code_as_string(res));
					return NULL;
				}
			}
			// hopefully channel order is fixed for chunks
			uint8_t* base = data + stride * (y - data_window.min.y);
			for (int i = 0; i < decode_pipeline.channel_count; i++) {
				decode_pipeline.channels[i].decode_to_ptr = base + offsets[i] * 4;
				decode_pipeline.channels[i].user_pixel_stride = pixel_stride;
				decode_pipeline.channels[i].user_line_stride = stride;

			}
			if ((res = exr_decoding_choose_default_routines(ctxt, the_part, &decode_pipeline))) {
				swaybg_log(LOG_ERROR, "Failed to get set default routines decoder: %s.",
					exr_get_error_code_as_string(res));
				return NULL;
			}

			if ((res = exr_decoding_run(ctxt, the_part, &decode_pipeline))) {
				swaybg_log(LOG_ERROR, "Failed to get run decoder: %s.",
					exr_get_error_code_as_string(res));
				return NULL;
			}
		}
		cairo_surface_mark_dirty(image);
		if ((res = exr_decoding_destroy(ctxt, &decode_pipeline))) {
			swaybg_log(LOG_ERROR, "Failed to destroy decoder: %s.",
				exr_get_error_code_as_string(res));
			return NULL;
		}
		if ((res = exr_finish(&ctxt))) {
			swaybg_log(LOG_ERROR, "Failed to close reading context: %s.",
				exr_get_error_code_as_string(res));
			return NULL;
		}

		/* Color space info: need to call exr_attr_get_chromaticities .
		 * If not available, matches Rec. ITU-R BT.709-3 .*/
	} else if (suffix && (strcmp(suffix, ".png") == 0 || strcmp(suffix, ".PNG") == 0)) {
		image = cairo_image_surface_create_from_png(path);

		if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
			swaybg_log(LOG_ERROR, "Failed to read background image: %s."
					, cairo_status_to_string(cairo_surface_status(image)));
			return NULL;
		}
	} else {
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
		swaybg_log(LOG_ERROR,
			   "\nSway was compiled without gdk_pixbuf support, so only"
			"\nPNG images can be loaded; %s does not end with .png or .PNG.",
			 path);
		return NULL;
#endif
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
