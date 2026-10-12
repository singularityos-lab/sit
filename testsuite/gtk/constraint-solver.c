#include <locale.h>
#include <string.h>

#include <gtk/gtk.h>
#include "../../gtk/gtkconstrainttypesprivate.h"
#include "../../gtk/gtkconstraintsolverprivate.h"
#include "../../gtk/gtkconstraintexpressionprivate.h"

static void
constraint_solver_simple (void)
{
  GtkConstraintSolver *solver = gtk_constraint_solver_new ();

  GtkConstraintVariable *x = gtk_constraint_solver_create_variable (solver, NULL, "x", 167.0);
  GtkConstraintVariable *y = gtk_constraint_solver_create_variable (solver, NULL, "y", 2.0);

  GtkConstraintExpression *e = gtk_constraint_expression_new_from_variable (y);

  gtk_constraint_solver_add_constraint (solver,
                                        x, GTK_CONSTRAINT_RELATION_EQ, e,
                                        GTK_CONSTRAINT_STRENGTH_REQUIRED);

  double x_value = gtk_constraint_variable_get_value (x);
  double y_value = gtk_constraint_variable_get_value (y);

  g_assert_cmpfloat_with_epsilon (x_value, y_value, 0.001);
  g_assert_cmpfloat_with_epsilon (x_value, 0.0, 0.001);
  g_assert_cmpfloat_with_epsilon (y_value, 0.0, 0.001);

  gtk_constraint_variable_unref (y);
  gtk_constraint_variable_unref (x);

  g_object_unref (solver);
}

static void
constraint_solver_stay (void)
{
  GtkConstraintSolver *solver = gtk_constraint_solver_new ();

  GtkConstraintVariable *x = gtk_constraint_solver_create_variable (solver, NULL, "x", 5.0);
  GtkConstraintVariable *y = gtk_constraint_solver_create_variable (solver, NULL, "y", 10.0);

  gtk_constraint_solver_add_stay_variable (solver, x, GTK_CONSTRAINT_STRENGTH_WEAK);
  gtk_constraint_solver_add_stay_variable (solver, y, GTK_CONSTRAINT_STRENGTH_WEAK);

  double x_value = gtk_constraint_variable_get_value (x);
  double y_value = gtk_constraint_variable_get_value (y);

  g_assert_cmpfloat_with_epsilon (x_value, 5.0, 0.001);
  g_assert_cmpfloat_with_epsilon (y_value, 10.0, 0.001);

  gtk_constraint_variable_unref (x);
  gtk_constraint_variable_unref (y);

  g_object_unref (solver);
}

static void
constraint_solver_remove_stay (gconstpointer data)
{
  GtkConstraintSolver *solver = gtk_constraint_solver_new ();
  int strength = GPOINTER_TO_INT (data);

  for (guint i = 0; i < 64; i++)
    {
      GtkConstraintVariable *var = gtk_constraint_solver_create_variable (solver, NULL, "x", i + 5.0);
      GtkConstraintRef *ref;
      char *dump;

      ref = gtk_constraint_solver_add_stay_variable (solver, var, strength);
      g_assert_true (gtk_constraint_solver_has_stay_variable (solver, var));
      g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (var), i + 5.0, 0.001);

      if (i % 2 == 0)
        gtk_constraint_solver_remove_stay_variable (solver, var);
      else
        gtk_constraint_solver_remove_constraint (solver, ref);

      g_assert_false (gtk_constraint_solver_has_stay_variable (solver, var));
      dump = gtk_constraint_solver_to_string (solver);
      g_assert_nonnull (strstr (dump, "Stay error vars: <empty>\n"));
      g_free (dump);
      gtk_constraint_variable_unref (var);
    }

  g_object_unref (solver);
}

