#pragma once

#include "gtkwidget.h"

G_BEGIN_DECLS

typedef struct
{
  guint id;
  char *code;
  char *service;
  char *domain;
  char *app;
  gint64 expires;
  gint64 received;
} SitOtpCode;

gboolean sit_otp_current      (SitOtpCode *out);
void     sit_otp_code_clear   (SitOtpCode *code);
char    *sit_otp_menu_label   (const SitOtpCode *code);
void     sit_otp_fill         (GtkWidget *text, const SitOtpCode *code);
void     sit_otp_focus_in     (GtkWidget *text);
void     sit_otp_focus_out    (GtkWidget *text);
void     sit_otp_detach       (GtkWidget *text);
void     sit_otp_mark_used    (guint id);

G_END_DECLS
