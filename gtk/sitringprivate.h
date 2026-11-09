#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum {
  SIT_RING_LOADED,
  SIT_RING_FAILED,
  SIT_RING_UNSUITABLE
} SitRingOutcome;

typedef void (* SitRingDone) (SitRingOutcome  outcome,
                              GBytes         *bytes,
                              int             error,
                              gpointer        user_data);

typedef void (* SitRingReadDone) (gssize   result,
                                  gpointer user_data);

gboolean sit_ring_available (void);
gboolean sit_ring_read      (int              fd,
                             gpointer         buffer,
                             gsize            count,
                             gboolean         idle,
                             SitRingReadDone  done,
                             gpointer         user_data);
gboolean sit_ring_load      (const char   *path,
                             gboolean      idle,
                             GCancellable *cancellable,
                             SitRingDone   done,
                             gpointer      user_data);

G_END_DECLS
