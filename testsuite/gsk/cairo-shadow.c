#include <gtk/gtk.h>
#include <string.h>

static cairo_surface_t *
draw_shadow (GskRenderNode               *node,
             const cairo_rectangle_int_t *clip)
{
  cairo_surface_t *surface;
  cairo_t *cr;

  surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 128, 128);
  cr = cairo_create (surface);
  if (clip)
    {
      cairo_rectangle (cr, clip->x, clip->y, clip->width, clip->height);
      cairo_clip (cr);
    }
  gsk_render_node_draw (node, cr);
  cairo_destroy (cr);
  cairo_surface_flush (surface);

  return surface;
}

static void
test_shadow_scale (void)
{
  const float scales[] = { 0.997f, 0.9975f, 0.997658f, 0.998f, 0.9985f, 0.999f, 1.f };
  const int samples[][2] = {
    { 100, 302 }, { 200, 305 }, { 350, 308 },
    { 100, 17 }, { 200, 18 }, { 350, 19 },
    { 14, 80 }, { 17, 150 }, { 19, 230 },
    { 461, 80 }, { 464, 150 }, { 467, 230 }
  };
  GskRoundedRect outline;
  GdkRGBA color = { 0, 0, 0, 0.34 };

  gsk_rounded_rect_init_from_rect (&outline, &GRAPHENE_RECT_INIT (-1, -1, 440, 280), 0);
  outline.corner[0] = GRAPHENE_SIZE_INIT (48, 48);
  outline.corner[1] = GRAPHENE_SIZE_INIT (8, 8);
  outline.corner[2] = GRAPHENE_SIZE_INIT (32, 32);

  for (gsize i = 0; i < G_N_ELEMENTS (scales); i++)
    {
      GskRenderNode *shadow, *translated, *scaled, *opacity, *node;
      GskTransform *transform;
      cairo_surface_t *surface;
      cairo_t *cr;
      const guchar *pixels;
      int stride;

      shadow = gsk_outset_shadow_node_new (&outline, &color, 0, 6, 0, 14);
      transform = gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (1, 1));
      translated = gsk_transform_node_new (shadow, transform);
      gsk_transform_unref (transform);
      transform = gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (220, 140));
      transform = gsk_transform_scale (transform, scales[i], scales[i]);
      transform = gsk_transform_translate (transform, &GRAPHENE_POINT_INIT (-220, -140));
      scaled = gsk_transform_node_new (translated, transform);
      gsk_transform_unref (transform);
      opacity = gsk_opacity_node_new (scaled, 0.941176);
      transform = gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (20, 20));
      node = gsk_transform_node_new (opacity, transform);
      gsk_transform_unref (transform);

      surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 480, 320);
      cr = cairo_create (surface);
      gsk_render_node_draw (node, cr);
      cairo_destroy (cr);
      cairo_surface_flush (surface);
      pixels = cairo_image_surface_get_data (surface);
      stride = cairo_image_surface_get_stride (surface);
      for (gsize j = 0; j < G_N_ELEMENTS (samples); j++)
        {
          guint32 pixel;
          int x = samples[j][0], y = samples[j][1];
          memcpy (&pixel, pixels + y * stride + x * 4, sizeof pixel);
          g_test_message ("scale %g, pixel %d,%d: alpha %u", scales[i], x, y, pixel >> 24);
          g_assert_cmpuint (pixel >> 24, >, 0);
        }
      cairo_surface_destroy (surface);
      gsk_render_node_unref (node);
      gsk_render_node_unref (opacity);
      gsk_render_node_unref (scaled);
      gsk_render_node_unref (translated);
      gsk_render_node_unref (shadow);
    }
}

