#include "config.h"

#include "sitotpprivate.h"

#include <glib/gi18n-lib.h>

#include "gtkbutton.h"
#include "gtkeditable.h"
#include "gtkpopover.h"
#include "gtktextprivate.h"

#define OTP_BUS_NAME "dev.sinty.OneTimeCodes"
#define OTP_PATH "/dev/sinty/OneTimeCodes"
#define OTP_IFACE "dev.sinty.OneTimeCodes1"
#define OTP_INLINE_SECONDS 180

static GDBusConnection *otp_bus;
static gboolean otp_started;
static gboolean otp_have;
static SitOtpCode otp_cache;
static guint otp_used;

void
sit_otp_code_clear (SitOtpCode *code)
{
  g_clear_pointer (&code->code, g_free);
  g_clear_pointer (&code->service, g_free);
  g_clear_pointer (&code->domain, g_free);
  g_clear_pointer (&code->app, g_free);
  code->id = 0;
  code->expires = 0;
  code->received = 0;
}

static void
otp_store (GVariant *reply)
{
  gboolean found;
  guint id;
  const char *code, *service, *domain, *app;
  gint64 expires;

  g_variant_get (reply, "(bu&s&s&s&sx)", &found, &id, &code, &service, &domain, &app, &expires);
  if (!found)
    {
      sit_otp_code_clear (&otp_cache);
      otp_have = FALSE;
      return;
    }
  if (!otp_have || otp_cache.id != id)
    {
      gint64 received = g_get_real_time () / G_USEC_PER_SEC;
      sit_otp_code_clear (&otp_cache);
      otp_cache.id = id;
      otp_cache.code = g_strdup (code);
      otp_cache.service = g_strdup (service);
      otp_cache.domain = g_strdup (domain);
      otp_cache.app = g_strdup (app);
      otp_cache.received = received;
    }
  otp_cache.expires = expires;
  otp_have = TRUE;
}

static void
otp_latest_done (GObject      *source,
                 GAsyncResult *result,
                 gpointer      data)
{
  GVariant *reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, NULL);

  if (reply)
    {
      otp_store (reply);
      g_variant_unref (reply);
    }
}

