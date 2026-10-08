#ifndef _SWAY_ICON_NODE_H
#define _SWAY_ICON_NODE_H
#include <wlr/types/wlr_scene.h>

struct sway_icon;

struct sway_icon_node {
	int size;

	struct wlr_scene_node *node;
};

/**
 * Creates a scene node showing an icon of size×size logical pixels, rendered
 * for the scale of the outputs it's on.
 */
struct sway_icon_node *sway_icon_node_create(struct wlr_scene_tree *parent,
		struct sway_icon *icon, int size);

void sway_icon_node_set_icon(struct sway_icon_node *node, struct sway_icon *icon);

void sway_icon_node_set_size(struct sway_icon_node *node, int size);

#endif
