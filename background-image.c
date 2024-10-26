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

#if HAVE_GDK_PIXBUF
struct size_callback {
	void (*size_chooser)(void *data, int width,
		int height, int *scale_width, int *scale_height);
	void *data;
	int *surf_orig_width;
	int *surf_orig_height;
};

void size_prepared_callback(GdkPixbufLoader *loader, gint width, gint height,
							gpointer user_data) {
	struct size_callback *callback = user_data;
	GdkPixbufFormat *format = gdk_pixbuf_loader_get_format(loader);
	*callback->surf_orig_width = width;
	*callback->surf_orig_height = height;
	if (gdk_pixbuf_format_is_scalable(format)) {
		int scaled_width, scaled_height;
		callback->size_chooser(callback->data, width, height,
							   &scaled_width, &scaled_height);
		gdk_pixbuf_loader_set_size(loader, scaled_width, scaled_height);
	}
}
#endif

cairo_surface_t *load_background_image(const char *path, void *data,
		void (*size_chooser)(void *data, int width,
			int height, int *scale_width, int *scale_height),
		int *surf_orig_width, int *surf_orig_height) {
	cairo_surface_t *image;
#if HAVE_GDK_PIXBUF
	GdkPixbufLoader *loader = gdk_pixbuf_loader_new();

	struct size_callback callback = {size_chooser, data, surf_orig_width, surf_orig_height};
	g_signal_connect (loader, "size-prepared",
		G_CALLBACK(size_prepared_callback), &callback);

	FILE *f = fopen(path, "r");
	size_t buf_len = 65536;
	char *buf = malloc(buf_len);
	GError *err = NULL;
	while (true) {
		size_t count = fread(buf, 1, buf_len, f);

		gdk_pixbuf_loader_write(loader, (guchar *)buf, count, &err);
		if (err) {
			swaybg_log(LOG_ERROR, "Failed to load background image (%s): %s",
				path, err->message);
			free(buf);
			g_object_unref(loader);
			return NULL;
		}

		if (count != buf_len) {
			if (feof(f)) {
				break;
			} else {
				swaybg_log(LOG_ERROR, "Failed to read from background image (%s)",
						   path);
				free(buf);
				g_object_unref(loader);
				return NULL;
			}
		}
	}
	free(buf);
	gdk_pixbuf_loader_close(loader, &err);
	if (err) {
		swaybg_log(LOG_ERROR, "Failed to load background image (%s): %s",
			path, err->message);
		g_object_unref(loader);
		return NULL;
	}

	GdkPixbuf *pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
	if (!pixbuf) {
		g_object_unref(loader);
		swaybg_log(LOG_ERROR, "Failed to get background image pixbuf");
		return NULL;
	}

	// Correct for embedded image orientation; typical images are not
	// rotated and will be handled efficiently
	GdkPixbuf *oriented = gdk_pixbuf_apply_embedded_orientation(pixbuf);
	g_object_unref(pixbuf);
	image = gdk_cairo_image_surface_create_from_pixbuf(oriented);
	g_object_unref(oriented);
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
#if !HAVE_GDK_PIXBUF
	*surf_orig_width = cairo_image_surface_get_width(image);
	*surf_orig_height = cairo_image_surface_get_height(image);
#endif
	return image;
}

void render_background_image(cairo_t *cairo, cairo_surface_t *image,
		int image_width, int image_height, enum background_mode mode,
		int buffer_width, int buffer_height) {
	double width = image_width;
	double height = image_height;
	double wscale = width / cairo_image_surface_get_width(image);
	double hscale = height / cairo_image_surface_get_height(image);

	cairo_save(cairo);
	switch (mode) {
	case BACKGROUND_MODE_STRETCH:
		cairo_scale(cairo,
			wscale * (double)buffer_width / width,
			hscale * (double)buffer_height / height);
		cairo_set_source_surface(cairo, image, 0, 0);
		break;
	case BACKGROUND_MODE_FILL: {
		double window_ratio = (double)buffer_width / buffer_height;
		double bg_ratio = width / height;

		if (window_ratio > bg_ratio) {
			double scale = (double)buffer_width / width;
			cairo_scale(cairo, wscale * scale, hscale * scale);
			cairo_set_source_surface(cairo, image,
				0, ((double)buffer_height / 2 / scale - height / 2) / hscale);
		} else {
			double scale = (double)buffer_height / height;
			cairo_scale(cairo, wscale * scale, hscale * scale);
			cairo_set_source_surface(cairo, image,
				((double)buffer_width / 2 / scale - width / 2) / wscale, 0);
		}
		break;
	}
	case BACKGROUND_MODE_FIT: {
		double window_ratio = (double)buffer_width / buffer_height;
		double bg_ratio = width / height;

		if (window_ratio > bg_ratio) {
			double scale = (double)buffer_height / height;
			cairo_scale(cairo, wscale * scale, hscale * scale);
			cairo_set_source_surface(cairo, image,
				((double)buffer_width / 2 / scale - width / 2) / wscale, 0);
		} else {
			double scale = (double)buffer_width / width;
			cairo_scale(cairo, wscale * scale, hscale * scale);
			cairo_set_source_surface(cairo, image,
				0, ((double)buffer_height / 2 / scale - height / 2) / hscale);
		}
		break;
	}
	case BACKGROUND_MODE_CENTER:
		cairo_scale(cairo, wscale, hscale);
		cairo_set_source_surface(cairo, image,
			((double)buffer_width / 2 - width / 2) / wscale,
			((double)buffer_height / 2 - height / 2) / hscale);
		break;
	case BACKGROUND_MODE_TILE: {
		cairo_pattern_t *pattern = cairo_pattern_create_for_surface(image);
		cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
		cairo_scale(cairo, wscale, hscale);
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
