#include <stdlib.h>
#include <string.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/icon.h"
#include "sway/tree/root.h"
#include "sway/tree/view.h"

static void update_icon_iterator(struct sway_container *con, void *data) {
	if (con->view) {
		view_update_icon(con->view);
	}
}

struct cmd_results *cmd_title_window_icon_theme(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "title_window_icon_theme", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}

	char *theme = strdup(argv[0]);
	if (!theme) {
		return cmd_results_new(CMD_FAILURE, "Unable to allocate resource");
	}
	free(config->title_window_icon_theme);
	config->title_window_icon_theme = theme;

	// When the config is being read, icons are updated after reloading
	if (!config->reading) {
		sway_icons_reset();
		root_for_each_container(update_icon_iterator, NULL);
	}

	return cmd_results_new(CMD_SUCCESS, NULL);
}
