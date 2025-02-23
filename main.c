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

#include "color-management-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "single-pixel-buffer-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"

/*
 * If `color` is a hexadecimal string of the form 'rrggbb' or '#rrggbb',
 * `*result` will be set to the uint32_t version of the color. Otherwise,
 * return false and leave `*result` unmodified.
 */
static bool parse_color(const char *color, uint32_t *result) {
	if (color[0] == '#') {
		++color;
	}

	int len = strlen(color);
	if (len != 6) {
		return false;
	}
	for (int i = 0; i < len; ++i) {
		if (!isxdigit(color[i])) {
			return false;
		}
	}

	uint32_t val = (uint32_t)strtoul(color, NULL, 16);
	*result = (val << 8) | 0xFF;
	return true;
}

struct swaybg_state {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct wp_viewporter *viewporter;
	struct wp_single_pixel_buffer_manager_v1 *single_pixel_buffer_manager;
	struct wp_fractional_scale_manager_v1 *fract_scale_manager;
	struct wp_color_manager_v1 *color_manager;
	struct wl_list configs;  // struct swaybg_output_config::link
	struct wl_list outputs;  // struct swaybg_output::link
	struct wl_list images;   // struct swaybg_image::link
	/* The list of all image descriptions for outputs pending commit */
	struct wl_list image_descs; // struct swaybg_image_desc::link
	/* The list of all rendered buffers for outputs pending commit */
	struct wl_list rendered_buffers; // struct swaybg_rendered_buffer::link
	bool run_display;
	/* Supported deep buffer types */
	bool has_xrgb2101010;
	bool has_xbgr2101010;
	/* Color management features and capabilities */
	bool has_parametric;
	bool supported_intents[8];
	bool supported_named_primaries[16];
	bool supported_named_tfs[16];
	bool color_info_done;
};

struct swaybg_image {
	struct wl_list link;
	const char *path;
	bool load_required;
	/* Images are loaded each time a frame is rendered. */
	uint64_t load_sequence_number;
};

enum image_desc_state {
	IMAGE_DESC_WAITING,
	IMAGE_DESC_READY,
	IMAGE_DESC_FAILED,
};

struct swaybg_image_desc {
	struct wl_list link;
	uint32_t reference_count;

	struct wp_image_description_v1 *description;
	enum image_desc_state state;

	/* Parameters that uniquely identify the image description; used to match
	 * it so that different outputs can share a description and do not need
	 * to wait for it to be ready. */
	enum wp_color_manager_v1_primaries primaries;
	enum wp_color_manager_v1_transfer_function transfer;
};

/* Parameters that uniquely determine the contents of a rendered buffer */
struct swaybg_buffer_spec {
	struct swaybg_image *image;
	uint64_t load_sequence_number;
	enum background_mode mode;
	/* The width and height _of the buffer_; may be 1x1 if solid_color + viewporter used */
	uint32_t width, height;
	uint32_t color;
};

struct swaybg_rendered_buffer {
	struct wl_list link;
	uint32_t reference_count;

	struct wl_buffer *buffer;

	struct swaybg_buffer_spec spec;
};

struct swaybg_output_config {
	char *output;
	const char *image_path;
	struct swaybg_image *image;
	enum background_mode mode;
	uint32_t color;
	struct wl_list link;
};

struct swaybg_output_state {
	uint32_t configure_serial;
	uint32_t width, height;
	int32_t scale;
	uint32_t pref_fract_scale;
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
	struct wp_viewport *viewport;
	struct wp_fractional_scale_v1 *fract_scale;
	struct wp_color_management_surface_v1 *color_surface;

	/* The last received state of the output */
	struct swaybg_output_state next_state;
	/* The state of the output that has been acknowledged */
	struct swaybg_output_state acked_state;
	/* If not NULL, wl_buffer to apply at next commit, matching acked_state */
	struct swaybg_rendered_buffer *queued_buffer;
	/* If not NULL, image description to apply at next commit (once it is ready) */
	struct swaybg_image_desc *queued_image_desc;

	/* scratch variables, computed in main loop */
	bool needs_new_buffer;
	bool needs_commit;

	struct wl_list link;
};

