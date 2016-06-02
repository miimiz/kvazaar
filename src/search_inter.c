/*****************************************************************************
* This file is part of Kvazaar HEVC encoder.
*
* Copyright (C) 2013-2015 Tampere University of Technology and others (see
* COPYING file).
*
* Kvazaar is free software: you can redistribute it and/or modify it under
* the terms of the GNU Lesser General Public License as published by the
* Free Software Foundation; either version 2.1 of the License, or (at your
* option) any later version.
*
* Kvazaar is distributed in the hope that it will be useful, but WITHOUT ANY
* WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
* FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
* more details.
*
* You should have received a copy of the GNU General Public License along
* with Kvazaar.  If not, see <http://www.gnu.org/licenses/>.
****************************************************************************/

#include "search_inter.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "cabac.h"
#include "encoder.h"
#include "image.h"
#include "imagelist.h"
#include "inter.h"
#include "kvazaar.h"
#include "rdo.h"
#include "strategies/strategies-ipol.h"
#include "strategies/strategies-picture.h"
#include "videoframe.h"


/**
 * \return  True if referred block is within current tile.
 */
static INLINE bool fracmv_within_tile(const encoder_state_t *state, const vector2d_t* orig, int x, int y, int width, int height, int wpp_limit)
{
  if (state->encoder_control->cfg->mv_constraint == KVZ_MV_CONSTRAIN_NONE) {
    return (wpp_limit == -1 || y + (height << 2) <= (wpp_limit << 2));
  };

  int margin = 0;
  if (state->encoder_control->cfg->mv_constraint == KVZ_MV_CONSTRAIN_FRAME_AND_TILE_MARGIN) {
    // Enforce a distance of 8 from any tile boundary.
    margin = 4 * 4;
  }

  // TODO implement KVZ_MV_CONSTRAIN_FRAM and KVZ_MV_CONSTRAIN_TILE.
  const vector2d_t abs_mv = { (orig->x << 2) + x, (orig->y << 2) + y };

  // Check that both margin and wpp_limit constraints are satisfied.
  if (abs_mv.x >= margin && abs_mv.x + (width << 2) <= (state->tile->frame->width << 2) - margin &&
      abs_mv.y >= margin && abs_mv.y + (height << 2) <= (state->tile->frame->height << 2) - margin &&
      (wpp_limit == -1 || y + (height << 2) <= (wpp_limit << 2)))
  {
    return true;
  } else {
    return false;
  }
}


static INLINE int get_wpp_limit(const encoder_state_t *state, const vector2d_t* orig)
{
  const encoder_control_t *ctrl = state->encoder_control;
  if (ctrl->owf && ctrl->wpp) {
    // Limit motion vectors to the LCU-row below this row.
    // To avoid fractional pixel interpolation depending on things outside
    // this range, add a margin of 4 pixels.
    // - fme needs 4 pixels
    // - odd chroma interpolation needs 4 pixels
    int wpp_limit = 2 * LCU_WIDTH - 4 - orig->y % LCU_WIDTH;
    if (ctrl->deblock_enable && !ctrl->sao_enable) {
      // As a special case, when deblocking is enabled but SAO is not, we have
      // to avoid the possibility of interpolation filters reaching the
      // non-deblocked pixels. The deblocking for the horizontal edge on the
      // LCU boundary can reach 4 pixels. If SAO is enabled, this WPP-row
      // depends on the SAO job, which depends on the deblocking having
      // already been done.
      wpp_limit -= 4;
    }
    return wpp_limit;
  } else {
    return -1;
  }
}


/**
 * \return  True if referred block is within current tile.
 */
static INLINE bool intmv_within_tile(const encoder_state_t *state, const vector2d_t* orig, int x, int y, int width, int height, int wpp_limit)
{
  return fracmv_within_tile(state, orig, x << 2, y << 2, width, height, wpp_limit);
}


static uint32_t get_ep_ex_golomb_bitcost(uint32_t symbol, uint32_t count)
{
  int32_t num_bins = 0;
  while (symbol >= (uint32_t)(1 << count)) {
    ++num_bins;
    symbol -= 1 << count;
    ++count;
  }
  num_bins ++;

  return num_bins;
}

/**Checks if mv is one of the merge candidates
* \return true if found else return false
*/
static bool mv_in_merge(const inter_merge_cand_t* merge_cand, int16_t num_cand, const vector2d_t* mv)
{
  for (int i = 0; i < num_cand; ++i) {
    if (merge_cand[i].dir == 3) continue;
    const vector2d_t merge_mv = {
      merge_cand[i].mv[merge_cand[i].dir - 1][0] >> 2,
      merge_cand[i].mv[merge_cand[i].dir - 1][1] >> 2
    };
    if (merge_mv.x == mv->x && merge_mv.y == mv->y) {
      return true;
    }
  }
  return false;
}


static unsigned select_starting_point(int16_t num_cand, inter_merge_cand_t *merge_cand, vector2d_t *mv_in_out, vector2d_t *mv, const encoder_state_t *const state,
                                      const vector2d_t *orig, unsigned width, unsigned height, int wpp_limit, const kvz_picture *pic, const kvz_picture *ref,
                                      int16_t mv_cand[2][2], int32_t ref_idx, unsigned best_cost, uint32_t *bitcost, unsigned *best_index, uint32_t *best_bitcost,
                                      int(*calc_mvd)(const encoder_state_t * const, int, int, int, int16_t[2][2], inter_merge_cand_t[MRG_MAX_NUM_CANDS],
                                      int16_t, int32_t, uint32_t *)){
  // Go through candidates
  for (unsigned i = 0; i < num_cand; ++i)
  {
    if (merge_cand[i].dir == 3) continue;
    mv->x = merge_cand[i].mv[merge_cand[i].dir - 1][0] >> 2;
    mv->y = merge_cand[i].mv[merge_cand[i].dir - 1][1] >> 2;

    if (mv->x == 0 && mv->y == 0) continue;
    if (!intmv_within_tile(state, orig, mv->x, mv->y, width, height, wpp_limit)) {
      continue;
    }

    uint32_t bitcost;
    unsigned cost = kvz_image_calc_sad(pic, ref, orig->x, orig->y,
      (state->tile->lcu_offset_x * LCU_WIDTH) + orig->x + mv->x,
      (state->tile->lcu_offset_y * LCU_WIDTH) + orig->y + mv->y,
      width, height, -1);
    cost += calc_mvd(state, mv->x, mv->y, 2, mv_cand, merge_cand, num_cand, ref_idx, &bitcost);

    if (cost < best_cost) {
      best_cost = cost;
      *best_index = i;
      *best_bitcost = bitcost;
    }
  }  
  if (*best_index < num_cand) {
    mv->x = merge_cand[*best_index].mv[merge_cand[*best_index].dir - 1][0] >> 2;
    mv->y = merge_cand[*best_index].mv[merge_cand[*best_index].dir - 1][1] >> 2;
  }
  else if (*best_index == num_cand) {
    mv->x = mv_in_out->x >> 2;
    mv->y = mv_in_out->y >> 2;
  }
  else {
    mv->x = 0;
    mv->y = 0;
  }
  return best_cost;
}

static uint32_t get_mvd_coding_cost(vector2d_t *mvd, cabac_data_t* cabac)
{
  uint32_t bitcost = 0;
  const int32_t mvd_hor = mvd->x;
  const int32_t mvd_ver = mvd->y;
  const int8_t hor_abs_gr0 = mvd_hor != 0;
  const int8_t ver_abs_gr0 = mvd_ver != 0;
  const uint32_t mvd_hor_abs = abs(mvd_hor);
  const uint32_t mvd_ver_abs = abs(mvd_ver);

  // Greater than 0 for x/y
  bitcost += 2;

  if (hor_abs_gr0) {
    if (mvd_hor_abs > 1) {
      bitcost += get_ep_ex_golomb_bitcost(mvd_hor_abs-2, 1) - 2; // TODO: tune the costs
    }
    // Greater than 1 + sign
    bitcost += 2;
  }

  if (ver_abs_gr0) {
    if (mvd_ver_abs > 1) {
      bitcost += get_ep_ex_golomb_bitcost(mvd_ver_abs-2, 1) - 2; // TODO: tune the costs
    }
    // Greater than 1 + sign
    bitcost += 2;
  }

  return bitcost;
}


