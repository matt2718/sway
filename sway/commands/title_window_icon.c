#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/tree/container.h"
#include "util.h"

static const char expected_syntax[] =
	"Expected 'title_window_icon yes|no|toggle' or "
	"'title_window_icon padding|toggle <px>'";

static bool parse_padding(int argc, char **argv, int *padding) {
	if (argc < 1 || argc > 2 || (argc == 2 && strcmp(argv[1], "px") != 0)) {
		return false;
	}
	char *end;
	long value = strtol(argv[0], &end, 10);
	if (end == argv[0] || (*end && strcmp(end, "px") != 0) ||
			(*end && argc == 2) || value < 0 || value > 1000) {
		return false;
	}
	*padding = value;
	return true;
}

struct cmd_results *cmd_title_window_icon(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "title_window_icon", EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	struct sway_container *container = config->handler_context.container;
	if (!container) {
		return cmd_results_new(CMD_INVALID,
			"Only valid containers can have a title_window_icon");
	}

	int padding = -1;
	if (strcasecmp(argv[0], "padding") == 0) {
		if (!parse_padding(argc - 1, argv + 1, &padding)) {
			return cmd_results_new(CMD_INVALID, expected_syntax);
		}
		container->title_window_icon = true;
		container->title_window_icon_padding = padding;
	} else if (strcasecmp(argv[0], "toggle") == 0) {
		if (argc > 1 && !parse_padding(argc - 1, argv + 1, &padding)) {
			return cmd_results_new(CMD_INVALID, expected_syntax);
		}
		container->title_window_icon = !container->title_window_icon;
		if (padding >= 0) {
			container->title_window_icon_padding = padding;
		}
	} else {
		if (argc > 1) {
			return cmd_results_new(CMD_INVALID, expected_syntax);
		}
		// Like in i3, this resets the padding
		container->title_window_icon =
			parse_boolean(argv[0], container->title_window_icon);
		container->title_window_icon_padding = 0;
	}

	container_arrange_title_bar(container);
	return cmd_results_new(CMD_SUCCESS, NULL);
}
