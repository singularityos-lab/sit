#include <gtk/sit.h>
#include "gdk/gdksurfaceprivate.h"
#include <math.h>

typedef struct
{
  GtkWidget parent;
  unsigned measures;
  unsigned allocations;
  unsigned snapshots;
} CounterWidget;

typedef GtkWidgetClass CounterWidgetClass;
G_DEFINE_TYPE (CounterWidget, counter_widget, GTK_TYPE_WIDGET)

static void
counter_measure (GtkWidget      *widget,
                 GtkOrientation orientation,
                 int            for_size,
                 int           *minimum,
                 int           *natural,
                 int           *minimum_baseline,
                 int           *natural_baseline)
{
  CounterWidget *self = (CounterWidget *) widget;

  self->measures++;
  *minimum = *natural = orientation == GTK_ORIENTATION_HORIZONTAL ? 120 : 60;
  *minimum_baseline = *natural_baseline = -1;
}

static void
counter_allocate (GtkWidget *widget,
                  int        width,
                  int        height,
                  int        baseline)
{
  ((CounterWidget *) widget)->allocations++;
}

static void
counter_snapshot (GtkWidget   *widget,
                  GtkSnapshot *snapshot)
{
  GdkRGBA color = { .red = 1, .green = 0, .blue = 0, .alpha = 1 };
  graphene_rect_t rect = GRAPHENE_RECT_INIT (0, 0, gtk_widget_get_width (widget), gtk_widget_get_height (widget));

  ((CounterWidget *) widget)->snapshots++;
  gtk_snapshot_append_color (snapshot, &color, &rect);
}

static void
counter_widget_class_init (CounterWidgetClass *klass)
{
  klass->measure = counter_measure;
  klass->size_allocate = counter_allocate;
  klass->snapshot = counter_snapshot;
}

static void
counter_widget_init (CounterWidget *self)
{
}

static void
retained_transform (void)
{
  GtkWidget *window = gtk_window_new ();
  GtkWidget *fixed = gtk_fixed_new ();
  GtkWidget *widget = g_object_new (counter_widget_get_type (), NULL);
  CounterWidget *counter = (CounterWidget *) widget;
  unsigned measures, allocations, snapshots;

  gtk_window_set_default_size (GTK_WINDOW (window), 440, 260);
  gtk_window_set_child (GTK_WINDOW (window), fixed);
  gtk_fixed_put (GTK_FIXED (fixed), widget, 40, 50);
  gtk_window_present (GTK_WINDOW (window));
  gtk_test_widget_wait_for_draw (window);
  measures = counter->measures;
  allocations = counter->allocations;
  snapshots = counter->snapshots;
  g_assert_cmpuint (snapshots, >, 0);

  for (int i = 0; i < 20; i++)
    {
      graphene_point_t offset = GRAPHENE_POINT_INIT (100 + i, 20);
      GskTransform *transform = gsk_transform_translate (NULL, &offset);
      graphene_point_t center = GRAPHENE_POINT_INIT (60, 30);
      graphene_point_t actual;

      g_assert_true (sit_widget_set_visual_transform (widget, transform));
      g_assert_true (gsk_transform_equal (sit_widget_get_visual_transform (widget), transform));
      gsk_transform_unref (transform);
      gtk_test_widget_wait_for_draw (window);
      g_assert_true (gtk_widget_compute_point (widget, window, &center, &actual));
      g_assert_cmpfloat_with_epsilon (actual.x, 200 + i, .001);
      g_assert_cmpfloat_with_epsilon (actual.y, 100, .001);
      g_assert_true (gtk_widget_pick (window, actual.x, actual.y, GTK_PICK_DEFAULT) == widget);
    }

  g_assert_cmpuint (counter->measures, ==, measures);
  g_assert_cmpuint (counter->allocations, ==, allocations);
  g_assert_cmpuint (counter->snapshots, ==, snapshots);
  g_assert_true (sit_widget_set_visual_transform (widget, NULL));
  gtk_test_widget_wait_for_draw (window);
  g_assert_null (sit_widget_get_visual_transform (widget));
  g_assert_true (gtk_widget_pick (window, 100, 80, GTK_PICK_DEFAULT) == widget);
  g_assert_true (gtk_widget_pick (window, 250, 100, GTK_PICK_DEFAULT) != widget);
  g_assert_false (sit_widget_set_visual_transform (window, NULL));
  gtk_window_destroy (GTK_WINDOW (window));
}

