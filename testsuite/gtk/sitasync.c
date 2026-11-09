#include <gtk/gtk.h>
#include <gtk/sit.h>
#include <string.h>
#include <glib/gstdio.h>

#ifdef __linux__
#include <sched.h>
#endif

static GThread *main_thread;

static void
spin_until (gboolean *flag)
{
  while (!*flag)
    g_main_context_iteration (NULL, TRUE);
}

static gpointer
square (GCancellable  *cancellable,
        gpointer       data,
        GError       **error)
{
  int *value = g_new (int, 1);

  g_assert_true (g_thread_self () != main_thread);
  *value = GPOINTER_TO_INT (data) * GPOINTER_TO_INT (data);
  return value;
}

static gpointer
fail (GCancellable  *cancellable,
      gpointer       data,
      GError       **error)
{
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "broken");
  return NULL;
}

typedef struct
{
  gboolean done;
  int value;
  GError *error;
  GThread *thread;
} Outcome;

static void
run_done (GObject      *source,
          GAsyncResult *result,
          gpointer      data)
{
  Outcome *outcome = data;
  int *value = sit_task_run_finish (result, &outcome->error);

  outcome->thread = g_thread_self ();
  if (value)
    outcome->value = *value;
  g_free (value);
  outcome->done = TRUE;
}

static void
test_run_result (void)
{
  Outcome outcome = { 0 };

  sit_task_run (NULL, SIT_QOS_USER, g_free, square, GINT_TO_POINTER (7), NULL, NULL, run_done, &outcome);
  g_assert_false (outcome.done);
  spin_until (&outcome.done);
  g_assert_no_error (outcome.error);
  g_assert_cmpint (outcome.value, ==, 49);
  g_assert_true (outcome.thread == main_thread);
}

static void
test_run_error (void)
{
  Outcome outcome = { 0 };

  sit_task_run (NULL, SIT_QOS_INTERACTIVE, NULL, fail, NULL, NULL, NULL, run_done, &outcome);
  spin_until (&outcome.done);
  g_assert_error (outcome.error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&outcome.error);
}

static gpointer
must_not_run (GCancellable  *cancellable,
              gpointer       data,
              GError       **error)
{
  g_atomic_int_inc ((int *) data);
  return NULL;
}

