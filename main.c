/* xwx: window manager based on river */
/* SPDX-License-Identifier: CC0-1.0 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>

#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <river-window-management-v1-client-protocol.h>
#include <river-xkb-bindings-v1-client-protocol.h>
#include <river-layer-shell-v1-client-protocol.h>

#include <cursor-shape-v1-client-protocol.h>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#define XCURSOR_THEME "Simp1e-Adw-Dark"
#define XCURSOR_SIZE  24

#define COLOR_BORDER_ACTIVE_R   85
#define COLOR_BORDER_ACTIVE_G   170
#define COLOR_BORDER_ACTIVE_B   170

#define COLOR_BORDER_INACTIVE_R 158
#define COLOR_BORDER_INACTIVE_G 238
#define COLOR_BORDER_INACTIVE_B 238

#define STRINGIZE_IMPL(x) #x
#define STRINGIZE(x) STRINGIZE_IMPL(x)

#define MOD_SUPER RIVER_SEAT_V1_MODIFIERS_MOD4
#define MOD_CTRL  RIVER_SEAT_V1_MODIFIERS_CTRL
#define MOD_ALT   RIVER_SEAT_V1_MODIFIERS_MOD1

#define LOG_MOD "xwx"
#include "log.h"

struct dimensions {
  int32_t x, y;
  int32_t width, height;
};

struct dimensions_hint {
  int32_t min_width, max_width;
  int32_t min_height, max_height;
};

struct wl_output_reg {
  uint32_t name;
  struct wl_output *wl_output;
  struct wl_list link; // wm.wl_outputs
};

struct output {
  struct river_output_v1 *obj;
  struct wl_output *wl_output;
  struct river_layer_shell_output_v1 *layer_shell_output;
  struct wm *wm;
  bool removed;

  struct dimensions dimensions;
  struct dimensions available;

  struct wl_list link; // wm.outputs
};

/* lifecycle */
enum window_states {
  WINDOW_STATE_NEW,
  WINDOW_STATE_AWAIT,
  WINDOW_STATE_READY,
  WINDOW_STATE_CLOSED,
};

enum window_pending {
  WINDOW_PENDING_FOCUS        = 1 << 0,
  WINDOW_PENDING_MAXIMIZED    = 1 << 1,
  WINDOW_PENDING_UNMAXIMIZED  = 1 << 2,
  WINDOW_PENDING_FULLSCREEN   = 1 << 3,
  WINDOW_PENDING_UNFULLSCREEN = 1 << 4,
};

enum window_modes {
  WINDOW_MODE_DEFAULT    = 1 << 0,
  WINDOW_MODE_MAXIMIZED  = 1 << 1,
  WINDOW_MODE_FULLSCREEN = 1 << 2,
};

enum window_border_state {
  WINDOW_BORDER_STATE_NONE,
  WINDOW_BORDER_STATE_INACTIVE,
  WINDOW_BORDER_STATE_ACTIVE,
};

struct seat;
struct window {
  struct river_window_v1 *obj;
  struct river_node_v1 *node;
  struct window *parent;
  struct wm *wm;

  enum window_states state;
  uint32_t pending_mode;    // WINDOW_PENDING_*
  uint32_t mode;            // WINDOW_MODE_*

  struct dimensions dimensions;
  struct dimensions dimensions_checkpoint;
  bool positioned; /* dimensions.x/y are known to have been applied */

  struct dimensions_hint dimensions_hint;
  int32_t decoration_hint; /* -1 for not delivered */

  /* Borders persist across render sequences; only send set_borders on change. */
  enum window_border_state border_state;

  const char *app_id;
  const char *title;

  struct seat *pointer_move_requested;
  struct seat *pointer_resize_requested;
  uint32_t pointer_resize_requested_edges;

  struct wl_list link; // wm.windows
};

enum action {
  ACTION_NONE,
  ACTION_SPAWN_FOOT,
  ACTION_SPAWN_MENU,
  ACTION_SWITCH_IM,
  ACTION_CLOSE,
  ACTION_FOCUS_NEXT,
  ACTION_MOVE,
  ACTION_RESIZE,
  ACTION_TOGGLE_FULLSCREEN,
  ACTION_TOGGLE_MAXIMIZE,
  ACTION_EXIT,
};

struct xkb_binding {
  struct river_xkb_binding_v1 *obj;
  struct seat *seat;
  enum action action;
  enum action action_released;
  struct wl_list link;
};

struct pointer_binding {
  struct river_pointer_binding_v1 *obj;
  struct seat *seat;
  enum action action;
  struct wl_list link;
};

enum seat_ops {
  SEAT_OP_NONE,
  SEAT_OP_MOVE,
  SEAT_OP_RESIZE,
};

enum layer_shell_seat_focus {
  LAYER_SHELL_SEAT_FOCUS_NONE,
  LAYER_SHELL_SEAT_FOCUS_EXCLUSIVE,
  LAYER_SHELL_SEAT_FOCUS_NON_EXCLUSIVE,
};

struct wl_seat_reg {
  uint32_t name;
  struct wm *wm;
  struct seat *seat;
  struct wl_seat *wl_seat;
  uint32_t capabilities;
  struct wl_list link; // wm.wl_seats
};

struct seat {
  struct river_seat_v1 *obj;
  struct wl_seat *wl_seat;
  struct wm *wm;
  bool new;
  bool removed;

  struct river_layer_shell_seat_v1 *layer_shell_seat;
  enum layer_shell_seat_focus layer_shell_seat_focus;
  bool layer_shell_seat_focus_pending_none;

  struct window *focused;
  struct window *hovered;
  struct window *interacted;

  struct wl_list xkb_bindings;     // struct xkb_binding
  struct wl_list pointer_bindings; // struct pointer_binding
  enum action    pending_action;

  enum seat_ops op;
  struct window *op_window;
  int32_t  op_start_x, op_start_y;
  int32_t  op_dx, op_dy;
  bool     op_release;
  int32_t  op_start_width, op_start_height;
  uint32_t op_edges;

  struct wp_cursor_shape_device_v1 *cursor_shape_dev;
  struct wl_pointer *wl_pointer;
  int32_t pointer_x;
  int32_t pointer_y;
  bool pointer_positioned;

  struct wl_list link; // wm.seats
};

struct wm {
  struct river_window_manager_v1 *rwm;
  struct river_xkb_bindings_v1   *xkb_bindings;
  struct river_layer_shell_v1    *layer_shell;
  struct wp_cursor_shape_manager_v1 *cursor_shape_mgr;

  struct wl_list outputs;
  struct wl_list wl_outputs;
  struct wl_list wl_seats;
  struct wl_list windows;
  struct wl_list seats;
};

///////////////////////////////////////////////////////////////////////////////
// sec:output
///////////////////////////////////////////////////////////////////////////////

static inline void output_maybe_destroy(struct output *output) {
  if (!output->removed) return;
  river_output_v1_destroy(output->obj);
  if (output->layer_shell_output)
    river_layer_shell_output_v1_destroy(output->layer_shell_output);
  wl_list_remove(&output->link);
  free(output);
}

static inline
struct wl_output *find_wl_output_by_name(struct wm *wm, uint32_t name) {
  struct wl_output_reg *reg;
  wl_list_for_each (reg, &wm->wl_outputs, link)
    if (reg->name == name) return reg->wl_output;
  return NULL;
}

