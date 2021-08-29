#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <ctype.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wayland-client.h>
#include "background-image.h"
#include "cairo_util.h"
#include "log.h"
#include "pool-buffer.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

static uint32_t parse_color(const char *color) {
	if (color[0] == '#') {
		++color;
	}

	int len = strlen(color);
	if (len != 6 && len != 8) {
		swaybg_log(LOG_DEBUG, "Invalid color %s, defaulting to 0xFFFFFFFF",
				color);
		return 0xFFFFFFFF;
	}
	uint32_t res = (uint32_t)strtoul(color, NULL, 16);
	if (strlen(color) == 6) {
		res = (res << 8) | 0xFF;
	}
	return res;
}

struct swaybg_state {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct zxdg_output_manager_v1 *xdg_output_manager;
	struct wl_list configs;  // struct swaybg_output_config::link
	struct wl_list outputs;  // struct swaybg_output::link
	struct wl_list images;   // struct swaybg_image::link
	uint32_t highest_depth_format;
	bool run_display;
};

struct swaybg_image {
	struct wl_list link;
	const char *path;
	bool load_required;
};

struct swaybg_output_config {
	char *output;
	const char *image_path;
	struct swaybg_image *image;
	enum background_mode mode;
	uint32_t color;
	struct wl_list link;
};

struct swaybg_output {
	uint32_t wl_name;
	struct wl_output *wl_output;
	struct zxdg_output_v1 *xdg_output;
	char *name;
	char *identifier;

	struct swaybg_state *state;
	struct swaybg_output_config *config;

	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;

	uint32_t width, height;
	int32_t scale;

	uint32_t configure_serial;
	bool dirty, needs_ack;

	struct wl_list link;
};

bool is_valid_color(const char *color) {
	int len = strlen(color);
	if (len != 7 || color[0] != '#') {
		swaybg_log(LOG_ERROR, "%s is not a valid color for swaybg. "
				"Color should be specified as #rrggbb (no alpha).", color);
		return false;
	}

	int i;
	for (i = 1; i < len; ++i) {
		if (!isxdigit(color[i])) {
			return false;
		}
	}

	return true;
}


/* taken from pixman which copied off Mesa ; can be replaced if pixman
 * ever merges its float16 patch */
static uint16_t
convert_float_to_half(float f)
{
	uint32_t sign_mask  = 0x80000000;
	uint32_t round_mask = ~0xfff;
	uint32_t f32inf = 0xff << 23;
	uint32_t f16inf = 0x1f << 23;
	uint32_t sign;
	union {
		float f;
		uint32_t ui;
	} magic, f32;
	uint16_t f16;

	magic.ui = 0xf << 23;

	 f32.f = f;

	/* Sign */
	sign = f32.ui & sign_mask;
	f32.ui ^= sign;

	if (f32.ui == f32inf) {
		/* Inf */
		f16 = 0x7c00;
	} else if (f32.ui > f32inf) {
		/* NaN */
		f16 = 0x7e00;
	} else {
		/* Number */
		f32.ui &= round_mask;
		f32.f  *= magic.f;
		f32.ui -= round_mask;
		/*
		 * XXX: The magic mul relies on denorms being available, otherwise all
		 * f16 denorms get flushed to zero.
		 */
		/*
		 * Clamp to max finite value if overflowed.
		 * OpenGL has completely undefined rounding behavior for float to
		 * half-float conversions, and this matches what is mandated for float
		 * to fp11/fp10, which recommend round-to-nearest-finite too.
		 * (d3d10 is deeply unhappy about flushing such values to infinity, and
		 * while it also mandates round-to-zero it doesn't care nearly as much
		 * about that.)
		 *
		 * pixman doesn't have any preconceptions, so let's do what Mesa does.
		 */
		if (f32.ui > f16inf)
			f32.ui = f16inf - 1;

		f16 = f32.ui >> 13;
	}

	/* Sign */
	f16 |= sign >> 16;

	return f16;
}

