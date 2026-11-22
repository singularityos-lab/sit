#include <gtk/gtk.h>
#include <glib/gstdio.h>

#include <string.h>

#define RED 0xffff0000
#define BLUE 0xff0000ff
#define RESOURCE_PATH "/org/gtk/test/iride"

static char *overlay;

static guint32
crc32_of (const guint8 *data,
          gsize         len)
{
  guint32 crc = 0xffffffff;

  for (gsize i = 0; i < len; i++)
    {
      crc ^= data[i];
      for (int k = 0; k < 8; k++)
        crc = (crc >> 1) ^ (0xedb88320 & -(crc & 1));
    }

  return ~crc;
}

static GBytes *
red_icon (int side)
{
  GByteArray *file = g_byte_array_new ();
  guint32 *pixels = g_new (guint32, side * side);
  GBytes *data, *png;
  GdkTexture *texture;
  const guint8 *body;
  gsize len;
  guint16 u16;
  guint32 u32;

  for (int i = 0; i < side * side; i++)
    pixels[i] = RED;
  data = g_bytes_new_take (pixels, side * side * 4);
  texture = gdk_memory_texture_new (side, side, GDK_MEMORY_DEFAULT, data, side * 4);
  png = gdk_texture_save_to_png_bytes (texture);
  body = g_bytes_get_data (png, &len);

  g_byte_array_append (file, (const guint8 *) "IRIDE", 6);
  u16 = GUINT16_TO_LE (1);
  g_byte_array_append (file, (guint8 *) &u16, 2);
  u32 = GUINT32_TO_LE (1);
  g_byte_array_append (file, (guint8 *) &u32, 4);
  u16 = GUINT16_TO_LE (side);
  g_byte_array_append (file, (guint8 *) &u16, 2);
  u16 = 0;
  g_byte_array_append (file, (guint8 *) &u16, 2);
  u32 = GUINT32_TO_LE (28);
  g_byte_array_append (file, (guint8 *) &u32, 4);
  u32 = GUINT32_TO_LE (len);
  g_byte_array_append (file, (guint8 *) &u32, 4);
  u32 = GUINT32_TO_LE (crc32_of (body, len));
  g_byte_array_append (file, (guint8 *) &u32, 4);
  g_byte_array_append (file, body, len);

  g_object_unref (texture);
  g_bytes_unref (data);
  g_bytes_unref (png);

  return g_byte_array_free_to_bytes (file);
}

static guint32
center_of_icon (GtkIconTheme *theme,
                int           size)
{
  GtkIconPaintable *icon;
  GtkSnapshot *snapshot = gtk_snapshot_new ();
  GskRenderNode *node;
  cairo_surface_t *surface;
  cairo_t *cr;
  guint32 pixel;

  icon = gtk_icon_theme_lookup_icon (theme, "probe", NULL, size, 1, GTK_TEXT_DIR_LTR, 0);
  gdk_paintable_snapshot (GDK_PAINTABLE (icon), snapshot, size, size);
  node = gtk_snapshot_free_to_node (snapshot);
  g_assert_nonnull (node);

  surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, size, size);
  cr = cairo_create (surface);
  gsk_render_node_draw (node, cr);
  cairo_destroy (cr);
  cairo_surface_flush (surface);
  pixel = *(guint32 *) (cairo_image_surface_get_data (surface) +
                        size / 2 * cairo_image_surface_get_stride (surface) + size / 2 * 4);

  cairo_surface_destroy (surface);
  gsk_render_node_unref (node);
  g_object_unref (icon);

  return pixel;
}

