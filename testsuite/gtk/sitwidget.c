#include <gtk/sit.h>
#include <math.h>

static void
check_box (const char *css,
           float       top_left,
           float       top_right,
           float       bottom_right,
           float       bottom_left)
{
  GtkWidget *widget = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkCssProvider *provider = gtk_css_provider_new ();
  GskRoundedRect box, copy;
  float radii[] = { top_left, top_right, bottom_right, bottom_left };

  g_object_ref_sink (widget);
  gtk_widget_add_css_class (widget, "shape");
  gtk_css_provider_load_from_string (provider, css);
  gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                              GTK_STYLE_PROVIDER (provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_USER);
  gtk_widget_allocate (widget, 200, 100, -1, NULL);
  sit_widget_get_border_box (widget, &box);

  g_assert_cmpfloat (box.bounds.origin.x, ==, -10);
  g_assert_cmpfloat (box.bounds.origin.y, ==, -10);
  g_assert_cmpfloat (box.bounds.size.width, ==, 200);
  g_assert_cmpfloat (box.bounds.size.height, ==, 100);
  for (unsigned i = 0; i < 4; i++)
    {
      g_assert_cmpfloat_with_epsilon (box.corner[i].width, radii[i], 0.001);
      g_assert_cmpfloat_with_epsilon (box.corner[i].height, radii[i], 0.001);
    }

  copy = box;
  box.corner[0].width = 999;
  sit_widget_get_border_box (widget, &box);
  g_assert_cmpfloat (box.corner[0].width, ==, copy.corner[0].width);
  gtk_style_context_remove_provider_for_display (gdk_display_get_default (),
                                                 GTK_STYLE_PROVIDER (provider));
  g_object_unref (provider);
  g_object_unref (widget);
}

static void
border_box (void)
{
  check_box (".shape { padding: 6px; border: 4px solid black; border-radius: 0; }",
             0, 0, 0, 0);
  check_box (".shape { padding: 6px; border: 4px solid black; border-radius: 12px 24px 8px 16px; }",
             12, 24, 8, 16);
  check_box (".shape { padding: 6px; border: 4px solid black; border-radius: 80px; }",
             50, 50, 50, 50);
}

