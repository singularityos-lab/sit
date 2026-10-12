#include "config.h"
#include <gtk/gtk.h>
#include <gdk/wayland/gdkwayland.h>

#include "gdk/gdkdisplayprivate.h"
#include "gdk/wayland/gdkdevice-wayland-private.h"

static void
drain_events (GdkDisplay *display)
{
  GdkEvent *event;

  while ((event = gdk_display_get_event (display)))
    gdk_event_unref (event);
}

static void
test_key_repeat (gconstpointer data)
{
  unsigned int phase = GPOINTER_TO_UINT (data);
  GdkDisplay *display = gdk_display_get_default ();
  GdkWaylandSeat *seat;
  GdkSurface *surface;
  GdkSurface *saved_focus;
  const struct wl_keyboard_listener *listener;
  struct wl_array keys = { 0 };
  gboolean saved_server_repeat;
  int saved_rate, saved_delay;
  unsigned int timer, count;
  int64_t deadline;
  struct wl_callback *callback = NULL;
  GdkEvent *event;

  if (!GDK_IS_WAYLAND_DISPLAY (display))
    {
      g_test_skip ("No Wayland display");
      return;
    }

  seat = GDK_WAYLAND_SEAT (gdk_display_get_default_seat (display));
  if (!seat->wl_keyboard)
    {
      g_test_skip ("No keyboard capability");
      return;
    }

  listener = wl_proxy_get_listener ((struct wl_proxy *) seat->wl_keyboard);
  saved_focus = g_steal_pointer (&seat->keyboard_focus);
  saved_server_repeat = seat->have_server_repeat;
  saved_rate = seat->server_repeat_rate;
  saved_delay = seat->server_repeat_delay;
  surface = gdk_surface_new_toplevel (display);
  listener->enter (seat, seat->wl_keyboard, 1,
                   gdk_wayland_surface_get_wl_surface (surface), &keys);
  listener->repeat_info (seat, seat->wl_keyboard, 50, 400);
  drain_events (display);

  listener->key (seat, seat->wl_keyboard, 2, 100, 30, WL_KEYBOARD_KEY_STATE_PRESSED);
  listener->key (seat, seat->wl_keyboard, 3, 110, 48, WL_KEYBOARD_KEY_STATE_PRESSED);
  event = gdk_display_get_event (display);
  g_assert_cmpint (gdk_event_get_event_type (event), ==, GDK_KEY_PRESS);
  g_assert_cmpuint (gdk_key_event_get_keycode (event), ==, 38);
  gdk_event_unref (event);
  event = gdk_display_get_event (display);
  g_assert_cmpint (gdk_event_get_event_type (event), ==, GDK_KEY_PRESS);
  g_assert_cmpuint (gdk_key_event_get_keycode (event), ==, 56);
  gdk_event_unref (event);

  timer = seat->repeat_timer;
  count = seat->repeat_count;
  deadline = seat->repeat_deadline;
  g_assert_cmpuint (timer, !=, 0);

  switch (phase)
    {
    case 0:
      listener->key (seat, seat->wl_keyboard, 4, 120, 30, WL_KEYBOARD_KEY_STATE_RELEASED);
      break;
    case 1:
      listener->key (seat, seat->wl_keyboard, 4, 120, 42, WL_KEYBOARD_KEY_STATE_PRESSED);
      break;
    case 2:
      listener->key (seat, seat->wl_keyboard, 4, 120, 42, WL_KEYBOARD_KEY_STATE_RELEASED);
      break;
    case 3:
      listener->key (seat, seat->wl_keyboard, 4, 120, 0, WL_KEYBOARD_KEY_STATE_PRESSED);
      break;
    case 4:
      g_source_remove (seat->repeat_timer);
      seat->repeat_timer = 0;
      timer = 0;
      callback = wl_display_sync (gdk_wayland_display_get_wl_display (display));
      seat->repeat_callback = callback;
      listener->key (seat, seat->wl_keyboard, 4, 120, 30, WL_KEYBOARD_KEY_STATE_RELEASED);
      break;
    case 5:
      listener->key (seat, seat->wl_keyboard, 4, 120, 48, WL_KEYBOARD_KEY_STATE_RELEASED);
      break;
    case 6:
      listener->leave (seat, seat->wl_keyboard, 4,
                       gdk_wayland_surface_get_wl_surface (surface));
      break;
    case 7:
      listener->repeat_info (seat, seat->wl_keyboard, 0, 400);
      listener->key (seat, seat->wl_keyboard, 4, 120, 48, WL_KEYBOARD_KEY_STATE_PRESSED);
      break;
    default:
      g_assert_not_reached ();
    }

  if (phase < 5)
    {
      g_assert_cmpuint (seat->repeat_timer, ==, timer);
      g_assert_cmpuint (seat->repeat_key, ==, 56);
      g_assert_cmpuint (seat->repeat_count, ==, count);
      g_assert_cmpint (seat->repeat_deadline, ==, deadline);
      g_assert_true (seat->repeat_callback == callback);
    }
  else
    {
      g_assert_cmpuint (seat->repeat_timer, ==, 0);
      g_assert_null (seat->repeat_callback);
      if (phase != 7)
        g_assert_cmpuint (seat->repeat_key, ==, 0);
    }

  listener->leave (seat, seat->wl_keyboard, 5,
                   gdk_wayland_surface_get_wl_surface (surface));
  g_assert_cmpuint (seat->repeat_timer, ==, 0);
  g_assert_null (seat->repeat_callback);
  seat->keyboard_focus = saved_focus;
  seat->have_server_repeat = saved_server_repeat;
  seat->server_repeat_rate = saved_rate;
  seat->server_repeat_delay = saved_delay;
  drain_events (display);
  gdk_surface_destroy (surface);
}

int
main (int argc, char **argv)
{
  const char *names[] = {
    "older-key-release", "modifier-press", "modifier-release", "no-symbol",
    "pending-sync", "active-key-release", "focus-leave", "repeat-disabled"
  };

  gtk_test_init (&argc, &argv);
  for (unsigned int i = 0; i < G_N_ELEMENTS (names); i++)
    {
      char *path = g_strdup_printf ("/wayland/keyboard/repeat/%s", names[i]);
      g_test_add_data_func (path, GUINT_TO_POINTER (i), test_key_repeat);
      g_free (path);
    }

  return g_test_run ();
}
