#ifndef _WSK_MAIN_H
#define _WSK_MAIN_H

/* System headers */
#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* External library headers */
#include <cairo/cairo.h>
#include <fontconfig/fontconfig.h>
#include <libinput.h>
#include <libudev.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

/* Protocol headers */
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

/* Project headers */
#include "devmgr.h"
#include "pango.h"
#include "shm.h"

/* Constants */
#ifndef INPUTDEVPATH
    #define INPUTDEVPATH "/dev/input"
#endif

/* Forward declarations */
struct wsk_keypress;
struct wsk_output;
struct wsk_state;

/* Structure definitions */
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

/* Function prototypes */
static void cairo_set_source_u32(cairo_t *cairo, uint32_t color);
static void trim_keys_by_width(struct wsk_state *state);
static cairo_subpixel_order_t to_cairo_subpixel_order(enum wl_output_subpixel subpixel);
static void render_to_cairo(cairo_t *cairo, struct wsk_state *state,
        int scale, uint32_t *width, uint32_t *height);
static void render_frame(struct wsk_state *state);
static void set_dirty(struct wsk_state *state);

/* Wayland listener callbacks */
static void layer_surface_configure(void *data,
        struct zwlr_layer_surface_v1 *zwlr_layer_surface_v1,
        uint32_t serial, uint32_t width, uint32_t height);
static void layer_surface_closed(void *data,
        struct zwlr_layer_surface_v1 *zwlr_layer_surface_v1);
static void surface_enter(void *data,
        struct wl_surface *wl_surface, struct wl_output *output);
static void surface_leave(void *data,
        struct wl_surface *wl_surface, struct wl_output *output);

/* Keyboard event callbacks */
static void keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
        uint32_t format, int32_t fd, uint32_t size);
static void keyboard_enter(void *data, struct wl_keyboard *wl_keyboard,
        uint32_t serial, struct wl_surface *surface, struct wl_array *keys);
static void keyboard_leave(void *data, struct wl_keyboard *wl_keyboard,
        uint32_t serial, struct wl_surface *surface);
static void keyboard_key(void *data, struct wl_keyboard *wl_keyboard,
        uint32_t serial, uint32_t time, uint32_t key, uint32_t state);
static void keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard,
        uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched,
        uint32_t mods_locked, uint32_t group);
static void keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
        int32_t rate, int32_t delay);

/* Seat event callbacks */
static void seat_capabilities(void *data, struct wl_seat *wl_seat, uint32_t capabilities);
static void seat_name(void *data, struct wl_seat *wl_seat, const char *name);

/* Output event callbacks */
static void output_geometry(void *data, struct wl_output *wl_output,
        int32_t x, int32_t y, int32_t physical_width, int32_t physical_height,
        int32_t subpixel, const char *make, const char *model,
        int32_t transform);
static void output_mode(void *data, struct wl_output *wl_output,
        uint32_t flags, int32_t width, int32_t height, int32_t refresh);
static void output_done(void *data, struct wl_output *wl_output);
static void output_scale(void *data, struct wl_output *wl_output, int32_t factor);

/* Registry event callbacks */
static void registry_global(void *data, struct wl_registry *wl_registry,
        uint32_t name, const char *interface, uint32_t version);
static void registry_global_remove(void *data,
        struct wl_registry *wl_registry, uint32_t name);

/* Input event handling */
static void handle_libinput_event(struct wsk_state *state,
        struct libinput_event *event);

/* libinput interface callbacks */
static int libinput_open_restricted(const char *path, int flags, void *data);
static void libinput_close_restricted(int fd, void *data);

/* Utility functions */
static uint32_t parse_color(const char *color);

/* Main function */
int main(int argc, char *argv[]);

#endif /* _WSK_MAIN_H */
