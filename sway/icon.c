#include <cairo.h>
#include <ctype.h>
#include <drm_fourcc.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include "config.h"
#if HAVE_LIBRSVG
#include <librsvg/rsvg.h>
#endif
#include "list.h"
#include "log.h"
#include "sway/config.h"
#include "sway/desktop_entry.h"
#include "sway/icon.h"
#include "sway/icon_theme.h"

// Images larger than this aren't loaded
#define MAX_IMAGE_SIZE 2048
// Image files larger than this aren't read
#define MAX_IMAGE_FILE_SIZE (16 * 1024 * 1024)
// The number of sizes an icon keeps rendered buffers for
#define MAX_ICON_BUFFERS 4
// Images provided by a client that we keep: how many, how large (larger ones
// are scaled down), and the size of images that we ignore
#define MAX_CLIENT_IMAGES 8
#define MAX_CLIENT_IMAGE_SIZE 128
#define MAX_CLIENT_SOURCE_SIZE 1024
// Lookup results are forgotten once there are more of them than this
#define MAX_CACHE_ENTRIES 256

enum sway_icon_type {
	ICON_NAMED, // from the icon theme
	ICON_FILE, // from an image file
	ICON_IMAGES, // from pixel data
};

struct icon_buffer {
	struct wlr_buffer base;
	cairo_surface_t *surface;
};

struct sway_icon {
	int refs;
	enum sway_icon_type type;
	char *name; // the icon name for ICON_NAMED, the path for ICON_FILE
	list_t *images; // cairo_surface_t *, for ICON_IMAGES

	// Buffers rendered at different sizes, most recently used first
	struct icon_buffer *buffers[MAX_ICON_BUFFERS];
};

struct cache_entry {
	char *key;
	struct sway_icon *icon; // NULL if there's no icon
};

static list_t *named_icons = NULL; // struct cache_entry *
static list_t *app_icons = NULL; // struct cache_entry *

static void icon_buffer_handle_destroy(struct wlr_buffer *wlr_buffer) {
	struct icon_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	cairo_surface_destroy(buffer->surface);
	free(buffer);
}

static bool icon_buffer_handle_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
		uint32_t flags, void **data, uint32_t *format, size_t *stride) {
	struct icon_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) {
		return false;
	}
	*data = cairo_image_surface_get_data(buffer->surface);
	*stride = cairo_image_surface_get_stride(buffer->surface);
	*format = DRM_FORMAT_ARGB8888;
	return true;
}

static void icon_buffer_handle_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
	// This space is intentionally left blank
}

static const struct wlr_buffer_impl icon_buffer_impl = {
	.destroy = icon_buffer_handle_destroy,
	.begin_data_ptr_access = icon_buffer_handle_begin_data_ptr_access,
	.end_data_ptr_access = icon_buffer_handle_end_data_ptr_access,
};

static cairo_surface_t *create_image_surface(int width, int height) {
	cairo_surface_t *surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		sway_log(SWAY_ERROR, "Failed to create a cairo surface: %s",
			cairo_status_to_string(cairo_surface_status(surface)));
		cairo_surface_destroy(surface);
		return NULL;
	}
	return surface;
}

/**
 * Paints an image on a size×size surface, scaled to fit and centered.
 */
static cairo_surface_t *render_image(cairo_surface_t *image, int size) {
	int width = cairo_image_surface_get_width(image);
	int height = cairo_image_surface_get_height(image);
	if (width <= 0 || height <= 0) {
		return NULL;
	}
	cairo_surface_t *surface = create_image_surface(size, size);
	if (!surface) {
		return NULL;
	}

	double scale = fmin((double)size / width, (double)size / height);
	if (scale > 1 && scale <= 4.0 / 3) {
		// Upscaling an image only slightly makes it blurry for little gain:
		// show it at its original size instead
		scale = 1;
	}

	cairo_t *cairo = cairo_create(surface);
	cairo_translate(cairo, floor((size - width * scale) / 2),
		floor((size - height * scale) / 2));
	cairo_scale(cairo, scale, scale);
	cairo_set_source_surface(cairo, image, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cairo), CAIRO_FILTER_GOOD);
	cairo_paint(cairo);
	cairo_destroy(cairo);
	cairo_surface_flush(surface);
	return surface;
}

static bool check_png_size(const char *path, const unsigned char *data,
		size_t len) {
	if (len < 24 || memcmp(data, "\x89PNG\r\n\x1a\n", 8) != 0 ||
			memcmp(data + 12, "IHDR", 4) != 0) {
		sway_log(SWAY_ERROR, "Failed to load icon %s: not a PNG file", path);
		return false;
	}

	uint32_t width = (uint32_t)data[16] << 24 | (uint32_t)data[17] << 16 |
		(uint32_t)data[18] << 8 | data[19];
	uint32_t height = (uint32_t)data[20] << 24 | (uint32_t)data[21] << 16 |
		(uint32_t)data[22] << 8 | data[23];
	if (width > MAX_IMAGE_SIZE || height > MAX_IMAGE_SIZE) {
		sway_log(SWAY_ERROR, "Not loading icon %s: image too large (%"PRIu32
			"x%"PRIu32")", path, width, height);
		return false;
	}
	return true;
}

