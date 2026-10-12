#pragma once

#include <gtk/gtk.h>

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif

G_BEGIN_DECLS

GDK_AVAILABLE_IN_ALL
gboolean sit_widget_set_visual_opacity (GtkWidget *widget,
                                        double     opacity);

GDK_AVAILABLE_IN_ALL
gboolean sit_widget_set_visual_clip (GtkWidget            *widget,
                                     const GskRoundedRect *clip);


GDK_AVAILABLE_IN_ALL
gboolean sit_widget_set_visual_transform (GtkWidget    *widget,
                                          GskTransform *transform);

GDK_AVAILABLE_IN_ALL
GskTransform *sit_widget_get_visual_transform (GtkWidget *widget);


GDK_AVAILABLE_IN_ALL
gboolean sit_window_set_border_widget (GtkWindow *window,
                                       GtkWidget *widget);

GDK_AVAILABLE_IN_ALL
void sit_widget_get_border_box (GtkWidget      *widget,
                                GskRoundedRect *box);

GDK_AVAILABLE_IN_ALL
cairo_region_t *sit_widget_create_border_region (GtkWidget    *widget,
                                                 GtkWidget    *transform_widget,
                                                 GskTransform *transform);

G_END_DECLS
