#include "config.h"

#include "sit.h"
#include "gtkcssboxesprivate.h"
#include "gtkwindowprivate.h"
#include "gtkwidgetprivate.h"

#include <math.h>

/**
 * sit_widget_get_border_box:
 * @widget: a widget
 * @box: (out caller-allocates): the widget's CSS border box
 *
 * Copies the current border box, including normalized corner radii, in
 * widget coordinates. Shadows and margins are excluded. The size follows
 * the widget's most recent allocation; snapshot transforms are not applied.
 * Windows with a border widget use its border in window coordinates.
 */
void
sit_widget_get_border_box (GtkWidget      *widget,
                           GskRoundedRect *box)
{
  GtkCssBoxes boxes;

  g_return_if_fail (GTK_IS_WIDGET (widget));
  g_return_if_fail (box != NULL);

  if (GTK_IS_WINDOW (widget) &&
      _gtk_window_get_border_box (GTK_WINDOW (widget), box))
    return;

  gtk_css_boxes_init (&boxes, widget);
  *box = *gtk_css_boxes_get_border_box (&boxes);
}

/**
 * sit_window_set_border_widget:
 * @window: a window
 * @widget: (nullable): a non-native descendant, or %NULL to restore the window border
 *
 * Uses @widget's CSS border for the window's shadow and native input shape.
 * The window keeps a weak reference. Layout is unchanged, and eligible CSD
 * resize handles remain outside the border. Allocation transforms must be
 * axis-aligned; unavailable geometry falls back to the window's own border.
 *
 * Returns: %TRUE if the binding was accepted, %FALSE for an invalid descendant
 */
gboolean
sit_window_set_border_widget (GtkWindow *window,
                              GtkWidget *widget)
{
  return _gtk_window_set_border_widget (window, widget);
}

static double
corner_inset (const graphene_size_t *corner,
              double                 distance)
{
  double t;

  if (corner->width <= 0 || corner->height <= 0 || distance >= corner->height)
    return 0;

  t = 1 - distance / corner->height;
  return corner->width * (1 - sqrt (fmax (0, 1 - t * t)));
}

static cairo_region_t *
border_region (const GskRoundedRect *box,
               int                   width,
               int                   height)
{
  cairo_region_t *region = cairo_region_create ();
  double left = box->bounds.origin.x;
  double top = box->bounds.origin.y;
  double right = left + box->bounds.size.width;
  double bottom = top + box->bounds.size.height;
  int first = (int) CLAMP (ceil (top - 0.5), 0, height);
  int last = (int) CLAMP (ceil (bottom - 0.5), 0, height);

  for (int y = first; y < last; y++)
    {
      double from_top = y + 0.5 - top;
      double from_bottom = bottom - y - 0.5;
      double a = MAX (corner_inset (&box->corner[0], from_top),
                      corner_inset (&box->corner[3], from_bottom));
      double b = MAX (corner_inset (&box->corner[1], from_top),
                      corner_inset (&box->corner[2], from_bottom));
      int x = (int) CLAMP (ceil (left + a - 0.5), 0, width);
      int end = (int) CLAMP (MIN (floor (right - b - 0.5) + 1,
                                 ceil (right - 0.5)), 0, width);

      if (end > x)
        {
          cairo_rectangle_int_t row = { x, y, end - x, 1 };
          cairo_region_union_rectangle (region, &row);
        }
    }

  return region;
}

static cairo_region_t *
transformed_region (const GskRoundedRect   *box,
                    const graphene_matrix_t *matrix,
                    GtkNative              *native)
{
  GdkSurface *surface = gtk_native_get_surface (native);
  GskRoundedRect scaled;
  float values[16];
  double xx, yx, xy, yy, dx, dy, offset_x, offset_y;
  double x, y, width, height;
  unsigned flip;

  graphene_matrix_to_float (matrix, values);
  for (unsigned i = 0; i < 16; i++)
    {
      if (!isfinite (values[i]))
        return cairo_region_create ();
    }
  if (!graphene_matrix_to_2d (matrix, &xx, &yx, &xy, &yy, &dx, &dy) || xy != 0 || yx != 0)
    return NULL;
  if (!isfinite (xx) || !isfinite (yy) || !isfinite (dx) || !isfinite (dy) || xx == 0 || yy == 0)
    return cairo_region_create ();

  gtk_native_get_surface_transform (native, &offset_x, &offset_y);
  x = box->bounds.origin.x * xx + dx + offset_x;
  y = box->bounds.origin.y * yy + dy + offset_y;
  width = box->bounds.size.width * xx;
  height = box->bounds.size.height * yy;
  scaled.bounds = GRAPHENE_RECT_INIT (MIN (x, x + width), MIN (y, y + height),
                                     fabs (width), fabs (height));
  flip = (xx < 0 ? 1 : 0) ^ (yy < 0 ? 3 : 0);
  for (unsigned i = 0; i < 4; i++)
    {
      scaled.corner[i].width = box->corner[i ^ flip].width * fabs (xx);
      scaled.corner[i].height = box->corner[i ^ flip].height * fabs (yy);
    }

  if (!isfinite (scaled.bounds.origin.x) || !isfinite (scaled.bounds.origin.y) ||
      !isfinite (scaled.bounds.size.width) || !isfinite (scaled.bounds.size.height))
    return cairo_region_create ();
  for (unsigned i = 0; i < 4; i++)
    {
      if (!isfinite (scaled.corner[i].width) || !isfinite (scaled.corner[i].height))
        return cairo_region_create ();
    }

  return border_region (&scaled, gdk_surface_get_width (surface),
                         gdk_surface_get_height (surface));
}

