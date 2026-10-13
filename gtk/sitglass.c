#include "config.h"

#include "sit.h"
#include "gtkwidgetprivate.h"
#include "gsk/gskdisplacementnodeprivate.h"
#include "gsk/gpu/gskglrenderer.h"
#include "gsk/gpu/gskvulkanrenderer.h"
#include "gdk/gdkdisplayprivate.h"
#include "gdk/gdkvulkancontextprivate.h"

#include <epoxy/gl.h>

#include <math.h>
#include <string.h>

#define MAP_SCALE 2.0f
#define MAP_MAX_SIZE 384
#define MAP_CACHE_SIZE 48

typedef struct {
  int width;
  int height;
  int radius;
  int bezel;
  int refraction;
  int zoom;
} MapKey;

typedef struct {
  MapKey key;
  float max;
  GdkTexture *texture;
} MapEntry;

static GQuark quark_sit_glass;
static GQuark quark_software_gl;
static GHashTable *map_cache;

static guint
map_key_hash (gconstpointer data)
{
  const MapKey *key = data;
  guint hash = 17;

  hash = hash * 31 + key->width;
  hash = hash * 31 + key->height;
  hash = hash * 31 + key->radius;
  hash = hash * 31 + key->bezel;
  hash = hash * 31 + key->refraction;
  hash = hash * 31 + key->zoom;
  return hash;
}

static gboolean
map_key_equal (gconstpointer a,
               gconstpointer b)
{
  return memcmp (a, b, sizeof (MapKey)) == 0;
}

static void
map_entry_free (gpointer data)
{
  MapEntry *entry = data;

  g_object_unref (entry->texture);
  g_free (entry);
}

static float
rounded_radius (const GskRoundedRect *shape)
{
  float radius = shape->corner[GSK_CORNER_TOP_LEFT].width;

  for (int i = 1; i < 4; i++)
    radius = MIN (radius, shape->corner[i].width);
  for (int i = 0; i < 4; i++)
    radius = MIN (radius, shape->corner[i].height);

  return CLAMP (radius, 0, MIN (shape->bounds.size.width, shape->bounds.size.height) / 2);
}

static void
displacement_at (float  x,
                 float  y,
                 float  half_width,
                 float  half_height,
                 float  radius,
                 float  bezel,
                 float  refraction,
                 float  shrink,
                 float *dx,
                 float *dy)
{
  float qx = fabsf (x) - (half_width - radius);
  float qy = fabsf (y) - (half_height - radius);
  float outside = hypotf (MAX (qx, 0), MAX (qy, 0));
  float inside = MIN (MAX (qx, qy), 0);
  float depth = radius - outside - inside;
  float nx, ny;

  if (qx > 0 && qy > 0 && outside > 0)
    {
      nx = qx / outside;
      ny = qy / outside;
    }
  else if (qx > qy)
    {
      nx = 1;
      ny = 0;
    }
  else
    {
      nx = 0;
      ny = 1;
    }
  nx = copysignf (nx, x);
  ny = copysignf (ny, y);

  *dx = - x * shrink;
  *dy = - y * shrink;

  if (bezel > 0 && depth < bezel)
    {
      float k = 1 - MAX (depth, 0) / bezel;
      float amount = refraction * (1 - sqrtf (MAX (0, 1 - k * k)));

      *dx -= nx * amount;
      *dy -= ny * amount;
    }
}

static MapEntry *
create_map (const MapKey *key,
            float         width,
            float         height,
            float         radius,
            float         bezel,
            float         refraction,
            float         zoom)
{
  int map_width = MIN (MAP_MAX_SIZE, MAX (2, (int) ceilf (width * MAP_SCALE)));
  int map_height = MIN (MAP_MAX_SIZE, MAX (2, (int) ceilf (height * MAP_SCALE)));
  float shrink = zoom > 0 ? 1 - 1 / zoom : 0;
  float max = MAX (1, refraction + fabsf (shrink) * MAX (width, height) / 2);
  gsize stride = map_width * 3 * sizeof (guint16);
  guint16 *pixels = g_malloc (stride * map_height);
  GdkMemoryTextureBuilder *builder;
  GBytes *bytes;
  MapEntry *entry;

  for (int j = 0; j < map_height; j++)
    {
      float y = ((j + 0.5f) / map_height - 0.5f) * height;

      for (int i = 0; i < map_width; i++)
        {
          float x = ((i + 0.5f) / map_width - 0.5f) * width;
          guint16 *pixel = pixels + j * map_width * 3 + i * 3;
          float dx, dy;

          displacement_at (x, y, width / 2, height / 2, radius, bezel, refraction, shrink, &dx, &dy);
          pixel[0] = (guint16) CLAMP (lrintf ((0.5f + dx / (2 * max)) * 65535), 0, 65535);
          pixel[1] = (guint16) CLAMP (lrintf ((0.5f + dy / (2 * max)) * 65535), 0, 65535);
          pixel[2] = 0;
        }
    }

  bytes = g_bytes_new_take (pixels, stride * map_height);
  builder = gdk_memory_texture_builder_new ();
  gdk_memory_texture_builder_set_bytes (builder, bytes);
  gdk_memory_texture_builder_set_stride (builder, stride);
  gdk_memory_texture_builder_set_width (builder, map_width);
  gdk_memory_texture_builder_set_height (builder, map_height);
  gdk_memory_texture_builder_set_format (builder, GDK_MEMORY_R16G16B16);

  entry = g_new0 (MapEntry, 1);
  entry->key = *key;
  entry->max = max;
  entry->texture = gdk_memory_texture_builder_build (builder);

  g_object_unref (builder);
  g_bytes_unref (bytes);

  return entry;
}