static void
constraint_solver_remove_one_stay (void)
{
  GtkConstraintSolver *solver = gtk_constraint_solver_new ();
  GtkConstraintVariable *x = gtk_constraint_solver_create_variable (solver, NULL, "x", 17.0);
  GtkConstraintVariable *y = gtk_constraint_solver_create_variable (solver, NULL, "y", 10.0);
  GtkConstraintExpression *expr;

  gtk_constraint_solver_add_stay_variable (solver, x, GTK_CONSTRAINT_STRENGTH_WEAK);
  gtk_constraint_solver_add_stay_variable (solver, y, GTK_CONSTRAINT_STRENGTH_STRONG);
  gtk_constraint_solver_remove_stay_variable (solver, x);
  g_assert_false (gtk_constraint_solver_has_stay_variable (solver, x));
  g_assert_true (gtk_constraint_solver_has_stay_variable (solver, y));

  expr = gtk_constraint_expression_plus_constant (gtk_constraint_expression_new_from_variable (y), 3.0);
  gtk_constraint_solver_add_constraint (solver, x, GTK_CONSTRAINT_RELATION_EQ, expr,
                                        GTK_CONSTRAINT_STRENGTH_REQUIRED);
  g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (x), 13.0, 0.001);
  g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (y), 10.0, 0.001);

  gtk_constraint_variable_unref (x);
  gtk_constraint_variable_unref (y);
  g_object_unref (solver);
}

static void
constraint_solver_variable_geq_constant (void)
{
  GtkConstraintSolver *solver = gtk_constraint_solver_new ();

  GtkConstraintVariable *x = gtk_constraint_solver_create_variable (solver, NULL, "x", 10.0);
  GtkConstraintExpression *e = gtk_constraint_expression_new (100.0);

  gtk_constraint_solver_add_constraint (solver,
                                        x, GTK_CONSTRAINT_RELATION_GE, e,
                                        GTK_CONSTRAINT_STRENGTH_REQUIRED);

  double x_value = gtk_constraint_variable_get_value (x);

  g_assert_cmpfloat (x_value, >=, 100.0);

  gtk_constraint_variable_unref (x);

  g_object_unref (solver);
}

static void
constraint_solver_remove_soft (gconstpointer data)
{
  GtkConstraintRelation relation = GPOINTER_TO_INT (data);
  GtkConstraintSolver *solver = gtk_constraint_solver_new ();
  GtkConstraintVariable *x = gtk_constraint_solver_create_variable (solver, NULL, "x", 0.0);

  gtk_constraint_solver_add_constraint (solver, x, GTK_CONSTRAINT_RELATION_EQ,
                                        gtk_constraint_expression_new (10.0),
                                        GTK_CONSTRAINT_STRENGTH_WEAK);

  for (guint i = 0; i < 16; i++)
    {
      double value = relation == GTK_CONSTRAINT_RELATION_LE ? 5.0 : 20.0;
      GtkConstraintRef *ref;

      ref = gtk_constraint_solver_add_constraint (solver, x, relation,
                                                  gtk_constraint_expression_new (value),
                                                  GTK_CONSTRAINT_STRENGTH_STRONG);
      g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (x), value, 0.001);
      gtk_constraint_solver_remove_constraint (solver, ref);
      g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (x), 10.0, 0.001);

      ref = gtk_constraint_solver_add_constraint (solver, x, GTK_CONSTRAINT_RELATION_EQ,
                                                  gtk_constraint_expression_new (30.0),
                                                  GTK_CONSTRAINT_STRENGTH_MEDIUM);
      g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (x), 30.0, 0.001);
      gtk_constraint_solver_remove_constraint (solver, ref);
      g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (x), 10.0, 0.001);
    }

  gtk_constraint_variable_unref (x);
  g_object_unref (solver);
}

