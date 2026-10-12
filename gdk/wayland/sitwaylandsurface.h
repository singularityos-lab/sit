#pragma once

#if !defined (__GDKWAYLAND_H_INSIDE__) && !defined (GTK_COMPILATION)
#error "Only <gdk/wayland/gdkwayland.h> can be included directly."
#endif

#include <gdk/gdk.h>

G_BEGIN_DECLS

typedef enum
{
  SIT_BACKGROUND_EFFECT_DISABLED,
  SIT_BACKGROUND_EFFECT_GLASS,
  SIT_BACKGROUND_EFFECT_BLUR
} SitBackgroundEffect;

GDK_AVAILABLE_IN_ALL
gboolean sit_wayland_surface_set_background_effect (GdkSurface          *surface,
                                                    SitBackgroundEffect  mode,
                                                    const GdkRectangle  *region,
                                                    guint                strength);

GDK_AVAILABLE_IN_ALL
gboolean sit_wayland_surface_set_background_effect_region (GdkSurface           *surface,
                                                           SitBackgroundEffect   mode,
                                                           const cairo_region_t *region,
                                                           guint                 strength);

G_END_DECLS
