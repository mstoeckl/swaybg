#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <gegl.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

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

void load_gegl_module_library(const char *name) {
	const char *plugin_folder = GEGL_PLUGIN_FOLDER;

	const char *xdg_runtime_dir = getenv("XDG_RUNTIME_DIR");
	char src_path[256];
	char dest_folder[256];
	char dest_path[256];
	sprintf(dest_folder, "%s/swaybg_gegl_hack_%s", xdg_runtime_dir, name);
	sprintf(dest_path, "%s/swaybg_gegl_hack_%s/plugin.so", xdg_runtime_dir, name);
	sprintf(src_path, "%s/%s", plugin_folder, name);

	if (mkdir(dest_folder, S_IRWXU) == -1 && errno != EEXIST) {
		swaybg_log(LOG_ERROR, "Failed to create plugin folder %s", plugin_folder);
	}

	if (symlink(src_path, dest_path) == -1 && errno != EEXIST) {
		swaybg_log(LOG_ERROR, "Failed to create plugin link %s", dest_path);
	}

	struct timespec t1, t2;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	gegl_load_module_directory(dest_folder);
	clock_gettime(CLOCK_MONOTONIC, &t2);
	double msec = 1e3 * (t2.tv_sec - t1.tv_sec) + 1e-6 * (t2.tv_nsec - t1.tv_nsec);
	swaybg_log(LOG_ERROR, "Loading module %s took %f msec", name, msec);
}

enum image_type {
	IMAGE_TYPE_PNG,
	IMAGE_TYPE_JPG,
	IMAGE_TYPE_EXR,
	IMAGE_TYPE_WEBP,
	IMAGE_TYPE_GIF,
	IMAGE_TYPE_PPM,
	IMAGE_TYPE_TIFF,
	IMAGE_TYPE_SVG,

	IMAGE_TYPE_UNKNOWN // this must always be the last entry
};
static const char *module_names[IMAGE_TYPE_UNKNOWN] = {
	"png-load.so",
	"jpg-load.so",
	"exr-load.so",
	"webp-load.so",
	"gif-load.so",
	"ppm-load.so",
	"tiff-load.so",
	"svg-load.so",
};
static const char *op_names[IMAGE_TYPE_UNKNOWN] = {
	"gegl:png-load",
	"gegl:jpg-load",
	"gegl:exr-load",
	"gegl:webp-load",
	"gegl:gif-load",
	"gegl:ppm-load",
	"gegl:tiff-load",
	"gegl:svg-load",
};

static enum image_type guess_image_type(const char *path) {
	char *dot = strrchr(path, '.');
	if (!dot) {
		return IMAGE_TYPE_UNKNOWN;
	}
	dot += 1;
	if (strcmp(dot, "png") == 0 || strcmp(dot, "PNG") == 0) {
		return IMAGE_TYPE_PNG;
	} else if (strcmp(dot, "jpg") == 0 || strcmp(dot, "JPG") == 0) {
		return IMAGE_TYPE_JPG;
	} else if (strcmp(dot, "exr") == 0 || strcmp(dot, "EXR") == 0) {
		return IMAGE_TYPE_EXR;
	}else if (strcmp(dot, "webp") == 0 || strcmp(dot, "WEBP") == 0) {
		return IMAGE_TYPE_WEBP;
	}else if (strcmp(dot, "gif") == 0 || strcmp(dot, "GIF") == 0) {
		return IMAGE_TYPE_GIF;
	}else if (strcmp(dot, "ppm") == 0 || strcmp(dot, "PPM") == 0) {
		return IMAGE_TYPE_PPM;
	}else if (strcmp(dot, "tiff") == 0 || strcmp(dot, "TIFF") == 0 ||
			strcmp(dot, "tif") == 0 || strcmp(dot, "TIF") == 0) {
		return IMAGE_TYPE_TIFF;
	}else if (strcmp(dot, "svg") == 0 || strcmp(dot, "SVG") == 0) {
		return IMAGE_TYPE_SVG;
	}
	swaybg_log(LOG_ERROR, "Unidentified image file ending: %s", dot);
	return IMAGE_TYPE_UNKNOWN;
}

GeglBuffer *load_background_image(const char *path) {
	static bool load_modules[IMAGE_TYPE_UNKNOWN] = {0};
	enum image_type type = guess_image_type(path);
	if (type == IMAGE_TYPE_UNKNOWN) {
		return NULL;
	}
	const char *module_name = module_names[type],
		*op_name = op_names[type];
	if (!load_modules[type]) {
		load_gegl_module_library(module_name);
		load_modules[type] = true;
	}

	GeglNode *graph = gegl_node_new();
	if (!graph) {
		swaybg_log(LOG_ERROR, "Failed to allocate graph");
	}
	// TODO: special handling would be helpful for SVG images, to pick the
	// optimal size to render at
	GeglNode *load = gegl_node_new_child (graph,
		"operation", op_name, "path", path, NULL);
	if (!load) {
		swaybg_log(LOG_ERROR, "Failed to create load op");
	}
	GeglBuffer *buffer = NULL;
	GeglNode *sink = gegl_node_new_child (graph,
		"operation", "gegl:buffer-sink", "buffer", &buffer,
		NULL);
	if (!sink) {
		swaybg_log(LOG_ERROR, "Failed to create sink op");
	}
	gegl_node_link_many (load, sink, NULL);
	gegl_node_process (sink);
	if (!buffer) {
		swaybg_log(LOG_ERROR, "Failed to load buffer");
		return NULL;
	}
	g_object_unref(load);
	g_object_unref(sink);
	g_object_unref(graph);

	return buffer;
}

bool render_background_image(GeglBuffer *out, GeglBuffer *image,
		GeglColor *bg_color, enum background_mode mode) {
	const GeglRectangle *out_rect = gegl_buffer_get_extent(out);
	int buffer_width = out_rect->width, buffer_height = out_rect->height;

	GeglNode *graph = gegl_node_new();
	if (!graph) {
		swaybg_log(LOG_ERROR, "Failed to allocate graph");
		return false;
	}

	// TODO: optimizations for opaque images; use 'cast-format'?

	GeglNode *load_color = gegl_node_new_child (graph,
		"operation", "gegl:color", "value", bg_color, NULL);
	double crop_x = 0, crop_y = 0, crop_width = buffer_width, crop_height = buffer_height;
	GeglNode *crop_color = gegl_node_new_child (graph,
		"operation", "gegl:crop", "x", crop_x, "y", crop_y, "width", crop_width, "height", crop_height, NULL);
	GeglNode *sink = gegl_node_new_child (graph,
		"operation", "gegl:write-buffer", "buffer", out, NULL);
	gegl_node_link(load_color, crop_color);

	GeglNode *load_img = NULL, *proc_scale = NULL, *proc_translate = NULL,
		*proc_tile = NULL, *proc_cropimg = NULL, *proc_srcover = NULL;

	if (!load_color || !crop_color || !sink) {
		swaybg_log(LOG_ERROR, "Failed to create a GEGL node");
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
			swaybg_log(LOG_ERROR, "Failed to create tile node sequence");
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
			swaybg_log(LOG_ERROR, "Failed to create scale node sequence");
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

	return true;
}