static void
test_shadow_clip (gconstpointer data)
{
  const cairo_rectangle_int_t clips[] = {
    { 8, 8, 120, 24 }, { 8, 96, 120, 32 },
    { 8, 32, 24, 64 }, { 96, 32, 32, 64 },
    { 0, 0, 8, 8 }, { 32, 32, 64, 64 },
    { 32, 32, 3, 3 }, { 93, 93, 3, 3 }
  };
  const char *variant = data;
  GskRoundedRect outline;
  GdkRGBA color = { 0, 0, 0, 0.75 };
  GskRenderNode *node;
  cairo_surface_t *full;

  gsk_rounded_rect_init_from_rect (&outline, &GRAPHENE_RECT_INIT (32, 32, 64, 64),
                                 strcmp (variant, "square") == 0 ? 0 : 12);
  if (strcmp (variant, "asymmetric") == 0)
    {
      outline.corner[0] = GRAPHENE_SIZE_INIT (28, 28);
      outline.corner[1] = GRAPHENE_SIZE_INIT (4, 4);
      outline.corner[2] = GRAPHENE_SIZE_INIT (20, 20);
      outline.corner[3] = GRAPHENE_SIZE_INIT (0, 0);
    }
  else if (strcmp (variant, "elliptical") == 0)
    {
      outline.corner[0] = GRAPHENE_SIZE_INIT (24, 8);
      outline.corner[1] = GRAPHENE_SIZE_INIT (8, 24);
      outline.corner[2] = GRAPHENE_SIZE_INIT (20, 10);
      outline.corner[3] = GRAPHENE_SIZE_INIT (4, 16);
    }
  if (strcmp (variant, "clear") == 0)
    color.alpha = 0;
  node = gsk_outset_shadow_node_new (&outline, &color,
                                   strcmp (variant, "offset") == 0 ? -12 : 4,
                                   strcmp (variant, "offset") == 0 ? 8 : 4,
                                   strcmp (variant, "offset") == 0 ? -2 : 2,
                                   strcmp (variant, "solid") == 0 ? 0 : 12);
  if (strcmp (variant, "opacity") == 0)
    {
      GskRenderNode *child = node;
      node = gsk_opacity_node_new (child, 0.9);
      gsk_render_node_unref (child);
    }
  else if (strcmp (variant, "transform") == 0)
    {
      GskRenderNode *child = node;
      GskTransform *transform = gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (0.25, -0.5));
      transform = gsk_transform_scale (transform, 0.875, 1.125);
      node = gsk_transform_node_new (child, transform);
      gsk_transform_unref (transform);
      gsk_render_node_unref (child);
    }
  full = draw_shadow (node, NULL);

  for (gsize i = 0; i < G_N_ELEMENTS (clips); i++)
    {
      cairo_surface_t *partial = draw_shadow (node, &clips[i]);
      const guchar *expected = cairo_image_surface_get_data (full);
      const guchar *actual = cairo_image_surface_get_data (partial);
      int stride = cairo_image_surface_get_stride (full);
      guint missing = 0, max_error = 0, outside = 0;

      for (int y = 0; y < 128; y++)
        for (int x = 0; x < 128; x++)
          {
            gboolean inside = x >= clips[i].x && y >= clips[i].y
              && x < clips[i].x + clips[i].width && y < clips[i].y + clips[i].height;
            guint32 wanted = 0, pixel;
            if (inside)
              memcpy (&wanted, expected + y * stride + x * 4, sizeof wanted);
            memcpy (&pixel, actual + y * stride + x * 4, sizeof pixel);
            missing += pixel != wanted;
            if (!inside)
              outside += pixel != 0;
            for (int channel = 0; channel < 4; channel++)
              {
                int a = (pixel >> (channel * 8)) & 255;
                int b = (wanted >> (channel * 8)) & 255;
                max_error = MAX (max_error, ABS (a - b));
              }
          }
      g_test_message ("clip %zu: %u different pixels, maximum channel error %u", i, missing, max_error);
      /* Narrow clips can change Cairo's antialias rounding. */
      g_assert_cmpuint (max_error, <=, 2);
      g_assert_cmpuint (outside, ==, 0);
      cairo_surface_destroy (partial);
    }

  cairo_surface_destroy (full);
  gsk_render_node_unref (node);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  gtk_init ();
  g_test_add_func ("/shadow/scale", test_shadow_scale);
  const char *variants[] = { "square", "rounded", "asymmetric", "elliptical",
                            "offset", "solid", "opacity", "transform", "clear" };
  for (gsize i = 0; i < G_N_ELEMENTS (variants); i++)
    {
      char *path = g_strdup_printf ("/shadow/clip/%s", variants[i]);
      g_test_add_data_func (path, variants[i], test_shadow_clip);
      g_free (path);
    }
  return g_test_run ();
}