static int calc_mvd_cost(const encoder_state_t * const state, int x, int y, int mv_shift,
                         int16_t mv_cand[2][2], inter_merge_cand_t merge_cand[MRG_MAX_NUM_CANDS],
                         int16_t num_cand,int32_t ref_idx, uint32_t *bitcost)
{
  uint32_t temp_bitcost = 0;
  uint32_t merge_idx;
  int cand1_cost,cand2_cost;
  vector2d_t mvd_temp1, mvd_temp2;
  int8_t merged      = 0;
  int8_t cur_mv_cand = 0;

  x <<= mv_shift;
  y <<= mv_shift;

  // Check every candidate to find a match
  for(merge_idx = 0; merge_idx < (uint32_t)num_cand; merge_idx++) {
    if (merge_cand[merge_idx].dir == 3) continue;
    if (merge_cand[merge_idx].mv[merge_cand[merge_idx].dir - 1][0] == x &&
        merge_cand[merge_idx].mv[merge_cand[merge_idx].dir - 1][1] == y &&
        merge_cand[merge_idx].ref[merge_cand[merge_idx].dir - 1] == ref_idx) {
      temp_bitcost += merge_idx;
      merged = 1;
      break;
    }
  }

  // Check mvd cost only if mv is not merged
  if(!merged) {
    mvd_temp1.x = x - mv_cand[0][0];
    mvd_temp1.y = y - mv_cand[0][1];
    cand1_cost = get_mvd_coding_cost(&mvd_temp1, NULL);

    mvd_temp2.x = x - mv_cand[1][0];
    mvd_temp2.y = y - mv_cand[1][1];
    cand2_cost = get_mvd_coding_cost(&mvd_temp2, NULL);

    // Select candidate 1 if it has lower cost
    if (cand2_cost < cand1_cost) {
      cur_mv_cand = 1;
    }
    temp_bitcost += cur_mv_cand ? cand2_cost : cand1_cost;
  }
  *bitcost = temp_bitcost;
  return temp_bitcost*(int32_t)(state->global->cur_lambda_cost_sqrt+0.5);
}


unsigned kvz_tz_pattern_search(const encoder_state_t * const state, const kvz_picture *pic, const kvz_picture *ref, unsigned pattern_type,
                           const vector2d_t *orig, const int iDist, vector2d_t *mv, unsigned best_cost, int *best_dist,
                           int16_t mv_cand[2][2], inter_merge_cand_t merge_cand[MRG_MAX_NUM_CANDS], int16_t num_cand, int32_t ref_idx, uint32_t *best_bitcost,
                           int width, int height, int wpp_limit)
{
  int n_points;
  int best_index = -1;
  int i;
  
  vector2d_t mv_best = { 0, 0 };


  int(*calc_mvd)(const encoder_state_t * const, int, int, int,
    int16_t[2][2], inter_merge_cand_t[MRG_MAX_NUM_CANDS],
    int16_t, int32_t, uint32_t *) = calc_mvd_cost;
  if (state->encoder_control->cfg->mv_rdo) {
    calc_mvd = kvz_calc_mvd_cost_cabac;
  }

  assert(pattern_type < 4);

  //implemented search patterns
  vector2d_t pattern[4][8] = {
      //diamond (8 points)
      //[ ][ ][ ][ ][1][ ][ ][ ][ ]
      //[ ][ ][ ][ ][ ][ ][ ][ ][ ]
      //[ ][ ][8][ ][ ][ ][5][ ][ ]
      //[ ][ ][ ][ ][ ][ ][ ][ ][ ]
      //[4][ ][ ][ ][o][ ][ ][ ][2]
      //[ ][ ][ ][ ][ ][ ][ ][ ][ ]
      //[ ][ ][7][ ][ ][ ][6][ ][ ]
      //[ ][ ][ ][ ][ ][ ][ ][ ][ ]
      //[ ][ ][ ][ ][3][ ][ ][ ][ ]
      {
        { 0, iDist }, { iDist, 0 }, { 0, -iDist }, { -iDist, 0 },
        { iDist / 2, iDist / 2 }, { iDist / 2, -iDist / 2 }, { -iDist / 2, -iDist / 2 }, { -iDist / 2, iDist / 2 }
      },

      //square (8 points)
      //[8][ ][ ][ ][1][ ][ ][ ][2]
      //[ ][ ][ ][ ][ ][ ][ ][ ][ ]
      //[ ][ ][ ][ ][ ][ ][ ][ ][ ]
      //[ ][ ][ ][ ][ ][ ][ ][ ][ ]
      //[7][ ][ ][ ][o][ ][ ][ ][3]
      //[ ][ ][ ][ ][ ][ ][ ][ ][ ]
      //[ ][ ][ ][ ][ ][ ][ ][ ][ ]
      //[ ][ ][ ][ ][ ][ ][ ][ ][ ]
      //[6][ ][ ][ ][5][ ][ ][ ][4]
      {
        { 0, iDist }, { iDist, iDist }, { iDist, 0 }, { iDist, -iDist }, { 0, -iDist },
        { -iDist, -iDist }, { -iDist, 0 }, { -iDist, iDist }
      },

      //octagon (8 points)
      //[ ][ ][5][ ][ ][ ][1][ ][ ]
      //[ ][ ][ ][ ][ ][ ][ ][ ][ ]
      //[ ][ ][ ][ ][ ][ ][ ][ ][2]
      //[4][ ][ ][ ][ ][ ][ ][ ][ ]
      //[ ][ ][ ][ ][o][ ][ ][ ][ ]
      //[ ][ ][ ][ ][ ][ ][ ][ ][ ]
      //[8][ ][ ][ ][ ][ ][ ][ ][6]
      //[ ][ ][ ][ ][ ][ ][ ][ ][ ]
      //[ ][ ][7][ ][ ][ ][3][ ][ ]
      {
        { iDist / 2, iDist }, { iDist, iDist / 2 }, { iDist / 2, -iDist }, { -iDist, iDist / 2 },
        { -iDist / 2, iDist }, { iDist, -iDist / 2 }, { -iDist / 2, -iDist }, { -iDist, -iDist / 2 }
      },

      //hexagon (6 points)
      //[ ][ ][5][ ][ ][ ][1][ ][ ]
      //[ ][ ][ ][ ][ ][ ][ ][ ][ ]
      //[ ][ ][ ][ ][ ][ ][ ][ ][ ]
      //[ ][ ][ ][ ][ ][ ][ ][ ][ ]
      //[4][ ][ ][ ][o][ ][ ][ ][2]
      //[ ][ ][ ][ ][ ][ ][ ][ ][ ]
      //[ ][ ][ ][ ][ ][ ][ ][ ][ ]
      //[ ][ ][ ][ ][ ][ ][ ][ ][ ]
      //[ ][ ][6][ ][ ][ ][3][ ][ ]
      {
        { iDist / 2, iDist }, { iDist, 0 }, { iDist / 2, -iDist }, { -iDist, 0 },
        { iDist / 2, iDist }, { -iDist / 2, -iDist }, { 0, 0 }, { 0, 0 }
      }

  };

  //set the number of points to be checked
  if (iDist == 1)
  {
    switch (pattern_type)
    {
      case 0:
        n_points = 4;
        break;
      case 2:
        n_points = 4;
        break;
      case 3:
        n_points = 4;
        break;
      default:
        n_points = 8;
        break;
    };
  }
  else
  {
    switch (pattern_type)
    {
      case 3:
        n_points = 6;
        break;
      default:
        n_points = 8;
        break;
    };
  }

  //compute SAD values for all chosen points
  for (i = 0; i < n_points; i++)
  {
    vector2d_t *current = &pattern[pattern_type][i];
    if (!intmv_within_tile(state, orig, mv->x + current->x, mv->y + current->y, width, height, wpp_limit)) {
      continue;
    }

    unsigned cost;
    uint32_t bitcost;

    {
      cost = kvz_image_calc_sad(pic, ref, orig->x, orig->y,
                            (state->tile->lcu_offset_x * LCU_WIDTH) + orig->x + mv->x + current->x,
                            (state->tile->lcu_offset_y * LCU_WIDTH) + orig->y + mv->y + current->y,
                            width, height, -1);
      cost += calc_mvd(state, mv->x + current->x, mv->y + current->y, 2, mv_cand, merge_cand, num_cand, ref_idx, &bitcost);
    }

    if (cost < best_cost)
    {
      best_cost = cost;
      *best_bitcost = bitcost;
      best_index = i;
    }

  }

  if (best_index >= 0)
  {
    mv_best = pattern[pattern_type][best_index];
    *best_dist = iDist;
  }
  
  mv->x += mv_best.x;
  mv->y += mv_best.y;

  return best_cost;

}


