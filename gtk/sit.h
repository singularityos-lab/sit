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

typedef struct _SitGlass SitGlass;

struct _SitGlass
{
  float blur;
  float refraction;
  float bezel;
  float zoom;
  float saturation;
  float opacity;
};

GDK_AVAILABLE_IN_ALL
void sit_snapshot_append_glass (GtkSnapshot          *snapshot,
                                const GskRoundedRect *shape,
                                const SitGlass       *glass);

GDK_AVAILABLE_IN_ALL
gboolean sit_widget_set_glass (GtkWidget      *widget,
                               const SitGlass *glass);

GDK_AVAILABLE_IN_ALL
gboolean sit_widget_get_glass_accelerated (GtkWidget *widget);

typedef enum {
  SIT_QOS_INTERACTIVE,
  SIT_QOS_USER,
  SIT_QOS_UTILITY,
  SIT_QOS_BACKGROUND
} SitQos;

#define SIT_TYPE_QOS (sit_qos_get_type ())
GDK_AVAILABLE_IN_ALL
GType sit_qos_get_type (void) G_GNUC_CONST;

#define SIT_TYPE_SCOPE (sit_scope_get_type ())
GDK_AVAILABLE_IN_ALL
G_DECLARE_FINAL_TYPE (SitScope, sit_scope, SIT, SCOPE, GObject)

GDK_AVAILABLE_IN_ALL
SitScope *sit_scope_new (SitScope *parent);

GDK_AVAILABLE_IN_ALL
void sit_scope_cancel (SitScope *self);

GDK_AVAILABLE_IN_ALL
gboolean sit_scope_is_cancelled (SitScope *self);

GDK_AVAILABLE_IN_ALL
GCancellable *sit_scope_get_cancellable (SitScope *self);

GDK_AVAILABLE_IN_ALL
SitScope *sit_widget_get_scope (GtkWidget *widget);

typedef gpointer (* SitTaskFunc) (GCancellable  *cancellable,
                                  gpointer       user_data,
                                  GError       **error);

GDK_AVAILABLE_IN_ALL
void sit_task_run (SitScope            *scope,
                   SitQos               qos,
                   GDestroyNotify       result_destroy,
                   SitTaskFunc          func,
                   gpointer             func_data,
                   GDestroyNotify       func_data_destroy,
                   GCancellable        *cancellable,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data);

GDK_AVAILABLE_IN_ALL
gpointer sit_task_run_finish (GAsyncResult  *result,
                              GError       **error);

#define SIT_TYPE_FUTURE (sit_future_get_type ())
GDK_AVAILABLE_IN_ALL
G_DECLARE_FINAL_TYPE (SitFuture, sit_future, SIT, FUTURE, GObject)

GDK_AVAILABLE_IN_ALL
SitFuture *sit_future_spawn (SitScope       *scope,
                             SitQos          qos,
                             GDestroyNotify  result_destroy,
                             SitTaskFunc     func,
                             gpointer        func_data,
                             GDestroyNotify  func_data_destroy);

GDK_AVAILABLE_IN_ALL
SitFuture *sit_future_timeout (SitFuture *future,
                               guint      timeout_ms);

GDK_AVAILABLE_IN_ALL
void sit_future_cancel (SitFuture *self);

GDK_AVAILABLE_IN_ALL
gboolean sit_future_is_done (SitFuture *self);

GDK_AVAILABLE_IN_ALL
void sit_future_wait (SitFuture           *self,
                      GCancellable        *cancellable,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data);

GDK_AVAILABLE_IN_ALL
gpointer sit_future_wait_finish (SitFuture     *self,
                                 GAsyncResult  *result,
                                 GError       **error);

GDK_AVAILABLE_IN_ALL
void sit_future_all (SitFuture           **futures,
                     guint                 n_futures,
                     GCancellable         *cancellable,
                     GAsyncReadyCallback   callback,
                     gpointer              user_data);

GDK_AVAILABLE_IN_ALL
gboolean sit_future_all_finish (GAsyncResult  *result,
                                GError       **error);

GDK_AVAILABLE_IN_ALL
void sit_future_any (SitFuture           **futures,
                     guint                 n_futures,
                     GCancellable         *cancellable,
                     GAsyncReadyCallback   callback,
                     gpointer              user_data);