static void output_handle_removed(void *data, struct river_output_v1 *obj) {
  struct output *output = data;
  output->removed = true;
}

static void output_handle_wl_output(
  void *data, struct river_output_v1 *obj,
  uint32_t name
) {
  struct output *output = data;
  output->wl_output = find_wl_output_by_name(output->wm, name);

  if (output->wl_output)
    LOG_INFO("associated river_output with wl_output name %u\n", name);
  else
    LOG_ERRO("couldn't find wl_output for name %u\n", name);
}

static void output_handle_position(
  void *data, struct river_output_v1 *obj,
  int32_t x, int32_t y
) {
  struct output *output = data;
  output->dimensions.x = x;
  output->dimensions.y = y;
}

static void output_handle_dimensions(
  void *data, struct river_output_v1 *obj,
  int32_t width, int32_t height
) {
  struct output *output = data;
  output->dimensions.width = width;
  output->dimensions.height = height;
  if (output->available.width <= 0)
    output->available = output->dimensions;
}

const struct river_output_v1_listener river_output_listener = {
  .removed = output_handle_removed,
  .wl_output = output_handle_wl_output,
  .position = output_handle_position,
  .dimensions = output_handle_dimensions,
};

static void layer_shell_output_non_exclusive_area(
  void *data, struct river_layer_shell_output_v1 *obj,
  int32_t x, int32_t y, int32_t width, int32_t height
) {
  struct output *output = data;
  output->available = (struct dimensions) { x, y, width, height };
}

static const struct river_layer_shell_output_v1_listener
river_layer_shell_output_listener = {
  .non_exclusive_area = layer_shell_output_non_exclusive_area,
};

static void wl_output_handle_geometry(
  void *data, struct wl_output *wl_output,
  int32_t x, int32_t y,
  int32_t physical_width, int32_t physical_height,
  int32_t subpixel, const char *make, const char *model,
  int32_t transform
) {}

static void wl_output_handle_mode(
  void *data, struct wl_output *wl_output,
  uint32_t flags, int32_t width, int32_t height, int32_t refresh
) {}

static const struct wl_output_listener wl_output_listener = {
  .geometry = wl_output_handle_geometry,
  .mode     = wl_output_handle_mode,
};

///////////////////////////////////////////////////////////////////////////////
// sec:window
///////////////////////////////////////////////////////////////////////////////

static void window_handle_closed(void *data, struct river_window_v1 *obj) {
  struct window *window = data;
  window->state = WINDOW_STATE_CLOSED;
}

static void window_handle_dimensions_hint(
  void *data, struct river_window_v1 *obj,
  int32_t min_width, int32_t min_height,
  int32_t max_width, int32_t max_height
) {
  struct window *window = data;
  window->dimensions_hint.min_width = min_width;
  window->dimensions_hint.max_width = max_width;
  window->dimensions_hint.min_height = min_height;
  window->dimensions_hint.max_height = max_height;
}

static void window_handle_dimensions(
  void *data, struct river_window_v1 *obj,
  int32_t width, int32_t height
) {
  struct window *window = data;
  window->dimensions.width = width;
  window->dimensions.height = height;
}

static void window_handle_app_id(
  void *data, struct river_window_v1 *obj,
  const char *app_id
) {
  struct window *window = data;
  free((void *) window->app_id);
  window->app_id = app_id ? strdup(app_id) : NULL;
}

static void window_handle_title(
  void *data, struct river_window_v1 *obj,
  const char *title
) {
  struct window *window = data;
  free((void *) window->title);
  window->title = title ? strdup(title) : NULL;
}

static void window_handle_parent(
  void *data, struct river_window_v1 *obj,
  struct river_window_v1 *parent
) {
  struct window *window = data;
  window->parent = parent ? river_window_v1_get_user_data(parent) : NULL;
}

static void window_handle_decoration_hint(
  void *data, struct river_window_v1 *obj,
  uint32_t hint
) {
  struct window *window = data;
  window->decoration_hint = hint;
}

static void window_handle_pointer_move_requested(
  void *data, struct river_window_v1 *obj,
  struct river_seat_v1 *river_seat
) {
  struct window *window = data;
  window->pointer_move_requested = river_seat_v1_get_user_data(river_seat);
}

static void window_handle_pointer_resize_requested(
  void *data, struct river_window_v1 *obj,
  struct river_seat_v1 *river_seat, uint32_t edges
) {
  struct window *window = data;
  window->pointer_resize_requested = river_seat_v1_get_user_data(river_seat);
  window->pointer_resize_requested_edges = edges;
}

static void window_handle_maximize_requested(
  void *data, struct river_window_v1 *obj
) {
  struct window *window = data;
  window->pending_mode |= WINDOW_PENDING_MAXIMIZED;
}

static void window_handle_unmaximize_requested(
  void *data, struct river_window_v1 *obj
) {
  struct window *window = data;
  window->pending_mode |= WINDOW_PENDING_UNMAXIMIZED;
}

static void window_handle_fullscreen_requested(
  void *data, struct river_window_v1 *obj,
  struct river_output_v1 *river_output
) {
  struct window *window = data;
  window->pending_mode |= WINDOW_PENDING_FULLSCREEN;
}
static void window_handle_exit_fullscreen_requested(
  void *data, struct river_window_v1 *obj
) {
  struct window *window = data;
  window->pending_mode |= WINDOW_PENDING_UNFULLSCREEN;
}

/* ignored */
static void window_handle_show_window_menu_requested(void *data, struct river_window_v1 *obj, int32_t x, int32_t y) {}
static void window_handle_minimize_requested(void *data, struct river_window_v1 *obj) {}
static void window_handle_unreliable_pid(void *data, struct river_window_v1 *obj, int32_t unreliable_pid) {}
static void window_handle_presentation_hint(void *data, struct river_window_v1 *obj, uint32_t hint) {}
static void window_handle_identifier(void *data, struct river_window_v1 *obj, const char *identifier) {}

const struct river_window_v1_listener river_window_listener = {
  .closed = window_handle_closed,
  .dimensions_hint = window_handle_dimensions_hint,
  .dimensions = window_handle_dimensions,
  .app_id = window_handle_app_id,
  .title = window_handle_title,
  .parent = window_handle_parent,
  .decoration_hint = window_handle_decoration_hint,
  .pointer_move_requested = window_handle_pointer_move_requested,
  .pointer_resize_requested = window_handle_pointer_resize_requested,
  .show_window_menu_requested = window_handle_show_window_menu_requested,
  .maximize_requested = window_handle_maximize_requested,
  .unmaximize_requested = window_handle_unmaximize_requested,
  .fullscreen_requested = window_handle_fullscreen_requested,
  .exit_fullscreen_requested = window_handle_exit_fullscreen_requested,
  .minimize_requested = window_handle_minimize_requested,
  .unreliable_pid = window_handle_unreliable_pid,
  .presentation_hint = window_handle_presentation_hint,
  .identifier = window_handle_identifier,
};

static void seat_set_cursor_shape(struct seat *seat, uint32_t shape);

