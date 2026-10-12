#include <gtk/gtk.h>
#include <string.h>

#include "gtk/gtkrootprivate.h"
#include "gtk/gtkconstraintsolverprivate.h"

static guint
solver_stat (GtkWidget  *window,
             const char *name)
{
  char *stats = gtk_constraint_solver_statistics (gtk_root_get_constraint_solver (GTK_ROOT (window)));
  const char *rows = strstr (stats, name);
  guint count;

  g_assert_nonnull (rows);
  count = g_ascii_strtoull (rows + strlen (name), NULL, 10);
  g_free (stats);

  return count;
}

static void
test_root (gconstpointer data)
{
  guint flags = GPOINTER_TO_UINT (data);
  GtkWidget *windows[2] = { gtk_window_new (), gtk_window_new () };
  GtkWidget *content = g_object_ref_sink (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  GtkConstraintLayout *layout = GTK_CONSTRAINT_LAYOUT (gtk_constraint_layout_new ());
  GtkConstraintGuide *guide = NULL;
  GtkWidget *hosts[2] = { NULL, NULL };
  GtkWidget *remaining[2] = { NULL, NULL };
  gpointer source;
  guint base_rows[2];
  guint base_columns[2];
  guint base_parametric[2];
  guint active_rows = 0;
  int expected_min[2] = { 0, 0 };
  int expected_nat[2] = { 100, 30 };

  gtk_widget_set_layout_manager (content, GTK_LAYOUT_MANAGER (layout));
  for (guint i = 0; i < 2; i++)
    {
      if (flags & 8)
        {
          GtkConstraintLayout *other = GTK_CONSTRAINT_LAYOUT (gtk_constraint_layout_new ());

          hosts[i] = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
          remaining[i] = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
          gtk_widget_set_layout_manager (remaining[i], GTK_LAYOUT_MANAGER (other));
          gtk_constraint_layout_add_constraint (other,
            gtk_constraint_new_constant (NULL, GTK_CONSTRAINT_ATTRIBUTE_WIDTH,
                                         GTK_CONSTRAINT_RELATION_EQ, 77,
                                         GTK_CONSTRAINT_STRENGTH_REQUIRED));
          gtk_constraint_layout_add_constraint (other,
            gtk_constraint_new_constant (NULL, GTK_CONSTRAINT_ATTRIBUTE_HEIGHT,
                                         GTK_CONSTRAINT_RELATION_EQ, 23,
                                         GTK_CONSTRAINT_STRENGTH_REQUIRED));
          gtk_box_append (GTK_BOX (hosts[i]), remaining[i]);
          gtk_window_set_child (GTK_WINDOW (windows[i]), hosts[i]);
        }
      base_rows[i] = solver_stat (windows[i], "\nRows: ");
      base_columns[i] = solver_stat (windows[i], "\nColumns: ");
      base_parametric[i] = solver_stat (windows[i], "\nExternal parametric variables: ");
    }

  if (flags & 4)
    {
      GtkWidget *child = gtk_drawing_area_new ();

      gtk_drawing_area_set_content_width (GTK_DRAWING_AREA (child), 100);
      gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (child), 30);
      gtk_widget_measure (child, GTK_ORIENTATION_HORIZONTAL, -1,
                          &expected_min[0], &expected_nat[0], NULL, NULL);
      gtk_widget_measure (child, GTK_ORIENTATION_VERTICAL, -1,
                          &expected_min[1], &expected_nat[1], NULL, NULL);
      gtk_box_append (GTK_BOX (content), child);
      source = child;
    }
  else
    {
      guide = gtk_constraint_guide_new ();
      gtk_constraint_guide_set_nat_size (guide, 100, 30);
      gtk_constraint_layout_add_guide (layout, guide);
      source = guide;
    }

  gtk_constraint_layout_add_constraint (layout,
    gtk_constraint_new (NULL, GTK_CONSTRAINT_ATTRIBUTE_WIDTH, GTK_CONSTRAINT_RELATION_EQ,
                        source, GTK_CONSTRAINT_ATTRIBUTE_WIDTH, 1.0, 0.0,
                        GTK_CONSTRAINT_STRENGTH_REQUIRED));
  gtk_constraint_layout_add_constraint (layout,
    gtk_constraint_new (NULL, GTK_CONSTRAINT_ATTRIBUTE_HEIGHT, GTK_CONSTRAINT_RELATION_EQ,
                        source, GTK_CONSTRAINT_ATTRIBUTE_HEIGHT, 1.0, 0.0,
                        GTK_CONSTRAINT_STRENGTH_REQUIRED));
  if (flags & 2)
    {
      GtkConstraintAttribute attributes[] = {
        GTK_CONSTRAINT_ATTRIBUTE_RIGHT, GTK_CONSTRAINT_ATTRIBUTE_BOTTOM,
        GTK_CONSTRAINT_ATTRIBUTE_CENTER_X, GTK_CONSTRAINT_ATTRIBUTE_CENTER_Y
      };

      for (guint i = 0; i < G_N_ELEMENTS (attributes); i++)
        gtk_constraint_layout_add_constraint (layout,
          gtk_constraint_new (NULL, attributes[i], GTK_CONSTRAINT_RELATION_EQ,
                              source, attributes[i], 1.0, 0.0,
                              GTK_CONSTRAINT_STRENGTH_REQUIRED));
    }

  for (guint i = 0; i < 32; i++)
    {
      guint target = flags & 1 ? i % 2 : 0;

      if (guide)
        {
          expected_nat[0] = 100 + i % 13;
          gtk_constraint_guide_set_nat_size (guide, expected_nat[0], 30);
        }
      if (hosts[target])
        gtk_box_append (GTK_BOX (hosts[target]), content);
      else
        gtk_window_set_child (GTK_WINDOW (windows[target]), content);
      gtk_widget_queue_resize (content);
      for (GtkOrientation orientation = GTK_ORIENTATION_HORIZONTAL;
           orientation <= GTK_ORIENTATION_VERTICAL; orientation++)
        {
          int minimum, natural;

          gtk_widget_measure (content, orientation, -1, &minimum, &natural, NULL, NULL);
          g_assert_cmpint (minimum, ==, expected_min[orientation]);
          g_assert_cmpint (natural, ==, expected_nat[orientation]);
        }

      if (i == 0)
        active_rows = solver_stat (windows[target], "\nRows: ");
      else
        g_assert_cmpuint (solver_stat (windows[target], "\nRows: "), ==, active_rows);
      if (hosts[target])
        gtk_box_remove (GTK_BOX (hosts[target]), content);
      else
        gtk_window_set_child (GTK_WINDOW (windows[target]), NULL);
      if (remaining[target])
        {
          gtk_widget_queue_resize (remaining[target]);
          for (GtkOrientation orientation = GTK_ORIENTATION_HORIZONTAL;
               orientation <= GTK_ORIENTATION_VERTICAL; orientation++)
            {
              int minimum, natural;
              int expected = orientation == GTK_ORIENTATION_HORIZONTAL ? 77 : 23;

              gtk_widget_measure (remaining[target], orientation, -1,
                                  &minimum, &natural, NULL, NULL);
              g_assert_cmpint (minimum, ==, expected);
              g_assert_cmpint (natural, ==, expected);
            }
        }
      g_assert_cmpuint (solver_stat (windows[target], "\nRows: "), ==, base_rows[target]);
      g_assert_cmpuint (solver_stat (windows[1 - target], "\nRows: "), ==, base_rows[1 - target]);
      g_assert_cmpuint (solver_stat (windows[target], "\nColumns: "), ==, base_columns[target]);
      g_assert_cmpuint (solver_stat (windows[target], "\nExternal parametric variables: "), ==, base_parametric[target]);
    }

  g_object_unref (content);
  gtk_window_destroy (GTK_WINDOW (windows[0]));
  gtk_window_destroy (GTK_WINDOW (windows[1]));
}

