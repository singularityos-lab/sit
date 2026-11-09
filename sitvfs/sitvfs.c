#include "config.h"

#include "gtk/sitringprivate.h"

#include <gio/gio.h>
#include <gio/gfiledescriptorbased.h>
#include <errno.h>

#define SIT_TYPE_VFS (sit_vfs_get_type ())
G_DECLARE_FINAL_TYPE (SitVfs, sit_vfs, SIT, VFS, GVfs)

struct _SitVfs
{
  GVfs parent_instance;
};

G_DEFINE_DYNAMIC_TYPE (SitVfs, sit_vfs, G_TYPE_VFS)

static gboolean
sit_vfs_is_active (GVfs *vfs)
{
  return FALSE;
}

static GFile *
sit_vfs_get_file_for_path (GVfs       *vfs,
                           const char *path)
{
  return g_vfs_get_file_for_path (g_vfs_get_local (), path);
}

static GFile *
sit_vfs_get_file_for_uri (GVfs       *vfs,
                          const char *uri)
{
  return g_vfs_get_file_for_uri (g_vfs_get_local (), uri);
}

static GFile *
sit_vfs_parse_name (GVfs       *vfs,
                    const char *parse_name)
{
  return g_vfs_parse_name (g_vfs_get_local (), parse_name);
}

static const char * const *
sit_vfs_get_supported_uri_schemes (GVfs *vfs)
{
  return g_vfs_get_supported_uri_schemes (g_vfs_get_local ());
}

static void
sit_vfs_class_init (SitVfsClass *klass)
{
  GVfsClass *vfs_class = G_VFS_CLASS (klass);

  vfs_class->is_active = sit_vfs_is_active;
  vfs_class->get_file_for_path = sit_vfs_get_file_for_path;
  vfs_class->get_file_for_uri = sit_vfs_get_file_for_uri;
  vfs_class->parse_name = sit_vfs_parse_name;
  vfs_class->get_supported_uri_schemes = sit_vfs_get_supported_uri_schemes;
}

static void
sit_vfs_class_finalize (SitVfsClass *klass)
{
}

static void
sit_vfs_init (SitVfs *self)
{
}

static void (* local_read_async) (GInputStream        *stream,
                                  void                *buffer,
                                  gsize                count,
                                  int                  io_priority,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data);
static gssize (* local_read_finish) (GInputStream  *stream,
                                     GAsyncResult  *result,
                                     GError       **error);

static void
ring_read_done (gssize   result,
                gpointer data)
{
  GTask *task = data;

  if (result < 0)
    g_task_return_new_error (task, G_IO_ERROR, g_io_error_from_errno (-result),
                             "%s", g_strerror (-result));
  else
    g_task_return_int (task, result);
  g_object_unref (task);
}

static void
sit_read_async (GInputStream        *stream,
                void                *buffer,
                gsize                count,
                int                  io_priority,
                GCancellable        *cancellable,
                GAsyncReadyCallback  callback,
                gpointer             user_data)
{
  int fd = g_file_descriptor_based_get_fd (G_FILE_DESCRIPTOR_BASED (stream));
  GTask *task;

  if (fd < 0 || !sit_ring_available () || g_cancellable_is_cancelled (cancellable))
    {
      local_read_async (stream, buffer, count, io_priority, cancellable, callback, user_data);
      return;
    }

  task = g_task_new (stream, cancellable, callback, user_data);
  g_task_set_source_tag (task, sit_read_async);
  g_task_set_priority (task, io_priority);
  if (sit_ring_read (fd, buffer, count, io_priority >= G_PRIORITY_LOW, ring_read_done, task))
    return;

  g_object_unref (task);
  local_read_async (stream, buffer, count, io_priority, cancellable, callback, user_data);
}

static gssize
sit_read_finish (GInputStream  *stream,
                 GAsyncResult  *result,
                 GError       **error)
{
  if (g_task_is_valid (result, stream) &&
      g_task_get_source_tag (G_TASK (result)) == sit_read_async)
    return g_task_propagate_int (G_TASK (result), error);

  return local_read_finish (stream, result, error);
}

static gboolean
hook_local_streams (void)
{
  GFile *file = g_vfs_get_file_for_path (g_vfs_get_local (), "/dev/null");
  GFileInputStream *stream = g_file_read (file, NULL, NULL);
  GInputStreamClass *klass;

  g_object_unref (file);
  if (stream == NULL)
    return FALSE;
  if (!G_IS_FILE_DESCRIPTOR_BASED (stream))
    {
      g_object_unref (stream);
      return FALSE;
    }

  klass = g_type_class_ref (G_OBJECT_TYPE (stream));
  g_object_unref (stream);
  if (klass->read_async == sit_read_async)
    return TRUE;

  local_read_async = klass->read_async;
  local_read_finish = klass->read_finish;
  klass->read_async = sit_read_async;
  klass->read_finish = sit_read_finish;

  return TRUE;
}

void
g_io_module_load (GIOModule *module)
{
  g_type_module_use (G_TYPE_MODULE (module));
  sit_vfs_register_type (G_TYPE_MODULE (module));
  g_io_extension_point_implement (G_VFS_EXTENSION_POINT_NAME, SIT_TYPE_VFS, "sit-vfs", -1000);

  if (g_strcmp0 (g_getenv ("SIT_ASYNC"), "off") == 0 || !sit_ring_available ())
    return;

  if (hook_local_streams ())
    g_debug ("sit-vfs: local file reads go through io_uring");
}

void
g_io_module_unload (GIOModule *module)
{
}

char **
g_io_module_query (void)
{
  return g_strsplit (G_VFS_EXTENSION_POINT_NAME, "!", -1);
}