static void
constraint_solver_remove_edit (void)
{
  GtkConstraintSolver *solver = gtk_constraint_solver_new ();
  GtkConstraintVariable *x = gtk_constraint_solver_create_variable (solver, NULL, "x", 0.0);

  gtk_constraint_solver_add_constraint (solver, x, GTK_CONSTRAINT_RELATION_EQ,
                                        gtk_constraint_expression_new (10.0),
                                        GTK_CONSTRAINT_STRENGTH_WEAK);

  for (guint i = 0; i < 16; i++)
    {
      gtk_constraint_solver_add_edit_variable (solver, x, GTK_CONSTRAINT_STRENGTH_STRONG);
      gtk_constraint_solver_begin_edit (solver);
      gtk_constraint_solver_suggest_value (solver, x, 0.0);
      gtk_constraint_solver_resolve (solver);
      g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (x), 0.0, 0.001);
      gtk_constraint_solver_remove_edit_variable (solver, x);
      gtk_constraint_solver_end_edit (solver);
      g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (x), 10.0, 0.001);
    }

  gtk_constraint_variable_unref (x);
  g_object_unref (solver);
}

static void
constraint_solver_variable_leq_constant (void)
{
  GtkConstraintSolver *solver = gtk_constraint_solver_new ();

  GtkConstraintVariable *x = gtk_constraint_solver_create_variable (solver, NULL, "x", 100.0);
  GtkConstraintExpression *e = gtk_constraint_expression_new (10.0);

  gtk_constraint_solver_add_constraint (solver,
                                        x, GTK_CONSTRAINT_RELATION_LE, e,
                                        GTK_CONSTRAINT_STRENGTH_REQUIRED);

  double x_value = gtk_constraint_variable_get_value (x);

  g_assert_cmpfloat (x_value, <=, 10.0);

  gtk_constraint_variable_unref (x);

  g_object_unref (solver);
}

static void
constraint_solver_variable_eq_constant (void)
{
  GtkConstraintSolver *solver = gtk_constraint_solver_new ();

  GtkConstraintVariable *x = gtk_constraint_solver_create_variable (solver, NULL, "x", 10.0);
  GtkConstraintExpression *e = gtk_constraint_expression_new (100.0);

  gtk_constraint_solver_add_constraint (solver,
                                        x, GTK_CONSTRAINT_RELATION_EQ, e,
                                        GTK_CONSTRAINT_STRENGTH_REQUIRED);

  double x_value = gtk_constraint_variable_get_value (x);

  g_assert_cmpfloat_with_epsilon (x_value, 100.0, 0.001);

  gtk_constraint_variable_unref (x);

  g_object_unref (solver);
}

static void
constraint_solver_eq_with_stay (void)
{
  GtkConstraintSolver *solver = gtk_constraint_solver_new ();

  GtkConstraintVariable *x = gtk_constraint_solver_create_variable (solver, NULL, "x", 10.0);
  GtkConstraintVariable *width = gtk_constraint_solver_create_variable (solver, NULL, "width", 10.0);
  GtkConstraintVariable *right_min = gtk_constraint_solver_create_variable (solver, NULL, "rightMin", 100.0);

  GtkConstraintExpressionBuilder builder;
  gtk_constraint_expression_builder_init (&builder, solver);
  gtk_constraint_expression_builder_term (&builder, x);
  gtk_constraint_expression_builder_plus (&builder);
  gtk_constraint_expression_builder_term (&builder, width);
  GtkConstraintExpression *right = gtk_constraint_expression_builder_finish (&builder);

  gtk_constraint_solver_add_stay_variable (solver, width, GTK_CONSTRAINT_STRENGTH_WEAK);
  gtk_constraint_solver_add_stay_variable (solver, right_min, GTK_CONSTRAINT_STRENGTH_WEAK);
  gtk_constraint_solver_add_constraint (solver,
                                        right_min, GTK_CONSTRAINT_RELATION_EQ, right,
                                        GTK_CONSTRAINT_STRENGTH_REQUIRED);

  double x_value = gtk_constraint_variable_get_value (x);
  double width_value = gtk_constraint_variable_get_value (width);

  g_assert_cmpfloat_with_epsilon (x_value, 90.0, 0.001);
  g_assert_cmpfloat_with_epsilon (width_value, 10.0, 0.001);

  gtk_constraint_variable_unref (right_min);
  gtk_constraint_variable_unref (width);
  gtk_constraint_variable_unref (x);

  g_object_unref (solver);
}