struct png_reader {
	const unsigned char *data;
	size_t len, pos;
};

static cairo_status_t read_png_data(void *closure, unsigned char *data,
		unsigned int length) {
	struct png_reader *reader = closure;
	if (length > reader->len - reader->pos) {
		return CAIRO_STATUS_READ_ERROR;
	}
	memcpy(data, reader->data + reader->pos, length);
	reader->pos += length;
	return CAIRO_STATUS_SUCCESS;
}

static cairo_surface_t *render_png(const char *path, int size) {
	// Read the file ourselves, as a regular file: a FIFO would block
	size_t len = 0;
	unsigned char *data = (unsigned char *)read_regular_file(path,
		MAX_IMAGE_FILE_SIZE, &len, NULL);
	if (!data) {
		sway_log(SWAY_ERROR, "Failed to load icon %s: unable to read the file "
			"or file too large", path);
		return NULL;
	}
	if (!check_png_size(path, data, len)) {
		free(data);
		return NULL;
	}

	struct png_reader reader = { .data = data, .len = len };
	cairo_surface_t *image =
		cairo_image_surface_create_from_png_stream(read_png_data, &reader);
	free(data);
	if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
		sway_log(SWAY_ERROR, "Failed to load icon %s: %s", path,
			cairo_status_to_string(cairo_surface_status(image)));
		cairo_surface_destroy(image);
		return NULL;
	}
	cairo_surface_t *surface = render_image(image, size);
	cairo_surface_destroy(image);
	return surface;
}

#if HAVE_LIBRSVG
static cairo_surface_t *render_svg(const char *path, int size) {
	// Don't let librsvg open the file: going through GIO would load GVfs
	// modules into the compositor. This also prevents loading external files.
	size_t len = 0;
	char *data = read_regular_file(path, MAX_IMAGE_FILE_SIZE, &len, NULL);
	if (!data) {
		sway_log(SWAY_ERROR, "Failed to load icon %s: unable to read the file "
			"or file too large", path);
		return NULL;
	}

	GError *error = NULL;
	RsvgHandle *handle =
		rsvg_handle_new_from_data((const guint8 *)data, len, &error);
	free(data);
	if (!handle) {
		sway_log(SWAY_ERROR, "Failed to load icon %s: %s", path,
			error ? error->message : "unknown error");
		g_clear_error(&error);
		return NULL;
	}

	cairo_surface_t *surface = create_image_surface(size, size);
	if (surface) {
		cairo_t *cairo = cairo_create(surface);
		RsvgRectangle viewport = { .x = 0, .y = 0, .width = size, .height = size };
		if (!rsvg_handle_render_document(handle, cairo, &viewport, &error)) {
			sway_log(SWAY_ERROR, "Failed to render icon %s: %s", path,
				error ? error->message : "unknown error");
			g_clear_error(&error);
			cairo_surface_destroy(surface);
			surface = NULL;
		}
		cairo_destroy(cairo);
	}
	g_object_unref(handle);

	if (surface) {
		cairo_surface_flush(surface);
	}
	return surface;
}
#endif

static cairo_surface_t *render_file(const char *path,
		enum icon_file_format format, int size) {
	switch (format) {
	case ICON_FORMAT_PNG:
		return render_png(path, size);
	case ICON_FORMAT_SVG:
#if HAVE_LIBRSVG
		return render_svg(path, size);
#else
		return NULL;
#endif
	}
	return NULL;
}

static bool has_suffix(const char *str, const char *suffix) {
	size_t len = strlen(str), suffix_len = strlen(suffix);
	return len >= suffix_len &&
		strcasecmp(str + len - suffix_len, suffix) == 0;
}

static cairo_surface_t *render_images(list_t *images, int size) {
	// Use the smallest image at least as large as requested, or else the
	// largest image
	cairo_surface_t *best = NULL;
	int best_size = 0;
	for (int i = 0; i < images->length; ++i) {
		cairo_surface_t *image = images->items[i];
		int image_size = MAX(cairo_image_surface_get_width(image),
			cairo_image_surface_get_height(image));
		if (!best || (best_size < size ? image_size > best_size :
				image_size >= size && image_size < best_size)) {
			best = image;
			best_size = image_size;
		}
	}
	return best ? render_image(best, size) : NULL;
}