unsigned kvz_tz_raster_search(const encoder_state_t * const state, const kvz_picture *pic, const kvz_picture *ref,
                          const vector2d_t *orig, vector2d_t *mv, unsigned best_cost,
                          int16_t mv_cand[2][2], inter_merge_cand_t merge_cand[MRG_MAX_NUM_CANDS], int16_t num_cand, int32_t ref_idx, uint32_t *best_bitcost,
                          int width, int height, int iSearchRange, int iRaster, int wpp_limit)
{
  int i;
  int k;

  vector2d_t mv_best = { 0, 0 };

  int(*calc_mvd)(const encoder_state_t * const, int, int, int,
    int16_t[2][2], inter_merge_cand_t[MRG_MAX_NUM_CANDS],
    int16_t, int32_t, uint32_t *) = calc_mvd_cost;
  if (state->encoder_control->cfg->mv_rdo) {
    calc_mvd = kvz_calc_mvd_cost_cabac;
  }
  
  //compute SAD values for every point in the iRaster downsampled version of the current search area
  for (i = iSearchRange; i >= -iSearchRange; i -= iRaster)
  {
    for (k = -iSearchRange; k <= iSearchRange; k += iRaster)
    {
      vector2d_t current = { k, i };
      if (!intmv_within_tile(state, orig, mv->x + current.x, mv->y + current.y, width, height, wpp_limit)) {
        continue;
      }

      unsigned cost;
      uint32_t bitcost;

      {
        cost = kvz_image_calc_sad(pic, ref, orig->x, orig->y,
          (state->tile->lcu_offset_x * LCU_WIDTH) + orig->x + mv->x + k,
          (state->tile->lcu_offset_y * LCU_WIDTH) + orig->y + mv->y + i,
          width, height, -1);
        cost += calc_mvd(state, mv->x + k, mv->y + i, 2, mv_cand, merge_cand, num_cand, ref_idx, &bitcost);
      }

      if (cost < best_cost)
      {
        best_cost = cost;
        *best_bitcost = bitcost;
        mv_best = current;
      }

    }
  }
  
  mv->x += mv_best.x;
  mv->y += mv_best.y;

  return best_cost;

}


static unsigned tz_search(const encoder_state_t * const state,
                          unsigned width, unsigned height,
                          const kvz_picture *pic, const kvz_picture *ref,
                          const vector2d_t *orig, vector2d_t *mv_in_out,
                          int16_t mv_cand[2][2], inter_merge_cand_t merge_cand[MRG_MAX_NUM_CANDS],
                          int16_t num_cand, int32_t ref_idx, uint32_t *bitcost_out)
{

  //TZ parameters
  const int iSearchRange = 96;  // search range for each stage
  const int iRaster = 5;  // search distance limit and downsampling factor for step 3                   
  const unsigned step2_type = 0;  // search patterns for steps 2 and 4
  const unsigned step4_type = 0;
  const bool bRasterRefinementEnable = true;  // enable step 4 mode 1
  const bool bStarRefinementEnable = false;   // enable step 4 mode 2 (only one mode will be executed)

  vector2d_t mv = { mv_in_out->x >> 2, mv_in_out->y >> 2 };

  unsigned best_cost = UINT32_MAX;
  uint32_t best_bitcost = 0, bitcost;
  int iDist;
  int best_dist = 0;
  unsigned best_index = num_cand + 1;
  int wpp_limit = get_wpp_limit(state, orig);

  int(*calc_mvd)(const encoder_state_t * const, int, int, int,
    int16_t[2][2], inter_merge_cand_t[MRG_MAX_NUM_CANDS],
    int16_t, int32_t, uint32_t *) = calc_mvd_cost;
  if (state->encoder_control->cfg->mv_rdo) {
    calc_mvd = kvz_calc_mvd_cost_cabac;
  }

  // Check the 0-vector, so we can ignore all 0-vectors in the merge cand list.
  if (intmv_within_tile(state, orig, 0, 0, width, height, wpp_limit)) {
    best_cost = kvz_image_calc_sad(pic, ref, orig->x, orig->y,
                                   (state->tile->lcu_offset_x * LCU_WIDTH) + orig->x,
                                   (state->tile->lcu_offset_y * LCU_WIDTH) + orig->y,
                                   width, height, -1);
    best_cost += calc_mvd(state, 0, 0, 2, mv_cand, merge_cand, num_cand, ref_idx, &best_bitcost);
    best_index = num_cand + 1;
  }

  // Check mv_in if it's not one of the merge candidates.
  if (!mv_in_merge(merge_cand, num_cand, &mv) &&
      intmv_within_tile(state, orig, mv.x, mv.y, width, height, wpp_limit))
  {
    unsigned cost = kvz_image_calc_sad(pic, ref, orig->x, orig->y,
                                      (state->tile->lcu_offset_x * LCU_WIDTH) + orig->x + mv.x,
                                      (state->tile->lcu_offset_y * LCU_WIDTH) + orig->y + mv.y,
                                      width, height, -1);
    cost += calc_mvd(state, mv.x, mv.y, 2, mv_cand, merge_cand, num_cand, ref_idx, &bitcost);
    if (cost < best_cost) {
      best_cost = cost;
      best_index = num_cand;
      best_bitcost = bitcost;
    }
  }

  // Select starting point from among merge candidates. These should include
  // both mv_cand vectors and (0, 0).
  best_cost = select_starting_point(num_cand, merge_cand, mv_in_out, &mv, state, orig, width, height, wpp_limit,
    pic, ref, mv_cand, ref_idx, best_cost, &bitcost, &best_index, &best_bitcost, calc_mvd);

  //step 2, grid search
  for (iDist = 1; iDist <= iSearchRange; iDist *= 2)
  {
    best_cost = kvz_tz_pattern_search(state, pic, ref, step2_type, orig, iDist, &mv, best_cost, &best_dist,
                                  mv_cand, merge_cand, num_cand, ref_idx, &best_bitcost, width, height, wpp_limit);
  }

  //step 3, raster scan
  if (best_dist > iRaster)
  {
    best_dist = iRaster;

    best_cost = kvz_tz_raster_search(state, pic, ref, orig, &mv, best_cost, mv_cand, merge_cand,
                                 num_cand, ref_idx, &best_bitcost, width, height, iSearchRange, iRaster, wpp_limit);
  }

  //step 4

  //raster refinement
  if (bRasterRefinementEnable && best_dist > 0)
  {
    iDist = best_dist >> 1;
    while (iDist > 0)
    {
      best_cost = kvz_tz_pattern_search(state, pic, ref, step4_type, orig, iDist, &mv, best_cost, &best_dist,
                                   mv_cand, merge_cand, num_cand, ref_idx, &best_bitcost, width, height, wpp_limit);

      iDist = iDist >> 1;
    }
  }

  //star refinement (repeat step 2 for the current starting point)
  if (bStarRefinementEnable && best_dist > 0)
  {
    for (iDist = 1; iDist <= iSearchRange; iDist *= 2)
    {
      best_cost = kvz_tz_pattern_search(state, pic, ref, step4_type, orig, iDist, &mv, best_cost, &best_dist,
                                   mv_cand, merge_cand, num_cand, ref_idx, &best_bitcost, width, height, wpp_limit);
    }
  }

  mv.x = mv.x << 2;
  mv.y = mv.y << 2;

  *mv_in_out = mv;
  *bitcost_out = best_bitcost;

  return best_cost;
}