// Create a wl_buffer with the specified dimensions and content
static struct wl_buffer *draw_buffer(struct swaybg_state *state,
		const struct swaybg_buffer_spec *spec, cairo_surface_t *surface) {
	uint32_t bg_color = spec->color ? spec->color : 0x000000ff;

	if (spec->width == 1 && spec->height == 1 &&
			spec->mode == BACKGROUND_MODE_SOLID_COLOR &&
			state->single_pixel_buffer_manager) {
		// create and return single pixel buffer
		uint8_t r8 = (bg_color >> 24) & 0xFF;
		uint8_t g8 = (bg_color >> 16) & 0xFF;
		uint8_t b8 = (bg_color >> 8) & 0xFF;
		uint32_t f = 0xFFFFFFFF / 0xFF; // division result is an integer
		uint32_t r32 = r8 * f;
		uint32_t g32 = g8 * f;
		uint32_t b32 = b8 * f;
		return wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
			state->single_pixel_buffer_manager,
			r32, g32, b32, 0xFFFFFFFF);
	}

	bool deep_image = false;
	if (surface) {
		cairo_format_t fmt = cairo_image_surface_get_format(surface);
		deep_image = deep_image || fmt == CAIRO_FORMAT_RGB30;
#if CAIRO_VERSION >= CAIRO_VERSION_ENCODE(1, 17, 2)
		deep_image = deep_image || fmt == CAIRO_FORMAT_RGB96F;
		deep_image = deep_image || fmt == CAIRO_FORMAT_RGBA128F;
#endif
	}

	uint32_t format = WL_SHM_FORMAT_XRGB8888;
	if (deep_image && state->has_xrgb2101010) {
		format = WL_SHM_FORMAT_XRGB2101010;
	} else if (deep_image && state->has_xbgr2101010) {
		format = WL_SHM_FORMAT_XBGR2101010;
	}

	struct pool_buffer buffer;
	if (!create_buffer(&buffer, state->shm,
			spec->width, spec->height, format)) {
		return NULL;
	}

	cairo_t *cairo = buffer.cairo;
	cairo_set_source_u32(cairo, bg_color);
	cairo_paint(cairo);

	if (surface) {
		render_background_image(cairo, surface,
			spec->mode, spec->width, spec->height);
	}

	if (format == WL_SHM_FORMAT_XBGR2101010) {
		cairo_rgb30_swap_rb(buffer.surface);
	}

	// return wl_buffer for caller to use and destroy
	struct wl_buffer *wl_buf = buffer.buffer;
	buffer.buffer = NULL;
	destroy_buffer(&buffer);
	return wl_buf;
}

#define FRACT_DENOM 120

// Return the size of the buffer that should be attached to this output
static void get_buffer_size(const struct swaybg_output_state *state,
		enum background_mode mode, bool has_viewporter,
		uint32_t *buffer_width, uint32_t *buffer_height) {
	if (mode == BACKGROUND_MODE_SOLID_COLOR) {
		if (has_viewporter) {
			*buffer_width = 1;
			*buffer_height = 1;
		} else {
			*buffer_width = state->width;
			*buffer_height = state->height;
		}
	} else {
		if (state->pref_fract_scale && has_viewporter) {
			// rounding mode is 'round half up'
			*buffer_width = (state->width * state->pref_fract_scale +
				FRACT_DENOM / 2) / FRACT_DENOM;
			*buffer_height = (state->height * state->pref_fract_scale +
				FRACT_DENOM / 2) / FRACT_DENOM;
		} else {
			*buffer_width = state->width * state->scale;
			*buffer_height = state->height * state->scale;
		}
	}
}

static struct swaybg_rendered_buffer *get_or_construct_rendered_buffer(
		struct swaybg_state *state, const struct swaybg_buffer_spec *spec,
		cairo_surface_t *surface) {
	struct swaybg_rendered_buffer *buf;
	wl_list_for_each(buf, &state->rendered_buffers, link) {
		if (buf->spec.image == spec->image &&
				buf->spec.color == spec->color &&
				buf->spec.width == spec->width &&
				buf->spec.height == spec->height &&
				buf->spec.mode == spec->mode &&
				buf->spec.load_sequence_number == spec->load_sequence_number) {
			buf->reference_count++;
			return buf;
		}
	}

	buf = calloc(1, sizeof(*buf));
	assert(buf);
	buf->reference_count = 1;
	wl_list_insert(&state->rendered_buffers, &buf->link);
	buf->spec = *spec;
	buf->buffer = draw_buffer(state, spec, surface);
	return buf;
}

