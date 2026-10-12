#include <gtk/gtk.h>

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif

static void
test_toplevel_icon (gconstpointer data)
{
  GdkDisplay *display = gdk_display_get_default ();
  GtkWidget *window;
  GdkSurface *surface;
  GBytes *bytes;
  GdkTexture *texture;
  GList icons;
  guint32 pixels[32 * 32];
  guint replacements = GPOINTER_TO_UINT (data);

#ifdef GDK_WINDOWING_WAYLAND
  if (!GDK_IS_WAYLAND_DISPLAY (display) ||
      !gdk_wayland_display_query_registry (display, "xdg_toplevel_icon_manager_v1"))
#endif
    {
      g_test_skip ("No Wayland toplevel icon support");
      return;
    }

  window = gtk_window_new ();
  gtk_window_set_child (GTK_WINDOW (window), gtk_label_new ("Toplevel icon"));
  gtk_window_set_default_size (GTK_WINDOW (window), 200, 100);
  gtk_window_present (GTK_WINDOW (window));
  gdk_display_sync (display);
  surface = gtk_native_get_surface (GTK_NATIVE (window));
  g_assert_true (GDK_IS_TOPLEVEL (surface));

  for (guint i = 0; i < G_N_ELEMENTS (pixels); i++)
    pixels[i] = 0xff336699;
  bytes = g_bytes_new (pixels, sizeof pixels);
  texture = gdk_memory_texture_new (32, 32, GDK_MEMORY_DEFAULT, bytes, 32 * 4);
  g_bytes_unref (bytes);
  icons = (GList) { .data = texture };

  for (guint i = 0; i < replacements; i++)
    {
      gdk_toplevel_set_icon_list (GDK_TOPLEVEL (surface), &icons);
      gdk_display_sync (display);
      while (g_main_context_pending (NULL))
        g_main_context_iteration (NULL, FALSE);
    }

  if (replacements > 1)
    {
      gdk_toplevel_set_icon_list (GDK_TOPLEVEL (surface), NULL);
      gdk_display_sync (display);
    }
  gtk_window_destroy (GTK_WINDOW (window));
  g_object_unref (texture);
  gdk_display_sync (display);
  while (g_main_context_pending (NULL))
    g_main_context_iteration (NULL, FALSE);
}

int
main (int argc, char **argv)
{
  gtk_test_init (&argc, &argv);
  g_test_add_data_func ("/toplevel-icon/destroy", GUINT_TO_POINTER (1), test_toplevel_icon);
  g_test_add_data_func ("/toplevel-icon/replace", GUINT_TO_POINTER (8), test_toplevel_icon);
  return g_test_run ();
}
