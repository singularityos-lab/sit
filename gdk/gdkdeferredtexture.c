#include "config.h"

#include "gdkdeferredtextureprivate.h"

#include "gdkcolorstateprivate.h"
#include "gdkmemoryformatprivate.h"
#include "gdktextureprivate.h"
#include "loaders/gdkjpegprivate.h"
#include "loaders/gdkpngprivate.h"
#include "sitschedulerprivate.h"

#include <string.h>

#define MIN_DEFERRED_BYTES (64 * 1024)

enum {
  QUEUED,
  DECODING,
  DONE
};

typedef GdkTexture * (* Loader) (GBytes *bytes, GError **error);

struct _GdkDeferredTexture
{
  GdkTexture parent_instance;

  GMutex lock;
  GCond cond;
  int state;
  Loader loader;
  GBytes *bytes;
  GdkTexture *decoded;
};

G_DEFINE_FINAL_TYPE (GdkDeferredTexture, gdk_deferred_texture, GDK_TYPE_TEXTURE)

typedef struct
{
  SitJob job;
  GdkDeferredTexture *texture;
} DecodeJob;

static GdkTexture *
load_png (GBytes  *bytes,
          GError **error)
{
  return gdk_load_png (bytes, NULL, error);
}

static void
decode (GdkDeferredTexture *self)
{
  GError *error = NULL;
  GdkTexture *decoded;
  GBytes *bytes;

  g_mutex_lock (&self->lock);
  bytes = g_steal_pointer (&self->bytes);
  g_mutex_unlock (&self->lock);

  decoded = self->loader (bytes, &error);
  if (decoded &&
      (gdk_texture_get_width (decoded) != GDK_TEXTURE (self)->width ||
       gdk_texture_get_height (decoded) != GDK_TEXTURE (self)->height))
    {
      g_warning ("Image size changed while decoding: %dx%d, expected %dx%d",
                 gdk_texture_get_width (decoded), gdk_texture_get_height (decoded),
                 GDK_TEXTURE (self)->width, GDK_TEXTURE (self)->height);
      g_clear_object (&decoded);
    }
  else if (!decoded)
    {
      g_warning ("Could not decode image: %s", error ? error->message : "unknown error");
    }
  g_clear_error (&error);
  g_bytes_unref (bytes);

  g_mutex_lock (&self->lock);
  self->decoded = decoded;
  self->state = DONE;
  g_cond_broadcast (&self->cond);
  g_mutex_unlock (&self->lock);
}

static void
decode_job_run (SitJob *job)
{
  DecodeJob *self = (DecodeJob *) job;

  if (g_atomic_int_compare_and_exchange (&self->texture->state, QUEUED, DECODING))
    decode (self->texture);

  sit_scheduler_post (job);
}

static void
decode_job_deliver (SitJob *job)
{
  DecodeJob *self = (DecodeJob *) job;

  g_object_unref (self->texture);
  g_free (self);
}

static void
gdk_deferred_texture_download (GdkTexture            *texture,
                               guchar                *data,
                               const GdkMemoryLayout *layout,
                               GdkColorState         *color_state)
{
  GdkDeferredTexture *self = GDK_DEFERRED_TEXTURE (texture);
  GdkTexture *decoded;

  if (g_atomic_int_compare_and_exchange (&self->state, QUEUED, DECODING))
    decode (self);

  g_mutex_lock (&self->lock);
  while (self->state != DONE)
    g_cond_wait (&self->cond, &self->lock);
  decoded = self->decoded;
  g_mutex_unlock (&self->lock);

  if (decoded)
    gdk_texture_do_download (decoded, data, layout, color_state);
  else
    memset (data, 0, layout->size);
}

static void
gdk_deferred_texture_finalize (GObject *object)
{
  GdkDeferredTexture *self = GDK_DEFERRED_TEXTURE (object);

  g_clear_pointer (&self->bytes, g_bytes_unref);
  g_clear_object (&self->decoded);
  g_mutex_clear (&self->lock);
  g_cond_clear (&self->cond);

  G_OBJECT_CLASS (gdk_deferred_texture_parent_class)->finalize (object);
}

static void
gdk_deferred_texture_class_init (GdkDeferredTextureClass *klass)
{
  GdkTextureClass *texture_class = GDK_TEXTURE_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  texture_class->download = gdk_deferred_texture_download;
  object_class->finalize = gdk_deferred_texture_finalize;
}

static void
gdk_deferred_texture_init (GdkDeferredTexture *self)
{
  g_mutex_init (&self->lock);
  g_cond_init (&self->cond);
}

static gboolean
deferral_enabled (void)
{
  static int enabled = -1;

  if (g_atomic_int_get (&enabled) < 0)
    g_atomic_int_set (&enabled, g_strcmp0 (g_getenv ("SIT_ASYNC"), "off") != 0);

  return g_atomic_int_get (&enabled);
}

GdkTexture *
gdk_deferred_texture_try_new (GBytes *bytes)
{
  GdkDeferredTexture *self;
  GdkColorState *color_state = NULL;
  GdkMemoryFormat format;
  guint width, height;
  Loader loader;
  DecodeJob *job;

  if (!deferral_enabled () ||
      g_bytes_get_size (bytes) < MIN_DEFERRED_BYTES ||
      !g_main_context_is_owner (g_main_context_default ()))
    return NULL;

  if (gdk_is_png (bytes))
    {
      if (!gdk_png_probe (bytes, &width, &height, &format, &color_state))
        return NULL;
      loader = load_png;
    }
  else if (gdk_is_jpeg (bytes))
    {
      if (!gdk_jpeg_probe (bytes, &width, &height, &format, &color_state))
        return NULL;
      loader = gdk_load_jpeg;
    }
  else
    return NULL;

  if (width == 0 || height == 0 || width > G_MAXINT || height > G_MAXINT)
    {
      gdk_color_state_unref (color_state);
      return NULL;
    }

  self = g_object_new (GDK_TYPE_DEFERRED_TEXTURE,
                       "width", (int) width,
                       "height", (int) height,
                       "color-state", color_state,
                       NULL);
  gdk_color_state_unref (color_state);
  GDK_TEXTURE (self)->format = format;
  self->loader = loader;
  self->bytes = g_bytes_ref (bytes);

  job = g_new0 (DecodeJob, 1);
  job->job.qos = SIT_JOB_USER;
  job->job.run = decode_job_run;
  job->job.deliver = decode_job_deliver;
  job->texture = g_object_ref (self);
  sit_scheduler_submit (&job->job);

  return GDK_TEXTURE (self);
}
