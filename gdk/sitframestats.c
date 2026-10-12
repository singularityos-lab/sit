#include "config.h"

#include "sitframestatsprivate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct
{
  double frame;
  double layout;
  double paint;
} SitFrameSample;

static int enabled = -1;
static char *output_path;
static gint64 warmup_until;
static gint64 first_frame;
static gint64 last_frame;
static GArray *samples;
static GMutex lock;

static int
compare_double (const void *a,
                const void *b)
{
  double x = *(const double *) a;
  double y = *(const double *) b;

  return x < y ? -1 : x > y;
}

static double
percentile (double *values,
            guint   n,
            double  p)
{
  guint index;

  if (n == 0)
    return 0;

  index = (guint) (p * (n - 1) + 0.5);
  return values[MIN (index, n - 1)];
}

static void
write_series (FILE       *f,
              const char *name,
              double     *values,
              guint       n)
{
  double sum = 0;

  for (guint i = 0; i < n; i++)
    sum += values[i];

  qsort (values, n, sizeof (double), compare_double);
  fprintf (f, "\"%s_mean_ms\":%.3f,\"%s_p50_ms\":%.3f,\"%s_p95_ms\":%.3f,\"%s_p99_ms\":%.3f,\"%s_max_ms\":%.3f,",
           name, n ? sum / n : 0,
           name, percentile (values, n, 0.50),
           name, percentile (values, n, 0.95),
           name, percentile (values, n, 0.99),
           name, n ? values[n - 1] : 0);
}

static void
sit_frame_stats_write (void)
{
  FILE *f;
  guint n;
  double *frame, *layout, *paint;
  double seconds;

  g_mutex_lock (&lock);

  n = samples ? samples->len : 0;
  f = fopen (output_path, "w");
  if (f == NULL)
    {
      g_mutex_unlock (&lock);
      return;
    }

  frame = g_new (double, MAX (n, 1));
  layout = g_new (double, MAX (n, 1));
  paint = g_new (double, MAX (n, 1));
  for (guint i = 0; i < n; i++)
    {
      SitFrameSample *s = &g_array_index (samples, SitFrameSample, i);
      frame[i] = s->frame;
      layout[i] = s->layout;
      paint[i] = s->paint;
    }

  seconds = n > 1 ? (last_frame - first_frame) / (double) G_USEC_PER_SEC : 0;
  fprintf (f, "{\"pid\":%d,\"frames\":%u,\"seconds\":%.3f,\"fps\":%.2f,",
           (int) getpid (), n, seconds, seconds > 0 ? (n - 1) / seconds : 0);
  write_series (f, "frame", frame, n);
  write_series (f, "layout", layout, n);
  write_series (f, "paint", paint, n);
  fprintf (f, "\"version\":1}\n");
  fclose (f);

  g_free (frame);
  g_free (layout);
  g_free (paint);
  g_mutex_unlock (&lock);
}

gboolean
sit_frame_stats_enabled (void)
{
  if (G_LIKELY (enabled != -1))
    return enabled;

  const char *path = g_getenv ("SIT_FRAME_STATS");
  const char *warmup = g_getenv ("SIT_FRAME_STATS_WARMUP");

  enabled = path != NULL && *path != '\0';
  if (!enabled)
    return FALSE;

  if (strstr (path, "%p"))
    {
      char *pid = g_strdup_printf ("%d", (int) getpid ());
      GString *s = g_string_new (path);
      g_string_replace (s, "%p", pid, 0);
      output_path = g_string_free (s, FALSE);
      g_free (pid);
    }
  else
    output_path = g_strdup (path);

  warmup_until = g_get_monotonic_time () + (gint64) ((warmup ? g_ascii_strtod (warmup, NULL) : 0) * G_USEC_PER_SEC);
  samples = g_array_new (FALSE, FALSE, sizeof (SitFrameSample));
  atexit (sit_frame_stats_write);

  return TRUE;
}

void
sit_frame_stats_record (gint64 frame_start,
                        gint64 layout_start,
                        gint64 paint_start,
                        gint64 frame_end)
{
  SitFrameSample sample;

  if (frame_start < warmup_until)
    return;

  sample.frame = (frame_end - frame_start) / 1000.0;
  sample.layout = layout_start && paint_start ? (paint_start - layout_start) / 1000.0 : 0;
  sample.paint = paint_start ? (frame_end - paint_start) / 1000.0 : 0;

  g_mutex_lock (&lock);
  if (first_frame == 0)
    first_frame = frame_start;
  last_frame = frame_start;
  g_array_append_val (samples, sample);
  g_mutex_unlock (&lock);
}
