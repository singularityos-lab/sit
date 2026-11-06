#pragma once

#include "gtksearchengineprivate.h"

G_BEGIN_DECLS

#define GTK_TYPE_SEARCH_ENGINE_SIT (gtk_search_engine_sit_get_type ())
G_DECLARE_FINAL_TYPE (GtkSearchEngineSit, gtk_search_engine_sit, GTK, SEARCH_ENGINE_SIT, GtkSearchEngine)

GtkSearchEngine *gtk_search_engine_sit_new (void);

G_END_DECLS
