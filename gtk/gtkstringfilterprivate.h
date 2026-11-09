#pragma once

#include "gtkstringfilter.h"

G_BEGIN_DECLS

char *   gtk_string_filter_evaluate     (GtkStringFilter *self,
                                         gpointer         item);
gboolean gtk_string_filter_match_string (GtkStringFilter *self,
                                         const char      *string);

G_END_DECLS
