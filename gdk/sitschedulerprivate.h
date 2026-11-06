#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define N_QOS 4

typedef enum {
  SIT_JOB_INTERACTIVE,
  SIT_JOB_USER,
  SIT_JOB_UTILITY,
  SIT_JOB_BACKGROUND
} SitJobClass;

typedef struct _SitJob SitJob;

struct _SitJob
{
  SitJob *next;
  SitJobClass qos;
  void (* run) (SitJob *job);
  void (* deliver) (SitJob *job);
};

void     sit_scheduler_init   (void);
void     sit_scheduler_submit (SitJob *job);
void     sit_scheduler_post   (SitJob *job);
gboolean sit_scheduler_drain  (gint64  budget);
void     sit_scheduler_settle (void);
gboolean sit_scheduler_enabled (void);
void     sit_scheduler_spawn  (SitJobClass  qos,
                               GFunc        func,
                               gpointer     object);

typedef void (* SitParallelFunc) (guint    index,
                                  gpointer data);

void     sit_scheduler_parallel (SitJobClass     qos,
                                 guint           n,
                                 SitParallelFunc func,
                                 gpointer        data);

G_END_DECLS
