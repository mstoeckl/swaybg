#ifndef _SWAY_BUFFERS_H
#define _SWAY_BUFFERS_H
#include <cairo.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

struct pool_buffer {
	struct wl_buffer *buffer;
	void *data;
	size_t size;
	int stride;
};

bool create_buffer(struct pool_buffer *buffer, struct wl_shm *shm,
		int32_t width, int32_t height, uint32_t format);
void destroy_buffer(struct pool_buffer *buffer);

#endif
