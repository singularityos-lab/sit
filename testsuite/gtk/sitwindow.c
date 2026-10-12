#include <gtk/sit.h>
#include <math.h>
#include "gdk/gdksurfaceprivate.h"

static GtkWidget *
new_window (GtkWidget *child)
{
  GtkWidget *window = gtk_window_new ();
  GtkWidget *titlebar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_visible (titlebar, FALSE);
  gtk_window_set_titlebar (GTK_WINDOW (window), titlebar);
  gtk_window_set_default_size (GTK_WINDOW (window), 360, 260);
  gtk_window_set_child (GTK_WINDOW (window), child);
  return window;
}

static void
binding_lifecycle (void)
{
  for (int i = 0; i < 32; i++)
    {
      GtkWidget *frame = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
      GtkWidget *window = new_window (frame);
      GtkWidget *other = gtk_label_new ("other");
      GtkWidget *second = new_window (other);
      gpointer weak = frame;

      g_object_add_weak_pointer (G_OBJECT (frame), &weak);
      g_assert_false (sit_window_set_border_widget (GTK_WINDOW (window), window));
      g_assert_false (sit_window_set_border_widget (GTK_WINDOW (window), other));
      g_assert_true (sit_window_set_border_widget (GTK_WINDOW (window), frame));
      g_assert_true (sit_window_set_border_widget (GTK_WINDOW (window), frame));
      if (i % 2 == 0)
        {
          gtk_window_set_child (GTK_WINDOW (window), NULL);
          g_assert_null (weak);
        }
      gtk_window_destroy (GTK_WINDOW (window));
      gtk_window_destroy (GTK_WINDOW (second));
      g_assert_null (weak);
    }
}

static void
check_geometry (GtkWidget *window,
                GtkWidget *frame)
{
  GskRoundedRect shape, actual;
  graphene_matrix_t matrix;
  double xx, yx, xy, yy, dx, dy;
  unsigned flip;

  sit_widget_get_border_box (frame, &shape);
  sit_widget_get_border_box (window, &actual);
  g_assert_true (gtk_widget_compute_transform (frame, window, &matrix));
  g_assert_true (graphene_matrix_to_2d (&matrix, &xx, &yx, &xy, &yy, &dx, &dy));
  double x = shape.bounds.origin.x * xx + dx;
  double y = shape.bounds.origin.y * yy + dy;
  double width = shape.bounds.size.width * xx;
  double height = shape.bounds.size.height * yy;
  g_assert_cmpfloat_with_epsilon (actual.bounds.origin.x, MIN (x, x + width), .001);
  g_assert_cmpfloat_with_epsilon (actual.bounds.origin.y, MIN (y, y + height), .001);
  g_assert_cmpfloat_with_epsilon (actual.bounds.size.width, fabs (width), .001);
  g_assert_cmpfloat_with_epsilon (actual.bounds.size.height, fabs (height), .001);
  flip = (xx < 0 ? 1 : 0) ^ (yy < 0 ? 3 : 0);
  for (unsigned i = 0; i < 4; i++)
    {
      g_assert_cmpfloat_with_epsilon (actual.corner[i].width, shape.corner[i ^ flip].width * fabs (xx), .001);
      g_assert_cmpfloat_with_epsilon (actual.corner[i].height, shape.corner[i ^ flip].height * fabs (yy), .001);
    }
  cairo_region_t *a = sit_widget_create_border_region (window, NULL, NULL);
  cairo_region_t *b = sit_widget_create_border_region (frame, NULL, NULL);
  g_assert_nonnull (a);
  g_assert_nonnull (b);
  g_assert_true (cairo_region_equal (a, b));
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (window));
  double offset_x, offset_y;
  gtk_native_get_surface_transform (GTK_NATIVE (window), &offset_x, &offset_y);
  g_assert_nonnull (surface->input_region);
  for (int y = 0; y < gdk_surface_get_height (surface); y++)
    for (int x = 0; x < gdk_surface_get_width (surface); x++)
      {
        graphene_point_t local = GRAPHENE_POINT_INIT ((x + 0.5 - offset_x - dx) / xx,
                                                       (y + 0.5 - offset_y - dy) / yy);
        gboolean expected = x + 0.5 >= actual.bounds.origin.x + offset_x &&
                            x + 0.5 < actual.bounds.origin.x + actual.bounds.size.width + offset_x &&
                            y + 0.5 >= actual.bounds.origin.y + offset_y &&
                            y + 0.5 < actual.bounds.origin.y + actual.bounds.size.height + offset_y &&
                            gsk_rounded_rect_contains_point (&shape, &local);
        g_assert_cmpint (cairo_region_contains_point (surface->input_region, x, y), ==, expected);
      }
  cairo_region_destroy (a);
  cairo_region_destroy (b);
}

