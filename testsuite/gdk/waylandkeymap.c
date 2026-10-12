#include "config.h"
#include <gdk/gdk.h>
#include "gdk/wayland/gdkkeymap-wayland.h"

typedef struct
{
  const char *name;
  guint keyval;
  GdkModifierType state;
  guint translated;
  GdkModifierType remaining;
} KeyFixture;

static const KeyFixture fixtures[] = {
  { "super", GDK_KEY_d, GDK_SUPER_MASK, GDK_KEY_d, GDK_SUPER_MASK },
  { "hyper", GDK_KEY_d, GDK_HYPER_MASK, GDK_KEY_d, GDK_HYPER_MASK },
  { "meta", GDK_KEY_d, GDK_META_MASK, GDK_KEY_d, GDK_META_MASK },
  { "control", GDK_KEY_d, GDK_CONTROL_MASK, GDK_KEY_d, GDK_CONTROL_MASK },
  { "alt", GDK_KEY_d, GDK_ALT_MASK, GDK_KEY_d, GDK_ALT_MASK },
  { "shift", GDK_KEY_d, GDK_SHIFT_MASK, GDK_KEY_D, 0 },
  { "super-shift", GDK_KEY_d, GDK_SUPER_MASK | GDK_SHIFT_MASK, GDK_KEY_D, GDK_SUPER_MASK },
  { "hyper-shift", GDK_KEY_d, GDK_HYPER_MASK | GDK_SHIFT_MASK, GDK_KEY_D, GDK_HYPER_MASK },
  { "meta-shift", GDK_KEY_d, GDK_META_MASK | GDK_SHIFT_MASK, GDK_KEY_D, GDK_META_MASK },
  { "super-shift-f1", GDK_KEY_F1, GDK_SUPER_MASK | GDK_SHIFT_MASK, GDK_KEY_F1, GDK_SUPER_MASK | GDK_SHIFT_MASK },
  { "control-super", GDK_KEY_d, GDK_CONTROL_MASK | GDK_SUPER_MASK, GDK_KEY_d, GDK_CONTROL_MASK | GDK_SUPER_MASK },
};

static void
test_consumed (gconstpointer data)
{
  const KeyFixture *fixture = data;
  GdkKeymap *keymap = _gdk_wayland_keymap_new (NULL);
  GdkKeymapKey *keys;
  GdkModifierType consumed;
  guint keyval;
  int n_keys;
  int i;

  g_assert_true (gdk_keymap_get_entries_for_keyval (keymap, fixture->keyval, &keys, &n_keys));
  for (i = 0; i < n_keys; i++)
    if (keys[i].group == 0 && keys[i].level == 0)
      break;
  g_assert_cmpint (i, <, n_keys);
  g_assert_true (gdk_keymap_translate_keyboard_state (keymap, keys[i].keycode,
                                                     fixture->state, 0,
                                                     &keyval, NULL, NULL, &consumed));
  g_test_message ("state 0x%x, consumed 0x%x, remaining 0x%x",
                  fixture->state, consumed, fixture->state & ~consumed);
  g_assert_cmphex (keyval, ==, fixture->translated);
  g_assert_cmphex (fixture->state & ~consumed, ==, fixture->remaining);

  g_free (keys);
  g_object_unref (keymap);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  for (guint i = 0; i < G_N_ELEMENTS (fixtures); i++)
    {
      char *path = g_strdup_printf ("/wayland/keymap/consumed/%s", fixtures[i].name);

      g_test_add_data_func (path, &fixtures[i], test_consumed);
      g_free (path);
    }
  return g_test_run ();
}
