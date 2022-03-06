/*
 * Copyright © 2012-2021 Collabora, Ltd.
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
#include "policy.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/input.h>

#include <libweston/backend-drm.h>
#include <libweston/backend-wayland.h>
#ifdef HAVE_BACKEND_HEADLESS
#include <libweston/backend-headless.h>
#endif
#ifdef HAVE_BACKEND_X11
#include <libweston/backend-x11.h>
#endif
#include <libweston/libweston.h>
#include <libweston/windowed-output-api.h>
#include <libweston/config-parser.h>
#include <libweston/weston-log.h>
#include <weston/weston.h>

#include "shared/os-compatibility.h"
#include "shared/helpers.h"

#include "agl-shell-server-protocol.h"

#ifdef HAVE_REMOTING
#include "remote.h"
#endif

#ifdef HAVE_WALTHAM
#include <waltham-transmitter/transmitter_api.h>
#endif

static int cached_tm_mday = -1;
static struct weston_log_scope *log_scope;

struct ivi_compositor *
to_ivi_compositor(struct weston_compositor *ec)
{
	return weston_compositor_get_user_data(ec);
}

static void
handle_output_destroy(struct wl_listener *listener, void *data)
{
	struct ivi_output *output;

	output = wl_container_of(listener, output, output_destroy);
	assert(output->output == data);

	if (output->fullscreen_view.fs->view) {
		weston_surface_destroy(output->fullscreen_view.fs->view->surface);
		output->fullscreen_view.fs->view = NULL;
	}

	output->output = NULL;
	wl_list_remove(&output->output_destroy.link);
}

struct ivi_output *
to_ivi_output(struct weston_output *o)
{
	struct wl_listener *listener;
	struct ivi_output *output;

	listener = weston_output_get_destroy_listener(o, handle_output_destroy);
	output = wl_container_of(listener, output, output_destroy);

	return output;
}

static void
ivi_output_configure_app_id(struct ivi_output *ivi_output)
{
	if (ivi_output->config) {
		if (ivi_output->app_id != NULL)
			return;

		weston_config_section_get_string(ivi_output->config,
						 "agl-shell-app-id",
						 &ivi_output->app_id,
						 NULL);

		if (ivi_output->app_id == NULL)
			return;

		weston_log("Will place app_id %s on output %s\n",
				ivi_output->app_id, ivi_output->name);
	}
}

static struct ivi_output *
ivi_ensure_output(struct ivi_compositor *ivi, char *name,
		  struct weston_config_section *config)
{
	struct ivi_output *output = NULL;
	wl_list_for_each(output, &ivi->outputs, link) {
		if (strcmp(output->name, name) == 0) {
			free(name);
			return output;
		}
	}

	output = zalloc(sizeof *output);
	if (!output) {
		free(name);
		return NULL;
	}

	output->ivi = ivi;
	output->name = name;
	output->config = config;

	output->output = weston_compositor_create_output(ivi->compositor, name);
	if (!output->output) {
		free(output->name);
		free(output);
		return NULL;
	}

	output->output_destroy.notify = handle_output_destroy;
	weston_output_add_destroy_listener(output->output,
					   &output->output_destroy);

	wl_list_insert(&ivi->outputs, &output->link);
	ivi_output_configure_app_id(output);
	return output;
}

static int
count_heads(struct weston_output *output)
{
	struct weston_head *iter = NULL;
	int n = 0;

	while ((iter = weston_output_iterate_heads(output, iter)))
		++n;

	return n;
}

static void
handle_head_destroy(struct wl_listener *listener, void *data)
{
	struct weston_head *head = data;
	struct weston_output *output;

	wl_list_remove(&listener->link);
	free(listener);

	output = weston_head_get_output(head);

	/* On shutdown path, the output might be already gone. */
	if (!output)
		return;

	/* We're the last head */
	if (count_heads(output) <= 1)
		weston_output_destroy(output);
}

static void
add_head_destroyed_listener(struct weston_head *head)
{
	/* We already have a destroy listener */
	if (weston_head_get_destroy_listener(head, handle_head_destroy))
		return;

	struct wl_listener *listener = zalloc(sizeof *listener);
	if (!listener)
		return;

	listener->notify = handle_head_destroy;
	weston_head_add_destroy_listener(head, listener);
}

static int
drm_configure_output(struct ivi_output *output)
{
	struct ivi_compositor *ivi = output->ivi;
	struct weston_config_section *section = output->config;
	enum weston_drm_backend_output_mode mode =
		WESTON_DRM_BACKEND_OUTPUT_PREFERRED;
	char *modeline = NULL;
	char *gbm_format = NULL;
	char *seat = NULL;

	if (section) {
		char *m;
		weston_config_section_get_string(section, "mode", &m, "preferred");

		/* This should have been handled earlier */
		assert(strcmp(m, "off") != 0);

		if (ivi->cmdline.use_current_mode || strcmp(m, "current") == 0) {
			mode = WESTON_DRM_BACKEND_OUTPUT_CURRENT;
		} else if (strcmp(m, "preferred") != 0) {
			modeline = m;
			m = NULL;
		}
		free(m);

		weston_config_section_get_string(section, "gbm-format",
						 &gbm_format, NULL);

		weston_config_section_get_string(section, "seat", &seat, "");
	}

	if (ivi->drm_api->set_mode(output->output, mode, modeline) < 0) {
		weston_log("Cannot configure output using weston_drm_output_api.\n");
		free(modeline);
		return -1;
	}
	free(modeline);

	ivi->drm_api->set_gbm_format(output->output, gbm_format);
	free(gbm_format);

	ivi->drm_api->set_seat(output->output, seat);
	free(seat);

	return 0;
}

#define WINDOWED_DEFAULT_WIDTH 1024
#define WINDOWED_DEFAULT_HEIGHT 768