static unsigned
capture_alpha (GdkPaintable *paintable,
               GtkWidget    *window,
               int           x,
               int           y)
{
  GtkSnapshot *snapshot = gtk_snapshot_new ();
  graphene_rect_t viewport = GRAPHENE_RECT_INIT (0, 0, 120, 60);
  GskRenderNode *node;
  GdkTexture *texture;
  guchar pixels[120 * 60 * 4];
  unsigned alpha;

  while (g_main_context_pending (NULL))
    g_main_context_iteration (NULL, FALSE);
  gdk_paintable_snapshot (paintable, GDK_SNAPSHOT (snapshot), 120, 60);
  node = gtk_snapshot_free_to_node (snapshot);
  if (node == NULL)
    return 0;
  texture = gsk_renderer_render_texture (gtk_native_get_renderer (GTK_NATIVE (window)), node, &viewport);
  g_assert_nonnull (texture);
  gdk_texture_download (texture, pixels, 120 * 4);
  alpha = pixels[(y * 120 + x) * 4 + 3];
  g_object_unref (texture);
  gsk_render_node_unref (node);
  return alpha;
}

static void
retained_opacity_clip (void)
{
  GtkWidget *window = gtk_window_new ();
  GtkWidget *fixed = gtk_fixed_new ();
  GtkWidget *widget = g_object_new (counter_widget_get_type (), NULL);
  CounterWidget *counter = (CounterWidget *) widget;
  GdkPaintable *paintable;
  GskRoundedRect clip;
  graphene_rect_t bounds = GRAPHENE_RECT_INIT (20, 10, 60, 40);
  unsigned snapshots, allocations, measures;

  gtk_window_set_default_size (GTK_WINDOW (window), 440, 260);
  gtk_window_set_child (GTK_WINDOW (window), fixed);
  gtk_fixed_put (GTK_FIXED (fixed), widget, 40, 50);
  gtk_window_present (GTK_WINDOW (window));
  gtk_test_widget_wait_for_draw (window);
  paintable = gtk_widget_paintable_new (widget);
  g_assert_cmpuint (capture_alpha (paintable, window, 40, 30), ==, 255);
  g_assert_true (sit_widget_set_visual_opacity (widget, .5));
  gtk_test_widget_wait_for_draw (window);
  snapshots = counter->snapshots;
  allocations = counter->allocations;
  measures = counter->measures;

  for (int i = 0; i < 12; i++)
    {
      double opacity = i == 10 ? 0 : i == 11 ? 1 : i % 2 ? .25 : .75;
      unsigned expected = (unsigned) round (opacity * 255);

      gtk_widget_set_opacity (widget, opacity);
      gsk_rounded_rect_init_from_rect (&clip, &bounds, i % 2 ? 15 : 0);
      g_assert_true (sit_widget_set_visual_clip (widget, &clip));
      gtk_test_widget_wait_for_draw (window);
      g_assert_cmpuint (capture_alpha (paintable, window, 40, 30), ==, expected);
      g_assert_cmpuint (capture_alpha (paintable, window, 5, 5), ==, 0);
      g_assert_cmpuint (capture_alpha (paintable, window, 20, 10), ==, i % 2 ? 0 : expected);
      g_assert_true (gtk_widget_pick (window, 100, 80, GTK_PICK_DEFAULT) == widget);
      g_assert_true (gtk_widget_pick (window, 45, 55, GTK_PICK_DEFAULT) != widget);
      g_assert_cmpint (gtk_widget_contains (widget, 22, 12), ==, i % 2 == 0);
    }

  g_assert_cmpuint (counter->snapshots, ==, snapshots);
  g_assert_cmpuint (counter->allocations, ==, allocations);
  g_assert_cmpuint (counter->measures, ==, measures);
  g_assert_true (sit_widget_set_visual_clip (widget, NULL));
  gtk_widget_set_opacity (widget, 1);
  gtk_test_widget_wait_for_draw (window);
  g_assert_cmpuint (capture_alpha (paintable, window, 5, 5), ==, 255);
  g_assert_false (sit_widget_set_visual_opacity (widget, NAN));
  g_assert_false (sit_widget_set_visual_clip (window, &clip));
  clip.corner[0].width = NAN;
  g_assert_false (sit_widget_set_visual_clip (widget, &clip));
  g_object_unref (paintable);
  gtk_window_destroy (GTK_WINDOW (window));
}

