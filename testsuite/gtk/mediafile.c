#include <gtk/gtk.h>

static void
test_audio (void)
{
  GtkMediaFile *file;
  GtkMediaStream *stream;

  file = GTK_MEDIA_FILE (gtk_media_file_new ());
  stream = GTK_MEDIA_STREAM (file);
  g_assert_false (gtk_media_stream_get_muted (stream));
  g_assert_cmpfloat (gtk_media_stream_get_volume (stream), ==, 1.0);

  gtk_media_stream_set_muted (stream, TRUE);
  gtk_media_stream_set_volume (stream, 0.25);
  g_assert_true (gtk_media_stream_get_muted (stream));
  g_assert_cmpfloat (gtk_media_stream_get_volume (stream), ==, 0.25);

  gtk_media_stream_set_muted (stream, FALSE);
  g_assert_false (gtk_media_stream_get_muted (stream));
  g_assert_cmpfloat (gtk_media_stream_get_volume (stream), ==, 0.25);
  g_object_unref (file);
}

static void
test_audio_construction (void)
{
  GtkMediaFile *file;
  GtkMediaFile *configured;

  file = GTK_MEDIA_FILE (gtk_media_file_new ());
  configured = g_object_new (G_OBJECT_TYPE (file),
                             "muted", TRUE,
                             "volume", 0.75,
                             NULL);
  g_assert_true (gtk_media_stream_get_muted (GTK_MEDIA_STREAM (configured)));
  g_assert_cmpfloat (gtk_media_stream_get_volume (GTK_MEDIA_STREAM (configured)), ==, 0.75);
  g_object_unref (configured);
  g_object_unref (file);
}

static void
test_audio_clamp (void)
{
  GtkMediaFile *file;
  GtkMediaStream *stream;

  file = GTK_MEDIA_FILE (gtk_media_file_new ());
  stream = GTK_MEDIA_STREAM (file);
  gtk_media_stream_set_volume (stream, -1.0);
  g_assert_cmpfloat (gtk_media_stream_get_volume (stream), ==, 0.0);
  gtk_media_stream_set_volume (stream, 2.0);
  g_assert_cmpfloat (gtk_media_stream_get_volume (stream), ==, 1.0);
  g_object_unref (file);
}

int
main (int argc, char *argv[])
{
  gtk_test_init (&argc, &argv, NULL);

  g_test_add_func ("/mediafile/audio", test_audio);
  g_test_add_func ("/mediafile/audio-construction", test_audio_construction);
  g_test_add_func ("/mediafile/audio-clamp", test_audio_clamp);

  return g_test_run ();
}
