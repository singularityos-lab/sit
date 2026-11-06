#include "config.h"

#include "sit.h"

#include <gio/gio.h>
#include <string.h>

#ifdef __linux__
#include <sched.h>
#endif

#define N_QOS 4
#define FOREGROUND 0
#define BACKGROUND 1
#define IDLE_TIMEOUT (15 * G_TIME_SPAN_SECOND)
#define DRAIN_BUDGET (2 * G_TIME_SPAN_MILLISECOND)
#define DRAIN_PRIORITY (GDK_PRIORITY_REDRAW + 1)

G_DEFINE_ENUM_TYPE (SitQos, sit_qos,
  G_DEFINE_ENUM_VALUE (SIT_QOS_INTERACTIVE, "interactive"),
  G_DEFINE_ENUM_VALUE (SIT_QOS_USER, "user"),
  G_DEFINE_ENUM_VALUE (SIT_QOS_UTILITY, "utility"),
  G_DEFINE_ENUM_VALUE (SIT_QOS_BACKGROUND, "background"))

typedef struct _Job Job;

struct _Job
{
  Job *next;
  SitQos qos;
  void (* run) (Job *job);
  void (* deliver) (Job *job);
};

typedef struct
{
  GMutex lock;
  GCond cond[2];
  GQueue queue[N_QOS];
  guint workers[2];
  guint idle[2];
  guint woken[2];
  guint limit[2];
  guint utility_running;
  guint utility_limit;
} Scheduler;

static Scheduler scheduler;
static Job *completed[N_QOS];
static int drain_armed;
static GSource *drain_source;
static Job *pending_head[N_QOS];
static Job *pending_tail[N_QOS];

static void schedule (Job *job);

