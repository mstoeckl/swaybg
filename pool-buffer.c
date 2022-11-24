#define _POSIX_C_SOURCE 200809
#include <assert.h>
#include <cairo.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "pool-buffer.h"

static bool set_cloexec(int fd) {
	long flags = fcntl(fd, F_GETFD);
	if (flags == -1) {
		return false;
	}

	if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) {
		return false;
	}

	return true;
}

static int create_pool_file(size_t size, char **name) {
	static const char template[] = "sway-client-XXXXXX";
	const char *path = getenv("XDG_RUNTIME_DIR");
	if (path == NULL) {
		fprintf(stderr, "XDG_RUNTIME_DIR is not set\n");
		return -1;
	}

	size_t name_size = strlen(template) + 1 + strlen(path) + 1;
	*name = malloc(name_size);
	if (*name == NULL) {
		fprintf(stderr, "allocation failed\n");
		return -1;
	}
	snprintf(*name, name_size, "%s/%s", path, template);

	int fd = mkstemp(*name);
	if (fd < 0) {
		return -1;
	}

	if (!set_cloexec(fd)) {
		close(fd);
		return -1;
	}

	if (ftruncate(fd, size) < 0) {
		close(fd);
		return -1;
	}

	return fd;
}

bool create_buffer(struct pool_buffer *buf, struct wl_shm *shm,
		int32_t width, int32_t height, uint32_t format) {
	uint32_t stride = width * 4;
	size_t size = stride * height;

	char *name;
	int fd = create_pool_file(size, &name);
	assert(fd != -1);
	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	buf->buffer = wl_shm_pool_create_buffer(pool, 0,
			width, height, stride, format);
	wl_shm_pool_destroy(pool);
	close(fd);
	unlink(name);
	free(name);
	fd = -1;

	buf->size = size;
	buf->data = data;
	return true;
}

void destroy_buffer(struct pool_buffer *buffer) {
	if (buffer->buffer) {
		wl_buffer_destroy(buffer->buffer);
	}
	if (buffer->data) {
		munmap(buffer->data, buffer->size);
	}
}
