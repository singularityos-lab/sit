#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <stdio.h>

static char *root;

static void
make_files (const char *dir,
            guint       n)
{
  g_mkdir_with_parents (dir, 0700);
  for (guint i = 0; i < n; i++)
    {
      char *path = g_strdup_printf ("%s/file-%06u.txt", dir, i);

      g_file_set_contents (path, "x", 1, NULL);
      g_free (path);
    }
}

static gint64
wait_loaded (GtkDirectoryList *list)
{
  gint64 deadline = g_get_monotonic_time () + 30 * G_TIME_SPAN_SECOND;
  gint64 longest = 0;

  while (gtk_directory_list_is_loading (list))
    {
      gint64 start = g_get_monotonic_time ();

      g_main_context_iteration (NULL, FALSE);
      longest = MAX (longest, g_get_monotonic_time () - start);
      g_assert_cmpint (g_get_monotonic_time (), <, deadline);
    }

  return longest;
}

static void
count_changes (GListModel *model,
               guint       position,
               guint       removed,
               guint       added,
               guint      *counter)
{
  (*counter)++;
}

static void
test_large (void)
{
  char *dir = g_build_filename (root, "large", NULL);
  GFile *file = g_file_new_for_path (dir);
  GtkDirectoryList *list;
  GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  guint changes = 0, n;
  gint64 start, longest;

  make_files (dir, 20000);
  list = gtk_directory_list_new ("standard::name,standard::size", NULL);
  gtk_directory_list_set_monitored (list, FALSE);
  g_signal_connect (list, "items-changed", G_CALLBACK (count_changes), &changes);

  start = g_get_monotonic_time ();
  gtk_directory_list_set_file (list, file);
  longest = wait_loaded (list);
  start = g_get_monotonic_time () - start;

  n = g_list_model_get_n_items (G_LIST_MODEL (list));
  g_assert_cmpuint (n, ==, 20000);
  for (guint i = 0; i < n; i++)
    {
      GFileInfo *info = g_list_model_get_item (G_LIST_MODEL (list), i);
      GFile *f = G_FILE (g_file_info_get_attribute_object (info, "standard::file"));

      g_assert_nonnull (f);
      g_hash_table_add (seen, g_strdup (g_file_info_get_name (info)));
      g_object_unref (info);
    }
  g_assert_cmpuint (g_hash_table_size (seen), ==, 20000);
  g_assert_null (gtk_directory_list_get_error (list));

  g_test_message ("20000 files in %.1f ms, %u items-changed, longest main loop step %.2f ms",
                  start / 1000.0, changes, longest / 1000.0);

  g_hash_table_unref (seen);
  g_object_unref (list);
  g_object_unref (file);
  g_free (dir);
}

static void
test_restart_and_dispose (void)
{
  char *dir = g_build_filename (root, "large", NULL);
  char *other = g_build_filename (root, "small", NULL);
  GFile *file = g_file_new_for_path (dir);
  GFile *small = g_file_new_for_path (other);

  make_files (other, 10);
  for (guint round = 0; round < 10; round++)
    {
      GtkDirectoryList *list = gtk_directory_list_new ("standard::name", file);

      gtk_directory_list_set_monitored (list, FALSE);
      for (guint i = 0; i < round; i++)
        g_main_context_iteration (NULL, FALSE);
      if (round % 2)
        {
          gtk_directory_list_set_file (list, small);
          wait_loaded (list);
          g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (list)), ==, 10);
        }
      g_object_unref (list);
    }

  for (guint i = 0; i < 200; i++)
    g_main_context_iteration (NULL, FALSE);

  g_object_unref (file);
  g_object_unref (small);
  g_free (dir);
  g_free (other);
}

static void
test_error (void)
{
  char *dir = g_build_filename (root, "missing", NULL);
  GFile *file = g_file_new_for_path (dir);
  GtkDirectoryList *list = gtk_directory_list_new ("standard::name", file);

  wait_loaded (list);
  g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (list)), ==, 0);
  g_assert_error (gtk_directory_list_get_error (list), G_IO_ERROR, G_IO_ERROR_NOT_FOUND);

  g_object_unref (list);
  g_object_unref (file);
  g_free (dir);
}

