#include <gtk/gtk.h>

static void
check_finalized (gpointer data,
                 GObject *where_the_object_was)
{
  gboolean *did_finalize = (gboolean *)data;

  *did_finalize = TRUE;
}

static void
popover (void)
{
  GtkWidget *button = gtk_menu_button_new ();
  GtkWidget *p = gtk_popover_new ();
  gboolean finalized = FALSE;

  gtk_menu_button_set_popover (GTK_MENU_BUTTON (button), p);

  /* GtkButton is a normal widget and thus floating */
  g_assert_true (g_object_is_floating (button));
  /* GtkPopver sinks itself */
  g_assert_true (!g_object_is_floating (p));

  g_object_weak_ref (G_OBJECT (p), check_finalized, &finalized);

  g_object_ref_sink (button);
  g_object_unref (button);
  /* We do NOT unref p since the only reference held to it gets
   * removed when the button gets disposed. */
  g_assert_true (finalized);
}

static void
popover2 (void)
{
  GtkWidget *button = gtk_menu_button_new ();
  GtkWidget *p = gtk_popover_new ();
  gboolean finalized = FALSE;

  gtk_menu_button_set_popover (GTK_MENU_BUTTON (button), p);

  g_assert_true (g_object_is_floating (button));
  g_assert_true (!g_object_is_floating (p));

  g_object_weak_ref (G_OBJECT (p), check_finalized, &finalized);

  g_object_ref_sink (button);

  gtk_menu_button_set_popover (GTK_MENU_BUTTON (button), NULL);

  g_assert_true (finalized);

  g_object_unref (button);
}

static void
filechooserwidget (void)
{
  /* We use GtkFileChooserWidget simply because it's a complex widget, that's it. */
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  GtkWidget *w = gtk_file_chooser_widget_new (GTK_FILE_CHOOSER_ACTION_OPEN);
G_GNUC_END_IGNORE_DEPRECATIONS
  gboolean finalized = FALSE;

  g_assert_true (g_object_is_floating (w));
  g_object_ref_sink (w);
  g_object_weak_ref (G_OBJECT (w), check_finalized, &finalized);

  g_object_unref (w);

  g_assert_true (finalized);
}

static void
window (void)
{
  GtkWidget *w = gtk_window_new ();
  gboolean finalized = FALSE;

  /* GTK holds a ref */
  g_assert_true (!g_object_is_floating (w));
  g_object_weak_ref (G_OBJECT (w), check_finalized, &finalized);

  gtk_window_destroy (GTK_WINDOW (w));

  g_assert_true (finalized);
}

static void
notebook_labels (gconstpointer user_data)
{
  guint flags = GPOINTER_TO_UINT (user_data);
  guint action = flags >> 2;
  gboolean setters = flags & 1;
  gboolean floating = flags & 2;
  GtkNotebook *notebook = GTK_NOTEBOOK (g_object_ref_sink (gtk_notebook_new ()));
  GtkWidget *child = gtk_label_new ("child");
  GtkWidget *tab = gtk_label_new ("tab");
  GtkWidget *menu = g_object_ref_sink (gtk_label_new ("menu"));
  gboolean tab_finalized = FALSE;
  gboolean menu_finalized = FALSE;
  gboolean child_finalized = FALSE;
  gboolean notebook_finalized = FALSE;

  if (!floating)
    g_object_ref_sink (tab);

  g_object_weak_ref (G_OBJECT (tab), check_finalized, &tab_finalized);
  g_object_weak_ref (G_OBJECT (menu), check_finalized, &menu_finalized);
  g_object_weak_ref (G_OBJECT (child), check_finalized, &child_finalized);
  g_object_weak_ref (G_OBJECT (notebook), check_finalized, &notebook_finalized);

  if (setters)
    {
      gtk_notebook_append_page (notebook, child, NULL);
      gtk_notebook_set_tab_label (notebook, child, tab);
      gtk_notebook_set_menu_label (notebook, child, menu);
    }
  else
    gtk_notebook_append_page_menu (notebook, child, tab, menu);

  if (!floating)
    g_object_unref (tab);
  g_object_unref (menu);
  g_assert_false (tab_finalized);
  g_assert_false (menu_finalized);

  switch (action)
    {
    case 0:
      gtk_notebook_remove_page (notebook, -1);
      break;
    case 1:
      gtk_notebook_set_tab_label (notebook, child, NULL);
      gtk_notebook_set_menu_label (notebook, child, NULL);
      g_assert_true (tab_finalized);
      g_assert_true (menu_finalized);
      gtk_notebook_set_show_tabs (notebook, FALSE);
      gtk_notebook_set_show_tabs (notebook, TRUE);
      gtk_notebook_remove_page (notebook, -1);
      break;
    case 2:
      {
        gboolean replacement_finalized = FALSE;
        GtkWidget *replacement = gtk_label_new ("replacement");

        g_object_weak_ref (G_OBJECT (replacement), check_finalized, &replacement_finalized);
        gtk_notebook_set_tab_label (notebook, child, replacement);
        gtk_notebook_set_menu_label_text (notebook, child, "replacement");
        g_assert_true (tab_finalized);
        g_assert_true (menu_finalized);
        gtk_notebook_remove_page (notebook, -1);
        g_assert_true (replacement_finalized);
      }
      break;
    case 3:
      g_clear_object (&notebook);
      break;
    default:
      g_assert_not_reached ();
    }

  g_assert_true (tab_finalized);
  g_assert_true (menu_finalized);
  g_assert_true (child_finalized);
  g_clear_object (&notebook);
  g_assert_true (notebook_finalized);
}