static void render_frame(struct swaybg_output *output, cairo_surface_t *surface) {
	int buffer_width = output->width * output->scale,
		buffer_height = output->height * output->scale;

	struct pool_buffer buffer;
	if (!create_buffer(&buffer, output->state->shm,
			buffer_width, buffer_height, output->state->highest_depth_format)) {
		return;
	}
	uint8_t *data;
	cairo_format_t format;
	int stride;
	if (output->state->highest_depth_format == WL_SHM_FORMAT_XBGR16161616F) {
		data = calloc(buffer_height, buffer_width * 12);
		format = CAIRO_FORMAT_RGB96F;
		stride = 12 * buffer_width;
	} else if (output->state->highest_depth_format == WL_SHM_FORMAT_XBGR2101010) {
		// todo: this needs R,B swap
		data = buffer.data;
		format = CAIRO_FORMAT_RGB30;
		stride = buffer.stride;
	} else {
		data = buffer.data;
		format = CAIRO_FORMAT_RGB24;
		stride = buffer.stride;
	}

	cairo_surface_t *dest = cairo_image_surface_create_for_data(data,
			format, buffer_width, buffer_height, stride);
	if (cairo_surface_status(dest) != CAIRO_STATUS_SUCCESS) {
		swaybg_log(LOG_ERROR, "Failed to make surface\n");
	}
	cairo_t *cairo = cairo_create(dest);
	cairo_save(cairo);
	cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cairo);
	cairo_restore(cairo);
	if (output->config->mode == BACKGROUND_MODE_SOLID_COLOR) {
		cairo_set_source_u32(cairo, output->config->color);
		cairo_paint(cairo);
	} else {
		if (output->config->color) {
			cairo_set_source_u32(cairo, output->config->color);
			cairo_paint(cairo);
		}

		if (surface) {
			render_background_image(cairo, surface,
				output->config->mode, buffer_width, buffer_height);
		}
	}

	if (output->state->highest_depth_format == WL_SHM_FORMAT_XBGR16161616F) {
		float *fdata = (float *)data;
		for (int y = 0; y < buffer_height; y++) {
			uint64_t *row = (uint64_t *)((uint8_t*)buffer.data + buffer.stride * y);
			for (int x = 0; x < buffer_width; x++) {
				float r = fdata[y * buffer_width * 3 + 3 * x + 0];
				float g = fdata[y * buffer_width * 3 + 3 * x + 1];
				float b = fdata[y * buffer_width * 3 + 3 * x + 2];
				uint64_t ur = convert_float_to_half(r);
				uint64_t ug = convert_float_to_half(g);
				uint64_t ub = convert_float_to_half(b);
				row[x] = (ur << 0) | (ub << 32) | (ug << 16);
			}
		}
		/* TODO: do a transfer */
		free(data);
	}

	cairo_destroy(cairo);
	cairo_surface_destroy(dest);


	wl_surface_set_buffer_scale(output->surface, output->scale);
	wl_surface_attach(output->surface, buffer.buffer, 0, 0);
	wl_surface_damage_buffer(output->surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(output->surface);
	// we will not reuse the buffer, so destroy it immediately
	destroy_buffer(&buffer);
}

static void destroy_swaybg_image(struct swaybg_image *image) {
	if (!image) {
		return;
	}
	wl_list_remove(&image->link);
	free(image);
}

static void destroy_swaybg_output_config(struct swaybg_output_config *config) {
	if (!config) {
		return;
	}
	wl_list_remove(&config->link);
	free(config->output);
	free(config);
}

static void destroy_swaybg_output(struct swaybg_output *output) {
	if (!output) {
		return;
	}
	wl_list_remove(&output->link);
	if (output->layer_surface != NULL) {
		zwlr_layer_surface_v1_destroy(output->layer_surface);
	}
	if (output->surface != NULL) {
		wl_surface_destroy(output->surface);
	}
	zxdg_output_v1_destroy(output->xdg_output);
	wl_output_destroy(output->wl_output);
	free(output->name);
	free(output->identifier);
	free(output);
}

static void layer_surface_configure(void *data,
		struct zwlr_layer_surface_v1 *surface,
		uint32_t serial, uint32_t width, uint32_t height) {
	struct swaybg_output *output = data;
	output->width = width;
	output->height = height;
	output->dirty = true;
	output->configure_serial = serial;
	output->needs_ack = true;
}

static void layer_surface_closed(void *data,
		struct zwlr_layer_surface_v1 *surface) {
	struct swaybg_output *output = data;
	swaybg_log(LOG_DEBUG, "Destroying output %s (%s)",
			output->name, output->identifier);
	destroy_swaybg_output(output);
}
static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

static void shm_format(void *data, struct wl_shm *wl_shm, uint32_t format) {
	struct swaybg_state *state = data;
	/* Since background images are typically only rendered once, always
	 * prefer the highest bit depth provided */
	uint32_t order[] = {
		WL_SHM_FORMAT_XRGB8888,
		WL_SHM_FORMAT_XBGR2101010,
		WL_SHM_FORMAT_XBGR16161616F
	};
	size_t old_rank = 0, this_rank = 0;
	for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
		if (order[i] == state->highest_depth_format) {
			old_rank = i;
		}
		if (order[i] == format) {
			this_rank = i;
		}
	}
	if (this_rank > old_rank) {
		state->highest_depth_format = format;
	}
}