static void
test_scope_cancel (void)
{
  SitScope *parent = sit_scope_new (NULL);
  SitScope *child = sit_scope_new (parent);
  Outcome outcome = { 0 };
  int ran = 0;

  sit_scope_cancel (parent);
  g_assert_true (sit_scope_is_cancelled (child));
  sit_task_run (child, SIT_QOS_USER, NULL, must_not_run, &ran, NULL, NULL, run_done, &outcome);
  spin_until (&outcome.done);
  g_assert_error (outcome.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpint (g_atomic_int_get (&ran), ==, 0);
  g_clear_error (&outcome.error);
  g_object_unref (child);
  g_object_unref (parent);
}

static void
test_widget_scope (void)
{
  GtkWidget *window = gtk_window_new ();
  GtkWidget *label = gtk_label_new ("x");
  SitScope *scope;

  gtk_window_set_child (GTK_WINDOW (window), label);
  scope = g_object_ref (sit_widget_get_scope (label));
  g_assert_true (sit_widget_get_scope (label) == scope);
  g_assert_false (sit_scope_is_cancelled (scope));
  gtk_window_destroy (GTK_WINDOW (window));
  g_assert_true (sit_scope_is_cancelled (scope));
  g_object_unref (scope);
}

typedef struct
{
  int qos;
  GArray *order;
} Tag;

static int completed_count;

static gpointer
count_done (GCancellable  *cancellable,
            gpointer       data,
            GError       **error)
{
  g_atomic_int_inc (&completed_count);
  return NULL;
}

static void
record (GObject      *source,
        GAsyncResult *result,
        gpointer      data)
{
  Tag *tag = data;

  sit_task_run_finish (result, NULL);
  g_array_append_val (tag->order, tag->qos);
  g_free (tag);
}

static void
test_delivery_order (void)
{
  GArray *order = g_array_new (FALSE, FALSE, sizeof (int));
  SitQos classes[] = { SIT_QOS_UTILITY, SIT_QOS_BACKGROUND, SIT_QOS_USER, SIT_QOS_INTERACTIVE };
  const int per_class = 50;

  g_atomic_int_set (&completed_count, 0);
  for (int i = 0; i < per_class; i++)
    for (guint c = 0; c < G_N_ELEMENTS (classes); c++)
      {
        Tag *tag = g_new0 (Tag, 1);

        tag->qos = classes[c];
        tag->order = order;
        sit_task_run (NULL, classes[c], NULL, count_done, NULL, NULL, NULL, record, tag);
      }

  while (g_atomic_int_get (&completed_count) < per_class * 4)
    g_usleep (1000);
  g_usleep (20000);
  while (order->len < per_class * 4)
    g_main_context_iteration (NULL, TRUE);

  g_assert_cmpuint (order->len, ==, per_class * 4);
  for (guint i = 1; i < order->len; i++)
    g_assert_cmpint (g_array_index (order, int, i - 1), <=, g_array_index (order, int, i));
  g_array_unref (order);
}

static gpointer
background_policy (GCancellable  *cancellable,
                   gpointer       data,
                   GError       **error)
{
  int *policy = g_new (int, 1);

#ifdef SCHED_IDLE
  *policy = sched_getscheduler (0);
#else
  *policy = -1;
#endif
  return policy;
}

static void
test_background_idle (void)
{
  Outcome outcome = { 0 };

#ifndef SCHED_IDLE
  g_test_skip ("SCHED_IDLE is not available");
  return;
#else
  sit_task_run (NULL, SIT_QOS_BACKGROUND, g_free, background_policy, NULL, NULL, NULL, run_done, &outcome);
  spin_until (&outcome.done);
  g_assert_no_error (outcome.error);
  g_assert_cmpint (outcome.value, ==, SCHED_IDLE);
#endif
}

static void
wait_done (GObject      *source,
           GAsyncResult *result,
           gpointer      data)
{
  Outcome *outcome = data;
  int *value = sit_future_wait_finish (SIT_FUTURE (source), result, &outcome->error);

  if (value)
    outcome->value = *value;
  outcome->done = TRUE;
}

static void
test_future_waiters (void)
{
  SitFuture *future = sit_future_spawn (NULL, SIT_QOS_USER, g_free, square, GINT_TO_POINTER (9), NULL);
  Outcome first = { 0 }, second = { 0 }, late = { 0 };

  sit_future_wait (future, NULL, wait_done, &first);
  sit_future_wait (future, NULL, wait_done, &second);
  spin_until (&first.done);
  spin_until (&second.done);
  g_assert_cmpint (first.value, ==, 81);
  g_assert_cmpint (second.value, ==, 81);
  g_assert_true (sit_future_is_done (future));
  sit_future_wait (future, NULL, wait_done, &late);
  spin_until (&late.done);
  g_assert_cmpint (late.value, ==, 81);
  g_object_unref (future);
}

static gpointer
sleepy (GCancellable  *cancellable,
        gpointer       data,
        GError       **error)
{
  for (int i = 0; i < 200 && !g_cancellable_is_cancelled (cancellable); i++)
    g_usleep (5000);
  return NULL;
}

static void
test_future_cancel (void)
{
  SitFuture *future = sit_future_spawn (NULL, SIT_QOS_USER, NULL, sleepy, NULL, NULL);
  Outcome outcome = { 0 };
  gint64 start = g_get_monotonic_time ();

  sit_future_wait (future, NULL, wait_done, &outcome);
  sit_future_cancel (future);
  spin_until (&outcome.done);
  g_assert_error (outcome.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpint (g_get_monotonic_time () - start, <, 500 * G_TIME_SPAN_MILLISECOND);
  g_clear_error (&outcome.error);
  g_object_unref (future);
}

static void
test_wait_cancel (void)
{
  SitFuture *future = sit_future_spawn (NULL, SIT_QOS_USER, NULL, sleepy, NULL, NULL);
  GCancellable *cancellable = g_cancellable_new ();
  Outcome outcome = { 0 };

  sit_future_wait (future, cancellable, wait_done, &outcome);
  g_cancellable_cancel (cancellable);
  spin_until (&outcome.done);
  g_assert_error (outcome.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_false (sit_future_is_done (future));
  g_clear_error (&outcome.error);
  sit_future_cancel (future);
  g_object_unref (cancellable);
  g_object_unref (future);
}

static void
test_future_timeout (void)
{
  SitFuture *slow = sit_future_spawn (NULL, SIT_QOS_UTILITY, NULL, sleepy, NULL, NULL);
  SitFuture *limited = sit_future_timeout (slow, 30);
  SitFuture *quick_work = sit_future_spawn (NULL, SIT_QOS_USER, g_free, square, GINT_TO_POINTER (3), NULL);
  SitFuture *fast = sit_future_timeout (quick_work, 5000);
  Outcome timed = { 0 }, quick = { 0 };

  sit_future_wait (limited, NULL, wait_done, &timed);
  sit_future_wait (fast, NULL, wait_done, &quick);
  spin_until (&timed.done);
  spin_until (&quick.done);
  g_assert_error (timed.error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
  g_assert_no_error (quick.error);
  g_assert_cmpint (quick.value, ==, 9);
  g_clear_error (&timed.error);
  g_object_unref (limited);
  g_object_unref (slow);
  g_object_unref (fast);
  g_object_unref (quick_work);
}

typedef struct
{
  gboolean done;
  gboolean ok;
  int index;
  GError *error;
} Combined;

static void
all_done (GObject      *source,
          GAsyncResult *result,
          gpointer      data)
{
  Combined *combined = data;

  combined->ok = sit_future_all_finish (result, &combined->error);
  combined->done = TRUE;
}

static void
any_done (GObject      *source,
          GAsyncResult *result,
          gpointer      data)
{
  Combined *combined = data;

  combined->index = sit_future_any_finish (result, &combined->error);
  combined->done = TRUE;
}

static void
race_done (GObject      *source,
           GAsyncResult *result,
           gpointer      data)
{
  Combined *combined = data;

  combined->index = sit_future_race_finish (result, &combined->error);
  combined->done = TRUE;
}

static void
test_combinators (void)
{
  SitFuture *good[3];
  SitFuture *mixed[3];
  SitFuture *racers[2];
  Combined all = { 0 }, all_bad = { 0 }, any = { 0 }, race = { 0 };

  for (int i = 0; i < 3; i++)
    good[i] = sit_future_spawn (NULL, SIT_QOS_USER, g_free, square, GINT_TO_POINTER (i + 1), NULL);
  mixed[0] = sit_future_spawn (NULL, SIT_QOS_USER, NULL, fail, NULL, NULL);
  mixed[1] = sit_future_spawn (NULL, SIT_QOS_USER, NULL, fail, NULL, NULL);
  mixed[2] = sit_future_spawn (NULL, SIT_QOS_USER, g_free, square, GINT_TO_POINTER (4), NULL);
  racers[0] = sit_future_spawn (NULL, SIT_QOS_UTILITY, NULL, sleepy, NULL, NULL);
  racers[1] = sit_future_spawn (NULL, SIT_QOS_USER, g_free, square, GINT_TO_POINTER (2), NULL);

  sit_future_all (good, 3, NULL, all_done, &all);
  sit_future_all (mixed, 3, NULL, all_done, &all_bad);
  sit_future_any (mixed, 3, NULL, any_done, &any);
  sit_future_race (racers, 2, NULL, race_done, &race);
  spin_until (&all.done);
  spin_until (&all_bad.done);
  spin_until (&any.done);
  spin_until (&race.done);

  g_assert_no_error (all.error);
  g_assert_true (all.ok);
  g_assert_error (all_bad.error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_false (all_bad.ok);
  g_assert_no_error (any.error);
  g_assert_cmpint (any.index, ==, 2);
  g_assert_no_error (race.error);
  g_assert_cmpint (race.index, ==, 1);

  g_clear_error (&all_bad.error);
  sit_future_cancel (racers[0]);
  for (int i = 0; i < 3; i++)
    {
      g_object_unref (good[i]);
      g_object_unref (mixed[i]);
    }
  g_object_unref (racers[0]);
  g_object_unref (racers[1]);
}

#define STREAM_ITEMS 20000

static GObject *
item_new (int n)
{
  GObject *item = g_object_new (G_TYPE_OBJECT, NULL);

  g_object_set_data (item, "n", GINT_TO_POINTER (n));
  return item;
}

static int
item_number (gpointer item)
{
  return GPOINTER_TO_INT (g_object_get_data (item, "n"));
}

static gboolean
produce (SitStream     *stream,
         GCancellable  *cancellable,
         gpointer       data,
         GError       **error)
{
  for (int i = 0; i < STREAM_ITEMS; i++)
    sit_stream_push (stream, item_new (i + 1));
  if (data)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "producer failed");
      return FALSE;
    }
  return TRUE;
}

typedef struct
{
  gboolean done;
  int count;
  int batches;
  gboolean ordered;
  GError *error;
} Reader;

static void
read_batch (GObject      *source,
            GAsyncResult *result,
            gpointer      data)
{
  Reader *reader = data;
  GPtrArray *batch = sit_stream_next_finish (SIT_STREAM (source), result, &reader->error);

  if (!batch)
    {
      reader->done = TRUE;
      return;
    }

  reader->batches++;
  for (guint i = 0; i < batch->len; i++)
    {
      if (item_number (g_ptr_array_index (batch, i)) != reader->count + 1)
        reader->ordered = FALSE;
      reader->count++;
    }
  g_ptr_array_unref (batch);
  sit_stream_next (SIT_STREAM (source), NULL, read_batch, reader);
}

static void
test_stream_batches (void)
{
  SitStream *stream = sit_stream_spawn (NULL, SIT_QOS_USER, produce, NULL, NULL);
  Reader reader = { .ordered = TRUE };

  sit_stream_next (stream, NULL, read_batch, &reader);
  spin_until (&reader.done);
  g_assert_no_error (reader.error);
  g_assert_cmpint (reader.count, ==, STREAM_ITEMS);
  g_assert_true (reader.ordered);
  g_assert_cmpint (reader.batches, <, STREAM_ITEMS / 10);
  g_object_unref (stream);
}

static void
test_stream_error (void)
{
  SitStream *stream = sit_stream_spawn (NULL, SIT_QOS_USER, produce, GINT_TO_POINTER (1), NULL);
  Reader reader = { .ordered = TRUE };

  sit_stream_next (stream, NULL, read_batch, &reader);
  spin_until (&reader.done);
  g_assert_cmpint (reader.count, ==, STREAM_ITEMS);
  g_assert_error (reader.error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&reader.error);
  g_object_unref (stream);
}

static gpointer
push_many (gpointer data)
{
  for (int i = 0; i < 5000; i++)
    sit_stream_push (data, item_new (1));
  return NULL;
}

typedef struct
{
  gboolean done;
  int count;
  GError *error;
} Counter;

static void
count_batch (GObject      *source,
             GAsyncResult *result,
             gpointer      data)
{
  Counter *counter = data;
  GPtrArray *batch = sit_stream_next_finish (SIT_STREAM (source), result, &counter->error);

  if (!batch)
    {
      counter->done = TRUE;
      return;
    }
  counter->count += batch->len;
  g_ptr_array_unref (batch);
  sit_stream_next (SIT_STREAM (source), NULL, count_batch, counter);
}

static void
test_stream_threads (void)
{
  SitStream *stream = sit_stream_new ();
  Counter counter = { 0 };
  GThread *producers[4];

  sit_stream_next (stream, NULL, count_batch, &counter);
  for (int i = 0; i < 4; i++)
    producers[i] = g_thread_new ("producer", push_many, stream);
  for (int i = 0; i < 4; i++)
    g_thread_join (producers[i]);
  sit_stream_close (stream, NULL);
  spin_until (&counter.done);
  g_assert_no_error (counter.error);
  g_assert_cmpint (counter.count, ==, 20000);
  g_object_unref (stream);
}

typedef struct
{
  gboolean done;
  GThread *thread;
} OtherContext;

static void
other_done (GObject      *source,
            GAsyncResult *result,
            gpointer      data)
{
  OtherContext *other = data;

  g_free (sit_task_run_finish (result, NULL));
  other->thread = g_thread_self ();
  other->done = TRUE;
}

static gpointer
other_thread (gpointer data)
{
  OtherContext *other = data;
  GMainContext *context = g_main_context_new ();

  g_main_context_push_thread_default (context);
  sit_task_run (NULL, SIT_QOS_USER, g_free, square, GINT_TO_POINTER (2), NULL, NULL, other_done, other);
  while (!other->done)
    g_main_context_iteration (context, TRUE);
  g_main_context_pop_thread_default (context);
  g_main_context_unref (context);
  return NULL;
}

static void
test_thread_default_context (void)
{
  OtherContext other = { 0 };
  GThread *thread = g_thread_new ("caller", other_thread, &other);

  g_thread_join (thread);
  g_assert_true (other.done);
  g_assert_true (other.thread == thread);
}

static gpointer
nothing (GCancellable  *cancellable,
         gpointer       data,
         GError       **error)
{
  return NULL;
}

static void
count_delivery (GObject      *source,
                GAsyncResult *result,
                gpointer      data)
{
  sit_task_run_finish (result, NULL);
  (*(int *) data)++;
}

static void
test_throughput (void)
{
  const int total = 100000;
  int delivered = 0;
  gint64 start = g_get_monotonic_time ();
  double seconds;

  for (int i = 0; i < total; i++)
    sit_task_run (NULL, SIT_QOS_USER, NULL, nothing, NULL, NULL, NULL, count_delivery, &delivered);
  while (delivered < total)
    g_main_context_iteration (NULL, TRUE);
  seconds = (g_get_monotonic_time () - start) / (double) G_TIME_SPAN_SECOND;
  g_test_message ("%d tasks in %.3f s, %.0f per second", total, seconds, total / seconds);
  g_assert_cmpint (delivered, ==, total);
}

static gpointer
add_one (gpointer       value,
         GCancellable  *cancellable,
         gpointer       data,
         GError       **error)
{
  int *next = g_new (int, 1);

  g_assert_true (g_thread_self () != main_thread);
  *next = *(int *) value + 1;
  return next;
}

static gpointer
never_called (gpointer       value,
              GCancellable  *cancellable,
              gpointer       data,
              GError       **error)
{
  g_assert_not_reached ();
  return NULL;
}

static void
test_future_then (void)
{
  SitFuture *first = sit_future_spawn (NULL, SIT_QOS_USER, g_free, square, GINT_TO_POINTER (3), NULL);
  SitFuture *second = sit_future_then (first, SIT_QOS_UTILITY, g_free, add_one, NULL, NULL);
  SitFuture *third = sit_future_then (second, SIT_QOS_USER, g_free, add_one, NULL, NULL);
  SitFuture *broken = sit_future_spawn (NULL, SIT_QOS_USER, NULL, fail, NULL, NULL);
  SitFuture *after_broken = sit_future_then (broken, SIT_QOS_USER, g_free, never_called, NULL, NULL);
  SitFuture *slow = sit_future_spawn (NULL, SIT_QOS_UTILITY, NULL, sleepy, NULL, NULL);
  SitFuture *after_slow = sit_future_then (slow, SIT_QOS_USER, g_free, never_called, NULL, NULL);
  Outcome chained = { 0 }, failed = { 0 }, cancelled = { 0 }, source = { 0 };

  g_object_unref (first);
  g_object_unref (second);
  sit_future_wait (third, NULL, wait_done, &chained);
  sit_future_wait (after_broken, NULL, wait_done, &failed);
  sit_future_wait (after_slow, NULL, wait_done, &cancelled);
  sit_future_wait (slow, NULL, wait_done, &source);
  sit_future_cancel (after_slow);
  spin_until (&chained.done);
  spin_until (&failed.done);
  spin_until (&cancelled.done);
  spin_until (&source.done);

  g_assert_no_error (chained.error);
  g_assert_cmpint (chained.value, ==, 11);
  g_assert_error (failed.error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_error (cancelled.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_error (source.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&failed.error);
  g_clear_error (&cancelled.error);
  g_clear_error (&source.error);
  g_object_unref (third);
  g_object_unref (broken);
  g_object_unref (after_broken);
  g_object_unref (slow);
  g_object_unref (after_slow);
}

#define PRODUCERS 3
#define PER_PRODUCER 2000

static gpointer
send_numbers (GCancellable  *cancellable,
         gpointer       data,
         GError       **error)
{
  SitChannel *channel = data;
  int producer = g_atomic_int_add ((int *) g_object_get_data (G_OBJECT (channel), "counter"), 1);
  for (int i = 0; i < PER_PRODUCER; i++)
    {
      int *item = g_new (int, 1);

      *item = producer * PER_PRODUCER + i;
      if (!sit_channel_send (channel, item, cancellable))
        break;
      g_assert_cmpuint (sit_channel_get_length (channel), <=, 8);
    }
  return NULL;
}

typedef struct
{
  gboolean done;
  int received;
  int last[PRODUCERS];
  gboolean in_order;
} Drain;

static void receive_one (GObject *source, GAsyncResult *result, gpointer data);

static void
receive_one (GObject      *source,
             GAsyncResult *result,
             gpointer      data)
{
  Drain *drain = data;
  GError *error = NULL;
  int *item = sit_channel_receive_finish (SIT_CHANNEL (source), result, &error);

  g_assert_no_error (error);
  if (!item)
    {
      drain->done = TRUE;
      return;
    }
  if (*item % PER_PRODUCER != drain->last[*item / PER_PRODUCER] + 1)
    drain->in_order = FALSE;
  drain->last[*item / PER_PRODUCER] = *item % PER_PRODUCER;
  drain->received++;
  g_free (item);
  sit_channel_receive_async (SIT_CHANNEL (source), NULL, receive_one, drain);
}

static void
close_when_sent (GObject      *source,
                 GAsyncResult *result,
                 gpointer      data)
{
  g_assert_true (sit_future_all_finish (result, NULL));
  sit_channel_close (SIT_CHANNEL (data));
}

static gpointer
sum_items (GCancellable  *cancellable,
           gpointer       data,
           GError       **error)
{
  int *sum = g_new0 (int, 1);
  int *item;

  while ((item = sit_channel_receive (data, cancellable, error)))
    {
      *sum += *item;
      g_free (item);
    }
  return sum;
}

static void
test_channel (void)
{
  SitChannel *channel = sit_channel_new (8, g_free);
  SitChannel *pipe = sit_channel_new (0, g_free);
  SitFuture *producers[PRODUCERS];
  SitFuture *consumer;
  Drain drain = { 0 };
  Outcome summed = { 0 };
  int counter = 0;
  int *spare = g_new (int, 1);

  g_object_set_data (G_OBJECT (channel), "counter", &counter);
  drain.in_order = TRUE;
  for (int i = 0; i < PRODUCERS; i++)
    drain.last[i] = -1;
  for (int i = 0; i < PRODUCERS; i++)
    producers[i] = sit_future_spawn (NULL, SIT_QOS_USER, NULL, send_numbers, channel, NULL);
  sit_future_all (producers, PRODUCERS, NULL, close_when_sent, channel);
  sit_channel_receive_async (channel, NULL, receive_one, &drain);
  spin_until (&drain.done);
  g_assert_cmpint (drain.received, ==, PRODUCERS * PER_PRODUCER);
  g_assert_true (drain.in_order);
  *spare = 1;
  g_assert_false (sit_channel_try_send (channel, spare));

  consumer = sit_future_spawn (NULL, SIT_QOS_USER, g_free, sum_items, pipe, NULL);
  for (int i = 1; i <= 100; i++)
    {
      int *item = g_new (int, 1);

      *item = i;
      g_assert_true (sit_channel_try_send (pipe, item));
    }
  sit_channel_close (pipe);
  sit_future_wait (consumer, NULL, wait_done, &summed);
  spin_until (&summed.done);
  g_assert_no_error (summed.error);
  g_assert_cmpint (summed.value, ==, 5050);

  g_free (spare);
  for (int i = 0; i < PRODUCERS; i++)
    g_object_unref (producers[i]);
  g_object_unref (consumer);
  g_object_unref (channel);
  g_object_unref (pipe);
}

typedef struct
{
  GMutex lock;
  GHashTable *threads;
  int finished;
  int frames_seen;
} Seen;

static gpointer
nap (GCancellable  *cancellable,
     gpointer       data,
     GError       **error)
{
  Seen *seen = data;

  g_mutex_lock (&seen->lock);
  g_hash_table_add (seen->threads, g_thread_self ());
  g_mutex_unlock (&seen->lock);
  g_usleep (20000);
  g_atomic_int_inc (&seen->finished);
  g_main_context_wakeup (NULL);
  return NULL;
}

#define NAPS 8

static gpointer
spawn_from_worker (GCancellable  *cancellable,
                   gpointer       data,
                   GError       **error)
{
  for (int i = 0; i < NAPS; i++)
    {
      g_object_unref (sit_future_spawn (NULL, SIT_QOS_USER, NULL, nap, data, NULL));
    }
  return NULL;
}

static void
test_work_stealing (void)
{
  Seen seen = { 0 };
  SitFuture *outer;
  gint64 start;
  double elapsed;

  g_mutex_init (&seen.lock);
  seen.threads = g_hash_table_new (NULL, NULL);
  start = g_get_monotonic_time ();
  outer = sit_future_spawn (NULL, SIT_QOS_USER, NULL, spawn_from_worker, &seen, NULL);
  while (g_atomic_int_get (&seen.finished) < NAPS)
    g_main_context_iteration (NULL, TRUE);
  elapsed = (g_get_monotonic_time () - start) / (double) G_TIME_SPAN_MILLISECOND;
  g_test_message ("%d naps queued on one core ran on %u threads in %.1f ms",
                  NAPS, g_hash_table_size (seen.threads), elapsed);
  if (g_get_num_processors () > 1)
    {
      g_assert_cmpuint (g_hash_table_size (seen.threads), >, 1);
      g_assert_cmpfloat (elapsed, <, NAPS * 20 * 0.75);
    }
  g_object_unref (outer);
  g_mutex_clear (&seen.lock);
  g_hash_table_unref (seen.threads);
}

#define FRAME_TASKS 200

typedef struct
{
  GdkFrameClock *clock;
  GHashTable *frames;
  int delivered;
  int most_in_frame;
  gint64 frame;
  int in_frame;
} Frames;

static void
busy_delivery (GObject      *source,
               GAsyncResult *result,
               gpointer      data)
{
  Frames *frames = data;
  gint64 counter = gdk_frame_clock_get_frame_counter (frames->clock);
  gint64 until = g_get_monotonic_time () + 500;

  if (counter != frames->frame)
    {
      frames->frame = counter;
      frames->in_frame = 0;
    }
  frames->in_frame++;
  frames->most_in_frame = MAX (frames->most_in_frame, frames->in_frame);
  g_hash_table_add (frames->frames, GINT_TO_POINTER ((int) counter));
  frames->delivered++;
  while (g_get_monotonic_time () < until)
    ;
}

static gboolean
keep_ticking (GtkWidget     *widget,
              GdkFrameClock *clock,
              gpointer       data)
{
  return G_SOURCE_CONTINUE;
}

static void
test_frame_drain (void)
{
  GtkWidget *window = gtk_window_new ();
  Frames frames = { 0 };
  gint64 first;

  gtk_window_set_default_size (GTK_WINDOW (window), 200, 200);
  gtk_window_present (GTK_WINDOW (window));
  while (!gtk_widget_get_mapped (window))
    g_main_context_iteration (NULL, TRUE);
  frames.clock = gtk_widget_get_frame_clock (window);
  frames.frames = g_hash_table_new (NULL, NULL);
  frames.frame = -1;
  gtk_widget_add_tick_callback (window, keep_ticking, NULL, NULL);
  first = gdk_frame_clock_get_frame_counter (frames.clock);
  while (gdk_frame_clock_get_frame_counter (frames.clock) < first + 3)
    g_main_context_iteration (NULL, TRUE);

  for (int i = 0; i < FRAME_TASKS; i++)
    sit_task_run (NULL, SIT_QOS_USER, NULL, nothing, NULL, NULL, NULL, busy_delivery, &frames);
  while (frames.delivered < FRAME_TASKS)
    g_main_context_iteration (NULL, TRUE);

  g_test_message ("%d deliveries of 0.5 ms spread over %u frames, at most %d in one frame",
                  FRAME_TASKS, g_hash_table_size (frames.frames), frames.most_in_frame);
  g_assert_cmpuint (g_hash_table_size (frames.frames), >=, 15);
  g_assert_cmpint (frames.most_in_frame, <=, 10);

  g_hash_table_unref (frames.frames);
  gtk_window_destroy (GTK_WINDOW (window));
}

typedef struct
{
  gboolean done;
  GBytes *bytes;
  GError *error;
} Loaded;

static void
file_loaded (GObject      *source,
             GAsyncResult *result,
             gpointer      data)
{
  Loaded *loaded = data;

  loaded->bytes = sit_file_load_finish (G_FILE (source), result, &loaded->error);
  loaded->done = TRUE;
}

static GBytes *
load (GFile *file, SitQos qos, GCancellable *cancellable, GError **error)
{
  Loaded loaded = { 0 };

  sit_file_load_async (file, qos, cancellable, file_loaded, &loaded);
  spin_until (&loaded.done);
  if (loaded.error)
    g_propagate_error (error, loaded.error);
  return loaded.bytes;
}

static void
test_file_load (void)
{
  char *dir = g_dir_make_tmp ("sitio-XXXXXX", NULL);
  char *path = g_build_filename (dir, "big", NULL);
  gsize size = 9 * 1024 * 1024 + 17;
  guint8 *data = g_malloc (size);
  GFile *file, *missing, *proc;
  GCancellable *cancellable = g_cancellable_new ();
  GBytes *bytes;
  GError *error = NULL;

  for (gsize i = 0; i < size; i++)
    data[i] = (guint8) (i * 31 + 7);
  g_assert_true (g_file_set_contents (path, (char *) data, size, NULL));
  file = g_file_new_for_path (path);

  bytes = load (file, SIT_QOS_USER, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpmem (g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes), data, size);
  g_bytes_unref (bytes);

  bytes = load (file, SIT_QOS_BACKGROUND, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (g_bytes_get_size (bytes), ==, size);
  g_bytes_unref (bytes);

  missing = g_file_new_for_path ("/nonexistent/sit/file");
  bytes = load (missing, SIT_QOS_USER, NULL, &error);
  g_assert_null (bytes);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error (&error);

  proc = g_file_new_for_path ("/proc/self/status");
  bytes = load (proc, SIT_QOS_USER, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (strstr (g_bytes_get_data (bytes, NULL), "Name:"));
  g_bytes_unref (bytes);

  g_cancellable_cancel (cancellable);
  bytes = load (file, SIT_QOS_USER, cancellable, &error);
  g_assert_null (bytes);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&error);

  {
    const int total = 200;
    int done = 0;
    Loaded many[200];
    gint64 start = g_get_monotonic_time ();

    memset (many, 0, sizeof many);
    for (int i = 0; i < total; i++)
      sit_file_load_async (proc, SIT_QOS_USER, NULL, file_loaded, &many[i]);
    while (done < total)
      {
        g_main_context_iteration (NULL, TRUE);
        done = 0;
        for (int i = 0; i < total; i++)
          done += many[i].done;
      }
    for (int i = 0; i < total; i++)
      {
        g_assert_no_error (many[i].error);
        g_bytes_unref (many[i].bytes);
      }
    g_test_message ("%d concurrent loads in %.1f ms", total,
                    (g_get_monotonic_time () - start) / 1000.0);
  }

  g_unlink (path);
  g_rmdir (dir);
  g_free (path);
  g_free (dir);
  g_free (data);
  g_object_unref (file);
  g_object_unref (missing);
  g_object_unref (proc);
  g_object_unref (cancellable);
}

static void
test_parallel_filter (void)
{
  const guint total = 200000;
  GtkStringList *list = gtk_string_list_new (NULL);
  GtkStringFilter *filter;
  GtkFilterListModel *model;
  guint expected = 0;
  gint64 start;

  for (guint i = 0; i < total; i++)
    {
      char *s = g_strdup_printf ("Élément Numéro %u %s", i, i % 7 == 0 ? "SPÉCIAL" : "commun");

      gtk_string_list_take (list, s);
      if (i % 7 == 0)
        expected++;
    }
  filter = gtk_string_filter_new (gtk_property_expression_new (GTK_TYPE_STRING_OBJECT, NULL, "string"));
  model = gtk_filter_list_model_new (G_LIST_MODEL (list), GTK_FILTER (filter));

  start = g_get_monotonic_time ();
  gtk_string_filter_set_search (filter, "spécial");
  g_test_message ("filtered %u items in %.1f ms", total, (g_get_monotonic_time () - start) / 1000.0);
  g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (model)), ==, expected);
  for (guint i = 0; i < 50; i++)
    {
      GtkStringObject *item = g_list_model_get_item (G_LIST_MODEL (model), i);

      g_assert_nonnull (strstr (gtk_string_object_get_string (item), "SPÉCIAL"));
      g_object_unref (item);
    }

  gtk_string_filter_set_match_mode (filter, GTK_STRING_FILTER_MATCH_MODE_PREFIX);
  gtk_string_filter_set_search (filter, "élément numéro 1999");
  g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (model)), ==, 111);

  gtk_string_filter_set_ignore_case (filter, FALSE);
  g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (model)), ==, 0);

  g_object_unref (model);
}

