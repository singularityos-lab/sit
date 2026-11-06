#include <gtk/gtk.h>

static GListModel *
make_strings (guint n,
              guint seed)
{
  GtkStringList *list = gtk_string_list_new (NULL);
  GRand *rand = g_rand_new_with_seed (seed);

  for (guint i = 0; i < n; i++)
    {
      char *s = g_strdup_printf ("item %08x", g_rand_int (rand));
      gtk_string_list_take (list, s);
    }

  g_rand_free (rand);
  return G_LIST_MODEL (list);
}

static GtkSorter *
string_sorter (void)
{
  return GTK_SORTER (gtk_string_sorter_new (gtk_property_expression_new (GTK_TYPE_STRING_OBJECT, NULL, "string")));
}

static void
assert_sorted (GListModel *model,
               guint       n)
{
  char *last = NULL;

  g_assert_cmpuint (g_list_model_get_n_items (model), ==, n);
  for (guint i = 0; i < n; i++)
    {
      GtkStringObject *item = g_list_model_get_item (model, i);
      const char *s = gtk_string_object_get_string (item);

      if (last)
        g_assert_cmpint (g_utf8_collate (last, s), <=, 0);
      g_free (last);
      last = g_strdup (s);
      g_object_unref (item);
    }
  g_free (last);
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
wait_sorted (GtkSortListModel *model)
{
  gint64 deadline = g_get_monotonic_time () + 20 * G_TIME_SPAN_SECOND;

  while (gtk_sort_list_model_get_pending (model) > 0)
    {
      g_assert_cmpint (g_get_monotonic_time (), <, deadline);
      g_main_context_iteration (NULL, TRUE);
    }
}

static double
time_sort (guint    n,
           gboolean incremental)
{
  GListModel *strings = make_strings (n, 7);
  GtkSortListModel *model = gtk_sort_list_model_new (strings, NULL);
  gint64 start;

  gtk_sort_list_model_set_incremental (model, incremental);
  start = g_get_monotonic_time ();
  gtk_sort_list_model_set_sorter (model, string_sorter ());
  if (incremental)
    wait_sorted (model);
  start = g_get_monotonic_time () - start;

  assert_sorted (G_LIST_MODEL (model), n);
  g_object_unref (model);

  return start / 1000.0;
}

static void
test_large_sync (void)
{
  double ms = time_sort (200000, FALSE);

  g_test_message ("200000 strings sorted synchronously in %.1f ms", ms);
}

static void
test_incremental (void)
{
  GListModel *strings = make_strings (60000, 11);
  GtkSortListModel *model = gtk_sort_list_model_new (strings, NULL);
  guint changes = 0;
  gint64 longest = 0, last, now;

  gtk_sort_list_model_set_incremental (model, TRUE);
  g_signal_connect (model, "items-changed", G_CALLBACK (count_changes), &changes);
  gtk_sort_list_model_set_sorter (model, string_sorter ());

  while (gtk_sort_list_model_get_pending (model) > 0)
    {
      last = g_get_monotonic_time ();
      g_main_context_iteration (NULL, FALSE);
      now = g_get_monotonic_time ();
      longest = MAX (longest, now - last);
    }

  g_test_message ("60000 strings, incremental: %u items-changed, longest main loop step %.1f ms",
                  changes, longest / 1000.0);
  assert_sorted (G_LIST_MODEL (model), 60000);
  g_object_unref (model);
}

static void
test_mutate_while_sorting (void)
{
  GListModel *strings = make_strings (40000, 23);
  GtkSortListModel *model = gtk_sort_list_model_new (g_object_ref (strings), NULL);

  gtk_sort_list_model_set_incremental (model, TRUE);
  gtk_sort_list_model_set_sorter (model, string_sorter ());

  for (guint round = 0; round < 20; round++)
    {
      for (guint i = 0; i < 3; i++)
        g_main_context_iteration (NULL, FALSE);
      gtk_string_list_splice (GTK_STRING_LIST (strings), round * 7, 3,
                              (const char *[]) { "a", "zz", "m", NULL });
    }

  wait_sorted (model);
  assert_sorted (G_LIST_MODEL (model), 40000);

  gtk_sort_list_model_set_incremental (model, FALSE);
  assert_sorted (G_LIST_MODEL (model), 40000);

  g_object_unref (model);
  g_object_unref (strings);
}

static void
test_dispose_while_sorting (void)
{
  for (guint round = 0; round < 10; round++)
    {
      GtkSortListModel *model = gtk_sort_list_model_new (make_strings (30000, round), NULL);

      gtk_sort_list_model_set_incremental (model, TRUE);
      gtk_sort_list_model_set_sorter (model, string_sorter ());
      for (guint i = 0; i < round; i++)
        g_main_context_iteration (NULL, FALSE);
      g_object_unref (model);
    }

  for (guint i = 0; i < 50; i++)
    g_main_context_iteration (NULL, FALSE);
}

static int
compare_custom (gconstpointer a,
                gconstpointer b,
                gpointer      data)
{
  g_assert_true (g_thread_self () == data);

  return g_utf8_collate (gtk_string_object_get_string ((gpointer) a),
                         gtk_string_object_get_string ((gpointer) b));
}

static void
test_custom_stays_on_main (void)
{
  GListModel *strings = make_strings (30000, 5);
  GtkSorter *custom = GTK_SORTER (gtk_custom_sorter_new (compare_custom, g_thread_self (), NULL));
  GtkMultiSorter *multi = gtk_multi_sorter_new ();
  GtkSortListModel *model;

  gtk_multi_sorter_append (multi, string_sorter ());
  gtk_multi_sorter_append (multi, custom);
  model = gtk_sort_list_model_new (strings, GTK_SORTER (multi));
  assert_sorted (G_LIST_MODEL (model), 30000);

  gtk_sort_list_model_set_incremental (model, TRUE);
  gtk_sorter_changed (gtk_sort_list_model_get_sorter (model), GTK_SORTER_CHANGE_DIFFERENT);
  wait_sorted (model);
  assert_sorted (G_LIST_MODEL (model), 30000);

  g_object_unref (model);
}

static guint
get_number (GObject  *item,
            gpointer  data)
{
  return GPOINTER_TO_UINT (g_object_get_data (item, "n"));
}

static void
test_numeric (void)
{
  GListStore *store = g_list_store_new (GTK_TYPE_STRING_OBJECT);
  GtkSortListModel *model;
  GRand *rand = g_rand_new_with_seed (3);
  GtkSorter *sorter;

  for (guint i = 0; i < 50000; i++)
    {
      char *s = g_strdup_printf ("%u", g_rand_int_range (rand, 0, 1000000));
      GtkStringObject *obj = gtk_string_object_new (s);

      g_object_set_data_full (G_OBJECT (obj), "n", GUINT_TO_POINTER (atoi (s) + 1), NULL);
      g_list_store_append (store, obj);
      g_object_unref (obj);
      g_free (s);
    }
  g_rand_free (rand);

  sorter = GTK_SORTER (gtk_numeric_sorter_new (gtk_cclosure_expression_new (G_TYPE_UINT, NULL, 0, NULL,
                                                                            G_CALLBACK (get_number),
                                                                            NULL, NULL)));
  model = gtk_sort_list_model_new (G_LIST_MODEL (store), sorter);

  for (guint i = 1; i < 50000; i++)
    {
      gpointer a = g_list_model_get_item (G_LIST_MODEL (model), i - 1);
      gpointer b = g_list_model_get_item (G_LIST_MODEL (model), i);

      g_assert_cmpuint (GPOINTER_TO_UINT (g_object_get_data (a, "n")), <=,
                        GPOINTER_TO_UINT (g_object_get_data (b, "n")));
      g_object_unref (a);
      g_object_unref (b);
    }

  g_object_unref (model);
}

int
main (int argc, char *argv[])
{
  gtk_test_init (&argc, &argv, NULL);

  g_test_add_func ("/sit/sort/large-sync", test_large_sync);
  g_test_add_func ("/sit/sort/incremental", test_incremental);
  g_test_add_func ("/sit/sort/mutate-while-sorting", test_mutate_while_sorting);
  g_test_add_func ("/sit/sort/dispose-while-sorting", test_dispose_while_sorting);
  g_test_add_func ("/sit/sort/custom-stays-on-main", test_custom_stays_on_main);
  g_test_add_func ("/sit/sort/numeric", test_numeric);

  return g_test_run ();
}
