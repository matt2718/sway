#ifndef _SWAY_DESKTOP_ENTRY_H
#define _SWAY_DESKTOP_ENTRY_H
#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>
#include "list.h"

/**
 * Opens a file for reading if it's a regular file of at most max_size bytes
 * (opening a FIFO would block), filling st if not NULL. Returns -1 otherwise.
 */
int open_regular_file(const char *path, size_t max_size, struct stat *st);

/**
 * Reads a whole regular file of at most max_size bytes, see open_regular_file.
 * Returns the newly allocated contents, or NULL on failure.
 */
char *read_regular_file(const char *path, size_t max_size, size_t *len,
		struct stat *st);

/**
 * Returns $XDG_DATA_HOME followed by the directories in $XDG_DATA_DIRS,
 * applying the defaults from the base directory specification. Free with
 * list_free_items_and_destroy().
 */
list_t *xdg_data_dirs(void);

/**
 * Called for every key in a desktop entry file. Return false to stop parsing.
 */
typedef bool (*desktop_file_handler_t)(void *data, const char *group,
		const char *key, const char *value);

/**
 * Parses a file in the XDG desktop entry format, which is also used by icon
 * theme index files. Returns false if the file couldn't be read.
 */
bool desktop_file_parse(const char *path, desktop_file_handler_t handler,
		void *data);

/**
 * Finds the icon (the value of its Icon key) of the desktop entry matching an
 * application identifier: a Wayland app_id, a sandbox app ID or an X11 class.
 * Returns a newly allocated string, or NULL if no matching desktop entry with
 * an icon was found.
 */
char *desktop_entry_find_icon(const char *app_id);

/**
 * Forgets all desktop entries read so far, so that they are read again on the
 * next lookup.
 */
void desktop_entries_reset(void);

#endif
