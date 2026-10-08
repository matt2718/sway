#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "list.h"
#include "log.h"
#include "stringop.h"
#include "sway/desktop_entry.h"

struct desktop_entry {
	char *id; // desktop file ID, without the .desktop suffix
	char *icon; // may be NULL
	char *wm_class; // StartupWMClass, may be NULL
};

// All desktop entries, in order of precedence. NULL until first needed.
static list_t *desktop_entries = NULL;

static char *strip(char *str) {
	while (isspace((unsigned char)*str)) {
		++str;
	}
	char *end = str + strlen(str);
	while (end > str && isspace((unsigned char)end[-1])) {
		--end;
	}
	*end = '\0';
	return str;
}

int open_regular_file(const char *path, size_t max_size, struct stat *st) {
	int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		return -1;
	}
	struct stat tmp;
	if (!st) {
		st = &tmp;
	}
	if (fstat(fd, st) != 0 || !S_ISREG(st->st_mode) || st->st_size < 0 ||
			(uintmax_t)st->st_size > max_size) {
		close(fd);
		return -1;
	}
	return fd;
}

char *read_regular_file(const char *path, size_t max_size, size_t *len,
		struct stat *st) {
	struct stat tmp;
	if (!st) {
		st = &tmp;
	}
	int fd = open_regular_file(path, max_size, st);
	if (fd < 0) {
		return NULL;
	}
	size_t size = st->st_size;
	char *data = malloc(size + 1);
	size_t done = 0;
	while (data && done < size) {
		ssize_t n = read(fd, data + done, size - done);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			free(data);
			data = NULL;
			break;
		}
		done += n;
	}
	close(fd);
	if (data) {
		data[size] = '\0';
		*len = size;
	}
	return data;
}

bool desktop_file_parse(const char *path, desktop_file_handler_t handler,
		void *data) {
	int fd = open_regular_file(path, 4 * 1024 * 1024, NULL);
	FILE *file = fd >= 0 ? fdopen(fd, "r") : NULL;
	if (!file) {
		if (fd >= 0) {
			close(fd);
		}
		return false;
	}

	char *group = NULL;
	char *line = NULL;
	size_t line_size = 0;
	while (getline(&line, &line_size, file) != -1) {
		char *str = strip(line);
		if (str[0] == '\0' || str[0] == '#') {
			continue;
		}

		if (str[0] == '[') {
			char *end = strchr(str, ']');
			if (!end) {
				break; // malformed group header
			}
			*end = '\0';
			free(group);
			group = strdup(str + 1);
			continue;
		}

		char *eq = strchr(str, '=');
		if (!group || !eq) {
			continue;
		}
		*eq = '\0';
		if (!handler(data, group, strip(str), strip(eq + 1))) {
			break;
		}
	}

	free(line);
	free(group);
	fclose(file);
	return true;
}

static int cmp_str(const void *item, const void *cmp_to) {
	return strcmp(item, cmp_to);
}

list_t *xdg_data_dirs(void) {
	list_t *dirs = create_list();

	const char *data_home = getenv("XDG_DATA_HOME");
	const char *home = getenv("HOME");
	if (data_home && data_home[0] == '/') {
		list_add(dirs, strdup(data_home));
	} else if (home && *home) {
		list_add(dirs, format_str("%s/.local/share", home));
	}

	const char *data_dirs = getenv("XDG_DATA_DIRS");
	if (!data_dirs || !*data_dirs) {
		data_dirs = "/usr/local/share:/usr/share";
	}
	list_t *split = split_string(data_dirs, ":");
	for (int i = 0; i < split->length; ++i) {
		char *dir = split->items[i];
		if (dir[0] == '/' && list_seq_find(dirs, cmp_str, dir) == -1) {
			list_add(dirs, dir);
		} else {
			free(dir);
		}
	}
	list_free(split);

	return dirs;
}

struct entry_parse_state {
	char *icon;
	char *wm_class;
};

static bool handle_desktop_entry_key(void *data, const char *group,
		const char *key, const char *value) {
	struct entry_parse_state *state = data;
	if (strcmp(group, "Desktop Entry") != 0) {
		// The main group comes first, there's nothing more for us to read
		return false;
	}
	if (strcmp(key, "Icon") == 0 && !state->icon && *value) {
		state->icon = strdup(value);
	} else if (strcmp(key, "StartupWMClass") == 0 && !state->wm_class && *value) {
		state->wm_class = strdup(value);
	}
	return true;
}

static struct desktop_entry *desktop_entry_load(const char *path,
		const char *id) {
	struct entry_parse_state state = {0};
	if (!desktop_file_parse(path, handle_desktop_entry_key, &state)) {
		return NULL;
	}
	struct desktop_entry *entry = calloc(1, sizeof(*entry));
	if (!entry) {
		free(state.icon);
		free(state.wm_class);
		return NULL;
	}
	entry->id = strdup(id);
	entry->icon = state.icon;
	entry->wm_class = state.wm_class;
	return entry;
}

static void desktop_entry_destroy(struct desktop_entry *entry) {
	free(entry->id);
	free(entry->icon);
	free(entry->wm_class);
	free(entry);
}

static struct desktop_entry *find_entry(const char *id) {
	for (int i = 0; i < desktop_entries->length; ++i) {
		struct desktop_entry *entry = desktop_entries->items[i];
		if (strcmp(entry->id, id) == 0) {
			return entry;
		}
	}
	return NULL;
}

