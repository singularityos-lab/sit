#include "config.h"

#include "sit.h"
#include "gdk/sitschedulerprivate.h"

#include <gio/gio.h>
#include <string.h>

#if defined(__linux__) && __has_include(<linux/io_uring.h>)
#define SIT_HAVE_URING 1
#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#endif

#define RING_ENTRIES 256
#define IOPRIO_IDLE (3 << 13)

typedef struct
{
  SitJob job;
  GTask *task;
  char *path;
  SitQos qos;
  int fd;
  gboolean opened;
  guint8 *data;
  gsize size;
  gsize done;
  GBytes *bytes;
  GError *error;
} LoadRequest;

static void
request_free (LoadRequest *self)
{
  g_clear_object (&self->task);
  g_free (self->path);
  g_free (self->data);
  g_clear_pointer (&self->bytes, g_bytes_unref);
  g_clear_error (&self->error);
  g_free (self);
}

static void
request_deliver (SitJob *job)
{
  LoadRequest *self = (LoadRequest *) job;

  if (self->error)
    g_task_return_error (self->task, g_steal_pointer (&self->error));
  else
    g_task_return_pointer (self->task, g_steal_pointer (&self->bytes), (GDestroyNotify) g_bytes_unref);
  request_free (self);
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

#ifdef SIT_HAVE_URING

typedef struct
{
  int fd;
  GMutex lock;
  guint *sq_head;
  guint *sq_tail;
  guint *sq_mask;
  guint *sq_entries;
  guint *sq_array;
  struct io_uring_sqe *sqes;
  guint *cq_head;
  guint *cq_tail;
  guint *cq_mask;
  struct io_uring_cqe *cqes;
} Ring;

static Ring ring = { .fd = -1 };

static gboolean
ring_setup (void)
{
  struct io_uring_params params = { 0 };
  gsize sq_size, cq_size;
  guint8 *sq, *cq;

  ring.fd = syscall (__NR_io_uring_setup, RING_ENTRIES, &params);
  if (ring.fd < 0)
    return FALSE;

  sq_size = params.sq_off.array + params.sq_entries * sizeof (guint);
  cq_size = params.cq_off.cqes + params.cq_entries * sizeof (struct io_uring_cqe);
  if (params.features & IORING_FEAT_SINGLE_MMAP)
    sq_size = cq_size = MAX (sq_size, cq_size);

  sq = mmap (NULL, sq_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring.fd, IORING_OFF_SQ_RING);
  if (sq == MAP_FAILED)
    goto fail;
  if (params.features & IORING_FEAT_SINGLE_MMAP)
    cq = sq;
  else
    {
      cq = mmap (NULL, cq_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring.fd, IORING_OFF_CQ_RING);
      if (cq == MAP_FAILED)
        goto fail;
    }
  ring.sqes = mmap (NULL, params.sq_entries * sizeof (struct io_uring_sqe), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_POPULATE, ring.fd, IORING_OFF_SQES);
  if (ring.sqes == MAP_FAILED)
    goto fail;

  ring.sq_head = (guint *) (sq + params.sq_off.head);
  ring.sq_tail = (guint *) (sq + params.sq_off.tail);
  ring.sq_mask = (guint *) (sq + params.sq_off.ring_mask);
  ring.sq_entries = (guint *) (sq + params.sq_off.ring_entries);
  ring.sq_array = (guint *) (sq + params.sq_off.array);
  ring.cq_head = (guint *) (cq + params.cq_off.head);
  ring.cq_tail = (guint *) (cq + params.cq_off.tail);
  ring.cq_mask = (guint *) (cq + params.cq_off.ring_mask);
  ring.cqes = (struct io_uring_cqe *) (cq + params.cq_off.cqes);
  g_mutex_init (&ring.lock);
  return TRUE;

fail:
  close (ring.fd);
  ring.fd = -1;
  return FALSE;
}

static gboolean
ring_push (LoadRequest *self,
           guint8       opcode)
{
  struct io_uring_sqe *sqe;
  guint tail, index;
  gboolean pushed = FALSE;

  g_mutex_lock (&ring.lock);
  tail = *ring.sq_tail;
  if (tail - __atomic_load_n (ring.sq_head, __ATOMIC_ACQUIRE) < *ring.sq_entries)
    {
      index = tail & *ring.sq_mask;
      sqe = &ring.sqes[index];
      memset (sqe, 0, sizeof (*sqe));
      sqe->opcode = opcode;
      sqe->user_data = (guint64) (guintptr) self;
      if (opcode == IORING_OP_OPENAT)
        {
          sqe->fd = AT_FDCWD;
          sqe->addr = (guint64) (guintptr) self->path;
          sqe->open_flags = O_RDONLY | O_CLOEXEC;
        }
      else
        {
          sqe->fd = self->fd;
          sqe->addr = (guint64) (guintptr) (self->data + self->done);
          sqe->len = self->size - self->done;
          sqe->off = self->done;
          if (self->qos == SIT_QOS_BACKGROUND)
            sqe->ioprio = IOPRIO_IDLE;
        }
      ring.sq_array[index] = index;
      __atomic_store_n (ring.sq_tail, tail + 1, __ATOMIC_RELEASE);
      pushed = TRUE;
    }
  for (;;)
    {
      guint waiting = *ring.sq_tail - __atomic_load_n (ring.sq_head, __ATOMIC_ACQUIRE);

      if (waiting == 0 ||
          (syscall (__NR_io_uring_enter, ring.fd, waiting, 0, 0, NULL, 0) < 0 &&
           errno != EINTR && errno != EAGAIN && errno != EBUSY))
        break;
      if (*ring.sq_tail == __atomic_load_n (ring.sq_head, __ATOMIC_ACQUIRE))
        break;
      g_thread_yield ();
    }
  g_mutex_unlock (&ring.lock);

  return pushed;
}

static void
request_close (LoadRequest *self)
{
  if (self->opened)
    close (self->fd);
  self->opened = FALSE;
}

static void
request_fail (LoadRequest *self,
              int          error)
{
  request_close (self);
  g_set_error (&self->error, G_IO_ERROR, g_io_error_from_errno (error),
               "%s: %s", self->path, g_strerror (error));
  request_finish (self);
}

static void
request_step (LoadRequest *self,
              int          result)
{
  struct stat st;

  if (g_cancellable_set_error_if_cancelled (g_task_get_cancellable (self->task), &self->error))
    {
      if (!self->opened && result >= 0)
        close (result);
      request_close (self);
      request_finish (self);
      return;
    }
  if (result < 0)
    {
      request_fail (self, -result);
      return;
    }

  if (!self->opened)
    {
      self->fd = result;
      self->opened = TRUE;
      if (fstat (self->fd, &st) < 0)
        {
          request_fail (self, errno);
          return;
        }
      if (!S_ISREG (st.st_mode) || st.st_size == 0)
        {
          request_close (self);
          pool_submit (self);
          return;
        }
      self->size = st.st_size;
      self->data = g_malloc (self->size);
    }
  else
    {
      if (result == 0)
        self->size = self->done;
      self->done += result;
    }

  if (self->done < self->size)
    {
      if (!ring_push (self, IORING_OP_READ))
        {
          request_close (self);
          g_clear_pointer (&self->data, g_free);
          self->size = self->done = 0;
          pool_submit (self);
        }
      return;
    }

  request_close (self);
  self->bytes = g_bytes_new_take (g_steal_pointer (&self->data), self->size);
  request_finish (self);
}

static gpointer
ring_reap (gpointer data)
{
  for (;;)
    {
      guint head = *ring.cq_head;

      if (head == __atomic_load_n (ring.cq_tail, __ATOMIC_ACQUIRE))
        {
          if (syscall (__NR_io_uring_enter, ring.fd, 0, 1, IORING_ENTER_GETEVENTS, NULL, 0) < 0 &&
              errno != EINTR)
            g_usleep (1000);
          continue;
        }
      while (head != __atomic_load_n (ring.cq_tail, __ATOMIC_ACQUIRE))
        {
          struct io_uring_cqe *cqe = &ring.cqes[head & *ring.cq_mask];
          LoadRequest *self = (LoadRequest *) (guintptr) cqe->user_data;
          int result = cqe->res;

          head++;
          __atomic_store_n (ring.cq_head, head, __ATOMIC_RELEASE);
          request_step (self, result);
        }
    }

  return NULL;
}

#endif

static gboolean
uring_ready (void)
{
  static gsize initialized = 0;
  static gboolean ready = FALSE;

  if (g_once_init_enter (&initialized))
    {
#ifdef SIT_HAVE_URING
      if (g_strcmp0 (g_getenv ("SIT_IO"), "pool") != 0 && ring_setup ())
        {
          g_thread_unref (g_thread_new ("sit-io", ring_reap, NULL));
          ready = TRUE;
        }
#endif
      g_debug ("sit: local files load through %s", ready ? "io_uring" : "the worker pool");
      g_once_init_leave (&initialized, 1);
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
#ifdef SIT_HAVE_URING
  if (uring_ready () && ring_push (self, IORING_OP_OPENAT))
    return;
#else
  uring_ready ();
#endif
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