static int
windowed_configure_output(struct ivi_output *output)
{
	struct ivi_compositor *ivi = output->ivi;
	struct weston_config_section *section = output->config;
	int width = WINDOWED_DEFAULT_WIDTH;
	int height = WINDOWED_DEFAULT_HEIGHT;

	if (section) {
		char *mode;

		weston_config_section_get_string(section, "mode", &mode, NULL);
		if (!mode || sscanf(mode, "%dx%d", &width, &height) != 2) {
			weston_log("Invalid mode for output %s. Using defaults.\n",
				   output->name);
			width = WINDOWED_DEFAULT_WIDTH;
			height = WINDOWED_DEFAULT_HEIGHT;
		}
		free(mode);
	}

	if (ivi->cmdline.width)
		width = ivi->cmdline.width;
	if (ivi->cmdline.height)
		height = ivi->cmdline.height;
	if (ivi->cmdline.scale)
		weston_output_set_scale(output->output, ivi->cmdline.scale);

	if (ivi->window_api->output_set_size(output->output, width, height) < 0) {
		weston_log("Cannot configure output '%s' using weston_windowed_output_api.\n",
			   output->name);
		return -1;
	}

	weston_log("Configured windowed_output_api to %dx%d\n", width, height);

	return 0;
}

static int
parse_transform(const char *transform, uint32_t *out)
{
	static const struct { const char *name; uint32_t token; } transforms[] = {
		{ "normal",     WL_OUTPUT_TRANSFORM_NORMAL },
		{ "90",         WL_OUTPUT_TRANSFORM_90 },
		{ "180",        WL_OUTPUT_TRANSFORM_180 },
		{ "270",        WL_OUTPUT_TRANSFORM_270 },
		{ "flipped",    WL_OUTPUT_TRANSFORM_FLIPPED },
		{ "flipped-90", WL_OUTPUT_TRANSFORM_FLIPPED_90 },
		{ "flipped-180", WL_OUTPUT_TRANSFORM_FLIPPED_180 },
		{ "flipped-270", WL_OUTPUT_TRANSFORM_FLIPPED_270 },
	};

	for (size_t i = 0; i < ARRAY_LENGTH(transforms); i++)
		if (strcmp(transforms[i].name, transform) == 0) {
			*out = transforms[i].token;
			return 0;
		}

	*out = WL_OUTPUT_TRANSFORM_NORMAL;
	return -1;
}

static int
configure_output(struct ivi_output *output)
{
	struct ivi_compositor *ivi = output->ivi;
	struct weston_config_section *section = output->config;
	int32_t scale = 1;
	uint32_t transform = WL_OUTPUT_TRANSFORM_NORMAL;

	/*
	 * This can happen with the wayland backend with 'sprawl'. The config
	 * is hard-coded, so we don't need to do anything.
	 */
	if (!ivi->drm_api && !ivi->window_api)
		return 0;

	if (section) {
		char *t;

		weston_config_section_get_int(section, "scale", &scale, 1);
		weston_config_section_get_string(section, "transform", &t, "normal");
		if (parse_transform(t, &transform) < 0)
			weston_log("Invalid transform \"%s\" for output %s\n",
				   t, output->name);
		free(t);
	}

	weston_output_set_scale(output->output, scale);
	weston_output_set_transform(output->output, transform);

	if (ivi->drm_api)
		return drm_configure_output(output);
	else
		return windowed_configure_output(output);
}

/*
 * Reorgainizes the output's add array into two sections.
 * add[0..ret-1] are the heads that failed to get attached.
 * add[ret..add_len] are the heads that were successfully attached.
 *
 * The order between elements in each section is stable.
 */
static size_t
try_attach_heads(struct ivi_output *output)
{
	size_t fail_len = 0;

	for (size_t i = 0; i < output->add_len; ++i) {
		if (weston_output_attach_head(output->output, output->add[i]) < 0) {
			struct weston_head *tmp = output->add[i];
			memmove(&output->add[fail_len + 1], output->add[fail_len],
				sizeof output->add[0] * (i - fail_len));
			output->add[fail_len++] = tmp;
		}
	}

	return fail_len;
}

/*
 * Like try_attach_heads, this reorganizes the output's add array into a failed
 * and successful section.
 * i is the number of heads that already failed the previous step.
 */
static size_t
try_enable_output(struct ivi_output *output, size_t i)
{
	for (; i < output->add_len; ++i) {
		struct weston_head *head;

		if (weston_output_enable(output->output) == 0)
			break;

		head = output->add[output->add_len - 1];
		memmove(&output->add[i + 1], &output->add[i],
			sizeof output->add[0] * (output->add_len - i));
		output->add[i] = head;

		weston_head_detach(head);
	}

	return i;
}

static int
try_attach_enable_heads(struct ivi_output *output)
{
	size_t fail_len;
	assert(!output->output->enabled);

	fail_len = try_attach_heads(output);

	if (configure_output(output) < 0)
		return -1;

	fail_len = try_enable_output(output, fail_len);

	/* All heads failed to be attached */
	if (fail_len == output->add_len)
		return -1;

	/* For each successful head attached */
	for (size_t i = fail_len; i < output->add_len; ++i)
		add_head_destroyed_listener(output->add[i]);

	output->add_len = fail_len;
	return 0;
}

static int
process_output(struct ivi_output *output)
{
	if (output->output->enabled) {
		output->add_len = try_attach_heads(output);
		return output->add_len == 0 ? 0 : -1;
	}

	return try_attach_enable_heads(output);
}

static void
head_disable(struct ivi_compositor *ivi, struct weston_head *head)
{
	struct weston_output *output;
	struct ivi_output *ivi_output;
	struct wl_listener *listener;

	output = weston_head_get_output(head);
	assert(output);

	listener = weston_output_get_destroy_listener(output,
						      handle_output_destroy);
	assert(listener);

	ivi_output = wl_container_of(listener, ivi_output, output_destroy);
	assert(ivi_output->output == output);

	weston_head_detach(head);
	if (count_heads(ivi_output->output) == 0) {
		weston_output_disable(ivi_output->output);
	}
}

static struct weston_config_section *
find_controlling_output_config(struct weston_config *config,
			       const char *name)
{
	struct weston_config_section *section;
	char *same_as;
	int depth = 0;

	same_as = strdup(name);
	do {
		section = weston_config_get_section(config, "output",
						    "name", same_as);
		if (!section && depth > 0)
			weston_log("Configuration error: output section reffered"
				   "to by same-as=%s not found.\n", same_as);
		free(same_as);

		if (!section)
			return NULL;

		if (depth++ > 8) {
			weston_log("Configuration error: same-as nested too "
				   "deep for output '%s'.\n", name);
			return NULL;
		}

		weston_config_section_get_string(section, "same-as",
						 &same_as, NULL);
	} while (same_as);

	return section;
}

