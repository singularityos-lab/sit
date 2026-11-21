#include <gtk/gtk.h>

#define GTK_COMPILATION
#include "gdk/gdkeventsprivate.h"

typedef struct {
  GtkWidget *widget;
  int x;
  int y;
  guint state;
  guint pressed : 1;
} PointState;

/* Controlled timestamps for swipe velocity tests. */
static guint32 event_time;
static PointState mouse_state;
static PointState touch_state[10]; /* touchpoint 0 gets pointer emulation,
                                    * use it first in tests for consistency.
                                    */

#define EVENT_SEQUENCE(point) (GdkEventSequence*) ((point) - touch_state + 1)

static void
show_toplevel (GtkWindow *window)
{
  GdkSurface *surface;

  gtk_window_present (window);

  surface = gtk_native_get_surface (GTK_NATIVE (window));

  while (gdk_surface_get_width (surface) <= 1 ||
         gdk_surface_get_height (surface) <= 1)
    g_main_context_iteration (NULL, TRUE);
}

static void
show_popover (GtkPopover *popover)
{
  GdkSurface *surface;

  gtk_popover_popup (popover);

  surface = gtk_native_get_surface (GTK_NATIVE (popover));

  while (gdk_surface_get_width (surface) <= 1 ||
         gdk_surface_get_height (surface) <= 1)
    g_main_context_iteration (NULL, TRUE);
}

static void
inject_event (GdkEvent *event)
{
  gboolean handled;

  if (event_time)
    event->time = event_time;

  g_signal_emit_by_name (event->surface, "event", event, &handled);
}

static void
point_press (PointState *point,
             GtkWidget  *widget,
             guint       button)
{
  GdkDisplay *display;
  GdkDevice *device;
  GdkSeat *seat;
  GdkSurface *surface;
  GdkEvent *ev;

  display = gtk_widget_get_display (widget);
  seat = gdk_display_get_default_seat (display);
  device = gdk_seat_get_pointer (seat);
  surface = gtk_native_get_surface (gtk_widget_get_native (widget));

  if (point == &mouse_state)
    {
      GdkButtonEvent *button_event;

      button_event = (GdkButtonEvent *) g_type_create_instance (GDK_TYPE_BUTTON_EVENT);
      ev = (GdkEvent *) button_event;

      ev->event_type = GDK_BUTTON_PRESS;
      ev->surface = g_object_ref (surface);
      ev->device = g_object_ref (device);
      ev->time = GDK_CURRENT_TIME;

      button_event->button = button;
      button_event->state = point->state;
      button_event->x = point->x;
      button_event->y = point->y;

      point->state |= GDK_BUTTON1_MASK << (button - 1);
    }
  else
    {
      GdkTouchEvent *touch_event;

      touch_event = (GdkTouchEvent *) g_type_create_instance (GDK_TYPE_TOUCH_EVENT);
      ev = (GdkEvent *) touch_event;

      ev->event_type = GDK_TOUCH_BEGIN;
      ev->surface = g_object_ref (surface);
      ev->device = g_object_ref (device);
      ev->time = GDK_CURRENT_TIME;

      touch_event->sequence = EVENT_SEQUENCE (point);
      touch_event->state = point->state;
      touch_event->x = point->x;
      touch_event->y = point->y;
      touch_event->touch_emulating = (point == &touch_state[0]);
    }

  inject_event (ev);

  gdk_event_unref (ev);

  point->widget = widget;
}

static void
point_update (PointState *point,
              GtkWidget  *widget,
              double      x,
              double      y)
{
  GdkDisplay *display;
  GdkDevice *device;
  GdkSeat *seat;
  GdkSurface *surface;
  GdkEvent *ev;

  display = gtk_widget_get_display (widget);
  seat = gdk_display_get_default_seat (display);
  device = gdk_seat_get_pointer (seat);
  surface = gtk_native_get_surface (gtk_widget_get_native (widget));

  point->x = x;
  point->y = y;

  if (point == &mouse_state)
    {
      GdkMotionEvent *motion_event;

      motion_event = (GdkMotionEvent *) g_type_create_instance (GDK_TYPE_MOTION_EVENT);
      ev = (GdkEvent *) motion_event;

      ev->event_type = GDK_MOTION_NOTIFY;
      ev->surface = g_object_ref (surface);
      ev->device = g_object_ref (device);
      ev->time = GDK_CURRENT_TIME;

      motion_event->state = point->state;
      motion_event->x = point->x;
      motion_event->y = point->y;
    }
  else
    {
      GdkTouchEvent *touch_event;

      if (!point->widget || widget != point->widget)
        return;

      touch_event = (GdkTouchEvent *) g_type_create_instance (GDK_TYPE_TOUCH_EVENT);
      ev = (GdkEvent *) touch_event;

      ev->event_type = GDK_TOUCH_UPDATE;
      ev->surface = g_object_ref (surface);
      ev->device = g_object_ref (device);
      ev->time = GDK_CURRENT_TIME;

      touch_event->sequence = EVENT_SEQUENCE (point);
      touch_event->state = point->state;
      touch_event->x = point->x;
      touch_event->y = point->y;
      touch_event->touch_emulating = (point == &touch_state[0]);
    }

  inject_event (ev);

  gdk_event_unref (ev);
}

static void
point_release (PointState *point,
               guint       button)
{
  GdkDisplay *display;
  GdkDevice *device;
  GdkSeat *seat;
  GdkSurface *surface;
  GdkEvent *ev;

  if (point->widget == NULL)
    return;

  display = gtk_widget_get_display (point->widget);
  seat = gdk_display_get_default_seat (display);
  device = gdk_seat_get_pointer (seat);
  surface = gtk_native_get_surface (gtk_widget_get_native (point->widget));

  if (!point->widget)
    return;

  if (point == &mouse_state)
    {
      GdkButtonEvent *button_event;

      if ((point->state & (GDK_BUTTON1_MASK << (button - 1))) == 0)
        return;

      button_event = (GdkButtonEvent *) g_type_create_instance (GDK_TYPE_BUTTON_EVENT);
      ev = (GdkEvent *) button_event;

      ev->event_type = GDK_BUTTON_RELEASE;
      ev->surface = g_object_ref (surface);
      ev->device = g_object_ref (device);
      ev->time = GDK_CURRENT_TIME;

      button_event->button = button;
      button_event->state = point->state;
      button_event->x = point->x;
      button_event->y = point->y;

      point->state &= ~(GDK_BUTTON1_MASK << (button - 1));
    }
  else
    {
      GdkTouchEvent *touch_event;

      touch_event = (GdkTouchEvent *) g_type_create_instance (GDK_TYPE_TOUCH_EVENT);
      ev = (GdkEvent *) touch_event;

      ev->event_type = GDK_TOUCH_END;
      ev->surface = g_object_ref (surface);
      ev->device = g_object_ref (device);
      ev->time = GDK_CURRENT_TIME;

      touch_event->sequence = EVENT_SEQUENCE (point);
      touch_event->state = point->state;
      touch_event->x = point->x;
      touch_event->y = point->y;
      touch_event->touch_emulating = (point == &touch_state[0]);
    }

  inject_event (ev);

  gdk_event_unref (ev);
}

static const char *
phase_nick (GtkPropagationPhase phase)
{
 GTypeClass *class;
 GEnumValue *value;

 class = g_type_class_ref (GTK_TYPE_PROPAGATION_PHASE);
 value = g_enum_get_value ((GEnumClass*)class, phase);
 g_type_class_unref (class);

 return value->value_nick;
}

static const char *
state_nick (GtkEventSequenceState state)
{
 GTypeClass *class;
 GEnumValue *value;

 class = g_type_class_ref (GTK_TYPE_EVENT_SEQUENCE_STATE);
 value = g_enum_get_value ((GEnumClass*)class, state);
 g_type_class_unref (class);

 return value->value_nick;
}

typedef struct {
  GtkEventController *controller;
  GString *str;
  gboolean exit;
} LegacyData;

