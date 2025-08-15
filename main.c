#include <assert.h>
#include <errno.h>
#include <cairo/cairo.h>
#include <getopt.h>
#include <libinput.h>
#include <libudev.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <fontconfig/fontconfig.h>
#include "devmgr.h"
#include "shm.h"
#include "pango.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

struct wsk_keypress {
	xkb_keysym_t sym;
	char name[128];
	char utf8[128];
	int count;
	struct wsk_keypress *next;
};

struct wsk_output {
	struct wl_output *output;
	int scale, width, heigh;
	enum wl_output_subpixel subpixel;
	struct wsk_output *next;
};

struct wsk_state {
	int devmgr;
	pid_t devmgr_pid;
	struct udev *udev;
	struct libinput *libinput;

	uint32_t foreground, background, specialfg;
	const char *font;
	int timeout;

	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct zxdg_output_manager_v1 *output_mgr;
	struct zwlr_layer_shell_v1 *layer_shell;

	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	uint32_t width, height;
	bool frame_scheduled, dirty;
	struct pool_buffer buffers[2];
	struct pool_buffer *current_buffer;
	struct wsk_output *output, *outputs;

	struct xkb_state *xkb_state;
	struct xkb_context *xkb_context;
	struct xkb_keymap *xkb_keymap;

	struct wsk_keypress *keys;
	struct timespec last_key;

	bool run;
};

static void cairo_set_source_u32(cairo_t *cairo, uint32_t color) {
	cairo_set_source_rgba(cairo,
			(color >> (3*8) & 0xFF) / 255.0,
			(color >> (2*8) & 0xFF) / 255.0,
			(color >> (1*8) & 0xFF) / 255.0,
			(color >> (0*8) & 0xFF) / 255.0);
}

// 화면을 벗어나는 키들을 제거하는 함수
static void trim_keys_by_width(struct wsk_state *state) {
	if (!state->keys) return;

    uint32_t max_width = 1800;  // 안전 마진 포함

    // 🔥 간단: 현재 키들의 총 너비 계산
    cairo_surface_t *temp_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *temp_cairo = cairo_create(temp_surface);

    uint32_t total_width = 0;
    struct wsk_keypress *key = state->keys;

    // 전체 너비 계산
    while (key) {
        int w, h;
        const char *name = key->utf8[0] ? key->utf8 : key->name;
        get_text_size(temp_cairo, state->font, &w, &h, NULL, 1, "%s ", name);
        total_width += w;
        key = key->next;
    }

    printf("Total width: %d, max: %d\n", total_width, max_width);

    // 🔥 초과하면 맨 앞 키들 제거
    while (total_width > max_width && state->keys) {
        struct wsk_keypress *to_remove = state->keys;

        // 제거할 키의 너비 계산
        int w, h;
        const char *name = to_remove->utf8[0] ? to_remove->utf8 : to_remove->name;
        get_text_size(temp_cairo, state->font, &w, &h, NULL, 1, "%s ", name);

        // 키 제거
        state->keys = state->keys->next;
        free(to_remove);

        total_width -= w;
        printf("Removed key, new total width: %d\n", total_width);
    }

    cairo_destroy(temp_cairo);
    cairo_surface_destroy(temp_surface);
}


static cairo_subpixel_order_t to_cairo_subpixel_order(
		enum wl_output_subpixel subpixel) {
	switch (subpixel) {
	case WL_OUTPUT_SUBPIXEL_HORIZONTAL_RGB:
		return CAIRO_SUBPIXEL_ORDER_RGB;
	case WL_OUTPUT_SUBPIXEL_HORIZONTAL_BGR:
		return CAIRO_SUBPIXEL_ORDER_BGR;
	case WL_OUTPUT_SUBPIXEL_VERTICAL_RGB:
		return CAIRO_SUBPIXEL_ORDER_VRGB;
	case WL_OUTPUT_SUBPIXEL_VERTICAL_BGR:
		return CAIRO_SUBPIXEL_ORDER_VBGR;
	default:
		return CAIRO_SUBPIXEL_ORDER_DEFAULT;
	}
	return CAIRO_SUBPIXEL_ORDER_DEFAULT;
}