static void
head_prepare_enable(struct ivi_compositor *ivi, struct weston_head *head)
{
	const char *name = weston_head_get_name(head);
	struct weston_config_section *section;
	struct ivi_output *output;
	char *output_name = NULL;

	section = find_controlling_output_config(ivi->config, name);
	if (section) {
		char *mode;

		weston_config_section_get_string(section, "mode", &mode, NULL);
		if (mode && strcmp(mode, "off") == 0) {
			free(mode);
			return;
		}
		free(mode);

		weston_config_section_get_string(section, "name",
						 &output_name, NULL);
	} else {
		output_name = strdup(name);
	}

	if (!output_name)
		return;

	output = ivi_ensure_output(ivi, output_name, section);
	if (!output)
		return;

	if (output->add_len >= ARRAY_LENGTH(output->add))
		return;

	output->add[output->add_len++] = head;
}

static void
heads_changed(struct wl_listener *listener, void *arg)
{
	struct weston_compositor *compositor = arg;
	struct weston_head *head = NULL;
	struct ivi_compositor *ivi = to_ivi_compositor(compositor);
	struct ivi_output *output;

	while ((head = weston_compositor_iterate_heads(ivi->compositor, head))) {
		bool connected = weston_head_is_connected(head);
		bool enabled = weston_head_is_enabled(head);
		bool changed = weston_head_is_device_changed(head);
		bool non_desktop = weston_head_is_non_desktop(head);

		if (connected && !enabled && !non_desktop)
			head_prepare_enable(ivi, head);
		else if (!connected && enabled)
			head_disable(ivi, head);
		else if (enabled && changed)
			weston_log("Detected a monitor change on head '%s', "
				   "not bothering to do anything about it.\n",
				   weston_head_get_name(head));

		weston_head_reset_device_changed(head);
	}

	wl_list_for_each(output, &ivi->outputs, link) {
		if (output->add_len == 0)
			continue;

		if (process_output(output) < 0) {
			output->add_len = 0;
			ivi->init_failed = true;
		}
	}
}

#ifdef HAVE_WALTHAM
static int
load_waltham_plugin(struct ivi_compositor *ivi, struct weston_config *config)
{
	struct weston_compositor *compositor = ivi->compositor;
	int (*module_init)(struct weston_compositor *wc);

	module_init = weston_load_module("waltham-transmitter.so",
					 "wet_module_init");
	if (!module_init)
		return -1;

	if (module_init(compositor) < 0)
		return -1;

	ivi->waltham_transmitter_api = weston_get_transmitter_api(compositor);
	if (!ivi->waltham_transmitter_api) {
		weston_log("Failed to load waltham-transmitter plugin.\n");
		return -1;
	}

	weston_log("waltham-transmitter plug-in loaded\n");
	return 0;
}
#else
static int
load_waltham_plugin(struct ivi_compositor *ivi, struct weston_config *config)
{
	return -1;
}
#endif

#ifdef HAVE_REMOTING
static int
drm_backend_remoted_output_configure(struct weston_output *output,
				     struct weston_config_section *section,
				     char *modeline,
				     const struct weston_remoting_api *api)
{
	char *gbm_format = NULL;
	char *seat = NULL;
	char *host = NULL;
	char *pipeline = NULL;
	int port, ret;
	int32_t scale = 1;
	uint32_t transform = WL_OUTPUT_TRANSFORM_NORMAL;
	char *trans;

	ret = api->set_mode(output, modeline);
	if (ret < 0) {
		weston_log("Cannot configure an output \"%s\" using "
				"weston_remoting_api. Invalid mode\n",
				output->name);
		return -1;
	}

	weston_config_section_get_int(section, "scale", &scale, 1);
	weston_output_set_scale(output, scale);

	weston_config_section_get_string(section, "transform", &trans, "normal");
	if (parse_transform(trans, &transform) < 0) {
		weston_log("Invalid transform \"%s\" for output %s\n",
			   trans, output->name);
	}
	weston_output_set_transform(output, transform);

	weston_config_section_get_string(section, "gbm-format",
					 &gbm_format, NULL);
	api->set_gbm_format(output, gbm_format);
	free(gbm_format);

	weston_config_section_get_string(section, "seat", &seat, "");

	api->set_seat(output, seat);
	free(seat);

	weston_config_section_get_string(section, "gst-pipeline", &pipeline,
			NULL);
	if (pipeline) {
		api->set_gst_pipeline(output, pipeline);
		free(pipeline);
		return 0;
	}

	weston_config_section_get_string(section, "host", &host, NULL);
	weston_config_section_get_int(section, "port", &port, 0);
	if (!host || port <= 0 || 65533 < port) {
		weston_log("Cannot configure an output \"%s\". "
				"Need to specify gst-pipeline or "
				"host and port (1-65533).\n", output->name);
	}
	api->set_host(output, host);
	free(host);
	api->set_port(output, port);

	return 0;
}


static int
remote_output_init(struct ivi_output *ivi_output,
		   struct weston_compositor *compositor,
		   struct weston_config_section *section,
		   const struct weston_remoting_api *api)
{
	char *output_name, *modeline = NULL;
	int ret = -1;

	weston_config_section_get_string(section, "name", &output_name, NULL);
	if (!output_name)
		return ret;

	weston_config_section_get_string(section, "mode", &modeline, "off");
	if (strcmp(modeline, "off") == 0)
		goto err;

	ivi_output->output = api->create_output(compositor, output_name);
	if (!ivi_output->output) {
		weston_log("Cannot create remoted output \"%s\".\n",
				output_name);
		goto err;
	}

	ret = drm_backend_remoted_output_configure(ivi_output->output, section,
						   modeline, api);
	if (ret < 0) {
		weston_log("Cannot configure remoted output \"%s\".\n",
				output_name);
		goto err;
	}

	if (weston_output_enable(ivi_output->output) < 0) {
		weston_log("Enabling remoted output \"%s\" failed.\n",
				output_name);
		goto err;
	}

	free(modeline);
	free(output_name);
	weston_log("remoted output '%s' enabled\n", ivi_output->output->name);

	return 0;

err:
	free(modeline);
	free(output_name);
	if (ivi_output->output)
		weston_output_destroy(ivi_output->output);

	return ret;
}