static void
constraint_solver_cassowary (void)
{
  GtkConstraintSolver *solver = gtk_constraint_solver_new ();

  GtkConstraintVariable *x = gtk_constraint_solver_create_variable (solver, NULL, "x", 0.0);
  GtkConstraintVariable *y = gtk_constraint_solver_create_variable (solver, NULL, "y", 0.0);

  GtkConstraintExpression *e;

  e = gtk_constraint_expression_new_from_variable (y);
  gtk_constraint_solver_add_constraint (solver,
                                        x, GTK_CONSTRAINT_RELATION_LE, e,
                                        GTK_CONSTRAINT_STRENGTH_REQUIRED);

  e = gtk_constraint_expression_plus_constant (gtk_constraint_expression_new_from_variable (x), 3.0);
  gtk_constraint_solver_add_constraint (solver,
                                        y, GTK_CONSTRAINT_RELATION_EQ, e,
                                        GTK_CONSTRAINT_STRENGTH_REQUIRED);

  e = gtk_constraint_expression_new (10.0);
  gtk_constraint_solver_add_constraint (solver,
                                        x, GTK_CONSTRAINT_RELATION_EQ, e,
                                        GTK_CONSTRAINT_STRENGTH_WEAK);

  e = gtk_constraint_expression_new (10.0);
  gtk_constraint_solver_add_constraint (solver,
                                        y, GTK_CONSTRAINT_RELATION_EQ, e,
                                        GTK_CONSTRAINT_STRENGTH_WEAK);

  double x_val = gtk_constraint_variable_get_value (x);
  double y_val = gtk_constraint_variable_get_value (y);

  g_test_message ("x = %g, y = %g", x_val, y_val);

  /* The system is unstable and has two possible solutions we need to test */
  g_assert_true ((G_APPROX_VALUE (x_val, 10.0, 1e-8) &&
                  G_APPROX_VALUE (y_val, 13.0, 1e-8)) ||
                 (G_APPROX_VALUE (x_val,  7.0, 1e-8) &&
                  G_APPROX_VALUE (y_val, 10.0, 1e-8)));

  gtk_constraint_variable_unref (x);
  gtk_constraint_variable_unref (y);

  g_object_unref (solver);
}

static void
constraint_solver_edit_var_required (void)
{
  GtkConstraintSolver *solver = gtk_constraint_solver_new ();

  GtkConstraintVariable *a = gtk_constraint_solver_create_variable (solver, NULL, "a", 0.0);
  gtk_constraint_solver_add_stay_variable (solver, a, GTK_CONSTRAINT_STRENGTH_STRONG);

  g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (a), 0.0, 0.001);

  gtk_constraint_solver_add_edit_variable (solver, a, GTK_CONSTRAINT_STRENGTH_REQUIRED);
  gtk_constraint_solver_begin_edit (solver);
  gtk_constraint_solver_suggest_value (solver, a, 2.0);
  gtk_constraint_solver_resolve (solver);

  g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (a), 2.0, 0.001);

  gtk_constraint_solver_suggest_value (solver, a, 10.0);
  gtk_constraint_solver_resolve (solver);

  g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (a), 10.0, 0.001);

  gtk_constraint_solver_end_edit (solver);

  gtk_constraint_variable_unref (a);

  g_object_unref (solver);
}

