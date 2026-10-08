#include <math.h>
#include <stdlib.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include "sway/icon.h"
#include "sway/sway_icon_node.h"

struct icon_node {
	struct sway_icon_node props;

	struct wlr_scene_buffer *buffer_node;
	struct sway_icon *icon;
	float scale; // 0 until the node is shown on an output

	struct wl_listener outputs_update;
	struct wl_listener destroy;
};

static void icon_node_update(struct icon_node *node) {
	if (node->scale <= 0) {
		return;
	}
	struct wlr_buffer *buffer = NULL;
	if (node->icon) {
		buffer = sway_icon_get_buffer(node->icon,
			ceil(node->props.size * node->scale));
	}
	wlr_scene_buffer_set_buffer(node->buffer_node, buffer);
}

static void handle_outputs_update(struct wl_listener *listener, void *data) {
	struct icon_node *node = wl_container_of(listener, node, outputs_update);
	struct wlr_scene_outputs_update_event *event = data;

	float scale = 0;
	for (size_t i = 0; i < event->size; i++) {
		scale = fmax(scale, event->active[i]->output->scale);
	}

	// Keep the current buffer while the node isn't shown anywhere
	if (scale > 0 && scale != node->scale) {
		node->scale = scale;
		icon_node_update(node);
	}
}

static void handle_destroy(struct wl_listener *listener, void *data) {
	struct icon_node *node = wl_container_of(listener, node, destroy);
	wl_list_remove(&node->outputs_update.link);
	wl_list_remove(&node->destroy.link);
	sway_icon_unref(node->icon);
	free(node);
}

struct sway_icon_node *sway_icon_node_create(struct wlr_scene_tree *parent,
		struct sway_icon *icon, int size) {
	struct icon_node *node = calloc(1, sizeof(*node));
	if (!node) {
		return NULL;
	}

	struct wlr_scene_buffer *buffer_node = wlr_scene_buffer_create(parent, NULL);
	if (!buffer_node) {
		free(node);
		return NULL;
	}

	node->buffer_node = buffer_node;
	node->props.node = &buffer_node->node;
	node->props.size = size;
	node->icon = icon ? sway_icon_ref(icon) : NULL;

	node->destroy.notify = handle_destroy;
	wl_signal_add(&buffer_node->node.events.destroy, &node->destroy);
	node->outputs_update.notify = handle_outputs_update;
	wl_signal_add(&buffer_node->events.outputs_update, &node->outputs_update);

	wlr_scene_buffer_set_dest_size(buffer_node, size, size);
	return &node->props;
}

void sway_icon_node_set_icon(struct sway_icon_node *props, struct sway_icon *icon) {
	struct icon_node *node = wl_container_of(props, node, props);
	if (node->icon == icon) {
		return;
	}
	sway_icon_unref(node->icon);
	node->icon = icon ? sway_icon_ref(icon) : NULL;
	icon_node_update(node);
}

void sway_icon_node_set_size(struct sway_icon_node *props, int size) {
	struct icon_node *node = wl_container_of(props, node, props);
	if (props->size == size) {
		return;
	}
	props->size = size;
	wlr_scene_buffer_set_dest_size(node->buffer_node, size, size);
	icon_node_update(node);
}
