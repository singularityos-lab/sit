#include <gtk/gtk.h>
#include <glib/gstdio.h>

#include "gtk/gtksearchengineprivate.h"
#include "gtk/gtkquery.h"

static char *root;

static void
touch (const char *path)
{
  char *dir = g_path_get_dirname (path);

  g_mkdir_with_parents (dir, 0700);
  g_file_set_contents (path, "x", 1, NULL);
  g_free (dir);
}

typedef struct
{
  GHashTable *names;
  gboolean finished;
  gboolean got_results;
  guint batches;
} Collect;

static void
hits_added (GtkSearchEngine *engine,
            GList           *hits,
            Collect         *c)
{
  c->batches++;
  for (GList *l = hits; l; l = l->next)
    {
      GtkSearchHit *hit = l->data;

      g_assert_nonnull (hit->info);
      g_assert_nonnull (g_file_info_get_display_name (hit->info));
      g_hash_table_add (c->names, g_file_get_basename (hit->file));
    }
}

static void
finished (GtkSearchEngine *engine,
          gboolean         got_results,
          Collect         *c)
{
  c->finished = TRUE;
  c->got_results = got_results;
}

static gboolean
sit_off (void)
{
  return g_strcmp0 (g_getenv ("SIT_ASYNC"), "off") == 0;
}

static void
run_query (const char *text,
           Collect    *c)
{
  GtkSearchEngine *engine = _gtk_search_engine_new ();
  GtkQuery *query = gtk_query_new ();
  GFile *location = g_file_new_for_path (root);
  gint64 deadline = g_get_monotonic_time () + 20 * G_TIME_SPAN_SECOND;

  c->names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  gtk_query_set_text (query, text);
  gtk_query_set_location (query, location);
  _gtk_search_engine_set_query (engine, query);
  g_signal_connect (engine, "hits-added", G_CALLBACK (hits_added), c);
  g_signal_connect (engine, "finished", G_CALLBACK (finished), c);
  _gtk_search_engine_start (engine);

  while (!c->finished)
    {
      g_main_context_iteration (NULL, TRUE);
      g_assert_cmpint (g_get_monotonic_time (), <, deadline);
    }

  g_object_unref (engine);
  g_object_unref (query);
  g_object_unref (location);
}

static void
test_recursive (void)
{
  Collect c = { 0 };

  if (sit_off ())
    {
      g_test_skip ("no recursive engine without SIT");
      return;
    }

  run_query ("report", &c);

  g_assert_true (c.got_results);
  g_assert_cmpuint (g_hash_table_size (c.names), ==, 3);
  g_assert_true (g_hash_table_contains (c.names, "Report-2025.odt"));
  g_assert_true (g_hash_table_contains (c.names, "annual report.pdf"));
  g_assert_true (g_hash_table_contains (c.names, "reports"));
  g_assert_false (g_hash_table_contains (c.names, "hidden-report.txt"));
  g_test_message ("3 hits in %u batches", c.batches);
  g_hash_table_unref (c.names);
}

static void
test_words (void)
{
  Collect c = { 0 };

  if (sit_off ())
    {
      g_test_skip ("no recursive engine without SIT");
      return;
    }

  run_query ("annual pdf", &c);
  g_assert_cmpuint (g_hash_table_size (c.names), ==, 1);
  g_assert_true (g_hash_table_contains (c.names, "annual report.pdf"));
  g_hash_table_unref (c.names);
}

static void
test_empty (void)
{
  Collect c = { 0 };

  if (sit_off ())
    {
      g_test_skip ("no recursive engine without SIT");
      return;
    }

  run_query ("", &c);
  g_assert_false (c.got_results);
  g_hash_table_unref (c.names);
}

static void
test_stop_restart (void)
{
  GtkSearchEngine *engine = _gtk_search_engine_new ();
  GtkQuery *query = gtk_query_new ();
  GFile *location = g_file_new_for_path (root);

  gtk_query_set_location (query, location);
  _gtk_search_engine_set_query (engine, query);
  for (guint i = 0; i < 20; i++)
    {
      gtk_query_set_text (query, i % 2 ? "file" : "report");
      _gtk_search_engine_start (engine);
      g_main_context_iteration (NULL, FALSE);
      _gtk_search_engine_stop (engine);
    }
  g_object_unref (engine);

  for (guint i = 0; i < 200; i++)
    g_main_context_iteration (NULL, FALSE);

  g_object_unref (query);
  g_object_unref (location);
}

int
main (int argc, char *argv[])
{
  char *cwd;
  int result;

  gtk_test_init (&argc, &argv, NULL);

  cwd = g_get_current_dir ();
  root = g_build_filename (cwd, "sitsearch-XXXXXX", NULL);
  g_free (cwd);
  g_assert_nonnull (g_mkdtemp (root));

  {
    const char *files[] = {
      "Documents/Report-2025.odt",
      "Documents/work/deep/er/annual report.pdf",
      "reports/notes.txt",
      ".cache/hidden-report.txt",
      "Music/song.mp3",
      NULL
    };

    for (guint i = 0; files[i]; i++)
      {
        char *path = g_build_filename (root, files[i], NULL);

        touch (path);
        g_free (path);
      }
    for (guint i = 0; i < 2000; i++)
      {
        char *path = g_strdup_printf ("%s/bulk/d%02u/file-%04u.txt", root, i % 50, i);

        touch (path);
        g_free (path);
      }
  }

  g_test_add_func ("/sit/search/recursive", test_recursive);
  g_test_add_func ("/sit/search/words", test_words);
  g_test_add_func ("/sit/search/empty", test_empty);
  g_test_add_func ("/sit/search/stop-restart", test_stop_restart);

  result = g_test_run ();

  {
    char *argv_rm[] = { (char *) "rm", (char *) "-rf", root, NULL };
    g_spawn_sync (NULL, argv_rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  }
  g_free (root);

  return result;
}
