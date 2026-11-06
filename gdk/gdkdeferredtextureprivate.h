#pragma once

#include "gdktextureprivate.h"

G_BEGIN_DECLS

#define GDK_TYPE_DEFERRED_TEXTURE (gdk_deferred_texture_get_type ())

G_DECLARE_FINAL_TYPE (GdkDeferredTexture, gdk_deferred_texture, GDK, DEFERRED_TEXTURE, GdkTexture)

GdkTexture *gdk_deferred_texture_try_new (GBytes *bytes);

G_END_DECLS