static void
otp_changed (GDBusConnection *connection,
             const char      *sender,
             const char      *path,
             const char      *iface,
             const char      *signal,
             GVariant        *parameters,
             gpointer         data)
{
  g_dbus_connection_call (connection, OTP_BUS_NAME, OTP_PATH, OTP_IFACE, "Latest",
                          NULL, G_VARIANT_TYPE ("(bussssx)"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
                          2000, NULL, otp_latest_done, NULL);
}

static void
otp_start (void)
{
  GVariant *reply;
  const char *mode;

  if (otp_started)
    return;
  otp_started = TRUE;

  mode = g_getenv ("SIT_OTP");
  if (mode && g_str_equal (mode, "off"))
    return;

  otp_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (!otp_bus)
    return;

  g_dbus_connection_signal_subscribe (otp_bus, OTP_BUS_NAME, OTP_IFACE, "Changed", OTP_PATH,
                                      NULL, G_DBUS_SIGNAL_FLAGS_NONE, otp_changed, NULL, NULL);
  reply = g_dbus_connection_call_sync (otp_bus, OTP_BUS_NAME, OTP_PATH, OTP_IFACE, "Latest",
                                       NULL, G_VARIANT_TYPE ("(bussssx)"),
                                       G_DBUS_CALL_FLAGS_NO_AUTO_START, 250, NULL, NULL);
  if (reply)
    {
      otp_store (reply);
      g_variant_unref (reply);
    }
}

gboolean
sit_otp_current (SitOtpCode *out)
{
  otp_start ();
  if (!otp_have || otp_cache.id == otp_used)
    return FALSE;
  if (otp_cache.expires <= g_get_real_time () / G_USEC_PER_SEC)
    return FALSE;

  out->id = otp_cache.id;
  out->code = g_strdup (otp_cache.code);
  out->service = g_strdup (otp_cache.service);
  out->domain = g_strdup (otp_cache.domain);
  out->app = g_strdup (otp_cache.app);
  out->expires = otp_cache.expires;
  out->received = otp_cache.received;
  return TRUE;
}

static const char *
otp_current_app (void)
{
  GApplication *app = g_application_get_default ();
  const char *id = app ? g_application_get_application_id (app) : NULL;

  return id ? id : g_get_prgname ();
}

static gboolean
otp_bound_elsewhere (const SitOtpCode *code)
{
  const char *here = otp_current_app ();

  if (!code->app || !*code->app)
    return FALSE;
  if (g_str_equal (code->app, "dev.sinty.messages") ||
      g_str_equal (code->app, "dev.sinty.lettere") ||
      g_str_has_prefix (code->app, "dev.sinty.nearby"))
    return FALSE;
  return !here || g_ascii_strcasecmp (here, code->app) != 0;
}

char *
sit_otp_menu_label (const SitOtpCode *code)
{
  if (code->domain && *code->domain)
    return g_strdup_printf (_("Paste Code %s from %s, Meant for %s"),
                            code->code, code->service, code->domain);
  if (otp_bound_elsewhere (code))
    return g_strdup_printf (_("Paste Code %s from %s, Meant for Another App"),
                            code->code, code->service);
  if (code->service && *code->service)
    return g_strdup_printf (_("Paste Code %s from %s"), code->code, code->service);
  return g_strdup_printf (_("Paste Code %s"), code->code);
}

void
sit_otp_mark_used (guint id)
{
  otp_used = id;
  if (otp_bus)
    g_dbus_connection_call (otp_bus, OTP_BUS_NAME, OTP_PATH, OTP_IFACE, "Use",
                            g_variant_new ("(u)", id), NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START,
                            2000, NULL, NULL, NULL);
}

static gboolean
otp_single_box (GtkWidget *widget)
{
  return GTK_IS_TEXT (widget) &&
         gtk_text_get_max_length (GTK_TEXT (widget)) == 1 &&
         gtk_editable_get_editable (GTK_EDITABLE (widget)) &&
         gtk_widget_get_visible (widget);
}

static void
otp_collect (GtkWidget *widget,
             GPtrArray *boxes)
{
  GtkWidget *child;

  if (otp_single_box (widget))
    {
      g_ptr_array_add (boxes, widget);
      return;
    }
  for (child = gtk_widget_get_first_child (widget); child; child = gtk_widget_get_next_sibling (child))
    otp_collect (child, boxes);
}

static int
otp_compare_position (gconstpointer a,
                      gconstpointer b)
{
  GtkWidget *wa = *(GtkWidget **) a;
  GtkWidget *wb = *(GtkWidget **) b;
  graphene_point_t pa = GRAPHENE_POINT_INIT (0, 0), pb = GRAPHENE_POINT_INIT (0, 0), ra, rb;
  GtkWidget *root = GTK_WIDGET (gtk_widget_get_root (wa));

  if (!root || !gtk_widget_compute_point (wa, root, &pa, &ra))
    ra = pa;
  if (!root || !gtk_widget_compute_point (wb, root, &pb, &rb))
    rb = pb;
  if (ABS (ra.y - rb.y) > 4)
    return ra.y < rb.y ? -1 : 1;
  if (ra.x == rb.x)
    return 0;
  return ra.x < rb.x ? -1 : 1;
}

static gboolean
otp_fill_boxes (GtkWidget  *text,
                const char *code)
{
  GtkWidget *ancestor = gtk_widget_get_parent (text);
  glong length = g_utf8_strlen (code, -1);
  int level;

  for (level = 0; ancestor && level < 4; level++, ancestor = gtk_widget_get_parent (ancestor))
    {
      GPtrArray *boxes = g_ptr_array_new ();
      guint start = 0, i;
      const char *p = code;

      otp_collect (ancestor, boxes);
      if (boxes->len < (guint) length)
        {
          g_ptr_array_free (boxes, TRUE);
          continue;
        }
      g_ptr_array_sort (boxes, otp_compare_position);
      for (i = 0; i < boxes->len; i++)
        if (g_ptr_array_index (boxes, i) == text)
          start = i;
      if (boxes->len - start < (guint) length)
        start = 0;
      for (i = start; i < boxes->len && *p; i++)
        {
          char ch[8] = { 0 };
          const char *next = g_utf8_next_char (p);
          memcpy (ch, p, MIN ((gsize) (next - p), sizeof ch - 1));
          gtk_editable_set_text (GTK_EDITABLE (g_ptr_array_index (boxes, i)), ch);
          p = next;
        }
      if (i > 0)
        gtk_widget_grab_focus (g_ptr_array_index (boxes, i - 1));
      g_ptr_array_free (boxes, TRUE);
      return TRUE;
    }
  return FALSE;
}

void
sit_otp_fill (GtkWidget        *text,
              const SitOtpCode *code)
{
  GtkEditable *editable = GTK_EDITABLE (text);

  if (!(otp_single_box (text) && otp_fill_boxes (text, code->code)))
    {
      int position;

      gtk_editable_delete_selection (editable);
      position = gtk_editable_get_position (editable);
      gtk_editable_insert_text (editable, code->code, -1, &position);
      gtk_editable_set_position (editable, position);
    }
  sit_otp_mark_used (code->id);
}

static void
otp_inline_clicked (GtkButton *button,
                    GtkWidget *text)
{
  SitOtpCode code = { 0 };
  GtkWidget *popover = g_object_get_data (G_OBJECT (text), "sit-otp-popover");

  if (sit_otp_current (&code))
    {
      sit_otp_fill (text, &code);
      sit_otp_code_clear (&code);
    }
  if (popover)
    gtk_popover_popdown (GTK_POPOVER (popover));
}

static gboolean
otp_wants_inline (GtkWidget *text)
{
  GtkInputPurpose purpose;
  int max_length;

  if (!GTK_IS_TEXT (text) || !gtk_editable_get_editable (GTK_EDITABLE (text)))
    return FALSE;
  if (gtk_editable_get_text (GTK_EDITABLE (text))[0] != '\0')
    return FALSE;
  purpose = gtk_text_get_input_purpose (GTK_TEXT (text));
  max_length = gtk_text_get_max_length (GTK_TEXT (text));
  return purpose == GTK_INPUT_PURPOSE_PIN ||
         purpose == GTK_INPUT_PURPOSE_DIGITS ||
         max_length == 1 ||
         (max_length >= 4 && max_length <= 10 && purpose == GTK_INPUT_PURPOSE_NUMBER);
}

static void
otp_popover_unparent (gpointer data)
{
  GtkWidget *popover = data;

  if (gtk_widget_get_parent (popover))
    gtk_widget_unparent (popover);
  g_object_unref (popover);
}

void
sit_otp_focus_in (GtkWidget *text)
{
  SitOtpCode code = { 0 };
  GtkWidget *popover, *button;
  char *label;

  if (!otp_wants_inline (text) || !sit_otp_current (&code))
    return;
  if (g_get_real_time () / G_USEC_PER_SEC - code.received > OTP_INLINE_SECONDS)
    {
      sit_otp_code_clear (&code);
      return;
    }

  popover = g_object_get_data (G_OBJECT (text), "sit-otp-popover");
  if (!popover)
    {
      popover = gtk_popover_new ();
      gtk_popover_set_autohide (GTK_POPOVER (popover), FALSE);
      gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);
      gtk_popover_set_position (GTK_POPOVER (popover), GTK_POS_BOTTOM);
      gtk_widget_set_halign (popover, GTK_ALIGN_START);
      gtk_widget_add_css_class (popover, "one-time-code");
      button = gtk_button_new ();
      gtk_widget_set_focus_on_click (button, FALSE);
      gtk_widget_set_can_focus (button, FALSE);
      gtk_button_set_has_frame (GTK_BUTTON (button), FALSE);
      g_signal_connect (button, "clicked", G_CALLBACK (otp_inline_clicked), text);
      gtk_popover_set_child (GTK_POPOVER (popover), button);
      gtk_widget_set_parent (popover, text);
      g_object_set_data_full (G_OBJECT (text), "sit-otp-popover", g_object_ref (popover), otp_popover_unparent);
    }
  button = gtk_popover_get_child (GTK_POPOVER (popover));
  if (code.domain && *code.domain)
    label = g_strdup_printf (_("Use Code %s from %s, Meant for %s"), code.code, code.service, code.domain);
  else if (otp_bound_elsewhere (&code))
    label = g_strdup_printf (_("Use Code %s from %s, Meant for Another App"), code.code, code.service);
  else
    label = g_strdup_printf (_("Use Code %s from %s"), code.code, code.service);
  gtk_button_set_label (GTK_BUTTON (button), label);
  g_free (label);
  gtk_popover_popup (GTK_POPOVER (popover));
  sit_otp_code_clear (&code);
}

void
sit_otp_focus_out (GtkWidget *text)
{
  GtkWidget *popover = g_object_get_data (G_OBJECT (text), "sit-otp-popover");

  if (popover)
    gtk_popover_popdown (GTK_POPOVER (popover));
}

void
sit_otp_detach (GtkWidget *text)
{
  g_object_set_data (G_OBJECT (text), "sit-otp-popover", NULL);
}