static const struct wl_shm_listener shm_listener = {
	.format = shm_format,
};

static void output_geometry(void *data, struct wl_output *output, int32_t x,
		int32_t y, int32_t width_mm, int32_t height_mm, int32_t subpixel,
		const char *make, const char *model, int32_t transform) {
	// Who cares
}

static void output_mode(void *data, struct wl_output *output, uint32_t flags,
		int32_t width, int32_t height, int32_t refresh) {
	// Who cares
}

static void output_done(void *data, struct wl_output *output) {
	// Who cares
}

static void output_scale(void *data, struct wl_output *wl_output,
		int32_t scale) {
	struct swaybg_output *output = data;
	output->scale = scale;
	if (output->state->run_display && output->width > 0 && output->height > 0) {
		output->dirty = true;
	}
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
};

static void xdg_output_handle_logical_position(void *data,
		struct zxdg_output_v1 *xdg_output, int32_t x, int32_t y) {
	// Who cares
}

static void xdg_output_handle_logical_size(void *data,
		struct zxdg_output_v1 *xdg_output, int32_t width, int32_t height) {
	// Who cares
}

static void find_config(struct swaybg_output *output, const char *name) {
	struct swaybg_output_config *config = NULL;
	wl_list_for_each(config, &output->state->configs, link) {
		if (strcmp(config->output, name) == 0) {
			output->config = config;
			return;
		} else if (!output->config && strcmp(config->output, "*") == 0) {
			output->config = config;
		}
	}
}

static void xdg_output_handle_name(void *data,
		struct zxdg_output_v1 *xdg_output, const char *name) {
	struct swaybg_output *output = data;
	output->name = strdup(name);

	// If description was sent first, the config may already be populated. If
	// there is an identifier config set, keep it.
	if (!output->config || strcmp(output->config->output, "*") == 0) {
		find_config(output, name);
	}
}

static void xdg_output_handle_description(void *data,
		struct zxdg_output_v1 *xdg_output, const char *description) {
	struct swaybg_output *output = data;

	// wlroots currently sets the description to `make model serial (name)`
	// If this changes in the future, this will need to be modified.
	char *paren = strrchr(description, '(');
	if (paren) {
		size_t length = paren - description;
		output->identifier = malloc(length);
		if (!output->identifier) {
			swaybg_log(LOG_ERROR, "Failed to allocate output identifier");
			return;
		}
		strncpy(output->identifier, description, length);
		output->identifier[length - 1] = '\0';

		find_config(output, output->identifier);
	}
}

static void create_layer_surface(struct swaybg_output *output) {
	output->surface = wl_compositor_create_surface(output->state->compositor);
	assert(output->surface);

	// Empty input region
	struct wl_region *input_region =
		wl_compositor_create_region(output->state->compositor);
	assert(input_region);
	wl_surface_set_input_region(output->surface, input_region);
	wl_region_destroy(input_region);

	output->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
			output->state->layer_shell, output->surface, output->wl_output,
			ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "wallpaper");
	assert(output->layer_surface);

	zwlr_layer_surface_v1_set_size(output->layer_surface, 0, 0);
	zwlr_layer_surface_v1_set_anchor(output->layer_surface,
			ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
	zwlr_layer_surface_v1_set_exclusive_zone(output->layer_surface, -1);
	zwlr_layer_surface_v1_add_listener(output->layer_surface,
			&layer_surface_listener, output);
	wl_surface_commit(output->surface);
}

static void xdg_output_handle_done(void *data,
		struct zxdg_output_v1 *xdg_output) {
	struct swaybg_output *output = data;
	if (!output->config) {
		swaybg_log(LOG_DEBUG, "Could not find config for output %s (%s)",
				output->name, output->identifier);
		destroy_swaybg_output(output);
	} else if (!output->layer_surface) {
		swaybg_log(LOG_DEBUG, "Found config %s for output %s (%s)",
				output->config->output, output->name, output->identifier);
		create_layer_surface(output);
	}
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
	.logical_position = xdg_output_handle_logical_position,
	.logical_size = xdg_output_handle_logical_size,
	.name = xdg_output_handle_name,
	.description = xdg_output_handle_description,
	.done = xdg_output_handle_done,
};

