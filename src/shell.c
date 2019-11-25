/*
 * Copyright © 2019 Collabora, Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "ivi-compositor.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <libweston-6/compositor.h>
#include <libweston-6/config-parser.h>

#include "shared/os-compatibility.h"

#include "agl-shell-server-protocol.h"

void
ivi_set_desktop_surface(struct ivi_surface *surface)
{
	assert(surface->role == IVI_SURFACE_ROLE_NONE);

	surface->role = IVI_SURFACE_ROLE_DESKTOP;
	wl_list_insert(&surface->ivi->surfaces, &surface->link);
}

/* TODO: Replace this with some callback or similar, to have
 * adjustable window management policy.
 */
void
ivi_reflow_outputs(struct ivi_compositor *ivi)
{
	struct ivi_surface *surface;
	struct ivi_output *output;
	int i = 0;

	if (wl_list_empty(&ivi->outputs))
		return;

	output = wl_container_of(ivi->outputs.next, output, link);

	wl_list_for_each(surface, &ivi->surfaces, link) {
		struct weston_desktop_surface *dsurface = surface->dsurface;

		int32_t w = output->area.width / 4;
		int32_t h = output->area.height / 2;
		int32_t x = output->output->x + output->area.x + w * (i % 4);
		int32_t y = output->output->y + output->area.y + h * (i / 4);

		if (surface->old_geom.width == -1) {
			weston_desktop_surface_set_size(dsurface, w, h);
			continue;
		} else {
			ivi_layout_set_mapped(surface);
			ivi_layout_set_position(surface, x, y, w, h);
		}

		if (++i == 8) {
			if (output->link.next == &ivi->outputs)
				break;

			output = wl_container_of(output->link.next,
					         output, link);
			i = 0;
		}
	}

	ivi_layout_commit(ivi);
}

void
ivi_layout_set_mapped(struct ivi_surface *surface)
{
	surface->pending.flags |= IVI_SURFACE_PROP_MAP;
}

void
ivi_layout_set_position(struct ivi_surface *surface,
			int32_t x, int32_t y,
			int32_t width, int32_t height)
{
	surface->pending.flags |= IVI_SURFACE_PROP_POSITION;
	surface->pending.x = x;
	surface->pending.y = y;
	surface->pending.width = width;
	surface->pending.height = height;
}

void
ivi_layout_commit(struct ivi_compositor *ivi)
{
	struct ivi_surface *surface;

	wl_list_for_each(surface, &ivi->surfaces, link) {
		struct weston_desktop_surface *dsurface = surface->dsurface;
		struct weston_surface *wsurface =
			weston_desktop_surface_get_surface(dsurface);
		struct weston_geometry geom;
		struct weston_view *view;

		/*
		 * TODO: Hoist view into ivi_struct. It doesn't need ot be part
		 * of the tagged union.
		 */
		switch (surface->role) {
		case IVI_SURFACE_ROLE_DESKTOP:
			view = surface->desktop.view;
			break;
		case IVI_SURFACE_ROLE_BACKGROUND:
			view = surface->bg.view;
			break;
		case IVI_SURFACE_ROLE_PANEL:
			view = surface->panel.view;
			break;
		default:
			continue;
		}

		if (surface->pending.flags & IVI_SURFACE_PROP_MAP) {
			view = weston_desktop_surface_create_view(dsurface);
			wsurface->is_mapped = true;

			surface->desktop.view = view;
			weston_layer_entry_insert(&ivi->normal.view_list,
						  &view->layer_link);
			weston_view_update_transform(view);
			weston_view_set_mask_infinite(view);
			weston_view_schedule_repaint(view);
		}

		geom = weston_desktop_surface_get_geometry(dsurface);

		if (surface->pending.flags & IVI_SURFACE_PROP_POSITION) {
			weston_desktop_surface_set_size(dsurface,
							surface->pending.width,
							surface->pending.height);
			weston_view_set_position(view,
						 surface->pending.x - geom.x,
						 surface->pending.y - geom.y);
		}

		surface->pending.flags = 0;
	}
}

int
ivi_shell_init(struct ivi_compositor *ivi)
{
	weston_layer_init(&ivi->background, ivi->compositor);
	weston_layer_init(&ivi->normal, ivi->compositor);
	weston_layer_init(&ivi->panel, ivi->compositor);
	weston_layer_init(&ivi->fullscreen, ivi->compositor);

	weston_layer_set_position(&ivi->background,
				  WESTON_LAYER_POSITION_BACKGROUND);
	weston_layer_set_position(&ivi->normal,
				  WESTON_LAYER_POSITION_NORMAL);
	weston_layer_set_position(&ivi->panel,
				  WESTON_LAYER_POSITION_UI);
	weston_layer_set_position(&ivi->fullscreen,
				  WESTON_LAYER_POSITION_FULLSCREEN);

	return 0;
}