/**
 * \brief Do motion search using the HEXBS algorithm.
 *
 * \param width      width of the block to search
 * \param height     height of the block to search
 * \param pic        Picture motion vector is searched for.
 * \param ref        Picture motion vector is searched from.
 * \param orig       Top left corner of the searched for block.
 * \param mv_in_out  Predicted mv in and best out. Quarter pixel precision.
 *
 * \returns  Cost of the motion vector.
 *
 * Motion vector is searched by first searching iteratively with the large
 * hexagon pattern until the best match is at the center of the hexagon.
 * As a final step a smaller hexagon is used to check the adjacent pixels.
 *
 * If a non 0,0 predicted motion vector predictor is given as mv_in_out,
 * the 0,0 vector is also tried. This is hoped to help in the case where
 * the predicted motion vector is way off. In the future even more additional
 * points like 0,0 might be used, such as vectors from top or left.
 */
static unsigned hexagon_search(const encoder_state_t * const state,
  unsigned width, unsigned height,
  const kvz_picture *pic, const kvz_picture *ref,
  const vector2d_t *orig, vector2d_t *mv_in_out,
  int16_t mv_cand[2][2], inter_merge_cand_t merge_cand[MRG_MAX_NUM_CANDS],
  int16_t num_cand, int32_t ref_idx, uint32_t *bitcost_out)
{
  // The start of the hexagonal pattern has been repeated at the end so that
  // the indices between 1-6 can be used as the start of a 3-point list of new
  // points to search.
  //   6--1,7
  //  /     \    =)
  // 5   0  2,8
  //  \     /
  //   4---3
  static const vector2d_t large_hexbs[9] = {
          { 0, 0 },
          { 1, -2 }, { 2, 0 }, { 1, 2 }, { -1, 2 }, { -2, 0 }, { -1, -2 },
          { 1, -2 }, { 2, 0 }
  };
  // This is used as the last step of the hexagon search.
  //   1
  // 2 0 3
  //   4
  static const vector2d_t small_hexbs[5] = {
          { 0, 0 },
          { 0, -1 }, { -1, 0 }, { 1, 0 }, { 0, 1 }
  };

  vector2d_t mv = { mv_in_out->x >> 2, mv_in_out->y >> 2 };
  unsigned best_cost = UINT32_MAX, cost;
  uint32_t best_bitcost = 0, bitcost;
  unsigned i;
  // Current best index, either to merge_cands, large_hebx or small_hexbs.
  unsigned best_index = num_cand + 1;
  int wpp_limit = get_wpp_limit(state, orig);

  int(*calc_mvd)(const encoder_state_t * const, int, int, int,
      int16_t[2][2], inter_merge_cand_t[MRG_MAX_NUM_CANDS],
      int16_t, int32_t, uint32_t *) = calc_mvd_cost;
  if (state->encoder_control->cfg->mv_rdo) {
      calc_mvd = kvz_calc_mvd_cost_cabac;
  }

  // Check the 0-vector, so we can ignore all 0-vectors in the merge cand list.
  if (intmv_within_tile(state, orig, 0, 0, width, height, wpp_limit)) {
    best_cost = kvz_image_calc_sad(pic, ref, orig->x, orig->y,
                                    (state->tile->lcu_offset_x * LCU_WIDTH) + orig->x,
                                    (state->tile->lcu_offset_y * LCU_WIDTH) + orig->y,
                                      width, height, -1);
    best_cost += calc_mvd(state, 0, 0, 2, mv_cand, merge_cand, num_cand, ref_idx, &bitcost);
    best_bitcost = bitcost;
    best_index = num_cand + 1;
  }

  // Check mv_in if it's not one of the merge candidates.
  if (!mv_in_merge(merge_cand, num_cand, &mv) &&
      intmv_within_tile(state, orig, mv.x, mv.y, width, height, wpp_limit))
  {
    cost = kvz_image_calc_sad(pic, ref, orig->x, orig->y,
                              (state->tile->lcu_offset_x * LCU_WIDTH) + orig->x + mv.x,
                              (state->tile->lcu_offset_y * LCU_WIDTH) + orig->y + mv.y,
                                width, height, -1);
    cost += calc_mvd(state, mv.x, mv.y, 2, mv_cand, merge_cand, num_cand, ref_idx, &bitcost);
    if (cost < best_cost) {
      best_cost = cost;
      best_index = num_cand;
      best_bitcost = bitcost;
    }
  }

  // Select starting point from among merge candidates. These should include
  // both mv_cand vectors and (0, 0).
  best_cost = select_starting_point(num_cand, merge_cand, mv_in_out, &mv, state, orig, width, height, wpp_limit,
                                    pic, ref, mv_cand, ref_idx, best_cost, &bitcost, &best_index, &best_bitcost, calc_mvd);

  // Search the initial 7 points of the hexagon.
  best_index = 0;
  for (i = 0; i < 7; ++i) {
    const vector2d_t *pattern = &large_hexbs[i];
    if (!intmv_within_tile(state, orig, mv.x + pattern->x, mv.y + pattern->y, width, height, wpp_limit)) {
        continue;
    }

    cost = kvz_image_calc_sad(pic, ref, orig->x, orig->y,
                              (state->tile->lcu_offset_x * LCU_WIDTH) + orig->x + mv.x + pattern->x,
                              (state->tile->lcu_offset_y * LCU_WIDTH) + orig->y + mv.y + pattern->y,
                                width, height, -1);
    cost += calc_mvd(state, mv.x + pattern->x, mv.y + pattern->y, 2, mv_cand, merge_cand, num_cand, ref_idx, &bitcost);

    if (cost < best_cost) {
      best_cost = cost;
      best_index = i;
      best_bitcost = bitcost;
    }
  }

  // Iteratively search the 3 new points around the best match, until the best
  // match is in the center.
  while (best_index != 0) {
    // Starting point of the 3 offsets to be searched.
    unsigned start;
    if (best_index == 1) {
        start = 6;
    }
    else if (best_index == 8) {
        start = 1;
    }
    else {
        start = best_index - 1;
    }

    // Move the center to the best match.
    mv.x += large_hexbs[best_index].x;
    mv.y += large_hexbs[best_index].y;
    best_index = 0;

    // Iterate through the next 3 points.
    for (i = 0; i < 3; ++i) {
      const vector2d_t *offset = &large_hexbs[start + i];
      if (!intmv_within_tile(state, orig, mv.x + offset->x, mv.y + offset->y, width, height, wpp_limit)) {
          continue;
      }

      cost = kvz_image_calc_sad(pic, ref, orig->x, orig->y,
                                (state->tile->lcu_offset_x * LCU_WIDTH) + orig->x + mv.x + offset->x,
                                (state->tile->lcu_offset_y * LCU_WIDTH) + orig->y + mv.y + offset->y,
                                  width, height, -1);
      cost += calc_mvd(state, mv.x + offset->x, mv.y + offset->y, 2, mv_cand, merge_cand, num_cand, ref_idx, &bitcost);

      if (cost < best_cost) {
        best_cost = cost;
        best_index = start + i;
        best_bitcost = bitcost;
      }
    }
  }

  // Move the center to the best match.
  mv.x += large_hexbs[best_index].x;
  mv.y += large_hexbs[best_index].y;
  best_index = 0;

  // Do the final step of the search with a small pattern.
  for (i = 1; i < 5; ++i) {
    const vector2d_t *offset = &small_hexbs[i];
    if (!intmv_within_tile(state, orig, mv.x + offset->x, mv.y + offset->y, width, height, wpp_limit)) {
      continue;
    }

    cost = kvz_image_calc_sad(pic, ref, orig->x, orig->y,
                              (state->tile->lcu_offset_x * LCU_WIDTH) + orig->x + mv.x + offset->x,
                              (state->tile->lcu_offset_y * LCU_WIDTH) + orig->y + mv.y + offset->y,
                              width, height, -1);
    cost += calc_mvd(state, mv.x + offset->x, mv.y + offset->y, 2, mv_cand, merge_cand, num_cand, ref_idx, &bitcost);

    if (cost > 0 && cost < best_cost) {
      best_cost = cost;
      best_index = i;
      best_bitcost = bitcost;
    }
  }

  // Adjust the movement vector according to the final best match.
  mv.x += small_hexbs[best_index].x;
  mv.y += small_hexbs[best_index].y;

  // Return final movement vector in quarter-pixel precision.
  mv_in_out->x = mv.x << 2;
  mv_in_out->y = mv.y << 2;

  *bitcost_out = best_bitcost;

  return best_cost;
}


