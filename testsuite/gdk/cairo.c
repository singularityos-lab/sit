#include <gtk/gtk.h>

static const double repeat_scales[] = { 1, 2, 3, 0.5 };
static const double device_scales[] = { 1, 2, 1.5 };

static void
test_set_source_big_pixbuf (void)
{
  cairo_surface_t *surface;
  GdkPixbuf *pixbuf;
  cairo_t *cr;

#define WAY_TOO_BIG 65540

  /* Check that too big really is to big.
   * If this check fails, somebody improved Cairo and this test is useless.
   */
  surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, WAY_TOO_BIG, 1);
  g_assert_cmpint (cairo_surface_status (surface), !=, CAIRO_STATUS_SUCCESS);
  cairo_surface_destroy (surface);

  surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 10, 10);
  cr = cairo_create (surface);
  pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, WAY_TOO_BIG, 1);

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  gdk_cairo_set_source_pixbuf (cr, pixbuf, 0, 0);
G_GNUC_END_IGNORE_DEPRECATIONS
  g_assert_cmpint (cairo_status (cr), !=, CAIRO_STATUS_SUCCESS);

  g_object_unref (pixbuf);
  cairo_destroy (cr);
  cairo_surface_destroy (surface);
}

static void
test_nested_repeat (gconstpointer data)
{
  unsigned int test = GPOINTER_TO_UINT (data);
  double scale = repeat_scales[test % G_N_ELEMENTS (repeat_scales)];
  double device_scale = device_scales[test / G_N_ELEMENTS (repeat_scales)];
  GskRenderNode *color;
  GskRenderNode *inner;
  GskRenderNode *outer;
  cairo_surface_t *surfaces[2];
  int width = ceil (128 * scale * device_scale);
  int height = ceil (64 * scale * device_scale);

  color = gsk_color_node_new (&(GdkRGBA) { 1, 0, 0, 1 },
                              &GRAPHENE_RECT_INIT (2, 2, 4, 4));
  inner = gsk_repeat_node_new (&GRAPHENE_RECT_INIT (0, 0, 32, 16), color,
                               &GRAPHENE_RECT_INIT (0, 0, 8, 8));
  outer = gsk_repeat_node_new (&GRAPHENE_RECT_INIT (0, 0, 128, 64), inner,
                               &GRAPHENE_RECT_INIT (0, 0, 32, 16));

  for (unsigned int i = 0; i < G_N_ELEMENTS (surfaces); i++)
    {
      cairo_t *cr;

      surfaces[i] = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, width, height);
      cairo_surface_set_device_scale (surfaces[i], device_scale, device_scale);
      cr = cairo_create (surfaces[i]);
      cairo_scale (cr, scale, scale);
      if (i == 0)
        gsk_render_node_draw (outer, cr);
      else
        {
          cairo_set_source_rgba (cr, 1, 0, 0, 1);
          for (int y = 0; y < 64; y += 8)
            for (int x = 0; x < 128; x += 8)
              cairo_rectangle (cr, x + 2, y + 2, 4, 4);
          cairo_fill (cr);
        }
      g_assert_cmpint (cairo_status (cr), ==, CAIRO_STATUS_SUCCESS);
      cairo_destroy (cr);
      cairo_surface_flush (surfaces[i]);
    }

  g_assert_cmpmem (cairo_image_surface_get_data (surfaces[0]),
                   height * cairo_image_surface_get_stride (surfaces[0]),
                   cairo_image_surface_get_data (surfaces[1]),
                   height * cairo_image_surface_get_stride (surfaces[1]));

  for (unsigned int i = 0; i < G_N_ELEMENTS (surfaces); i++)
    cairo_surface_destroy (surfaces[i]);
  gsk_render_node_unref (outer);
  gsk_render_node_unref (inner);
  gsk_render_node_unref (color);
}

int
main (int argc, char *argv[])
{
  (g_test_init) (&argc, &argv, NULL);
  gtk_init ();

  g_test_add_func ("/drawing/set-source-big-pixbuf", test_set_source_big_pixbuf);

  for (unsigned int d = 0; d < G_N_ELEMENTS (device_scales); d++)
    for (unsigned int i = 0; i < G_N_ELEMENTS (repeat_scales); i++)
      {
        char *name = g_strdup_printf ("/drawing/nested-repeat/scale-%g-device-%g",
                                      repeat_scales[i], device_scales[d]);
        g_test_add_data_func (name,
                              GUINT_TO_POINTER (d * G_N_ELEMENTS (repeat_scales) + i),
                              test_nested_repeat);
        g_free (name);
      }

  return g_test_run ();
}