static gboolean
has_name (GListModel *model,
          const char *name)
{
  for (guint i = 0; i < g_list_model_get_n_items (model); i++)
    {
      GFileInfo *info = g_list_model_get_item (model, i);
      gboolean match = g_str_equal (g_file_info_get_name (info), name);

      g_object_unref (info);
      if (match)
        return TRUE;
    }

  return FALSE;
}

static void
spin_until_name (GListModel *model,
                 const char *name,
                 gboolean    present)
{
  gint64 deadline = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;

  while (has_name (model, name) != present)
    {
      g_main_context_iteration (NULL, FALSE);
      g_usleep (1000);
      g_assert_cmpint (g_get_monotonic_time (), <, deadline);
    }
}

static void
monitor_event (GFileMonitor      *monitor,
               GFile             *file,
               GFile             *other,
               GFileMonitorEvent  event,
               gboolean          *seen)
{
  *seen = TRUE;
}

static gboolean
monitors_work (const char *dir)
{
  GFile *file = g_file_new_for_path (dir);
  GFileMonitor *monitor = g_file_monitor_directory (file, G_FILE_MONITOR_NONE, NULL, NULL);
  char *path = g_build_filename (dir, "probe.txt", NULL);
  gboolean seen = FALSE;
  gint64 deadline = g_get_monotonic_time () + 2 * G_TIME_SPAN_SECOND;

  if (monitor)
    {
      g_signal_connect (monitor, "changed", G_CALLBACK (monitor_event), &seen);
      g_file_set_contents (path, "p", 1, NULL);
      while (!seen && g_get_monotonic_time () < deadline)
        {
          g_main_context_iteration (NULL, FALSE);
          g_usleep (1000);
        }
      g_unlink (path);
      for (int i = 0; i < 50; i++)
        g_main_context_iteration (NULL, FALSE);
      g_object_unref (monitor);
    }

  g_object_unref (file);
  g_free (path);
  return seen;
}

static void
test_monitor (void)
{
  char *dir = g_build_filename (root, "watched", NULL);
  char *path = g_build_filename (dir, "new.txt", NULL);
  GFile *file = g_file_new_for_path (dir);
  GtkDirectoryList *list;

  make_files (dir, 300);
  if (!monitors_work (dir))
    {
      g_test_skip ("file monitors deliver no events here");
      g_object_unref (file);
      g_free (path);
      g_free (dir);
      return;
    }
  list = gtk_directory_list_new ("standard::name", file);
  wait_loaded (list);
  g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (list)), ==, 300);

  {
    FILE *f = g_fopen (path, "w");

    fputs ("y", f);
    fclose (f);
  }
  spin_until_name (G_LIST_MODEL (list), "new.txt", TRUE);
  g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (list)), ==, 301);

  g_unlink (path);
  spin_until_name (G_LIST_MODEL (list), "new.txt", FALSE);
  g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (list)), ==, 300);

  for (guint i = 0; i < 5; i++)
    {
      char *saved = g_build_filename (dir, "saved.txt", NULL);

      g_file_set_contents (saved, "atomic", -1, NULL);
      g_free (saved);
    }
  spin_until_name (G_LIST_MODEL (list), "saved.txt", TRUE);

  {
    FILE *f = g_fopen (path, "w");

    fputs ("z", f);
    fclose (f);
  }
  spin_until_name (G_LIST_MODEL (list), "new.txt", TRUE);

  g_object_unref (list);
  g_object_unref (file);
  g_free (path);
  g_free (dir);
}

int
main (int argc, char *argv[])
{
  char *cwd;
  int result;

  gtk_test_init (&argc, &argv, NULL);

  cwd = g_get_current_dir ();
  root = g_build_filename (cwd, "sitdirlist-XXXXXX", NULL);
  g_free (cwd);
  g_assert_nonnull (g_mkdtemp (root));

  g_test_add_func ("/sit/dirlist/large", test_large);
  g_test_add_func ("/sit/dirlist/restart-and-dispose", test_restart_and_dispose);
  g_test_add_func ("/sit/dirlist/error", test_error);
  g_test_add_func ("/sit/dirlist/monitor", test_monitor);

  result = g_test_run ();

  {
    char *argv_rm[] = { (char *) "rm", (char *) "-rf", root, NULL };
    g_spawn_sync (NULL, argv_rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  }
  g_free (root);

  return result;
}
