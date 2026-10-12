#include "config.h"
#include <gtk/gtk.h>
#include <gdk/wayland/gdkwayland.h>

#include "gdk/gdkdisplayprivate.h"
#include "gdk/wayland/gdkdevice-wayland-private.h"

static void
test_pointer_frame (gconstpointer data)
{
  guint flags = GPOINTER_TO_UINT (data);
  guint mode = flags & 3;
  guint order = flags >> 2;
  GdkDisplay *display = gdk_display_get_default ();
  GdkWaylandSeat *seat;
  GdkWaylandPointerData saved;
  GdkSurface *surface;
  const struct wl_pointer_listener *listener;
  GdkEvent *event;
  guint version;

  if (!GDK_IS_WAYLAND_DISPLAY (display))
    {
      g_test_skip ("No Wayland display");
      return;
    }

  seat = GDK_WAYLAND_SEAT (gdk_display_get_default_seat (display));
  if (!seat->wl_pointer)
    {
      g_test_skip ("No pointer");
      return;
    }

  version = wl_pointer_get_version (seat->wl_pointer);
  if (version < WL_POINTER_FRAME_SINCE_VERSION ||
      (mode == 2 && version < WL_POINTER_AXIS_VALUE120_SINCE_VERSION))
    {
      g_test_skip ("Pointer protocol version too old");
      return;
    }

  listener = wl_proxy_get_listener ((struct wl_proxy *) seat->wl_pointer);
  surface = gdk_surface_new_toplevel (display);
  while ((event = g_queue_pop_head (&display->queued_events)))
    gdk_event_unref (event);
  saved = seat->pointer_info;
  g_assert_null (saved.frame.event);
  seat->pointer_info.focus = g_object_ref (surface);
  seat->pointer_info.frame = (GdkWaylandPointerFrameData) { 0 };

  for (guint i = 0; i < 16; i++)
    {
      double delta_x, delta_y;
      guint n_motion = 0, n_button = 0, n_scroll = 0, n_leave = 0;

      if (!seat->pointer_info.focus)
        seat->pointer_info.focus = g_object_ref (surface);
      seat->pointer_info.button_modifiers = 0;
      listener->axis_source (seat, seat->wl_pointer,
                              mode == 0 || mode == 3 ? WL_POINTER_AXIS_SOURCE_FINGER :
                                                       WL_POINTER_AXIS_SOURCE_WHEEL);
      if (order == 1 || order == 3)
        listener->motion (seat, seat->wl_pointer, 111,
                           wl_fixed_from_int (15), wl_fixed_from_int (25));
      if (order == 3)
        listener->button (seat, seat->wl_pointer, 22, 111, 0x110,
                           WL_POINTER_BUTTON_STATE_PRESSED);

      if (mode == 3)
        {
          listener->axis_stop (seat, seat->wl_pointer, 111, WL_POINTER_AXIS_VERTICAL_SCROLL);
          listener->axis_stop (seat, seat->wl_pointer, 111, WL_POINTER_AXIS_HORIZONTAL_SCROLL);
        }
      else
        {
          listener->axis (seat, seat->wl_pointer, 111, WL_POINTER_AXIS_VERTICAL_SCROLL,
                         wl_fixed_from_int (12));
          if (mode != 1)
            listener->axis (seat, seat->wl_pointer, 111, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                           wl_fixed_from_int (-3));
          if (mode == 1)
            {
              listener->axis_discrete (seat, seat->wl_pointer, WL_POINTER_AXIS_VERTICAL_SCROLL, 1);
            }
          else if (mode == 2)
            {
              listener->axis_value120 (seat, seat->wl_pointer, WL_POINTER_AXIS_VERTICAL_SCROLL, 60);
              listener->axis_value120 (seat, seat->wl_pointer, WL_POINTER_AXIS_HORIZONTAL_SCROLL, -120);
            }
        }
      if (version >= WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION)
        listener->axis_relative_direction (seat, seat->wl_pointer,
                                            WL_POINTER_AXIS_VERTICAL_SCROLL,
                                            WL_POINTER_AXIS_RELATIVE_DIRECTION_INVERTED);
      if (order == 2 || order == 4)
        listener->motion (seat, seat->wl_pointer, 111,
                           wl_fixed_from_int (15), wl_fixed_from_int (25));
      if (order == 4)
        listener->leave (seat, seat->wl_pointer, 23,
                          gdk_wayland_surface_get_wl_surface (surface));
      listener->frame (seat, seat->wl_pointer);

      /* Inspect backend delivery before frame-clock event compression. */
      while ((event = g_queue_pop_head (&display->queued_events)))
        {
          g_assert_true (gdk_event_get_surface (event) == surface);
          g_assert_cmpuint (gdk_event_get_time (event), ==,
                            gdk_event_get_event_type (event) == GDK_LEAVE_NOTIFY ? 0 : 111);
          switch ((unsigned int) gdk_event_get_event_type (event))
            {
            case GDK_MOTION_NOTIFY:
              n_motion++;
              gdk_event_get_position (event, &delta_x, &delta_y);
              g_assert_cmpfloat (delta_x, ==, 15);
              g_assert_cmpfloat (delta_y, ==, 25);
              break;
            case GDK_BUTTON_PRESS:
              n_button++;
              g_assert_cmpuint (gdk_button_event_get_button (event), ==, GDK_BUTTON_PRIMARY);
              break;
            case GDK_LEAVE_NOTIFY:
              n_leave++;
              break;
            case GDK_SCROLL:
              n_scroll++;
              gdk_scroll_event_get_deltas (event, &delta_x, &delta_y);
              g_assert_cmpfloat (delta_x, ==, mode == 0 ? -3 : mode == 2 ? -1 : 0);
              g_assert_cmpfloat (delta_y, ==, mode == 0 ? 12 : mode == 1 ? 1 : mode == 2 ? 0.5 : 0);
              g_assert_cmpint (gdk_scroll_event_is_stop (event), ==, mode == 3);
              g_assert_cmpint (gdk_scroll_event_get_unit (event), ==,
                               mode == 0 || mode == 3 ? GDK_SCROLL_UNIT_SURFACE : GDK_SCROLL_UNIT_WHEEL);
              g_assert_true (gdk_event_get_device (event) ==
                             (mode == 0 || mode == 3 ? seat->finger_scrolling : seat->wheel_scrolling));
              if (version >= WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION)
                g_assert_cmpint (gdk_scroll_event_get_relative_direction (event), ==,
                                 GDK_SCROLL_RELATIVE_DIRECTION_INVERTED);
              break;
            default:
              g_assert_not_reached ();
            }
          gdk_event_unref (event);
        }
      g_assert_cmpuint (n_motion, ==, order != 0);
      g_assert_cmpuint (n_button, ==, order == 3);
      g_assert_cmpuint (n_scroll, ==, 1);
      g_assert_cmpuint (n_leave, ==, order == 4);
      listener->frame (seat, seat->wl_pointer);
      g_assert_true (g_queue_is_empty (&display->queued_events));
    }

  g_clear_object (&seat->pointer_info.focus);
  seat->pointer_info = saved;
  gdk_surface_destroy (surface);
}

int
main (int argc, char **argv)
{
  const char *modes[] = { "smooth", "discrete", "value120", "stop" };
  const char *orders[] = { "scroll-only", "motion-first", "scroll-first", "motion-button", "motion-leave" };

  gtk_test_init (&argc, &argv);
  for (guint flags = 0; flags < 20; flags++)
    {
      char *path = g_strdup_printf ("/wayland/pointer/frame/%s/%s", modes[flags & 3], orders[flags >> 2]);

      g_test_add_data_func (path, GUINT_TO_POINTER (flags), test_pointer_frame);
      g_free (path);
    }
  return g_test_run ();
}