static void
constraint_solver_edit_var_suggest (void)
{
  GtkConstraintSolver *solver = gtk_constraint_solver_new ();

  GtkConstraintVariable *a = gtk_constraint_solver_create_variable (solver, NULL, "a", 0.0);
  GtkConstraintVariable *b = gtk_constraint_solver_create_variable (solver, NULL, "b", 0.0);

  gtk_constraint_solver_add_stay_variable (solver, a, GTK_CONSTRAINT_STRENGTH_STRONG);

  GtkConstraintExpression *e = gtk_constraint_expression_new_from_variable (b);
  gtk_constraint_solver_add_constraint (solver,
                                        a, GTK_CONSTRAINT_RELATION_EQ, e,
                                        GTK_CONSTRAINT_STRENGTH_REQUIRED);

  gtk_constraint_solver_resolve (solver);

  g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (a), 0.0, 0.001);
  g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (b), 0.0, 0.001);

  gtk_constraint_solver_add_edit_variable (solver, a, GTK_CONSTRAINT_STRENGTH_REQUIRED);
  gtk_constraint_solver_begin_edit (solver);

  gtk_constraint_solver_suggest_value (solver, a, 2.0);
  gtk_constraint_solver_resolve (solver);

  g_test_message ("Check values after first edit");

  g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (a), 2.0, 0.001);
  g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (b), 2.0, 0.001);

  gtk_constraint_solver_suggest_value (solver, a, 10.0);
  gtk_constraint_solver_resolve (solver);

  g_test_message ("Check values after second edit");

  g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (a), 10.0, 0.001);
  g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (b), 10.0, 0.001);

  gtk_constraint_solver_suggest_value (solver, a, 12.0);
  gtk_constraint_solver_resolve (solver);

  g_test_message ("Check values after third edit");

  g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (a), 12.0, 0.001);
  g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (b), 12.0, 0.001);

  gtk_constraint_variable_unref (a);
  gtk_constraint_variable_unref (b);

  g_object_unref (solver);
}

static void
constraint_solver_paper (void)
{
  GtkConstraintSolver *solver = gtk_constraint_solver_new ();

  GtkConstraintVariable *left = gtk_constraint_solver_create_variable (solver, NULL, "left", 0.0);
  GtkConstraintVariable *middle = gtk_constraint_solver_create_variable (solver, NULL, "middle", 0.0);
  GtkConstraintVariable *right = gtk_constraint_solver_create_variable (solver, NULL, "right", 0.0);

  GtkConstraintExpressionBuilder builder;
  GtkConstraintExpression *expr;

  gtk_constraint_expression_builder_init (&builder, solver);
  gtk_constraint_expression_builder_term (&builder, left);
  gtk_constraint_expression_builder_plus (&builder);
  gtk_constraint_expression_builder_term (&builder, right);
  gtk_constraint_expression_builder_divide_by (&builder);
  gtk_constraint_expression_builder_constant (&builder, 2.0);
  expr = gtk_constraint_expression_builder_finish (&builder);
  gtk_constraint_solver_add_constraint (solver,
                                        middle, GTK_CONSTRAINT_RELATION_EQ, expr,
                                        GTK_CONSTRAINT_STRENGTH_REQUIRED);

  gtk_constraint_expression_builder_init (&builder, solver);
  gtk_constraint_expression_builder_term (&builder, left);
  gtk_constraint_expression_builder_plus (&builder);
  gtk_constraint_expression_builder_constant (&builder, 10.0);
  expr = gtk_constraint_expression_builder_finish (&builder);
  gtk_constraint_solver_add_constraint (solver,
                                        right, GTK_CONSTRAINT_RELATION_EQ, expr,
                                        GTK_CONSTRAINT_STRENGTH_REQUIRED);

  expr = gtk_constraint_expression_new (100.0);
  gtk_constraint_solver_add_constraint (solver,
                                        right, GTK_CONSTRAINT_RELATION_LE, expr,
                                        GTK_CONSTRAINT_STRENGTH_REQUIRED);

  expr = gtk_constraint_expression_new (0.0);
  gtk_constraint_solver_add_constraint (solver,
                                        left, GTK_CONSTRAINT_RELATION_GE, expr,
                                        GTK_CONSTRAINT_STRENGTH_REQUIRED);

  g_test_message ("Check constraints hold");

  g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (middle),
                                  (gtk_constraint_variable_get_value (left) + gtk_constraint_variable_get_value (right)) / 2.0,
                                  0.001);
  g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (right),
                                  gtk_constraint_variable_get_value (left) + 10.0,
                                  0.001);
  g_assert_cmpfloat (gtk_constraint_variable_get_value (right), <=, 100.0);
  g_assert_cmpfloat (gtk_constraint_variable_get_value (left), >=, 0.0);

  gtk_constraint_variable_set_value (middle, 45.0);
  gtk_constraint_solver_add_stay_variable (solver, middle, GTK_CONSTRAINT_STRENGTH_WEAK);

  g_test_message ("Check constraints hold after setting middle");

  g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (middle),
                                  (gtk_constraint_variable_get_value (left) + gtk_constraint_variable_get_value (right)) / 2.0,
                                  0.001);
  g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (right),
                                  gtk_constraint_variable_get_value (left) + 10.0,
                                  0.001);
  g_assert_cmpfloat (gtk_constraint_variable_get_value (right), <=, 100.0);
  g_assert_cmpfloat (gtk_constraint_variable_get_value (left), >=, 0.0);

  g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (left), 40.0, 0.001);
  g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (middle), 45.0, 0.001);
  g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (right), 50.0, 0.001);

  gtk_constraint_variable_unref (left);
  gtk_constraint_variable_unref (middle);
  gtk_constraint_variable_unref (right);

  g_object_unref (solver);
}