static void
client_exec(const char *command, int fd)
{
	sigset_t sig;
	char s[32];

	/* Don't give the child our signal mask */
	sigfillset(&sig);
	sigprocmask(SIG_UNBLOCK, &sig, NULL);

	/* Launch clients as the user; don't give them the wrong euid */
	if (seteuid(getuid()) == -1) {
		weston_log("seteuid failed: %s\n", strerror(errno));
		return;
	}

	/* Duplicate fd to unset the CLOEXEC flag. We don't need to worry about
	 * clobbering fd, as we'll exit/exec either way.
	 */
	fd = dup(fd);
	if (fd == -1) {
		weston_log("dup failed: %s\n", strerror(errno));
		return;
	}

	snprintf(s, sizeof s, "%d", fd);
	setenv("WAYLAND_SOCKET", s, 1);

	execl("/bin/sh", "/bin/sh", "-c", command, NULL);
	weston_log("executing '%s' failed: %s", command, strerror(errno));
}

static struct wl_client *
launch_shell_client(struct ivi_compositor *ivi, const char *command)
{
	struct wl_client *client;
	int sock[2];
	pid_t pid;

	weston_log("launching' %s'\n", command);

	if (os_socketpair_cloexec(AF_UNIX, SOCK_STREAM, 0, sock) < 0) {
		weston_log("socketpair failed while launching '%s': %s\n",
			   command, strerror(errno));
		return NULL;
	}

	pid = fork();
	if (pid == -1) {
		close(sock[0]);
		close(sock[1]);
		weston_log("fork failed while launching '%s': %s\n",
			   command, strerror(errno));
		return NULL;
	}

	if (pid == 0) {
		client_exec(command, sock[1]);
		_Exit(EXIT_FAILURE);
	}
	close(sock[1]);

	client = wl_client_create(ivi->compositor->wl_display, sock[0]);
	if (!client) {
		close(sock[0]);
		weston_log("Failed to create wayland client for '%s'",
			   command);
		return NULL;
	}

	return client;
}

int
ivi_launch_shell_client(struct ivi_compositor *ivi)
{
	struct weston_config_section *section;
	char *command = NULL;

	section = weston_config_get_section(ivi->config, "shell-client",
					    NULL, NULL);
	if (section)
		weston_config_section_get_string(section, "command",
						 &command, NULL);

	if (!command)
		return -1;

	ivi->shell_client.client = launch_shell_client(ivi, command);
	if (!ivi->shell_client.client)
		return -1;

	return 0;
}

static void
shell_ready(struct wl_client *client, struct wl_resource *shell_res)
{
	struct ivi_compositor *ivi = wl_resource_get_user_data(shell_res);
	struct ivi_output *output;
	struct ivi_surface *surface, *tmp;

	/* Init already finished. Do nothing */
	if (ivi->shell_client.ready)
		return;

	ivi->shell_client.ready = true;
	/* TODO: Create a black screen and remove it here */

	wl_list_for_each(output, &ivi->outputs, link) {
		struct weston_desktop_surface *dsurf;
		struct weston_geometry geom;

		output->area.x = 0;
		output->area.y = 0;
		output->area.width = output->output->width;
		output->area.height = output->output->height;

		if (output->top) {
			dsurf = output->top->dsurface;
			geom = weston_desktop_surface_get_geometry(dsurf);

			output->area.y += geom.height;
			output->area.height -= geom.height;
		}
		if (output->bottom) {
			dsurf = output->bottom->dsurface;
			geom = weston_desktop_surface_get_geometry(dsurf);

			output->area.height -= geom.height;
		}
		if (output->left) {
			dsurf = output->left->dsurface;
			geom = weston_desktop_surface_get_geometry(dsurf);

			output->area.x += geom.width;
			output->area.width -= geom.width;
		}
		if (output->right) {
			dsurf = output->right->dsurface;
			geom = weston_desktop_surface_get_geometry(dsurf);

			output->area.width -= geom.width;
		}

		weston_log("Usable area: %dx%d+%d,%d\n",
			   output->area.width, output->area.height,
			   output->area.x, output->area.y);
	}

	wl_list_for_each_safe(surface, tmp, &ivi->pending_surfaces, link) {
		wl_list_remove(&surface->link);
		ivi_set_desktop_surface(surface);
	}
}

static void
shell_set_background(struct wl_client *client,
		     struct wl_resource *shell_res,
		     struct wl_resource *surface_res,
		     struct wl_resource *output_res)
{
	struct weston_head *head = weston_head_from_resource(output_res);
	struct weston_output *woutput = weston_head_get_output(head);
	struct ivi_output *output = to_ivi_output(woutput);
	struct weston_surface *wsurface = wl_resource_get_user_data(surface_res);
	struct weston_desktop_surface *dsurface;
	struct ivi_surface *surface;