static void render_to_cairo(cairo_t *cairo, struct wsk_state *state,
		int scale, uint32_t *width, uint32_t *height) {
	cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_u32(cairo, state->background);
	cairo_paint(cairo);

	// 🔥 실제 화면 너비 사용
    uint32_t max_width = 800;  // 기본값
    if (state->output && state->output->width > 0) {
        max_width = state->output->width - 100;  // 화면 너비 - 마진
        fprintf(stdout, "Using screen width: %d\n", max_width);
    } else {
        fprintf(stdout, "Using default width: %d\n", max_width);
	}

	struct wsk_keypress *key = state->keys;
	while (key) {
		bool special = false;
		const char *name = key->utf8;
		if (!name[0]) {
			special = true;
			cairo_set_source_u32(cairo, state->specialfg);

			switch(key->sym) {
				case XKB_KEY_space:                             name = "⎵"; break;
				case XKB_KEY_Control_L: case XKB_KEY_Control_R: name = "^"; break;
				case XKB_KEY_Super_L:   case XKB_KEY_Super_R:   name = "⌘"; break;
				case XKB_KEY_Alt_L:     case XKB_KEY_Alt_R:     name = "⌥"; break;
				case XKB_KEY_Shift_L:   case XKB_KEY_Shift_R:   name = "⇧"; break;
				case XKB_KEY_Return:                            name = "⏎"; break;
				case XKB_KEY_BackSpace:                         name = "⌫"; break;
				case XKB_KEY_Delete:                            name = "⌦"; break;
				case XKB_KEY_Escape:                            name = "⎋"; break;
				case XKB_KEY_Up:                                name = "↑"; break;
				case XKB_KEY_Down:                              name = "↓"; break;
				case XKB_KEY_Left:                              name = "←"; break;
				case XKB_KEY_Right:                             name = "→"; break;
				case XKB_KEY_Next:                              name = "↡"; break;
				case XKB_KEY_Prior:                             name = "↟"; break;
				case XKB_KEY_Print:                             name = "⎙"; break;
				case XKB_KEY_Menu:                              name = "≡"; break;
				case XKB_KEY_Tab:                               name = "⇥"; break;
				case XKB_KEY_ISO_Left_Tab:                      name = "⇤"; break;
				case XKB_KEY_Caps_Lock:                         name = "⇪"; break;
				case XKB_KEY_Home:                              name = "⇱"; break;
				case XKB_KEY_End:                               name = "⇲"; break;
				default:                                        name = key->name;
			}
		} else {
			cairo_set_source_u32(cairo, state->foreground);
		}

		cairo_move_to(cairo, *width, 0);

		int w, h;
		if (special) {
		    if (key->count > 1) {
		        get_text_size(cairo, state->font, &w, &h, NULL, scale, "%s x%d ", name, key->count);
		        pango_printf(cairo, state->font, scale, "%s x%d ", name, key->count);
		    } else {
		        get_text_size(cairo, state->font, &w, &h, NULL, scale, "%s ", name);
		        pango_printf(cairo, state->font, scale, "%s ", name);
		    }
		} else {
			get_text_size(cairo, state->font, &w, &h, NULL, scale, "%s", name);
			pango_printf(cairo, state->font, scale, "%s", name);
		}

		*width = *width + w;
		if ((int)*height < h) {
			*height = h;
		}
		key = key->next;
	}
}