static void
count_invalidations (GdkPaintable *paintable,
                     int          *count)
{
  (*count)++;
}

static void
test_deferred_placeholder (void)
{
  const int size = 300;
  guchar *pixels = g_malloc (size * size * 4);
  guchar *back = g_malloc (size * size * 4);
  GBytes *raw, *png;
  GdkTexture *source, *texture;
  GtkSnapshot *snapshot;
  GskRenderNode *node;
  int invalidated = 0;

  for (int i = 0; i < size * size * 4; i++)
    pixels[i] = g_random_int_range (0, 256);
  for (int i = 3; i < size * size * 4; i += 4)
    pixels[i] = 255;
  raw = g_bytes_new_take (pixels, size * size * 4);
  source = gdk_memory_texture_new (size, size, GDK_MEMORY_R8G8B8A8, raw, size * 4);
  png = gdk_texture_save_to_png_bytes (source);
  g_assert_cmpuint (g_bytes_get_size (png), >=, 64 * 1024);

  g_assert_true (g_main_context_acquire (NULL));
  texture = gdk_texture_new_from_bytes (png, NULL);
  g_main_context_release (NULL);
  g_assert_cmpstr (G_OBJECT_TYPE_NAME (texture), ==, "GdkDeferredTexture");
  g_assert_cmpint (gdk_paintable_get_intrinsic_width (GDK_PAINTABLE (texture)), ==, size);
  g_assert_cmpint (gdk_paintable_get_intrinsic_height (GDK_PAINTABLE (texture)), ==, size);
  g_assert_cmpint (gdk_paintable_get_flags (GDK_PAINTABLE (texture)), ==, GDK_PAINTABLE_STATIC_SIZE);
  g_signal_connect (texture, "invalidate-contents", G_CALLBACK (count_invalidations), &invalidated);

  while (invalidated == 0)
    g_main_context_iteration (NULL, TRUE);
  g_assert_cmpint (invalidated, ==, 1);
  g_assert_cmpint (gdk_paintable_get_flags (GDK_PAINTABLE (texture)), ==,
                   GDK_PAINTABLE_STATIC_SIZE | GDK_PAINTABLE_STATIC_CONTENTS);

  snapshot = gtk_snapshot_new ();
  gdk_paintable_snapshot (GDK_PAINTABLE (texture), snapshot, size, size);
  node = gtk_snapshot_free_to_node (snapshot);
  g_assert_nonnull (node);
  gsk_render_node_unref (node);

  gdk_texture_download (texture, back, size * 4);
  gdk_texture_download (source, pixels = g_malloc (size * size * 4), size * 4);
  g_assert_cmpmem (back, size * size * 4, pixels, size * size * 4);

  g_free (pixels);
  g_free (back);
  g_bytes_unref (raw);
  g_bytes_unref (png);
  g_object_unref (source);
  g_object_unref (texture);
}