static cairo_surface_t *icon_render(struct sway_icon *icon, int size) {
	switch (icon->type) {
	case ICON_NAMED:;
		struct icon_file file;
		if (!icon_theme_find(config->title_window_icon_theme, icon->name, size,
				&file)) {
			return NULL;
		}
		cairo_surface_t *surface = render_file(file.path, file.format, size);
		icon_file_finish(&file);
		return surface;
	case ICON_FILE:
		return render_file(icon->name, has_suffix(icon->name, ".svg") ?
			ICON_FORMAT_SVG : ICON_FORMAT_PNG, size);
	case ICON_IMAGES:
		return render_images(icon->images, size);
	}
	return NULL;
}

struct wlr_buffer *sway_icon_get_buffer(struct sway_icon *icon, int size) {
	if (size <= 0 || size > MAX_IMAGE_SIZE) {
		return NULL;
	}

	for (int i = 0; i < MAX_ICON_BUFFERS; ++i) {
		struct icon_buffer *buffer = icon->buffers[i];
		if (buffer && buffer->base.width == size) {
			memmove(&icon->buffers[1], &icon->buffers[0],
				i * sizeof(icon->buffers[0]));
			icon->buffers[0] = buffer;
			return &buffer->base;
		}
	}

	cairo_surface_t *surface = icon_render(icon, size);
	if (!surface) {
		return NULL;
	}
	struct icon_buffer *buffer = calloc(1, sizeof(*buffer));
	if (!buffer) {
		cairo_surface_destroy(surface);
		return NULL;
	}
	wlr_buffer_init(&buffer->base, &icon_buffer_impl, size, size);
	buffer->surface = surface;

	struct icon_buffer *oldest = icon->buffers[MAX_ICON_BUFFERS - 1];
	if (oldest) {
		wlr_buffer_drop(&oldest->base);
	}
	memmove(&icon->buffers[1], &icon->buffers[0],
		(MAX_ICON_BUFFERS - 1) * sizeof(icon->buffers[0]));
	icon->buffers[0] = buffer;
	return &buffer->base;
}

static struct sway_icon *icon_create(enum sway_icon_type type,
		const char *name, list_t *images) {
	struct sway_icon *icon = calloc(1, sizeof(*icon));
	if (!icon) {
		return NULL;
	}
	icon->refs = 1;
	icon->type = type;
	icon->name = name ? strdup(name) : NULL;
	icon->images = images;
	return icon;
}

struct sway_icon *sway_icon_ref(struct sway_icon *icon) {
	++icon->refs;
	return icon;
}

void sway_icon_unref(struct sway_icon *icon) {
	if (!icon || --icon->refs > 0) {
		return;
	}
	for (int i = 0; i < MAX_ICON_BUFFERS; ++i) {
		if (icon->buffers[i]) {
			wlr_buffer_drop(&icon->buffers[i]->base);
		}
	}
	if (icon->images) {
		for (int i = 0; i < icon->images->length; ++i) {
			cairo_surface_destroy(icon->images->items[i]);
		}
		list_free(icon->images);
	}
	free(icon->name);
	free(icon);
}

void sway_icon_choose_images(const int *sizes, size_t n, bool *keep) {
	// Keep the largest images (once scaled down to MAX_CLIENT_IMAGE_SIZE), as
	// scaling down gives better results than scaling up, and only one image of
	// each size, preferring the smallest source image
	int kept_sizes[MAX_CLIENT_IMAGES];
	int n_kept = 0;
	for (size_t i = 0; i < n; ++i) {
		keep[i] = false;
	}
	while (n_kept < MAX_CLIENT_IMAGES) {
		size_t best = n;
		int best_size = 0;
		for (size_t i = 0; i < n; ++i) {
			if (keep[i] || sizes[i] <= 0 || sizes[i] > MAX_CLIENT_SOURCE_SIZE) {
				continue;
			}
			int size = MIN(sizes[i], MAX_CLIENT_IMAGE_SIZE);
			bool taken = false;
			for (int j = 0; j < n_kept; ++j) {
				taken |= kept_sizes[j] == size;
			}
			if (!taken && (best == n || size > best_size ||
					(size == best_size && sizes[i] < sizes[best]))) {
				best = i;
				best_size = size;
			}
		}
		if (best == n) {
			break;
		}
		keep[best] = true;
		kept_sizes[n_kept++] = best_size;
	}
}

cairo_surface_t *sway_icon_copy_image(cairo_surface_t *image) {
	int width = cairo_image_surface_get_width(image);
	int height = cairo_image_surface_get_height(image);
	if (width <= 0 || height <= 0) {
		return NULL;
	}
	if (MAX(width, height) > MAX_CLIENT_IMAGE_SIZE) {
		return render_image(image, MAX_CLIENT_IMAGE_SIZE);
	}

	cairo_surface_t *copy = create_image_surface(width, height);
	if (!copy) {
		return NULL;
	}
	cairo_t *cairo = cairo_create(copy);
	cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_surface(cairo, image, 0, 0);
	cairo_paint(cairo);
	cairo_destroy(cairo);
	cairo_surface_flush(copy);
	return copy;
}