static void
surface_region_for_window (gboolean decorated)
{
  GtkWidget *window = gtk_window_new ();
  GtkWidget *widget = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkCssProvider *provider = gtk_css_provider_new ();
  GskRoundedRect box;
  graphene_point_t origin, zero = GRAPHENE_POINT_INIT (0, 0);
  graphene_point_t shift = GRAPHENE_POINT_INIT (0.25, 2.75);
  graphene_point_t outside = GRAPHENE_POINT_INIT (10000, 10000);
  graphene_point_t invalid = GRAPHENE_POINT_INIT (NAN, NAN);
  GskTransform *transforms[] = {
    NULL,
    gsk_transform_translate (NULL, &shift),
    gsk_transform_scale (NULL, 0.75, 1.25),
    gsk_transform_scale (NULL, -1, 1),
    gsk_transform_scale (NULL, 1, -1),
  };
  cairo_region_t *region;
  GskTransform *transform;
  double offset_x, offset_y;

  g_assert_null (sit_widget_create_border_region (widget, NULL, NULL));
  gtk_widget_add_css_class (widget, "shape");
  gtk_css_provider_load_from_string (provider,
    ".shape { padding: 6px; border: 4px solid black; border-radius: 30px 12px 20px 5px; border-top-left-radius: 30px 10px; border-bottom-right-radius: 20px 6px; }");
  gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                              GTK_STYLE_PROVIDER (provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_USER);
  gtk_window_set_decorated (GTK_WINDOW (window), decorated);
  if (decorated)
    {
      GtkWidget *titlebar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_widget_set_visible (titlebar, FALSE);
      gtk_window_set_titlebar (GTK_WINDOW (window), titlebar);
    }
  gtk_window_set_default_size (GTK_WINDOW (window), 200, 100);
  gtk_window_set_child (GTK_WINDOW (window), widget);
  gtk_window_present (GTK_WINDOW (window));
  gtk_test_widget_wait_for_draw (window);
  sit_widget_get_border_box (widget, &box);
  g_assert_true (gtk_widget_compute_point (widget, window, &zero, &origin));
  gtk_native_get_surface_transform (GTK_NATIVE (window), &offset_x, &offset_y);
  g_test_message ("origin %g,%g surface offset %g,%g", origin.x, origin.y, offset_x, offset_y);

  if (decorated)
    {
      g_assert_cmpfloat (offset_x, >, 0);
      g_assert_cmpfloat (offset_y, >, 0);
    }

  for (unsigned i = 0; i < G_N_ELEMENTS (transforms); i++)
    {
      float xx, yx, xy, yy, dx, dy;
      double x1, y1, x2, y2;

      gsk_transform_to_2d (transforms[i], &xx, &yx, &xy, &yy, &dx, &dy);
      x1 = box.bounds.origin.x * xx + dx + origin.x + offset_x;
      y1 = box.bounds.origin.y * yy + dy + origin.y + offset_y;
      x2 = x1 + box.bounds.size.width * xx;
      y2 = y1 + box.bounds.size.height * yy;
      region = sit_widget_create_border_region (widget, widget, transforms[i]);
      g_assert_nonnull (region);
      for (int y = 0; y < gdk_surface_get_height (gtk_native_get_surface (GTK_NATIVE (window))); y++)
        {
          for (int x = 0; x < gdk_surface_get_width (gtk_native_get_surface (GTK_NATIVE (window))); x++)
            {
              graphene_point_t point = GRAPHENE_POINT_INIT ((x + 0.5 - offset_x - origin.x - dx) / xx,
                                                            (y + 0.5 - offset_y - origin.y - dy) / yy);
              gboolean expected = x + 0.5 >= MIN (x1, x2) && x + 0.5 < MAX (x1, x2) &&
                                  y + 0.5 >= MIN (y1, y2) && y + 0.5 < MAX (y1, y2) &&
                                  gsk_rounded_rect_contains_point (&box, &point);
              if (cairo_region_contains_point (region, x, y) != expected)
                g_test_message ("transform %u pixel %d,%d local %g,%g", i, x, y, point.x, point.y);
              g_assert_cmpint (cairo_region_contains_point (region, x, y), ==,
                               expected);
            }
        }
      cairo_region_destroy (region);
      gsk_transform_unref (transforms[i]);
    }

  transform = gsk_transform_scale (NULL, 0, 0);
  region = sit_widget_create_border_region (widget, NULL, transform);
  g_assert_true (cairo_region_is_empty (region));
  cairo_region_destroy (region);
  gsk_transform_unref (transform);
  transform = gsk_transform_translate (NULL, &outside);
  region = sit_widget_create_border_region (widget, NULL, transform);
  g_assert_true (cairo_region_is_empty (region));
  cairo_region_destroy (region);
  gsk_transform_unref (transform);
  transform = gsk_transform_translate (NULL, &invalid);
  region = sit_widget_create_border_region (widget, NULL, transform);
  g_assert_true (cairo_region_is_empty (region));
  cairo_region_destroy (region);
  gsk_transform_unref (transform);
  transform = gsk_transform_rotate (NULL, 45);
  g_assert_null (sit_widget_create_border_region (widget, NULL, transform));
  gsk_transform_unref (transform);
  gtk_window_destroy (GTK_WINDOW (window));
  gtk_style_context_remove_provider_for_display (gdk_display_get_default (),
                                                 GTK_STYLE_PROVIDER (provider));
  g_object_unref (provider);
}

static void
surface_region (void)
{
  surface_region_for_window (FALSE);
}

static void
surface_region_csd (void)
{
  surface_region_for_window (TRUE);
}

int
main (int argc, char **argv)
{
  gtk_test_init (&argc, &argv);
  g_test_add_func ("/sit/widget/border-box", border_box);
  g_test_add_func ("/sit/widget/surface-region", surface_region);
  g_test_add_func ("/sit/widget/surface-region-csd", surface_region_csd);
  return g_test_run ();
}
