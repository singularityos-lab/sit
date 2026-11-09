#include "config.h"

#include "sitschedulerprivate.h"

#include "gdkeventsprivate.h"

#include <gio/gio.h>

#ifdef __linux__
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#define FOREGROUND 0
#define BACKGROUND 1
#define IDLE_TIMEOUT (15 * G_TIME_SPAN_SECOND)
#define DRAIN_BUDGET (2 * G_TIME_SPAN_MILLISECOND)
#define DRAIN_PRIORITY (GDK_PRIORITY_REDRAW + 1)
#define MAX_CORES 64

typedef struct
{
  GMutex lock;
  GCond cond[2];
  GQueue queue[MAX_CORES][N_QOS];
  guint cores;
  guint next_core;
  guint64 slots[2];
  guint queued[N_QOS];
  guint workers[2];
  guint idle[2];
  guint woken[2];
  guint limit[2];
  guint utility_running;
  guint utility_limit;
} Scheduler;

static Scheduler scheduler;
static GMutex announce_lock;
static GArray *background_tids;
static GDBusConnection *announce_bus;
static int announce_state;
static GPrivate home_core;
static SitJob *completed[N_QOS];
static int drain_armed;
static GSource *drain_source;
static SitJob *pending_head[N_QOS];
static SitJob *pending_tail[N_QOS];

gboolean
sit_scheduler_drain (gint64 budget)
{
  gint64 start = g_get_monotonic_time ();

  g_atomic_int_set (&drain_armed, 0);
  for (int q = 0; q < N_QOS; q++)
    {
      SitJob *list = g_atomic_pointer_exchange (&completed[q], NULL);
      SitJob *reversed = NULL;

      while (list)
        {
          SitJob *next = list->next;
          list->next = reversed;
          reversed = list;
          list = next;
        }
      if (!reversed)
        continue;
      if (pending_tail[q])
        pending_tail[q]->next = reversed;
      else
        pending_head[q] = reversed;
      while (reversed->next)
        reversed = reversed->next;
      pending_tail[q] = reversed;
    }

  for (int q = 0; q < N_QOS; q++)
    {
      while (pending_head[q])
        {
          SitJob *job = pending_head[q];

          pending_head[q] = job->next;
          if (!pending_head[q])
            pending_tail[q] = NULL;
          job->next = NULL;
          job->deliver (job);
          if (budget >= 0 && g_get_monotonic_time () - start >= budget)
            goto out;
        }
    }

out:
  for (int q = 0; q < N_QOS; q++)
    if (pending_head[q] || g_atomic_pointer_get (&completed[q]))
      return TRUE;
  return FALSE;
}

#define FRAME_WINDOW (50 * G_TIME_SPAN_MILLISECOND)

static gint64 last_frame;

static gboolean
drain_dispatch (GSource     *source,
                GSourceFunc  callback,
                gpointer     user_data)
{
  gint64 now = g_get_monotonic_time ();

  if (last_frame != 0 && now - last_frame < FRAME_WINDOW)
    {
      g_source_set_ready_time (source, last_frame + FRAME_WINDOW);
      return G_SOURCE_CONTINUE;
    }

  if (sit_scheduler_drain (DRAIN_BUDGET))
    g_source_set_ready_time (source, 0);
  else
    {
      g_source_set_ready_time (source, -1);
      if (g_atomic_int_get (&drain_armed))
        g_source_set_ready_time (source, 0);
    }
  return G_SOURCE_CONTINUE;
}

void
sit_scheduler_after_frame (void)
{
  if (drain_source == NULL)
    return;

  last_frame = g_get_monotonic_time ();
  if (sit_scheduler_drain (DRAIN_BUDGET))
    g_source_set_ready_time (drain_source, last_frame + FRAME_WINDOW);
}

static GSourceFuncs drain_funcs = {
  .dispatch = drain_dispatch,
};