	dsurface = weston_surface_get_desktop_surface(wsurface);
	if (!dsurface) {
		wl_resource_post_error(shell_res,
				       AGL_SHELL_ERROR_INVALID_ARGUMENT,
				       "surface must be a desktop surface");
		return;
	}

	surface = weston_desktop_surface_get_user_data(dsurface);
	if (surface->role != IVI_SURFACE_ROLE_NONE) {
		wl_resource_post_error(shell_res,
				       AGL_SHELL_ERROR_INVALID_ARGUMENT,
				       "surface already has another ivi role");
		return;
	}

	if (output->background) {
		wl_resource_post_error(shell_res,
				       AGL_SHELL_ERROR_BACKGROUND_EXISTS,
				       "output already has background");
		return;
	}

	surface->role = IVI_SURFACE_ROLE_BACKGROUND;
	surface->bg.output = output;
	wl_list_remove(&surface->link);
	wl_list_init(&surface->link);

	output->background = surface;

	weston_desktop_surface_set_maximized(dsurface, true);
	weston_desktop_surface_set_size(dsurface,
					output->output->width,
					output->output->height);
}

static void
shell_set_panel(struct wl_client *client,
		struct wl_resource *shell_res,
		struct wl_resource *surface_res,
		struct wl_resource *output_res,
		uint32_t edge)
{
	struct weston_head *head = weston_head_from_resource(output_res);
	struct weston_output *woutput = weston_head_get_output(head);
	struct ivi_output *output = to_ivi_output(woutput);
	struct weston_surface *wsurface = wl_resource_get_user_data(surface_res);
	struct weston_desktop_surface *dsurface;
	struct ivi_surface *surface;
	struct ivi_surface **member;
	int32_t width = 0, height = 0;

	dsurface = weston_surface_get_desktop_surface(wsurface);
	if (!dsurface) {
		wl_resource_post_error(shell_res,
				       AGL_SHELL_ERROR_INVALID_ARGUMENT,
				       "surface must be a desktop surface");
		return;
	}

	surface = weston_desktop_surface_get_user_data(dsurface);
	if (surface->role != IVI_SURFACE_ROLE_NONE) {
		wl_resource_post_error(shell_res,
				       AGL_SHELL_ERROR_INVALID_ARGUMENT,
				       "surface already has another ivi role");
		return;
	}

	switch (edge) {
	case AGL_SHELL_EDGE_TOP:
		member = &output->top;
		break;
	case AGL_SHELL_EDGE_BOTTOM:
		member = &output->bottom;
		break;
	case AGL_SHELL_EDGE_LEFT:
		member = &output->left;
		break;
	case AGL_SHELL_EDGE_RIGHT:
		member = &output->right;
		break;
	default:
		wl_resource_post_error(shell_res,
				       AGL_SHELL_ERROR_INVALID_ARGUMENT,
				       "invalid edge for panel");
		return;
	}

	if (*member) {
		wl_resource_post_error(shell_res,
				       AGL_SHELL_ERROR_BACKGROUND_EXISTS,
				       "output already has panel on this edge");
		return;
	}

	surface->role = IVI_SURFACE_ROLE_PANEL;
	surface->panel.output = output;
	surface->panel.edge = edge;
	wl_list_remove(&surface->link);
	wl_list_init(&surface->link);

	*member = surface;

	switch (surface->panel.edge) {
	case AGL_SHELL_EDGE_TOP:
	case AGL_SHELL_EDGE_BOTTOM:
		width = woutput->width;
		break;
	case AGL_SHELL_EDGE_LEFT:
	case AGL_SHELL_EDGE_RIGHT:
		height = woutput->height;
		break;
	}

	weston_desktop_surface_set_size(dsurface, width, height);
}

static const struct agl_shell_interface agl_shell_implementation = {
	.ready = shell_ready,
	.set_background = shell_set_background,
	.set_panel = shell_set_panel,
};

static void
unbind_agl_shell(struct wl_resource *resource)
{
}

static void
bind_agl_shell(struct wl_client *client,
	       void *data, uint32_t version, uint32_t id)
{
	struct ivi_compositor *ivi = data;
	struct wl_resource *resource;

	resource = wl_resource_create(client, &agl_shell_interface,
				      1, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

#if 0
	if (ivi->shell_client.client != client) {
		wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "client not authorized to use agl_shell");
		return;
	}
#endif

	if (ivi->shell_client.resource) {
		wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "agl_shell has already been bound");
		return;
	}

	wl_resource_set_implementation(resource, &agl_shell_implementation,
				       ivi, unbind_agl_shell);
	ivi->shell_client.resource = resource;
}

int
ivi_shell_create_global(struct ivi_compositor *ivi)
{
	ivi->agl_shell = wl_global_create(ivi->compositor->wl_display,
					  &agl_shell_interface, 1,
					  ivi, bind_agl_shell);
	if (!ivi->agl_shell) {
		weston_log("Failed to create wayland global.\n");
		return -1;
	}

	return 0;
}
