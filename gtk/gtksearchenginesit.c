#include "config.h"

#include "gtksearchenginesitprivate.h"
#include "gtkquery.h"
#include "gdk/sitschedulerprivate.h"

#include <string.h>

#define WALK_ATTRIBUTES "standard::name,standard::display-name,standard::type," \
                        "standard::is-hidden,standard::is-backup,standard::is-symlink"
#define HIT_ATTRIBUTES "standard::name,standard::type,standard::display-name," \
                       "standard::is-hidden,standard::is-backup,standard::size," \
                       "standard::content-type,standard::fast-content-type,time::modified,time::access," \
                       "access::can-rename,access::can-delete,access::can-trash," \
                       "standard::target-uri"
#define MAX_DIRECTORIES 20000
#define MAX_HITS 2000
#define FIRST_BATCH 16
#define BATCH 256
#define FLUSH_US (8 * G_TIME_SPAN_MILLISECOND)

typedef struct _SearchJob
{
  SitJob job;
  GtkSearchEngineSit *engine;
  GFile *root;
  char **words;
  GCancellable *cancellable;
  GMutex lock;
  GList *ready;
  guint n_hits;
  gboolean finished;
  gboolean posted;
  gboolean abandoned;
} SearchJob;

struct _GtkSearchEngineSit
{
  GtkSearchEngine parent;

  GtkQuery *query;
  SearchJob *job;
};

G_DEFINE_FINAL_TYPE (GtkSearchEngineSit, gtk_search_engine_sit, GTK_TYPE_SEARCH_ENGINE)

static char *
prepare (const char *string)
{
  char *normalized = g_utf8_normalize (string, -1, G_NORMALIZE_NFD);
  char *result = g_utf8_strdown (normalized, -1);

  g_free (normalized);
  return result;
}

static gboolean
matches (char       **words,
         const char  *name)
{
  char *prepared = prepare (name);
  gboolean found = TRUE;

  for (guint i = 0; words[i]; i++)
    {
      if (words[i][0] != '\0' && strstr (prepared, words[i]) == NULL)
        {
          found = FALSE;
          break;
        }
    }

  g_free (prepared);
  return found;
}

static void
search_job_free (SearchJob *job)
{
  g_object_unref (job->root);
  g_strfreev (job->words);
  g_object_unref (job->cancellable);
  g_list_free_full (job->ready, (GDestroyNotify) _gtk_search_hit_free);
  g_mutex_clear (&job->lock);
  g_free (job);
}

static void
search_job_flush (SearchJob *job,
                  GList     *hits,
                  gboolean   finished)
{
  gboolean post;

  g_mutex_lock (&job->lock);
  job->ready = g_list_concat (job->ready, hits);
  if (finished)
    job->finished = TRUE;
  post = !job->posted;
  job->posted = TRUE;
  g_mutex_unlock (&job->lock);

  if (post)
    sit_scheduler_post (&job->job);
}

static void
search_job_run (SitJob *sit_job)
{
  SearchJob *job = (SearchJob *) sit_job;
  GQueue pending = G_QUEUE_INIT;
  GList *batch = NULL;
  guint batch_len = 0, limit = FIRST_BATCH, visited = 0, hits = 0;
  gint64 last_flush = g_get_monotonic_time ();

  g_queue_push_tail (&pending, g_object_ref (job->root));

  while (!g_queue_is_empty (&pending) && visited < MAX_DIRECTORIES && hits < MAX_HITS &&
         !g_cancellable_is_cancelled (job->cancellable))
    {
      GFile *dir = g_queue_pop_head (&pending);
      GFileEnumerator *enumerator;
      GFileInfo *info;

      visited++;
      enumerator = g_file_enumerate_children (dir, WALK_ATTRIBUTES, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                              job->cancellable, NULL);
      if (enumerator == NULL)
        {
          g_object_unref (dir);
          continue;
        }

      while (hits < MAX_HITS &&
             (info = g_file_enumerator_next_file (enumerator, job->cancellable, NULL)) != NULL)
        {
          const char *name = g_file_info_get_display_name (info);
          gboolean hidden = g_file_info_get_is_hidden (info) || g_file_info_get_is_backup (info);
          GFile *child;

          if (hidden || name == NULL)
            {
              g_object_unref (info);
              continue;
            }

          child = g_file_enumerator_get_child (enumerator, info);

          if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY &&
              !g_file_info_get_is_symlink (info))
            g_queue_push_tail (&pending, g_object_ref (child));

          if (matches (job->words, name))
            {
              GtkSearchHit *hit = g_new0 (GtkSearchHit, 1);

              hit->file = g_object_ref (child);
              hit->info = g_file_query_info (child, HIT_ATTRIBUTES, G_FILE_QUERY_INFO_NONE,
                                             job->cancellable, NULL);
              batch = g_list_prepend (batch, hit);
              batch_len++;
              hits++;
            }

          g_object_unref (child);
          g_object_unref (info);

          if (batch_len > 0 &&
              (batch_len >= limit || g_get_monotonic_time () - last_flush >= FLUSH_US))
            {
              search_job_flush (job, g_list_reverse (batch), FALSE);
              batch = NULL;
              batch_len = 0;
              limit = BATCH;
              last_flush = g_get_monotonic_time ();
            }
        }

      g_object_unref (enumerator);
      g_object_unref (dir);
    }

  g_queue_clear_full (&pending, g_object_unref);
  search_job_flush (job, g_list_reverse (batch), TRUE);
}