static void handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct swaybg_state *state = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		state->compositor =
			wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
		wl_shm_add_listener(state->shm, &shm_listener, state);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct swaybg_output *output = calloc(1, sizeof(struct swaybg_output));
		output->state = state;
		output->wl_name = name;
		output->wl_output =
			wl_registry_bind(registry, name, &wl_output_interface, 3);
		wl_output_add_listener(output->wl_output, &output_listener, output);
		wl_list_insert(&state->outputs, &output->link);

		if (state->run_display) {
			output->xdg_output = zxdg_output_manager_v1_get_xdg_output(
				state->xdg_output_manager, output->wl_output);
			zxdg_output_v1_add_listener(output->xdg_output,
				&xdg_output_listener, output);
		}
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		state->layer_shell =
			wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
		state->xdg_output_manager = wl_registry_bind(registry, name,
			&zxdg_output_manager_v1_interface, 2);
	}
}

static void handle_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
	struct swaybg_state *state = data;
	struct swaybg_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &state->outputs, link) {
		if (output->wl_name == name) {
			swaybg_log(LOG_DEBUG, "Destroying output %s (%s)",
					output->name, output->identifier);
			destroy_swaybg_output(output);
			break;
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

static bool store_swaybg_output_config(struct swaybg_state *state,
		struct swaybg_output_config *config) {
	struct swaybg_output_config *oc = NULL;
	wl_list_for_each(oc, &state->configs, link) {
		if (strcmp(config->output, oc->output) == 0) {
			// Merge on top
			if (config->image_path) {
				oc->image_path = config->image_path;
			}
			if (config->color) {
				oc->color = config->color;
			}
			if (config->mode != BACKGROUND_MODE_INVALID) {
				oc->mode = config->mode;
			}
			return false;
		}
	}
	// New config, just add it
	wl_list_insert(&state->configs, &config->link);
	return true;
}

static void parse_command_line(int argc, char **argv,
		struct swaybg_state *state) {
	static struct option long_options[] = {
		{"color", required_argument, NULL, 'c'},
		{"help", no_argument, NULL, 'h'},
		{"image", required_argument, NULL, 'i'},
		{"mode", required_argument, NULL, 'm'},
		{"output", required_argument, NULL, 'o'},
		{"version", no_argument, NULL, 'v'},
		{0, 0, 0, 0}
	};

	const char *usage =
		"Usage: swaybg <options...>\n"
		"\n"
		"  -c, --color            Set the background color.\n"
		"  -h, --help             Show help message and quit.\n"
		"  -i, --image            Set the image to display.\n"
		"  -m, --mode             Set the mode to use for the image.\n"
		"  -o, --output           Set the output to operate on or * for all.\n"
		"  -v, --version          Show the version number and quit.\n"
		"\n"
		"Background Modes:\n"
		"  stretch, fit, fill, center, tile, or solid_color\n";

	struct swaybg_output_config *config = calloc(sizeof(struct swaybg_output_config), 1);
	config->output = strdup("*");
	config->mode = BACKGROUND_MODE_INVALID;
	wl_list_init(&config->link); // init for safe removal

	int c;
	while (1) {
		int option_index = 0;
		c = getopt_long(argc, argv, "c:hi:m:o:v", long_options, &option_index);
		if (c == -1) {
			break;
		}
		switch (c) {
		case 'c':  // color
			if (!is_valid_color(optarg)) {
				swaybg_log(LOG_ERROR, "Invalid color: %s", optarg);
				continue;
			}
			config->color = parse_color(optarg);
			break;
		case 'i':  // image
			config->image_path = optarg;
			break;
		case 'm':  // mode
			config->mode = parse_background_mode(optarg);
			if (config->mode == BACKGROUND_MODE_INVALID) {
				swaybg_log(LOG_ERROR, "Invalid mode: %s", optarg);
			}
			break;
		case 'o':  // output
			if (config && !store_swaybg_output_config(state, config)) {
				// Empty config or merged on top of an existing one
				destroy_swaybg_output_config(config);
			}
			config = calloc(sizeof(struct swaybg_output_config), 1);
			config->output = strdup(optarg);
			config->mode = BACKGROUND_MODE_INVALID;
			wl_list_init(&config->link);  // init for safe removal
			break;
		case 'v':  // version
			fprintf(stdout, "swaybg version " SWAYBG_VERSION "\n");
			exit(EXIT_SUCCESS);
			break;
		default:
			fprintf(c == 'h' ? stdout : stderr, "%s", usage);
			exit(c == 'h' ? EXIT_SUCCESS : EXIT_FAILURE);
		}
	}
	if (config && !store_swaybg_output_config(state, config)) {
		// Empty config or merged on top of an existing one
		destroy_swaybg_output_config(config);
	}

	// Check for invalid options
	if (optind < argc) {
		config = NULL;
		struct swaybg_output_config *tmp = NULL;
		wl_list_for_each_safe(config, tmp, &state->configs, link) {
			destroy_swaybg_output_config(config);
		}
		// continue into empty list
	}
	if (wl_list_empty(&state->configs)) {
		fprintf(stderr, "%s", usage);
		exit(EXIT_FAILURE);
	}

	// Set default mode and remove empties
	config = NULL;
	struct swaybg_output_config *tmp = NULL;
	wl_list_for_each_safe(config, tmp, &state->configs, link) {
		if (!config->image_path && !config->color) {
			destroy_swaybg_output_config(config);
		} else if (config->mode == BACKGROUND_MODE_INVALID) {
			config->mode = config->image_path
				? BACKGROUND_MODE_STRETCH
				: BACKGROUND_MODE_SOLID_COLOR;
		}
	}
}

int main(int argc, char **argv) {
	swaybg_log_init(LOG_DEBUG);

	struct swaybg_state state = {0};
	state.highest_depth_format = WL_SHM_FORMAT_XRGB8888;
	wl_list_init(&state.configs);
	wl_list_init(&state.outputs);
	wl_list_init(&state.images);

	parse_command_line(argc, argv, &state);

	// Identify distinct image paths which will need to be loaded
	struct swaybg_image *image;
	struct swaybg_output_config *config;
	wl_list_for_each(config, &state.configs, link) {
		if (!config->image_path) {
			continue;
		}
		wl_list_for_each(image, &state.images, link) {
			if (strcmp(image->path, config->image_path) == 0) {
				config->image = image;
				break;
			}
		}
		if (config->image) {
			continue;
		}
		image = calloc(1, sizeof(struct swaybg_image));
		image->path = config->image_path;
		wl_list_insert(&state.images, &image->link);
		config->image = image;
	}

	state.display = wl_display_connect(NULL);
	if (!state.display) {
		swaybg_log(LOG_ERROR, "Unable to connect to the compositor. "
				"If your compositor is running, check or set the "
				"WAYLAND_DISPLAY environment variable.");
		return 1;
	}

	struct wl_registry *registry = wl_display_get_registry(state.display);
	wl_registry_add_listener(registry, &registry_listener, &state);
	wl_display_roundtrip(state.display);
	if (state.compositor == NULL || state.shm == NULL ||
			state.layer_shell == NULL || state.xdg_output_manager == NULL) {
		swaybg_log(LOG_ERROR, "Missing a required Wayland interface");
		return 1;
	}

	struct swaybg_output *output;
	wl_list_for_each(output, &state.outputs, link) {
		output->xdg_output = zxdg_output_manager_v1_get_xdg_output(
			state.xdg_output_manager, output->wl_output);
		zxdg_output_v1_add_listener(output->xdg_output,
			&xdg_output_listener, output);
	}

	state.run_display = true;
	while (wl_display_dispatch(state.display) != -1 && state.run_display) {
		// Send acks, and determine which images need to be loaded
		wl_list_for_each(output, &state.outputs, link) {
			if (output->needs_ack) {
				output->needs_ack = false;
				zwlr_layer_surface_v1_ack_configure(
						output->layer_surface,
						output->configure_serial);
			}

			if (output->dirty && output->config->image) {
				output->config->image->load_required = true;
			}
		}

		// Load images, render associated frames, and unload
		wl_list_for_each(image, &state.images, link) {
			if (!image->load_required) {
				continue;
			}

			cairo_surface_t *surface = load_background_image(image->path);
			if (!surface) {
				swaybg_log(LOG_ERROR, "Failed to load image: %s", image->path);
				continue;
			}

			wl_list_for_each(output, &state.outputs, link) {
				if (output->dirty && output->config->image == image) {
					output->dirty = false;
					render_frame(output, surface);
				}
			}

			image->load_required = false;
			cairo_surface_destroy(surface);
		}

		// Redraw outputs without associated image
		wl_list_for_each(output, &state.outputs, link) {
			if (output->dirty) {
				output->dirty = false;
				render_frame(output, NULL);
			}
		}
	}

	struct swaybg_output *tmp_output;
	wl_list_for_each_safe(output, tmp_output, &state.outputs, link) {
		destroy_swaybg_output(output);
	}

	struct swaybg_output_config *tmp_config = NULL;
	wl_list_for_each_safe(config, tmp_config, &state.configs, link) {
		destroy_swaybg_output_config(config);
	}

	struct swaybg_image *tmp_image;
	wl_list_for_each_safe(image, tmp_image, &state.images, link) {
		destroy_swaybg_image(image);
	}

	return 0;
}