static void render_frame(struct wsk_state *state) {
	cairo_surface_t *recorder = cairo_recording_surface_create(
			CAIRO_CONTENT_COLOR_ALPHA, NULL);
	cairo_t *cairo = cairo_create(recorder);
	cairo_set_antialias(cairo, CAIRO_ANTIALIAS_BEST);
	cairo_font_options_t *fo = cairo_font_options_create();
	cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_FULL);
	cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_SUBPIXEL);
	if (state->output) {
		cairo_font_options_set_subpixel_order(
				fo, to_cairo_subpixel_order(state->output->subpixel));
	}
	cairo_set_font_options(cairo, fo);
	cairo_font_options_destroy(fo);
	cairo_save(cairo);
	cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cairo);
	cairo_restore(cairo);

	int scale = state->output ? state->output->scale : 1;
	uint32_t width = 0, height = 0;
	render_to_cairo(cairo, state, scale, &width, &height);
	if (height / scale != state->height
			|| width / scale != state->width
			|| state->width == 0) {
		// Reconfigure surface
		if (width == 0 || height == 0) {
			wl_surface_attach(state->surface, NULL, 0, 0);
		} else {
			zwlr_layer_surface_v1_set_size(
					state->layer_surface, width / scale, height / scale);
		}

		// TODO: this could infinite loop if the compositor assigns us a
		// different height than what we asked for
		wl_surface_commit(state->surface);
	} else if (height > 0) {
		// Replay recording into shm and send it off
		state->current_buffer = get_next_buffer(state->shm,
				state->buffers, state->width * scale, state->height * scale);
		if (!state->current_buffer) {
			cairo_surface_destroy(recorder);
			cairo_destroy(cairo);
			return;
		}
		cairo_t *shm = state->current_buffer->cairo;

		cairo_save(shm);
		cairo_set_operator(shm, CAIRO_OPERATOR_CLEAR);
		cairo_paint(shm);
		cairo_restore(shm);

		cairo_set_source_surface(shm, recorder, 0.0, 0.0);
		cairo_paint(shm);

		wl_surface_set_buffer_scale(state->surface, scale);
		wl_surface_attach(state->surface,
				state->current_buffer->buffer, 0, 0);
		wl_surface_damage_buffer(state->surface, 0, 0,
				state->width, state->height);
		wl_surface_commit(state->surface);
	}
}

static void set_dirty(struct wsk_state *state) {
	if (state->frame_scheduled) {
		state->dirty = true;
	} else if (state->surface) {
		render_frame(state);
	}
}

static void layer_surface_configure(void *data,
			struct zwlr_layer_surface_v1 *zwlr_layer_surface_v1,
			uint32_t serial, uint32_t width, uint32_t height) {
	struct wsk_state *state = data;
	state->width = width;
	state->height = height;
	zwlr_layer_surface_v1_ack_configure(zwlr_layer_surface_v1, serial);
	set_dirty(state);
}

static void layer_surface_closed(void *data,
		struct zwlr_layer_surface_v1 *zwlr_layer_surface_v1) {
	struct wsk_state *state = data;
	state->run = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

static void surface_enter(void *data,
		struct wl_surface *wl_surface, struct wl_output *output) {
	struct wsk_state *state = data;
	struct wsk_output *wsk_output = state->outputs;
	while (wsk_output->output != output) {
		wsk_output = wsk_output->next;
	}
	state->output = wsk_output;
}

static void surface_leave(void *data,
		struct wl_surface *wl_surface, struct wl_output *output) {
	// Who cares (not really possible with layer shell)
}

static const struct wl_surface_listener wl_surface_listener = {
	.enter = surface_enter,
	.leave = surface_leave,
};

static void keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t format, int32_t fd, uint32_t size) {
	struct wsk_state *state = data;

	// 🔥 크기 체크
    if (size == 0) {
        close(fd);
        fprintf(stderr, "Compositor sent empty keymap, using default\n");

        // 기본 keymap 생성
        struct xkb_keymap *keymap = xkb_keymap_new_from_names(
                state->xkb_context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (!keymap) {
            fprintf(stderr, "Failed to create default keymap\n");
            return;
        }

        struct xkb_state *xkb_state = xkb_state_new(keymap);
        if (!xkb_state) {
            xkb_keymap_unref(keymap);
            return;
        }

        xkb_keymap_unref(state->xkb_keymap);
        xkb_state_unref(state->xkb_state);
        state->xkb_keymap = keymap;
        state->xkb_state = xkb_state;
        return;
    }

	char *map_shm = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	if (map_shm == MAP_FAILED) {
		close(fd);
		fprintf(stderr, "Unable to mmap keymap\nsize: %u, code: '%s'", size, strerror(errno));
		return;
	}
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		munmap(map_shm, size);
		close(fd);
		return;
	}

	struct xkb_keymap *keymap = xkb_keymap_new_from_string(
			state->xkb_context, map_shm, XKB_KEYMAP_FORMAT_TEXT_V1,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map_shm, size);
	close(fd);

	struct xkb_state *xkb_state = xkb_state_new(keymap);
	xkb_keymap_unref(state->xkb_keymap);
	xkb_state_unref(state->xkb_state);
	state->xkb_keymap = keymap;
	state->xkb_state = xkb_state;
}