static void
search_job_deliver (SitJob *sit_job)
{
  SearchJob *job = (SearchJob *) sit_job;
  GtkSearchEngineSit *self = job->engine;
  GList *hits;
  gboolean finished;

  g_mutex_lock (&job->lock);
  hits = job->ready;
  job->ready = NULL;
  finished = job->finished;
  job->posted = FALSE;
  g_mutex_unlock (&job->lock);

  if (job->abandoned)
    {
      g_list_free_full (hits, (GDestroyNotify) _gtk_search_hit_free);
      if (finished)
        search_job_free (job);
      return;
    }

  if (hits)
    {
      job->n_hits += g_list_length (hits);
      _gtk_search_engine_hits_added (GTK_SEARCH_ENGINE (self), hits);
      g_list_free_full (hits, (GDestroyNotify) _gtk_search_hit_free);
    }

  if (finished)
    {
      gboolean got_results = job->n_hits > 0;

      self->job = NULL;
      search_job_free (job);
      _gtk_search_engine_finished (GTK_SEARCH_ENGINE (self), got_results);
    }
}

static void
gtk_search_engine_sit_stop (GtkSearchEngine *engine)
{
  GtkSearchEngineSit *self = GTK_SEARCH_ENGINE_SIT (engine);

  if (self->job == NULL)
    return;

  self->job->abandoned = TRUE;
  g_cancellable_cancel (self->job->cancellable);
  self->job = NULL;
}

static void
finish_empty (gpointer data)
{
  _gtk_search_engine_finished (data, FALSE);
  g_object_unref (data);
}

static void
gtk_search_engine_sit_start (GtkSearchEngine *engine)
{
  GtkSearchEngineSit *self = GTK_SEARCH_ENGINE_SIT (engine);
  GFile *location;
  const char *text;
  SearchJob *job;
  char *prepared;

  gtk_search_engine_sit_stop (engine);

  text = self->query ? gtk_query_get_text (self->query) : NULL;
  location = self->query ? gtk_query_get_location (self->query) : NULL;
  if (text == NULL || text[0] == '\0' || location == NULL || !g_file_is_native (location))
    {
      g_idle_add_once (finish_empty, g_object_ref (engine));
      return;
    }

  job = g_new0 (SearchJob, 1);
  job->job.qos = SIT_JOB_USER;
  job->job.run = search_job_run;
  job->job.deliver = search_job_deliver;
  job->engine = self;
  job->root = g_object_ref (location);
  prepared = prepare (text);
  job->words = g_strsplit (prepared, " ", -1);
  g_free (prepared);
  job->cancellable = g_cancellable_new ();
  g_mutex_init (&job->lock);

  self->job = job;
  sit_scheduler_submit (&job->job);
}

static void
gtk_search_engine_sit_set_query (GtkSearchEngine *engine,
                                 GtkQuery        *query)
{
  GtkSearchEngineSit *self = GTK_SEARCH_ENGINE_SIT (engine);

  g_set_object (&self->query, query);
}

static void
gtk_search_engine_sit_dispose (GObject *object)
{
  GtkSearchEngineSit *self = GTK_SEARCH_ENGINE_SIT (object);

  gtk_search_engine_sit_stop (GTK_SEARCH_ENGINE (self));
  g_clear_object (&self->query);

  G_OBJECT_CLASS (gtk_search_engine_sit_parent_class)->dispose (object);
}

static void
gtk_search_engine_sit_class_init (GtkSearchEngineSitClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);
  GtkSearchEngineClass *engine_class = GTK_SEARCH_ENGINE_CLASS (class);

  object_class->dispose = gtk_search_engine_sit_dispose;

  engine_class->set_query = gtk_search_engine_sit_set_query;
  engine_class->start = gtk_search_engine_sit_start;
  engine_class->stop = gtk_search_engine_sit_stop;
}

static void
gtk_search_engine_sit_init (GtkSearchEngineSit *self)
{
}

GtkSearchEngine *
gtk_search_engine_sit_new (void)
{
  if (!sit_scheduler_enabled ())
    return NULL;

  return g_object_new (GTK_TYPE_SEARCH_ENGINE_SIT, NULL);
}