static void
visual_lifecycle (void)
{
  for (int i = 0; i < 16; i++)
    {
      GtkWidget *window = gtk_window_new ();
      GtkWidget *fixed = gtk_fixed_new ();
      GtkWidget *widget = g_object_new (counter_widget_get_type (), NULL);
      graphene_point_t offset = GRAPHENE_POINT_INIT (60, 10);
      GskTransform *transform = gsk_transform_translate (NULL, &offset);
      gpointer weak = widget;

      gtk_window_set_child (GTK_WINDOW (window), fixed);
      gtk_fixed_put (GTK_FIXED (fixed), widget, 40, 50);
      g_object_add_weak_pointer (G_OBJECT (widget), &weak);
      g_object_ref (widget);
      g_assert_true (sit_widget_set_visual_transform (widget, transform));
      g_assert_true (sit_widget_set_visual_opacity (widget, .5));
      gsk_transform_unref (transform);
      gtk_window_present (GTK_WINDOW (window));
      gtk_test_widget_wait_for_draw (window);
      gtk_widget_set_visible (widget, FALSE);
      gtk_widget_set_visible (widget, TRUE);
      gtk_test_widget_wait_for_draw (window);
      gtk_fixed_remove (GTK_FIXED (fixed), widget);
      gtk_fixed_put (GTK_FIXED (fixed), widget, 80, 50);
      gtk_test_widget_wait_for_draw (window);
      g_assert_true (gtk_widget_pick (window, 200, 90, GTK_PICK_DEFAULT) == widget);
      gtk_window_destroy (GTK_WINDOW (window));
      g_object_unref (widget);
      g_assert_null (weak);
    }
}

static void
visual_region (void)
{
  GtkWidget *window = gtk_window_new ();
  GtkWidget *fixed = gtk_fixed_new ();
  GtkWidget *widget = g_object_new (counter_widget_get_type (), NULL);
  graphene_rect_t bounds = GRAPHENE_RECT_INIT (20, 10, 60, 40);
  graphene_rect_t parent_bounds = GRAPHENE_RECT_INIT (55, 65, 45, 20);
  GskRoundedRect clip, parent_clip;
  cairo_region_t *region;
  double offset_x, offset_y;
  unsigned mismatch = 0;

  gtk_window_set_decorated (GTK_WINDOW (window), FALSE);
  gtk_window_set_default_size (GTK_WINDOW (window), 240, 180);
  gtk_window_set_child (GTK_WINDOW (window), fixed);
  gtk_fixed_put (GTK_FIXED (fixed), widget, 40, 50);
  gtk_window_present (GTK_WINDOW (window));
  gtk_test_widget_wait_for_draw (window);
  gsk_rounded_rect_init_from_rect (&clip, &bounds, 12);
  gsk_rounded_rect_init_from_rect (&parent_clip, &parent_bounds, 8);
  g_assert_true (sit_widget_set_visual_clip (widget, &clip));
  g_assert_true (sit_widget_set_visual_clip (fixed, &parent_clip));
  for (int stage = 0; stage < 5; stage++)
    {
      GskTransform *visual = NULL;
      graphene_point_t offset = GRAPHENE_POINT_INIT (20, 5);

      mismatch = 0;
      if (stage == 1)
        visual = gsk_transform_translate (NULL, &offset);
      else if (stage >= 2)
        {
          offset.x = 120;
          offset.y = 0;
          visual = gsk_transform_scale (gsk_transform_translate (NULL, &offset), -1, 1);
        }
      g_assert_true (sit_widget_set_visual_transform (widget, visual));
      gsk_transform_unref (visual);
      if (stage == 2)
        {
          offset.x = 15;
          offset.y = 8;
          visual = gsk_transform_scale (gsk_transform_translate (NULL, &offset), 1.1, .8);
          g_assert_true (sit_widget_set_visual_transform (fixed, visual));
          gsk_transform_unref (visual);
        }
      if (stage == 3)
        g_assert_true (sit_widget_set_visual_clip (fixed, NULL));
      if (stage == 4)
        g_assert_true (sit_widget_set_visual_clip (widget, NULL));
      gtk_test_widget_wait_for_draw (window);
      region = sit_widget_create_border_region (widget, NULL, NULL);
      g_assert_nonnull (region);
      gtk_native_get_surface_transform (GTK_NATIVE (window), &offset_x, &offset_y);

      for (int y = 0; y < 180; y++)
        for (int x = 0; x < 240; x++)
          {
            graphene_point_t surface = GRAPHENE_POINT_INIT (x + .5 - offset_x, y + .5 - offset_y);
            graphene_point_t local, parent;
            gboolean expected;

            g_assert_true (gtk_widget_compute_point (window, widget, &surface, &local));
            g_assert_true (gtk_widget_compute_point (window, fixed, &surface, &parent));
            expected = local.x >= 0 && local.x < 120 && local.y >= 0 && local.y < 60 &&
                       (stage >= 4 || gsk_rounded_rect_contains_point (&clip, &local)) &&
                       (stage >= 3 || gsk_rounded_rect_contains_point (&parent_clip, &parent));
            if (cairo_region_contains_point (region, x, y) != expected)
              mismatch++;
          }

      g_test_message ("Native region differs at %u pixel centers", mismatch);
      g_assert_cmpuint (mismatch, ==, 0);
      cairo_region_destroy (region);
    }
  GskTransform *collapsed = gsk_transform_scale (NULL, 0, 1);
  g_assert_true (sit_widget_set_visual_transform (widget, collapsed));
  gsk_transform_unref (collapsed);
  region = sit_widget_create_border_region (widget, NULL, NULL);
  g_assert_nonnull (region);
  g_assert_true (cairo_region_is_empty (region));
  cairo_region_destroy (region);
  gtk_window_destroy (GTK_WINDOW (window));
}