static gboolean
legacy_cb (GtkEventControllerLegacy *c, GdkEvent *button, gpointer data)
{
  if (gdk_event_get_event_type (button) == GDK_BUTTON_PRESS)
    {
      LegacyData *ld = data;
      GtkWidget *w;

      w = gtk_event_controller_get_widget (ld->controller);

      if (ld->str->len > 0)
        g_string_append (ld->str, ", ");
      g_string_append_printf (ld->str, "legacy %s", gtk_widget_get_name (w));

      return ld->exit;
    }

  return GDK_EVENT_PROPAGATE;
}

typedef struct {
  GString *str;
  GtkEventSequenceState state;
} GestureData;

static void
press_cb (GtkGesture *g, int n_press, double x, double y, gpointer data)
{
  GtkEventController *c = GTK_EVENT_CONTROLLER (g);
  GdkEventSequence *sequence;
  GtkPropagationPhase phase;
  GestureData *gd = data;
  const char *name;

  name = g_object_get_data (G_OBJECT (g), "name");
  phase = gtk_event_controller_get_propagation_phase (c);

  if (gd->str->len > 0)
    g_string_append (gd->str, ", ");
  g_string_append_printf (gd->str, "%s %s", phase_nick (phase), name);

  sequence = gtk_gesture_get_last_updated_sequence (g);

  if (sequence)
    g_string_append_printf (gd->str, " (%x)", GPOINTER_TO_UINT (sequence));

  if (gd->state != GTK_EVENT_SEQUENCE_NONE)
    gtk_gesture_set_state (g, gd->state);
}

static void
cancel_cb (GtkGesture *g, GdkEventSequence *sequence, gpointer data)
{
  GestureData *gd = data;
  const char *name;

  name = g_object_get_data (G_OBJECT (g), "name");

  if (gd->str->len > 0)
    g_string_append (gd->str, ", ");
  g_string_append_printf (gd->str, "%s cancelled", name);
}

static void
begin_cb (GtkGesture *g, GdkEventSequence *sequence, gpointer data)
{
  GestureData *gd = data;
  const char *name;

  name = g_object_get_data (G_OBJECT (g), "name");

  if (gd->str->len > 0)
    g_string_append (gd->str, ", ");
  g_string_append_printf (gd->str, "%s began", name);

  if (gd->state != GTK_EVENT_SEQUENCE_NONE)
    gtk_gesture_set_state (g, gd->state);
}

static void
end_cb (GtkGesture *g, GdkEventSequence *sequence, gpointer data)
{
  GestureData *gd = data;
  const char *name;

  name = g_object_get_data (G_OBJECT (g), "name");

  if (gd->str->len > 0)
    g_string_append (gd->str, ", ");
  g_string_append_printf (gd->str, "%s ended", name);
}

static void
update_cb (GtkGesture *g, GdkEventSequence *sequence, gpointer data)
{
  GestureData *gd = data;
  const char *name;

  name = g_object_get_data (G_OBJECT (g), "name");

  if (gd->str->len > 0)
    g_string_append (gd->str, ", ");
  g_string_append_printf (gd->str, "%s updated", name);
}

static void
state_changed_cb (GtkGesture *g, GdkEventSequence *sequence, GtkEventSequenceState state, gpointer data)
{
  GestureData *gd = data;
  const char *name;

  name = g_object_get_data (G_OBJECT (g), "name");

  if (gd->str->len > 0)
    g_string_append (gd->str, ", ");
  g_string_append_printf (gd->str, "%s state %s", name, state_nick (state));

  if (sequence != NULL)
    g_string_append_printf (gd->str, " (%x)", GPOINTER_TO_UINT (sequence));
}


static GtkGesture *
add_gesture (GtkWidget *w, const char *name, GtkPropagationPhase phase, GString *str, GtkEventSequenceState state)
{
  GtkGesture *g;
  GestureData *data;
  GList *gestures;

  data = g_new (GestureData, 1);
  data->str = str;
  data->state = state;

  g = gtk_gesture_click_new ();
  gtk_gesture_single_set_touch_only (GTK_GESTURE_SINGLE (g), FALSE);
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (g), 1);
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (g), phase);
  gtk_widget_add_controller (w, GTK_EVENT_CONTROLLER (g));

  g_object_set_data (G_OBJECT (g), "name", (gpointer)name);

  gestures = g_object_steal_data (G_OBJECT (w), "gestures");
  gestures = g_list_prepend (gestures, g);
  g_object_set_data_full (G_OBJECT (w), "gestures", (gpointer) gestures,
                          (GDestroyNotify) g_list_free);

  g_signal_connect (g, "pressed", G_CALLBACK (press_cb), data);
  g_signal_connect (g, "cancel", G_CALLBACK (cancel_cb), data);
  g_signal_connect (g, "update", G_CALLBACK (update_cb), data);
  g_signal_connect (g, "sequence-state-changed", G_CALLBACK (state_changed_cb), data);

  return g;
}

static GtkGesture *
add_mt_gesture (GtkWidget *w, const char *name, GtkPropagationPhase phase, GString *str, GtkEventSequenceState state)
{
  GtkGesture *g;
  GestureData *data;
  GList *gestures;

  data = g_new (GestureData, 1);
  data->str = str;
  data->state = state;

  g = gtk_gesture_rotate_new ();
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (g), phase);
  gtk_widget_add_controller (w, GTK_EVENT_CONTROLLER (g));

  g_object_set_data (G_OBJECT (g), "name", (gpointer)name);

  gestures = g_object_get_data (G_OBJECT (w), "gestures");
  gestures = g_list_prepend (gestures, g);
  g_object_set_data_full (G_OBJECT (w), "gestures", (gpointer) gestures,
                          (GDestroyNotify) g_list_free);

  g_signal_connect (g, "begin", G_CALLBACK (begin_cb), data);
  g_signal_connect (g, "update", G_CALLBACK (update_cb), data);
  g_signal_connect (g, "end", G_CALLBACK (end_cb), data);
  g_signal_connect (g, "sequence-state-changed", G_CALLBACK (state_changed_cb), data);

  return g;
}

static void
add_legacy (GtkWidget *w, GString *str, gboolean exit)
{
  LegacyData *data;

  data = g_new (LegacyData, 1);
  data->controller = gtk_event_controller_legacy_new ();
  data->str = str;
  data->exit = exit;

  gtk_event_controller_set_propagation_phase (data->controller, GTK_PHASE_BUBBLE);
  gtk_widget_add_controller (w, data->controller);
  g_signal_connect (data->controller, "event", G_CALLBACK (legacy_cb), data);
}

static void
assert_gesture_state (GtkWidget *w, const char *name, GdkEventSequence *seq, GtkEventSequenceState state)
{
  GList *gestures, *l;

  gestures = g_object_get_data (G_OBJECT (w), "gestures");

  for (l = gestures; l; l = l->next)
    {
      const char *n;

      n = g_object_get_data (l->data, "name");
      if (g_strcmp0 (n, name) != 0)
        continue;

      g_assert_cmpint (gtk_gesture_get_sequence_state (l->data, seq), ==, state);
      return;
    }

  g_assert_not_reached ();
}