static void keyboard_enter(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
	// Who cares
}

static void keyboard_leave(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, struct wl_surface *surface) {
	// Who cares
}

static void keyboard_key(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
	// Who cares
}

static void keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched,
		uint32_t mods_locked, uint32_t group) {
	// Who cares
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
		int32_t rate, int32_t delay) {
	// TODO
}

static const struct wl_keyboard_listener wl_keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

static void seat_capabilities(
		void *data, struct wl_seat *wl_seat, uint32_t capabilities) {
	struct wsk_state *state = data;
	if (state->keyboard) {
		// TODO: support multiple seats
		return;
	}

	if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD)) {
		fprintf(stderr, "wl_seat does not support keyboard");
		state->run = false;
		return;
	}

	state->keyboard = wl_seat_get_keyboard(wl_seat);
	wl_keyboard_add_listener(state->keyboard, &wl_keyboard_listener, state);
}

static void seat_name(void *data, struct wl_seat *wl_seat, const char *name) {
	struct wsk_state *state = data;
	/* TODO: support multiple seats */
	if (libinput_udev_assign_seat(state->libinput, "seat0") != 0) {
		fprintf(stderr, "Failed to assign libinput seat\n");
		state->run = false;
		return;
	}
}

static const struct wl_seat_listener wl_seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static void output_geometry(void *data, struct wl_output *wl_output,
		int32_t x, int32_t y, int32_t physical_width, int32_t physical_height,
		int32_t subpixel, const char *make, const char *model,
		int32_t transform) {
	struct wsk_output *output = data;
	output->subpixel = subpixel;
}

static void output_mode(void *data, struct wl_output *wl_output,
		uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
	// 🔥 실제 화면 크기 저장
	struct wsk_output *output = data;
	output->width = width;
	output->heigh = height;
	fprintf(stdout, "Screen resolution: %dx%d\n", width, height);
}

static void output_done(void *data, struct wl_output *wl_output) {
	// Who cares
}

static void output_scale(void *data,
		struct wl_output *wl_output, int32_t factor) {
	struct wsk_output *output = data;
	output->scale = factor;
}

static const struct wl_output_listener wl_output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
};