static void
notebook_default_label (void)
{
  GtkNotebook *notebook = GTK_NOTEBOOK (g_object_ref_sink (gtk_notebook_new ()));
  GtkWidget *child = gtk_label_new ("child");
  GtkNotebookPage *page;

  gtk_notebook_append_page (notebook, child, NULL);
  page = gtk_notebook_get_page (notebook, child);

  for (guint i = 0; i < 3; i++)
    {
      GtkWidget *tab;
      gboolean finalized = FALSE;

      g_object_get (page, "tab", &tab, NULL);
      g_assert_true (GTK_IS_LABEL (tab));
      g_object_weak_ref (G_OBJECT (tab), check_finalized, &finalized);
      g_object_unref (tab);
      gtk_notebook_set_show_tabs (notebook, FALSE);
      g_assert_true (finalized);
      gtk_notebook_set_show_tabs (notebook, TRUE);
    }

  g_object_unref (notebook);
}

static void
notebook_floating_menu (gconstpointer user_data)
{
  GtkNotebook *notebook = GTK_NOTEBOOK (g_object_ref_sink (gtk_notebook_new ()));
  GtkWidget *child = gtk_label_new ("child");
  GtkWidget *menu = gtk_label_new ("menu");
  gboolean finalized = FALSE;

  if (GPOINTER_TO_UINT (user_data))
    gtk_notebook_popup_enable (notebook);

  g_object_weak_ref (G_OBJECT (menu), check_finalized, &finalized);
  gtk_notebook_append_page_menu (notebook, child, NULL, menu);
  gtk_notebook_remove_page (notebook, -1);
  g_assert_true (finalized);
  g_object_unref (notebook);
}

static void
notebook_same_menu (void)
{
  GtkNotebook *notebook = GTK_NOTEBOOK (g_object_ref_sink (gtk_notebook_new ()));
  GtkWidget *child = gtk_label_new ("child");
  GtkWidget *menu = gtk_label_new ("menu");
  gboolean finalized = FALSE;

  g_object_weak_ref (G_OBJECT (menu), check_finalized, &finalized);
  gtk_notebook_append_page (notebook, child, NULL);
  gtk_notebook_set_menu_label (notebook, child, menu);
  gtk_notebook_set_menu_label (notebook, child, gtk_notebook_get_menu_label (notebook, child));
  g_assert_false (finalized);
  gtk_notebook_remove_page (notebook, -1);
  g_assert_true (finalized);
  g_object_unref (notebook);
}

static void
emoji_completion_lifecycle (gconstpointer user_data)
{
  guint flags = GPOINTER_TO_UINT (user_data);

  for (guint i = 0; i < 16; i++)
    {
      GtkWidget *entry = g_object_ref_sink (gtk_entry_new ());
      GtkWidget *text = gtk_widget_get_first_child (entry);
      GtkWidget *completion = NULL;
      gboolean entry_finalized = FALSE;
      gboolean text_finalized = FALSE;
      gboolean completion_finalized = FALSE;

      g_object_weak_ref (G_OBJECT (entry), check_finalized, &entry_finalized);
      g_object_weak_ref (G_OBJECT (text), check_finalized, &text_finalized);
      if (flags & 1)
        {
          g_object_set (entry, "enable-emoji-completion", TRUE, NULL);
          completion = gtk_widget_get_last_child (text);
          g_assert_true (GTK_IS_POPOVER (completion));
          g_object_weak_ref (G_OBJECT (completion), check_finalized, &completion_finalized);
          if (flags & 4)
            g_object_ref (completion);
          if (flags & 8)
            g_object_freeze_notify (G_OBJECT (completion));
          if (flags & 2)
            g_object_set (entry, "enable-emoji-completion", FALSE, NULL);
        }

      g_object_unref (entry);
      g_assert_true (entry_finalized);
      g_assert_true (text_finalized);
      if (flags & 4)
        {
          g_assert_false (completion_finalized);
          g_assert_null (gtk_widget_get_parent (completion));
          if (flags & 8)
            g_object_thaw_notify (G_OBJECT (completion));
          g_object_run_dispose (G_OBJECT (completion));
          g_object_run_dispose (G_OBJECT (completion));
          g_object_unref (completion);
        }
      if (flags & 1)
        g_assert_true (completion_finalized);
    }
}

typedef struct {
  GtkWidget *entry;
  GtkWindow *window;
  gboolean called;
} EmojiActivation;