static MapEntry *
lookup_map (float width,
            float height,
            float radius,
            float bezel,
            float refraction,
            float zoom)
{
  MapKey key = {
    lrintf (width * 4),
    lrintf (height * 4),
    lrintf (radius * 4),
    lrintf (bezel * 4),
    lrintf (refraction * 4),
    lrintf (zoom * 1000),
  };
  MapEntry *entry;

  if (map_cache == NULL)
    map_cache = g_hash_table_new_full (map_key_hash, map_key_equal, NULL, map_entry_free);

  entry = g_hash_table_lookup (map_cache, &key);
  if (entry)
    return entry;

  if (g_hash_table_size (map_cache) >= MAP_CACHE_SIZE)
    g_hash_table_remove_all (map_cache);

  entry = create_map (&key, width, height, radius, bezel, refraction, zoom);
  g_hash_table_insert (map_cache, &entry->key, entry);

  return entry;
}

static GskRenderNode *
saturate_node (GskRenderNode *child,
               float          saturation)
{
  static const float luma[3] = { 0.2126f, 0.7152f, 0.0722f };
  float values[16] = { 0 };
  graphene_matrix_t matrix;
  GskRenderNode *node;

  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      values[i * 4 + j] = (1 - saturation) * luma[i] + (i == j ? saturation : 0);
  values[15] = 1;

  graphene_matrix_init_from_float (&matrix, values);
  node = gsk_color_matrix_node_new (child, &matrix, graphene_vec4_zero ());
  gsk_render_node_unref (child);

  return node;
}

static gboolean
glass_is_valid (const SitGlass *glass)
{
  return isfinite (glass->blur) && glass->blur >= 0 &&
         isfinite (glass->refraction) && glass->refraction >= 0 &&
         isfinite (glass->bezel) && glass->bezel >= 0 &&
         isfinite (glass->zoom) && glass->zoom > 0 &&
         isfinite (glass->saturation) && glass->saturation >= 0 &&
         isfinite (glass->opacity);
}

/**
 * sit_snapshot_append_glass:
 * @snapshot: a `GtkSnapshot`
 * @shape: the glass outline in current coordinates
 * @glass: the glass parameters
 *
 * Draws the content already rendered behind @shape as seen through a
 * piece of glass: blurred by @glass->blur, magnified by @glass->zoom,
 * saturated by @glass->saturation and bent by up to @glass->refraction
 * pixels across a rim of @glass->bezel pixels. Tint and highlights are
 * left to the caller, which draws them on top.
 */
void
sit_snapshot_append_glass (GtkSnapshot          *snapshot,
                           const GskRoundedRect *shape,
                           const SitGlass       *glass)
{
  const GdkColorChannel channels[2] = { GDK_COLOR_CHANNEL_RED, GDK_COLOR_CHANNEL_GREEN };
  GskRoundedRect outline;
  graphene_rect_t paste_bounds;
  GskRenderNode *child, *map, *node;
  MapEntry *entry;
  float width, height, opacity;

  g_return_if_fail (snapshot != NULL);
  g_return_if_fail (shape != NULL);
  g_return_if_fail (glass != NULL);

  if (!glass_is_valid (glass))
    return;

  opacity = CLAMP (glass->opacity, 0, 1);
  width = shape->bounds.size.width;
  height = shape->bounds.size.height;
  if (opacity <= 0 || width < 1 || height < 1)
    return;

  gsk_rounded_rect_init_copy (&outline, shape);
  gsk_rounded_rect_normalize (&outline);

  entry = lookup_map (width, height, rounded_radius (&outline),
                      MIN (glass->bezel, MIN (width, height) / 2),
                      glass->refraction, glass->zoom);

  paste_bounds = outline.bounds;
  graphene_rect_inset (&paste_bounds, - entry->max - 2 * glass->blur, - entry->max - 2 * glass->blur);

  child = gsk_paste_node_new (&paste_bounds, 0);
  if (glass->blur > 0)
    {
      node = gsk_blur_node_new (child, glass->blur);
      gsk_render_node_unref (child);
      child = node;
    }
  if (glass->saturation != 1)
    child = saturate_node (child, glass->saturation);

  map = gsk_texture_node_new (entry->texture, &outline.bounds);
  node = gsk_displacement_node_new (&outline.bounds, child, map, channels,
                                    &GRAPHENE_SIZE_INIT (entry->max, entry->max),
                                    &GRAPHENE_SIZE_INIT (2 * entry->max, 2 * entry->max),
                                    &GRAPHENE_POINT_INIT (0.5f, 0.5f));
  gsk_render_node_unref (map);
  gsk_render_node_unref (child);

  gtk_snapshot_push_copy (snapshot);
  if (opacity < 1)
    gtk_snapshot_push_opacity (snapshot, opacity);
  gtk_snapshot_push_rounded_clip (snapshot, &outline);
  gtk_snapshot_append_node (snapshot, node);
  gtk_snapshot_pop (snapshot);
  if (opacity < 1)
    gtk_snapshot_pop (snapshot);
  gtk_snapshot_pop (snapshot);

  gsk_render_node_unref (node);
}

