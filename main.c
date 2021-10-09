#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <ctype.h>
#include <getopt.h>
#include <gegl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wayland-client.h>
#include "background-image.h"
#include "log.h"
#include "pool-buffer.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

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
	bool has_abgr16f;
	const Babl* fmt_abgr16f;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct wl_list configs;  // struct swaybg_output_config::link
	struct wl_list outputs;  // struct swaybg_output::link
	struct wl_list images;   // struct swaybg_image::link
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
	int32_t committed_width, committed_height, committed_scale;

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

static void render_frame(struct swaybg_output *output, GeglBuffer *surface) {
	int buffer_width = output->width * output->scale,
		buffer_height = output->height * output->scale;

	// If the last committed buffer has the same size as this one would, do
	// not render a new buffer, because it will be identical to the old one
	if (output->committed_width == buffer_width &&
			output->committed_height == buffer_height) {
		if (output->committed_scale != output->scale) {
			wl_surface_set_buffer_scale(output->surface, output->scale);
			wl_surface_commit(output->surface);

			output->committed_scale = output->scale;
		}
		return;
	}

	bool use_high_bit_depth = false;
	if (surface) {
		const Babl *surf_fmt = gegl_buffer_get_format(surface);
		int components = babl_format_get_n_components(surf_fmt);
		for (int i=0;i<components;i++) {
			const Babl *ex_comp = babl_format_get_type(surf_fmt, 0);
			use_high_bit_depth = use_high_bit_depth || (ex_comp != babl_type("u8"));
		}
	}

	int32_t buffer_stride;
	const Babl* bablfmt;
	uint32_t shm_format;
	if (output->state->has_abgr16f && use_high_bit_depth) {
		buffer_stride = buffer_width * 8;
		shm_format = WL_SHM_FORMAT_ABGR16161616F;
		bablfmt = output->state->fmt_abgr16f;
	} else {
		buffer_stride = buffer_width * 4;
		shm_format = WL_SHM_FORMAT_ARGB8888;
		bablfmt = babl_format("B'aG'aR'aA u8");
		if (!bablfmt) {
			swaybg_log(LOG_ERROR, "Failed to create babl format\n");
			return;
		}
	}

	struct pool_buffer buffer;
	if (!create_buffer(&buffer, output->state->shm,
			buffer_width, buffer_height, buffer_stride, shm_format)) {
		return;
	}

	GeglColor *bg_color = gegl_color_new("black");
	if (!bg_color) {
		swaybg_log(LOG_ERROR, "Failed to create color\n");
	}
	float r = (output->config->color >> (3*8) & 0xFF) / 255.0;
	float g = (output->config->color >> (2*8) & 0xFF) / 255.0;
	float b = (output->config->color >> (1*8) & 0xFF) / 255.0;
	float a = 1.0; // override, must be opaque
	gegl_color_set_rgba(bg_color, r, g, b, a);

	GeglRectangle dest_extent = {
		.x = 0, .y = 0,
		.width = buffer_width, .height = buffer_height
	};
	GeglBuffer *dest = gegl_buffer_linear_new_from_data(
		buffer.data, bablfmt, &dest_extent, buffer_stride, NULL, NULL);
	if (!dest) {
		swaybg_log(LOG_ERROR, "failed to create destination buffer");
	}

	if (!render_background_image(dest, surface,
			bg_color, output->config->mode)) {
		swaybg_log(LOG_ERROR, "failed to render background image");
	}

	GeglRectangle rect = {.x = 0, .y = 0, .width = buffer_width, .height = buffer_height};
	const GeglRectangle *actual_rect = gegl_buffer_get_extent(dest);
	if (actual_rect->x != 0 || actual_rect->y != 0 || actual_rect->width != buffer_width || actual_rect->height != buffer_height) {
		swaybg_log(LOG_ERROR, "incorrect background image size: %d %d %d %d, expected 0,0,%d,%d",
			actual_rect->x, actual_rect->y,actual_rect->width, actual_rect->height, buffer_width, buffer_height);

	}
	gegl_buffer_get(dest, &rect, 1.0, bablfmt, buffer.data,
			buffer_stride, GEGL_ABYSS_NONE);

	g_object_unref(dest);
	g_object_unref(bg_color);

	wl_surface_set_buffer_scale(output->surface, output->scale);
	wl_surface_attach(output->surface, buffer.buffer, 0, 0);
	wl_surface_damage_buffer(output->surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(output->surface);

	output->committed_width = buffer_width;
	output->committed_height = buffer_height;
	output->committed_scale = output->scale;

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

static void output_geometry(void *data, struct wl_output *output, int32_t x,
		int32_t y, int32_t width_mm, int32_t height_mm, int32_t subpixel,
		const char *make, const char *model, int32_t transform) {
	// Who cares
}

static void output_mode(void *data, struct wl_output *output, uint32_t flags,
		int32_t width, int32_t height, int32_t refresh) {
	// Who cares
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

static void output_done(void *data, struct wl_output *wl_output) {
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

static void output_scale(void *data, struct wl_output *wl_output,
		int32_t scale) {
	struct swaybg_output *output = data;
	output->scale = scale;
	if (output->state->run_display && output->width > 0 && output->height > 0) {
		output->dirty = true;
	}
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

static void output_name(void *data, struct wl_output *wl_output,
		const char *name) {
	struct swaybg_output *output = data;
	output->name = strdup(name);

	// If description was sent first, the config may already be populated. If
	// there is an identifier config set, keep it.
	if (!output->config || strcmp(output->config->output, "*") == 0) {
		find_config(output, name);
	}
}

static void output_description(void *data, struct wl_output *wl_output,
		const char *description) {
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

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
	.name = output_name,
	.description = output_description,
};

static void shm_handle_format(void *data, struct wl_shm *wl_shm, uint32_t format) {
	struct swaybg_state *state = data;
	if (format == WL_SHM_FORMAT_ABGR16161616F) {
		state->has_abgr16f = true;
	}
}

static const struct wl_shm_listener shm_listener = {
	.format = shm_handle_format,
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
			wl_registry_bind(registry, name, &wl_output_interface, 4);
		wl_output_add_listener(output->wl_output, &output_listener, output);
		wl_list_insert(&state->outputs, &output->link);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		state->layer_shell =
			wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
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

	gint gegl_argc = 0;
	gchar **gegl_argv = NULL;
	gegl_init(&gegl_argc, &gegl_argv);

	struct swaybg_state state = {0};
	wl_list_init(&state.configs);
	wl_list_init(&state.outputs);
	wl_list_init(&state.images);
	state.fmt_abgr16f = babl_format_new (babl_model ("R'G'B'A"), babl_type ("half"),
		babl_component ("R'"), babl_component ("G'"), babl_component ("B'"), babl_component ("A"),
		NULL);

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
	if (wl_display_roundtrip(state.display) < 0) {
		swaybg_log(LOG_ERROR, "wl_display_roundtrip failed");
		return 1;
	}
	if (state.compositor == NULL || state.shm == NULL ||
			state.layer_shell == NULL) {
		swaybg_log(LOG_ERROR, "Missing a required Wayland interface");
		return 1;
	}

	state.run_display = true;
	while (wl_display_dispatch(state.display) != -1 && state.run_display) {
		// Send acks, and determine which images need to be loaded
		struct swaybg_output *output;
		wl_list_for_each(output, &state.outputs, link) {
			if (output->needs_ack) {
				output->needs_ack = false;
				zwlr_layer_surface_v1_ack_configure(
						output->layer_surface,
						output->configure_serial);
			}

			int buffer_width = output->width * output->scale,
				buffer_height = output->height * output->scale;
			bool buffer_change =
				output->committed_height != buffer_height ||
				output->committed_width != buffer_width;
			if (output->dirty && output->config->image && buffer_change) {
				output->config->image->load_required = true;
			}
		}

		// Load images, render associated frames, and unload
		wl_list_for_each(image, &state.images, link) {
			if (!image->load_required) {
				continue;
			}

			GeglBuffer *surface = load_background_image(image->path);
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
			g_object_unref(surface);
		}

		// Redraw outputs without associated image
		wl_list_for_each(output, &state.outputs, link) {
			if (output->dirty) {
				output->dirty = false;
				render_frame(output, NULL);
			}
		}
	}

	struct swaybg_output *output, *tmp_output;
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

	gegl_exit();

	return 0;
}
