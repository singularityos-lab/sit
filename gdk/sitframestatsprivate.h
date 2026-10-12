#pragma once

#include <glib.h>

G_BEGIN_DECLS

gboolean sit_frame_stats_enabled (void);

void     sit_frame_stats_record  (gint64 frame_start,
                                  gint64 layout_start,
                                  gint64 paint_start,
                                  gint64 frame_end);

G_END_DECLS