static void
ivi_enable_remote_outputs(struct ivi_compositor *ivi)
{
	struct weston_config_section *remote_section = NULL;
	const char *section_name;
	struct weston_config *config = ivi->config;

	while (weston_config_next_section(config, &remote_section, &section_name)) {
		if (strcmp(section_name, "remote-output"))
			continue;

		struct ivi_output *ivi_output = NULL;
		bool output_found = false;
		char *_name = NULL;

		weston_config_section_get_string(remote_section,
						 "name", &_name, NULL);
		wl_list_for_each(ivi_output, &ivi->outputs, link) {
			if (!strcmp(ivi_output->name, _name)) {
				output_found = true;
				break;
			}
		}

		if (output_found) {
			free(_name);
			continue;
		}

		ivi_output = zalloc(sizeof(*ivi_output));
		if (!ivi_output) {
			free(_name);
			continue;
		}

		ivi_output->ivi = ivi;
		ivi_output->name = _name;
		ivi_output->config = remote_section;
		ivi_output->type = OUTPUT_REMOTE;

		if (remote_output_init(ivi_output, ivi->compositor,
				       remote_section, ivi->remoting_api)) {
			free(ivi_output->name);
			free(ivi_output);
			continue;
		}

		ivi_output->output_destroy.notify = handle_output_destroy;
		weston_output_add_destroy_listener(ivi_output->output,
						   &ivi_output->output_destroy);

		wl_list_insert(&ivi->outputs, &ivi_output->link);
		ivi_output_configure_app_id(ivi_output);
	}
}

static void
ivi_enable_waltham_outputs(struct ivi_compositor *ivi)
{
	struct weston_config_section *transmitter_section = NULL;
	const char *sect_name;
	struct weston_config *config = ivi->config;

	while (weston_config_next_section(config, &transmitter_section, &sect_name)) {
		if (strcmp(sect_name, "transmitter-output"))
			continue;

		struct ivi_output *ivi_output = NULL;
		bool output_found = false;
		char *_name = NULL;

		weston_config_section_get_string(transmitter_section,
				"name", &_name, NULL);
		wl_list_for_each(ivi_output, &ivi->outputs, link) {
			if (!strcmp(ivi_output->name, _name)) {
				output_found = true;
				break;
			}
		}

		if (output_found) {
			free(_name);
			continue;
		}

		ivi_output = zalloc(sizeof(*ivi_output));
		if (!ivi_output) {
			free(_name);
			continue;
		}

		ivi_output->ivi = ivi;
		ivi_output->name = _name;
		ivi_output->config = transmitter_section;

		if (remote_output_init(ivi_output, ivi->compositor,
					transmitter_section, ivi->remoting_api)) {
			free(ivi_output->name);
			free(ivi_output);
			continue;
		}

		ivi_output->type = OUTPUT_WALTHAM;
		ivi_output->output_destroy.notify = handle_output_destroy;
		weston_output_add_destroy_listener(ivi_output->output,
				&ivi_output->output_destroy);

		wl_list_insert(&ivi->outputs, &ivi_output->link);
		ivi_output_configure_app_id(ivi_output);
	}
}

static int
load_remoting_plugin(struct ivi_compositor *ivi, struct weston_config *config)
{
	struct weston_compositor *compositor = ivi->compositor;
	int (*module_init)(struct weston_compositor *wc);

	module_init = weston_load_module("remoting-plugin.so",
					 "weston_module_init");
	if (!module_init)
		return -1;

	if (module_init(compositor) < 0)
		return -1;

	ivi->remoting_api = weston_remoting_get_api(compositor);
	if (!ivi->remoting_api)
		return -1;
	return 0;
}
#else
static int
load_remoting_plugin(struct weston_compositor *compositor, struct weston_config *config)
{
	return -1;
}
#endif

static int
load_drm_backend(struct ivi_compositor *ivi, int *argc, char *argv[])
{
	struct weston_drm_backend_config config = {
		.base = {
			.struct_version = WESTON_DRM_BACKEND_CONFIG_VERSION,
			.struct_size = sizeof config,
		},
	};
	struct weston_config_section *section;
	int use_current_mode = 0;
	int use_pixman = 0;
	bool use_shadow;
	int ret;

	const struct weston_option options[] = {
		{ WESTON_OPTION_STRING, "seat", 0, &config.seat_id },
		{ WESTON_OPTION_INTEGER, "tty", 0, &config.tty },
		{ WESTON_OPTION_STRING, "drm-device", 0, &config.specific_device },
		{ WESTON_OPTION_BOOLEAN, "current-mode", 0, &use_current_mode },
		{ WESTON_OPTION_BOOLEAN, "use-pixman", 0, &use_pixman },
	};

	parse_options(options, ARRAY_LENGTH(options), argc, argv);
	config.use_pixman = use_pixman;
	ivi->cmdline.use_current_mode = use_current_mode;

	section = weston_config_get_section(ivi->config, "core", NULL, NULL);
	weston_config_section_get_string(section, "gbm-format",
					 &config.gbm_format, NULL);
	weston_config_section_get_uint(section, "pageflip-timeout",
				       &config.pageflip_timeout, 0);
	weston_config_section_get_bool(section, "pixman-shadow", &use_shadow, 1);
	config.use_pixman_shadow = use_shadow;

	ret = weston_compositor_load_backend(ivi->compositor, WESTON_BACKEND_DRM,
					     &config.base);
	if (ret < 0)
		return ret;

	ivi->drm_api = weston_drm_output_get_api(ivi->compositor);
	if (!ivi->drm_api) {
		weston_log("Cannot use drm output api.\n");
		ret = -1;
		goto error;
	}

	load_remoting_plugin(ivi, ivi->config);
	load_waltham_plugin(ivi, ivi->config);

error:
	free(config.gbm_format);
	free(config.seat_id);
	return ret;
}

static void
windowed_parse_common_options(struct ivi_compositor *ivi, int *argc, char *argv[],
			      bool *use_pixman, bool *fullscreen, int *output_count)
{
	struct weston_config_section *section;
	bool pixman;
	int fs = 0;

	const struct weston_option options[] = {
		{ WESTON_OPTION_INTEGER, "width", 0, &ivi->cmdline.width },
		{ WESTON_OPTION_INTEGER, "height", 0, &ivi->cmdline.height },
		{ WESTON_OPTION_INTEGER, "scale", 0, &ivi->cmdline.scale },
		{ WESTON_OPTION_BOOLEAN, "use-pixman", 0, &pixman },
		{ WESTON_OPTION_BOOLEAN, "fullscreen", 0, &fs },
		{ WESTON_OPTION_INTEGER, "output-count", 0, output_count },
	};

	section = weston_config_get_section(ivi->config, "core", NULL, NULL);
	weston_config_section_get_bool(section, "use-pixman", &pixman, 0);

	*output_count = 1;
	parse_options(options, ARRAY_LENGTH(options), argc, argv);
	*use_pixman = pixman;
	*fullscreen = fs;
}