static GtkWidget *
create_content (GtkConstraintGuide **guide)
{
  GtkWidget *content = g_object_ref_sink (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  GtkConstraintLayout *layout = GTK_CONSTRAINT_LAYOUT (gtk_constraint_layout_new ());
  gpointer source;

  gtk_widget_set_layout_manager (content, GTK_LAYOUT_MANAGER (layout));
  if (guide)
    {
      *guide = gtk_constraint_guide_new ();
      gtk_constraint_guide_set_nat_size (*guide, 100, 30);
      gtk_constraint_layout_add_guide (layout, *guide);
      source = *guide;
    }
  else
    {
      guchar pixels[100 * 30 * 4] = { 0 };
      GBytes *bytes = g_bytes_new (pixels, sizeof pixels);
      GdkTexture *texture = gdk_memory_texture_new (100, 30, GDK_MEMORY_R8G8B8A8, bytes, 100 * 4);
      GtkWidget *child = gtk_picture_new_for_paintable (GDK_PAINTABLE (texture));

      g_object_unref (texture);
      g_bytes_unref (bytes);
      gtk_box_append (GTK_BOX (content), child);
      source = child;
    }

  gtk_constraint_layout_add_constraint (layout,
    gtk_constraint_new (NULL, GTK_CONSTRAINT_ATTRIBUTE_WIDTH, GTK_CONSTRAINT_RELATION_EQ,
                        source, GTK_CONSTRAINT_ATTRIBUTE_WIDTH, 1.0, 0.0,
                        GTK_CONSTRAINT_STRENGTH_REQUIRED));
  gtk_constraint_layout_add_constraint (layout,
    gtk_constraint_new (NULL, GTK_CONSTRAINT_ATTRIBUTE_HEIGHT, GTK_CONSTRAINT_RELATION_EQ,
                        source, GTK_CONSTRAINT_ATTRIBUTE_HEIGHT, 1.0, 0.0,
                        GTK_CONSTRAINT_STRENGTH_REQUIRED));

  return content;
}

static void
assert_measure (GtkWidget *content,
                int        min_width,
                int        nat_width,
                int        min_height,
                int        nat_height)
{
  int minimum, natural;

  gtk_widget_queue_resize (content);
  gtk_widget_measure (content, GTK_ORIENTATION_HORIZONTAL, -1, &minimum, &natural, NULL, NULL);
  g_assert_cmpint (minimum, ==, min_width);
  g_assert_cmpint (natural, ==, nat_width);
  gtk_widget_queue_resize (content);
  gtk_widget_measure (content, GTK_ORIENTATION_VERTICAL, -1, &minimum, &natural, NULL, NULL);
  g_assert_cmpint (minimum, ==, min_height);
  g_assert_cmpint (natural, ==, nat_height);
}

static void
test_natural (gconstpointer data)
{
  guint flags = GPOINTER_TO_UINT (data);
  GtkWidget *window = gtk_window_new ();
  GtkConstraintGuide *guide = NULL;
  GtkWidget *content = create_content (flags & 1 ? NULL : &guide);
  int expected[2] = { 100, 30 };

  gtk_window_set_child (GTK_WINDOW (window), content);
  for (guint i = 0; i < 32; i++)
    {
      if (guide)
        {
          if (flags & 2)
            {
              expected[1] = 30 + i % 13;
              gtk_constraint_guide_set_nat_size (guide, -1, expected[1]);
            }
          else
            {
              expected[0] = 100 + i % 13;
              gtk_constraint_guide_set_nat_size (guide, expected[0], -1);
            }
        }

      if (flags & 4)
        {
          gtk_widget_allocate (content, 200 + i, 80 + i, -1, NULL);
          if (!guide)
            {
              GtkWidget *child = gtk_widget_get_first_child (content);

              g_assert_cmpint (gtk_widget_get_width (child), ==, 200 + i);
              g_assert_cmpint (gtk_widget_get_height (child), ==, 80 + i);
            }
        }

      for (guint step = 0; step < 2; step++)
        {
          GtkOrientation orientation = flags & 2 ? 1 - step : step;
          int minimum, natural;

          gtk_widget_queue_resize (content);
          gtk_widget_measure (content, orientation, -1, &minimum, &natural, NULL, NULL);
          g_assert_cmpint (minimum, ==, 0);
          g_assert_cmpint (natural, ==, expected[orientation]);
        }
    }

  gtk_window_destroy (GTK_WINDOW (window));
  g_object_unref (content);
}

static void
test_guide_limits (void)
{
  GtkWidget *window = gtk_window_new ();
  GtkConstraintGuide *guide;
  GtkWidget *content = create_content (&guide);

  gtk_constraint_guide_set_min_size (guide, 20, 10);
  gtk_constraint_guide_set_max_size (guide, 120, 40);
  gtk_window_set_child (GTK_WINDOW (window), content);
  assert_measure (content, 20, 100, 10, 30);
  gtk_constraint_guide_set_nat_size (guide, 180, 90);
  assert_measure (content, 20, 120, 10, 40);
  gtk_constraint_guide_set_nat_size (guide, 5, 2);
  assert_measure (content, 20, 20, 10, 10);
  gtk_constraint_guide_set_min_size (guide, 50, 25);
  gtk_constraint_guide_set_max_size (guide, 50, 25);
  assert_measure (content, 50, 50, 25, 25);
  gtk_constraint_guide_set_max_size (guide, 120, 40);
  gtk_constraint_guide_set_nat_size (guide, 100, 30);
  assert_measure (content, 50, 100, 25, 30);

  gtk_window_destroy (GTK_WINDOW (window));
  g_object_unref (content);
}

static void
test_guide_strength (void)
{
  GtkWidget *window = gtk_window_new ();
  GtkConstraintGuide *guide;
  GtkWidget *content = create_content (&guide);
  GtkConstraintLayout *layout = GTK_CONSTRAINT_LAYOUT (gtk_widget_get_layout_manager (content));

  gtk_constraint_guide_set_min_size (guide, 20, 10);
  gtk_constraint_layout_add_constraint (layout,
    gtk_constraint_new_constant (NULL, GTK_CONSTRAINT_ATTRIBUTE_WIDTH,
                                 GTK_CONSTRAINT_RELATION_EQ, 60,
                                 GTK_CONSTRAINT_STRENGTH_MEDIUM));
  gtk_window_set_child (GTK_WINDOW (window), content);
  gtk_constraint_guide_set_strength (guide, GTK_CONSTRAINT_STRENGTH_WEAK);
  assert_measure (content, 20, 60, 10, 30);
  gtk_constraint_guide_set_strength (guide, GTK_CONSTRAINT_STRENGTH_STRONG);
  assert_measure (content, 20, 100, 10, 30);
  gtk_constraint_guide_set_strength (guide, GTK_CONSTRAINT_STRENGTH_REQUIRED);
  assert_measure (content, 100, 100, 30, 30);
  gtk_constraint_guide_set_strength (guide, GTK_CONSTRAINT_STRENGTH_WEAK);
  assert_measure (content, 20, 60, 10, 30);

  gtk_window_destroy (GTK_WINDOW (window));
  g_object_unref (content);
}

int
main (int argc, char **argv)
{
  gtk_test_init (&argc, &argv);
  for (guint flags = 0; flags < 16; flags++)
    {
      char *path = g_strdup_printf ("/constraint-layout/root/%s/%s/%s/%s",
                                    flags & 4 ? "child" : "guide",
                                    flags & 2 ? "derived" : "size",
                                    flags & 1 ? "different" : "same",
                                    flags & 8 ? "remaining" : "empty");

      g_test_add_data_func (path, GUINT_TO_POINTER (flags), test_root);
      g_free (path);
    }
  for (guint flags = 0; flags < 8; flags++)
    {
      char *path = g_strdup_printf ("/constraint-layout/natural/%s/%s/%s",
                                    flags & 1 ? "child" : "guide",
                                    flags & 2 ? "vertical-first" : "horizontal-first",
                                    flags & 4 ? "allocate" : "measure");

      g_test_add_data_func (path, GUINT_TO_POINTER (flags), test_natural);
      g_free (path);
    }
  g_test_add_func ("/constraint-layout/guide/limits", test_guide_limits);
  g_test_add_func ("/constraint-layout/guide/strength", test_guide_strength);
  return g_test_run ();
}
