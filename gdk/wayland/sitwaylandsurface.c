#include "config.h"

#include "sitwaylandsurface.h"
#include "gdksurface-wayland-private.h"
#include "gdkdisplay-wayland.h"

struct _SitWaylandSurfaceEffect
{
  struct zsingularity_blur_v1 *proxy;
  SitBackgroundEffect mode;
  cairo_region_t *region;
  guint strength;
  guint corner_radius;
  gboolean dirty;
};

/**
 * sit_wayland_surface_set_background_effect:
 * @surface: a Wayland surface
 * @mode: the background material
 * @region: (nullable): a surface-local rectangle, or %NULL for the whole surface
 * @strength: material strength, clamped to 100
 *
 * Queues the material for the next surface update. No surface commit or
 * display roundtrip is performed. The state survives hiding and remapping.
 *
 * Returns: %TRUE if the compositor supports background materials
 */
gboolean
sit_wayland_surface_set_background_effect (GdkSurface          *surface,
                                           SitBackgroundEffect  mode,
                                           const GdkRectangle  *region,
                                           guint                strength)
{
  cairo_region_t *shape = NULL;
  gboolean supported;

  g_return_val_if_fail (region == NULL ||
                        (region->width > 0 && region->height > 0), FALSE);

  if (region != NULL)
    {
      cairo_rectangle_int_t rect = { region->x, region->y, region->width, region->height };

      shape = cairo_region_create_rectangle (&rect);
    }
  supported = sit_wayland_surface_set_background_effect_region (surface, mode, shape, strength);
  g_clear_pointer (&shape, cairo_region_destroy);

  return supported;
}

/**
 * sit_wayland_surface_set_background_effect_region:
 * @surface: a Wayland surface
 * @mode: the background material
 * @region: (nullable) (transfer none): a surface-local region, or %NULL for the whole surface
 * @strength: material strength, clamped to 100
 *
 * Copies the region and queues the material for the next surface update.
 * An empty region excludes the whole surface. A non-null region requires
 * protocol version 2; older compositors return %FALSE. Strength requires
 * version 3 and is ignored by older versions. No surface commit or display
 * roundtrip is performed. The state survives hiding and remapping.
 *
 * Returns: %TRUE if the compositor supports background materials
 */
gboolean
sit_wayland_surface_set_background_effect_region (GdkSurface           *surface,
                                                  SitBackgroundEffect   mode,
                                                  const cairo_region_t *region,
                                                  guint                 strength)
{
  GdkWaylandSurface *self;
  GdkWaylandDisplay *display;
  SitWaylandSurfaceEffect *effect;

  g_return_val_if_fail (GDK_IS_WAYLAND_SURFACE (surface), FALSE);
  g_return_val_if_fail (mode >= SIT_BACKGROUND_EFFECT_DISABLED &&
                        mode <= SIT_BACKGROUND_EFFECT_LIQUID, FALSE);
  g_return_val_if_fail (region == NULL ||
                        cairo_region_status (region) == CAIRO_STATUS_SUCCESS, FALSE);

  if (gdk_surface_is_destroyed (surface))
    return FALSE;

  self = GDK_WAYLAND_SURFACE (surface);
  display = GDK_WAYLAND_DISPLAY (gdk_surface_get_display (surface));
  if (display->background_effect_manager == NULL)
    return FALSE;
  if (region != NULL &&
      zsingularity_blur_manager_v1_get_version (display->background_effect_manager) <
        ZSINGULARITY_BLUR_V1_SET_REGION_SINCE_VERSION)
    return FALSE;

  if (self->background_effect == NULL)
    self->background_effect = g_new0 (SitWaylandSurfaceEffect, 1);

  effect = self->background_effect;
  strength = MIN (strength, 100);
  if (effect->mode == mode && effect->strength == strength &&
      (effect->region == NULL) == (region == NULL) &&
      (region == NULL || cairo_region_equal (effect->region, region)))
    return TRUE;

  effect->mode = mode;
  effect->strength = strength;
  g_clear_pointer (&effect->region, cairo_region_destroy);
  if (region != NULL)
    effect->region = cairo_region_copy (region);
  effect->dirty = TRUE;
  gdk_surface_queue_render (surface);

  return TRUE;
}

/**
 * sit_wayland_surface_set_background_effect_corner_radius:
 * @surface: a Wayland surface
 * @radius: the corner radius of every connected part of the material region
 *
 * Queues the corner radius used by refractive materials for the next
 * surface update. Compositors older than protocol version 5 ignore it.
 *
 * Returns: %TRUE if the compositor supports background materials
 */