static int
windowed_create_outputs(struct ivi_compositor *ivi, int output_count,
			const char *match_prefix, const char *name_prefix)
{
	struct weston_config_section *section = NULL;
	const char *section_name;
	char *default_output = NULL;
	int i = 0;
	size_t match_len = strlen(match_prefix);

	while (weston_config_next_section(ivi->config, &section, &section_name)) {
		char *output_name;

		if (i >= output_count)
			break;

		if (strcmp(section_name, "output") != 0)
			continue;

		weston_config_section_get_string(section, "name", &output_name, NULL);
		if (output_name == NULL)
			continue;
		if (strncmp(output_name, match_prefix, match_len) != 0) {
			free(output_name);
			continue;
		}

		if (ivi->window_api->create_head(ivi->compositor, output_name) < 0) {
			free(output_name);
			return -1;
		}

		free(output_name);
		++i;
	}

	for (; i < output_count; ++i) {
		if (asprintf(&default_output, "%s%d", name_prefix, i) < 0)
			return -1;

		if (ivi->window_api->create_head(ivi->compositor, default_output) < 0) {
			free(default_output);
			return -1;
		}

		free(default_output);
	}

	return 0;
}

static int
load_wayland_backend(struct ivi_compositor *ivi, int *argc, char *argv[])
{
	struct weston_wayland_backend_config config = {
		.base = {
			.struct_version = WESTON_WAYLAND_BACKEND_CONFIG_VERSION,
			.struct_size = sizeof config,
		},
	};
	struct weston_config_section *section;
	int sprawl = 0;
	int output_count;
	int ret;

	const struct weston_option options[] = {
		{ WESTON_OPTION_STRING, "display", 0, &config.display_name },
		{ WESTON_OPTION_STRING, "sprawl", 0, &sprawl },
	};

	windowed_parse_common_options(ivi, argc, argv, &config.use_pixman,
				      &config.fullscreen, &output_count);

	parse_options(options, ARRAY_LENGTH(options), argc, argv);
	config.sprawl = sprawl;

	section = weston_config_get_section(ivi->config, "shell", NULL, NULL);
	weston_config_section_get_string(section, "cursor-theme",
					 &config.cursor_theme, NULL);
	weston_config_section_get_int(section, "cursor-size",
				      &config.cursor_size, 32);

	ret = weston_compositor_load_backend(ivi->compositor, WESTON_BACKEND_WAYLAND,
					     &config.base);

	free(config.cursor_theme);
	free(config.display_name);

	if (ret < 0)
		return ret;

	ivi->window_api = weston_windowed_output_get_api(ivi->compositor);

	/*
	 * We will just assume if load_backend() finished cleanly and
	 * windowed_output_api is not present that wayland backend is started
	 * with --sprawl or runs on fullscreen-shell. In this case, all values
	 * are hardcoded, so nothing can be configured; simply create and
	 * enable an output.
	 */
	if (ivi->window_api == NULL)
		return 0;

	return windowed_create_outputs(ivi, output_count, "WL", "wayland");
}

#ifdef HAVE_BACKEND_X11
static int
load_x11_backend(struct ivi_compositor *ivi, int *argc, char *argv[])
{
	struct weston_x11_backend_config config = {
		.base = {
			.struct_version = WESTON_X11_BACKEND_CONFIG_VERSION,
			.struct_size = sizeof config,
		},
	};
	int no_input = 0;
	int output_count;
	int ret;

	const struct weston_option options[] = {
	       { WESTON_OPTION_BOOLEAN, "no-input", 0, &no_input },
	};

	windowed_parse_common_options(ivi, argc, argv, &config.use_pixman,
				      &config.fullscreen, &output_count);

	parse_options(options, ARRAY_LENGTH(options), argc, argv);
	config.no_input = no_input;

	ret = weston_compositor_load_backend(ivi->compositor, WESTON_BACKEND_X11,
					     &config.base);

	if (ret < 0)
		return ret;

	ivi->window_api = weston_windowed_output_get_api(ivi->compositor);
	if (!ivi->window_api) {
		weston_log("Cannot use weston_windowed_output_api.\n");
		return -1;
	}

	return windowed_create_outputs(ivi, output_count, "X", "screen");
}
#else
static int
load_x11_backend(struct ivi_compositor *ivi, int *argc, char *argv[])
{
	return -1;
}
#endif

#ifdef HAVE_BACKEND_HEADLESS
static int
load_headless_backend(struct ivi_compositor *ivi, int *argc, char **argv)
{
	struct weston_headless_backend_config config = {};
	int ret = 0;

	bool use_pixman;
	bool fullscreen;
	bool use_gl;
	int output_count;

	struct weston_compositor *c = ivi->compositor;

	const struct weston_option options[] = {
		{ WESTON_OPTION_BOOLEAN, "use-pixman", 0, &use_pixman },
		{ WESTON_OPTION_BOOLEAN, "use-gl", 0, &use_gl },
	};

	windowed_parse_common_options(ivi, argc, argv, &use_pixman,
			&fullscreen, &output_count);

	parse_options(options, ARRAY_LENGTH(options), argc, argv);
	config.use_pixman = use_pixman;
	config.use_gl = use_gl;

	config.base.struct_version = WESTON_HEADLESS_BACKEND_CONFIG_VERSION;
	config.base.struct_size = sizeof(struct weston_headless_backend_config);

	/* load the actual headless-backend and configure it */
	ret = weston_compositor_load_backend(c, WESTON_BACKEND_HEADLESS,
					     &config.base);
	if (ret < 0)
		return ret;

	ivi->window_api = weston_windowed_output_get_api(c);
	if (!ivi->window_api) {
		weston_log("Cannot use weston_windowed_output_api.\n");
		return -1;
	}

	if (ivi->window_api->create_head(c, "headless") < 0) {
		weston_log("Cannot create headless back-end\n");
		return -1;
	}

	return 0;
}
#else
static int
load_headless_backend(struct ivi_compositor *ivi, int *argc, char **argv)
{
	return -1;
}
#endif