static unsigned search_mv_full(const encoder_state_t * const state,
                               unsigned width, unsigned height,
                               const kvz_picture *pic, const kvz_picture *ref,
                               const vector2d_t *orig, vector2d_t *mv_in_out,
                               int16_t mv_cand[2][2], inter_merge_cand_t merge_cand[MRG_MAX_NUM_CANDS],
                               int16_t num_cand, int32_t ref_idx, const int32_t search_range, uint32_t *bitcost_out)
{
  vector2d_t mv = { mv_in_out->x >> 2, mv_in_out->y >> 2 };
  vector2d_t best_mv = { 0, 0 };
  unsigned best_cost = UINT32_MAX;
  uint32_t best_bitcost = 0, bitcost;
  int wpp_limit = get_wpp_limit(state, orig);

  int (*calc_mvd)(const encoder_state_t * const, int, int, int,
                  int16_t[2][2], inter_merge_cand_t[MRG_MAX_NUM_CANDS],
                  int16_t, int32_t, uint32_t *) = calc_mvd_cost;
  if (state->encoder_control->cfg->mv_rdo) {
    calc_mvd = kvz_calc_mvd_cost_cabac;
  }

  // Check the 0-vector, so we can ignore all 0-vectors in the merge cand list.
  if (intmv_within_tile(state, orig, 0, 0, width, height, wpp_limit)) {
    vector2d_t min_mv = { 0 - search_range, 0 - search_range };
    vector2d_t max_mv = { 0 + search_range, 0 + search_range };

    for (int y = min_mv.y; y <= max_mv.y; ++y) {
      for (int x = min_mv.x; x <= max_mv.x; ++x) {
        if (!intmv_within_tile(state, orig, x, y, width, height, wpp_limit)) {
          continue;
        }
        unsigned cost = kvz_image_calc_sad(pic, ref, orig->x, orig->y,
                                           orig->x + x,
                                           orig->y + y,
                                           width, height, -1);
        cost += calc_mvd(state, x, y, 2, mv_cand, merge_cand, num_cand, ref_idx, &bitcost);
        if (cost < best_cost) {
          best_cost = cost;
          best_bitcost = bitcost;
          best_mv.x = x;
          best_mv.y = y;
        }
      }
    }
  }

  // Check mv_in if it's not one of the merge candidates.
  if (!mv_in_merge(merge_cand, num_cand, &mv) &&
      intmv_within_tile(state, orig, mv.x, mv.y, width, height, wpp_limit))
  {
    vector2d_t min_mv = { mv.x - search_range, mv.y - search_range };
    vector2d_t max_mv = { mv.x + search_range, mv.y + search_range };

    for (int y = min_mv.y; y <= max_mv.y; ++y) {
      for (int x = min_mv.x; x <= max_mv.x; ++x) {
        if (!intmv_within_tile(state, orig, x, y, width, height, wpp_limit)) {
          continue;
        }
        unsigned cost = kvz_image_calc_sad(pic, ref, orig->x, orig->y,
                                           orig->x + x,
                                           orig->y + y,
                                           width, height, -1);
        cost += calc_mvd(state, x, y, 2, mv_cand, merge_cand, num_cand, ref_idx, &bitcost);
        if (cost < best_cost) {
          best_cost = cost;
          best_bitcost = bitcost;
          mv.x = x;
          mv.y = y;
        }
      }
    }
  }

  // Select starting point from among merge candidates. These should include
  // both mv_cand vectors and (0, 0).
  for (int i = 0; i < num_cand; ++i) {
    if (merge_cand[i].dir == 3) continue;
    mv.x = merge_cand[i].mv[merge_cand[i].dir - 1][0] >> 2;
    mv.y = merge_cand[i].mv[merge_cand[i].dir - 1][1] >> 2;

    // Ignore 0-vector because it has already been checked.
    if (mv.x == 0 && mv.y == 0) continue;

    vector2d_t min_mv = { mv.x - search_range, mv.y - search_range };
    vector2d_t max_mv = { mv.x + search_range, mv.y + search_range };

    for (int y = min_mv.y; y <= max_mv.y; ++y) {
      for (int x = min_mv.x; x <= max_mv.x; ++x) {
        if (!intmv_within_tile(state, orig, x, y, width, height, wpp_limit)) {
          continue;
        }

        // Avoid calculating the same points over and over again.
        bool already_tested = false;
        for (int j = -1; j < i; ++j) {
          int xx = 0;
          int yy = 0;
          if (j >= 0) {
            if (merge_cand[j].dir == 3) continue;
            xx = merge_cand[j].mv[merge_cand[j].dir - 1][0] >> 2;
            yy = merge_cand[j].mv[merge_cand[j].dir - 1][1] >> 2;
          }
          if (x >= xx - search_range && x <= xx + search_range &&
              y >= yy - search_range && y <= yy + search_range)
          {
            already_tested = true;
            x = xx + search_range;
            break;
          }
        }
        if (already_tested) continue;

        unsigned cost = kvz_image_calc_sad(pic, ref, orig->x, orig->y,
                                           orig->x + x,
                                           orig->y + y,
                                           width, height, -1);
        cost += calc_mvd(state, x, y, 2, mv_cand, merge_cand, num_cand, ref_idx, &bitcost);
        if (cost < best_cost) {
          best_cost = cost;
          best_bitcost = bitcost;
          best_mv.x = x;
          best_mv.y = y;
        }
      }
    }
  }

  mv_in_out->x = best_mv.x << 2;
  mv_in_out->y = best_mv.y << 2;

  *bitcost_out = best_bitcost;

  return best_cost;
}


/**
 * \brief Do fractional motion estimation
 *
 * \param width      width of the block
 * \param height     height of the block
 * \param pic        Picture motion vector is searched for.
 * \param ref        Picture motion vector is searched from.
 * \param orig       Top left corner of the searched for block.
 * \param mv_in_out  Predicted mv in and best out. Quarter pixel precision.
 *
 * \returns  Cost of the motion vector.
 *
 * Algoritm first searches 1/2-pel positions around integer mv and after best match is found,
 * refines the search by searching best 1/4-pel postion around best 1/2-pel position.
 */