GDK_AVAILABLE_IN_ALL
int sit_future_any_finish (GAsyncResult  *result,
                           GError       **error);

GDK_AVAILABLE_IN_ALL
void sit_future_race (SitFuture           **futures,
                      guint                 n_futures,
                      GCancellable         *cancellable,
                      GAsyncReadyCallback   callback,
                      gpointer              user_data);

GDK_AVAILABLE_IN_ALL
int sit_future_race_finish (GAsyncResult  *result,
                            GError       **error);

typedef gpointer (* SitThenFunc) (gpointer       value,
                                  GCancellable  *cancellable,
                                  gpointer       user_data,
                                  GError       **error);

GDK_AVAILABLE_IN_ALL
SitFuture *sit_future_then (SitFuture      *future,
                            SitQos          qos,
                            GDestroyNotify  result_destroy,
                            SitThenFunc     func,
                            gpointer        func_data,
                            GDestroyNotify  func_data_destroy);

#define SIT_TYPE_STREAM (sit_stream_get_type ())
GDK_AVAILABLE_IN_ALL
G_DECLARE_FINAL_TYPE (SitStream, sit_stream, SIT, STREAM, GObject)

typedef gboolean (* SitStreamFunc) (SitStream     *stream,
                                    GCancellable  *cancellable,
                                    gpointer       user_data,
                                    GError       **error);

GDK_AVAILABLE_IN_ALL
SitStream *sit_stream_new (void);

GDK_AVAILABLE_IN_ALL
SitStream *sit_stream_spawn (SitScope       *scope,
                             SitQos          qos,
                             SitStreamFunc   func,
                             gpointer        func_data,
                             GDestroyNotify  func_data_destroy);

GDK_AVAILABLE_IN_ALL
void sit_stream_push (SitStream *self,
                      GObject   *item);

GDK_AVAILABLE_IN_ALL
void sit_stream_close (SitStream    *self,
                       const GError *error);

GDK_AVAILABLE_IN_ALL
void sit_stream_next (SitStream           *self,
                      GCancellable        *cancellable,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data);

GDK_AVAILABLE_IN_ALL
GPtrArray *sit_stream_next_finish (SitStream     *self,
                                   GAsyncResult  *result,
                                   GError       **error);

#define SIT_TYPE_CHANNEL (sit_channel_get_type ())
GDK_AVAILABLE_IN_ALL
G_DECLARE_FINAL_TYPE (SitChannel, sit_channel, SIT, CHANNEL, GObject)

GDK_AVAILABLE_IN_ALL
SitChannel *sit_channel_new (guint          capacity,
                             GDestroyNotify item_destroy);

GDK_AVAILABLE_IN_ALL
gboolean sit_channel_send (SitChannel    *self,
                           gpointer       item,
                           GCancellable  *cancellable);

GDK_AVAILABLE_IN_ALL
gboolean sit_channel_try_send (SitChannel *self,
                               gpointer    item);

GDK_AVAILABLE_IN_ALL
gpointer sit_channel_receive (SitChannel    *self,
                              GCancellable  *cancellable,
                              GError       **error);

GDK_AVAILABLE_IN_ALL
void sit_channel_receive_async (SitChannel          *self,
                                GCancellable        *cancellable,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data);

GDK_AVAILABLE_IN_ALL
gpointer sit_channel_receive_finish (SitChannel    *self,
                                     GAsyncResult  *result,
                                     GError       **error);

GDK_AVAILABLE_IN_ALL
void sit_channel_close (SitChannel *self);

GDK_AVAILABLE_IN_ALL
guint sit_channel_get_length (SitChannel *self);

GDK_AVAILABLE_IN_ALL
void sit_file_load_async (GFile               *file,
                          SitQos               qos,
                          GCancellable        *cancellable,
                          GAsyncReadyCallback  callback,
                          gpointer             user_data);

GDK_AVAILABLE_IN_ALL
GBytes *sit_file_load_finish (GFile         *file,
                              GAsyncResult  *result,
                              GError       **error);

GDK_AVAILABLE_IN_ALL
void sit_async_flush (void);

G_END_DECLS
