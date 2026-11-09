#include "config.h"

#include "sitringprivate.h"

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

typedef struct _SitRingRequest LoadRequest;

struct _SitRingRequest
{
  char *path;
  gboolean idle;
  GCancellable *cancellable;
  SitRingDone done;
  gpointer user_data;
  int fd;
  gboolean opened;
  guint8 *data;
  gsize size;
  gsize done_bytes;
  SitRingReadDone read_done;
  gpointer buffer;
  gsize count;
};

static void
request_free (LoadRequest *self)
{
  g_clear_object (&self->cancellable);
  g_free (self->path);
  g_free (self->data);
  g_free (self);
}

static void
request_end (LoadRequest    *self,
             SitRingOutcome  outcome,
             GBytes         *bytes,
             int             error)
{
  self->done (outcome, bytes, error, self->user_data);
  request_free (self);
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
static gboolean ring_cur_pos;

static gboolean
ring_setup (void)
{
  struct io_uring_params params = { 0 };
  gsize sq_size, cq_size;
  guint8 *sq, *cq;

  ring.fd = syscall (__NR_io_uring_setup, RING_ENTRIES, &params);
  if (ring.fd < 0)
    return FALSE;
  ring_cur_pos = (params.features & IORING_FEAT_RW_CUR_POS) != 0;

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
      else if (self->read_done)
        {
          sqe->fd = self->fd;
          sqe->addr = (guint64) (guintptr) self->buffer;
          sqe->len = self->count;
          sqe->off = (guint64) -1;
          if (self->idle)
            sqe->ioprio = IOPRIO_IDLE;
        }
      else
        {
          sqe->fd = self->fd;
          sqe->addr = (guint64) (guintptr) (self->data + self->done_bytes);
          sqe->len = self->size - self->done_bytes;
          sqe->off = self->done_bytes;
          if (self->idle)
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
request_step (LoadRequest *self,
              int          result)
{
  struct stat st;

  if (self->read_done)
    {
      self->read_done (result, self->user_data);
      request_free (self);
      return;
    }

  if (g_cancellable_is_cancelled (self->cancellable))
    {
      if (!self->opened && result >= 0)
        close (result);
      request_close (self);
      request_end (self, SIT_RING_FAILED, NULL, ECANCELED);
      return;
    }
  if (result < 0)
    {
      request_close (self);
      request_end (self, SIT_RING_FAILED, NULL, -result);
      return;
    }

  if (!self->opened)
    {
      self->fd = result;
      self->opened = TRUE;
      if (fstat (self->fd, &st) < 0)
        {
          int error = errno;

          request_close (self);
          request_end (self, SIT_RING_FAILED, NULL, error);
          return;
        }
      if (!S_ISREG (st.st_mode) || st.st_size == 0)
        {
          request_close (self);
          request_end (self, SIT_RING_UNSUITABLE, NULL, 0);
          return;
        }
      self->size = st.st_size;
      self->data = g_malloc (self->size);
    }
  else
    {
      if (result == 0)
        self->size = self->done_bytes;
      self->done_bytes += result;
    }

  if (self->done_bytes < self->size)
    {
      if (!ring_push (self, IORING_OP_READ))
        {
          request_close (self);
          request_end (self, SIT_RING_UNSUITABLE, NULL, 0);
        }
      return;
    }

  request_close (self);
  request_end (self, SIT_RING_LOADED,
               g_bytes_new_take (g_steal_pointer (&self->data), self->size), 0);
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

gboolean
sit_ring_available (void)
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
      g_once_init_leave (&initialized, 1);
    }

  return ready;
}

gboolean
sit_ring_load (const char   *path,
               gboolean      idle,
               GCancellable *cancellable,
               SitRingDone   done,
               gpointer      user_data)
{
#ifdef SIT_HAVE_URING
  LoadRequest *self;

  if (!sit_ring_available ())
    return FALSE;

  self = g_new0 (LoadRequest, 1);
  self->path = g_strdup (path);
  self->idle = idle;
  self->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
  self->done = done;
  self->user_data = user_data;
  if (ring_push (self, IORING_OP_OPENAT))
    return TRUE;
  request_free (self);
#endif
  return FALSE;
}

gboolean
sit_ring_read (int              fd,
               gpointer         buffer,
               gsize            count,
               gboolean         idle,
               SitRingReadDone  done,
               gpointer         user_data)
{
#ifdef SIT_HAVE_URING
  LoadRequest *self;

  if (!sit_ring_available () || !ring_cur_pos)
    return FALSE;

  self = g_new0 (LoadRequest, 1);
  self->fd = fd;
  self->buffer = buffer;
  self->count = MIN (count, G_MAXUINT32);
  self->idle = idle;
  self->read_done = done;
  self->user_data = user_data;
  if (ring_push (self, IORING_OP_READ))
    return TRUE;
  request_free (self);
#endif
  return FALSE;
}