static void
constraint_solver_remove_unused (void)
{
  GtkConstraintSolver *solver = gtk_constraint_solver_new ();
  GtkConstraintVariable *x = gtk_constraint_solver_create_variable (solver, NULL, "x", 0.0);
  GtkConstraintVariable *y = gtk_constraint_solver_create_variable (solver, NULL, "y", 0.0);
  GtkConstraintExpressionBuilder builder;
  GtkConstraintRef *sum, *fixed;
  char *before, *after;

  gtk_constraint_expression_builder_init (&builder, solver);
  gtk_constraint_expression_builder_constant (&builder, 20.0);
  gtk_constraint_expression_builder_minus (&builder);
  gtk_constraint_expression_builder_term (&builder, y);
  sum = gtk_constraint_solver_add_constraint (solver, x, GTK_CONSTRAINT_RELATION_EQ,
                                              gtk_constraint_expression_builder_finish (&builder),
                                              GTK_CONSTRAINT_STRENGTH_REQUIRED);
  before = gtk_constraint_solver_statistics (solver);
  gtk_constraint_solver_remove_unused_variable (solver, x);
  gtk_constraint_solver_remove_unused_variable (solver, y);
  after = gtk_constraint_solver_statistics (solver);
  g_assert_cmpstr (before, ==, after);
  g_free (before);
  g_free (after);

  fixed = gtk_constraint_solver_add_constraint (solver, y, GTK_CONSTRAINT_RELATION_EQ,
                                                gtk_constraint_expression_new (7.0),
                                                GTK_CONSTRAINT_STRENGTH_REQUIRED);
  g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (x), 13.0, 0.001);
  g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (y), 7.0, 0.001);
  gtk_constraint_solver_remove_constraint (solver, fixed);
  gtk_constraint_solver_remove_constraint (solver, sum);
  gtk_constraint_solver_remove_unused_variable (solver, x);
  gtk_constraint_solver_remove_unused_variable (solver, y);
  after = gtk_constraint_solver_statistics (solver);
  g_assert_nonnull (strstr (after, "\nRows: 1\n"));
  g_assert_nonnull (strstr (after, "\nColumns: 0\n"));
  g_assert_nonnull (strstr (after, "\nExternal basic variables: 0\n"));
  g_assert_nonnull (strstr (after, "\nExternal parametric variables: 0\n"));
  g_free (after);

  gtk_constraint_variable_unref (x);
  gtk_constraint_variable_unref (y);
  g_object_unref (solver);
}

