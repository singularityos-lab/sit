#include <gtk/gtk.h>

#include "gdk/gdkdeferredtextureprivate.h"
#include "gdk/gdktextureprivate.h"
#include "gdk/loaders/gdkjpegprivate.h"
#include "gdk/loaders/gdkpngprivate.h"

static GdkTexture *
noise_texture (int             width,
               int             height,
               GdkMemoryFormat format,
               int             bpp)
{
  GRand *rand = g_rand_new_with_seed (width * 31 + height);
  gsize stride = width * bpp;
  guchar *data = g_malloc (stride * height);
  GBytes *bytes;
  GdkTexture *texture;

  for (gsize i = 0; i < stride * height; i++)
    data[i] = g_rand_int_range (rand, 0, 256);
  bytes = g_bytes_new_take (data, stride * height);
  texture = gdk_memory_texture_new (width, height, format, bytes, stride);
  g_bytes_unref (bytes);
  g_rand_free (rand);

  return texture;
}

static guchar *
download (GdkTexture *texture)
{
  gsize stride = gdk_texture_get_width (texture) * 4;
  guchar *data = g_malloc (stride * gdk_texture_get_height (texture));

  gdk_texture_download (texture, data, stride);
  return data;
}

static void
test_png_matches (void)
{
  GdkTexture *source = noise_texture (300, 200, GDK_MEMORY_R8G8B8A8, 4);
  GBytes *png = gdk_texture_save_to_png_bytes (source);
  GdkTexture *deferred;
  guchar *expected, *actual;
  gsize size = 300 * 4 * 200;

  g_assert_cmpuint (g_bytes_get_size (png), >=, 64 * 1024);
  deferred = gdk_texture_new_from_bytes (png, NULL);
  g_assert_true (GDK_IS_DEFERRED_TEXTURE (deferred));
  g_assert_cmpint (gdk_texture_get_format (deferred), ==, GDK_MEMORY_R8G8B8A8);
  g_assert_cmpint (gdk_texture_get_width (deferred), ==, 300);
  g_assert_cmpint (gdk_texture_get_height (deferred), ==, 200);
  expected = download (source);
  actual = download (deferred);
  g_assert_cmpmem (expected, size, actual, size);

  g_free (expected);
  g_free (actual);
  g_object_unref (deferred);
  g_object_unref (source);
  g_bytes_unref (png);
}

static void
test_png16 (void)
{
  GdkTexture *source = noise_texture (180, 120, GDK_MEMORY_R16G16B16, 6);
  GBytes *png = gdk_texture_save_to_png_bytes (source);
  GdkTexture *deferred = gdk_texture_new_from_bytes (png, NULL);
  GdkTextureDownloader *downloader;
  GBytes *a, *b;
  gsize stride_a, stride_b;

  g_assert_true (GDK_IS_DEFERRED_TEXTURE (deferred));
  g_assert_cmpint (gdk_texture_get_format (deferred), ==, GDK_MEMORY_R16G16B16);

  downloader = gdk_texture_downloader_new (source);
  gdk_texture_downloader_set_format (downloader, GDK_MEMORY_R16G16B16A16);
  a = gdk_texture_downloader_download_bytes (downloader, &stride_a);
  gdk_texture_downloader_set_texture (downloader, deferred);
  b = gdk_texture_downloader_download_bytes (downloader, &stride_b);
  g_assert_cmpuint (stride_a, ==, stride_b);
  g_assert_true (g_bytes_equal (a, b));

  g_bytes_unref (a);
  g_bytes_unref (b);
  gdk_texture_downloader_free (downloader);
  g_object_unref (deferred);
  g_object_unref (source);
  g_bytes_unref (png);
}

static void
test_jpeg_matches (void)
{
  GdkTexture *source = noise_texture (400, 300, GDK_MEMORY_R8G8B8, 3);
  GBytes *jpeg = gdk_save_jpeg (source);
  GdkTexture *sync = gdk_load_jpeg (jpeg, NULL);
  GdkTexture *deferred = gdk_texture_new_from_bytes (jpeg, NULL);
  guchar *a, *b;
  gsize size = 400 * 4 * 300;

  g_assert_cmpuint (g_bytes_get_size (jpeg), >=, 64 * 1024);
  g_assert_true (GDK_IS_DEFERRED_TEXTURE (deferred));
  g_assert_cmpint (gdk_texture_get_format (deferred), ==, gdk_texture_get_format (sync));
  a = download (sync);
  b = download (deferred);
  g_assert_cmpmem (a, size, b, size);

  g_free (a);
  g_free (b);
  g_object_unref (sync);
  g_object_unref (deferred);
  g_object_unref (source);
  g_bytes_unref (jpeg);
}