int
main (int argc, char *argv[])
{
  gtk_test_init (&argc, &argv, NULL);
  main_thread = g_thread_self ();

  g_test_add_func ("/sit/async/run-result", test_run_result);
  g_test_add_func ("/sit/async/run-error", test_run_error);
  g_test_add_func ("/sit/async/scope-cancel", test_scope_cancel);
  g_test_add_func ("/sit/async/widget-scope", test_widget_scope);
  g_test_add_func ("/sit/async/delivery-order", test_delivery_order);
  g_test_add_func ("/sit/async/background-idle", test_background_idle);
  g_test_add_func ("/sit/async/future-waiters", test_future_waiters);
  g_test_add_func ("/sit/async/future-cancel", test_future_cancel);
  g_test_add_func ("/sit/async/wait-cancel", test_wait_cancel);
  g_test_add_func ("/sit/async/future-timeout", test_future_timeout);
  g_test_add_func ("/sit/async/combinators", test_combinators);
  g_test_add_func ("/sit/async/stream-batches", test_stream_batches);
  g_test_add_func ("/sit/async/stream-error", test_stream_error);
  g_test_add_func ("/sit/async/stream-threads", test_stream_threads);
  g_test_add_func ("/sit/async/thread-default-context", test_thread_default_context);
  g_test_add_func ("/sit/async/future-then", test_future_then);
  g_test_add_func ("/sit/async/channel", test_channel);
  g_test_add_func ("/sit/async/work-stealing", test_work_stealing);
  g_test_add_func ("/sit/async/frame-drain", test_frame_drain);
  g_test_add_func ("/sit/async/file-load", test_file_load);
  g_test_add_func ("/sit/async/parallel-filter", test_parallel_filter);
  g_test_add_func ("/sit/async/deferred-placeholder", test_deferred_placeholder);
  g_test_add_func ("/sit/async/throughput", test_throughput);

  return g_test_run ();
}
