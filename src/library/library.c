
#include "library/library.h"
#include "control/jobs.h"
#include "control/settings.h"
#include "common/imageio.h"
#include "common/image_cache.h"
#include "gui/gtk.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <assert.h>


void dt_library_init(dt_library_t *lib)
{
  lib->film = (dt_film_roll_t *)malloc(sizeof(dt_film_roll_t));
  lib->select_offset_x = lib->select_offset_y = 0.5f;
  lib->last_selected_id = -1;
  lib->button = 0;
  lib->modifiers = 0;
  dt_film_roll_init(lib->film);
}

void dt_library_cleanup(dt_library_t *lib)
{
  dt_film_roll_cleanup(lib->film);
  free(lib->film);
}

void dt_library_toggle_selection(int iid)
{
  int rc;
  sqlite3_stmt *stmt;
  rc = sqlite3_prepare_v2(darktable.db, "select * from selected_images where imgid = ?1", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, iid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    sqlite3_finalize(stmt);
    rc = sqlite3_prepare_v2(darktable.db, "delete from selected_images where imgid = ?1", -1, &stmt, NULL);
    rc = sqlite3_bind_int (stmt, 1, iid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  else
  {
    sqlite3_finalize(stmt);
    rc = sqlite3_prepare_v2(darktable.db, "insert into selected_images values (?1)", -1, &stmt, NULL);
    rc = sqlite3_bind_int (stmt, 1, iid);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
}

void dt_library_expose(dt_library_t *lib, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  float zoom, zoom_x, zoom_y;
  int32_t mouse_over_id, pan, track, center;
  DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
  DT_CTL_GET_GLOBAL(zoom, lib_zoom);
  DT_CTL_GET_GLOBAL(zoom_x, lib_zoom_x);
  DT_CTL_GET_GLOBAL(zoom_y, lib_zoom_y);
  DT_CTL_GET_GLOBAL(pan, lib_pan);
  DT_CTL_GET_GLOBAL(track, lib_track);
  DT_CTL_GET_GLOBAL(center, lib_center);

  cairo_set_source_rgb (cr, .2, .2, .2);
  cairo_paint(cr);

  const float wd = width/zoom;
  const float ht = width/zoom;

  static int oldpan = 0;
  static float oldzoom = -1;
  if(oldzoom < 0) oldzoom = zoom;

  // TODO: exaggerate mouse gestures to pan when zoom == 1
  if(pan)// && mouse_over_id >= 0)
  {
    zoom_x = lib->select_offset_x - /* (zoom == 1 ? 2. : 1.)*/pointerx;
    zoom_y = lib->select_offset_y - /* (zoom == 1 ? 2. : 1.)*/pointery;
  }

  if(oldzoom != zoom)
  {
    float oldx = (pointerx + zoom_x)*oldzoom/width;
    float oldy = (pointery + zoom_y)*oldzoom/width;
    if(zoom == 1)
    {
      zoom_x = (int)oldx*wd;
      zoom_y = (int)oldy*ht;
    }
    else
    {
      zoom_x = oldx*wd - pointerx;
      zoom_y = oldy*ht - pointery;
    }
  }
  oldzoom = zoom;

  // TODO: track on aoe, keys!
  if(center)
  {
    if(mouse_over_id >= 0)
    {
      zoom_x -= width*.5f  - pointerx;
      zoom_y -= height*.5f - pointery;
    }
    else zoom_x = zoom_y = 0.0;
    center = 0;
  }

  // mouse left the area
  if(!pan) DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, -1);

  int offset_i = (int)(zoom_x/wd);
  int offset_j = (int)(zoom_y/ht);
  float offset_x = zoom == 1 ? 0.0 : zoom_x/wd - (int)(zoom_x/wd);
  float offset_y = zoom == 1 ? 0.0 : zoom_y/ht - (int)(zoom_y/ht);
  const int max_rows = zoom == 1 ? 1 : 2 + (int)((height)/ht + .5);
  const int max_cols = zoom == 1 ? 1 : MIN(DT_LIBRARY_MAX_ZOOM - MAX(0, offset_i), 1 + (int)(zoom+.5));

  int offset = MAX(0, offset_i) + DT_LIBRARY_MAX_ZOOM*offset_j;
  int seli = (int)((pointerx + zoom_x)/wd) - MAX(offset_i, 0);
  int selj = (int)((pointery + zoom_y)/ht) - offset_j;

  sqlite3_stmt *stmt, *stmt2;
  int rc, rc2, id, clicked1, last_seli = 1<<30, last_selj = 1<<30;
  clicked1 = (oldpan == 0 && pan == 1 && lib->button == 1);
  if(clicked1 &&
    (lib->modifiers & GDK_SHIFT_MASK) == 0 && (lib->modifiers & GDK_CONTROL_MASK) == 0)
  { // clear selected if no modifier
    rc2 = sqlite3_prepare_v2(darktable.db, "delete from selected_images", -1, &stmt2, NULL);
    rc2 = sqlite3_step(stmt2);
    sqlite3_finalize(stmt2);
  }
  // TODO: order by and where clauses from sort widget!
  rc = sqlite3_prepare_v2(darktable.db, "select * from (select id from images where film_id = ?1 order by filename) as dreggn limit ?2, ?3", -1, &stmt, NULL);
  cairo_translate(cr, -offset_x*wd, -offset_y*ht);
  cairo_translate(cr, -MIN(offset_i*wd, 0.0), 0.0);
  for(int row = 0; row < max_rows; row++)
  {
    if(offset < 0)
    {
      cairo_translate(cr, 0, ht);
      offset += DT_LIBRARY_MAX_ZOOM;
      continue;
    }
    rc = sqlite3_bind_int (stmt, 1, lib->film->id);
    rc = sqlite3_bind_int (stmt, 2, offset);
    rc = sqlite3_bind_int (stmt, 3, max_cols);
    for(int col = 0; col < max_cols; col++)
    {
      if(sqlite3_step(stmt) == SQLITE_ROW)
      {
        id = sqlite3_column_int(stmt, 0);
        dt_image_t *image = dt_image_cache_get(id, 'r');
        if(image)
        {
          // set mouse over id
          if((zoom == 1 && mouse_over_id < 0) || (!pan && seli == col && selj == row))
          {
            mouse_over_id = image->id;
            DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, mouse_over_id);
          }
          // add clicked image to selected table
          if(clicked1)
          {
            // FIXME: whatever comes first assumtion is broken!
            // if((lib->modifiers & GDK_SHIFT_MASK) && (last_seli == (1<<30)) &&
            //    (image->id == lib->last_selected_id || image->id == mouse_over_id)) { last_seli = col; last_selj = row; }
            // if(last_seli < (1<<30) && ((lib->modifiers & GDK_SHIFT_MASK) && (col >= MIN(last_seli,seli) && row >= MIN(last_selj,selj) &&
            //         col <= MAX(last_seli,seli) && row <= MAX(last_selj,selj)) && (col != last_seli || row != last_selj)) ||
            if((lib->modifiers & GDK_SHIFT_MASK) && image->id == lib->last_selected_id) { last_seli = col; last_selj = row; }
            if((last_seli < (1<<30) && ((lib->modifiers & GDK_SHIFT_MASK) && (col >= last_seli && row >= last_selj &&
                    col <= seli && row <= selj) && (col != last_seli || row != last_selj))) ||
               (seli == col && selj == row))
            { // insert all in range if shift, or only the one the mouse is over for ctrl or plain click.
              dt_library_toggle_selection(image->id);
              lib->last_selected_id = image->id;
            }
          }
          cairo_save(cr);
          dt_image_expose(image, image->id, cr, wd, zoom == 1 ? height : ht, zoom);
          cairo_restore(cr);
          dt_image_cache_release(image, 'r');
        }
      }
      cairo_translate(cr, wd, 0.0f);
    }
    cairo_translate(cr, -max_cols*wd, ht);
    offset += DT_LIBRARY_MAX_ZOOM;
    rc = sqlite3_reset(stmt);
    rc = sqlite3_clear_bindings(stmt);
  }
  sqlite3_finalize(stmt);

  oldpan = pan;
  DT_CTL_SET_GLOBAL(lib_zoom_x, zoom_x);
  DT_CTL_SET_GLOBAL(lib_zoom_y, zoom_y);
  DT_CTL_SET_GLOBAL(lib_track, track);
  DT_CTL_SET_GLOBAL(lib_center, center);
}

void dt_library_button_released(dt_library_t *lib, double x, double y, int which, uint32_t state)
{
  DT_CTL_SET_GLOBAL(lib_pan, 0);
}

void dt_library_button_pressed(dt_library_t *lib, double xx, double yy, int which, uint32_t state)
{
  lib->modifiers = state;
  lib->button = which;
  DT_CTL_GET_GLOBAL(lib->select_offset_x, lib_zoom_x);
  DT_CTL_GET_GLOBAL(lib->select_offset_y, lib_zoom_y);
  lib->select_offset_x += xx;
  lib->select_offset_y += yy;
  DT_CTL_SET_GLOBAL(lib_pan, 1);
}

void dt_library_mouse_leave(dt_library_t *lib)
{
  if(!darktable.control->global_settings.lib_pan && darktable.control->global_settings.lib_zoom != 1)
  {
    DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, -1);
    dt_control_queue_draw(); // remove focus
  }
}