static void
test_small_stays_sync (void)
{
  GdkTexture *source = noise_texture (16, 16, GDK_MEMORY_R8G8B8A8, 4);
  GBytes *png = gdk_texture_save_to_png_bytes (source);
  GdkTexture *texture = gdk_texture_new_from_bytes (png, NULL);

  g_assert_false (GDK_IS_DEFERRED_TEXTURE (texture));
  g_assert_true (GDK_IS_MEMORY_TEXTURE (texture));

  g_object_unref (texture);
  g_object_unref (source);
  g_bytes_unref (png);
}

static gpointer
load_in_thread (gpointer data)
{
  return gdk_texture_new_from_bytes (data, NULL);
}

static void
test_worker_thread_stays_sync (void)
{
  GdkTexture *source = noise_texture (300, 200, GDK_MEMORY_R8G8B8A8, 4);
  GBytes *png = gdk_texture_save_to_png_bytes (source);
  GThread *thread = g_thread_new ("loader", load_in_thread, png);
  GdkTexture *texture = g_thread_join (thread);

  g_assert_true (GDK_IS_MEMORY_TEXTURE (texture));

  g_object_unref (texture);
  g_object_unref (source);
  g_bytes_unref (png);
}

static void
test_truncated (void)
{
  GdkTexture *source = noise_texture (300, 200, GDK_MEMORY_R8G8B8A8, 4);
  GBytes *png = gdk_texture_save_to_png_bytes (source);
  GBytes *cut = g_bytes_new_from_bytes (png, 0, g_bytes_get_size (png) - 4096);
  GLogLevelFlags fatal;
  GdkTexture *texture;
  guchar *data;

  fatal = g_log_set_always_fatal (G_LOG_FATAL_MASK);
  texture = gdk_texture_new_from_bytes (cut, NULL);
  g_assert_true (GDK_IS_DEFERRED_TEXTURE (texture));
  data = download (texture);
  g_log_set_always_fatal (fatal);
  for (gsize i = 0; i < 300 * 4 * 200; i++)
    if (data[i] != 0)
      g_assert_not_reached ();

  g_free (data);
  g_object_unref (texture);
  g_object_unref (source);
  g_bytes_unref (cut);
  g_bytes_unref (png);
}

static void
test_off_main_thread (void)
{
  GdkTexture *source = noise_texture (2000, 1500, GDK_MEMORY_R8G8B8, 3);
  GBytes *png = gdk_texture_save_to_png_bytes (source);
  gint64 start, created, sync_time;
  GdkTexture *texture, *reference;

  start = g_get_monotonic_time ();
  reference = gdk_load_png (png, NULL, NULL);
  sync_time = g_get_monotonic_time () - start;

  start = g_get_monotonic_time ();
  texture = gdk_texture_new_from_bytes (png, NULL);
  created = g_get_monotonic_time () - start;

  g_test_message ("2000x1500 png: decode %.1f ms on the caller before, %.1f ms now",
                  sync_time / 1000.0, created / 1000.0);
  g_assert_true (GDK_IS_DEFERRED_TEXTURE (texture));
  g_assert_cmpint (created * 5, <, sync_time);

  g_object_unref (texture);
  g_object_unref (reference);
  g_object_unref (source);
  g_bytes_unref (png);
}

int
main (int argc, char *argv[])
{
  (g_test_init) (&argc, &argv, NULL);
  g_main_context_acquire (NULL);

  g_test_add_func ("/deferredtexture/png", test_png_matches);
  g_test_add_func ("/deferredtexture/png16", test_png16);
  g_test_add_func ("/deferredtexture/jpeg", test_jpeg_matches);
  g_test_add_func ("/deferredtexture/small", test_small_stays_sync);
  g_test_add_func ("/deferredtexture/worker-thread", test_worker_thread_stays_sync);
  g_test_add_func ("/deferredtexture/truncated", test_truncated);
  g_test_add_func ("/deferredtexture/off-main-thread", test_off_main_thread);

  return g_test_run ();
}