static void window_maybe_destroy(struct window *window) {
  if (window->state != WINDOW_STATE_CLOSED) return;

  struct seat *seat;
  wl_list_for_each (seat, &window->wm->seats, link) {
    if (seat->focused == window)
      seat->focused = NULL;
    if (seat->op_window == window) {
      /* The seat is still live, so end the operation on it. The window is
         already inert after its closed event (river ignores any request other
         than destroy), so a resize notification would never reach it. */
      river_seat_v1_op_end(seat->obj);
      seat_set_cursor_shape(seat, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
      seat->op = SEAT_OP_NONE;
      seat->op_window = NULL;
    }
    if (seat->hovered == window) seat->hovered = NULL;
    if (seat->interacted == window) seat->interacted = NULL;
  }

  struct window *other;
  wl_list_for_each (other, &window->wm->windows, link)
    if (other->parent == window)
      other->parent = NULL;

  river_window_v1_destroy(window->obj);
  wl_list_remove(&window->link);
  free((void *) window->app_id);
  free((void *) window->title);
  LOG_DEBG("window %p destroyed and freed", (void *) window);
  free(window);
}

static void window_set_position(struct window *window, int32_t x, int32_t y) {
  /* Rendering state stays applied until the next set_position request, and this
     runs for every render sequence while a window is moved or resized; resend
     the position only when it actually changed. */
  if (window->positioned &&
      window->dimensions.x == x && window->dimensions.y == y)
    return;

  river_node_v1_set_position(window->node, x, y);
  window->dimensions.x = x;
  window->dimensions.y = y;
  window->positioned = true;
}

static struct output *output_get_active(struct wm *wm) {
  struct seat *seat;
  struct output *output;
  wl_list_for_each (seat, &wm->seats, link) {
    if (!seat->pointer_positioned) continue;
    wl_list_for_each (output, &wm->outputs, link) {
      if (seat->pointer_x >= output->dimensions.x &&
          seat->pointer_x < output->dimensions.x + output->dimensions.width &&
          seat->pointer_y >= output->dimensions.y &&
          seat->pointer_y < output->dimensions.y + output->dimensions.height)
      {
        return output;
      }
    }
  }

  if (wl_list_empty(&wm->outputs)) return NULL;
  return wl_container_of(wm->outputs.next, output, link);
}

static bool window_is_focused(struct wm *wm, struct window *window) {
  struct seat *seat;
  wl_list_for_each (seat, &wm->seats, link)
    if (seat->focused == window) return true;
  return false;
}

static bool window_wants_ssd(const struct window *window) {
  if (window->mode & WINDOW_MODE_FULLSCREEN) return false;
  switch (window->decoration_hint) {
    case RIVER_WINDOW_V1_DECORATION_HINT_ONLY_SUPPORTS_CSD:
    case RIVER_WINDOW_V1_DECORATION_HINT_PREFERS_CSD:
      return false;
    case RIVER_WINDOW_V1_DECORATION_HINT_PREFERS_SSD:
    case RIVER_WINDOW_V1_DECORATION_HINT_NO_PREFERENCE:
      return true;
    default:
      return false;
  }
}

static void window_set_borders(struct window *window) {
  enum window_border_state state;
  if (!window_wants_ssd(window))
    state = WINDOW_BORDER_STATE_NONE;
  else if (window_is_focused(window->wm, window))
    state = WINDOW_BORDER_STATE_ACTIVE;
  else
    state = WINDOW_BORDER_STATE_INACTIVE;

  /* This runs in every render sequence, which river starts whenever rendering
     state may change, e.g. on every pointer motion while a window is moved or
     resized. Borders stay configured until the next set_borders request, so
     only send it when the state actually changed; otherwise the borders are
     re-applied (and redrawn) once per frame. */
  if (state == window->border_state) return;
  window->border_state = state;

#define U32(x) ((uint32_t)(x) * 0x01010101u)

  switch (state) {
  case WINDOW_BORDER_STATE_NONE:
    river_window_v1_set_borders(window->obj,
        RIVER_WINDOW_V1_EDGES_NONE, 0, 0, 0, 0, 0);
    break;
  case WINDOW_BORDER_STATE_INACTIVE:
    river_window_v1_set_borders(window->obj,
        RIVER_WINDOW_V1_EDGES_TOP | RIVER_WINDOW_V1_EDGES_BOTTOM |
        RIVER_WINDOW_V1_EDGES_LEFT | RIVER_WINDOW_V1_EDGES_RIGHT,
        4,
        U32(COLOR_BORDER_INACTIVE_R),
        U32(COLOR_BORDER_INACTIVE_G),
        U32(COLOR_BORDER_INACTIVE_B),
        0xFFFFFFFF);
    break;
  case WINDOW_BORDER_STATE_ACTIVE:
    river_window_v1_set_borders(window->obj,
        RIVER_WINDOW_V1_EDGES_TOP | RIVER_WINDOW_V1_EDGES_BOTTOM |
        RIVER_WINDOW_V1_EDGES_LEFT | RIVER_WINDOW_V1_EDGES_RIGHT,
        4,
        U32(COLOR_BORDER_ACTIVE_R),
        U32(COLOR_BORDER_ACTIVE_G),
        U32(COLOR_BORDER_ACTIVE_B),
        0xFFFFFFFF);
    break;
  }

#undef U32
}

static void window_render(struct wm *wm, struct window *window) {
  if (window->state == WINDOW_STATE_AWAIT) {
    if (window->dimensions.width <= 0 || window->dimensions.height <= 0) return;

    int32_t x, y;

    if (window->parent && window->parent->state == WINDOW_STATE_READY &&
      window->parent->dimensions.width > 0 && window->parent->dimensions.height > 0) {
      x = window->parent->dimensions.x + (window->parent->dimensions.width - window->dimensions.width) / 2;
      y = window->parent->dimensions.y + (window->parent->dimensions.height - window->dimensions.height) / 2;
    } else {
      struct output *output = output_get_active(wm);
      if (!output) {
        x = 0;
        y = 0;
      } else {
        x = output->dimensions.x + (output->dimensions.width - window->dimensions.width) / 2;
        y = output->dimensions.y + (output->dimensions.height - window->dimensions.height) / 2;
      }
    }

    window_set_position(window, x, y);
    window->state = WINDOW_STATE_READY;
  }

  if (window->state != WINDOW_STATE_READY) return;
  window_set_borders(window);
}

static void window_apply_mode(struct window *window, uint32_t mode, struct output *output)
{
  uint32_t checkpoint = window->mode;
  if (checkpoint == mode) return;

  if (checkpoint == WINDOW_MODE_DEFAULT)
    window->dimensions_checkpoint = window->dimensions;

  bool was_full = checkpoint & WINDOW_MODE_FULLSCREEN;
  bool is_full  = mode & WINDOW_MODE_FULLSCREEN;
  bool was_max  = checkpoint & WINDOW_MODE_MAXIMIZED;
  bool is_max   = mode & WINDOW_MODE_MAXIMIZED;

  if (was_full != is_full) {
    if (is_full) {
      river_window_v1_fullscreen(window->obj, output->obj);
      river_window_v1_inform_fullscreen(window->obj);
    } else {
      river_window_v1_exit_fullscreen(window->obj);
      river_window_v1_inform_not_fullscreen(window->obj);
    }
  }

  if (was_max != is_max) {
    if (is_max) river_window_v1_inform_maximized(window->obj);
    else        river_window_v1_inform_unmaximized(window->obj);
  }

  if (is_full) {
    /* compositor owns size and position */
  } else if (is_max) {
    window_set_position(window, output->available.x, output->available.y);
    river_window_v1_propose_dimensions(window->obj,
        output->available.width, output->available.height);
  } else {
    window_set_position(window, window->dimensions_checkpoint.x,
                                window->dimensions_checkpoint.y);
    river_window_v1_propose_dimensions(window->obj,
        window->dimensions_checkpoint.width,
        window->dimensions_checkpoint.height);
  }

  window->mode = mode;
}

static void seat_pointer_move(struct seat *seat, struct window *window);
static void seat_pointer_resize(struct seat *seat, struct window *window, uint32_t edges);

static void window_apply_decoration(struct window *window) {
  if (window->decoration_hint < 0) return;

  if (window_wants_ssd(window))
    river_window_v1_use_ssd(window->obj);
  else
    river_window_v1_use_csd(window->obj);
}

static void window_manage(struct window *window) {
  if (window->state == WINDOW_STATE_NEW) {
    window_apply_decoration(window);
    river_window_v1_propose_dimensions(window->obj, 0, 0);
    river_window_v1_set_capabilities(window->obj,
      RIVER_WINDOW_V1_CAPABILITIES_MAXIMIZE |
      RIVER_WINDOW_V1_CAPABILITIES_FULLSCREEN);
    window->state = WINDOW_STATE_AWAIT;
    window->pending_mode |= WINDOW_PENDING_FOCUS;
  }

  if (window->state == WINDOW_STATE_READY) {
    struct output *output = output_get_active(window->wm);
    if (output) {
      uint32_t mode = window->mode;
      if (window->pending_mode & WINDOW_PENDING_MAXIMIZED)    mode |=  WINDOW_MODE_MAXIMIZED;
      if (window->pending_mode & WINDOW_PENDING_UNMAXIMIZED)  mode &= ~WINDOW_MODE_MAXIMIZED;
      if (window->pending_mode & WINDOW_PENDING_FULLSCREEN)   mode |=  WINDOW_MODE_FULLSCREEN;
      if (window->pending_mode & WINDOW_PENDING_UNFULLSCREEN) mode &= ~WINDOW_MODE_FULLSCREEN;

      window_apply_mode(window, mode, output);

      window->pending_mode &= ~(WINDOW_PENDING_MAXIMIZED | WINDOW_PENDING_UNMAXIMIZED |
                                WINDOW_PENDING_FULLSCREEN | WINDOW_PENDING_UNFULLSCREEN);
    }
  }

  if (window->pointer_move_requested && window->state == WINDOW_STATE_READY) {
    seat_pointer_move(window->pointer_move_requested, window);
    window->pointer_move_requested = NULL;
  }
  if (window->pointer_resize_requested && window->state == WINDOW_STATE_READY) {
    seat_pointer_resize(window->pointer_resize_requested, window, window->pointer_resize_requested_edges);
    window->pointer_resize_requested = NULL;
  }
}

static void window_toggle_pending_mode(struct window *window, uint32_t on, uint32_t off, uint32_t mode) {
  bool active;
  if (window->pending_mode & on)
    active = true;
  else if (window->pending_mode & off)
    active = false;
  else
    active = (window->mode & mode) != 0;

  window->pending_mode &= ~(on | off);
  window->pending_mode |= active ? off : on;
}

///////////////////////////////////////////////////////////////////////////////
// sec:xkb_binding
///////////////////////////////////////////////////////////////////////////////

static void xkb_binding_handle_pressed(
  void *data, struct river_xkb_binding_v1 *obj
) {
  struct xkb_binding *binding = data;
  binding->seat->pending_action = binding->action;
}

static void xkb_binding_handle_released(void *data, struct river_xkb_binding_v1 *obj) {
  struct xkb_binding *binding = data;
  if (binding->action_released == ACTION_NONE) return;
  binding->seat->pending_action = binding->action_released;
}

const struct river_xkb_binding_v1_listener river_xkb_binding_listener = {
  .pressed = xkb_binding_handle_pressed,
  .released = xkb_binding_handle_released,
};

static void xkb_binding_destroy(struct xkb_binding *binding) {
  river_xkb_binding_v1_destroy(binding->obj);
  wl_list_remove(&binding->link);
  free(binding);
}

static void xkb_binding_create(
  struct seat *seat, uint32_t mods,
  xkb_keysym_t keysym,
  enum action action,
  enum action action_released
) {
  struct xkb_binding *binding = calloc(1, sizeof(struct xkb_binding));
  if (!binding) LOG_FATA_AND_EXIT("failed to allocate memory for xkb binding\n");
  binding->obj = river_xkb_bindings_v1_get_xkb_binding(seat->wm->xkb_bindings, seat->obj, keysym, mods);
  binding->seat = seat;
  binding->action = action;
  binding->action_released = action_released;

  river_xkb_binding_v1_add_listener(binding->obj, &river_xkb_binding_listener, binding);
  river_xkb_binding_v1_enable(binding->obj);

  wl_list_insert(seat->xkb_bindings.prev, &binding->link);
}

///////////////////////////////////////////////////////////////////////////////
// sec:pointer_binding
///////////////////////////////////////////////////////////////////////////////

static void pointer_binding_handle_pressed(void *data, struct river_pointer_binding_v1 *obj) {
  struct pointer_binding *binding = data;
  binding->seat->pending_action = binding->action;
}

static void pointer_binding_handle_released(void *data, struct river_pointer_binding_v1 *obj) {}

const struct river_pointer_binding_v1_listener river_pointer_binding_listener = {
  .pressed = pointer_binding_handle_pressed,
  .released = pointer_binding_handle_released,
};

static void pointer_binding_destroy(struct pointer_binding *binding) {
  river_pointer_binding_v1_destroy(binding->obj);
  wl_list_remove(&binding->link);
  free(binding);
}

static void pointer_binding_create(
  struct seat *seat, uint32_t mods, uint32_t button, enum action action
) {
  struct pointer_binding *binding = calloc(1, sizeof(struct pointer_binding));
  if (!binding) LOG_FATA_AND_EXIT("failed to allocate memory for pointer binding\n");
  binding->obj = river_seat_v1_get_pointer_binding(seat->obj, button, mods);
  binding->seat = seat;
  binding->action = action;

  river_pointer_binding_v1_add_listener(binding->obj, &river_pointer_binding_listener, binding);
  river_pointer_binding_v1_enable(binding->obj);

  wl_list_insert(seat->pointer_bindings.prev, &binding->link);
}

///////////////////////////////////////////////////////////////////////////////
// sec:seat
///////////////////////////////////////////////////////////////////////////////

static void seat_handle_removed(void *data, struct river_seat_v1 *obj) {
  struct seat *seat = data;
  seat->removed = true;
}

static void seat_handle_pointer_enter(
  void *data, struct river_seat_v1 *obj,
  struct river_window_v1 *river_window
) {
  struct seat *seat = data;
  seat->hovered = river_window_v1_get_user_data(river_window);
}

static void seat_handle_pointer_leave(void *data, struct river_seat_v1 *obj) {
  struct seat *seat = data;
  seat->hovered = NULL;
}

static void seat_handle_window_interaction(
  void *data, struct river_seat_v1 *obj,
  struct river_window_v1 *river_window
) {
  struct seat *seat = data;
  seat->interacted = river_window_v1_get_user_data(river_window);
}

static void seat_handle_op_delta(
  void *data, struct river_seat_v1 *obj,
  int32_t dx, int32_t dy
) {
  struct seat *seat = data;
  seat->op_dx = dx;
  seat->op_dy = dy;
}

static void seat_handle_op_release(void *data, struct river_seat_v1 *obj) {
  struct seat *seat = data;
  seat->op_release = true;
}

static void seat_handle_pointer_position(void *data, struct river_seat_v1 *obj, int32_t x, int32_t y) {
  struct seat *seat = data;
  seat->pointer_x = x;
  seat->pointer_y = y;
  seat->pointer_positioned = true;
}

static void seat_get_pointer(struct seat *seat);

static void seat_handle_wl_seat(void *data, struct river_seat_v1 *obj, uint32_t name) {
  struct seat *seat = data;
  struct wl_seat_reg *reg;
  wl_list_for_each (reg, &seat->wm->wl_seats, link) {
    if (reg->name != name) continue;
    seat->wl_seat = reg->wl_seat;
    reg->seat = seat;
    /* A seat without a pointer (e.g. a keyboard-only seat) must not get a
       wl_pointer, the compositor disconnects a client asking for one. The
       capability is still tracked for when it shows up later. */
    if (reg->capabilities & WL_SEAT_CAPABILITY_POINTER)
      seat_get_pointer(seat);
    return;
  }
  LOG_ERRO("couldn't find wl_seat for name %u\n", name);
}

/* ignored */
static void seat_handle_shell_surface_interaction(void *data, struct river_seat_v1 *obj, struct river_shell_surface_v1 *river_shell_surface) {}

const struct river_seat_v1_listener river_seat_listener = {
  .removed = seat_handle_removed,
  .wl_seat = seat_handle_wl_seat,
  .pointer_enter = seat_handle_pointer_enter,
  .pointer_leave = seat_handle_pointer_leave,
  .window_interaction = seat_handle_window_interaction,
  .shell_surface_interaction = seat_handle_shell_surface_interaction,
  .op_delta = seat_handle_op_delta,
  .op_release = seat_handle_op_release,
  .pointer_position = seat_handle_pointer_position,
};

static void seat_maybe_destroy(struct seat *seat) {
  if (!seat->removed) return;

  { struct xkb_binding *binding, *tmp;
    wl_list_for_each_safe (binding, tmp, &seat->xkb_bindings, link)
      xkb_binding_destroy(binding);
  }
  { struct pointer_binding *binding, *tmp;
    wl_list_for_each_safe (binding, tmp, &seat->pointer_bindings, link)
      pointer_binding_destroy(binding);
  }

  if (seat->layer_shell_seat) {
    river_layer_shell_seat_v1_destroy(seat->layer_shell_seat);
    seat->layer_shell_seat = NULL;
  }

  /* The compositor ends the interactive operation with the seat, but the
     window keeps the resize state it was told about, so close that down. Also
     drop the references windows may still hold to this seat. */
  if (seat->op == SEAT_OP_RESIZE && seat->op_window)
    river_window_v1_inform_resize_end(seat->op_window->obj);

  struct window *window;
  wl_list_for_each (window, &seat->wm->windows, link) {
    if (window->pointer_move_requested == seat)
      window->pointer_move_requested = NULL;
    if (window->pointer_resize_requested == seat) {
      window->pointer_resize_requested = NULL;
      window->pointer_resize_requested_edges = 0;
    }
  }

  /* Capability updates arrive on the wl_seat, not on the river seat, so drop
     the back reference the registry entry may still hold to this seat. */
  { struct wl_seat_reg *reg;
    wl_list_for_each (reg, &seat->wm->wl_seats, link)
      if (reg->seat == seat) reg->seat = NULL;
  }

  river_seat_v1_destroy(seat->obj);
  wl_list_remove(&seat->link);
  free(seat);
}

static void seat_focus_impl(struct seat *seat, struct window *window, bool force) {
  if (!window && !wl_list_empty(&seat->wm->windows))
    window = wl_container_of(seat->wm->windows.prev, window, link);

  if (!force && seat->focused == window) return;

  if (window) {
    river_seat_v1_focus_window(seat->obj, window->obj);
    river_node_v1_place_top(window->node);
    wl_list_remove(&window->link);
    wl_list_insert(seat->wm->windows.prev, &window->link);
  } else {
    river_seat_v1_clear_focus(seat->obj);
  }

  seat->focused = window;
}

static void seat_focus(struct seat *seat, struct window *window) {
  seat_focus_impl(seat, window, false);
}

static void seat_focus_force(struct seat *seat, struct window *window) {
  seat_focus_impl(seat, window, true);
}

static uint32_t cursor_shape_for_edges(uint32_t edges) {
  bool top    = edges & RIVER_WINDOW_V1_EDGES_TOP;
  bool bottom = edges & RIVER_WINDOW_V1_EDGES_BOTTOM;
  bool left   = edges & RIVER_WINDOW_V1_EDGES_LEFT;
  bool right  = edges & RIVER_WINDOW_V1_EDGES_RIGHT;

  if ((top && left) || (bottom && right)) return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE;
  if ((top && right) || (bottom && left)) return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE;
  if (top || bottom)  return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE;
  if (left || right)  return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE;
  return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
}

static void seat_set_cursor_shape(struct seat *seat, uint32_t shape) {
  if (!seat->wl_pointer || !seat->wm->cursor_shape_mgr) return;
  if (!seat->cursor_shape_dev) {
    seat->cursor_shape_dev = wp_cursor_shape_manager_v1_get_pointer(
        seat->wm->cursor_shape_mgr, seat->wl_pointer);
  }
  wp_cursor_shape_device_v1_set_shape(seat->cursor_shape_dev, 0, shape);
}

static void seat_pointer_move(struct seat *seat, struct window *window) {
  if (window->state != WINDOW_STATE_READY) return;
  /* river ignores an op_start_pointer while an operation is in progress;
     mirror that here instead of overwriting our own operation state. */
  if (seat->op != SEAT_OP_NONE) return;
  seat_focus(seat, window);
  seat_set_cursor_shape(seat, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING);
  river_seat_v1_op_start_pointer(seat->obj);
  seat->op = SEAT_OP_MOVE;
  seat->op_window = window;
  seat->op_start_x = window->dimensions.x;
  seat->op_start_y = window->dimensions.y;
  seat->op_dx = 0;
  seat->op_dy = 0;
}

static void seat_pointer_resize(struct seat *seat, struct window *window, uint32_t edges) {
  if (window->state != WINDOW_STATE_READY) return;
  /* See seat_pointer_move(): never start an operation over a running one. */
  if (seat->op != SEAT_OP_NONE) return;
  seat_focus(seat, window);
  river_window_v1_inform_resize_start(window->obj);
  /* See seat_pointer_move(): the shape has to be set before the op begins. */
  seat_set_cursor_shape(seat, cursor_shape_for_edges(edges));
  river_seat_v1_op_start_pointer(seat->obj);
  seat->op = SEAT_OP_RESIZE;
  seat->op_window = window;
  seat->op_edges = edges;
  seat->op_start_x = window->dimensions.x;
  seat->op_start_y = window->dimensions.y;
  seat->op_start_width = window->dimensions.width;
  seat->op_start_height = window->dimensions.height;
  seat->op_dx = 0;
  seat->op_dy = 0;
}

static inline pid_t spawn(const char *path, ...) {
    va_list ap, ap2;
    int argc = 1;

    va_start(ap, path);
    va_copy(ap2, ap);
    while (va_arg(ap2, char *) != NULL)
        argc++;
    va_end(ap2);

    char **argv = malloc(sizeof(char *) * (argc + 1));
    if (!argv) {
        va_end(ap);
        LOG_ERRO("failed to allocate argv: %s\n", strerror(errno));
        return -1;
    }

    argv[0] = (char *)path;
    for (int i = 1; i < argc; i++)
        argv[i] = va_arg(ap, char *);
    argv[argc] = NULL;
    va_end(ap);

    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERRO("failed to fork: %s\n", strerror(errno));
        free(argv);
        return -1;
    }

    if (pid == 0) {
        execvp(path, argv);
        LOG_ERRO("failed to exec: %s (%s)\n", path, strerror(errno));
        _exit(127);
    }

    LOG_INFO("spawned %s (pid %d)\n", path, pid);
    free(argv);
    return pid;
}