/**
 * sit_widget_set_glass:
 * @widget: a widget
 * @glass: (nullable): the glass parameters
 *
 * Draws the widget's CSS border box as glass, below its CSS background,
 * as [func@Sit.snapshot_append_glass] does. The parameters are copied.
 * %NULL removes the glass.
 *
 * Returns: %TRUE if the glass was accepted
 */
gboolean
sit_widget_set_glass (GtkWidget      *widget,
                      const SitGlass *glass)
{
  g_return_val_if_fail (GTK_IS_WIDGET (widget), FALSE);

  if (quark_sit_glass == 0)
    quark_sit_glass = g_quark_from_static_string ("sit-widget-glass");

  if (glass == NULL)
    {
      if (g_object_get_qdata (G_OBJECT (widget), quark_sit_glass) == NULL)
        return TRUE;
      g_object_set_qdata (G_OBJECT (widget), quark_sit_glass, NULL);
      gtk_widget_queue_draw (widget);
      return TRUE;
    }

  if (!glass_is_valid (glass))
    return FALSE;

  g_object_set_qdata_full (G_OBJECT (widget), quark_sit_glass,
                           g_memdup2 (glass, sizeof (SitGlass)), g_free);
  gtk_widget_queue_draw (widget);

  return TRUE;
}

static gboolean
display_has_software_gl (GdkDisplay *display)
{
  gpointer cached;
  GdkGLContext *context;
  const char *renderer;
  gboolean software = FALSE;

  if (quark_software_gl == 0)
    quark_software_gl = g_quark_from_static_string ("sit-software-gl");

  cached = g_object_get_qdata (G_OBJECT (display), quark_software_gl);
  if (cached)
    return GPOINTER_TO_INT (cached) == 2;

  context = gdk_display_get_gl_context (display);
  if (context)
    {
      gdk_gl_context_make_current (context);
      renderer = (const char *) glGetString (GL_RENDERER);
      software = renderer != NULL &&
                 (strstr (renderer, "llvmpipe") != NULL ||
                  strstr (renderer, "softpipe") != NULL ||
                  strstr (renderer, "Software") != NULL);
    }

  g_object_set_qdata (G_OBJECT (display), quark_software_gl, GINT_TO_POINTER (software ? 2 : 1));

  return software;
}

/**
 * sit_widget_get_glass_accelerated:
 * @widget: a realized widget
 *
 * Tells whether glass drawn in @widget's window runs on a GPU. Glass is
 * not drawn by [method@Sit.widget_set_glass] when the window renders with
 * Cairo, a software OpenGL driver or a CPU Vulkan device, since
 * refraction there costs whole frames.
 *
 * Returns: %TRUE if the window renders on a GPU
 */
gboolean
sit_widget_get_glass_accelerated (GtkWidget *widget)
{
  GtkNative *native;
  GskRenderer *renderer;

  g_return_val_if_fail (GTK_IS_WIDGET (widget), FALSE);

  native = gtk_widget_get_native (widget);
  if (native == NULL)
    return FALSE;

  renderer = gtk_native_get_renderer (native);
  if (renderer == NULL)
    return FALSE;

  if (GSK_IS_GL_RENDERER (renderer))
    return !display_has_software_gl (gtk_widget_get_display (widget));

#ifdef GDK_RENDERING_VULKAN
  if (GSK_IS_VULKAN_RENDERER (renderer))
    {
      GdkDisplay *display = gtk_widget_get_display (widget);
      VkPhysicalDeviceProperties props;

      if (display->vk_physical_device == VK_NULL_HANDLE)
        return FALSE;
      vkGetPhysicalDeviceProperties (display->vk_physical_device, &props);
      return props.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU;
    }
#endif

  return FALSE;
}

void
_sit_widget_snapshot_glass (GtkWidget            *widget,
                            GtkSnapshot          *snapshot,
                            const GskRoundedRect *border_box)
{
  const SitGlass *glass;

  if (quark_sit_glass == 0)
    return;

  glass = g_object_get_qdata (G_OBJECT (widget), quark_sit_glass);
  if (glass && sit_widget_get_glass_accelerated (widget))
    sit_snapshot_append_glass (snapshot, border_box, glass);
}