static void image_desc_failed(void *data,
		struct wp_image_description_v1 *wp_image_description_v1,
		uint32_t cause, const char *msg) {
	struct swaybg_image_desc *desc = data;
	swaybg_log(LOG_ERROR, "Failed to create image description for tf=%d, primaries=%d: cause=%d, msg=%s",
		desc->transfer, desc->primaries, cause, msg);
	assert(desc->state == IMAGE_DESC_WAITING);
	desc->state = IMAGE_DESC_FAILED;
}

static void image_desc_ready(void *data,
		struct wp_image_description_v1 *wp_image_description_v1,
		uint32_t identity) {
	struct swaybg_image_desc *desc = data;
	assert(desc->state == IMAGE_DESC_WAITING);
	desc->state = IMAGE_DESC_READY;
}

static const struct wp_image_description_v1_listener image_desc_listener = {
	.failed = image_desc_failed,
	.ready = image_desc_ready,
};

static struct swaybg_image_desc *get_or_construct_image_desc(struct swaybg_state *state,
		enum wp_color_manager_v1_transfer_function tf, enum wp_color_manager_v1_primaries primaries) {
	struct swaybg_image_desc *desc;
	wl_list_for_each(desc, &state->image_descs, link) {
		if (desc->primaries == primaries && desc->transfer == tf) {
			desc->reference_count++;
			return desc;
		}
	}

	if (!state->color_manager) {
		return NULL;
	}
	if (tf >= sizeof(state->supported_named_tfs) / sizeof(state->supported_named_tfs[0]) ||
			!state->supported_named_tfs[tf]) {
		swaybg_log(LOG_ERROR, "Failed to create image description, named transfer function %d not supported", tf);
		return NULL;
	}
	if (primaries >= sizeof(state->supported_named_primaries) / sizeof(state->supported_named_primaries[0]) ||
			!state->supported_named_primaries[primaries]) {
		swaybg_log(LOG_ERROR, "Failed to create image description, named primaries %d not supported", primaries);
		return NULL;
	}
	struct wp_image_description_creator_params_v1 *params =
		wp_color_manager_v1_create_parametric_creator(state->color_manager);
	wp_image_description_creator_params_v1_set_primaries_named(params, primaries);
	wp_image_description_creator_params_v1_set_tf_named(params, tf);

	desc = calloc(1, sizeof(*desc));
	assert(desc);
	wl_list_insert(&state->image_descs, &desc->link);
	desc->description = wp_image_description_creator_params_v1_create(params);
	wp_image_description_v1_add_listener(desc->description, &image_desc_listener, desc);
	desc->primaries = primaries;
	desc->transfer = tf;
	desc->state = IMAGE_DESC_WAITING;
	desc->reference_count = 1;
	return desc;

}

static void commit_frame(struct swaybg_output *output, struct swaybg_rendered_buffer *buffer, struct swaybg_image_desc *desc) {
	if (buffer) {
		wl_surface_attach(output->surface, buffer->buffer, 0, 0);
		wl_surface_damage_buffer(output->surface, 0, 0,
			buffer->spec.width, buffer->spec.height);
	}
	if (output->viewport) {
		wp_viewport_set_destination(output->viewport, output->acked_state.width, output->acked_state.height);
	} else {
		if (buffer->spec.mode == BACKGROUND_MODE_SOLID_COLOR) {
			wl_surface_set_buffer_scale(output->surface, 1);
		} else {
			wl_surface_set_buffer_scale(output->surface, output->acked_state.scale);
		}
	}
	if (output->color_surface && desc) {
		assert(desc->state == IMAGE_DESC_READY);
		wp_color_management_surface_v1_set_image_description(output->color_surface, desc->description, WP_COLOR_MANAGER_V1_RENDER_INTENT_RELATIVE);
	}
	wl_surface_commit(output->surface);
}

static void destroy_swaybg_image(struct swaybg_image *image) {
	if (!image) {
		return;
	}
	wl_list_remove(&image->link);
	free(image);
}