static void seat_consume_action(struct seat *seat) {
  switch (seat->pending_action) {
  case ACTION_NONE: break;
  case ACTION_SPAWN_FOOT: spawn("foot", (char *) NULL); break;
  case ACTION_SPAWN_MENU: spawn("menu-run", (char *) NULL); break;
  case ACTION_SWITCH_IM: spawn("fcitx5-remote", "-t", (char *) NULL); break;
  case ACTION_CLOSE:
    if (seat->focused) river_window_v1_close(seat->focused->obj);
    break; 
  case ACTION_FOCUS_NEXT:
    if (!wl_list_empty(&seat->wm->windows)) {
      struct window *window = wl_container_of(seat->wm->windows.next, window, link);
      seat_focus(seat, window);
    }
    break;
  case ACTION_MOVE:
    if (seat->op == SEAT_OP_NONE && seat->hovered)
      seat_pointer_move(seat, seat->hovered);
    break;
  case ACTION_RESIZE:
    if (seat->op == SEAT_OP_NONE && seat->hovered)
      seat_pointer_resize(seat, seat->hovered, RIVER_WINDOW_V1_EDGES_BOTTOM | RIVER_WINDOW_V1_EDGES_RIGHT);
    break;
  case ACTION_TOGGLE_FULLSCREEN:
    if (seat->focused) {
      window_toggle_pending_mode(seat->focused, WINDOW_PENDING_FULLSCREEN, WINDOW_PENDING_UNFULLSCREEN, WINDOW_MODE_FULLSCREEN);
      window_manage(seat->focused);
    }
    break;
  case ACTION_TOGGLE_MAXIMIZE:
    if (seat->focused) {
      window_toggle_pending_mode(seat->focused, WINDOW_PENDING_MAXIMIZED, WINDOW_PENDING_UNMAXIMIZED, WINDOW_MODE_MAXIMIZED);
      window_manage(seat->focused);
    }
    break;
  case ACTION_EXIT:
    river_window_manager_v1_exit_session(seat->wm->rwm);
    break;
  }
  seat->pending_action = ACTION_NONE;
}

