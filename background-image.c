#include <assert.h>
#include <gegl.h>
#include <stdbool.h>

#include "background-image.h"
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

GeglBuffer *load_background_image(const char *path) {
	GeglNode *graph = gegl_node_new();
	if (!graph) {
		swaybg_log(LOG_ERROR, "Failed to allocate graph\n");
	}
	GeglNode *load = gegl_node_new_child (graph,
		"operation", "gegl:load", "path", path, NULL);
	if (!load) {
		swaybg_log(LOG_ERROR, "Failed to create load op\n");
	}
	GeglBuffer *buffer = NULL;
	GeglNode *sink = gegl_node_new_child (graph,
		"operation", "gegl:buffer-sink", "buffer", &buffer,
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
	g_object_unref(load);
	g_object_unref(sink);
	g_object_unref(graph);

	return buffer;
}

GeglBuffer *render_background_image(GeglBuffer *image, GeglColor *bg_color,
		const Babl* output_fmt, enum background_mode mode,
		int buffer_width, int buffer_height) {

	// Inputs: bg_color;
	// Process: transform and blend
	// Output: in output_fmt
	GeglNode *graph = gegl_node_new();
	if (!graph) {
		swaybg_log(LOG_ERROR, "Failed to allocate graph\n");
		return NULL;
	}

	GeglNode *load_color = gegl_node_new_child (graph,
		"operation", "gegl:color", "value", bg_color, NULL);
	double crop_x = 0, crop_y = 0, crop_width = buffer_width, crop_height = buffer_height;
	GeglNode *crop_color = gegl_node_new_child (graph,
		"operation", "gegl:crop", "x", crop_x, "y", crop_y, "width", crop_width, "height", crop_height, NULL);
	GeglBuffer *dest = NULL;
	GeglNode *sink = gegl_node_new_child (graph,
		"operation", "gegl:buffer-sink", "buffer", &dest,
		"format", output_fmt,
		NULL);
	gegl_node_link(load_color, crop_color);

	GeglNode *load_img = NULL, *proc_scale = NULL, *proc_translate = NULL,
		*proc_tile = NULL, *proc_cropimg = NULL, *proc_srcover = NULL;

	if (!load_color || !crop_color || !sink) {
		swaybg_log(LOG_ERROR, "Failed to create a GEGL node\n");
		goto cleanup;
	}

	switch (mode) {

	case BACKGROUND_MODE_TILE: {
		load_img = gegl_node_new_child (graph,
			"operation", "gegl:buffer-source", "buffer", image, NULL);
		proc_tile = gegl_node_new_child (graph,
			"operation", "gegl:tile", NULL);
		proc_cropimg = gegl_node_new_child (graph,
			"operation", "gegl:crop", "x", crop_x, "y", crop_y, "width", crop_width, "height", crop_height, NULL);
		proc_srcover = gegl_node_new_child (graph,
			"operation", "svg:src-over", NULL);

		if (!load_img || !proc_tile || !proc_cropimg || !proc_srcover) {
			swaybg_log(LOG_ERROR, "Failed to create tile node sequence\n");
		}

		gegl_node_link(load_img, proc_tile);
		gegl_node_link(proc_tile, proc_cropimg);
		gegl_node_link(crop_color, proc_srcover);
		gegl_node_connect_to(proc_cropimg, "output", proc_srcover, "aux");
		gegl_node_link(proc_srcover, sink);
	} break;

	case BACKGROUND_MODE_STRETCH:
	case BACKGROUND_MODE_CENTER:
	case BACKGROUND_MODE_FILL:
	case BACKGROUND_MODE_FIT: {
		const GeglRectangle *image_rect = gegl_buffer_get_extent(image);

		double x_tr, y_tr, x_scl, y_scl;
		bool buffer_is_wider = (int64_t)image_rect->width * buffer_height >= (int64_t)image_rect->height * buffer_width;
		if (mode == BACKGROUND_MODE_CENTER) {
			x_tr = (buffer_width - image_rect->width) * 0.5;
			y_tr = (buffer_height - image_rect->height) * 0.5;
			x_scl = 1.0;
			y_scl = 1.0;
		} else if (mode == BACKGROUND_MODE_STRETCH) {
			x_tr = 0;
			y_tr = 0;
			x_scl = buffer_width / (double)image_rect->width;
			y_scl = buffer_height / (double)image_rect->height;
		} else if (mode == BACKGROUND_MODE_FILL || mode == BACKGROUND_MODE_FIT) {
			bool match_x = buffer_is_wider == (mode == BACKGROUND_MODE_FIT);
			if (match_x) {
				x_scl = (double)buffer_width / image_rect->width;
				x_tr = 0;
				y_scl = x_scl;
				y_tr = (buffer_height - y_scl * image_rect->height) * 0.5;
			} else {
				y_scl = (double)buffer_height / image_rect->height;
				y_tr = 0;
				x_scl = y_scl;
				x_tr = (buffer_width - y_scl * image_rect->width) * 0.5;
			}
		} else {
			x_tr = y_tr = 0.0;
			x_scl = y_scl = 1.0;
		}

		load_img = gegl_node_new_child (graph,
			"operation", "gegl:buffer-source", "buffer", image, NULL);
		proc_scale = gegl_node_new_child (graph,
			"operation", "gegl:scale-ratio", "x", x_scl, "y", y_scl, NULL);
		proc_translate = gegl_node_new_child (graph,
			"operation", "gegl:translate", "x", x_tr, "y", y_tr,NULL);
		proc_cropimg = gegl_node_new_child (graph,
			"operation", "gegl:crop", "x", crop_x, "y", crop_y, "width", crop_width, "height", crop_height, NULL);
		proc_srcover = gegl_node_new_child (graph,
			"operation", "svg:src-over", NULL);

		if (!load_img || !proc_scale || !proc_translate || !proc_cropimg || !proc_srcover) {
			swaybg_log(LOG_ERROR, "Failed to create scale node sequence\n");
		}

		gegl_node_link(load_img, proc_scale);
		gegl_node_link(proc_scale, proc_translate);
		gegl_node_link(proc_translate, proc_cropimg);
		gegl_node_link(crop_color, proc_srcover);
		gegl_node_connect_to(proc_cropimg, "output", proc_srcover, "aux");
		gegl_node_link(proc_srcover, sink);
	}	break;
	case BACKGROUND_MODE_INVALID:
	case BACKGROUND_MODE_SOLID_COLOR:
		gegl_node_link(crop_color, sink);
		break;
	}

	gegl_node_process (sink);

cleanup:
	if (load_img) {
		g_object_unref(load_img);
	}
	if (proc_cropimg) {
		g_object_unref(proc_cropimg);
	}
	if (proc_scale) {
		g_object_unref(proc_scale);
	}
	if (proc_translate) {
		g_object_unref(proc_translate);
	}
	if (proc_tile) {
		g_object_unref(proc_tile);
	}
	if (proc_srcover) {
		g_object_unref(proc_srcover);
	}
	if (load_color) {
		g_object_unref(load_color);
	}
	if (crop_color) {
		g_object_unref(crop_color);
	}
	if (sink) {
		g_object_unref(sink);
	}
	g_object_unref(graph);
	return dest;
}