static gboolean
region_transform (GtkWidget         *widget,
                  GtkWidget         *transform_widget,
                  GskTransform      *transform,
                  GtkNative         *native,
                  graphene_matrix_t *matrix)
{
  graphene_matrix_t widget_to_transform, visual, transform_to_native, combined;

  if (!gtk_widget_compute_transform (widget, transform_widget, &widget_to_transform) ||
      !gtk_widget_compute_transform (transform_widget, GTK_WIDGET (native), &transform_to_native))
    return FALSE;

  gsk_transform_to_matrix (transform, &visual);
  graphene_matrix_multiply (&widget_to_transform, &visual, &combined);
  graphene_matrix_multiply (&combined, &transform_to_native, matrix);
  return TRUE;
}

/**
 * sit_widget_create_border_region:
 * @widget: a widget in a native surface
 * @transform_widget: (nullable): the coordinate space of @transform, or @widget
 * @transform: (nullable): an additional translation and scale, or identity
 *
 * Creates an integer region of the CSS border box in native surface coordinates.
 * The transform is applied in @transform_widget coordinates after allocation
 * transforms. Pixel centers determine membership in half-open border bounds;
 * rounded corner boundaries are included. The surface bounds clip the
 * result, including visual clips on the widget and its ancestors. Shadows are excluded. Collapsed or non-finite transforms produce an
 * empty region. Rotations and perspective are not supported.
 *
 * Returns: (transfer full) (nullable): the region, or %NULL without a surface or
 *   when the transform is not an axis-aligned 2D transform
 */
cairo_region_t *
sit_widget_create_border_region (GtkWidget    *widget,
                                 GtkWidget    *transform_widget,
                                 GskTransform *transform)
{
  GtkNative *native;
  graphene_matrix_t matrix;
  GskRoundedRect box;
  cairo_region_t *region;

  g_return_val_if_fail (GTK_IS_WIDGET (widget), NULL);
  g_return_val_if_fail (transform_widget == NULL || GTK_IS_WIDGET (transform_widget), NULL);

  if (transform_widget == NULL)
    transform_widget = widget;
  native = gtk_widget_get_native (widget);
  if (native == NULL || gtk_native_get_surface (native) == NULL)
    return NULL;
  if (!region_transform (widget, transform_widget, transform, native, &matrix))
    return NULL;

  sit_widget_get_border_box (widget, &box);
  region = transformed_region (&box, &matrix, native);
  if (region == NULL)
    return NULL;

  for (GtkWidget *ancestor = widget; ancestor != GTK_WIDGET (native);
       ancestor = gtk_widget_get_parent (ancestor))
    {
      cairo_region_t *clip;
      gboolean transformed;

      if (!_gtk_widget_get_visual_clip (ancestor, &box))
        continue;

      transformed = ancestor == transform_widget ||
                    gtk_widget_is_ancestor (ancestor, transform_widget);
      if (!region_transform (ancestor, transformed ? transform_widget : ancestor,
                             transformed ? transform : NULL, native, &matrix))
        {
          cairo_region_destroy (region);
          return NULL;
        }
      clip = transformed_region (&box, &matrix, native);
      if (clip == NULL)
        {
          cairo_region_destroy (region);
          return NULL;
        }
      cairo_region_intersect (region, clip);
      cairo_region_destroy (clip);
    }

  return region;
}

/**
 * sit_widget_set_visual_transform:
 * @widget: a non-native widget
 * @transform: (nullable): a transform in widget content coordinates
 *
 * Composes @transform with the widget's allocation and CSS transform.
 * Rendering, picking and coordinate conversion use the same result.
 * The widget's layout and cached content are unchanged. %NULL restores
 * the allocation transform. Native widgets and non-finite transforms
 * are rejected.
 *
 * Returns: %TRUE if the transform was accepted
 */
gboolean
sit_widget_set_visual_transform (GtkWidget    *widget,
                                 GskTransform *transform)
{
  return _gtk_widget_set_visual_transform (widget, transform);
}

/**
 * sit_widget_get_visual_transform:
 * @widget: a widget
 *
 * Returns: (transfer none) (nullable): the current visual transform
 */
GskTransform *
sit_widget_get_visual_transform (GtkWidget *widget)
{
  return _gtk_widget_get_visual_transform (widget);
}

/**
 * sit_widget_set_visual_opacity:
 * @widget: a non-native widget
 * @opacity: opacity between zero and one
 *
 * Enables cached content for subsequent GtkWidget opacity changes.
 * The GTK opacity property and notification are preserved. The first
 * call invalidates existing content once; later fades reuse it.
 *
 * Returns: %TRUE if the opacity was accepted
 */
gboolean
sit_widget_set_visual_opacity (GtkWidget *widget,
                               double     opacity)
{
  return _gtk_widget_set_visual_opacity (widget, opacity);
}

/**
 * sit_widget_set_visual_clip:
 * @widget: a non-native widget
 * @clip: (nullable): a rounded clip in widget content coordinates
 *
 * Clips cached content and picking without changing layout. The clip
 * is copied and its radii are normalized. %NULL removes the clip.
 *
 * Returns: %TRUE if the clip was accepted
 */
gboolean
sit_widget_set_visual_clip (GtkWidget            *widget,
                            const GskRoundedRect *clip)
{
  return _gtk_widget_set_visual_clip (widget, clip);
}