void
sit_scheduler_init (void)
{
  static gsize initialized = 0;

  if (g_once_init_enter (&initialized))
    {
      guint cores = MAX (2, g_get_num_processors ());

      g_mutex_init (&scheduler.lock);
      for (int k = 0; k < 2; k++)
        g_cond_init (&scheduler.cond[k]);
      scheduler.cores = MIN (cores, MAX_CORES);
      for (guint c = 0; c < scheduler.cores; c++)
        for (int q = 0; q < N_QOS; q++)
          g_queue_init (&scheduler.queue[c][q]);
      scheduler.limit[FOREGROUND] = scheduler.cores;
      scheduler.limit[BACKGROUND] = MAX (1, cores / 4);
      scheduler.utility_limit = MAX (1, cores / 2);

      drain_source = g_source_new (&drain_funcs, sizeof (GSource));
      g_source_set_static_name (drain_source, "[sit] async completions");
      g_source_set_priority (drain_source, DRAIN_PRIORITY);
      g_source_set_ready_time (drain_source, -1);
      g_source_attach (drain_source, g_main_context_default ());

      g_once_init_leave (&initialized, 1);
    }
}

void
sit_scheduler_post (SitJob *job)
{
  SitJob **stack = &completed[job->qos];
  SitJob *head;

  do
    {
      head = g_atomic_pointer_get (stack);
      job->next = head;
    }
  while (!g_atomic_pointer_compare_and_exchange (stack, head, job));

  if (g_atomic_int_compare_and_exchange (&drain_armed, 0, 1))
    g_source_set_ready_time (drain_source, 0);
}

static SitJob *
take (guint core,
      int   qos)
{
  if (scheduler.queued[qos] == 0)
    return NULL;

  for (guint i = 0; i < scheduler.cores; i++)
    {
      GQueue *queue = &scheduler.queue[(core + i) % scheduler.cores][qos];

      if (!g_queue_is_empty (queue))
        {
          scheduler.queued[qos]--;
          return g_queue_pop_head (queue);
        }
    }

  return NULL;
}

static SitJob *
pick (int   kind,
      guint core)
{
  SitJob *job;

  if (kind == BACKGROUND)
    return take (core, SIT_JOB_BACKGROUND);

  for (int q = SIT_JOB_INTERACTIVE; q <= SIT_JOB_USER; q++)
    if ((job = take (core, q)))
      return job;

  if (scheduler.utility_running < scheduler.utility_limit)
    return take (core, SIT_JOB_UTILITY);

  return NULL;
}

static gboolean
has_work (int kind)
{
  if (kind == BACKGROUND)
    return scheduler.queued[SIT_JOB_BACKGROUND] > 0;

  return scheduler.queued[SIT_JOB_INTERACTIVE] > 0 ||
         scheduler.queued[SIT_JOB_USER] > 0 ||
         (scheduler.utility_running < scheduler.utility_limit &&
          scheduler.queued[SIT_JOB_UTILITY] > 0);
}

static guint
claim_slot (int kind)
{
  for (guint c = 0; c < scheduler.cores; c++)
    if (!(scheduler.slots[kind] & (G_GUINT64_CONSTANT (1) << c)))
      {
        scheduler.slots[kind] |= G_GUINT64_CONSTANT (1) << c;
        return c;
      }

  return 0;
}

static gboolean
announce_wanted (void)
{
  if (announce_state == 0)
    {
      const char *policy[] = {
        "/usr/share/dbus-1/system.d/dev.sinty.Scheduler.conf",
        "/etc/dbus-1/system.d/dev.sinty.Scheduler.conf",
      };

      announce_state = 2;
      if (g_strcmp0 (g_getenv ("SIT_ANNOUNCE"), "off") != 0)
        for (guint i = 0; i < G_N_ELEMENTS (policy); i++)
          if (g_file_test (policy[i], G_FILE_TEST_EXISTS))
            announce_state = 1;
    }

  return announce_state == 1;
}

static void
announce_send_locked (void)
{
  GVariantBuilder builder;

  if (!announce_bus || !background_tids)
    return;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("au"));
  for (guint i = 0; i < background_tids->len; i++)
    g_variant_builder_add (&builder, "u", g_array_index (background_tids, guint32, i));
  g_dbus_connection_call (announce_bus, "dev.sinty.Scheduler", "/dev/sinty/Scheduler",
                          "dev.sinty.Scheduler1", "Announce",
                          g_variant_new ("(au)", &builder), NULL,
                          G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, NULL, NULL, NULL);
}

static gpointer
announce_connect (gpointer data)
{
  GDBusConnection *bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);

  g_mutex_lock (&announce_lock);
  announce_bus = bus;
  announce_send_locked ();
  g_mutex_unlock (&announce_lock);

  return NULL;
}