static void registry_global(void *data, struct wl_registry *wl_registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct wsk_state *state = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		state->compositor = wl_registry_bind(wl_registry,
				name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		state->shm = wl_registry_bind(wl_registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		state->seat = wl_registry_bind(wl_registry,
				name, &wl_seat_interface, 5);
	} else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
		state->output_mgr = wl_registry_bind(wl_registry,
				name, &zxdg_output_manager_v1_interface, 1);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		state->layer_shell = wl_registry_bind(wl_registry,
				name, &zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct wsk_output *output = calloc(1, sizeof(struct wsk_output));
		output->output = wl_registry_bind(wl_registry,
				name, &wl_output_interface, 3);
		output->scale = 1; output->heigh = 0; output->width = 0;
		struct wsk_output **link = &state->outputs;
		while (*link) {
			link = &(*link)->next;
		}
		*link = output;
		wl_output_add_listener(output->output, &wl_output_listener, output);

		// 🔥 첫 번째 output을 기본으로 설정
    	if (!state->output) {
    	    state->output = output;
    	    printf("Set primary output\n");
    	}
	}
}

static void registry_global_remove(void *data,
		struct wl_registry *wl_registry, uint32_t name) {
	/* This space deliberately left blank */
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void handle_libinput_event(struct wsk_state *state,
		struct libinput_event *event) {
	if (!state->xkb_state) {
		return;
	}

	enum libinput_event_type event_type = libinput_event_get_type(event);
	if (event_type != LIBINPUT_EVENT_KEYBOARD_KEY) {
		return;
	}

	struct libinput_event_keyboard *kbevent =
		libinput_event_get_keyboard_event(event);

	uint32_t keycode = libinput_event_keyboard_get_key(kbevent) + 8;
	enum libinput_key_state key_state =
		libinput_event_keyboard_get_key_state(kbevent);
	xkb_state_update_key(state->xkb_state, keycode,
			key_state == LIBINPUT_KEY_STATE_RELEASED ?
				XKB_KEY_UP : XKB_KEY_DOWN);

	xkb_keysym_t keysym = xkb_state_key_get_one_sym(state->xkb_state, keycode);

	struct wsk_keypress *keypress;
	switch (key_state) {
	case LIBINPUT_KEY_STATE_RELEASED:
		/* Who cares */
		break;
	case LIBINPUT_KEY_STATE_PRESSED:
		if(keysym == XKB_KEY_Pause || keysym == XKB_KEY_Break) {
			state->run = false;
			return;
		}

    	// 현재 키가 마지막 키와 같은지 확인
    	struct wsk_keypress *last_key = NULL;
    	if (state->keys) {
    	    struct wsk_keypress *current = state->keys;
    	    while (current->next) {
    	        current = current->next;
    	    }
    	    last_key = current;
    	}

    	// UTF-8 문자 확인
    	unsigned char current_utf8[128] = {0};
    	bool is_special = false;
    	if (xkb_state_key_get_utf8(state->xkb_state, keycode,
    	        (char*)current_utf8, sizeof(current_utf8)) <= 0 ||
    	        current_utf8[0] <= ' ' ||
			    (current_utf8[0] >= 0x7F && current_utf8[0] <= 0x9F)
			) {
    	    current_utf8[0] = '\0';
    	    is_special = true;
    	}

    	// 🔥 특수 키만 카운트, 일반 키는 항상 새로 추가
    	bool should_count = false;
    	if (is_special && last_key && last_key->sym == keysym && last_key->utf8[0] == '\0') {
    	    should_count = true;  // 연속된 같은 특수 키만 카운트
    	}

    	if (should_count) {
    	    // 🔥 특수 키 카운트 증가
    	    last_key->count++;
    	} else {
    	    // 🔥 새로운 키 추가 (일반 키는 항상 여기로)
    	    keypress = calloc(1, sizeof(struct wsk_keypress));
    	    assert(keypress);
    	    keypress->sym = keysym;
    	    keypress->count = 1;

    	    xkb_keysym_get_name(keypress->sym, keypress->name,
    	            sizeof(keypress->name));
    	    strcpy(keypress->utf8, (char*)current_utf8);

    	    // 링크드 리스트 끝에 추가
    	    struct wsk_keypress **link = &state->keys;
    	    while (*link) {
    	        link = &(*link)->next;
    	    }
    	    *link = keypress;
    	}

		trim_keys_by_width(state);
    	break;
	}

	clock_gettime(CLOCK_MONOTONIC, &state->last_key);
	set_dirty(state);
}

static int libinput_open_restricted(const char *path,
		int flags, void *data) {
	int *fd = data;
	return devmgr_open(*fd, path);
}

static void libinput_close_restricted(int fd, void *data) {
	close(fd);
}

static const struct libinput_interface libinput_impl = {
	.open_restricted = libinput_open_restricted,
	.close_restricted = libinput_close_restricted,
};

static uint32_t parse_color(const char *color) {
	if (color[0] == '#') {
		++color;
	}

	int len = strlen(color);
	if (len != 6 && len != 8) {
		fprintf(stderr, "Invalid color %s, defaulting to color "
				"0xFFFFFFFF\n", color);
		return 0xFFFFFFFF;
	}
	uint32_t res = (uint32_t)strtoul(color, NULL, 16);
	if (strlen(color) == 6) {
		res = (res << 8) | 0xFF;
	}
	return res;
}

int main(int argc, char *argv[]) {
	// Fontconfig initializations
	if(!FcInit()) {
		fprintf(stderr, "Failed to initialize fontconfig\n");
		return 1;
	}

	/* NOTICE: This code runs as root */
	struct wsk_state state = { 0 };
	if (devmgr_start(&state.devmgr, &state.devmgr_pid, INPUTDEVPATH) > 0) {
		return 1;
	}

	fprintf(stdout, "Compositor: %s\n", getenv("WAYLAND_DISPLAY") ?: "wayland-0");
	fprintf(stdout, "Using compositor interfaces...\n");


	/* Begin normal user code: */
	int ret = 0;

	unsigned int anchor = 0;
	int margin = 32;
	state.background = 0x000000CC;
	state.specialfg = 0xAAAAAAFF;
	state.foreground = 0xFFFFFFFF;
	state.font = "monospace 24";
	state.timeout = 1;

	int c;
	while ((c = getopt(argc, argv, "hb:f:s:F:t:a:m:o:")) != -1) {
		switch (c) {
		case 'b':
			state.background = parse_color(optarg);
			break;
		case 'f':
			state.foreground = parse_color(optarg);
			break;
		case 's':
			state.specialfg = parse_color(optarg);
			break;
		case 'F':
			state.font = optarg;
			break;
		case 't':
			state.timeout = atoi(optarg);
			break;
		case 'a':
			if (strcmp(optarg, "top") == 0) {
				anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
			} else if (strcmp(optarg, "left") == 0) {
				anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
			} else if (strcmp(optarg, "right") == 0) {
				anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
			} else if (strcmp(optarg, "bottom") == 0) {
				anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
			}
			break;
		case 'm':
			margin = atoi(optarg);
			break;
		case 'o':
			fprintf(stderr, "-o is unimplemented\n");
			return 0;
		default:
			fprintf(stderr, "usage: wshowkeys [-b|-f|-s #RRGGBB[AA]] [-F font] "
					"[-t timeout]\n\t[-a top|left|right|bottom] [-m margin] "
					"[-o output]\n");
			return 1;
		}
	}

	state.udev = udev_new();
	if (!state.udev) {
		fprintf(stderr, "udev_create: %s\n", strerror(errno));
		ret = 1;
		goto exit;
	}

	state.libinput = libinput_udev_create_context(
			&libinput_impl, &state.devmgr, state.udev);
	udev_unref(state.udev);
	if (!state.libinput) {
		fprintf(stderr, "libinput_udev_create_context: %s\n", strerror(errno));
		ret = 1;
		goto exit;
	}

	state.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!state.xkb_context) {
		fprintf(stderr, "xkb_context_new: %s\n", strerror(errno));
		ret = 1;
		goto exit;
	}
	fprintf(stdout, "XKB context created successfully\n");

	state.display = wl_display_connect(NULL);
	if (!state.display) {
		fprintf(stderr, "wl_display_connect: %s\n", strerror(errno));
		ret = 1;
		goto exit;
	}

	state.registry = wl_display_get_registry(state.display);
	assert(state.registry);
	wl_registry_add_listener(state.registry, &registry_listener, &state);
	wl_display_roundtrip(state.display);

	struct {
		const char *name;
		void *ptr;
	} need_globals[] = {
		"wl_compositor", &state.compositor,
		"wl_shm", &state.shm,
		"wl_seat", &state.seat,
		"wlr_layer_shell", &state.layer_shell,
	};
	for (size_t i = 0; i < sizeof(need_globals) / sizeof(need_globals[0]); ++i) {
		if (!need_globals[i].ptr) {
			fprintf(stderr, "Error: required Wayland interface '%s' "
					"is not present\n", need_globals[i].name);
			ret = 1;
			goto exit;
		}
	}

	// TODO: Listener for xdg output

	wl_seat_add_listener(state.seat, &wl_seat_listener, &state);
	wl_display_roundtrip(state.display);
	
	state.surface = wl_compositor_create_surface(state.compositor);
	assert(state.surface);

	state.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
			state.layer_shell, state.surface, NULL,
			ZWLR_LAYER_SHELL_V1_LAYER_TOP, "showkeys");
	assert(state.layer_surface);

	wl_surface_add_listener(state.surface, &wl_surface_listener, &state);
	zwlr_layer_surface_v1_add_listener(
			state.layer_surface, &layer_surface_listener, &state);
	zwlr_layer_surface_v1_set_size(state.layer_surface, 1, 1);
	zwlr_layer_surface_v1_set_anchor(state.layer_surface, anchor);
	zwlr_layer_surface_v1_set_margin(state.layer_surface,
			margin, margin, margin, margin);
	zwlr_layer_surface_v1_set_exclusive_zone(state.layer_surface, -1);
	wl_surface_commit(state.surface);

	// Configure 이벤트 대기
    int retry_count = 0;
    while ((state.width == 0 || state.height == 0) && retry_count < 10) {
		wl_display_roundtrip(state.display);
		retry_count++;
	}

	retry_count = 0;
    while ((state.width == 0 || state.height == 0) && retry_count < 10) {
        wl_display_dispatch(state.display);
        retry_count++;
    }
    
    if (state.width == 0 || state.height == 0) {
        fprintf(stderr, "Layer surface configuration failed\n");
        ret = 1;
        goto exit;
    }

	struct pollfd pollfds[] = {
		{ .fd = libinput_get_fd(state.libinput), .events = POLLIN, },
		{ .fd = wl_display_get_fd(state.display), .events = POLLIN, },
	};

	state.run = true;
	while (state.run) {
		errno = 0;
		do {
			if (wl_display_flush(state.display) == -1 && errno != EAGAIN) {
				fprintf(stderr, "wl_display_flush: %s\n", strerror(errno));
				break;
			}
		} while (errno == EAGAIN);

		int timeout = -1;
		if (state.keys) {
			timeout = 100;
		}

		if (poll(pollfds, sizeof(pollfds) / sizeof(pollfds[0]), timeout) < 0) {
			fprintf(stderr, "poll: %s\n", strerror(errno));
			break;
		}

		/* Clear out old keys */
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec >= state.last_key.tv_sec + state.timeout &&
				now.tv_nsec >= state.last_key.tv_nsec) {
			struct wsk_keypress *key = state.keys;
			while (key) {
				struct wsk_keypress *next = key->next;
				free(key);
				key = next;
			}
			state.keys = NULL;
			set_dirty(&state);
		}

		if ((pollfds[0].revents & POLLIN)) {
			if (libinput_dispatch(state.libinput) != 0) {
				fprintf(stderr, "libinput_dispatch: %s\n", strerror(errno));
				break;
			}
			struct libinput_event *event;
			while ((event = libinput_get_event(state.libinput))) {
				handle_libinput_event(&state, event);
				libinput_event_destroy(event);
			}
		}

		if ((pollfds[1].revents & POLLIN)
				&& wl_display_dispatch(state.display) == -1) {
			fprintf(stderr, "wl_display_dispatch: %s\n", strerror(errno));
			break;
		}
	}

exit:
	FcInit();
	wl_display_disconnect(state.display);
	libinput_unref(state.libinput);
	devmgr_finish(state.devmgr, state.devmgr_pid);
	return ret;
}