gboolean
sit_wayland_surface_set_background_effect_corner_radius (GdkSurface *surface,
                                                         guint       radius)
{
  GdkWaylandSurface *self;
  GdkWaylandDisplay *display;

  g_return_val_if_fail (GDK_IS_WAYLAND_SURFACE (surface), FALSE);

  if (gdk_surface_is_destroyed (surface))
    return FALSE;

  self = GDK_WAYLAND_SURFACE (surface);
  display = GDK_WAYLAND_DISPLAY (gdk_surface_get_display (surface));
  if (display->background_effect_manager == NULL)
    return FALSE;

  if (self->background_effect == NULL)
    self->background_effect = g_new0 (SitWaylandSurfaceEffect, 1);
  if (self->background_effect->corner_radius == radius)
    return TRUE;

  self->background_effect->corner_radius = radius;
  self->background_effect->dirty = TRUE;
  gdk_surface_queue_render (surface);

  return TRUE;
}

void
sit_wayland_surface_sync_background_effect (GdkWaylandSurface *self)
{
  GdkWaylandDisplay *display;
  SitWaylandSurfaceEffect *effect = self->background_effect;
  guint version;

  if (effect == NULL || !effect->dirty || self->display_server.wl_surface == NULL)
    return;

  display = GDK_WAYLAND_DISPLAY (gdk_surface_get_display (GDK_SURFACE (self)));
  if (effect->proxy == NULL)
    {
      if (display->background_effect_manager == NULL)
        return;

      effect->proxy = zsingularity_blur_manager_v1_get_blur (display->background_effect_manager,
                                                           self->display_server.wl_surface);
    }

  version = zsingularity_blur_v1_get_version (effect->proxy);
  if (version >= ZSINGULARITY_BLUR_V1_SET_MODE_SINCE_VERSION)
    {
      struct wl_region *region = NULL;

      SitBackgroundEffect mode = effect->mode;

      if (mode == SIT_BACKGROUND_EFFECT_LIQUID &&
          version < ZSINGULARITY_BLUR_V1_SET_CORNER_RADIUS_SINCE_VERSION)
        mode = SIT_BACKGROUND_EFFECT_GLASS;
      zsingularity_blur_v1_set_mode (effect->proxy, mode);
      if (version >= ZSINGULARITY_BLUR_V1_SET_STRENGTH_SINCE_VERSION)
        zsingularity_blur_v1_set_strength (effect->proxy, effect->strength);
      if (version >= ZSINGULARITY_BLUR_V1_SET_CORNER_RADIUS_SINCE_VERSION)
        zsingularity_blur_v1_set_corner_radius (effect->proxy, effect->corner_radius);

      if (effect->region != NULL)
        {
          region = wl_compositor_create_region (display->compositor);
          for (int i = 0; i < cairo_region_num_rectangles (effect->region); i++)
            {
              cairo_rectangle_int_t rect;

              cairo_region_get_rectangle (effect->region, i, &rect);
              wl_region_add (region, rect.x, rect.y, rect.width, rect.height);
            }
        }
      zsingularity_blur_v1_set_region (effect->proxy, region);
      if (region != NULL)
        wl_region_destroy (region);
    }
  else
    {
      zsingularity_blur_v1_set_radius (effect->proxy,
                                      effect->mode == SIT_BACKGROUND_EFFECT_DISABLED ? 0 : 24);
      zsingularity_blur_v1_set_noise (effect->proxy, 0);
    }

  zsingularity_blur_v1_commit (effect->proxy);
  effect->dirty = FALSE;
}

void
sit_wayland_surface_clear_background_effect (GdkWaylandSurface *self)
{
  if (self->background_effect == NULL)
    return;

  g_clear_pointer (&self->background_effect->proxy, zsingularity_blur_v1_destroy);
  self->background_effect->dirty = TRUE;
}

gboolean
sit_wayland_surface_background_effect_needs_commit (GdkWaylandSurface *self)
{
  return self->background_effect != NULL && self->background_effect->dirty;
}

void
sit_wayland_surface_destroy_background_effect (GdkWaylandSurface *self)
{
  if (self->background_effect == NULL)
    return;

  sit_wayland_surface_clear_background_effect (self);
  g_clear_pointer (&self->background_effect->region, cairo_region_destroy);
  g_clear_pointer (&self->background_effect, g_free);
}