static gboolean
drain (gint64 budget)
{
  gint64 start = g_get_monotonic_time ();

  g_atomic_int_set (&drain_armed, 0);
  for (int q = 0; q < N_QOS; q++)
    {
      Job *list = g_atomic_pointer_exchange (&completed[q], NULL);
      Job *reversed = NULL;

      while (list)
        {
          Job *next = list->next;
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
          Job *job = pending_head[q];

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

static gboolean
drain_dispatch (GSource     *source,
                GSourceFunc  callback,
                gpointer     user_data)
{
  if (drain (DRAIN_BUDGET))
    g_source_set_ready_time (source, 0);
  else
    g_source_set_ready_time (source, -1);
  return G_SOURCE_CONTINUE;
}

static GSourceFuncs drain_funcs = {
  .dispatch = drain_dispatch,
};

static void
scheduler_init (void)
{
  static gsize initialized = 0;

  if (g_once_init_enter (&initialized))
    {
      guint cores = MAX (2, g_get_num_processors ());

      g_mutex_init (&scheduler.lock);
      for (int k = 0; k < 2; k++)
        g_cond_init (&scheduler.cond[k]);
      for (int q = 0; q < N_QOS; q++)
        g_queue_init (&scheduler.queue[q]);
      scheduler.limit[FOREGROUND] = cores;
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

static void
post (Job *job)
{
  Job **stack = &completed[job->qos];
  Job *head;

  do
    {
      head = g_atomic_pointer_get (stack);
      job->next = head;
    }
  while (!g_atomic_pointer_compare_and_exchange (stack, head, job));

  if (g_atomic_int_compare_and_exchange (&drain_armed, 0, 1))
    g_source_set_ready_time (drain_source, 0);
}

static Job *
pick (int kind)
{
  if (kind == BACKGROUND)
    return g_queue_pop_head (&scheduler.queue[SIT_QOS_BACKGROUND]);

  for (int q = SIT_QOS_INTERACTIVE; q <= SIT_QOS_USER; q++)
    if (!g_queue_is_empty (&scheduler.queue[q]))
      return g_queue_pop_head (&scheduler.queue[q]);

  if (scheduler.utility_running < scheduler.utility_limit)
    return g_queue_pop_head (&scheduler.queue[SIT_QOS_UTILITY]);

  return NULL;
}

static gboolean
has_work (int kind)
{
  if (kind == BACKGROUND)
    return !g_queue_is_empty (&scheduler.queue[SIT_QOS_BACKGROUND]);

  return !g_queue_is_empty (&scheduler.queue[SIT_QOS_INTERACTIVE]) ||
         !g_queue_is_empty (&scheduler.queue[SIT_QOS_USER]) ||
         (scheduler.utility_running < scheduler.utility_limit &&
          !g_queue_is_empty (&scheduler.queue[SIT_QOS_UTILITY]));
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

  g_mutex_lock (&scheduler.lock);
  for (;;)
    {
      Job *job = pick (kind);

      if (job)
        {
          gboolean utility = job->qos == SIT_QOS_UTILITY;

          if (utility)
            scheduler.utility_running++;
          g_mutex_unlock (&scheduler.lock);
          job->run (job);
          g_mutex_lock (&scheduler.lock);
          if (utility)
            {
              scheduler.utility_running--;
              if (!g_queue_is_empty (&scheduler.queue[SIT_QOS_UTILITY]) &&
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
  g_mutex_unlock (&scheduler.lock);

  return NULL;
}

static void
schedule (Job *job)
{
  int kind = job->qos == SIT_QOS_BACKGROUND ? BACKGROUND : FOREGROUND;
  gboolean inline_run = FALSE;

  scheduler_init ();

  g_mutex_lock (&scheduler.lock);
  g_queue_push_tail (&scheduler.queue[job->qos], job);
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
        inline_run = g_queue_remove (&scheduler.queue[job->qos], job);
    }
  g_mutex_unlock (&scheduler.lock);

  if (inline_run)
    job->run (job);
}

typedef struct
{
  Job job;
  GTask *task;
  GCancellable *cancellable;
  SitFuture *future;
  SitTaskFunc func;
  gpointer func_data;
  GDestroyNotify func_data_destroy;
  GDestroyNotify result_destroy;
  gpointer result;
  GError *error;
  gboolean direct;
} TaskJob;

typedef void (* CancelFunc) (GTask *task);

typedef struct
{
  Job job;
  GTask *task;
  CancelFunc func;
} CancelJob;

typedef struct
{
  CancelFunc func;
  GTask *task;
} CancelWatch;

static void
cancel_job_deliver (Job *job)
{
  CancelJob *self = (CancelJob *) job;

  self->func (self->task);
  g_object_unref (self->task);
  g_free (self);
}

static void
cancel_watch_fired (GCancellable *cancellable,
                    gpointer      data)
{
  CancelWatch *watch = data;
  CancelJob *job = g_new0 (CancelJob, 1);

  job->job.qos = SIT_QOS_INTERACTIVE;
  job->job.deliver = cancel_job_deliver;
  job->task = g_object_ref (watch->task);
  job->func = watch->func;
  post (&job->job);
}

static gulong
watch_cancel (GCancellable *cancellable,
              GTask        *task,
              CancelFunc    func)
{
  CancelWatch *watch;

  if (!cancellable)
    return 0;

  scheduler_init ();
  watch = g_new0 (CancelWatch, 1);
  watch->func = func;
  watch->task = task;
  return g_cancellable_connect (cancellable, G_CALLBACK (cancel_watch_fired), watch, g_free);
}

struct _SitScope
{
  GObject parent_instance;
  GCancellable *cancellable;
  SitScope *parent;
  gulong parent_handler;
};

G_DEFINE_FINAL_TYPE (SitScope, sit_scope, G_TYPE_OBJECT)

static void
sit_scope_finalize (GObject *object)
{
  SitScope *self = SIT_SCOPE (object);

  if (self->parent)
    {
      g_cancellable_disconnect (self->parent->cancellable, self->parent_handler);
      g_object_unref (self->parent);
    }
  g_object_unref (self->cancellable);

  G_OBJECT_CLASS (sit_scope_parent_class)->finalize (object);
}

static void
sit_scope_class_init (SitScopeClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = sit_scope_finalize;
}

static void
sit_scope_init (SitScope *self)
{
  self->cancellable = g_cancellable_new ();
}

static void
parent_cancelled (GCancellable *cancellable,
                  gpointer      data)
{
  g_cancellable_cancel (G_CANCELLABLE (data));
}

/**
 * sit_scope_new: (constructor)
 * @parent: (nullable): a scope whose cancellation also cancels the new one
 *
 * Returns: (transfer full): a scope that groups work cancelled together
 */
SitScope *
sit_scope_new (SitScope *parent)
{
  SitScope *self = g_object_new (SIT_TYPE_SCOPE, NULL);

  g_return_val_if_fail (parent == NULL || SIT_IS_SCOPE (parent), self);

  if (parent)
    {
      self->parent = g_object_ref (parent);
      self->parent_handler = g_cancellable_connect (parent->cancellable,
                                                    G_CALLBACK (parent_cancelled),
                                                    self->cancellable, NULL);
    }

  return self;
}

void
sit_scope_cancel (SitScope *self)
{
  g_return_if_fail (SIT_IS_SCOPE (self));

  g_cancellable_cancel (self->cancellable);
}

gboolean
sit_scope_is_cancelled (SitScope *self)
{
  g_return_val_if_fail (SIT_IS_SCOPE (self), TRUE);

  return g_cancellable_is_cancelled (self->cancellable);
}

/**
 * sit_scope_get_cancellable:
 * @self: a scope
 *
 * Returns: (transfer none): the cancellable cancelled with @self
 */
GCancellable *
sit_scope_get_cancellable (SitScope *self)
{
  g_return_val_if_fail (SIT_IS_SCOPE (self), NULL);

  return self->cancellable;
}

static void
widget_destroyed (GtkWidget *widget,
                  SitScope  *scope)
{
  sit_scope_cancel (scope);
}

static void
widget_scope_free (gpointer data)
{
  sit_scope_cancel (data);
  g_object_unref (data);
}

/**
 * sit_widget_get_scope:
 * @widget: a widget
 *
 * The scope is cancelled when @widget is destroyed, so work started in it
 * never delivers results to a disposed widget.
 *
 * Returns: (transfer none): the scope of @widget
 */
SitScope *
sit_widget_get_scope (GtkWidget *widget)
{
  static GQuark quark;
  SitScope *scope;

  g_return_val_if_fail (GTK_IS_WIDGET (widget), NULL);

  if (!quark)
    quark = g_quark_from_static_string ("sit-widget-scope");

  scope = g_object_get_qdata (G_OBJECT (widget), quark);
  if (!scope)
    {
      scope = sit_scope_new (NULL);
      g_object_set_qdata_full (G_OBJECT (widget), quark, scope, widget_scope_free);
      g_signal_connect (widget, "destroy", G_CALLBACK (widget_destroyed), scope);
    }

  return scope;
}

typedef struct
{
  GCancellable *cancellable;
  GCancellable *scope;
  GCancellable *caller;
  gulong scope_handler;
  gulong caller_handler;
} Link;

static void
link_fired (GCancellable *cancellable,
            gpointer      data)
{
  g_cancellable_cancel (G_CANCELLABLE (data));
}

static GCancellable *
link_new (Link         *link,
          SitScope     *scope,
          GCancellable *caller)
{
  memset (link, 0, sizeof (Link));
  link->cancellable = g_cancellable_new ();
  if (scope)
    {
      link->scope = g_object_ref (scope->cancellable);
      link->scope_handler = g_cancellable_connect (link->scope, G_CALLBACK (link_fired),
                                                   link->cancellable, NULL);
    }
  if (caller)
    {
      link->caller = g_object_ref (caller);
      link->caller_handler = g_cancellable_connect (link->caller, G_CALLBACK (link_fired),
                                                    link->cancellable, NULL);
    }
  return link->cancellable;
}

static void
link_clear (Link *link)
{
  if (link->scope)
    {
      g_cancellable_disconnect (link->scope, link->scope_handler);
      g_clear_object (&link->scope);
    }
  if (link->caller)
    {
      g_cancellable_disconnect (link->caller, link->caller_handler);
      g_clear_object (&link->caller);
    }
}

typedef struct
{
  TaskJob base;
  Link link;
} RunJob;

static void
task_job_run (Job *job)
{
  TaskJob *self = (TaskJob *) job;

  if (!g_cancellable_set_error_if_cancelled (self->cancellable, &self->error))
    self->result = self->func (self->cancellable, self->func_data, &self->error);

  if (self->direct)
    job->deliver (job);
  else
    post (job);
}

static void
run_job_deliver (Job *job)
{
  RunJob *self = (RunJob *) job;
  TaskJob *task = &self->base;

  if (task->error)
    {
      if (task->result && task->result_destroy)
        task->result_destroy (task->result);
      g_task_return_error (task->task, task->error);
    }
  else
    g_task_return_pointer (task->task, task->result, task->result_destroy);

  if (task->func_data_destroy)
    task->func_data_destroy (task->func_data);
  g_object_unref (task->task);
  link_clear (&self->link);
  g_clear_object (&self->link.cancellable);
  g_free (self);
}

/**
 * sit_task_run:
 * @scope: (nullable): the scope that cancels the work
 * @qos: the class of service of the work
 * @result_destroy: (nullable): frees a result that is never consumed
 * @func: (scope notified) (closure func_data) (destroy func_data_destroy): work run on a worker thread
 * @func_data: data for @func
 * @func_data_destroy: (nullable): frees @func_data on the thread that started the work
 * @cancellable: (nullable): a cancellable
 * @callback: (scope async) (closure user_data): called with the result
 * @user_data: data for @callback
 *
 * Runs @func on the worker pool of @qos. Results started from the default
 * main context are delivered in batches after the frame is drawn, interactive
 * results first, within a time budget per main loop iteration.
 */
void
sit_task_run (SitScope            *scope,
              SitQos               qos,
              GDestroyNotify       result_destroy,
              SitTaskFunc          func,
              gpointer             func_data,
              GDestroyNotify       func_data_destroy,
              GCancellable        *cancellable,
              GAsyncReadyCallback  callback,
              gpointer             user_data)
{
  RunJob *self;
  GMainContext *context;

  g_return_if_fail (scope == NULL || SIT_IS_SCOPE (scope));
  g_return_if_fail (qos <= SIT_QOS_BACKGROUND);
  g_return_if_fail (func != NULL);

  scheduler_init ();

  self = g_new0 (RunJob, 1);
  self->base.job.qos = qos;
  self->base.job.run = task_job_run;
  self->base.job.deliver = run_job_deliver;
  self->base.cancellable = link_new (&self->link, scope, cancellable);
  self->base.task = g_task_new (NULL, self->base.cancellable, callback, user_data);
  g_task_set_source_tag (self->base.task, sit_task_run);
  g_task_set_static_name (self->base.task, "sit_task_run");
  self->base.func = func;
  self->base.func_data = func_data;
  self->base.func_data_destroy = func_data_destroy;
  self->base.result_destroy = result_destroy;

  context = g_main_context_ref_thread_default ();
  self->base.direct = context != g_main_context_default ();
  g_main_context_unref (context);

  schedule (&self->base.job);
}

/**
 * sit_task_run_finish:
 * @result: the result passed to the callback of sit_task_run()
 * @error: return location for an error
 *
 * Returns: (transfer full) (nullable): the value returned by the work
 */
gpointer
sit_task_run_finish (GAsyncResult  *result,
                     GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);
  g_return_val_if_fail (g_task_get_source_tag (G_TASK (result)) == sit_task_run, NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

struct _SitFuture
{
  GObject parent_instance;
  GCancellable *cancellable;
  Link link;
  gboolean done;
  gpointer result;
  GDestroyNotify result_destroy;
  GError *error;
  GList *waiters;
  SitFuture *inner;
  guint timeout_id;
};

G_DEFINE_FINAL_TYPE (SitFuture, sit_future, G_TYPE_OBJECT)

typedef struct
{
  GCancellable *cancellable;
  gulong handler;
} Waiter;

static void
waiter_free (gpointer data)
{
  Waiter *waiter = data;

  if (waiter->cancellable)
    {
      g_cancellable_disconnect (waiter->cancellable, waiter->handler);
      g_object_unref (waiter->cancellable);
    }
  g_free (waiter);
}

static void
waiter_return (SitFuture *self,
               GTask     *task)
{
  if (self->error)
    g_task_return_error (task, g_error_copy (self->error));
  else
    g_task_return_pointer (task, self->result, NULL);
}

static void
future_complete (SitFuture *self,
                 gpointer   result,
                 GError    *error)
{
  GList *waiters;

  g_assert (!self->done);

  self->done = TRUE;
  self->result = result;
  self->error = error;
  g_clear_handle_id (&self->timeout_id, g_source_remove);
  link_clear (&self->link);

  waiters = g_steal_pointer (&self->waiters);
  g_object_ref (self);
  for (GList *l = waiters; l; l = l->next)
    {
      waiter_return (self, l->data);
      g_object_unref (l->data);
    }
  g_list_free (waiters);
  g_object_unref (self);
}

static void
sit_future_dispose (GObject *object)
{
  SitFuture *self = SIT_FUTURE (object);

  g_clear_handle_id (&self->timeout_id, g_source_remove);
  g_clear_object (&self->inner);

  G_OBJECT_CLASS (sit_future_parent_class)->dispose (object);
}

static void
sit_future_finalize (GObject *object)
{
  SitFuture *self = SIT_FUTURE (object);

  link_clear (&self->link);
  g_clear_object (&self->link.cancellable);
  if (self->result && self->result_destroy)
    self->result_destroy (self->result);
  g_clear_error (&self->error);

  G_OBJECT_CLASS (sit_future_parent_class)->finalize (object);
}

static void
sit_future_class_init (SitFutureClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = sit_future_dispose;
  object_class->finalize = sit_future_finalize;
}

static void
sit_future_init (SitFuture *self)
{
}

static void
future_job_deliver (Job *job)
{
  TaskJob *self = (TaskJob *) job;
  SitFuture *future = self->future;

  if (future->done)
    {
      if (self->result && self->result_destroy)
        self->result_destroy (self->result);
      g_clear_error (&self->error);
    }
  else if (!self->error && g_cancellable_is_cancelled (self->cancellable))
    {
      if (self->result && self->result_destroy)
        self->result_destroy (self->result);
      future_complete (future, NULL,
                       g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "Operation was cancelled"));
    }
  else if (self->error)
    {
      if (self->result && self->result_destroy)
        self->result_destroy (self->result);
      future_complete (future, NULL, self->error);
    }
  else
    future_complete (future, self->result, NULL);

  if (self->func_data_destroy)
    self->func_data_destroy (self->func_data);
  g_object_unref (self->cancellable);
  g_object_unref (future);
  g_free (self);
}

/**
 * sit_future_spawn:
 * @scope: (nullable): the scope that cancels the work
 * @qos: the class of service of the work
 * @result_destroy: (nullable): frees the result with the future
 * @func: (scope notified) (closure func_data) (destroy func_data_destroy): work run on a worker thread
 * @func_data: data for @func
 * @func_data_destroy: (nullable): frees @func_data on the main thread
 *
 * Starts @func at once. The future completes on the default main context and
 * keeps its result until it is finalized.
 *
 * Returns: (transfer full): the future of the work
 */
SitFuture *
sit_future_spawn (SitScope       *scope,
                  SitQos          qos,
                  GDestroyNotify  result_destroy,
                  SitTaskFunc     func,
                  gpointer        func_data,
                  GDestroyNotify  func_data_destroy)
{
  SitFuture *future;
  TaskJob *job;

  g_return_val_if_fail (scope == NULL || SIT_IS_SCOPE (scope), NULL);
  g_return_val_if_fail (qos <= SIT_QOS_BACKGROUND, NULL);
  g_return_val_if_fail (func != NULL, NULL);

  scheduler_init ();

  future = g_object_new (SIT_TYPE_FUTURE, NULL);
  future->cancellable = link_new (&future->link, scope, NULL);
  future->result_destroy = result_destroy;

  job = g_new0 (TaskJob, 1);
  job->job.qos = qos;
  job->job.run = task_job_run;
  job->job.deliver = future_job_deliver;
  job->cancellable = g_object_ref (future->cancellable);
  job->future = g_object_ref (future);
  job->func = func;
  job->func_data = func_data;
  job->func_data_destroy = func_data_destroy;
  job->result_destroy = result_destroy;

  schedule (&job->job);

  return future;
}

void
sit_future_cancel (SitFuture *self)
{
  g_return_if_fail (SIT_IS_FUTURE (self));

  if (self->done)
    return;

  if (self->cancellable)
    g_cancellable_cancel (self->cancellable);
  if (self->inner)
    sit_future_cancel (self->inner);
  future_complete (self, NULL,
                   g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "Operation was cancelled"));
}

gboolean
sit_future_is_done (SitFuture *self)
{
  g_return_val_if_fail (SIT_IS_FUTURE (self), FALSE);

  return self->done;
}

static void
waiter_cancelled (GTask *task)
{
  SitFuture *self = g_task_get_source_object (task);
  GList *link = g_list_find (self->waiters, task);

  if (!link)
    return;

  self->waiters = g_list_delete_link (self->waiters, link);
  g_task_return_new_error_literal (task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Operation was cancelled");
  g_object_unref (task);
}

/**
 * sit_future_wait:
 * @self: a future
 * @cancellable: (nullable): stops waiting without cancelling the work
 * @callback: (scope async) (closure user_data): called once @self is done
 * @user_data: data for @callback
 */
void
sit_future_wait (SitFuture           *self,
                 GCancellable        *cancellable,
                 GAsyncReadyCallback  callback,
                 gpointer             user_data)
{
  GTask *task;
  Waiter *waiter;

  g_return_if_fail (SIT_IS_FUTURE (self));

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, sit_future_wait);
  g_task_set_check_cancellable (task, FALSE);

  if (self->done)
    {
      waiter_return (self, task);
      g_object_unref (task);
      return;
    }

  waiter = g_new0 (Waiter, 1);
  if (cancellable)
    {
      waiter->cancellable = g_object_ref (cancellable);
      waiter->handler = watch_cancel (cancellable, task, waiter_cancelled);
    }
  g_task_set_task_data (task, waiter, waiter_free);
  self->waiters = g_list_append (self->waiters, task);
}

/**
 * sit_future_wait_finish:
 * @self: a future
 * @result: the result passed to the callback of sit_future_wait()
 * @error: return location for an error
 *
 * Returns: (transfer none) (nullable): the value owned by @self
 */
gpointer
sit_future_wait_finish (SitFuture     *self,
                        GAsyncResult  *result,
                        GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static gboolean
future_timed_out (gpointer data)
{
  SitFuture *self = data;

  self->timeout_id = 0;
  g_object_ref (self);
  if (!self->done)
    future_complete (self, NULL,
                     g_error_new_literal (G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Operation timed out"));
  if (self->inner)
    sit_future_cancel (self->inner);
  g_object_unref (self);

  return G_SOURCE_REMOVE;
}

static void
inner_done (GObject      *source,
            GAsyncResult *result,
            gpointer      data)
{
  SitFuture *self = data;
  GError *error = NULL;
  gpointer value = sit_future_wait_finish (SIT_FUTURE (source), result, &error);

  if (!self->done)
    future_complete (self, value, error);
  else
    g_clear_error (&error);
  g_object_unref (self);
}

/**
 * sit_future_timeout:
 * @future: a future
 * @timeout_ms: the time allowed, in milliseconds
 *
 * Returns: (transfer full): a future with the result of @future, or
 *   %G_IO_ERROR_TIMED_OUT after @timeout_ms, which also cancels @future
 */
SitFuture *
sit_future_timeout (SitFuture *future,
                    guint      timeout_ms)
{
  SitFuture *self;

  g_return_val_if_fail (SIT_IS_FUTURE (future), NULL);

  self = g_object_new (SIT_TYPE_FUTURE, NULL);
  self->inner = g_object_ref (future);
  self->timeout_id = g_timeout_add (timeout_ms, future_timed_out, self);
  sit_future_wait (future, NULL, inner_done, g_object_ref (self));

  return self;
}

typedef enum {
  COMBINE_ALL,
  COMBINE_ANY,
  COMBINE_RACE
} CombineMode;

typedef struct
{
  CombineMode mode;
  guint remaining;
  guint failed;
  guint total;
  gboolean finished;
  GCancellable *cancellable;
  gulong handler;
} Combine;

typedef struct
{
  GTask *task;
  guint index;
} CombineSlot;

static void
combine_free (gpointer data)
{
  Combine *self = data;

  if (self->cancellable)
    {
      g_cancellable_disconnect (self->cancellable, self->handler);
      g_object_unref (self->cancellable);
    }
  g_free (self);
}

static void
combine_cancelled (GTask *task)
{
  Combine *self = g_task_get_task_data (task);

  if (self->finished)
    return;

  self->finished = TRUE;
  g_task_return_new_error_literal (task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Operation was cancelled");
}

static void
combine_one_done (GObject      *source,
                  GAsyncResult *result,
                  gpointer      data)
{
  CombineSlot *slot = data;
  Combine *self = g_task_get_task_data (slot->task);
  GError *error = NULL;

  sit_future_wait_finish (SIT_FUTURE (source), result, &error);
  self->remaining--;

  if (!self->finished)
    {
      switch (self->mode)
        {
        case COMBINE_ALL:
          if (error)
            {
              self->finished = TRUE;
              g_task_return_error (slot->task, g_steal_pointer (&error));
            }
          else if (self->remaining == 0)
            {
              self->finished = TRUE;
              g_task_return_boolean (slot->task, TRUE);
            }
          break;

        case COMBINE_ANY:
          if (!error)
            {
              self->finished = TRUE;
              g_task_return_int (slot->task, slot->index);
            }
          else if (++self->failed == self->total)
            {
              self->finished = TRUE;
              g_task_return_error (slot->task, g_steal_pointer (&error));
            }
          break;

        case COMBINE_RACE:
          self->finished = TRUE;
          g_task_return_int (slot->task, slot->index);
          break;

        default:
          g_assert_not_reached ();
        }
    }

  g_clear_error (&error);
  g_object_unref (slot->task);
  g_free (slot);
}

static void
combine (CombineMode           mode,
         gpointer              tag,
         SitFuture           **futures,
         guint                 n_futures,
         GCancellable         *cancellable,
         GAsyncReadyCallback   callback,
         gpointer              user_data)
{
  GTask *task = g_task_new (NULL, cancellable, callback, user_data);
  Combine *self = g_new0 (Combine, 1);

  g_task_set_source_tag (task, tag);
  g_task_set_check_cancellable (task, FALSE);
  self->mode = mode;
  self->remaining = n_futures;
  self->total = n_futures;
  g_task_set_task_data (task, self, combine_free);

  if (n_futures == 0)
    {
      self->finished = TRUE;
      if (mode == COMBINE_ALL)
        g_task_return_boolean (task, TRUE);
      else
        g_task_return_new_error_literal (task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "No futures");
      g_object_unref (task);
      return;
    }

  if (cancellable)
    {
      self->cancellable = g_object_ref (cancellable);
      self->handler = watch_cancel (cancellable, task, combine_cancelled);
    }

  for (guint i = 0; i < n_futures; i++)
    {
      CombineSlot *slot = g_new0 (CombineSlot, 1);

      slot->task = g_object_ref (task);
      slot->index = i;
      sit_future_wait (futures[i], NULL, combine_one_done, slot);
    }
  g_object_unref (task);
}

/**
 * sit_future_all:
 * @futures: (array length=n_futures): the futures
 * @n_futures: the number of @futures
 * @cancellable: (nullable): stops waiting
 * @callback: (scope async) (closure user_data): called when every future succeeded or one failed
 * @user_data: data for @callback
 */
void
sit_future_all (SitFuture           **futures,
                guint                 n_futures,
                GCancellable         *cancellable,
                GAsyncReadyCallback   callback,
                gpointer              user_data)
{
  combine (COMBINE_ALL, sit_future_all, futures, n_futures, cancellable, callback, user_data);
}

gboolean
sit_future_all_finish (GAsyncResult  *result,
                       GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

/**
 * sit_future_any:
 * @futures: (array length=n_futures): the futures
 * @n_futures: the number of @futures
 * @cancellable: (nullable): stops waiting
 * @callback: (scope async) (closure user_data): called with the first success, or when all failed
 * @user_data: data for @callback
 */
void
sit_future_any (SitFuture           **futures,
                guint                 n_futures,
                GCancellable         *cancellable,
                GAsyncReadyCallback   callback,
                gpointer              user_data)
{
  combine (COMBINE_ANY, sit_future_any, futures, n_futures, cancellable, callback, user_data);
}

/**
 * sit_future_any_finish:
 * @result: the result passed to the callback of sit_future_any()
 * @error: return location for the last error when every future failed
 *
 * Returns: the index of the first future that succeeded, or -1
 */
int
sit_future_any_finish (GAsyncResult  *result,
                       GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), -1);

  return g_task_propagate_int (G_TASK (result), error);
}

/**
 * sit_future_race:
 * @futures: (array length=n_futures): the futures
 * @n_futures: the number of @futures
 * @cancellable: (nullable): stops waiting
 * @callback: (scope async) (closure user_data): called when the first future is done
 * @user_data: data for @callback
 */
void
sit_future_race (SitFuture           **futures,
                 guint                 n_futures,
                 GCancellable         *cancellable,
                 GAsyncReadyCallback   callback,
                 gpointer              user_data)
{
  combine (COMBINE_RACE, sit_future_race, futures, n_futures, cancellable, callback, user_data);
}

/**
 * sit_future_race_finish:
 * @result: the result passed to the callback of sit_future_race()
 * @error: return location for an error
 *
 * Returns: the index of the first future that is done, or -1
 */
int
sit_future_race_finish (GAsyncResult  *result,
                        GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), -1);

  return g_task_propagate_int (G_TASK (result), error);
}

struct _SitStream
{
  GObject parent_instance;
  GMutex lock;
  GPtrArray *items;
  gboolean closed;
  GError *error;
  GTask *waiter;
  gboolean posted;
  GCancellable *cancellable;
  Link link;
};

G_DEFINE_FINAL_TYPE (SitStream, sit_stream, G_TYPE_OBJECT)

typedef struct
{
  Job job;
  SitStream *stream;
} StreamJob;

static void stream_job_deliver (Job *job);

static void
stream_post_locked (SitStream *self)
{
  StreamJob *job;

  if (!self->waiter || self->posted || (self->items->len == 0 && !self->closed))
    return;

  self->posted = TRUE;
  job = g_new0 (StreamJob, 1);
  job->job.qos = SIT_QOS_USER;
  job->job.deliver = stream_job_deliver;
  job->stream = g_object_ref (self);
  post (&job->job);
}

static void
stream_job_deliver (Job *job)
{
  StreamJob *self = (StreamJob *) job;
  SitStream *stream = self->stream;
  GTask *task = NULL;
  GPtrArray *batch = NULL;
  GError *error = NULL;

  g_mutex_lock (&stream->lock);
  stream->posted = FALSE;
  if (stream->waiter && (stream->items->len > 0 || stream->closed))
    {
      task = g_steal_pointer (&stream->waiter);
      if (stream->items->len > 0)
        {
          batch = g_steal_pointer (&stream->items);
          stream->items = g_ptr_array_new_with_free_func (g_object_unref);
        }
      else if (stream->error)
        error = g_error_copy (stream->error);
    }
  g_mutex_unlock (&stream->lock);

  if (task)
    {
      if (error)
        g_task_return_error (task, error);
      else
        g_task_return_pointer (task, batch, (GDestroyNotify) g_ptr_array_unref);
      g_object_unref (task);
    }

  g_object_unref (stream);
  g_free (self);
}

static void
sit_stream_finalize (GObject *object)
{
  SitStream *self = SIT_STREAM (object);

  link_clear (&self->link);
  g_clear_object (&self->link.cancellable);
  g_ptr_array_unref (self->items);
  g_clear_error (&self->error);
  g_mutex_clear (&self->lock);

  G_OBJECT_CLASS (sit_stream_parent_class)->finalize (object);
}

static void
sit_stream_class_init (SitStreamClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = sit_stream_finalize;
}

static void
sit_stream_init (SitStream *self)
{
  g_mutex_init (&self->lock);
}

/**
 * sit_stream_new:
 *
 * A stream carries items pushed from any thread to the main thread, which
 * receives everything pushed since its last read in one batch.
 *
 * Returns: (transfer full): an open stream
 */
SitStream *
sit_stream_new (void)
{
  SitStream *self = g_object_new (SIT_TYPE_STREAM, NULL);

  scheduler_init ();
  self->items = g_ptr_array_new_with_free_func (g_object_unref);

  return self;
}

/**
 * sit_stream_push:
 * @self: a stream
 * @item: (transfer full): the item
 *
 * Can be called from any thread. Items pushed after the stream is closed are freed.
 */
void
sit_stream_push (SitStream *self,
                 GObject   *item)
{
  g_return_if_fail (SIT_IS_STREAM (self));
  g_return_if_fail (G_IS_OBJECT (item));

  g_mutex_lock (&self->lock);
  if (self->closed)
    {
      g_mutex_unlock (&self->lock);
      g_object_unref (item);
      return;
    }
  g_ptr_array_add (self->items, item);
  stream_post_locked (self);
  g_mutex_unlock (&self->lock);
}

/**
 * sit_stream_close:
 * @self: a stream
 * @error: (nullable): why the stream ended early
 *
 * Can be called from any thread. Readers get the remaining items, then
 * @error or the end of the stream.
 */
void
sit_stream_close (SitStream    *self,
                  const GError *error)
{
  g_return_if_fail (SIT_IS_STREAM (self));

  g_mutex_lock (&self->lock);
  if (!self->closed)
    {
      self->closed = TRUE;
      if (error)
        self->error = g_error_copy (error);
      stream_post_locked (self);
    }
  g_mutex_unlock (&self->lock);
}

static void
stream_waiter_cancelled (GTask *task)
{
  SitStream *self = g_task_get_source_object (task);
  gboolean mine;

  g_mutex_lock (&self->lock);
  mine = self->waiter == task;
  if (mine)
    self->waiter = NULL;
  g_mutex_unlock (&self->lock);

  if (mine)
    {
      g_task_return_new_error_literal (task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Operation was cancelled");
      g_object_unref (task);
    }
}

/**
 * sit_stream_next:
 * @self: a stream
 * @cancellable: (nullable): stops waiting
 * @callback: (scope async) (closure user_data): called with the next batch
 * @user_data: data for @callback
 *
 * Only one read can be pending at a time.
 */
void
sit_stream_next (SitStream           *self,
                 GCancellable        *cancellable,
                 GAsyncReadyCallback  callback,
                 gpointer             user_data)
{
  GTask *task;
  Waiter *waiter;

  g_return_if_fail (SIT_IS_STREAM (self));

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, sit_stream_next);
  g_task_set_check_cancellable (task, FALSE);

  waiter = g_new0 (Waiter, 1);
  if (cancellable)
    {
      waiter->cancellable = g_object_ref (cancellable);
      waiter->handler = watch_cancel (cancellable, task, stream_waiter_cancelled);
    }
  g_task_set_task_data (task, waiter, waiter_free);

  g_mutex_lock (&self->lock);
  if (self->waiter)
    {
      g_mutex_unlock (&self->lock);
      g_task_return_new_error_literal (task, G_IO_ERROR, G_IO_ERROR_PENDING, "A read is already pending");
      g_object_unref (task);
      return;
    }
  self->waiter = task;
  stream_post_locked (self);
  g_mutex_unlock (&self->lock);
}

/**
 * sit_stream_next_finish:
 * @self: a stream
 * @result: the result passed to the callback of sit_stream_next()
 * @error: return location for an error
 *
 * Returns: (element-type GObject) (transfer full) (nullable): the items in
 *   push order, or %NULL without an error at the end of the stream
 */
GPtrArray *
sit_stream_next_finish (SitStream     *self,
                        GAsyncResult  *result,
                        GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

typedef struct
{
  Job job;
  SitStream *stream;
  SitStreamFunc func;
  gpointer func_data;
  GDestroyNotify func_data_destroy;
} ProducerJob;

static void
producer_job_deliver (Job *job)
{
  ProducerJob *self = (ProducerJob *) job;

  if (self->func_data_destroy)
    self->func_data_destroy (self->func_data);
  g_object_unref (self->stream);
  g_free (self);
}

static void
producer_job_run (Job *job)
{
  ProducerJob *self = (ProducerJob *) job;
  GError *error = NULL;

  if (!g_cancellable_set_error_if_cancelled (self->stream->cancellable, &error))
    self->func (self->stream, self->stream->cancellable, self->func_data, &error);
  if (!error)
    g_cancellable_set_error_if_cancelled (self->stream->cancellable, &error);
  sit_stream_close (self->stream, error);
  g_clear_error (&error);

  post (job);
}

/**
 * sit_stream_spawn:
 * @scope: (nullable): the scope that cancels the producer
 * @qos: the class of service of the producer
 * @func: (scope notified) (closure func_data) (destroy func_data_destroy): pushes items on a worker thread
 * @func_data: data for @func
 * @func_data_destroy: (nullable): frees @func_data on the main thread
 *
 * The stream closes when @func returns, with its error if it failed.
 *
 * Returns: (transfer full): the stream fed by @func
 */
SitStream *
sit_stream_spawn (SitScope       *scope,
                  SitQos          qos,
                  SitStreamFunc   func,
                  gpointer        func_data,
                  GDestroyNotify  func_data_destroy)
{
  SitStream *self;
  ProducerJob *job;

  g_return_val_if_fail (scope == NULL || SIT_IS_SCOPE (scope), NULL);
  g_return_val_if_fail (qos <= SIT_QOS_BACKGROUND, NULL);
  g_return_val_if_fail (func != NULL, NULL);

  self = sit_stream_new ();
  self->cancellable = link_new (&self->link, scope, NULL);

  job = g_new0 (ProducerJob, 1);
  job->job.qos = qos;
  job->job.run = producer_job_run;
  job->job.deliver = producer_job_deliver;
  job->stream = g_object_ref (self);
  job->func = func;
  job->func_data = func_data;
  job->func_data_destroy = func_data_destroy;

  schedule (&job->job);

  return self;
}

/**
 * sit_async_flush:
 *
 * Delivers every result that is already complete, without a time budget.
 * Must be called on the thread that runs the default main context.
 */
void
sit_async_flush (void)
{
  scheduler_init ();
  while (drain (-1))
    ;
  g_source_set_ready_time (drain_source, -1);
}