static void seat_op_target_dimensions(
  struct seat *seat, int32_t *width, int32_t *height
) {
  int32_t w = seat->op_start_width;
  int32_t h = seat->op_start_height;
  if ((seat->op_edges & RIVER_WINDOW_V1_EDGES_LEFT)   != 0) w -= seat->op_dx;
  if ((seat->op_edges & RIVER_WINDOW_V1_EDGES_RIGHT)  != 0) w += seat->op_dx;
  if ((seat->op_edges & RIVER_WINDOW_V1_EDGES_TOP)    != 0) h -= seat->op_dy;
  if ((seat->op_edges & RIVER_WINDOW_V1_EDGES_BOTTOM) != 0) h += seat->op_dy;
  *width  = w > 1 ? w : 1;
  *height = h > 1 ? h : 1;
}

static void seat_op_apply_position(struct seat *seat) {
  if (seat->op != SEAT_OP_RESIZE || !seat->op_window) return;

  int32_t width, height;
  seat_op_target_dimensions(seat, &width, &height);

  int32_t x = seat->op_start_x;
  int32_t y = seat->op_start_y;
  if ((seat->op_edges & RIVER_WINDOW_V1_EDGES_LEFT) != 0)
    x += seat->op_start_width - width;
  if ((seat->op_edges & RIVER_WINDOW_V1_EDGES_TOP) != 0)
    y += seat->op_start_height - height;

  window_set_position(seat->op_window, x, y);
}