static void
border_geometry (void)
{
  GtkWidget *fixed = gtk_fixed_new ();
  GtkWidget *frame = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *window = new_window (fixed);
  gtk_window_set_resizable (GTK_WINDOW (window), FALSE);
  GtkCssProvider *css = gtk_css_provider_new ();
  graphene_point_t point = GRAPHENE_POINT_INIT (210.25, 130.75);
  GskTransform *transforms[] = {
    NULL,
    gsk_transform_translate (NULL, &point),
    gsk_transform_scale (NULL, .75, 1.25),
    gsk_transform_scale (gsk_transform_translate (NULL, &point), -1, 1),
    gsk_transform_scale (gsk_transform_translate (NULL, &point), 1, -1),
    gsk_transform_scale (gsk_transform_translate (NULL, &point), -1, -1),
  };

  gtk_widget_add_css_class (frame, "test-border");
  gtk_css_provider_load_from_string (css,
    ".test-border { border: 2px solid black; padding: 3px; border-radius: 44px 12px 26px 6px; border-top-left-radius: 44px 18px; transition: none; }");
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (css), GTK_STYLE_PROVIDER_PRIORITY_USER);
  gtk_widget_set_size_request (frame, 200, 100);
  gtk_fixed_put (GTK_FIXED (fixed), frame, 20, 30);
  g_assert_true (sit_window_set_border_widget (GTK_WINDOW (window), frame));
  gtk_window_present (GTK_WINDOW (window));
  gtk_test_widget_wait_for_draw (window);

  for (unsigned i = 0; i < G_N_ELEMENTS (transforms); i++)
    {
      gtk_fixed_set_child_transform (GTK_FIXED (fixed), frame, transforms[i]);
      gtk_test_widget_wait_for_draw (window);
      check_geometry (window, frame);
      gsk_transform_unref (transforms[i]);
    }

  gtk_fixed_set_child_transform (GTK_FIXED (fixed), frame, NULL);
  gtk_test_widget_wait_for_draw (window);
  GskRoundedRect original, fallback;
  g_assert_true (sit_window_set_border_widget (GTK_WINDOW (window), NULL));
  sit_widget_get_border_box (window, &original);
  g_assert_true (sit_window_set_border_widget (GTK_WINDOW (window), frame));
  GskTransform *rotation = gsk_transform_rotate (NULL, 15);
  gtk_fixed_set_child_transform (GTK_FIXED (fixed), frame, rotation);
  gsk_transform_unref (rotation);
  gtk_test_widget_wait_for_draw (window);
  sit_widget_get_border_box (window, &fallback);
  g_assert_true (graphene_rect_equal (&original.bounds, &fallback.bounds));
  for (int i = 0; i < 4; i++)
    g_assert_true (graphene_size_equal (&original.corner[i], &fallback.corner[i]));

  gtk_window_destroy (GTK_WINDOW (window));
  gtk_style_context_remove_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (css));
  g_object_unref (css);
}

static void
source_unparented (void)
{
  GtkWidget *frame = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *window = new_window (frame);
  GskRoundedRect own, fallback;
  gtk_window_set_resizable (GTK_WINDOW (window), FALSE);
  g_object_ref (frame);
  gtk_window_present (GTK_WINDOW (window));
  gtk_test_widget_wait_for_draw (window);
  sit_widget_get_border_box (window, &own);
  g_assert_true (sit_window_set_border_widget (GTK_WINDOW (window), frame));
  gtk_widget_set_visible (frame, FALSE);
  gtk_test_widget_wait_for_draw (window);
  sit_widget_get_border_box (window, &fallback);
  g_assert_true (graphene_rect_equal (&own.bounds, &fallback.bounds));
  gtk_widget_set_visible (frame, TRUE);
  gtk_test_widget_wait_for_draw (window);
  check_geometry (window, frame);
  gtk_window_set_child (GTK_WINDOW (window), NULL);
  gtk_test_widget_wait_for_draw (window);
  sit_widget_get_border_box (window, &fallback);
  g_assert_true (graphene_rect_equal (&own.bounds, &fallback.bounds));
  for (int i = 0; i < 4; i++)
    g_assert_true (graphene_size_equal (&own.corner[i], &fallback.corner[i]));
  g_object_unref (frame);
  gtk_test_widget_wait_for_draw (window);
  g_assert_true (sit_window_set_border_widget (GTK_WINDOW (window), NULL));
  gtk_window_destroy (GTK_WINDOW (window));
}

int
main (int argc, char **argv)
{
  gtk_test_init (&argc, &argv, NULL);
  g_test_add_func ("/sit/window/binding-lifecycle", binding_lifecycle);
  g_test_add_func ("/sit/window/border-geometry", border_geometry);
  g_test_add_func ("/sit/window/source-unparented", source_unparented);
  return g_test_run ();
}