static int
load_backend(struct ivi_compositor *ivi, const char *backend,
	     int *argc, char *argv[])
{
	if (strcmp(backend, "drm-backend.so") == 0) {
		load_drm_backend(ivi, argc, argv);
		return load_drm_backend(ivi, argc, argv);
	} else if (strcmp(backend, "wayland-backend.so") == 0) {
		return load_wayland_backend(ivi, argc, argv);
	} else if (strcmp(backend, "x11-backend.so") == 0) {
		return load_x11_backend(ivi, argc, argv);
	} else if (strcmp(backend, "headless-backend.so") == 0) {
		return load_headless_backend(ivi, argc, argv);
	}

	weston_log("fatal: unknown backend '%s'.\n", backend);
	return -1;
}

static int
load_modules(struct ivi_compositor *ivi, const char *modules,
	     int *argc, char *argv[], bool *xwayland)
{
	const char *p, *end;
	char buffer[256];
	int (*module_init)(struct weston_compositor *wc, int argc, char *argv[]);

	if (modules == NULL)
		return 0;

	p = modules;
	while (*p) {
		end = strchrnul(p, ',');
		snprintf(buffer, sizeof buffer, "%.*s", (int) (end - p), p);

		if (strstr(buffer, "xwayland.so")) {
			weston_log("Xwayland plug-in not supported!\n");
			p = end;
			while (*p == ',')
				p++;
			continue;
		}

		if (strstr(buffer, "systemd-notify.so")) {
			weston_log("systemd-notify plug-in already loaded!\n");
			p = end;
			while (*p == ',')
				p++;
			continue;
		}

		module_init = weston_load_module(buffer, "wet_module_init");
		if (!module_init)
			return -1;

		if (module_init(ivi->compositor, *argc, argv) < 0)
			return -1;

		p = end;
		while (*p == ',')
			p++;
	}

	return 0;
}


static char *
choose_default_backend(void)
{
	char *backend = NULL;

	if (getenv("WAYLAND_DISPLAY") || getenv("WAYLAND_SOCKET"))
		backend = strdup("wayland-backend.so");
	else if (getenv("DISPLAY"))
		backend = strdup("x11-backend.so");
	else
		backend = strdup("drm-backend.so");

	return backend;
}

static int
compositor_init_config(struct weston_compositor *compositor,
		       struct weston_config *config)
{
	struct xkb_rule_names xkb_names;
	struct weston_config_section *section;
	int repaint_msec;
	bool vt_switching;
	bool require_input;

	/* agl-compositor.ini [keyboard] */
	section = weston_config_get_section(config, "keyboard", NULL, NULL);
	weston_config_section_get_string(section, "keymap_rules",
					 (char **) &xkb_names.rules, NULL);
	weston_config_section_get_string(section, "keymap_model",
					 (char **) &xkb_names.model, NULL);
	weston_config_section_get_string(section, "keymap_layout",
					 (char **) &xkb_names.layout, NULL);
	weston_config_section_get_string(section, "keymap_variant",
					 (char **) &xkb_names.variant, NULL);
	weston_config_section_get_string(section, "keymap_options",
					 (char **) &xkb_names.options, NULL);

	if (weston_compositor_set_xkb_rule_names(compositor, &xkb_names) < 0)
		return -1;

	weston_config_section_get_int(section, "repeat-rate",
				      &compositor->kb_repeat_rate, 40);
	weston_config_section_get_int(section, "repeat-delay",
				      &compositor->kb_repeat_delay, 400);

	weston_config_section_get_bool(section, "vt-switching",
				       &vt_switching, false);
	compositor->vt_switching = vt_switching;

	/* agl-compositor.ini [core] */
	section = weston_config_get_section(config, "core", NULL, NULL);

	weston_config_section_get_bool(section, "require-input", &require_input, true);
	compositor->require_input = require_input;

	weston_config_section_get_int(section, "repaint-window", &repaint_msec,
				      compositor->repaint_msec);
	if (repaint_msec < -10 || repaint_msec > 1000) {
		weston_log("Invalid repaint_window value in config: %d\n",
			   repaint_msec);
	} else {
		compositor->repaint_msec = repaint_msec;
	}
	weston_log("Output repaint window is %d ms maximum.\n",
		   compositor->repaint_msec);

	return 0;
}

struct ivi_surface *
to_ivi_surface(struct weston_surface *surface)
{
	struct weston_desktop_surface *dsurface;

	dsurface = weston_surface_get_desktop_surface(surface);
	if (!dsurface)
		return NULL;

	return weston_desktop_surface_get_user_data(dsurface);
}

static void
activate_binding(struct weston_seat *seat,
		 struct weston_view *focus_view)
{
	struct weston_surface *focus = focus_view->surface;
	struct weston_surface *main_surface =
		weston_surface_get_main_surface(focus);
	struct ivi_surface *surface;

	surface = to_ivi_surface(main_surface);
	if (!surface)
		return;

	weston_seat_set_keyboard_focus(seat, focus);
}

static void
click_to_activate_binding(struct weston_pointer *pointer,
			  const struct timespec *time,
			  uint32_t button, void *data)
{
	if (pointer->grab != &pointer->default_grab)
		return;
	if (pointer->focus == NULL)
		return;

	activate_binding(pointer->seat, pointer->focus);
}

static void
touch_to_activate_binding(struct weston_touch *touch,
			  const struct timespec *time,
			  void *data)
{
	if (touch->grab != &touch->default_grab)
		return;
	if (touch->focus == NULL)
		return;

	activate_binding(touch->seat, touch->focus);
}

static void
add_bindings(struct weston_compositor *compositor)
{
	weston_compositor_add_button_binding(compositor, BTN_LEFT, 0,
					     click_to_activate_binding,
					     NULL);
	weston_compositor_add_button_binding(compositor, BTN_RIGHT, 0,
					     click_to_activate_binding,
					     NULL);
	weston_compositor_add_touch_binding(compositor, 0,
					    touch_to_activate_binding,
					    NULL);
}

static int
create_listening_socket(struct wl_display *display, const char *socket_name)
{
	if (socket_name) {
		if (wl_display_add_socket(display, socket_name)) {
			weston_log("fatal: failed to add socket: %s\n",
				   strerror(errno));
			return -1;
		}
	} else {
		socket_name = wl_display_add_socket_auto(display);
		if (!socket_name) {
			weston_log("fatal: failed to add socket: %s\n",
				   strerror(errno));
			return -1;
		}
	}

	setenv("WAYLAND_DISPLAY", socket_name, 1);

	return 0;
}