struct sway_icon *sway_icon_from_images(list_t *images) {
	struct sway_icon *icon = icon_create(ICON_IMAGES, NULL, images);
	if (!icon) {
		for (int i = 0; i < images->length; ++i) {
			cairo_surface_destroy(images->items[i]);
		}
		list_free(images);
	}
	return icon;
}

static struct cache_entry *cache_find(list_t *cache, const char *key) {
	if (!cache) {
		return NULL;
	}
	for (int i = 0; i < cache->length; ++i) {
		struct cache_entry *entry = cache->items[i];
		if (strcmp(entry->key, key) == 0) {
			return entry;
		}
	}
	return NULL;
}

static void cache_clear(list_t *cache) {
	if (!cache) {
		return;
	}
	for (int i = 0; i < cache->length; ++i) {
		struct cache_entry *entry = cache->items[i];
		sway_icon_unref(entry->icon);
		free(entry->key);
		free(entry);
	}
	cache->length = 0;
}

static void cache_add(list_t **cache, const char *key, struct sway_icon *icon) {
	if (!*cache) {
		*cache = create_list();
	}
	if ((*cache)->length >= MAX_CACHE_ENTRIES) {
		cache_clear(*cache);
	}
	struct cache_entry *entry = calloc(1, sizeof(*entry));
	if (!entry) {
		return;
	}
	entry->key = strdup(key);
	entry->icon = icon ? sway_icon_ref(icon) : NULL;
	list_add(*cache, entry);
}

/**
 * Checks that the icon can be rendered, so that we can fall back to another
 * one otherwise.
 */
static struct sway_icon *icon_check(struct sway_icon *icon) {
	int size = config->font_height > 0 ? config->font_height : 16;
	if (icon && !sway_icon_get_buffer(icon, size)) {
		sway_icon_unref(icon);
		return NULL;
	}
	return icon;
}

static struct sway_icon *icon_from_path(const char *path) {
	struct cache_entry *entry = cache_find(named_icons, path);
	if (entry) {
		return entry->icon ? sway_icon_ref(entry->icon) : NULL;
	}
	struct sway_icon *icon = icon_check(icon_create(ICON_FILE, path, NULL));
	cache_add(&named_icons, path, icon);
	return icon;
}

struct sway_icon *sway_icon_from_name(const char *name) {
	if (!name || !*name || strchr(name, '/')) {
		return NULL;
	}
	struct cache_entry *entry = cache_find(named_icons, name);
	if (entry) {
		return entry->icon ? sway_icon_ref(entry->icon) : NULL;
	}

	struct sway_icon *icon = icon_check(icon_create(ICON_NAMED, name, NULL));
	if (!icon && (has_suffix(name, ".png") || has_suffix(name, ".svg") ||
			has_suffix(name, ".xpm"))) {
		// Some desktop entries wrongly include the file extension
		char *stem = strndup(name, strlen(name) - 4);
		if (stem) {
			icon = sway_icon_from_name(stem);
			free(stem);
		}
	}
	cache_add(&named_icons, name, icon);
	return icon;
}

struct sway_icon *sway_icon_for_app(const char *app_id) {
	if (!app_id || !*app_id) {
		return NULL;
	}
	struct cache_entry *entry = cache_find(app_icons, app_id);
	if (entry) {
		return entry->icon ? sway_icon_ref(entry->icon) : NULL;
	}

	struct sway_icon *icon = NULL;
	char *icon_name = desktop_entry_find_icon(app_id);
	if (icon_name) {
		if (icon_name[0] == '/') {
			icon = icon_from_path(icon_name);
		} else {
			icon = sway_icon_from_name(icon_name);
		}
		free(icon_name);
	}

	// Many applications use their app ID as icon name
	if (!icon) {
		icon = sway_icon_from_name(app_id);
	}
	if (!icon) {
		char *lower = strdup(app_id);
		if (lower) {
			for (char *c = lower; *c; ++c) {
				*c = tolower((unsigned char)*c);
			}
			if (strcmp(lower, app_id) != 0) {
				icon = sway_icon_from_name(lower);
			}
			free(lower);
		}
	}

	sway_log(SWAY_DEBUG, "Application '%s' %s", app_id,
		icon ? "has an icon" : "has no icon");
	cache_add(&app_icons, app_id, icon);
	return icon;
}

void sway_icons_reset(void) {
	cache_clear(named_icons);
	cache_clear(app_icons);
	icon_themes_reset();
	desktop_entries_reset();
}