static unsigned search_frac(const encoder_state_t * const state,
                            unsigned width, unsigned height,
                            const kvz_picture *pic, const kvz_picture *ref,
                            const vector2d_t *orig, vector2d_t *mv_in_out,
                            int16_t mv_cand[2][2], inter_merge_cand_t merge_cand[MRG_MAX_NUM_CANDS],
                            int16_t num_cand, int32_t ref_idx, uint32_t *bitcost_out)
{
  // Map indexes to relative coordinates in the following way:
  // 6 7 8
  // 3 4 5
  // 0 1 2
  static const vector2d_t square[9] = {
      { -1, 1 },  { 0, 1 },  { 1, 1 },
      { -1, 0 },  { 0, 0 },  { 1, 0 },
      { -1, -1 }, { 0, -1 }, { 1, -1 }
  };

  int wpp_limit = get_wpp_limit(state, orig);

  //Set mv to halfpel precision
  vector2d_t mv = { mv_in_out->x >> 2, mv_in_out->y >> 2 };
  unsigned best_cost = UINT32_MAX;
  uint32_t best_bitcost = 0, bitcost;
  unsigned i;
  unsigned best_index = 4;

  unsigned cost = 0;

  vector2d_t halfpel_offset;

  #define FILTER_SIZE 8
  #define HALF_FILTER (FILTER_SIZE>>1)

  kvz_extended_block src = { 0, 0, 0 };

  //destination buffer for interpolation
  int dst_stride = (width + 1) * 4;
  kvz_pixel dst[(LCU_WIDTH+1) * (LCU_WIDTH+1) * 16];
  kvz_pixel* dst_off = &dst[dst_stride*4+4];

  int(*calc_mvd)(const encoder_state_t * const, int, int, int,
    int16_t[2][2], inter_merge_cand_t[MRG_MAX_NUM_CANDS],
    int16_t, int32_t, uint32_t *) = calc_mvd_cost;
  if (state->encoder_control->cfg->mv_rdo) {
    calc_mvd = kvz_calc_mvd_cost_cabac;
  }

  kvz_get_extended_block(orig->x, orig->y, mv.x-1, mv.y-1,
                state->tile->lcu_offset_x * LCU_WIDTH,
                state->tile->lcu_offset_y * LCU_WIDTH,
                ref->y, ref->width, ref->height, FILTER_SIZE, width+1, height+1, &src);

  kvz_filter_inter_quarterpel_luma(state->encoder_control, src.orig_topleft, src.stride, width+1,
      height+1, dst, dst_stride, 1, 1);

  if (src.malloc_used) free(src.buffer);

  //Set mv to half-pixel precision
  mv.x <<= 1;
  mv.y <<= 1;

  kvz_pixel tmp_filtered[LCU_WIDTH*LCU_WIDTH];
  kvz_pixel tmp_pic[LCU_WIDTH*LCU_WIDTH];
  kvz_pixels_blit(pic->y + orig->y*pic->width + orig->x, tmp_pic, width, height, pic->stride, width);

  // Search halfpel positions around best integer mv
  for (i = 0; i < 9; ++i) {
    const vector2d_t *pattern = &square[i];
    if (!fracmv_within_tile(state, orig, (mv.x + pattern->x) << 1, (mv.y + pattern->y) << 1, width, height, wpp_limit)) {
      continue;
    }

    int y,x;
    for(y = 0; y < height; ++y) {
      int dst_y = y*4+pattern->y*2;
      for(x = 0; x < width; ++x) {
        int dst_x = x*4+pattern->x*2;
        tmp_filtered[y*width+x] = dst_off[dst_y*dst_stride+dst_x];
      }
    }

    cost = kvz_satd_any_size(width, height,
                             tmp_pic, width,
                             tmp_filtered, width);

    cost += calc_mvd(state, mv.x + pattern->x, mv.y + pattern->y, 1, mv_cand, merge_cand, num_cand, ref_idx, &bitcost);
    if (cost < best_cost) {
      best_cost    = cost;
      best_index   = i;
      best_bitcost = bitcost;

    }
  }

  // Move search to best_index
  mv.x += square[best_index].x;
  mv.y += square[best_index].y;
  halfpel_offset.x = square[best_index].x*2;
  halfpel_offset.y = square[best_index].y*2;
  best_index = 4;

  //Set mv to quarterpel precision
  mv.x <<= 1;
  mv.y <<= 1;

  //Search quarterpel points around best halfpel mv
  for (i = 0; i < 9; ++i) {
    const vector2d_t *pattern = &square[i];
    if (!fracmv_within_tile(state, orig, mv.x + pattern->x, mv.y + pattern->y, width, height, wpp_limit)) {
      continue;
    }

    int y,x;
    for(y = 0; y < height; ++y) {
      int dst_y = y*4+halfpel_offset.y+pattern->y;
      for(x = 0; x < width; ++x) {
        int dst_x = x*4+halfpel_offset.x+pattern->x;
        tmp_filtered[y*width+x] = dst_off[dst_y*dst_stride+dst_x];
      }
    }

    cost = kvz_satd_any_size(width, height,
                             tmp_pic, width,
                             tmp_filtered, width);

    cost += calc_mvd(state, mv.x + pattern->x, mv.y + pattern->y, 0, mv_cand, merge_cand, num_cand, ref_idx, &bitcost);

    if (cost < best_cost) {
      best_cost    = cost;
      best_index   = i;
      best_bitcost = bitcost;
    }
  }

  //Set mv to best final best match
  mv.x += square[best_index].x;
  mv.y += square[best_index].y;

  mv_in_out->x = mv.x;
  mv_in_out->y = mv.y;

  *bitcost_out = best_bitcost;

  return best_cost;
}


/**
 * \brief Perform inter search for a single reference frame.
 */
static void search_pu_inter_ref(const encoder_state_t * const state,
                                int x, int y,
                                int width, int height,
                                int depth,
                                lcu_t *lcu, cu_info_t *cur_cu,
                                int16_t mv_cand[2][2],
                                inter_merge_cand_t merge_cand[MRG_MAX_NUM_CANDS],
                                int16_t num_cand,
                                unsigned ref_idx,
                                uint32_t(*get_mvd_cost)(vector2d_t *, cabac_data_t*))
{
  const int x_cu = x >> 3;
  const int y_cu = y >> 3;
  const videoframe_t * const frame = state->tile->frame;
  kvz_picture *ref_image = state->global->ref->images[ref_idx];
  uint32_t temp_bitcost = 0;
  uint32_t temp_cost = 0;
  vector2d_t orig, mvd;
  int32_t merged = 0;
  uint8_t cu_mv_cand = 0;
  int8_t merge_idx = 0;
  int8_t ref_list = state->global->refmap[ref_idx].list-1;
  int8_t temp_ref_idx = cur_cu->inter.mv_ref[ref_list];
  orig.x = x_cu * CU_MIN_SIZE_PIXELS;
  orig.y = y_cu * CU_MIN_SIZE_PIXELS;
  // Get MV candidates
  cur_cu->inter.mv_ref[ref_list] = ref_idx;
  kvz_inter_get_mv_cand(state, x, y, width, height, mv_cand, cur_cu, lcu, ref_list);
  cur_cu->inter.mv_ref[ref_list] = temp_ref_idx;


  vector2d_t mv = { 0, 0 };
  {
    // Take starting point for MV search from previous frame.
    // When temporal motion vector candidates are added, there is probably
    // no point to this anymore, but for now it helps.
    // TODO: Update this to work with SMP/AMP blocks.
    const vector2d_t frame_px = { 
        (state->tile->lcu_offset_x << LOG2_LCU_WIDTH) + x, 
        (state->tile->lcu_offset_y << LOG2_LCU_WIDTH) + y
    };
    const vector2d_t frame_cu = {
      (frame_px.x + (width >> 1)) >> MIN_SIZE,
      (frame_px.y + (height >> 1)) >> MIN_SIZE
    };
    const cu_info_t *ref_cu_array = state->global->ref->cu_arrays[ref_idx]->data;
    const int width_in_scu = frame->width_in_lcu << MAX_DEPTH;
    const cu_info_t *ref_cu = &ref_cu_array[frame_cu.x + frame_cu.y * width_in_scu];
    if (ref_cu->type == CU_INTER) {
      if (ref_cu->inter.mv_dir & 1) {
        mv.x = ref_cu->inter.mv[0][0];
        mv.y = ref_cu->inter.mv[0][1];
      } else {
        mv.x = ref_cu->inter.mv[1][0];
        mv.y = ref_cu->inter.mv[1][1];
      }
    }
  }

  int search_range = 32;
  switch (state->encoder_control->cfg->ime_algorithm) {
    case KVZ_IME_FULL64: search_range = 64; break;
    case KVZ_IME_FULL32: search_range = 32; break;
    case KVZ_IME_FULL16: search_range = 16; break;
    case KVZ_IME_FULL8: search_range = 8; break;
    default: break;
  }

  switch (state->encoder_control->cfg->ime_algorithm) {
    case KVZ_IME_TZ:
      temp_cost += tz_search(state,
                             width, height,
                             frame->source,
                             ref_image,
                             &orig,
                             &mv,
                             mv_cand,
                             merge_cand,
                             num_cand,
                             ref_idx,
                             &temp_bitcost);
      break;


    case KVZ_IME_FULL64:
    case KVZ_IME_FULL32:
    case KVZ_IME_FULL16:
    case KVZ_IME_FULL8:
    case KVZ_IME_FULL:
      temp_cost += search_mv_full(state,
                                  width, height,
                                  frame->source,
                                  ref_image,
                                  &orig,
                                  &mv,
                                  mv_cand,
                                  merge_cand,
                                  num_cand,
                                  ref_idx,
                                  search_range,
                                  &temp_bitcost);
      break;

    default:
      temp_cost += hexagon_search(state,
                                  width, height,
                                  frame->source,
                                  ref_image,
                                  &orig,
                                  &mv,
                                  mv_cand,
                                  merge_cand,
                                  num_cand,
                                  ref_idx,
                                  &temp_bitcost);
      break;
  }

  if (state->encoder_control->cfg->fme_level > 0) {
    temp_cost = search_frac(state,
                            width, height,
                            frame->source,
                            ref_image,
                            &orig,
                            &mv,
                            mv_cand,
                            merge_cand,
                            num_cand,
                            ref_idx,
                            &temp_bitcost);
  }
  
  merged = 0;
  // Check every candidate to find a match
  for(merge_idx = 0; merge_idx < num_cand; merge_idx++) {
    if (merge_cand[merge_idx].dir != 3 &&
        merge_cand[merge_idx].mv[merge_cand[merge_idx].dir - 1][0] == mv.x &&
        merge_cand[merge_idx].mv[merge_cand[merge_idx].dir - 1][1] == mv.y &&
        (uint32_t)merge_cand[merge_idx].ref[merge_cand[merge_idx].dir - 1] == ref_idx) {
      merged = 1;
      break;
    }
  }

  // Only check when candidates are different
  if (!merged && (mv_cand[0][0] != mv_cand[1][0] || mv_cand[0][1] != mv_cand[1][1])) {
    vector2d_t mvd_temp1, mvd_temp2;
    int cand1_cost,cand2_cost;

    mvd_temp1.x = mv.x - mv_cand[0][0];
    mvd_temp1.y = mv.y - mv_cand[0][1];
    cand1_cost = get_mvd_cost(&mvd_temp1, (cabac_data_t*)&state->cabac);

    mvd_temp2.x = mv.x - mv_cand[1][0];
    mvd_temp2.y = mv.y - mv_cand[1][1];
    cand2_cost = get_mvd_cost(&mvd_temp2, (cabac_data_t*)&state->cabac);

    // Select candidate 1 if it has lower cost
    if (cand2_cost < cand1_cost) {
      cu_mv_cand = 1;
    }
  }
  mvd.x = mv.x - mv_cand[cu_mv_cand][0];
  mvd.y = mv.y - mv_cand[cu_mv_cand][1];

  if(temp_cost < cur_cu->inter.cost) {

    // Map reference index to L0/L1 pictures
    cur_cu->inter.mv_dir = ref_list+1;
    cur_cu->inter.mv_ref_coded[ref_list] = state->global->refmap[ref_idx].idx;

    cur_cu->merged        = merged;
    cur_cu->merge_idx     = merge_idx;
    cur_cu->inter.mv_ref[ref_list] = ref_idx;
    cur_cu->inter.mv[ref_list][0] = (int16_t)mv.x;
    cur_cu->inter.mv[ref_list][1] = (int16_t)mv.y;
    cur_cu->inter.mvd[ref_list][0] = (int16_t)mvd.x;
    cur_cu->inter.mvd[ref_list][1] = (int16_t)mvd.y;
    cur_cu->inter.cost    = temp_cost;
    cur_cu->inter.bitcost = temp_bitcost + cur_cu->inter.mv_dir - 1 + cur_cu->inter.mv_ref_coded[ref_list];
    cur_cu->inter.mv_cand[ref_list] = cu_mv_cand;
  }
}