static void
check_icon (GBytes  *iride,
            int      size,
            guint32  expected)
{
  char *root = g_dir_make_tmp ("iride-XXXXXX", NULL);
  char *theme = g_build_filename (root, "test", NULL);
  char *apps = g_build_filename (theme, "apps", NULL);
  char *index = g_build_filename (theme, "index.theme", NULL);
  char *svg = g_build_filename (apps, "probe.svg", NULL);
  char *sibling = g_build_filename (apps, "probe.iride", NULL);
  GtkIconTheme *icons;
  gboolean ok;

  g_assert_cmpint (g_mkdir_with_parents (apps, 0700), ==, 0);
  ok = g_file_set_contents (index,
                            "[Icon Theme]\nName=test\nDirectories=apps\n\n"
                            "[apps]\nSize=48\nMinSize=8\nMaxSize=512\nType=Scalable\n", -1, NULL);
  g_assert_true (ok);
  ok = g_file_set_contents (svg,
                            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"48\" height=\"48\">"
                            "<rect width=\"48\" height=\"48\" fill=\"#0000ff\"/></svg>", -1, NULL);
  g_assert_true (ok);
  if (iride)
    {
      ok = g_file_set_contents (sibling, g_bytes_get_data (iride, NULL), g_bytes_get_size (iride), NULL);
      g_assert_true (ok);
    }

  icons = gtk_icon_theme_new ();
  gtk_icon_theme_set_theme_name (icons, "test");
  gtk_icon_theme_set_search_path (icons, (const char * const[]) { root, NULL });
  g_assert_cmphex (center_of_icon (icons, size), ==, expected);
  g_object_unref (icons);

  g_remove (sibling);
  g_remove (svg);
  g_remove (index);
  g_remove (apps);
  g_remove (theme);
  g_remove (root);
  g_free (sibling);
  g_free (svg);
  g_free (index);
  g_free (apps);
  g_free (theme);
  g_free (root);
}

static void
test_sibling (void)
{
  GBytes *iride = red_icon (32);

  check_icon (iride, 32, RED);
  check_icon (iride, 16, RED);
  g_bytes_unref (iride);
}

static void
test_never_enlarged (void)
{
  GBytes *iride = red_icon (32);

  check_icon (iride, 64, BLUE);
  g_bytes_unref (iride);
}

static void
test_svg_fallback (void)
{
  GBytes *broken = g_bytes_new_static ("IRIDE", 6);
  GBytes *empty = g_bytes_new_static ("", 0);

  check_icon (NULL, 32, BLUE);
  check_icon (broken, 32, BLUE);
  check_icon (empty, 32, BLUE);
  g_bytes_unref (broken);
  g_bytes_unref (empty);
}

static void
check_resource (GBytes  *iride,
                int      size,
                guint32  expected)
{
  char *svg = g_build_filename (overlay, "probe.svg", NULL);
  char *sibling = g_build_filename (overlay, "probe.iride", NULL);
  GtkIconTheme *icons;
  gboolean ok;

  ok = g_file_set_contents (svg,
                            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"48\" height=\"48\">"
                            "<rect width=\"48\" height=\"48\" fill=\"#0000ff\"/></svg>", -1, NULL);
  g_assert_true (ok);
  if (iride)
    {
      ok = g_file_set_contents (sibling, g_bytes_get_data (iride, NULL), g_bytes_get_size (iride), NULL);
      g_assert_true (ok);
    }

  icons = gtk_icon_theme_new ();
  gtk_icon_theme_set_theme_name (icons, "test");
  gtk_icon_theme_set_search_path (icons, (const char * const[]) { NULL });
  gtk_icon_theme_set_resource_path (icons, (const char * const[]) { RESOURCE_PATH, NULL });
  g_assert_cmphex (center_of_icon (icons, size), ==, expected);
  g_object_unref (icons);

  g_remove (sibling);
  g_remove (svg);
  g_free (sibling);
  g_free (svg);
}

static void
test_resource (void)
{
  GBytes *iride = red_icon (32);

  check_resource (NULL, 32, BLUE);
  check_resource (iride, 64, BLUE);
  check_resource (iride, 32, RED);
  g_bytes_unref (iride);
}

int
main (int argc, char *argv[])
{
  char *mapping;
  int result;

  overlay = g_dir_make_tmp ("iride-XXXXXX", NULL);
  g_assert_nonnull (overlay);
  mapping = g_strdup_printf ("%s=%s", RESOURCE_PATH, overlay);
  g_setenv ("G_RESOURCE_OVERLAYS", mapping, TRUE);
  g_free (mapping);

  gtk_test_init (&argc, &argv);

  g_test_add_func ("/iride/sibling", test_sibling);
  g_test_add_func ("/iride/never-enlarged", test_never_enlarged);
  g_test_add_func ("/iride/svg-fallback", test_svg_fallback);

  g_test_add_func ("/iride/resource", test_resource);

  result = g_test_run ();
  g_remove (overlay);
  g_free (overlay);

  return result;
}