void dt_library_mouse_moved(dt_library_t *lib, double xx, double yy, int which)
{
  dt_control_queue_draw();
}

void dt_image_expose(dt_image_t *img, int32_t index, cairo_t *cr, int32_t width, int32_t height, int32_t zoom)
{
  float bgcol = 0.4, fontcol = 0.5, bordercol = 0.1;
  int selected = 0, imgsel;
  DT_CTL_GET_GLOBAL(imgsel, lib_image_mouse_over_id);
  // if(img->flags & DT_IMAGE_SELECTED) selected = 1;
  sqlite3_stmt *stmt;
  int rc;
  rc = sqlite3_prepare_v2(darktable.db, "select * from selected_images where imgid = ?1", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, img->id);
  if(sqlite3_step(stmt) == SQLITE_ROW) selected = 1;
  sqlite3_finalize(stmt);
  if(selected == 1)
  {
    bgcol = 0.6; fontcol = 0.5;
  }
  if(imgsel == img->id) { bgcol = 0.8; fontcol = 0.7; } // selected = 1;
  float imgwd = 0.8f;
  if(zoom == 1)
  {
    // TODO: draw exif info etc
    imgwd = .97f;
    // cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
  }
  else
  {
    char num[10];
    cairo_rectangle(cr, 0.01f*width, 0.01f*height, 0.99f*width, 0.99f*height);
    cairo_set_source_rgb(cr, bgcol, bgcol, bgcol);
    cairo_fill(cr);
    // cairo_rectangle(cr, 0, 0, width, height);
    // cairo_set_source_rgb(cr, bgcol, bgcol, bgcol);
    // cairo_fill_preserve(cr);
    // cairo_set_source_rgb(cr, fontcol, fontcol, fontcol);
    // cairo_set_line_width(cr, 1);
    // cairo_stroke(cr);

    cairo_set_source_rgb(cr, fontcol, fontcol, fontcol);
    cairo_select_font_face (cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size (cr, .3*width);

    cairo_move_to (cr, .0*width, .24*height);
    snprintf(num, 10, "%d", index);
    cairo_show_text (cr, num);
  }

#if 1
  int32_t iwd = width*imgwd, iht = height*imgwd, stride;
  float scale = 1.0;
  dt_image_buffer_t mip;
  mip = dt_image_get_matching_mip_size(img, imgwd*width, imgwd*height, &iwd, &iht);
  mip = dt_image_get(img, mip, 'r');
  dt_image_get_mip_size(img, mip, &iwd, &iht);
  float fwd, fht;
  dt_image_get_exact_mip_size(img, mip, &fwd, &fht);
  cairo_surface_t *surface = NULL;
  if(mip != DT_IMAGE_NONE)
  {
    stride = cairo_format_stride_for_width (CAIRO_FORMAT_RGB24, iwd);
    surface = cairo_image_surface_create_for_data (img->mip[mip], CAIRO_FORMAT_RGB24, iwd, iht, stride); 
    scale = fminf(width*imgwd/fwd, height*imgwd/fht);
  }

  // draw centered and fitted:
  cairo_save(cr);
  cairo_translate(cr, width/2.0, height/2.0f);
  cairo_scale(cr, scale, scale);
  cairo_translate(cr, -.5f*fwd, -.5f*fht);

  const float border = zoom == 1 ? 16/scale : 2/scale;

  if(mip != DT_IMAGE_NONE)
  {
    cairo_set_source_surface (cr, surface, 0, 0);
    cairo_rectangle(cr, 0, 0, fwd, fht);
    cairo_fill(cr);
    cairo_surface_destroy (surface);
    dt_image_release(img, mip, 'r');
  }

  if(zoom == 1) cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
  cairo_rectangle(cr, 0, 0, fwd, fht);
  cairo_set_source_rgb(cr, bordercol, bordercol, bordercol);
  if(selected)
  {
    cairo_set_line_width(cr, 1./scale);
    if(zoom == 1)
    { // draw shadow around border
      cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
      cairo_stroke(cr);
      // cairo_new_path(cr);
      cairo_set_fill_rule (cr, CAIRO_FILL_RULE_EVEN_ODD);
      float alpha = 1.0f;
      for(int k=0;k<16;k++)
      {
        cairo_rectangle(cr, 0, 0, fwd, fht);
        cairo_new_sub_path(cr);
        cairo_rectangle(cr, -k/scale, -k/scale, fwd+2.*k/scale, fht+2.*k/scale);
        cairo_set_source_rgba(cr, 0, 0, 0, alpha);
        alpha *= 0.6f;
        cairo_fill(cr);
      }
    }
    else
    {
      cairo_set_fill_rule (cr, CAIRO_FILL_RULE_EVEN_ODD);
      cairo_new_sub_path(cr);
      cairo_rectangle(cr, -border, -border, fwd+2.*border, fht+2.*border);
      cairo_stroke_preserve(cr);
      cairo_set_source_rgb(cr, 1.0-bordercol, 1.0-bordercol, 1.0-bordercol);
      cairo_fill(cr);
    }
  }
  else
  {
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
  }
  cairo_restore(cr);

  if(selected && (zoom == 1))
  {
    cairo_set_source_rgb(cr, fontcol, fontcol, fontcol);
    cairo_select_font_face (cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size (cr, .02*width);

    cairo_move_to (cr, .0*width, .024*height);
    cairo_show_text(cr, img->filename);
    // cairo_text_path(cr, image->filename);
    // cairo_fill_preserve(cr);
    // cairo_set_source_rgb(cr, 0, 0, 0);
    // cairo_stroke(cr);
  }

  // if(zoom == 1) cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
#endif
}