static void
constraint_solver_freeze (void)
{
  GtkConstraintSolver *solver = gtk_constraint_solver_new ();
  GtkConstraintVariable *x = gtk_constraint_solver_create_variable (solver, NULL, "x", 0.0);
  GtkConstraintRef *medium, *strong;

  gtk_constraint_solver_add_constraint (solver, x, GTK_CONSTRAINT_RELATION_GE,
                                        gtk_constraint_expression_new (0.0),
                                        GTK_CONSTRAINT_STRENGTH_REQUIRED);
  gtk_constraint_solver_freeze (solver);
  gtk_constraint_solver_freeze (solver);
  medium = gtk_constraint_solver_add_constraint (solver, x, GTK_CONSTRAINT_RELATION_EQ,
                                                 gtk_constraint_expression_new (100.0),
                                                 GTK_CONSTRAINT_STRENGTH_MEDIUM);
  strong = gtk_constraint_solver_add_constraint (solver, x, GTK_CONSTRAINT_RELATION_EQ,
                                                 gtk_constraint_expression_new (60.0),
                                                 GTK_CONSTRAINT_STRENGTH_STRONG);
  gtk_constraint_solver_thaw (solver);
  g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (x), 0.0, 0.001);
  gtk_constraint_solver_thaw (solver);
  g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (x), 60.0, 0.001);
  gtk_constraint_solver_freeze (solver);
  gtk_constraint_solver_remove_constraint (solver, strong);
  gtk_constraint_solver_thaw (solver);
  g_assert_cmpfloat_with_epsilon (gtk_constraint_variable_get_value (x), 100.0, 0.001);
  gtk_constraint_solver_remove_constraint (solver, medium);

  gtk_constraint_variable_unref (x);
  g_object_unref (solver);
}

int
main (int argc, char *argv[])
{
  (g_test_init) (&argc, &argv, NULL);
  setlocale (LC_ALL, "C");

  g_test_add_func ("/constraint-solver/paper", constraint_solver_paper);
  g_test_add_func ("/constraint-solver/simple", constraint_solver_simple);
  g_test_add_func ("/constraint-solver/constant/eq", constraint_solver_variable_eq_constant);
  g_test_add_func ("/constraint-solver/constant/ge", constraint_solver_variable_geq_constant);
  g_test_add_func ("/constraint-solver/constant/le", constraint_solver_variable_leq_constant);
  g_test_add_func ("/constraint-solver/stay/simple", constraint_solver_stay);
  g_test_add_func ("/constraint-solver/stay/eq", constraint_solver_eq_with_stay);
  g_test_add_data_func ("/constraint-solver/stay/remove-weak", GINT_TO_POINTER (GTK_CONSTRAINT_STRENGTH_WEAK), constraint_solver_remove_stay);
  g_test_add_data_func ("/constraint-solver/stay/remove-medium", GINT_TO_POINTER (GTK_CONSTRAINT_STRENGTH_MEDIUM), constraint_solver_remove_stay);
  g_test_add_data_func ("/constraint-solver/stay/remove-strong", GINT_TO_POINTER (GTK_CONSTRAINT_STRENGTH_STRONG), constraint_solver_remove_stay);
  g_test_add_func ("/constraint-solver/stay/remove-one", constraint_solver_remove_one_stay);
  g_test_add_func ("/constraint-solver/cassowary", constraint_solver_cassowary);
  g_test_add_func ("/constraint-solver/edit/required", constraint_solver_edit_var_required);
  g_test_add_func ("/constraint-solver/edit/suggest", constraint_solver_edit_var_suggest);
  g_test_add_data_func ("/constraint-solver/remove-soft/eq", GINT_TO_POINTER (GTK_CONSTRAINT_RELATION_EQ), constraint_solver_remove_soft);
  g_test_add_data_func ("/constraint-solver/remove-soft/ge", GINT_TO_POINTER (GTK_CONSTRAINT_RELATION_GE), constraint_solver_remove_soft);
  g_test_add_data_func ("/constraint-solver/remove-soft/le", GINT_TO_POINTER (GTK_CONSTRAINT_RELATION_LE), constraint_solver_remove_soft);
  g_test_add_func ("/constraint-solver/edit/remove", constraint_solver_remove_edit);
  g_test_add_func ("/constraint-solver/variable/remove-unused", constraint_solver_remove_unused);
  g_test_add_func ("/constraint-solver/freeze/nested", constraint_solver_freeze);

  return g_test_run ();
}