static void
bound_visual_clip (void)
{
  GtkWidget *window = gtk_window_new ();
  GtkWidget *fixed = gtk_fixed_new ();
  GtkWidget *widget = g_object_new (counter_widget_get_type (), NULL);
  graphene_rect_t bounds = GRAPHENE_RECT_INIT (20, 10, 60, 40);
  GskRoundedRect clip;
  GdkSurface *surface;

  gtk_window_set_decorated (GTK_WINDOW (window), FALSE);
  gtk_window_set_resizable (GTK_WINDOW (window), FALSE);
  gtk_window_set_default_size (GTK_WINDOW (window), 240, 180);
  gtk_window_set_child (GTK_WINDOW (window), fixed);
  gtk_fixed_put (GTK_FIXED (fixed), widget, 40, 50);
  g_assert_true (sit_window_set_border_widget (GTK_WINDOW (window), widget));
  gtk_window_present (GTK_WINDOW (window));
  gtk_test_widget_wait_for_draw (window);
  surface = gtk_native_get_surface (GTK_NATIVE (window));

  for (int stage = 0; stage < 7; stage++)
    {
      cairo_region_t *expected;

      if (stage == 1)
        {
          gsk_rounded_rect_init_from_rect (&clip, &bounds, 12);
          g_assert_true (sit_widget_set_visual_clip (widget, &clip));
        }
      if (stage == 2)
        {
          bounds = GRAPHENE_RECT_INIT (0, 0, 100, 50);
          gsk_rounded_rect_init_from_rect (&clip, &bounds, 20);
          g_assert_true (sit_widget_set_visual_clip (fixed, &clip));
        }
      if (stage == 3)
        {
          g_assert_true (sit_widget_set_visual_clip (fixed, NULL));
          g_assert_true (sit_widget_set_visual_clip (widget, NULL));
        }
      if (stage == 4)
        {
          bounds = GRAPHENE_RECT_INIT (20, 10, 60, 40);
          gsk_rounded_rect_init_from_rect (&clip, &bounds, 12);
          g_assert_true (sit_widget_set_visual_clip (widget, &clip));
        }
      if (stage == 5)
        {
          GskTransform *reflection = gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (gtk_widget_get_width (widget), 0));

          reflection = gsk_transform_scale (reflection, -1, 1);
          g_assert_true (sit_widget_set_visual_transform (widget, reflection));
          gsk_transform_unref (reflection);
        }
      if (stage == 6)
        g_assert_true (sit_widget_set_visual_transform (widget, NULL));
      gtk_test_widget_wait_for_draw (window);
      expected = sit_widget_create_border_region (widget, NULL, NULL);
      g_assert_nonnull (expected);
      g_test_message ("Bound clip stage %d", stage);
      g_assert_true (cairo_region_equal (surface->input_region, expected));
      cairo_region_destroy (expected);
    }

  gtk_window_destroy (GTK_WINDOW (window));
}

int
main (int argc, char **argv)
{
  gtk_test_init (&argc, &argv, NULL);
  g_test_add_func ("/sit/widget/retained-transform", retained_transform);
  g_test_add_func ("/sit/widget/retained-opacity-clip", retained_opacity_clip);
  g_test_add_func ("/sit/widget/visual-lifecycle", visual_lifecycle);
  g_test_add_func ("/sit/widget/visual-region", visual_region);
  g_test_add_func ("/sit/widget/bound-visual-clip", bound_visual_clip);
  return g_test_run ();
}