static bool is_dir(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void scan_applications_dir(const char *path, const char *prefix,
		int depth) {
	DIR *dir = opendir(path);
	if (!dir) {
		return;
	}

	static const char suffix[] = ".desktop";
	const size_t suffix_len = strlen(suffix);

	struct dirent *dirent;
	while ((dirent = readdir(dir))) {
		const char *name = dirent->d_name;
		if (name[0] == '.') {
			continue;
		}
		char *child = format_str("%s/%s", path, name);
		if (!child) {
			continue;
		}

		size_t len = strlen(name);
		bool is_desktop_file = len > suffix_len &&
			strcmp(name + len - suffix_len, suffix) == 0;
		if (!is_desktop_file && is_dir(child)) {
			// Desktop file IDs of entries in subdirectories are prefixed with
			// the subdirectory name and a dash
			if (depth < 3) {
				char *child_prefix = format_str("%s%s-", prefix, name);
				if (child_prefix) {
					scan_applications_dir(child, child_prefix, depth + 1);
					free(child_prefix);
				}
			}
		} else if (is_desktop_file) {
			char *id = format_str("%s%.*s", prefix,
				(int)(len - suffix_len), name);
			// Entries found earlier take precedence
			if (id && !find_entry(id)) {
				struct desktop_entry *entry = desktop_entry_load(child, id);
				if (entry) {
					list_add(desktop_entries, entry);
				}
			}
			free(id);
		}
		free(child);
	}
	closedir(dir);
}

static void load_desktop_entries(void) {
	if (desktop_entries) {
		return;
	}
	desktop_entries = create_list();

	list_t *data_dirs = xdg_data_dirs();
	for (int i = 0; i < data_dirs->length; ++i) {
		char *path = format_str("%s/applications", (char *)data_dirs->items[i]);
		if (path) {
			scan_applications_dir(path, "", 0);
			free(path);
		}
	}
	list_free_items_and_destroy(data_dirs);

	sway_log(SWAY_DEBUG, "Loaded %d desktop entries", desktop_entries->length);
}

/**
 * Reads the desktop entry with the given ID directly, in case it has been
 * installed after the desktop entries were loaded.
 */
static struct desktop_entry *load_new_desktop_entry(const char *id) {
	if (strchr(id, '/')) {
		return NULL;
	}

	struct desktop_entry *entry = NULL;
	list_t *data_dirs = xdg_data_dirs();
	for (int i = 0; i < data_dirs->length && !entry; ++i) {
		char *path = format_str("%s/applications/%s.desktop",
			(char *)data_dirs->items[i], id);
		if (path) {
			entry = desktop_entry_load(path, id);
			free(path);
		}
	}
	list_free_items_and_destroy(data_dirs);

	if (entry) {
		list_add(desktop_entries, entry);
	}
	return entry;
}

static bool match_id_case_insensitive(struct desktop_entry *entry,
		const char *app_id) {
	return strcasecmp(entry->id, app_id) == 0;
}

static bool match_wm_class(struct desktop_entry *entry, const char *app_id) {
	return entry->wm_class && strcmp(entry->wm_class, app_id) == 0;
}

static bool match_wm_class_case_insensitive(struct desktop_entry *entry,
		const char *app_id) {
	return entry->wm_class && strcasecmp(entry->wm_class, app_id) == 0;
}

// Matches reverse-DNS desktop file IDs to their last component, for instance
// "com.example.Foo" to the app_id "foo"
static bool match_id_last_component(struct desktop_entry *entry,
		const char *app_id) {
	const char *last = strrchr(entry->id, '.');
	return last && strcasecmp(last + 1, app_id) == 0;
}

char *desktop_entry_find_icon(const char *app_id) {
	if (!app_id || !*app_id) {
		return NULL;
	}

	load_desktop_entries();

	struct desktop_entry *entry = find_entry(app_id);
	if (!entry) {
		entry = load_new_desktop_entry(app_id);
	}

	bool (*matchers[])(struct desktop_entry *entry, const char *app_id) = {
		match_id_case_insensitive,
		match_wm_class,
		match_wm_class_case_insensitive,
		match_id_last_component,
	};
	size_t n_matchers = sizeof(matchers) / sizeof(matchers[0]);
	for (size_t i = 0; i < n_matchers && !(entry && entry->icon); ++i) {
		for (int j = 0; j < desktop_entries->length; ++j) {
			struct desktop_entry *candidate = desktop_entries->items[j];
			if (candidate->icon && matchers[i](candidate, app_id)) {
				entry = candidate;
				break;
			}
		}
	}

	if (!entry || !entry->icon) {
		sway_log(SWAY_DEBUG, "No desktop entry with an icon for '%s'", app_id);
		return NULL;
	}
	sway_log(SWAY_DEBUG, "Desktop entry '%s' for '%s' has icon '%s'",
		entry->id, app_id, entry->icon);
	return strdup(entry->icon);
}

void desktop_entries_reset(void) {
	if (!desktop_entries) {
		return;
	}
	for (int i = 0; i < desktop_entries->length; ++i) {
		desktop_entry_destroy(desktop_entries->items[i]);
	}
	list_free(desktop_entries);
	desktop_entries = NULL;
}
