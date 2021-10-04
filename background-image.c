#include <assert.h>
#include <gegl.h>

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
	GeglNode *graph = gegl_node_new();
	if (!graph) {
		swaybg_log(LOG_ERROR, "Failed to allocate graph\n");
	}
	GeglNode *load = gegl_node_new_child (graph,
		"operation", "gegl:load", "path", path, NULL);
	if (!load) {
		swaybg_log(LOG_ERROR, "Failed to create load op\n");
	}
	const Babl* bablfmt = babl_format("B'aG'aR'aA u8");
	if (!bablfmt) {
		swaybg_log(LOG_ERROR, "Failed to create babl format\n");
		return NULL;
	}
	GeglBuffer *buffer = NULL;
	GeglNode *sink = gegl_node_new_child (graph,
		"operation", "gegl:buffer-sink", "buffer", &buffer,
		"format", bablfmt,
		NULL);
	if (!sink) {
		swaybg_log(LOG_ERROR, "Failed to create sink op\n");
	}
	gegl_node_link_many (load, sink, NULL);
	gegl_node_process (sink);
	if (!buffer) {
		swaybg_log(LOG_ERROR, "Failed to load buffer\n");
		return NULL;
	}
	const GeglRectangle *rect = gegl_buffer_get_extent(buffer);
	const Babl *fmt = gegl_buffer_get_format(buffer);

	cairo_surface_t *image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, rect->width, rect->height);
	gegl_buffer_get(buffer, NULL, 1.0, fmt, cairo_image_surface_get_data(image),
			cairo_image_surface_get_stride(image), GEGL_ABYSS_NONE);

	cairo_surface_mark_dirty(image);

	g_object_unref(buffer);
	g_object_unref(load);
	g_object_unref(sink);
	g_object_unref(graph);

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