static void
announce_start (void)
{
  static gsize started = 0;

  if (g_once_init_enter (&started))
    {
      if (announce_wanted ())
        g_thread_unref (g_thread_new ("sit-announce", announce_connect, NULL));
      g_once_init_leave (&started, 1);
    }
}

static void
announce_background (gboolean joining)
{
#ifdef SYS_gettid
  guint32 tid = (guint32) syscall (SYS_gettid);

  g_mutex_lock (&announce_lock);
  if (!background_tids)
    background_tids = g_array_new (FALSE, FALSE, sizeof (guint32));
  if (joining)
    g_array_append_val (background_tids, tid);
  else
    for (guint i = 0; i < background_tids->len; i++)
      if (g_array_index (background_tids, guint32, i) == tid)
        {
          g_array_remove_index_fast (background_tids, i);
          break;
        }
  announce_send_locked ();
  g_mutex_unlock (&announce_lock);
#endif
}

static gpointer
worker_main (gpointer data)
{
  int kind = GPOINTER_TO_INT (data);

#ifdef SCHED_IDLE
  if (kind == BACKGROUND)
    {
      struct sched_param param = { 0 };

      sched_setscheduler (0, SCHED_IDLE, &param);
    }
#endif
#ifdef SYS_ioprio_set
  if (kind == BACKGROUND)
    syscall (SYS_ioprio_set, 1, 0, 3 << 13);
#endif
  if (kind == BACKGROUND)
    announce_background (TRUE);

  g_mutex_lock (&scheduler.lock);
  guint core = claim_slot (kind);
  g_private_set (&home_core, GUINT_TO_POINTER (core + 1));
  for (;;)
    {
      SitJob *job = pick (kind, core);

      if (job)
        {
          gboolean utility = job->qos == SIT_JOB_UTILITY;

          if (utility)
            scheduler.utility_running++;
          g_mutex_unlock (&scheduler.lock);
          job->run (job);
          g_mutex_lock (&scheduler.lock);
          if (utility)
            {
              scheduler.utility_running--;
              if (scheduler.queued[SIT_JOB_UTILITY] > 0 &&
                  scheduler.idle[FOREGROUND] > scheduler.woken[FOREGROUND])
                {
                  scheduler.woken[FOREGROUND]++;
                  g_cond_signal (&scheduler.cond[FOREGROUND]);
                }
            }
          continue;
        }

      scheduler.idle[kind]++;
      gboolean signaled = g_cond_wait_until (&scheduler.cond[kind], &scheduler.lock,
                                             g_get_monotonic_time () + IDLE_TIMEOUT);
      scheduler.idle[kind]--;
      if (scheduler.woken[kind] > 0)
        scheduler.woken[kind]--;
      if (!signaled && !has_work (kind))
        break;
    }
  scheduler.workers[kind]--;
  scheduler.slots[kind] &= ~(G_GUINT64_CONSTANT (1) << core);
  g_mutex_unlock (&scheduler.lock);

  if (kind == BACKGROUND)
    announce_background (FALSE);

  return NULL;
}

void
sit_scheduler_submit (SitJob *job)
{
  int kind = job->qos == SIT_JOB_BACKGROUND ? BACKGROUND : FOREGROUND;
  gboolean inline_run = FALSE;

  sit_scheduler_init ();
  if (kind == BACKGROUND)
    announce_start ();

  g_mutex_lock (&scheduler.lock);
  guint home = GPOINTER_TO_UINT (g_private_get (&home_core));
  guint core = home > 0 ? home - 1 : scheduler.next_core++ % scheduler.cores;
  g_queue_push_tail (&scheduler.queue[core][job->qos], job);
  scheduler.queued[job->qos]++;
  if (scheduler.idle[kind] > scheduler.woken[kind])
    {
      scheduler.woken[kind]++;
      g_cond_signal (&scheduler.cond[kind]);
    }
  else if (scheduler.workers[kind] < scheduler.limit[kind])
    {
      GThread *thread;

      scheduler.workers[kind]++;
      thread = g_thread_try_new (kind == BACKGROUND ? "sit-background" : "sit-worker",
                                 worker_main, GINT_TO_POINTER (kind), NULL);
      if (thread)
        g_thread_unref (thread);
      else if (--scheduler.workers[kind] == 0)
        {
          inline_run = g_queue_remove (&scheduler.queue[core][job->qos], job);
          if (inline_run)
            scheduler.queued[job->qos]--;
        }
    }
  g_mutex_unlock (&scheduler.lock);

  if (inline_run)
    job->run (job);
}