static void
emoji_closed_cb (GtkPopover      *popover,
                 EmojiActivation *data)
{
  data->called = TRUE;
  gtk_window_destroy (data->window);
}

static void
emoji_changed_cb (GtkEditable     *editable,
                  EmojiActivation *data)
{
  data->called = TRUE;
  g_object_set (data->entry, "enable-emoji-completion", FALSE, NULL);
}

static void
emoji_completion_activation (gconstpointer user_data)
{
  gboolean close_window = GPOINTER_TO_UINT (user_data);
  GtkWidget *entry = gtk_entry_new ();
  GtkWidget *text = gtk_widget_get_first_child (entry);
  GtkWindow *window = GTK_WINDOW (gtk_window_new ());
  GtkWidget *completion;
  GtkWidget *list;
  GtkListBoxRow *row;
  gboolean entry_finalized = FALSE;
  gboolean text_finalized = FALSE;
  gboolean completion_finalized = FALSE;
  EmojiActivation data = { entry, window, FALSE };
  gint64 deadline;

  g_object_set (gtk_settings_get_default (), "gtk-enable-animations", FALSE, NULL);
  g_object_weak_ref (G_OBJECT (entry), check_finalized, &entry_finalized);
  g_object_weak_ref (G_OBJECT (text), check_finalized, &text_finalized);
  g_object_set (entry, "enable-emoji-completion", TRUE, NULL);
  completion = g_object_ref (gtk_widget_get_last_child (text));
  g_object_weak_ref (G_OBJECT (completion), check_finalized, &completion_finalized);
  gtk_window_set_child (window, entry);
  gtk_window_present (window);
  gtk_widget_grab_focus (entry);

  deadline = g_get_monotonic_time () + G_USEC_PER_SEC;
  while (!gtk_widget_get_mapped (GTK_WIDGET (window)) && g_get_monotonic_time () < deadline)
    g_main_context_iteration (NULL, FALSE);
  g_assert_true (gtk_widget_get_mapped (GTK_WIDGET (window)));

  gtk_editable_set_text (GTK_EDITABLE (entry), ":grin");
  list = gtk_popover_get_child (GTK_POPOVER (completion));
  row = gtk_list_box_get_row_at_index (GTK_LIST_BOX (list), 0);
  g_assert_nonnull (row);
  if (close_window)
    g_signal_connect (completion, "closed", G_CALLBACK (emoji_closed_cb), &data);
  else
    g_signal_connect (entry, "changed", G_CALLBACK (emoji_changed_cb), &data);

  g_signal_emit_by_name (list, "row-activated", row);
  g_assert_true (data.called);
  if (!close_window)
    {
      g_assert_cmpstr (gtk_editable_get_text (GTK_EDITABLE (entry)), !=, ":grin");
      gtk_window_destroy (window);
    }

  g_assert_true (entry_finalized);
  g_assert_true (text_finalized);
  g_signal_handlers_disconnect_by_data (completion, &data);
  g_object_unref (completion);
  g_assert_true (completion_finalized);
}

int
main (int argc, char **argv)
{
  (g_test_init) (&argc, &argv, NULL);
  gtk_init ();

  g_test_add_func ("/gtk/widget-refcount/popover", popover);
  g_test_add_func ("/gtk/widget-refcount/popover2", popover2);
  g_test_add_func ("/gtk/widget-refcount/filechoosewidget", filechooserwidget);
  g_test_add_func ("/gtk/widget-refcount/window", window);

  for (guint flags = 0; flags < 16; flags++)
    {
      char *name = g_strdup_printf ("/gtk/widget-refcount/notebook/labels-%u", flags);

      g_test_add_data_func (name, GUINT_TO_POINTER (flags), notebook_labels);
      g_free (name);
    }

  g_test_add_func ("/gtk/widget-refcount/notebook/default", notebook_default_label);
  g_test_add_data_func ("/gtk/widget-refcount/notebook/menu-floating", GUINT_TO_POINTER (0), notebook_floating_menu);
  g_test_add_data_func ("/gtk/widget-refcount/notebook/menu-floating-popup", GUINT_TO_POINTER (1), notebook_floating_menu);
  g_test_add_func ("/gtk/widget-refcount/notebook/menu-same", notebook_same_menu);

  guint emoji_flags[] = { 0, 1, 3, 5, 7, 13, 15 };
  for (guint i = 0; i < G_N_ELEMENTS (emoji_flags); i++)
    {
      char *name = g_strdup_printf ("/gtk/widget-refcount/emoji/lifecycle-%u", emoji_flags[i]);

      g_test_add_data_func (name, GUINT_TO_POINTER (emoji_flags[i]), emoji_completion_lifecycle);
      g_free (name);
    }
  g_test_add_data_func ("/gtk/widget-refcount/emoji/close-on-activation", GUINT_TO_POINTER (1), emoji_completion_activation);
  g_test_add_data_func ("/gtk/widget-refcount/emoji/disable-on-activation", GUINT_TO_POINTER (0), emoji_completion_activation);

  return g_test_run ();
}