static void seat_op_propose_dimensions(struct seat *seat) {
  if (seat->op != SEAT_OP_RESIZE || !seat->op_window) return;

  int32_t width, height;
  seat_op_target_dimensions(seat, &width, &height);

  river_window_v1_propose_dimensions(seat->op_window->obj, width, height);
}

static void seat_consume_operation(struct seat *seat) {
  switch (seat->op) {
  case SEAT_OP_NONE: break;
  case SEAT_OP_MOVE:
    if (seat->op_release) {
      /* Apply the last motion before the operation ends: seat_render() will
         not handle this operation any more, so its final delta would be lost. */
      window_set_position(seat->op_window,
          seat->op_start_x + seat->op_dx,
          seat->op_start_y + seat->op_dy);
      river_seat_v1_op_end(seat->obj);
      seat_set_cursor_shape(seat, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
      seat->op = SEAT_OP_NONE;
      seat->op_window = NULL;
    }
    break;
  /* TODO: unmaximized while resizing */
  case SEAT_OP_RESIZE: {
    if (seat->op_release) {
      seat_op_propose_dimensions(seat);
      seat_op_apply_position(seat);
      river_window_v1_inform_resize_end(seat->op_window->obj);
      river_seat_v1_op_end(seat->obj);
      seat_set_cursor_shape(seat, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
      seat->op = SEAT_OP_NONE;
      seat->op_window = NULL;
      break;
    }
    seat_op_propose_dimensions(seat);
    break;
  }
  }
  seat->op_release = false;
}

static void seat_apply_focus(struct seat *seat) {
  if (seat->layer_shell_seat_focus_pending_none) {
    seat->layer_shell_seat_focus_pending_none = false;
    seat_focus_force(seat, seat->focused);
  }

  if (seat->interacted) {
    seat_focus(seat, seat->interacted);
    seat->interacted = NULL;
  } else {
    struct window *window;
    wl_list_for_each (window, &seat->wm->windows, link) {
      if (window->pending_mode & WINDOW_PENDING_FOCUS) {
        seat_focus(seat, window);
        window->pending_mode &= ~WINDOW_PENDING_FOCUS;
        break;
      }
    }
  }

  if (!seat->focused &&
      seat->layer_shell_seat_focus != LAYER_SHELL_SEAT_FOCUS_EXCLUSIVE &&
      seat->layer_shell_seat_focus != LAYER_SHELL_SEAT_FOCUS_NON_EXCLUSIVE)
  {
    seat_focus(seat, NULL);
  }
}

static void seat_new(struct seat *seat) {
  xkb_binding_create(seat, MOD_SUPER, XKB_KEY_Return, ACTION_SPAWN_FOOT, ACTION_NONE);
  xkb_binding_create(seat, MOD_SUPER, XKB_KEY_d,      ACTION_SPAWN_MENU, ACTION_NONE);
  xkb_binding_create(seat, MOD_CTRL,  XKB_KEY_space,  ACTION_SWITCH_IM,  ACTION_NONE);
  xkb_binding_create(seat, MOD_ALT,   XKB_KEY_Tab,    ACTION_FOCUS_NEXT, ACTION_NONE);
  xkb_binding_create(seat, MOD_SUPER, XKB_KEY_q,      ACTION_CLOSE, ACTION_NONE);
  xkb_binding_create(seat, MOD_SUPER, XKB_KEY_f,      ACTION_TOGGLE_FULLSCREEN, ACTION_NONE);
  xkb_binding_create(seat, MOD_SUPER, XKB_KEY_m,      ACTION_TOGGLE_MAXIMIZE, ACTION_NONE);
  xkb_binding_create(seat, MOD_SUPER, XKB_KEY_Escape, ACTION_EXIT, ACTION_NONE);

  pointer_binding_create(seat, MOD_SUPER, BTN_LEFT,  ACTION_MOVE);
  pointer_binding_create(seat, MOD_SUPER, BTN_RIGHT, ACTION_RESIZE);

  if (river_seat_v1_get_version(seat->obj)
      >= RIVER_SEAT_V1_SET_XCURSOR_THEME_SINCE_VERSION)
  {
    river_seat_v1_set_xcursor_theme(seat->obj, XCURSOR_THEME, XCURSOR_SIZE);
  }
  
  seat->new = false;
}

static void seat_manage(struct seat *seat) {
  if (seat->new) seat_new(seat);
  seat_apply_focus(seat);
  seat_consume_action(seat);
  seat_consume_operation(seat);
}

static void seat_render(struct seat *seat) {
  switch (seat->op) {
  case SEAT_OP_NONE: break;
  case SEAT_OP_MOVE:
    window_set_position(seat->op_window, seat->op_start_x + seat->op_dx, seat->op_start_y + seat->op_dy);
    break;
  case SEAT_OP_RESIZE:
    seat_op_apply_position(seat);
    break;
  }
}

///////////////////////////////////////////////////////////////////////////////
// sec:wl_pointer
// sec:wl_seat
///////////////////////////////////////////////////////////////////////////////

static const struct wl_pointer_listener wl_pointer_listener = { 0 };

static void seat_get_pointer(struct seat *seat) {
  if (seat->wl_pointer || !seat->wl_seat) return;
  seat->wl_pointer = wl_seat_get_pointer(seat->wl_seat);
  wl_pointer_add_listener(seat->wl_pointer, &wl_pointer_listener, seat);
}

/* Drop the pointer state when the seat loses the pointer capability. The
   cursor shape bookkeeping goes with the device backing it. */
static void seat_release_pointer(struct seat *seat) {
  if (seat->cursor_shape_dev) {
    wp_cursor_shape_device_v1_destroy(seat->cursor_shape_dev);
    seat->cursor_shape_dev = NULL;
  }
  if (seat->wl_pointer) {
    wl_pointer_destroy(seat->wl_pointer);
    seat->wl_pointer = NULL;
  }
}

static void wl_seat_handle_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
  struct wl_seat_reg *reg = data;
  reg->capabilities = caps;
  if (!reg->seat) return;
  if (caps & WL_SEAT_CAPABILITY_POINTER)
    seat_get_pointer(reg->seat);
  else
    seat_release_pointer(reg->seat);
}

/* ignored */
static void wl_seat_handle_name(void *data, struct wl_seat *seat, const char *name) {}

static const struct wl_seat_listener wl_seat_listener = {
  .capabilities = wl_seat_handle_capabilities,
  .name = wl_seat_handle_name,
};

///////////////////////////////////////////////////////////////////////////////
// sec:layer_shell_seat
///////////////////////////////////////////////////////////////////////////////

static void layer_shell_seat_handle_focus_exclusive(void *data, struct river_layer_shell_seat_v1 *obj) {
  struct seat *seat = data;
  seat->layer_shell_seat_focus = LAYER_SHELL_SEAT_FOCUS_EXCLUSIVE;
}

static void layer_shell_seat_handle_focus_non_exclusive(void *data, struct river_layer_shell_seat_v1 *obj) {
  struct seat *seat = data;
  seat->layer_shell_seat_focus = LAYER_SHELL_SEAT_FOCUS_NON_EXCLUSIVE;
}

static void layer_shell_seat_handle_focus_none(void *data, struct river_layer_shell_seat_v1 *obj) {
  struct seat *seat = data;
  seat->layer_shell_seat_focus = LAYER_SHELL_SEAT_FOCUS_NONE;
  seat->layer_shell_seat_focus_pending_none = true;
}

static const struct river_layer_shell_seat_v1_listener layer_shell_seat_listener = {
  .focus_exclusive = layer_shell_seat_handle_focus_exclusive,
  .focus_non_exclusive = layer_shell_seat_handle_focus_non_exclusive,
  .focus_none = layer_shell_seat_handle_focus_none,
};

///////////////////////////////////////////////////////////////////////////////
// sec:wm
///////////////////////////////////////////////////////////////////////////////

static void wm_handle_unavailable(
  void *data, struct river_window_manager_v1 *obj
) {
  LOG_FATA_AND_EXIT("another windows manager is already running\n");
}

static void wm_handle_finished(void *data, struct river_window_manager_v1 *obj) {
  LOG_DEBG("wm_handle_finished\n");
  exit(EXIT_SUCCESS);
}

static void wm_handle_manage_start(void *data, struct river_window_manager_v1 *obj) {
  LOG_DEBG("manage start\n");

  struct wm *wm = data;

  { struct output *output, *tmp;
    wl_list_for_each_safe (output, tmp, &wm->outputs, link)
      output_maybe_destroy(output);
  }
  { struct window *window, *tmp;
    wl_list_for_each_safe (window, tmp, &wm->windows, link)
      window_maybe_destroy(window);
  }
  { struct seat *seat, *tmp;
    wl_list_for_each_safe (seat, tmp, &wm->seats, link)
      seat_maybe_destroy(seat);
  }

  struct window *window, *tmp;
  wl_list_for_each_safe (window, tmp, &wm->windows, link)
    window_manage(window);

  struct seat *seat;
  wl_list_for_each (seat, &wm->seats, link)
    seat_manage(seat);

  river_window_manager_v1_manage_finish(wm->rwm);
}

static void wm_handle_render_start(void *data, struct river_window_manager_v1 *obj) {
  LOG_DEBG("render start\n");

  struct wm *wm = data;

  struct window *window;
  wl_list_for_each (window, &wm->windows, link)
    window_render(wm, window);

  struct seat *seat;
  wl_list_for_each (seat, &wm->seats, link)
    seat_render(seat);

  river_window_manager_v1_render_finish(wm->rwm);
}

static void wm_handle_window(
  void *data,
  struct river_window_manager_v1 *obj,
  struct river_window_v1 *river_window
) {
  struct wm *wm = data;
  struct window *window = calloc(1, sizeof(struct window));
  if (!window)
    LOG_FATA_AND_EXIT("failed to allocate memory for window\n");
  window->wm = wm;
  window->obj = river_window;
  window->node = river_window_v1_get_node(window->obj);
  window->state = WINDOW_STATE_NEW;
  window->decoration_hint = -1;
  window->pending_mode |= WINDOW_PENDING_FOCUS;
  window->mode |= WINDOW_MODE_DEFAULT;
  river_window_v1_add_listener(window->obj, &river_window_listener, window);
  wl_list_insert(wm->windows.prev, &window->link);
}

static void wm_handle_output(
  void *data, 
  struct river_window_manager_v1 *obj,
  struct river_output_v1 *river_output
) {
  struct wm *wm = data;

  struct output *output = calloc(1, sizeof(struct output));
  if (!output) LOG_FATA_AND_EXIT("failed to allocate memory for output\n");
  output->obj = river_output;
  output->wm = wm;
  river_output_v1_add_listener(output->obj, &river_output_listener, output);

  output->layer_shell_output = river_layer_shell_v1_get_output(wm->layer_shell, output->obj);
  river_layer_shell_output_v1_add_listener(output->layer_shell_output, &river_layer_shell_output_listener, output);

  wl_list_insert(wm->outputs.prev, &output->link);
}

static void wm_handle_seat(
  void *data,
  struct river_window_manager_v1 *obj,
  struct river_seat_v1 *river_seat
) {
  struct wm *wm = data;
  struct seat *seat = calloc(1, sizeof(struct seat));
  if (!seat) LOG_FATA_AND_EXIT("failed to allocate memory for seat\n");
  seat->obj = river_seat;
  seat->new = true;
  seat->wm = wm;
  wl_list_init(&seat->xkb_bindings);
  wl_list_init(&seat->pointer_bindings);

  river_seat_v1_add_listener(seat->obj, &river_seat_listener, seat);

  seat->layer_shell_seat = river_layer_shell_v1_get_seat(wm->layer_shell, seat->obj);
  river_layer_shell_seat_v1_add_listener(seat->layer_shell_seat, &layer_shell_seat_listener, seat);

  wl_list_insert(wm->seats.prev, &seat->link);
}

/* ignored */
static void wm_handle_session_locked(void *data, struct river_window_manager_v1 *obj) {}
static void wm_handle_session_unlocked(void *data, struct river_window_manager_v1 *obj) {}

static const struct river_window_manager_v1_listener wm_listener = {
  .unavailable      = wm_handle_unavailable,
  .finished         = wm_handle_finished,
  .manage_start     = wm_handle_manage_start,
  .render_start     = wm_handle_render_start,
  .session_locked   = wm_handle_session_locked,
  .session_unlocked = wm_handle_session_unlocked,
  .window           = wm_handle_window,
  .output           = wm_handle_output,
  .seat             = wm_handle_seat,
};

static void wm_init(struct wm *wm) {
  wl_list_init(&wm->outputs);
  wl_list_init(&wm->wl_outputs);
  wl_list_init(&wm->wl_seats);
  wl_list_init(&wm->windows);
  wl_list_init(&wm->seats);
}

///////////////////////////////////////////////////////////////////////////////
// sec:global
///////////////////////////////////////////////////////////////////////////////

static void handle_global(
  void *data,
  struct wl_registry *registry,
  uint32_t name,
  const char *interface,
  uint32_t version
) {
  struct wm *wm = data;
  if (strcmp(interface, river_window_manager_v1_interface.name) == 0) {
    if (version >= 4)
      wm->rwm = wl_registry_bind(
        registry, name, &river_window_manager_v1_interface, 4);
  } else if (strcmp(interface, river_xkb_bindings_v1_interface.name) == 0) {
    wm->xkb_bindings = wl_registry_bind(
      registry, name, &river_xkb_bindings_v1_interface, 1);
  } else if (strcmp(interface, river_layer_shell_v1_interface.name) == 0) {
    wm->layer_shell = wl_registry_bind(
      registry, name, &river_layer_shell_v1_interface, 1);
  } else if (strcmp(interface, wl_output_interface.name) == 0) {
    struct wl_output_reg *reg = calloc(1, sizeof(*reg));
    if (!reg) LOG_FATA_AND_EXIT("failed to allocate memory for wl_output\n");
    reg->name = name;
    reg->wl_output = wl_registry_bind(
      registry, name, &wl_output_interface, 1);
    wl_output_add_listener(reg->wl_output, &wl_output_listener, reg);
    wl_list_insert(&wm->wl_outputs, &reg->link);
  } else if (strcmp(interface, wl_seat_interface.name) == 0) {
    struct wl_seat_reg *reg = calloc(1, sizeof(*reg));
    if (!reg) LOG_FATA_AND_EXIT("failed to allocate memory for wl_seat\n");
    reg->name = name;
    reg->wm = wm;
    reg->wl_seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    wl_seat_add_listener(reg->wl_seat, &wl_seat_listener, reg);
    wl_list_insert(&wm->wl_seats, &reg->link);
  } else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
    wm->cursor_shape_mgr = wl_registry_bind(
      registry, name, &wp_cursor_shape_manager_v1_interface, 1);
    // wm->cursor_shape_dev = wp_cursor_shape_manager_v1_get_pointer(wm->cursor_shape_mgr, pointer);
  }
}

