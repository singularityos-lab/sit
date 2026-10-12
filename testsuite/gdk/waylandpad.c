#include "config.h"
#include <gtk/gtk.h>
#include <gdk/wayland/gdkwayland.h>

#include "gdk/gdkdisplayprivate.h"
#include "gdk/wayland/gdkdevice-wayland-private.h"
#include "gdk/wayland/tablet-v2-client-protocol.h"

static void
test_pad_focus (gconstpointer data)
{
  GdkEventType type = GPOINTER_TO_UINT (data);
  GdkDisplay *display = gdk_display_get_default ();
  GdkWaylandSeat *seat;
  GdkSurface *surface;
  GdkSurface *other_surface;
  GdkSurface *weak_surface;
  GdkSurface *saved_focus;
  GdkWaylandTabletPadData *pad;
  GdkWaylandTabletPadGroupData *group;
  char name[] = "Pad test";
  GdkWaylandTabletData tablet = { .name = name };
  const struct zwp_tablet_seat_v2_listener *seat_listener;
  const struct zwp_tablet_pad_v2_listener *pad_listener;
  const struct zwp_tablet_pad_group_v2_listener *group_listener;
  const struct zwp_tablet_pad_ring_v2_listener *ring_listener;
  const struct zwp_tablet_pad_strip_v2_listener *strip_listener;
  const struct zwp_tablet_pad_dial_v2_listener *dial_listener;
  struct zwp_tablet_v2 *wp_tablet;
  struct zwp_tablet_pad_v2 *wp_pad;
  struct zwp_tablet_pad_group_v2 *wp_group;
  struct zwp_tablet_pad_ring_v2 *wp_ring;
  struct zwp_tablet_pad_strip_v2 *wp_strip;
  struct zwp_tablet_pad_dial_v2 *wp_dial;
  struct wl_array buttons;
  uint32_t button = 7;
  GdkEvent *event;

  if (!GDK_IS_WAYLAND_DISPLAY (display))
    {
      g_test_skip ("No Wayland display");
      return;
    }

  seat = GDK_WAYLAND_SEAT (gdk_display_get_default_seat (display));
  if (!seat->wp_tablet_seat)
    {
      g_test_skip ("No tablet protocol support");
      return;
    }

  surface = gdk_surface_new_toplevel (display);
  other_surface = gdk_surface_new_toplevel (display);
  saved_focus = g_steal_pointer (&seat->keyboard_focus);
  while ((event = gdk_display_get_event (display)))
    gdk_event_unref (event);

  /* Local proxies exercise the registered listeners without sending requests. */
  wp_tablet = (struct zwp_tablet_v2 *) wl_proxy_create ((struct wl_proxy *) seat->wl_seat,
                                                      &zwp_tablet_v2_interface);
  zwp_tablet_v2_set_user_data (wp_tablet, &tablet);
  wp_pad = (struct zwp_tablet_pad_v2 *) wl_proxy_create ((struct wl_proxy *) seat->wl_seat,
                                                      &zwp_tablet_pad_v2_interface);
  seat_listener = wl_proxy_get_listener ((struct wl_proxy *) seat->wp_tablet_seat);
  seat_listener->pad_added (seat, seat->wp_tablet_seat, wp_pad);
  pad = zwp_tablet_pad_v2_get_user_data (wp_pad);
  pad_listener = wl_proxy_get_listener ((struct wl_proxy *) wp_pad);
  pad_listener->enter (pad, wp_pad, 1, wp_tablet,
                       gdk_wayland_surface_get_wl_surface (surface));

  wp_group = (struct zwp_tablet_pad_group_v2 *) wl_proxy_create ((struct wl_proxy *) seat->wl_seat,
                                                              &zwp_tablet_pad_group_v2_interface);
  pad_listener->group (pad, wp_pad, wp_group);
  group = zwp_tablet_pad_group_v2_get_user_data (wp_group);
  group_listener = wl_proxy_get_listener ((struct wl_proxy *) wp_group);
  buttons = (struct wl_array) { .size = sizeof button, .data = &button };
  group_listener->buttons (group, wp_group, &buttons);
  group_listener->modes (group, wp_group, 3);

  wp_ring = (struct zwp_tablet_pad_ring_v2 *) wl_proxy_create ((struct wl_proxy *) seat->wl_seat,
                                                            &zwp_tablet_pad_ring_v2_interface);
  group_listener->ring (group, wp_group, wp_ring);
  ring_listener = wl_proxy_get_listener ((struct wl_proxy *) wp_ring);
  wp_strip = (struct zwp_tablet_pad_strip_v2 *) wl_proxy_create ((struct wl_proxy *) seat->wl_seat,
                                                             &zwp_tablet_pad_strip_v2_interface);
  group_listener->strip (group, wp_group, wp_strip);
  strip_listener = wl_proxy_get_listener ((struct wl_proxy *) wp_strip);
  wp_dial = (struct zwp_tablet_pad_dial_v2 *) wl_proxy_create ((struct wl_proxy *) seat->wl_seat,
                                                           &zwp_tablet_pad_dial_v2_interface);
  group_listener->dial (group, wp_group, wp_dial);
  dial_listener = wl_proxy_get_listener ((struct wl_proxy *) wp_dial);

  group->current_mode = 2;
  for (unsigned int phase = 0; phase < 8; phase++)
    {
      GdkSurface *expected_surface = NULL;

      switch (phase)
        {
        case 0:
          g_clear_object (&seat->keyboard_focus);
          expected_surface = surface;
          break;
        case 1:
          g_set_object (&seat->keyboard_focus, other_surface);
          expected_surface = surface;
          break;
        case 2:
          pad_listener->leave (pad, wp_pad, 20, gdk_wayland_surface_get_wl_surface (surface));
          g_assert_null (tablet.pads);
          break;
        case 3:
          pad_listener->enter (pad, wp_pad, 21, wp_tablet,
                               gdk_wayland_surface_get_wl_surface (other_surface));
          g_set_object (&seat->keyboard_focus, surface);
          expected_surface = other_surface;
          break;
        case 4:
          g_clear_object (&seat->keyboard_focus);
          expected_surface = other_surface;
          break;
        case 5:
          pad_listener->leave (pad, wp_pad, 22, gdk_wayland_surface_get_wl_surface (other_surface));
          g_assert_null (tablet.pads);
          break;
        case 6:
          pad_listener->enter (pad, wp_pad, 23, wp_tablet,
                               gdk_wayland_surface_get_wl_surface (surface));
          g_set_object (&seat->keyboard_focus, surface);
          expected_surface = surface;
          break;
        case 7:
          pad_listener->leave (pad, wp_pad, 24, gdk_wayland_surface_get_wl_surface (surface));
          g_assert_null (tablet.pads);
          break;
        default:
          g_assert_not_reached ();
        }

      if (phase == 5)
        g_clear_object (&seat->keyboard_focus);

      switch ((unsigned int) type)
        {
        case GDK_PAD_GROUP_MODE:
          group_listener->mode_switch (group, wp_group, 0, 10 + phase, 2);
          g_assert_cmpuint (group->mode_switch_serial, ==, 10 + phase);
          g_assert_cmpuint (group->current_mode, ==, 2);
          break;
        case GDK_PAD_RING:
          ring_listener->angle (group, wp_ring, wl_fixed_from_double (45));
          ring_listener->frame (group, wp_ring, 111);
          break;
        case GDK_PAD_STRIP:
          strip_listener->position (group, wp_strip, 32767);
          strip_listener->frame (group, wp_strip, 111);
          break;
        case GDK_PAD_DIAL:
          dial_listener->delta (group, wp_dial, -120);
          dial_listener->frame (group, wp_dial, 111);
          break;
        case GDK_PAD_BUTTON_PRESS:
        case GDK_PAD_BUTTON_RELEASE:
          pad_listener->button (pad, wp_pad, 111, button,
                                 type == GDK_PAD_BUTTON_PRESS
                                     ? ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED
                                     : ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED);
          break;
        default:
          g_assert_not_reached ();
        }

      event = gdk_display_get_event (display);
      if (expected_surface == NULL)
        g_assert_null (event);
      else
        {
          guint n_group, mode;

          g_assert_nonnull (event);
          g_assert_cmpint (gdk_event_get_event_type (event), ==, type);
          g_assert_true (gdk_event_get_surface (event) == expected_surface);
          g_assert_true (gdk_event_get_device (event) == pad->device);
          gdk_pad_event_get_group_mode (event, &n_group, &mode);
          g_assert_cmpuint (n_group, ==, 0);
          g_assert_cmpuint (mode, ==, 2);
          g_assert_cmpuint (gdk_event_get_time (event), ==, type == GDK_PAD_GROUP_MODE ? 0 : 111);
          if (type == GDK_PAD_BUTTON_PRESS || type == GDK_PAD_BUTTON_RELEASE)
            g_assert_cmpuint (gdk_pad_event_get_button (event), ==, button);
          else if (type != GDK_PAD_GROUP_MODE)
            {
              guint index;
              double value;

              gdk_pad_event_get_axis_value (event, &index, &value);
              g_assert_cmpuint (index, ==, 0);
              g_assert_cmpfloat (value, ==, type == GDK_PAD_RING ? 45 :
                                          type == GDK_PAD_STRIP ? 32767. / 65535 : -120);
            }
          gdk_event_unref (event);
        }
      g_assert_null (gdk_display_get_event (display));
    }

  g_clear_object (&seat->keyboard_focus);
  pad_listener->enter (pad, wp_pad, 25, wp_tablet,
                       gdk_wayland_surface_get_wl_surface (surface));
  weak_surface = surface;
  g_object_add_weak_pointer (G_OBJECT (surface), (gpointer *) &weak_surface);
  gdk_surface_destroy (surface);
  g_assert_nonnull (weak_surface);
  g_list_free (group->buttons);
  g_list_free (group->rings);
  g_list_free (group->strips);
  g_list_free (group->dials);
  g_free (group);
  g_list_free (pad->mode_groups);
  g_list_free (pad->rings);
  g_list_free (pad->strips);
  g_list_free (pad->dials);
  pad_listener->removed (pad, wp_pad);
  g_assert_null (tablet.pads);
  g_assert_null (weak_surface);
  wl_proxy_destroy ((struct wl_proxy *) wp_dial);
  wl_proxy_destroy ((struct wl_proxy *) wp_strip);
  wl_proxy_destroy ((struct wl_proxy *) wp_ring);
  wl_proxy_destroy ((struct wl_proxy *) wp_group);
  wl_proxy_destroy ((struct wl_proxy *) wp_pad);
  wl_proxy_destroy ((struct wl_proxy *) wp_tablet);
  g_clear_object (&seat->keyboard_focus);
  seat->keyboard_focus = saved_focus;
  gdk_surface_destroy (other_surface);
}

int
main (int argc, char **argv)
{
  gtk_test_init (&argc, &argv);
  g_test_add_data_func ("/wayland/pad/mode", GUINT_TO_POINTER (GDK_PAD_GROUP_MODE), test_pad_focus);
  g_test_add_data_func ("/wayland/pad/ring", GUINT_TO_POINTER (GDK_PAD_RING), test_pad_focus);
  g_test_add_data_func ("/wayland/pad/strip", GUINT_TO_POINTER (GDK_PAD_STRIP), test_pad_focus);
  g_test_add_data_func ("/wayland/pad/dial", GUINT_TO_POINTER (GDK_PAD_DIAL), test_pad_focus);
  g_test_add_data_func ("/wayland/pad/button-press", GUINT_TO_POINTER (GDK_PAD_BUTTON_PRESS), test_pad_focus);
  g_test_add_data_func ("/wayland/pad/button-release", GUINT_TO_POINTER (GDK_PAD_BUTTON_RELEASE), test_pad_focus);
  return g_test_run ();
}