/**
 * \brief Update PU to have best modes at this depth.
 *
 * \param state       encoder state
 * \param x_cu        x-coordinate of the containing CU
 * \param y_cu        y-coordinate of the containing CU
 * \param depth       depth of the CU in the quadtree
 * \param part_mode   partition mode of the CU
 * \param i_pu        index of the PU in the CU
 * \param lcu         containing LCU
 *
 * \return            cost of the best mode
 */
static int search_pu_inter(const encoder_state_t * const state,
                           int x_cu, int y_cu,
                           int depth,
                           part_mode_t part_mode,
                           int i_pu,
                           lcu_t *lcu)
{
  const videoframe_t * const frame = state->tile->frame;
  const int width_cu  = LCU_WIDTH >> depth;
  const int x         = PU_GET_X(part_mode, width_cu, x_cu, i_pu);
  const int y         = PU_GET_Y(part_mode, width_cu, y_cu, i_pu);
  const int width     = PU_GET_W(part_mode, width_cu, i_pu);
  const int height    = PU_GET_H(part_mode, width_cu, i_pu);

  // Merge candidate A1 may not be used for the second PU of Nx2N, nLx2N and
  // nRx2N partitions.
  const bool merge_a1 = i_pu == 0 || width >= height;
  // Merge candidate B1 may not be used for the second PU of 2NxN, 2NxnU and
  // 2NxnD partitions.
  const bool merge_b1 = i_pu == 0 || width <= height;

  const int x_local   = SUB_SCU(x);
  const int y_local   = SUB_SCU(y);
  cu_info_t *cur_cu   = LCU_GET_CU_AT_PX(lcu, x_local, y_local);

  int16_t mv_cand[2][2];
  // Search for merge mode candidate
  inter_merge_cand_t merge_cand[MRG_MAX_NUM_CANDS];
  // Get list of candidates
  int16_t num_cand = kvz_inter_get_merge_cand(state,
                                              x, y,
                                              width, height,
                                              merge_a1, merge_b1,
                                              merge_cand,
                                              lcu);

  uint32_t(*get_mvd_cost)(vector2d_t *, cabac_data_t*) = get_mvd_coding_cost;
  if (state->encoder_control->cfg->mv_rdo) {
    get_mvd_cost = kvz_get_mvd_coding_cost_cabac;
  }

  int max_px_below_lcu = -1;
  if (state->encoder_control->owf) {
    max_px_below_lcu = LCU_WIDTH;
    if (state->encoder_control->fme_level > 0) {
      // Fractional motion estimation can change the mv by at most 1 pixel.
      max_px_below_lcu -= 1;
    }
    if (state->encoder_control->deblock_enable) {
      // Strong deblock filter modifies 3 pixels.
      max_px_below_lcu -= 3;
    }
  }

  // Default to candidate 0
  cur_cu->inter.mv_cand[0] = 0;
  cur_cu->inter.mv_cand[1] = 0;

  cur_cu->inter.cost = INT_MAX;

  uint32_t ref_idx;
  for (ref_idx = 0; ref_idx < state->global->ref->used_size; ref_idx++) {
    search_pu_inter_ref(state,
                        x, y,
                        width, height,
                        depth,
                        lcu, cur_cu,
                        mv_cand, merge_cand, num_cand,
                        ref_idx,
                        get_mvd_cost);
  }

  // Search bi-pred positions
  if (state->global->slicetype == KVZ_SLICE_B && state->encoder_control->cfg->bipred) {
    lcu_t *templcu = MALLOC(lcu_t, 1);
    unsigned cu_width = LCU_WIDTH >> depth;
    #define NUM_PRIORITY_LIST 12;
    static const uint8_t priorityList0[] = { 0, 1, 0, 2, 1, 2, 0, 3, 1, 3, 2, 3 };
    static const uint8_t priorityList1[] = { 1, 0, 2, 0, 2, 1, 3, 0, 3, 1, 3, 2 };
    uint8_t cutoff = num_cand;


    int(*calc_mvd)(const encoder_state_t * const, int, int, int,
      int16_t[2][2], inter_merge_cand_t[MRG_MAX_NUM_CANDS],
      int16_t, int32_t, uint32_t *) = calc_mvd_cost;
    if (state->encoder_control->cfg->mv_rdo) {
      calc_mvd = kvz_calc_mvd_cost_cabac;
    }

    for (int32_t idx = 0; idx<cutoff*(cutoff - 1); idx++) {
      uint8_t i = priorityList0[idx];
      uint8_t j = priorityList1[idx];
      if (i >= num_cand || j >= num_cand) break;

      // Find one L0 and L1 candidate according to the priority list
      if ((merge_cand[i].dir & 0x1) && (merge_cand[j].dir & 0x2)) {
        if (merge_cand[i].ref[0] != merge_cand[j].ref[1] ||
          merge_cand[i].mv[0][0] != merge_cand[j].mv[1][0] ||
          merge_cand[i].mv[0][1] != merge_cand[j].mv[1][1]) {
          uint32_t bitcost[2];
          uint32_t cost = 0;
          int8_t cu_mv_cand = 0;
          int16_t mv[2][2];
          kvz_pixel tmp_block[64 * 64];
          kvz_pixel tmp_pic[64 * 64];
          // Force L0 and L1 references
          if (state->global->refmap[merge_cand[i].ref[0]].list == 2 || state->global->refmap[merge_cand[j].ref[1]].list == 1) continue;

          mv[0][0] = merge_cand[i].mv[0][0];
          mv[0][1] = merge_cand[i].mv[0][1];
          mv[1][0] = merge_cand[j].mv[1][0];
          mv[1][1] = merge_cand[j].mv[1][1];

          {
            // Don't try merge candidates that don't satisfy mv constraints.
            vector2d_t orig = { x, y };
            if (fracmv_within_tile(state, &orig, mv[0][0], mv[0][1], width, height, -1) ||
                fracmv_within_tile(state, &orig, mv[1][0], mv[1][1], width, height, -1))
            {
              continue;
            }
          }

          kvz_inter_recon_lcu_bipred(state,
                                     state->global->ref->images[merge_cand[i].ref[0]],
                                     state->global->ref->images[merge_cand[j].ref[1]],
                                     x, y,
                                     width,
                                     height,
                                     mv,
                                     templcu);

          for (int ypos = 0; ypos < height; ++ypos) {
            int dst_y = ypos * width;
            for (int xpos = 0; xpos < width; ++xpos) {
              tmp_block[dst_y + xpos] = templcu->rec.y[
                SUB_SCU(y + ypos) * LCU_WIDTH + SUB_SCU(x + xpos)];
              tmp_pic[dst_y + xpos] = frame->source->y[x + xpos + (y + ypos)*frame->source->width];
            }
          }

          cost = kvz_satd_any_size(cu_width, cu_width, tmp_pic, cu_width, tmp_block, cu_width);

          cost += calc_mvd(state, merge_cand[i].mv[0][0], merge_cand[i].mv[0][1], 0, mv_cand, merge_cand, 0, ref_idx, &bitcost[0]);
          cost += calc_mvd(state, merge_cand[i].mv[1][0], merge_cand[i].mv[1][1], 0, mv_cand, merge_cand, 0, ref_idx, &bitcost[1]);

          if (cost < cur_cu->inter.cost) {

            cur_cu->inter.mv_dir = 3;
            cur_cu->inter.mv_ref_coded[0] = state->global->refmap[merge_cand[i].ref[0]].idx;
            cur_cu->inter.mv_ref_coded[1] = state->global->refmap[merge_cand[j].ref[1]].idx;



            cur_cu->inter.mv_ref[0] = merge_cand[i].ref[0];
            cur_cu->inter.mv_ref[1] = merge_cand[j].ref[1];

            cur_cu->inter.mv[0][0] = merge_cand[i].mv[0][0];
            cur_cu->inter.mv[0][1] = merge_cand[i].mv[0][1];
            cur_cu->inter.mv[1][0] = merge_cand[j].mv[1][0];
            cur_cu->inter.mv[1][1] = merge_cand[j].mv[1][1];
            cur_cu->merged = 0;
                        
            // Check every candidate to find a match
            for(int merge_idx = 0; merge_idx < num_cand; merge_idx++) {
              if (
                  merge_cand[merge_idx].mv[0][0] == cur_cu->inter.mv[0][0] &&
                  merge_cand[merge_idx].mv[0][1] == cur_cu->inter.mv[0][1] &&     
                  merge_cand[merge_idx].mv[1][0] == cur_cu->inter.mv[1][0] &&
                  merge_cand[merge_idx].mv[1][1] == cur_cu->inter.mv[1][1] &&    
                  merge_cand[merge_idx].ref[0] == cur_cu->inter.mv_ref[0] && 
                  merge_cand[merge_idx].ref[1] == cur_cu->inter.mv_ref[1]) {
                cur_cu->merged = 1;
                cur_cu->merge_idx = merge_idx;
                break;
              }
            }

            // Each motion vector has its own candidate
            for (int reflist = 0; reflist < 2; reflist++) {
              cu_mv_cand = 0;
              kvz_inter_get_mv_cand(state, x, y, width, height, mv_cand, cur_cu, lcu, reflist);
              if ((mv_cand[0][0] != mv_cand[1][0] || mv_cand[0][1] != mv_cand[1][1])) {
                vector2d_t mvd_temp1, mvd_temp2;
                int cand1_cost, cand2_cost;

                mvd_temp1.x = cur_cu->inter.mv[reflist][0] - mv_cand[0][0];
                mvd_temp1.y = cur_cu->inter.mv[reflist][1] - mv_cand[0][1];
                cand1_cost = get_mvd_cost(&mvd_temp1, (cabac_data_t*)&state->cabac);

                mvd_temp2.x = cur_cu->inter.mv[reflist][0] - mv_cand[1][0];
                mvd_temp2.y = cur_cu->inter.mv[reflist][1] - mv_cand[1][1];
                cand2_cost = get_mvd_cost(&mvd_temp2, (cabac_data_t*)&state->cabac);

                // Select candidate 1 if it has lower cost
                if (cand2_cost < cand1_cost) {
                  cu_mv_cand = 1;                  
                }
              }
              cur_cu->inter.mvd[reflist][0] = cur_cu->inter.mv[reflist][0] - mv_cand[cu_mv_cand][0];
              cur_cu->inter.mvd[reflist][1] = cur_cu->inter.mv[reflist][1] - mv_cand[cu_mv_cand][1];
              cur_cu->inter.mv_cand[reflist] = cu_mv_cand;
            }
            cur_cu->inter.cost = cost;
            cur_cu->inter.bitcost = bitcost[0] + bitcost[1] + cur_cu->inter.mv_dir - 1 + cur_cu->inter.mv_ref_coded[0] + cur_cu->inter.mv_ref_coded[1];
          }
        }
      }
    }
    FREE_POINTER(templcu);
  }

  if (cur_cu->inter.cost < INT_MAX) {
    const vector2d_t orig = { x, y };
    if (cur_cu->inter.mv_dir == 1) {
      assert(fracmv_within_tile(state, &orig, cur_cu->inter.mv[0][0], cur_cu->inter.mv[0][1], width, height, -1));
    }
  }

  return cur_cu->inter.cost;
}