static bool
global_filter(const struct wl_client *client, const struct wl_global *global,
	      void *data)
{
	return true;
}

static int
load_config(struct weston_config **config, bool no_config,
	    const char *config_file)
{
	const char *file = "agl-compositor.ini";
	const char *full_path;

	if (config_file)
		file = config_file;

	if (!no_config)
		*config = weston_config_parse(file);

	if (*config) {
		full_path = weston_config_get_full_path(*config);

		weston_log("Using config file '%s'.\n", full_path);
		setenv(WESTON_CONFIG_FILE_ENV_VAR, full_path, 1);

		return 0;
	}

	if (config_file && !no_config) {
		weston_log("fatal: error opening or reading config file '%s'.\n",
			config_file);

		return -1;
	}

	weston_log("Starting with no config file.\n");
	setenv(WESTON_CONFIG_FILE_ENV_VAR, "", 1);

	return 0;
}

static FILE *logfile;

static char *
log_timestamp(char *buf, size_t len)
{
	struct timeval tv;
	struct tm *brokendown_time;
	char datestr[128];
	char timestr[128];

	gettimeofday(&tv, NULL);

	brokendown_time = localtime(&tv.tv_sec);
	if (brokendown_time == NULL) {
		snprintf(buf, len, "%s", "[(NULL)localtime] ");
		return buf;
	}

	memset(datestr, 0, sizeof(datestr));
	if (brokendown_time->tm_mday != cached_tm_mday) {
		strftime(datestr, sizeof(datestr), "Date: %Y-%m-%d %Z\n",
				brokendown_time);
		cached_tm_mday = brokendown_time->tm_mday;
	}

	strftime(timestr, sizeof(timestr), "%H:%M:%S", brokendown_time);
	/* if datestr is empty it prints only timestr*/
	snprintf(buf, len, "%s[%s.%03li]", datestr,
			timestr, (tv.tv_usec / 1000));

	return buf;
}

static void
custom_handler(const char *fmt, va_list arg)
{
	char timestr[512];

	weston_log_scope_printf(log_scope, "%s libwayland: ",
			log_timestamp(timestr, sizeof(timestr)));
	weston_log_scope_vprintf(log_scope, fmt, arg);
}

static void
log_file_open(const char *filename)
{
	wl_log_set_handler_server(custom_handler);

	if (filename)
		logfile = fopen(filename, "a");

	if (!logfile) {
		logfile = stderr;
	} else {
		os_fd_set_cloexec(fileno(logfile));
		setvbuf(logfile, NULL, _IOLBF, 256);
	}
}

static void
log_file_close(void)
{
	if (logfile && logfile != stderr)
		fclose(logfile);
	logfile = stderr;
}

static int
vlog(const char *fmt, va_list ap)
{
	const char *oom = "Out of memory";
	char timestr[128];
	int len = 0;
	char *str;

	if (weston_log_scope_is_enabled(log_scope)) {
		int len_va;
		char *xlog_timestamp = log_timestamp(timestr, sizeof(timestr));
		len_va = vasprintf(&str, fmt, ap);
		if (len_va >= 0) {
			len = weston_log_scope_printf(log_scope, "%s %s",
					xlog_timestamp, str);
			free(str);
		} else {
			len = weston_log_scope_printf(log_scope, "%s %s",
					xlog_timestamp, oom);
		}
	}

	return len;
}


static int
vlog_continue(const char *fmt, va_list ap)
{
	return weston_log_scope_vprintf(log_scope, fmt, ap);
}

static int
on_term_signal(int signo, void *data)
{
	struct wl_display *display = data;

	weston_log("caught signal %d\n", signo);
	wl_display_terminate(display);

	return 1;
}

static void
handle_exit(struct weston_compositor *compositor)
{
	wl_display_terminate(compositor->wl_display);
}

static void
usage(int error_code)
{
	FILE *out = error_code == EXIT_SUCCESS ? stdout : stderr;
	fprintf(out,
		"Usage: agl-compositor [OPTIONS]\n"
		"\n"
		"This is " PACKAGE_STRING ", the reference compositor for\n"
		"Automotive Grade Linux. " PACKAGE_STRING " supports multiple "
		"backends,\nand depending on which backend is in use different "
		"options will be accepted.\n"
		"\n"
		"Core options:\n"
		"\n"
		"  --version\t\tPrint agl-compositor version\n"
		"  -B, --backend=MODULE\tBackend module, one of\n"
			"\t\t\t\tdrm-backend.so\n"
			"\t\t\t\twayland-backend.so\n"
			"\t\t\t\tx11-backend.so\n"
			"\t\t\t\theadless-backend.so\n"
		"  -S, --socket=NAME\tName of socket to listen on\n"
		"  --log=FILE\t\tLog to the given file\n"
		"  -c, --config=FILE\tConfig file to load, defaults to agl-compositor.ini\n"
		"  --no-config\t\tDo not read agl-compositor.ini\n"
		"  --debug\t\tEnable debug extension(s)\n"
		"  -h, --help\t\tThis help message\n"
		"\n");
	exit(error_code);
}

static char *
copy_command_line(int argc, char * const argv[])
{
	FILE *fp;
	char *str = NULL;
	size_t size = 0;
	int i;

	fp = open_memstream(&str, &size);
	if (!fp)
		return NULL;

	fprintf(fp, "%s", argv[0]);
	for (i = 1; i < argc; i++)
		fprintf(fp, " %s", argv[i]);
	fclose(fp);

	return str;
}