static void handle_global_remove(
  void *data,
  struct wl_registry *registry,
  uint32_t name
) {
  struct wm *wm = data;
  { struct wl_output_reg *reg, *tmp;
    wl_list_for_each_safe (reg, tmp, &wm->wl_outputs, link) {
      if (reg->name != name) continue;

      wl_list_remove(&reg->link);

      struct output *output;
      wl_list_for_each (output, &wm->outputs, link) {
        if (output->wl_output == reg->wl_output) {
          output->wl_output = NULL;
          break;
        }
      }

      wl_output_destroy(reg->wl_output);
      free(reg);
      break;
    }
  }
  { struct wl_seat_reg *reg, *tmp;
    wl_list_for_each_safe (reg, tmp, &wm->wl_seats, link) {
      if (reg->name != name) continue;

      if (reg->seat) {
        struct seat *seat = reg->seat;
        seat_release_pointer(seat);
        seat->wl_seat = NULL;
        reg->seat = NULL;
      }

      wl_list_remove(&reg->link);
      wl_seat_destroy(reg->wl_seat);
      free(reg);
      break;
    }
  }
}

static const struct wl_registry_listener registry_listener = {
  .global = handle_global,
  .global_remove = handle_global_remove,
};

///////////////////////////////////////////////////////////////////////////////
// sec:main
///////////////////////////////////////////////////////////////////////////////

