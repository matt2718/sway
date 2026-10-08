#ifndef _SWAY_ICON_THEME_H
#define _SWAY_ICON_THEME_H
#include <stdbool.h>

enum icon_file_format {
	ICON_FORMAT_PNG,
	ICON_FORMAT_SVG,
};

struct icon_file {
	char *path;
	enum icon_file_format format;
	int size; // the size the image was made for in pixels, or 0 if unknown
};

/**
 * Finds the image file that best represents the icon `name` at `size`×`size`
 * pixels, following the XDG icon theme specification: the icon is looked up
 * in `theme` (which may be NULL), the themes it inherits from, the hicolor
 * theme and finally in the fallback directories (e.g. /usr/share/pixmaps).
 *
 * Returns false if there is no such icon. On success, release the file with
 * icon_file_finish().
 */
bool icon_theme_find(const char *theme, const char *name, int size,
		struct icon_file *file);

void icon_file_finish(struct icon_file *file);

/**
 * Forgets all icon themes loaded so far, so that changes on disk are picked
 * up by the next lookups.
 */
void icon_themes_reset(void);

#endif