WL_EXPORT
int wet_main(int argc, char *argv[])
{
	struct ivi_compositor ivi = { 0 };
	char *cmdline;
	struct wl_display *display = NULL;
	struct wl_event_loop *loop;
	struct wl_event_source *signals[3] = { 0 };
	struct weston_config_section *section;
	/* Command line options */
	char *backend = NULL;
	char *socket_name = NULL;
	char *log = NULL;
	char *modules = NULL;
	char *option_modules = NULL;
	int help = 0;
	int version = 0;
	int no_config = 0;
	int debug = 0;
	char *config_file = NULL;
	struct weston_log_context *log_ctx = NULL;
	struct weston_log_subscriber *logger;
	int ret = EXIT_FAILURE;
	bool xwayland = false;

	const struct weston_option core_options[] = {
		{ WESTON_OPTION_STRING, "backend", 'B', &backend },
		{ WESTON_OPTION_STRING, "socket", 'S', &socket_name },
		{ WESTON_OPTION_STRING, "log", 0, &log },
		{ WESTON_OPTION_BOOLEAN, "help", 'h', &help },
		{ WESTON_OPTION_BOOLEAN, "version", 0, &version },
		{ WESTON_OPTION_BOOLEAN, "no-config", 0, &no_config },
		{ WESTON_OPTION_BOOLEAN, "debug", 0, &debug },
		{ WESTON_OPTION_STRING, "config", 'c', &config_file },
		{ WESTON_OPTION_STRING, "modules", 0, &option_modules },
	};

	wl_list_init(&ivi.outputs);
	wl_list_init(&ivi.surfaces);
	wl_list_init(&ivi.pending_surfaces);
	wl_list_init(&ivi.popup_pending_apps);
	wl_list_init(&ivi.fullscreen_pending_apps);
	wl_list_init(&ivi.split_pending_apps);
	wl_list_init(&ivi.remote_pending_apps);
	wl_list_init(&ivi.desktop_clients);

	/* Prevent any clients we spawn getting our stdin */
	os_fd_set_cloexec(STDIN_FILENO);

	cmdline = copy_command_line(argc, argv);
	parse_options(core_options, ARRAY_LENGTH(core_options), &argc, argv);

	if (help)
		usage(EXIT_SUCCESS);

	if (version) {
		printf(PACKAGE_STRING "\n");
		return EXIT_SUCCESS;
	}

	log_ctx = weston_log_ctx_compositor_create();
	if (!log_ctx) {
		fprintf(stderr, "Failed to initialize weston debug framework.\n");
		return ret;
	}

        log_scope = weston_compositor_add_log_scope(log_ctx, "log",
						    "agl-compositor log\n",
						    NULL, NULL, NULL);

	log_file_open(log);
	weston_log_set_handler(vlog, vlog_continue);

	logger = weston_log_subscriber_create_log(logfile);
	weston_log_subscribe(log_ctx, logger, "log");

	weston_log("Command line: %s\n", cmdline);
	free(cmdline);

	if (load_config(&ivi.config, no_config, config_file) < 0)
		goto error_signals;
	section = weston_config_get_section(ivi.config, "core", NULL, NULL);
	if (!backend) {
		weston_config_section_get_string(section, "backend", &backend,
						 NULL);
		if (!backend)
			backend = choose_default_backend();
	}
	/* from [core] */
	weston_config_section_get_bool(section, "hide-cursor", &ivi.hide_cursor, false);
	weston_config_section_get_bool(section, "activate-by-default", &ivi.activate_by_default, true);

	display = wl_display_create();
	loop = wl_display_get_event_loop(display);

	wl_display_set_global_filter(display,
				     global_filter, &ivi);

	/* Register signal handlers so we shut down cleanly */

	signals[0] = wl_event_loop_add_signal(loop, SIGTERM, on_term_signal,
					      display);
	signals[1] = wl_event_loop_add_signal(loop, SIGINT, on_term_signal,
					      display);
	signals[2] = wl_event_loop_add_signal(loop, SIGQUIT, on_term_signal,
					      display);

	for (size_t i = 0; i < ARRAY_LENGTH(signals); ++i)
		if (!signals[i])
			goto error_signals;

	ivi.compositor = weston_compositor_create(display, log_ctx, &ivi);
	if (!ivi.compositor) {
		weston_log("fatal: failed to create compositor.\n");
		goto error_signals;
	}

	if (compositor_init_config(ivi.compositor, ivi.config) < 0)
		goto error_compositor;

	if (load_backend(&ivi, backend, &argc, argv) < 0) {
		weston_log("fatal: failed to create compositor backend.\n");
		goto error_compositor;
	}

	ivi.heads_changed.notify = heads_changed;
	weston_compositor_add_heads_changed_listener(ivi.compositor,
						     &ivi.heads_changed);

	if (ivi_desktop_init(&ivi) < 0)
		goto error_compositor;

	ivi_seat_init(&ivi);

	/* load additional modules */
	weston_config_section_get_string(section, "modules", &modules, "");
	if (load_modules(&ivi, modules, &argc, argv, &xwayland) < 0)
		goto error_compositor;

	if (load_modules(&ivi, option_modules, &argc, argv, &xwayland) < 0)
		goto error_compositor;

	if (ivi_policy_init(&ivi) < 0)
		goto error_compositor;

	if (ivi_shell_init(&ivi) < 0)
		goto error_compositor;

	add_bindings(ivi.compositor);

	weston_compositor_flush_heads_changed(ivi.compositor);

	if (ivi.remoting_api)
		ivi_enable_remote_outputs(&ivi);

	if (ivi.waltham_transmitter_api)
		ivi_enable_waltham_outputs(&ivi);

	if (create_listening_socket(display, socket_name) < 0)
		goto error_compositor;

	ivi_shell_init_black_fs(&ivi);

	ivi.compositor->exit = handle_exit;

	weston_compositor_wake(ivi.compositor);

	ivi_shell_create_global(&ivi);
	ivi_launch_shell_client(&ivi);
	if (debug)
		ivi_screenshooter_create(&ivi);
	ivi_agl_systemd_notify(&ivi);

	wl_display_run(display);

	ret = ivi.compositor->exit_code;

	wl_display_destroy_clients(display);

error_compositor:
	free(backend);
	backend = NULL;
	free(modules);
	modules = NULL;

	weston_compositor_tear_down(ivi.compositor);

	weston_compositor_log_scope_destroy(log_scope);
	log_scope = NULL;

	weston_log_ctx_compositor_destroy(ivi.compositor);
	weston_compositor_destroy(ivi.compositor);

	weston_log_subscriber_destroy_log(logger);

	ivi_policy_destroy(ivi.policy);

error_signals:
	for (size_t i = 0; i < ARRAY_LENGTH(signals); ++i)
		if (signals[i])
			wl_event_source_remove(signals[i]);

	wl_display_destroy(display);

	log_file_close();
	if (ivi.config)
		weston_config_destroy(ivi.config);

	return ret;
}