int main(void) {
  setenv("XCURSOR_THEME", XCURSOR_THEME, 0);
  setenv("XCURSOR_SIZE", STRINGIZE(XCURSOR_SIZE), 1);

  struct wl_display *display = wl_display_connect(NULL);
  if (!display)
    LOG_FATA_AND_EXIT("failed to connect wayland server\n");
  LOG_INFO("connected to wayland display server\n");

  unsetenv("WAYLAND_DEBUG");
  signal(SIGCHLD, SIG_IGN);

  struct wm wm = {0};
  wm_init(&wm);
  LOG_INFO("initialized\n");

  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, &wm);
  if (wl_display_roundtrip(display) < 0) // sync point
    LOG_FATA_AND_EXIT("roundtrip failed\n");

  if (!wm.rwm || !wm.xkb_bindings)
    LOG_FATA_AND_EXIT("river_window_manager_v1 or river_xkb_bindings_v1 "
                      "is not supported by the Wayland server\n");

  if (!wm.layer_shell)
    LOG_FATA_AND_EXIT("river_layer_shell_v1 is not supported by the Wayland server\n");

  LOG_DEBG("synchronized to server successfully\n");

  river_window_manager_v1_add_listener(wm.rwm, &wm_listener, &wm);
  LOG_DEBG("added wm listener\n");

  for (;;) {
    if (wl_display_dispatch(display) < 0) {
      if (errno == EPIPE) break;
      LOG_FATA_AND_EXIT("dispatch failed\n");
    }
  }

  exit(EXIT_SUCCESS);
}