void
sit_scheduler_settle (void)
{
  sit_scheduler_init ();
  while (sit_scheduler_drain (-1))
    ;
  g_source_set_ready_time (drain_source, -1);
}

gboolean
sit_scheduler_enabled (void)
{
  static int enabled = -1;

  if (g_atomic_int_get (&enabled) < 0)
    g_atomic_int_set (&enabled, g_strcmp0 (g_getenv ("SIT_ASYNC"), "off") != 0);

  return g_atomic_int_get (&enabled);
}

typedef struct
{
  SitJob job;
  GFunc func;
  gpointer object;
} SpawnJob;

static void
spawn_run (SitJob *job)
{
  SpawnJob *self = (SpawnJob *) job;

  self->func (self->object, NULL);
  sit_scheduler_post (job);
}

static void
spawn_deliver (SitJob *job)
{
  SpawnJob *self = (SpawnJob *) job;

  g_object_unref (self->object);
  g_free (self);
}

void
sit_scheduler_spawn (SitJobClass  qos,
                     GFunc        func,
                     gpointer     object)
{
  SpawnJob *self = g_new0 (SpawnJob, 1);

  self->job.qos = qos;
  self->job.run = spawn_run;
  self->job.deliver = spawn_deliver;
  self->func = func;
  self->object = g_object_ref (object);
  sit_scheduler_submit (&self->job);
}

typedef struct
{
  int ref_count;
  guint n;
  guint next;
  guint done;
  GMutex lock;
  GCond cond;
  SitParallelFunc func;
  gpointer data;
} Parallel;

typedef struct
{
  SitJob job;
  Parallel *parallel;
} ParallelJob;

static void
parallel_unref (Parallel *self)
{
  if (!g_atomic_int_dec_and_test (&self->ref_count))
    return;

  g_mutex_clear (&self->lock);
  g_cond_clear (&self->cond);
  g_free (self);
}

static void
parallel_work (Parallel *self)
{
  for (;;)
    {
      guint index = (guint) g_atomic_int_add (&self->next, 1);

      if (index >= self->n)
        break;

      self->func (index, self->data);

      if ((guint) g_atomic_int_add (&self->done, 1) + 1 == self->n)
        {
          g_mutex_lock (&self->lock);
          g_cond_broadcast (&self->cond);
          g_mutex_unlock (&self->lock);
        }
    }
}

static void
parallel_run (SitJob *job)
{
  ParallelJob *self = (ParallelJob *) job;

  parallel_work (self->parallel);
  parallel_unref (self->parallel);
  g_free (self);
}

void
sit_scheduler_parallel (SitJobClass     qos,
                        guint           n,
                        SitParallelFunc func,
                        gpointer        data)
{
  Parallel *self;
  guint helpers;

  if (n == 0)
    return;

  sit_scheduler_init ();
  helpers = MIN (n - 1, scheduler.limit[FOREGROUND] - 1);
  if (helpers == 0 || !sit_scheduler_enabled ())
    {
      for (guint i = 0; i < n; i++)
        func (i, data);
      return;
    }

  self = g_new0 (Parallel, 1);
  self->ref_count = 1 + helpers;
  self->n = n;
  self->func = func;
  self->data = data;
  g_mutex_init (&self->lock);
  g_cond_init (&self->cond);

  for (guint i = 0; i < helpers; i++)
    {
      ParallelJob *job = g_new0 (ParallelJob, 1);

      job->job.qos = qos == SIT_JOB_BACKGROUND ? SIT_JOB_UTILITY : qos;
      job->job.run = parallel_run;
      job->parallel = self;
      sit_scheduler_submit (&job->job);
    }

  parallel_work (self);

  g_mutex_lock (&self->lock);
  while ((guint) g_atomic_int_get (&self->done) < n)
    g_cond_wait (&self->cond, &self->lock);
  g_mutex_unlock (&self->lock);

  parallel_unref (self);
}