/**
 * \brief Update CU to have best modes at this depth.
 *
 * Only searches the 2Nx2N partition mode.
 *
 * \param state       encoder state
 * \param x           x-coordinate of the CU
 * \param y           y-coordinate of the CU
 * \param depth       depth of the CU in the quadtree
 * \param lcu         containing LCU
 *
 * \return            cost of the best mode
 */
int kvz_search_cu_inter(const encoder_state_t * const state, int x, int y, int depth, lcu_t *lcu)
{
  return search_pu_inter(state, x, y, depth, SIZE_2Nx2N, 0, lcu);
}


/**
 * \brief Update CU to have best modes at this depth.
 *
 * Only searches the given partition mode.
 *
 * \param state       encoder state
 * \param x           x-coordinate of the CU
 * \param y           y-coordinate of the CU
 * \param depth       depth of the CU in the quadtree
 * \param part_mode   partition mode to search
 * \param lcu         containing LCU
 *
 * \return            cost of the best mode
 */
int kvz_search_cu_smp(const encoder_state_t * const state,
                      int x, int y,
                      int depth,
                      part_mode_t part_mode,
                      lcu_t *lcu)
{
  const int num_pu    = kvz_part_mode_num_parts[part_mode];
  const int width_scu = (LCU_WIDTH >> depth) >> MAX_DEPTH;
  const int y_scu     = SUB_SCU(y) >> MAX_DEPTH;
  const int x_scu     = SUB_SCU(x) >> MAX_DEPTH;

  int cost = 0;
  for (int i = 0; i < num_pu; ++i) {
    const int x_pu      = PU_GET_X(part_mode, width_scu, x_scu, i);
    const int y_pu      = PU_GET_Y(part_mode, width_scu, y_scu, i);
    const int width_pu  = PU_GET_W(part_mode, width_scu, i);
    const int height_pu = PU_GET_H(part_mode, width_scu, i);
    cu_info_t *cur_pu   = LCU_GET_CU(lcu, x_pu, y_pu);

    cur_pu->type = CU_INTER;
    cur_pu->part_size = part_mode;
    cur_pu->depth = depth;

    cost += search_pu_inter(state, x, y, depth, part_mode, i, lcu);

    for (int y = y_pu; y < y_pu + height_pu; ++y) {
      for (int x = x_pu; x < x_pu + width_pu; ++x) {
        cu_info_t *scu = LCU_GET_CU(lcu, x, y);
        scu->type = CU_INTER;
        memcpy(&scu->inter, &cur_pu->inter, sizeof(cur_pu->inter));
      }
    }
  }

  return cost;
}
