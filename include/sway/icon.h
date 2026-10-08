#ifndef _SWAY_ICON_H
#define _SWAY_ICON_H
#include <cairo.h>
#include <stdbool.h>
#include <stddef.h>
#include "list.h"

struct wlr_buffer;

/**
 * A reference-counted icon, which can be rendered at any size: either an icon
 * from the icon theme, an image file, or pixel data provided by a client.
 */
struct sway_icon;

/**
 * Looks up an icon by name in the configured icon theme. Returns NULL if
 * there's no such icon.
 */
struct sway_icon *sway_icon_from_name(const char *name);

/**
 * Creates an icon from images of different sizes, typically provided by a
 * client. Takes ownership of the list and of the cairo image surfaces in it.
 */
struct sway_icon *sway_icon_from_images(list_t *images);

/**
 * Icons are only shown in title bars, so we keep only a few images of the ones
 * a client provides: given their sizes, sets keep[i] for the images to copy
 * with sway_icon_copy_image().
 */
void sway_icon_choose_images(const int *sizes, size_t n, bool *keep);

/**
 * Copies an image provided by a client, scaling it down if it's larger than
 * needed. Returns NULL on failure.
 */
cairo_surface_t *sway_icon_copy_image(cairo_surface_t *image);

/**
 * Finds the icon of an application, given its app_id, sandbox app ID or X11
 * class: the icon of its desktop entry if there is one, or else the icon named
 * after it. Returns NULL if no icon was found.
 */
struct sway_icon *sway_icon_for_app(const char *app_id);

struct sway_icon *sway_icon_ref(struct sway_icon *icon);

void sway_icon_unref(struct sway_icon *icon);

/**
 * Returns the icon rendered at size×size pixels, or NULL on failure. Rendered
 * buffers are cached: the buffer belongs to the icon, lock it to keep it.
 */
struct wlr_buffer *sway_icon_get_buffer(struct sway_icon *icon, int size);

/**
 * Forgets all cached icons and lookup results, and the icon themes and desktop
 * entries read so far.
 */
void sway_icons_reset(void);

#endif
