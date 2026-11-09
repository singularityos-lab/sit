#include "config.h"

#include "sit.h"
#include "sitringprivate.h"
#include "gdk/sitschedulerprivate.h"

#include <gio/gio.h>
#include <errno.h>

typedef struct
{
  SitJob job;
  GTask *task;
  char *path;
  SitQos qos;
  GBytes *bytes;
  GError *error;
} LoadRequest;

static void
request_deliver (SitJob *job)
{
  LoadRequest *self = (LoadRequest *) job;

  if (self->error)
    g_task_return_error (self->task, g_steal_pointer (&self->error));
  else
    g_task_return_pointer (self->task, g_steal_pointer (&self->bytes), (GDestroyNotify) g_bytes_unref);
  g_object_unref (self->task);
  g_free (self->path);
  g_free (self);
}

static void
request_finish (LoadRequest *self)
{
  self->job.qos = SIT_JOB_USER;
  self->job.deliver = request_deliver;
  sit_scheduler_post (&self->job);
}

static void
pool_run (SitJob *job)
{
  LoadRequest *self = (LoadRequest *) job;
  GCancellable *cancellable = g_task_get_cancellable (self->task);
  GFile *file = g_file_new_for_path (self->path);
  char *contents = NULL;
  gsize length = 0;

  if (!g_cancellable_set_error_if_cancelled (cancellable, &self->error) &&
      g_file_load_contents (file, cancellable, &contents, &length, NULL, &self->error))
    self->bytes = g_bytes_new_take (contents, length);
  g_object_unref (file);
  request_finish (self);
}

static void
pool_submit (LoadRequest *self)
{
  self->job.qos = (SitJobClass) self->qos;
  self->job.run = pool_run;
  sit_scheduler_submit (&self->job);
}

static void
ring_done (SitRingOutcome  outcome,
           GBytes         *bytes,
           int             error,
           gpointer        data)
{
  LoadRequest *self = data;

  switch (outcome)
    {
    case SIT_RING_LOADED:
      self->bytes = bytes;
      break;
    case SIT_RING_FAILED:
      if (error == ECANCELED)
        g_set_error_literal (&self->error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Operation was cancelled");
      else
        g_set_error (&self->error, G_IO_ERROR, g_io_error_from_errno (error),
                     "%s: %s", self->path, g_strerror (error));
      break;
    case SIT_RING_UNSUITABLE:
    default:
      pool_submit (self);
      return;
    }
  request_finish (self);
}

static gboolean
uring_ready (void)
{
  static gsize logged = 0;
  gboolean ready = sit_ring_available ();

  if (g_once_init_enter (&logged))
    {
      g_debug ("sit: local files load through %s", ready ? "io_uring" : "the worker pool");
      g_once_init_leave (&logged, 1);
    }

  return ready;
}

static void
remote_loaded (GObject      *source,
               GAsyncResult *result,
               gpointer      data)
{
  GTask *task = data;
  GError *error = NULL;
  GBytes *bytes = g_file_load_bytes_finish (G_FILE (source), result, NULL, &error);

  if (error)
    g_task_return_error (task, error);
  else
    g_task_return_pointer (task, bytes, (GDestroyNotify) g_bytes_unref);
  g_object_unref (task);
}

/**
 * sit_file_load_async:
 * @file: the file to read
 * @qos: the class of service of the read
 * @cancellable: (nullable): a cancellable
 * @callback: (scope async) (closure user_data): called with the contents
 * @user_data: data for @callback
 *
 * Local files are read through io_uring where the kernel allows it, and on
 * the worker pool otherwise. Background reads use the idle disk class.
 */
void
sit_file_load_async (GFile               *file,
                     SitQos               qos,
                     GCancellable        *cancellable,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
  LoadRequest *self;
  GTask *task;
  char *path;

  g_return_if_fail (G_IS_FILE (file));
  g_return_if_fail (qos <= SIT_QOS_BACKGROUND);

  sit_scheduler_init ();
  task = g_task_new (file, cancellable, callback, user_data);
  g_task_set_source_tag (task, sit_file_load_async);

  path = g_file_get_path (file);
  if (!path)
    {
      g_file_load_bytes_async (file, cancellable, remote_loaded, task);
      return;
    }

  self = g_new0 (LoadRequest, 1);
  self->task = task;
  self->path = path;
  self->qos = qos;
  if (uring_ready () &&
      sit_ring_load (path, qos == SIT_QOS_BACKGROUND, cancellable, ring_done, self))
    return;
  pool_submit (self);
}

/**
 * sit_file_load_finish:
 * @file: the file passed to sit_file_load_async()
 * @result: the result passed to the callback
 * @error: return location for an error
 *
 * Returns: (transfer full) (nullable): the contents of the file
 */
GBytes *
sit_file_load_finish (GFile         *file,
                      GAsyncResult  *result,
                      GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, file), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}