static void unref_swaybg_image_desc(struct swaybg_image_desc *desc) {
	assert(desc->reference_count > 0);
	desc->reference_count--;
	if (desc->reference_count == 0) {
		wl_list_remove(&desc->link);
		wp_image_description_v1_destroy(desc->description);
		free(desc);
	}
}

static void unref_swaybg_rendered_buffer(struct swaybg_rendered_buffer *buf) {
	assert(buf->reference_count > 0);
	buf->reference_count--;
	if (buf->reference_count == 0) {
		wl_list_remove(&buf->link);
		wl_buffer_destroy(buf->buffer);
		free(buf);
	}
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
	if (output->viewport != NULL) {
		wp_viewport_destroy(output->viewport);
	}
	if (output->fract_scale != NULL) {
		wp_fractional_scale_v1_destroy(output->fract_scale);
	}
	if (output->color_surface != NULL) {
		wp_color_management_surface_v1_destroy(output->color_surface);
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
	output->next_state.width = width;
	output->next_state.height = height;
	output->next_state.configure_serial = serial;
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

static void fract_preferred_scale(void *data, struct wp_fractional_scale_v1 *f,
		uint32_t scale) {
	struct swaybg_output *output = data;
	output->next_state.pref_fract_scale = scale;
}

static const struct wp_fractional_scale_v1_listener fract_scale_listener = {
	.preferred_scale = fract_preferred_scale
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

	if (output->state->fract_scale_manager) {
		output->fract_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
			output->state->fract_scale_manager, output->surface);
		assert(output->fract_scale);
		wp_fractional_scale_v1_add_listener(output->fract_scale,
			&fract_scale_listener, output);
	}

	if (output->state->viewporter &&
			(output->config->mode == BACKGROUND_MODE_SOLID_COLOR ||
				output->state->fract_scale_manager)) {
		output->viewport =  wp_viewporter_get_viewport(
			output->state->viewporter, output->surface);
	}

	if (output->state->color_manager && output->state->has_parametric
		&& output->state->supported_intents[WP_COLOR_MANAGER_V1_RENDER_INTENT_RELATIVE]
		&& output->state->supported_named_primaries[WP_COLOR_MANAGER_V1_PRIMARIES_SRGB]
		&& output->state->supported_named_tfs[WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB]
		) {
		output->color_surface = wp_color_manager_v1_get_surface(
			output->state->color_manager, output->surface);
	}

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
	output->next_state.scale = scale;
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


static void shm_format(void *data, struct wl_shm *wl_shm, uint32_t format) {
	struct swaybg_state *state = data;
	if (format == WL_SHM_FORMAT_XBGR2101010) {
		state->has_xbgr2101010 = true;
	}
	if (format == WL_SHM_FORMAT_XRGB2101010) {
		state->has_xrgb2101010 = true;
	}
}

static const struct wl_shm_listener shm_listener = {
	.format = shm_format,
};

static void color_supported_intent(void *data,
		struct wp_color_manager_v1 *wp_color_manager_v1, uint32_t render_intent) {
	struct swaybg_state *state = data;
	if (render_intent < sizeof(state->supported_intents) / sizeof(state->supported_intents[0])) {
		state->supported_intents[render_intent] = true;
	}
}

static void color_supported_feature(void *data,
		struct wp_color_manager_v1 *wp_color_manager_v1, uint32_t feature) {
	struct swaybg_state *state = data;
	if (feature == WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC) {
		state->has_parametric = true;
	}
}

static void color_supported_tf_named(void *data,
		struct wp_color_manager_v1 *wp_color_manager_v1, uint32_t tf) {
	struct swaybg_state *state = data;
	if (tf < sizeof(state->supported_named_tfs) / sizeof(state->supported_named_tfs[0])) {
		state->supported_named_tfs[tf] = true;
	}
}

static void color_supported_primaries_named(void *data,
		struct wp_color_manager_v1 *wp_color_manager_v1, uint32_t primaries) {
	struct swaybg_state *state = data;
	if (primaries < sizeof(state->supported_named_primaries) / sizeof(state->supported_named_primaries[0])) {
		state->supported_named_primaries[primaries] = true;
	}
}

static void color_done(void *data,
			 struct wp_color_manager_v1 *wp_color_manager_v1) {
	struct swaybg_state *state = data;
	state->color_info_done = true;
}

static const struct wp_color_manager_v1_listener color_manager_listener = {
	.supported_intent = color_supported_intent,
	.supported_feature = color_supported_feature,
	.supported_tf_named = color_supported_tf_named,
	.supported_primaries_named = color_supported_primaries_named,
	.done = color_done,
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
		output->next_state.scale = 1;
		output->acked_state.scale = 1;
		output->wl_name = name;
		output->wl_output =
			wl_registry_bind(registry, name, &wl_output_interface, 4);
		wl_output_add_listener(output->wl_output, &output_listener, output);
		wl_list_insert(&state->outputs, &output->link);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		state->layer_shell =
			wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
		state->viewporter = wl_registry_bind(registry, name,
			&wp_viewporter_interface, 1);
	} else if (strcmp(interface,
			wp_single_pixel_buffer_manager_v1_interface.name) == 0) {
		state->single_pixel_buffer_manager = wl_registry_bind(registry, name,
			&wp_single_pixel_buffer_manager_v1_interface, 1);
	} else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
		state->fract_scale_manager = wl_registry_bind(registry, name,
			&wp_fractional_scale_manager_v1_interface, 1);
	} else if (strcmp(interface, wp_color_manager_v1_interface.name) == 0) {
		state->color_manager = wl_registry_bind(registry, name,
			&wp_color_manager_v1_interface, 1);
		wp_color_manager_v1_add_listener(state->color_manager,
			&color_manager_listener, state);
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

static void convert_cicp_to_wl(const struct swaybg_state *state, const struct cicp *cicp,
		enum wp_color_manager_v1_transfer_function *tf, enum wp_color_manager_v1_primaries *primaries) {
	bool unsupported = false;
	uint32_t ctf = cicp_to_wl_tf(cicp->transfer);
	uint32_t cp = cicp_to_wl_primaries(cicp->primaries);

	if (!state->color_manager && ctf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB && cp == WP_COLOR_MANAGER_V1_PRIMARIES_SRGB) {
		/* No color manager, srgb is supported by default */
		*tf = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB;
		*primaries = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB;
		return;
	}

	if (ctf == 0) {
		swaybg_log(LOG_ERROR, "Received image with CICP transfer = %d, has no Wayland equivalent", cicp->transfer);
		unsupported = true;
	} else if (ctf >= sizeof(state->supported_named_tfs) / sizeof(state->supported_named_tfs[0]) ||
			   !state->supported_named_tfs[ctf]) {
		swaybg_log(LOG_ERROR, "Received image with CICP transfer = %d, not supported by compositor", cicp->transfer);
		unsupported = true;
	}

	if (cp == 0) {
		swaybg_log(LOG_ERROR, "Received image with CICP primaries = %d, has no Wayland equivalent", cicp->primaries);
		unsupported = true;
	} else if (cp >= sizeof(state->supported_named_primaries) / sizeof(state->supported_named_primaries[0]) ||
			   !state->supported_named_primaries[cp]) {
		swaybg_log(LOG_ERROR, "Received image with CICP primaries = %d, not supported by compositor", cicp->primaries);
		unsupported = true;
	}

	if (cicp->range != 1) {
		swaybg_log(LOG_ERROR, "Received image with CICP range = %d; only full range (1) supported", cicp->range);
		unsupported = true;
	}
	if (cicp->matrix != 0) {
		swaybg_log(LOG_ERROR, "Received image with CICP matrix = %d; only RGB (0) supported", cicp->matrix);
		unsupported = true;
	}

	if (unsupported) {
		*tf = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB;
		*primaries = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB;
	} else {
		*tf = ctf;
		*primaries = cp;
	}
}



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
	// TODO: need to add an argument specifying rendering intent for each config.
	// Why? Currently swaybg has a nice design where the background modes can take
	// any image and draw it in a reasonable fashion on an _unexpected_ display
	// of arbitrary size/aspect ratio. This sort of thing can _not_ reasonably
	// be done without swaybg integration -- would need a complex script to
	// scale/crop images on hotplug, update the config, etc.

	// To properly handle image color adjustment for arbitrary displays, need
	// a rendering intent option, because the user does not know in advance all
	// attached display properties and which rendering intent is appropriate
	// may depend on the image content + other factors.

	const char *usage =
		"Usage: swaybg <options...>\n"
		"\n"
		"  -c, --color RRGGBB     Set the background color.\n"
		"  -h, --help             Show help message and quit.\n"
		"  -i, --image <path>     Set the image to display.\n"
		"  -m, --mode <mode>      Set the mode to use for the image.\n"
		"  -o, --output <name>    Set the output to operate on or * for all.\n"
		"  -v, --version          Show the version number and quit.\n"
		"\n"
		"Background Modes:\n"
		"  stretch, fit, fill, center, tile, or solid_color\n";

	struct swaybg_output_config *config = calloc(1, sizeof(struct swaybg_output_config));
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
			if (!parse_color(optarg, &config->color)) {
				swaybg_log(LOG_ERROR, "%s is not a valid color for swaybg. "
					"Color should be specified as rrggbb or #rrggbb (no alpha).", optarg);
				continue;
			}
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
			config = calloc(1, sizeof(struct swaybg_output_config));
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
	wl_list_init(&state.configs);
	wl_list_init(&state.outputs);
	wl_list_init(&state.images);
	wl_list_init(&state.image_descs);
	wl_list_init(&state.rendered_buffers);

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

	/* Before processing any images, wait to see what color management
	 * supports, as this can affect what error messages are made for
	 * images whose color parameters are not supported by the compositor.
	 *
	 * This wait _might_ be safe to remove in the future, if swaybg does the
	 * conversion of image data to f16 buffers itself and does not need
	 * any complicated image descriptions. */
	while (state.color_manager && !state.color_info_done && state.run_display) {
		if (wl_display_dispatch(state.display) == -1) {
			state.run_display = false;
			break;
		}
	}

	while (state.run_display) {
		struct swaybg_output *output;
		bool work_to_do_now = false;
		wl_list_for_each(output, &state.outputs, link) {
			if (output->queued_buffer || output->queued_image_desc) {
				/* This output is waiting for an image description to
				 * be ready and should wait to render a new frame until the
				 * description results are in. */
				continue;
			}
			if (output->next_state.configure_serial != output->acked_state.configure_serial) {
				work_to_do_now = true;
			}
		}

		if (work_to_do_now) {
			if (wl_display_prepare_read(state.display) != -1) {
				if (wl_display_read_events(state.display) == -1 && errno != EAGAIN) {
					break;
				}
			} else if (errno != EAGAIN) {
				break;
			}
			if (wl_display_dispatch_pending(state.display) == -1) {
				break;
			}
		} else {
			if (wl_display_dispatch(state.display) == -1) {
				break;
			}
		}

		/* Identify which outputs have changed, what images need to be reloaded,
		 * and acknowledge configure events. */
		wl_list_for_each(output, &state.outputs, link) {
			if (!output->config) {
				continue;
			}

			if (output->queued_buffer || output->queued_image_desc) {
				/* Have already queued a state update; do _NOT_ queue another
				 * one until the previous image descriptions are ready to avoid
				 * flooding the compositor. */
				continue;
			}

			if (output->next_state.configure_serial != output->acked_state.configure_serial) {
				zwlr_layer_surface_v1_ack_configure(
					output->layer_surface,
					output->next_state.configure_serial);
			}


			uint32_t acked_width, acked_height;
			get_buffer_size(&output->acked_state, output->config->mode,
				output->viewport != NULL, &acked_width, &acked_height);
			uint32_t next_width, next_height;
			get_buffer_size(&output->next_state, output->config->mode,
				output->viewport != NULL, &next_width, &next_height);

			bool needs_new_buffer = (acked_width != next_width) || (acked_height != next_height);
			bool needs_commit = needs_new_buffer;
			if (state.viewporter && (output->next_state.width != output->acked_state.width
				|| output->next_state.height != output->acked_state.height)) {
				needs_commit = true;
			}

			output->acked_state = output->next_state;
			output->needs_commit = needs_commit;
			output->needs_new_buffer = needs_commit;
			if (needs_new_buffer && output->config->image) {
				output->config->image->load_required = true;
			}
		}

		/* Load images, render associated frames, and unload, to avoid keeping
		 * image data in memory any longer than necessary */
		wl_list_for_each(image, &state.images, link) {
			if (!image->load_required) {
				continue;
			}

			/* Note: the results of load_background_image _may_ be different
			 * every time -- the background image might be a symlink and get
			 * changed, so when loading the image description may change from
			 * before, and should be kept in sync with the image content.
			 *
			 * This is done to avoid keeping the (possibly huge, since users may
			 * use an 8k image 'just in case' they connect to an 8k display) image
			 * data in memory.
			 *
			 * Since swaybg already does ~99% of the work for 'on demand reloading',
			 * it _may_ be worth it to explicitly support this.
			 */
			struct cicp info;
			cairo_surface_t *surface = load_background_image(image->path, &info);
			if (!surface) {
				swaybg_log(LOG_ERROR, "Failed to load image: %s", image->path);
				continue;
			}
			image->load_sequence_number++;

			enum wp_color_manager_v1_transfer_function tf;
			enum wp_color_manager_v1_primaries primaries;
			convert_cicp_to_wl(&state, &info, &tf, &primaries);

			struct swaybg_image_desc *desc = get_or_construct_image_desc(&state, tf, primaries);

			wl_list_for_each(output, &state.outputs, link) {
				if (output->needs_new_buffer && output->config->image == image) {
					output->needs_new_buffer = false;

					struct swaybg_buffer_spec spec = (struct swaybg_buffer_spec) {
						.color = output->config->color,
						.mode = output->config->mode,
						.image = output->config->image,
						.load_sequence_number = output->config->image->load_sequence_number,
					};
					get_buffer_size(&output->acked_state, spec.mode,
						state.viewporter != NULL, &spec.width, &spec.height);

					struct swaybg_rendered_buffer *buffer =
						get_or_construct_rendered_buffer(&state, &spec, surface);
					output->queued_buffer = buffer;
					output->queued_image_desc = desc;
				}
			}

			image->load_required = false;
			cairo_surface_destroy(surface);
		}

		wl_list_for_each(output, &state.outputs, link) {
			if (output->needs_new_buffer && !output->config->image) {
				output->needs_new_buffer = false;

				struct swaybg_buffer_spec spec = (struct swaybg_buffer_spec) {
					.color = output->config->color,
					.mode = output->config->mode,
					.image = NULL,
					.load_sequence_number = 0,
				};
				get_buffer_size(&output->acked_state, spec.mode,
					state.viewporter != NULL, &spec.width, &spec.height);

				struct swaybg_rendered_buffer *buffer =
					get_or_construct_rendered_buffer(&state, &spec, NULL);
				output->queued_buffer = buffer;
			}
		}

		/* Commit all outputs for which the buffer and image description are ready */
		wl_list_for_each(output, &state.outputs, link) {
			if (!output->needs_commit) {
				continue;
			}
			if (output->queued_image_desc) {
				if (output->queued_image_desc->state == IMAGE_DESC_WAITING) {
					continue;
				}
				if (output->queued_image_desc->state == IMAGE_DESC_FAILED) {
					/* Have already printed a warning, so just commit without changing the description */
					output->queued_image_desc = NULL;
				}
			}
			commit_frame(output, output->queued_buffer, output->queued_image_desc);
			output->needs_commit = false;

			if (output->queued_buffer) {
				unref_swaybg_rendered_buffer(output->queued_buffer);
				output->queued_buffer = NULL;
			}
			if (output->queued_image_desc) {
				unref_swaybg_image_desc(output->queued_image_desc);
				output->queued_image_desc = NULL;
			}
			// todo: consider moving 'queued_buffer'/'queued_image_desc' to
			// output->current_buffer/output->current_image_desc fields, to keep
			// the objects alive and let them be reused the next time this output
			// or a similar one is redrawn, reducing latency. Doing this _should_
			// be effectively free because the compositor needs to keep its copy
			// of the buffer or image description alive to draw existing surfaces.
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

	/* There should be no references remaining from outputs */
	assert(wl_list_empty(&state.image_descs));
	assert(wl_list_empty(&state.rendered_buffers));

	if (state.color_manager) {
		wp_color_manager_v1_destroy(state.color_manager);
	}

	return 0;
}