static void
test_phases (void)
{
  GtkWidget *A, *B, *C;
  GdkDisplay *display;
  GString *str;
  graphene_rect_t allocation;

  A = gtk_window_new ();
  display = gtk_widget_get_display (A);
  if (gdk_display_get_default_seat (display) == NULL)
    {
      g_test_skip ("Display has no seat");
      return;
    }

  gtk_widget_set_name (A, "A");
  B = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name (B, "B");
  C = gtk_image_new ();
  gtk_widget_set_hexpand (C, TRUE);
  gtk_widget_set_vexpand (C, TRUE);
  gtk_widget_set_name (C, "C");

  gtk_window_set_default_size (GTK_WINDOW (A), 400, 400);
  gtk_window_set_child (GTK_WINDOW (A), B);
  gtk_box_append (GTK_BOX (B), C);

  show_toplevel (GTK_WINDOW (A));

  str = g_string_new ("");

  add_gesture (A, "a1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (B, "b1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (C, "c1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (A, "a2", GTK_PHASE_TARGET, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (B, "b2", GTK_PHASE_TARGET, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (C, "c2", GTK_PHASE_TARGET, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (A, "a3", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (B, "b3", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (C, "c3", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);

  g_assert_true (gtk_widget_compute_bounds (B, B, &allocation));

  point_update (&mouse_state, A,
                allocation.origin.x + allocation.size.width / 2 ,
                allocation.origin.y + allocation.size.height / 2);
  point_press (&mouse_state, A, 1);

  g_assert_cmpstr (str->str, ==,
                   "capture a1, "
                   "capture b1, "
                   "capture c1, "
                   "target c2, "
                   "bubble c3, "
                   "bubble b3, "
                   "bubble a3");

  gtk_window_destroy (GTK_WINDOW (A));

  g_string_free (str, TRUE);
}

static void
test_mixed (void)
{
  GtkWidget *A, *B, *C;
  GdkDisplay *display;
  GString *str;
  graphene_rect_t allocation;

  A = gtk_window_new ();
  display = gtk_widget_get_display (A);
  if (gdk_display_get_default_seat (display) == NULL)
    {
      g_test_skip ("Display has no seat");
      return;
    }

  gtk_widget_set_name (A, "A");
  B = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name (B, "B");
  C = gtk_image_new ();
  gtk_widget_set_hexpand (C, TRUE);
  gtk_widget_set_vexpand (C, TRUE);
  gtk_widget_set_name (C, "C");

  gtk_window_set_default_size (GTK_WINDOW (A), 400, 400);
  gtk_window_set_child (GTK_WINDOW (A), B);
  gtk_box_append (GTK_BOX (B), C);

  show_toplevel (GTK_WINDOW (A));

  str = g_string_new ("");

  add_legacy (A, str, GDK_EVENT_PROPAGATE);
  add_legacy (B, str, GDK_EVENT_PROPAGATE);
  add_legacy (C, str, GDK_EVENT_PROPAGATE);

  add_gesture (A, "a1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (B, "b1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (C, "c1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (A, "a2", GTK_PHASE_TARGET, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (B, "b2", GTK_PHASE_TARGET, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (C, "c2", GTK_PHASE_TARGET, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (A, "a3", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (B, "b3", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (C, "c3", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);

  g_assert_true (gtk_widget_compute_bounds (B, B, &allocation));

  point_update (&mouse_state, A,
                allocation.origin.x + allocation.size.width / 2 ,
                allocation.origin.y + allocation.size.height / 2);
  point_press (&mouse_state, A, 1);

  g_assert_cmpstr (str->str, ==,
                   "capture a1, "
                   "capture b1, "
                   "capture c1, "
                   "target c2, "
                   "bubble c3, "
                   "legacy C, "
                   "bubble b3, "
                   "legacy B, "
                   "bubble a3, "
                   "legacy A");

  gtk_window_destroy (GTK_WINDOW (A));

  g_string_free (str, TRUE);
}

static void
test_early_exit (void)
{
  GtkWidget *A, *B, *C;
  GdkDisplay *display;
  GString *str;
  graphene_rect_t allocation;

  A = gtk_window_new ();
  display = gtk_widget_get_display (A);
  if (gdk_display_get_default_seat (display) == NULL)
    {
      g_test_skip ("Display has no seat");
      return;
    }

  gtk_widget_set_name (A, "A");
  B = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name (B, "B");
  C = gtk_image_new ();
  gtk_widget_set_hexpand (C, TRUE);
  gtk_widget_set_vexpand (C, TRUE);
  gtk_widget_set_name (C, "C");

  gtk_window_set_default_size (GTK_WINDOW (A), 400, 400);
  gtk_window_set_child (GTK_WINDOW (A), B);
  gtk_box_append (GTK_BOX (B), C);

  show_toplevel (GTK_WINDOW (A));

  str = g_string_new ("");

  add_legacy (A, str, GDK_EVENT_PROPAGATE);
  add_legacy (B, str, GDK_EVENT_STOP);
  add_legacy (C, str, GDK_EVENT_PROPAGATE);

  add_gesture (A, "a1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (B, "b1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (C, "c1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (C, "c2", GTK_PHASE_TARGET, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (A, "a3", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (B, "b3", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (C, "c3", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);

  g_assert_true (gtk_widget_compute_bounds (B, B, &allocation));

  point_update (&mouse_state, A,
                allocation.origin.x + allocation.size.width / 2 ,
                allocation.origin.y + allocation.size.height / 2);
  point_press (&mouse_state, A, 1);

  g_assert_cmpstr (str->str, ==,
                   "capture a1, "
                   "capture b1, "
                   "capture c1, "
                   "target c2, "
                   "bubble c3, "
                   "legacy C, "
                   "bubble b3, "
                   "legacy B");

  gtk_window_destroy (GTK_WINDOW (A));

  g_string_free (str, TRUE);
}

static void
test_claim_capture (void)
{
  GtkWidget *A, *B, *C;
  GdkDisplay *display;
  GString *str;
  graphene_rect_t allocation;

  A = gtk_window_new ();
  display = gtk_widget_get_display (A);
  if (gdk_display_get_default_seat (display) == NULL)
    {
      g_test_skip ("Display has no seat");
      return;
    }

  gtk_widget_set_name (A, "A");
  B = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name (B, "B");
  C = gtk_image_new ();
  gtk_widget_set_hexpand (C, TRUE);
  gtk_widget_set_vexpand (C, TRUE);
  gtk_widget_set_name (C, "C");

  gtk_window_set_default_size (GTK_WINDOW (A), 400, 400);
  gtk_window_set_child (GTK_WINDOW (A), B);
  gtk_box_append (GTK_BOX (B), C);

  show_toplevel (GTK_WINDOW (A));

  str = g_string_new ("");

  add_gesture (A, "a1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (B, "b1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (C, "c1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_CLAIMED);
  add_gesture (C, "c2", GTK_PHASE_TARGET, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (A, "a3", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (B, "b3", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (C, "c3", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);

  g_assert_true (gtk_widget_compute_bounds (B, B, &allocation));

  point_update (&mouse_state, A,
                allocation.origin.x + allocation.size.width / 2 ,
                allocation.origin.y + allocation.size.height / 2);
  point_press (&mouse_state, A, 1);

  g_assert_cmpstr (str->str, ==,
                   "capture a1, "
                   "capture b1, "
                   "capture c1, "
                   "b1 cancelled, "
                   "a1 cancelled, "
                   "c1 state claimed");

  assert_gesture_state (A, "a1", NULL, GTK_EVENT_SEQUENCE_NONE);
  assert_gesture_state (B, "b1", NULL, GTK_EVENT_SEQUENCE_NONE);
  assert_gesture_state (C, "c1", NULL, GTK_EVENT_SEQUENCE_CLAIMED);
  assert_gesture_state (C, "c2", NULL, GTK_EVENT_SEQUENCE_NONE);
  assert_gesture_state (C, "c3", NULL, GTK_EVENT_SEQUENCE_NONE);
  assert_gesture_state (B, "b3", NULL, GTK_EVENT_SEQUENCE_NONE);
  assert_gesture_state (A, "a3", NULL, GTK_EVENT_SEQUENCE_NONE);

  gtk_window_destroy (GTK_WINDOW (A));

  g_string_free (str, TRUE);
}

static void
test_claim_target (void)
{
  GtkWidget *A, *B, *C;
  GdkDisplay *display;
  GString *str;
  graphene_rect_t allocation;

  A = gtk_window_new ();
  display = gtk_widget_get_display (A);
  if (gdk_display_get_default_seat (display) == NULL)
    {
      g_test_skip ("Display has no seat");
      return;
    }

  gtk_widget_set_name (A, "A");
  B = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name (B, "B");
  C = gtk_image_new ();
  gtk_widget_set_hexpand (C, TRUE);
  gtk_widget_set_vexpand (C, TRUE);
  gtk_widget_set_name (C, "C");

  gtk_window_set_default_size (GTK_WINDOW (A), 400, 400);
  gtk_window_set_child (GTK_WINDOW (A), B);
  gtk_box_append (GTK_BOX (B), C);

  show_toplevel (GTK_WINDOW (A));

  str = g_string_new ("");

  add_gesture (A, "a1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (B, "b1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (C, "c1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (C, "c2", GTK_PHASE_TARGET, str, GTK_EVENT_SEQUENCE_CLAIMED);
  add_gesture (A, "a3", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (B, "b3", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (C, "c3", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);

  g_assert_true (gtk_widget_compute_bounds (B, B, &allocation));
  point_update (&mouse_state, A,
                allocation.origin.x + allocation.size.width / 2 ,
                allocation.origin.y + allocation.size.height / 2);
  point_press (&mouse_state, A, 1);

  g_assert_cmpstr (str->str, ==,
                   "capture a1, "
                   "capture b1, "
                   "capture c1, "
                   "target c2, "
                   "c1 state denied, "
                   "b1 state denied, "
                   "a1 state denied, "
                   "c2 state claimed");

  assert_gesture_state (A, "a1", NULL, GTK_EVENT_SEQUENCE_DENIED);
  assert_gesture_state (B, "b1", NULL, GTK_EVENT_SEQUENCE_DENIED);
  assert_gesture_state (C, "c1", NULL, GTK_EVENT_SEQUENCE_DENIED);
  assert_gesture_state (C, "c2", NULL, GTK_EVENT_SEQUENCE_CLAIMED);
  assert_gesture_state (C, "c3", NULL, GTK_EVENT_SEQUENCE_NONE);
  assert_gesture_state (B, "b3", NULL, GTK_EVENT_SEQUENCE_NONE);
  assert_gesture_state (A, "a3", NULL, GTK_EVENT_SEQUENCE_NONE);

  gtk_window_destroy (GTK_WINDOW (A));

  g_string_free (str, TRUE);
}

static void
test_claim_bubble (void)
{
  GtkWidget *A, *B, *C;
  GdkDisplay *display;
  GString *str;
  graphene_rect_t allocation;

  A = gtk_window_new ();
  display = gtk_widget_get_display (A);
  if (gdk_display_get_default_seat (display) == NULL)
    {
      g_test_skip ("Display has no seat");
      return;
    }

  gtk_widget_set_name (A, "A");
  B = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name (B, "B");
  C = gtk_image_new ();
  gtk_widget_set_hexpand (C, TRUE);
  gtk_widget_set_vexpand (C, TRUE);
  gtk_widget_set_name (C, "C");

  gtk_window_set_default_size (GTK_WINDOW (A), 400, 400);
  gtk_window_set_child (GTK_WINDOW (A), B);
  gtk_box_append (GTK_BOX (B), C);

  show_toplevel (GTK_WINDOW (A));

  str = g_string_new ("");

  add_gesture (A, "a1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (B, "b1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (C, "c1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (C, "c2", GTK_PHASE_TARGET, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (A, "a3", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (B, "b3", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_CLAIMED);
  add_gesture (C, "c3", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);

  g_assert_true (gtk_widget_compute_bounds (B, B, &allocation));

  point_update (&mouse_state, A,
                allocation.origin.x + allocation.size.width / 2 ,
                allocation.origin.y + allocation.size.height / 2);
  point_press (&mouse_state, A, 1);

  g_assert_cmpstr (str->str, ==,
                   "capture a1, "
                   "capture b1, "
                   "capture c1, "
                   "target c2, "
                   "bubble c3, "
                   "bubble b3, "
                   "b1 state denied, "
                   "c3 state denied, "
                   "c2 state denied, "
                   "c1 state denied, "
                   "a1 state denied, "
                   "b3 state claimed");

  assert_gesture_state (A, "a1", NULL, GTK_EVENT_SEQUENCE_DENIED);
  assert_gesture_state (B, "b1", NULL, GTK_EVENT_SEQUENCE_DENIED);
  assert_gesture_state (C, "c1", NULL, GTK_EVENT_SEQUENCE_DENIED);
  assert_gesture_state (C, "c2", NULL, GTK_EVENT_SEQUENCE_DENIED);
  assert_gesture_state (C, "c3", NULL, GTK_EVENT_SEQUENCE_DENIED);
  assert_gesture_state (B, "b3", NULL, GTK_EVENT_SEQUENCE_CLAIMED);
  assert_gesture_state (A, "a3", NULL, GTK_EVENT_SEQUENCE_NONE);

  gtk_window_destroy (GTK_WINDOW (A));

  g_string_free (str, TRUE);
}

static void
test_early_claim_capture (void)
{
  GtkWidget *A, *B, *C;
  GdkDisplay *display;
  GtkGesture *g;
  GString *str;
  graphene_rect_t allocation;

  A = gtk_window_new ();
  display = gtk_widget_get_display (A);
  if (gdk_display_get_default_seat (display) == NULL)
    {
      g_test_skip ("Display has no seat");
      return;
    }

  gtk_widget_set_name (A, "A");
  B = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name (B, "B");
  C = gtk_image_new ();
  gtk_widget_set_hexpand (C, TRUE);
  gtk_widget_set_vexpand (C, TRUE);
  gtk_widget_set_name (C, "C");

  gtk_window_set_default_size (GTK_WINDOW (A), 400, 400);
  gtk_window_set_child (GTK_WINDOW (A), B);
  gtk_box_append (GTK_BOX (B), C);

  show_toplevel (GTK_WINDOW (A));

  str = g_string_new ("");

  add_gesture (A, "a1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  g = add_gesture (B, "b1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_CLAIMED);
  add_gesture (C, "c1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_CLAIMED);
  add_gesture (C, "c2", GTK_PHASE_TARGET, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (A, "a3", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (B, "b3", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (C, "c3", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);

  g_assert_true (gtk_widget_compute_bounds (B, B, &allocation));

  point_update (&mouse_state, A,
                allocation.origin.x + allocation.size.width / 2 ,
                allocation.origin.y + allocation.size.height / 2);
  point_press (&mouse_state, A, 1);

  g_assert_cmpstr (str->str, ==,
                   "capture a1, "
                   "capture b1, "
                   "a1 cancelled, "
                   "b1 state claimed");

  /* Reset the string */
  g_string_erase (str, 0, str->len);

  gtk_gesture_set_state (g, GTK_EVENT_SEQUENCE_DENIED);

  g_assert_cmpstr (str->str, ==,
                   "capture a1, "
                   "capture c1, "
                   "b1 cancelled, "
                   "a1 cancelled, "
                   "c1 state claimed, "
                   "b1 state denied");

  point_release (&mouse_state, 1);
  gtk_window_destroy (GTK_WINDOW (A));

  g_string_free (str, TRUE);
}

static void
test_late_claim_capture (void)
{
  GtkWidget *A, *B, *C;
  GdkDisplay *display;
  GtkGesture *g;
  GString *str;
  graphene_rect_t allocation;

  A = gtk_window_new ();
  display = gtk_widget_get_display (A);
  if (gdk_display_get_default_seat (display) == NULL)
    {
      g_test_skip ("Display has no seat");
      return;
    }

  gtk_widget_set_name (A, "A");
  B = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name (B, "B");
  C = gtk_image_new ();
  gtk_widget_set_hexpand (C, TRUE);
  gtk_widget_set_vexpand (C, TRUE);
  gtk_widget_set_name (C, "C");

  gtk_window_set_default_size (GTK_WINDOW (A), 400, 400);
  gtk_window_set_child (GTK_WINDOW (A), B);
  gtk_box_append (GTK_BOX (B), C);

  show_toplevel (GTK_WINDOW (A));

  str = g_string_new ("");

  add_gesture (A, "a1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  g = add_gesture (B, "b1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (C, "c1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (C, "c2", GTK_PHASE_TARGET, str, GTK_EVENT_SEQUENCE_CLAIMED);
  add_gesture (A, "a3", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (B, "b3", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (C, "c3", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);

  g_assert_true (gtk_widget_compute_bounds (B, B, &allocation));

  point_update (&mouse_state, A,
                allocation.origin.x + allocation.size.width / 2 ,
                allocation.origin.y + allocation.size.height / 2);
  point_press (&mouse_state, A, 1);

  g_assert_cmpstr (str->str, ==,
                   "capture a1, "
                   "capture b1, "
                   "capture c1, "
                   "target c2, "
                   "c1 state denied, "
                   "b1 state denied, "
                   "a1 state denied, "
                   "c2 state claimed");

  /* Reset the string */
  g_string_erase (str, 0, str->len);

  gtk_gesture_set_state (g, GTK_EVENT_SEQUENCE_CLAIMED);

  g_assert_cmpstr (str->str, ==, "");

  point_release (&mouse_state, 1);
  gtk_window_destroy (GTK_WINDOW (A));

  g_string_free (str, TRUE);
}

static void
test_group (void)
{
  GtkWidget *A, *B, *C;
  GdkDisplay *display;
  GString *str;
  GtkGesture *g1, *g2;
  graphene_rect_t allocation;

  A = gtk_window_new ();
  display = gtk_widget_get_display (A);
  if (gdk_display_get_default_seat (display) == NULL)
    {
      g_test_skip ("Display has no seat");
      return;
    }

  gtk_widget_set_name (A, "A");
  B = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name (B, "B");
  C = gtk_image_new ();
  gtk_widget_set_hexpand (C, TRUE);
  gtk_widget_set_vexpand (C, TRUE);
  gtk_widget_set_name (C, "C");

  gtk_window_set_default_size (GTK_WINDOW (A), 400, 400);
  gtk_window_set_child (GTK_WINDOW (A), B);
  gtk_box_append (GTK_BOX (B), C);

  show_toplevel (GTK_WINDOW (A));

  str = g_string_new ("");

  add_gesture (A, "a1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (B, "b1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (C, "c1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  g1 = add_gesture (C, "c2", GTK_PHASE_TARGET, str, GTK_EVENT_SEQUENCE_NONE);
  g2 = add_gesture (C, "c3", GTK_PHASE_TARGET, str, GTK_EVENT_SEQUENCE_CLAIMED);
  gtk_gesture_group (g1, g2);
  add_gesture (A, "a3", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (B, "b3", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (C, "c4", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);

  g_assert_true (gtk_widget_compute_bounds (B, B, &allocation));

  point_update (&mouse_state, A,
                allocation.origin.x + allocation.size.width / 2 ,
                allocation.origin.y + allocation.size.height / 2);
  point_press (&mouse_state, A, 1);

  g_assert_cmpstr (str->str, ==,
                   "capture a1, "
                   "capture b1, "
                   "capture c1, "
                   "target c3, "
                   "c1 state denied, "
                   "b1 state denied, "
                   "a1 state denied, "
                   "c3 state claimed, "
                   "c2 state claimed, "
                   "target c2");

  gtk_window_destroy (GTK_WINDOW (A));

  g_string_free (str, TRUE);
}

G_GNUC_UNUSED static void
test_gestures_outside_grab (void)
{
  GtkWidget *A, *B, *C, *D, *E;
  GdkDisplay *display;
  GString *str;
  graphene_rect_t allocation;

  A = gtk_window_new ();
  display = gtk_widget_get_display (A);
  if (gdk_display_get_default_seat (display) == NULL)
    {
      g_test_skip ("Display has no seat");
      return;
    }

  gtk_widget_set_name (A, "A");
  B = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name (B, "B");
  C = gtk_image_new ();
  gtk_widget_set_hexpand (C, TRUE);
  gtk_widget_set_vexpand (C, TRUE);
  gtk_widget_set_name (C, "C");

  gtk_window_set_default_size (GTK_WINDOW (A), 400, 400);
  gtk_window_set_child (GTK_WINDOW (A), B);
  gtk_box_append (GTK_BOX (B), C);

  show_toplevel (GTK_WINDOW (A));

  D = gtk_popover_new ();
  gtk_widget_set_name (D, "D");
  gtk_widget_set_parent (D, C);

  E = gtk_image_new ();
  gtk_widget_set_name (E, "E");
  gtk_popover_set_child (GTK_POPOVER (D), E);

  str = g_string_new ("");

  add_gesture (A, "a1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (B, "b1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (C, "c1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (C, "c2", GTK_PHASE_TARGET, str, GTK_EVENT_SEQUENCE_CLAIMED);
  add_gesture (B, "b2", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (A, "a2", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);

  g_assert_true (gtk_widget_compute_bounds (B, B, &allocation));

  point_update (&mouse_state, A,
                allocation.origin.x + allocation.size.width / 2 ,
                allocation.origin.y + allocation.size.height / 2);
  point_press (&mouse_state, A, 1);

  g_assert_cmpstr (str->str, ==,
                   "capture a1, "
                   "capture b1, "
                   "capture c1, "
                   "target c2, "
                   "c1 state denied, "
                   "c2 state claimed");

  /* Set a grab on another window */
  g_string_erase (str, 0, str->len);
  show_popover (GTK_POPOVER (D));

  g_assert_cmpstr (str->str, ==,
                   "c1 cancelled, "
                   "c2 cancelled, "
                   "b1 cancelled, "
                   "a1 cancelled");

  gtk_window_destroy (GTK_WINDOW (A));

  g_string_free (str, TRUE);
}

G_GNUC_UNUSED static void
test_gestures_inside_grab (void)
{
  GtkWidget *A, *B, *C;
  GdkDisplay *display;
  GString *str;
  graphene_rect_t allocation;

  A = gtk_window_new ();
  display = gtk_widget_get_display (A);
  if (gdk_display_get_default_seat (display) == NULL)
    {
      g_test_skip ("Display has no seat");
      return;
    }

  gtk_widget_set_name (A, "A");
  B = gtk_popover_new ();
  gtk_widget_set_name (B, "B");
  gtk_widget_set_parent (B, A);
  C = gtk_image_new ();
  gtk_widget_set_hexpand (C, TRUE);
  gtk_widget_set_vexpand (C, TRUE);
  gtk_widget_set_name (C, "C");
  gtk_popover_set_child (GTK_POPOVER (B), C);

  gtk_window_set_default_size (GTK_WINDOW (A), 400, 400);

  show_toplevel (GTK_WINDOW (A));
  show_popover (GTK_POPOVER (B));

  str = g_string_new ("");

  add_gesture (A, "a1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (B, "b1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (C, "c1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (C, "c2", GTK_PHASE_TARGET, str, GTK_EVENT_SEQUENCE_CLAIMED);
  add_gesture (B, "b2", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (A, "a2", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_NONE);

  g_assert_true (gtk_widget_compute_bounds (B, B, &allocation));

  point_update (&mouse_state, A,
                allocation.origin.x + allocation.size.width / 2 ,
                allocation.origin.y + allocation.size.height / 2);
  point_press (&mouse_state, A, 1);

  g_assert_cmpstr (str->str, ==,
                   "capture a1, "
                   "capture b1, "
                   "capture c1, "
                   "target c2, "
                   "c2 state claimed");

  g_string_erase (str, 0, str->len);
  g_assert_cmpstr (str->str, ==,
                   "a1 cancelled");

  /* Update with the grab under effect */
  g_string_erase (str, 0, str->len);
  point_update (&mouse_state, A, allocation.origin.x, allocation.origin.y);
  g_assert_cmpstr (str->str, ==,
                   "b1 updated, "
                   "c1 updated, "
                   "c2 updated");

  gtk_window_destroy (GTK_WINDOW (A));

  g_string_free (str, TRUE);
}

G_GNUC_UNUSED static void
test_multitouch_on_single (void)
{
  GtkWidget *A, *B, *C;
  GdkDisplay *display;
  GString *str;
  graphene_rect_t allocation;

  A = gtk_window_new ();
  display = gtk_widget_get_display (A);
  if (gdk_display_get_default_seat (display) == NULL)
    {
      g_test_skip ("Display has no seat");
      return;
    }

  gtk_widget_set_name (A, "A");
  B = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name (B, "B");
  C = gtk_image_new ();
  gtk_widget_set_hexpand (C, TRUE);
  gtk_widget_set_vexpand (C, TRUE);
  gtk_widget_set_name (C, "C");

  gtk_window_set_default_size (GTK_WINDOW (A), 400, 400);
  gtk_window_set_child (GTK_WINDOW (A), B);
  gtk_box_append (GTK_BOX (B), C);

  show_toplevel (GTK_WINDOW (A));

  str = g_string_new ("");

  add_gesture (A, "a1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_NONE);
  add_gesture (B, "b1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_CLAIMED);

  g_assert_true (gtk_widget_compute_bounds (B, B, &allocation));

  /* First touch down */
  point_update (&touch_state[0], A,
                allocation.origin.x + allocation.size.width / 2 ,
                allocation.origin.y + allocation.size.height / 2);
  point_press (&touch_state[0], A, 1);

  g_assert_cmpstr (str->str, ==,
                   "capture a1 (1), "
                   "capture b1 (1), "
                   "a1 cancelled, "
                   "b1 state claimed (1)");

  /* Second touch down */
  g_string_erase (str, 0, str->len);
  point_update (&touch_state[1], A,
                allocation.origin.x + allocation.size.width / 2 ,
                allocation.origin.y + allocation.size.height / 2);
  point_press (&touch_state[1], A, 1);

  g_assert_cmpstr (str->str, ==,
                   "capture a1 (2), "
                   "b1 state denied (2)");

  gtk_window_destroy (GTK_WINDOW (A));

  g_string_free (str, TRUE);
}

G_GNUC_UNUSED static void
test_multitouch_activation (void)
{
  GtkWidget *A, *B, *C;
  GdkDisplay *display;
  GString *str;
  graphene_rect_t allocation;

  A = gtk_window_new ();
  display = gtk_widget_get_display (A);
  if (gdk_display_get_default_seat (display) == NULL)
    {
      g_test_skip ("Display has no seat");
      return;
    }

  gtk_widget_set_name (A, "A");
  B = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name (B, "B");
  C = gtk_image_new ();
  gtk_widget_set_hexpand (C, TRUE);
  gtk_widget_set_vexpand (C, TRUE);
  gtk_widget_set_name (C, "C");

  gtk_window_set_default_size (GTK_WINDOW (A), 400, 400);
  gtk_window_set_child (GTK_WINDOW (A), B);
  gtk_box_append (GTK_BOX (B), C);

  show_toplevel (GTK_WINDOW (A));

  str = g_string_new ("");

  add_mt_gesture (C, "c1", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_CLAIMED);
  g_assert_true (gtk_widget_compute_bounds (B, B, &allocation));

  /* First touch down */
  point_update (&touch_state[0], A,
                allocation.origin.x + allocation.size.width / 2 ,
                allocation.origin.y + allocation.size.height / 2);
  point_press (&touch_state[0], A, 1);

  g_assert_cmpstr (str->str, ==, "");

  /* Second touch down */
  point_update (&touch_state[1], A,
                allocation.origin.x + allocation.size.width / 2 ,
                allocation.origin.y + allocation.size.height / 2);
  point_press (&touch_state[1], A, 1);

  g_assert_cmpstr (str->str, ==,
                   "c1 began, "
                   "c1 state claimed (2), "
                   "c1 state claimed (1)");

  /* First touch up */
  g_string_erase (str, 0, str->len);
  point_release (&touch_state[0], 1);

  g_assert_cmpstr (str->str, ==,
                   "c1 ended");

  /* A third touch down triggering again action */
  g_string_erase (str, 0, str->len);
  point_update (&touch_state[2], A,
                allocation.origin.x + allocation.size.width / 2 ,
                allocation.origin.y + allocation.size.height / 2);
  point_press (&touch_state[2], A, 1);

  g_assert_cmpstr (str->str, ==,
                   "c1 began, "
                   "c1 state claimed (3)");

  /* One touch up, gesture is finished again */
  g_string_erase (str, 0, str->len);
  point_release (&touch_state[2], 1);

  g_assert_cmpstr (str->str, ==,
                   "c1 ended");

  /* Another touch up, gesture remains inactive */
  g_string_erase (str, 0, str->len);
  point_release (&touch_state[1], 1);

  g_assert_cmpstr (str->str, ==, "");

  gtk_window_destroy (GTK_WINDOW (A));

  g_string_free (str, TRUE);
}

G_GNUC_UNUSED static void
test_multitouch_interaction (void)
{
  GtkWidget *A, *B, *C;
  GdkDisplay *display;
  GtkGesture *g;
  GString *str;
  graphene_rect_t allocation;

  A = gtk_window_new ();
  display = gtk_widget_get_display (A);
  if (gdk_display_get_default_seat (display) == NULL)
    {
      g_test_skip ("Display has no seat");
      return;
    }

  gtk_widget_set_name (A, "A");
  B = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name (B, "B");
  C = gtk_image_new ();
  gtk_widget_set_hexpand (C, TRUE);
  gtk_widget_set_vexpand (C, TRUE);
  gtk_widget_set_name (C, "C");

  gtk_window_set_default_size (GTK_WINDOW (A), 400, 400);
  gtk_window_set_child (GTK_WINDOW (A), B);
  gtk_box_append (GTK_BOX (B), C);

  show_toplevel (GTK_WINDOW (A));

  str = g_string_new ("");

  g = add_gesture (A, "a1", GTK_PHASE_CAPTURE, str, GTK_EVENT_SEQUENCE_CLAIMED);
  add_mt_gesture (C, "c1", GTK_PHASE_BUBBLE, str, GTK_EVENT_SEQUENCE_CLAIMED);
  g_assert_true (gtk_widget_compute_bounds (B, B, &allocation));

  /* First touch down, a1 claims the sequence */
  point_update (&touch_state[0], A,
                allocation.origin.x + allocation.size.width / 2 ,
                allocation.origin.y + allocation.size.height / 2);
  point_press (&touch_state[0], A, 1);

  g_assert_cmpstr (str->str, ==,
                   "capture a1 (1), "
                   "a1 state claimed (1)");

  /* Second touch down, a1 denies and c1 takes over */
  g_string_erase (str, 0, str->len);
  point_update (&touch_state[1], A,
                allocation.origin.x + allocation.size.width / 2 ,
                allocation.origin.y + allocation.size.height / 2);
  point_press (&touch_state[1], A, 1);

  /* Denying sequences in touch-excess situation is a responsibility of the caller */
  gtk_gesture_set_state (g, GTK_EVENT_SEQUENCE_DENIED);

  g_assert_cmpstr (str->str, ==,
                   "a1 state denied (2), "
                   "c1 began, "
                   "c1 state claimed (1), "
                   "c1 state claimed (2), "
                   "a1 state denied (1)");

  /* Move first point, only c1 should update */
  g_string_erase (str, 0, str->len);
  point_update (&touch_state[0], A, allocation.origin.x, allocation.origin.y);

  g_assert_cmpstr (str->str, ==,
                   "c1 updated");

  /* First touch up */
  g_string_erase (str, 0, str->len);
  point_release (&touch_state[0], 1);

  g_assert_cmpstr (str->str, ==,
                   "c1 ended");

  /* A third touch down triggering again action on c1 */
  g_string_erase (str, 0, str->len);
  point_update (&touch_state[2], A,
                allocation.origin.x + allocation.size.width / 2 ,
                allocation.origin.y + allocation.size.height / 2);
  point_press (&touch_state[2], A, 1);

  g_assert_cmpstr (str->str, ==,
                   "a1 state denied (3), "
                   "c1 began, "
                   "c1 state claimed (3)");

  /* One touch up, gesture is finished again */
  g_string_erase (str, 0, str->len);
  point_release (&touch_state[2], 1);

  g_assert_cmpstr (str->str, ==,
                   "c1 ended");

  /* Another touch up, gesture remains inactive */
  g_string_erase (str, 0, str->len);
  point_release (&touch_state[1], 1);

  g_assert_cmpstr (str->str, ==, "");

  gtk_window_destroy (GTK_WINDOW (A));

  g_string_free (str, TRUE);
}

typedef struct {
  guint count;
  double x;
  double y;
} SwipeData;

static void
swipe_cb (GtkGestureSwipe *gesture,
          double           velocity_x,
          double           velocity_y,
          SwipeData       *data)
{
  data->count++;
  data->x = velocity_x;
  data->y = velocity_y;
}

static void
test_swipe_cancel (gconstpointer data)
{
  guint mode = GPOINTER_TO_UINT (data);
  GtkWidget *window = gtk_window_new ();
  GtkGesture *swipe = gtk_gesture_swipe_new ();
  GtkGesture *drag = gtk_gesture_drag_new ();
  PointState *point = mode & 2 ? &mouse_state : &touch_state[0];
  SwipeData result = { 0 };

  *point = (PointState) { 0 };
  gtk_window_set_default_size (GTK_WINDOW (window), 240, 200);
  gtk_widget_add_controller (window, GTK_EVENT_CONTROLLER (swipe));
  gtk_widget_add_controller (window, GTK_EVENT_CONTROLLER (drag));
  if (mode & 1)
    gtk_gesture_group (swipe, drag);
  g_signal_connect (swipe, "swipe", G_CALLBACK (swipe_cb), &result);
  show_toplevel (GTK_WINDOW (window));
  g_test_message ("renderer: %s", G_OBJECT_TYPE_NAME (gtk_native_get_renderer (GTK_NATIVE (window))));

  event_time = 1000;
  point_update (point, window, 80, 80);
  point_press (point, window, 1);
  event_time += 16;
  point_update (point, window, 120, 80);
  event_time += 16;
  point_update (point, window, 160, 80);
  gtk_gesture_set_state (drag, mode & 1 ? GTK_EVENT_SEQUENCE_DENIED : GTK_EVENT_SEQUENCE_CLAIMED);
  g_test_message ("cancelled swipe signals: %u", result.count);
  g_assert_cmpuint (result.count, ==, 0);
  event_time += 16;
  point_release (point, 1);
  g_assert_cmpuint (result.count, ==, 0);

  event_time += 16;
  point_update (point, window, 80, 80);
  point_press (point, window, 1);
  event_time += 16;
  point_release (point, 1);
  g_test_message ("tap swipe signals: %u, velocity: %g %g", result.count, result.x, result.y);
  g_assert_cmpuint (result.count, ==, 1);
  g_assert_cmpfloat (result.x, ==, 0);
  g_assert_cmpfloat (result.y, ==, 0);

  event_time += 200;
  point_press (point, window, 1);
  event_time += 16;
  point_update (point, window, 120, 80);
  event_time += 16;
  point_update (point, window, 160, 80);
  event_time += 16;
  point_release (point, 1);
  g_test_message ("normal swipe signals: %u, velocity: %g %g", result.count, result.x, result.y);
  g_assert_cmpuint (result.count, ==, 2);
  g_assert_cmpfloat (result.x, >, 0);
  g_assert_cmpfloat (result.y, ==, 0);

  event_time = 0;
  *point = (PointState) { 0 };
  gtk_window_destroy (GTK_WINDOW (window));
}

typedef struct {
  guint secondary_pressed;
  guint secondary_released;
  guint primary_released;
} HoldData;

static void
hold_pressed_cb (GtkGestureClick *gesture, int n_press, double x, double y, HoldData *data)
{
  data->secondary_pressed++;
}

static void
hold_released_cb (GtkGestureClick *gesture, int n_press, double x, double y, HoldData *data)
{
  data->secondary_released++;
}

static void
tap_released_cb (GtkGestureClick *gesture, int n_press, double x, double y, HoldData *data)
{
  data->primary_released++;
}

static gboolean
wait_done_cb (gpointer user_data)
{
  *(gboolean *) user_data = TRUE;
  return G_SOURCE_REMOVE;
}

static void
wait_ms (guint ms)
{
  gboolean done = FALSE;

  g_timeout_add (ms, wait_done_cb, &done);
  while (!done)
    g_main_context_iteration (NULL, TRUE);
}

static void
test_touch_hold_secondary (void)
{
  GtkWidget *window = gtk_window_new ();
  GtkGesture *menu = gtk_gesture_click_new ();
  GtkGesture *tap = gtk_gesture_click_new ();
  PointState *point = &touch_state[0];
  HoldData data = { 0 };
  guint long_press_time;

  g_object_get (gtk_widget_get_settings (window), "gtk-long-press-time", &long_press_time, NULL);
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (menu), GDK_BUTTON_SECONDARY);
  gtk_window_set_default_size (GTK_WINDOW (window), 240, 200);
  gtk_widget_add_controller (window, GTK_EVENT_CONTROLLER (menu));
  gtk_widget_add_controller (window, GTK_EVENT_CONTROLLER (tap));
  g_signal_connect (menu, "pressed", G_CALLBACK (hold_pressed_cb), &data);
  g_signal_connect (menu, "released", G_CALLBACK (hold_released_cb), &data);
  g_signal_connect (tap, "released", G_CALLBACK (tap_released_cb), &data);
  show_toplevel (GTK_WINDOW (window));

  *point = (PointState) { 0 };
  point_update (point, window, 80, 80);
  point_press (point, window, 1);
  wait_ms (long_press_time / 2);
  g_assert_cmpuint (data.secondary_pressed, ==, 0);
  point_release (point, 1);
  g_assert_cmpuint (data.secondary_pressed, ==, 0);
  g_assert_cmpuint (data.primary_released, ==, 1);

  wait_ms (long_press_time);
  point_update (point, window, 80, 80);
  point_press (point, window, 1);
  wait_ms (long_press_time + 100);
  g_assert_cmpuint (data.secondary_pressed, ==, 1);
  point_release (point, 1);
  g_assert_cmpuint (data.secondary_released, ==, 1);
  g_assert_cmpuint (data.primary_released, ==, 1);

  wait_ms (long_press_time);
  point_update (point, window, 80, 80);
  point_press (point, window, 1);
  point_update (point, window, 140, 80);
  wait_ms (long_press_time + 100);
  g_assert_cmpuint (data.secondary_pressed, ==, 1);
  point_release (point, 1);

  wait_ms (long_press_time);
  point_update (&mouse_state, window, 80, 80);
  point_press (&mouse_state, window, GDK_BUTTON_SECONDARY);
  g_assert_cmpuint (data.secondary_pressed, ==, 2);
  point_release (&mouse_state, GDK_BUTTON_SECONDARY);
  g_assert_cmpuint (data.secondary_released, ==, 2);

  *point = (PointState) { 0 };
  mouse_state = (PointState) { 0 };
  gtk_window_destroy (GTK_WINDOW (window));
}

typedef struct {
  GtkWidget *popover;
  guint first_clicked;
  guint second_clicked;
} SlideData;

static void
slide_first_cb (GtkButton *button, SlideData *data)
{
  data->first_clicked++;
}

static void
slide_second_cb (GtkButton *button, SlideData *data)
{
  data->second_clicked++;
}

static void
slide_open_cb (GtkGestureClick *gesture, int n_press, double x, double y, SlideData *data)
{
  GdkRectangle rect = { (int) x, (int) y, 1, 1 };

  gtk_popover_set_pointing_to (GTK_POPOVER (data->popover), &rect);
  gtk_popover_popup (GTK_POPOVER (data->popover));
}

static void
wait_popover_shown (GtkWidget *popover)
{
  GdkSurface *surface;
  int i;

  for (i = 0; i < 400; i++)
    {
      surface = gtk_native_get_surface (GTK_NATIVE (popover));
      if (gtk_widget_get_mapped (popover) && surface != NULL &&
          gdk_surface_get_width (surface) > 1 && gdk_surface_get_height (surface) > 1)
        break;
      wait_ms (5);
    }
  wait_ms (50);
}

static void
popover_point_in_parent (GtkWidget *popover,
                         GtkWidget *target,
                         double    *x,
                         double    *y)
{
  graphene_point_t center, local;
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (popover));
  double tx, ty;

  center = GRAPHENE_POINT_INIT (gtk_widget_get_width (target) / 2.0, gtk_widget_get_height (target) / 2.0);
  g_assert_true (gtk_widget_compute_point (target, popover, &center, &local));
  gtk_native_get_surface_transform (GTK_NATIVE (popover), &tx, &ty);
  *x = gdk_popup_get_position_x (GDK_POPUP (surface)) + tx + local.x;
  *y = gdk_popup_get_position_y (GDK_POPUP (surface)) + ty + local.y;
}

static void
test_touch_hold_slide (void)
{
  GtkWidget *window = gtk_window_new ();
  GtkGesture *menu = gtk_gesture_click_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *first = gtk_button_new_with_label ("First");
  GtkWidget *second = gtk_button_new_with_label ("Second");
  PointState *point = &touch_state[0];
  SlideData data = { 0 };
  guint long_press_time;
  double x, y;

  g_object_get (gtk_widget_get_settings (window), "gtk-long-press-time", &long_press_time, NULL);
  gtk_window_set_default_size (GTK_WINDOW (window), 320, 320);
  data.popover = gtk_popover_new ();
  gtk_popover_set_has_arrow (GTK_POPOVER (data.popover), FALSE);
  gtk_popover_set_position (GTK_POPOVER (data.popover), GTK_POS_BOTTOM);
  gtk_widget_set_size_request (first, 160, 48);
  gtk_widget_set_size_request (second, 160, 48);
  gtk_box_append (GTK_BOX (box), first);
  gtk_box_append (GTK_BOX (box), second);
  gtk_popover_set_child (GTK_POPOVER (data.popover), box);
  gtk_widget_set_parent (data.popover, window);
  g_signal_connect (first, "clicked", G_CALLBACK (slide_first_cb), &data);
  g_signal_connect (second, "clicked", G_CALLBACK (slide_second_cb), &data);
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (menu), GDK_BUTTON_SECONDARY);
  gtk_widget_add_controller (window, GTK_EVENT_CONTROLLER (menu));
  g_signal_connect (menu, "pressed", G_CALLBACK (slide_open_cb), &data);
  show_toplevel (GTK_WINDOW (window));

  *point = (PointState) { 0 };
  point_update (point, window, 60, 40);
  point_press (point, window, 1);
  wait_ms (long_press_time + 100);
  wait_popover_shown (data.popover);
  g_assert_true (gtk_widget_get_mapped (data.popover));
  point_release (point, 1);
  wait_ms (400);
  g_assert_cmpuint (data.first_clicked, ==, 0);
  g_assert_cmpuint (data.second_clicked, ==, 0);
  g_assert_true (gtk_widget_get_visible (data.popover));
  gtk_popover_popdown (GTK_POPOVER (data.popover));
  wait_ms (50);

  *point = (PointState) { 0 };
  point_update (point, window, 60, 40);
  point_press (point, window, 1);
  wait_ms (long_press_time + 100);
  wait_popover_shown (data.popover);
  popover_point_in_parent (data.popover, second, &x, &y);
  point_update (point, window, x, y);
  g_assert_true (gtk_widget_get_state_flags (second) & GTK_STATE_FLAG_PRELIGHT);
  g_assert_false (gtk_widget_get_state_flags (first) & GTK_STATE_FLAG_PRELIGHT);
  point_release (point, 1);
  wait_ms (400);
  g_assert_cmpuint (data.first_clicked, ==, 0);
  g_assert_cmpuint (data.second_clicked, ==, 1);
  g_assert_false (gtk_widget_get_state_flags (second) & GTK_STATE_FLAG_PRELIGHT);

  *point = (PointState) { 0 };
  gtk_widget_unparent (data.popover);
  gtk_window_destroy (GTK_WINDOW (window));
}

static void
count_clicked_cb (GtkButton *button, guint *count)
{
  (*count)++;
}

static void
button_center (GtkWidget *button, double *x, double *y)
{
  GtkNative *native = gtk_widget_get_native (button);
  graphene_point_t p;
  double nx, ny;

  g_assert_true (gtk_widget_compute_point (button, GTK_WIDGET (native),
                                           &GRAPHENE_POINT_INIT (gtk_widget_get_width (button) / 2.0,
                                                                 gtk_widget_get_height (button) / 2.0),
                                           &p));
  gtk_native_get_surface_transform (native, &nx, &ny);
  *x = p.x + nx;
  *y = p.y + ny;
}

static void
test_rolling_touch_buttons (gconstpointer data)
{
  gboolean with_capture = GPOINTER_TO_UINT (data) & 1;
  gboolean release_second_first = GPOINTER_TO_UINT (data) & 2;
  GtkWidget *window = gtk_window_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *a = gtk_button_new_with_label ("a");
  GtkWidget *b = gtk_button_new_with_label ("b");
  guint a_count = 0, b_count = 0;
  double ax, ay, bx, by;

  gtk_widget_set_size_request (a, 80, 60);
  gtk_widget_set_size_request (b, 80, 60);
  gtk_box_append (GTK_BOX (box), a);
  gtk_box_append (GTK_BOX (box), b);
  if (with_capture)
    {
      GtkGesture *haptic = gtk_gesture_click_new ();
      gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (haptic), GTK_PHASE_CAPTURE);
      gtk_widget_add_controller (box, GTK_EVENT_CONTROLLER (haptic));
    }
  gtk_window_set_child (GTK_WINDOW (window), box);
  g_signal_connect (a, "clicked", G_CALLBACK (count_clicked_cb), &a_count);
  g_signal_connect (b, "clicked", G_CALLBACK (count_clicked_cb), &b_count);
  show_toplevel (GTK_WINDOW (window));
  while (gtk_widget_get_width (b) <= 0)
    g_main_context_iteration (NULL, TRUE);
  button_center (a, &ax, &ay);
  button_center (b, &bx, &by);

  touch_state[0] = (PointState) { 0 };
  touch_state[1] = (PointState) { 0 };
  touch_state[0].x = ax; touch_state[0].y = ay;
  touch_state[1].x = bx; touch_state[1].y = by;
  point_press (&touch_state[0], window, 1);
  point_press (&touch_state[1], window, 1);
  if (release_second_first)
    {
      point_release (&touch_state[1], 1);
      point_release (&touch_state[0], 1);
    }
  else
    {
      point_release (&touch_state[0], 1);
      point_release (&touch_state[1], 1);
    }
  g_test_message ("a clicked %u, b clicked %u", a_count, b_count);
  g_assert_cmpuint (a_count, ==, 1);
  g_assert_cmpuint (b_count, ==, 1);

  touch_state[0] = (PointState) { 0 };
  touch_state[1] = (PointState) { 0 };
  gtk_window_destroy (GTK_WINDOW (window));
}

int
main (int argc, char *argv[])
{
  gtk_test_init (&argc, &argv);

  g_test_add_func ("/gestures/propagation/phases", test_phases);
  g_test_add_func ("/gestures/propagation/mixed", test_mixed);
  g_test_add_func ("/gestures/propagation/early-exit", test_early_exit);
  g_test_add_func ("/gestures/claim/capture", test_claim_capture);
  g_test_add_func ("/gestures/claim/target", test_claim_target);
  g_test_add_func ("/gestures/claim/bubble", test_claim_bubble);
  g_test_add_func ("/gestures/claim/early-capture", test_early_claim_capture);
  g_test_add_func ("/gestures/claim/late-capture", test_late_claim_capture);
  g_test_add_func ("/gestures/group", test_group);
  g_test_add_data_func ("/gestures/swipe/touch/grouped", GUINT_TO_POINTER (1), test_swipe_cancel);
  g_test_add_data_func ("/gestures/swipe/touch/claimed", GUINT_TO_POINTER (0), test_swipe_cancel);
  g_test_add_data_func ("/gestures/swipe/pointer/grouped", GUINT_TO_POINTER (3), test_swipe_cancel);
  g_test_add_data_func ("/gestures/swipe/pointer/claimed", GUINT_TO_POINTER (2), test_swipe_cancel);
  g_test_add_func ("/gestures/click/touch-hold-secondary", test_touch_hold_secondary);
  g_test_add_func ("/gestures/click/touch-hold-slide", test_touch_hold_slide);
  g_test_add_data_func ("/gestures/click/rolling-touch/plain", GUINT_TO_POINTER (0), test_rolling_touch_buttons);
  g_test_add_data_func ("/gestures/click/rolling-touch/capture", GUINT_TO_POINTER (1), test_rolling_touch_buttons);
  g_test_add_data_func ("/gestures/click/rolling-touch/plain-reversed", GUINT_TO_POINTER (2), test_rolling_touch_buttons);
  g_test_add_data_func ("/gestures/click/rolling-touch/capture-reversed", GUINT_TO_POINTER (3), test_rolling_touch_buttons);
#if 0
  g_test_add_func ("/gestures/grabs/gestures-outside-grab", test_gestures_outside_grab);
  g_test_add_func ("/gestures/grabs/gestures-inside-grab", test_gestures_inside_grab);
  g_test_add_func ("/gestures/multitouch/gesture-single", test_multitouch_on_single);
  g_test_add_func ("/gestures/multitouch/multitouch-activation", test_multitouch_activation);
  g_test_add_func ("/gestures/multitouch/interaction", test_multitouch_interaction);
#endif

  return g_test_run ();
}
