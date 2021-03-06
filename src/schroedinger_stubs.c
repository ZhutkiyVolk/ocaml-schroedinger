/*
  Copyright 2003-2009 Savonet team

  This file is part of Ocaml-schroedinger.

  Ocaml-schroedinger is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  Ocaml-schroedinger is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Ocaml-schroedinger; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/* The code here is inspired/stolen from ffmpeg2dirac */

#include <caml/custom.h>
#include <caml/fail.h>
#include <caml/memory.h>
#include <caml/bigarray.h>
#include <caml/alloc.h>
#include <caml/callback.h>
#include <caml/signals.h>

#include <ogg/ogg.h>
#include <ocaml-ogg.h>

#include <string.h>

#include <schroedinger/schro.h>
#include <schroedinger/schroencoder.h>
#include <schroedinger/schrodecoder.h>

#define ROUND_UP_SHIFT(x,y) (((x) + (1<<(y)) - 1)>>(y))

/* Common */

static inline SchroFrameFormat schro_frame_format_of_chroma_format(SchroChromaFormat format)
{
  switch (format) {
    case SCHRO_CHROMA_444:
      return SCHRO_FRAME_FORMAT_U8_444;
    case SCHRO_CHROMA_422:
      return SCHRO_FRAME_FORMAT_U8_422;
    case SCHRO_CHROMA_420:
      return SCHRO_FRAME_FORMAT_U8_420;
    default:
      caml_failwith("invalid value");
  }
}

static SchroBuffer *schro_buffer_of_ogg_packet(ogg_packet *op)
{
  SchroBuffer *buffer = schro_buffer_new_and_alloc(op->bytes);
  memcpy(buffer->data, op->packet, op->bytes);
  return buffer;
}

void frame_planar_free(SchroFrame *frame, void *private)
{
  free(frame->components[0].data);
  free(frame->components[1].data);
  free(frame->components[2].data);
}

static SchroFrame *schro_frame_of_val(value v)
{
  SchroFrame *frame = schro_frame_new();
  if (frame == NULL)
    caml_raise_out_of_memory();
  int i = 0;
  int j = 0;
  int h_shift;
  int v_shift;
  int len;
  int stride;
  value plane;
  value planes;
  struct caml_ba_array *data;
  void *tmp;

  planes = Field(v, i++);
  /* Get params */
  frame->width = Int_val(Field(v, i++));
  frame->height = Int_val(Field(v, i++));
  frame->format = Int_val(Field(v, i++));

  /* Get data */

  h_shift = SCHRO_FRAME_FORMAT_H_SHIFT(frame->format);
  v_shift = SCHRO_FRAME_FORMAT_V_SHIFT(frame->format);
  
  /* First plane */
  plane = Field(planes, j++);
  data = Caml_ba_array_val(Field(plane,0));
  stride = Int_val(Field(plane,1));
  len = stride*frame->height;
  if (stride < frame->width || 
      (int)data->dim[0] != len)
    caml_failwith("invalid frame dimension");
  tmp = malloc(len);
  if (tmp == NULL)
    caml_raise_out_of_memory();
  memcpy(tmp,data->data,len);
  frame->components[0].format = frame->format;
  frame->components[0].data = tmp;
  frame->components[0].stride = stride;
  frame->components[0].width = frame->width;
  frame->components[0].height = frame->height;
  frame->components[0].length = len;
  frame->components[0].h_shift = 0;
  frame->components[0].v_shift = 0;

  /* Secondary planes */
  plane = Field(planes, j++);
  data = Caml_ba_array_val(Field(plane,0));
  stride = Int_val(Field(plane,1));
  len = stride*(ROUND_UP_SHIFT(frame->height, v_shift));
  if (stride < ROUND_UP_SHIFT(frame->width, h_shift) ||
      (int)data->dim[0] != len)
    caml_failwith("invalid frame dimension");
  tmp = malloc(len);
  if (tmp == NULL)
    caml_raise_out_of_memory();
  memcpy(tmp,data->data,len);
  frame->components[1].format = frame->format;
  frame->components[1].data = tmp;
  frame->components[1].stride = stride;
  frame->components[1].width = ROUND_UP_SHIFT(frame->width, h_shift);
  frame->components[1].height = ROUND_UP_SHIFT(frame->height, v_shift);
  frame->components[1].length = len;
  frame->components[1].h_shift = h_shift;
  frame->components[1].v_shift = v_shift;

  plane = Field(planes, j++);
  data = Caml_ba_array_val(Field(plane,0));
  stride = Int_val(Field(plane,1));
  len = stride*(ROUND_UP_SHIFT(frame->height, v_shift));
  if (stride < (ROUND_UP_SHIFT(frame->width, h_shift)) ||
      (int)data->dim[0] != len)
    caml_failwith("invalid frame dimension");
  tmp = malloc(len);
  if (tmp == NULL)
    caml_raise_out_of_memory();
  memcpy(tmp,data->data,len);
  frame->components[2].format = frame->format;
  frame->components[2].data = tmp;
  frame->components[2].stride = stride;
  frame->components[2].width = ROUND_UP_SHIFT(frame->width, h_shift);
  frame->components[2].height = ROUND_UP_SHIFT(frame->height, v_shift);
  frame->components[2].length = len;
  frame->components[2].h_shift = h_shift;
  frame->components[2].v_shift = v_shift;

  schro_frame_set_free_callback(frame,frame_planar_free,NULL);

  return frame;
}

static SchroFrame *schro_frame_alloc(SchroFrameFormat format, int width, int height)
{
  SchroFrame *frame = schro_frame_new();
  if (frame == NULL)
    caml_raise_out_of_memory();
  int h_shift;
  int v_shift;
  int len;
  int stride;
  char *tmp;

  /* Set params */
  frame->width = width;
  frame->height = height;
  frame->format = format;

  /* Get data */

  h_shift = SCHRO_FRAME_FORMAT_H_SHIFT(frame->format);
  v_shift = SCHRO_FRAME_FORMAT_V_SHIFT(frame->format);
  
  /* First plane */
  stride = frame->width;
  len = stride*frame->height;
  tmp = malloc(len);
  if (tmp == NULL)
    caml_raise_out_of_memory();
  frame->components[0].format = format;
  frame->components[0].data = tmp;
  frame->components[0].stride = stride;
  frame->components[0].width = frame->width;
  frame->components[0].height = frame->height;
  frame->components[0].length = len;
  frame->components[0].h_shift = 0;
  frame->components[0].v_shift = 0;

  /* Secondary planes */
  stride = ROUND_UP_SHIFT(frame->width, h_shift);
  len = stride*(ROUND_UP_SHIFT(frame->height, v_shift));
  tmp = malloc(len);
  if (tmp == NULL)
    caml_raise_out_of_memory();
  frame->components[1].format = format;
  frame->components[1].data = tmp;
  frame->components[1].stride = stride;
  frame->components[1].width = ROUND_UP_SHIFT(frame->width, h_shift);
  frame->components[1].height = ROUND_UP_SHIFT(frame->height, v_shift);
  frame->components[1].length = len;
  frame->components[1].h_shift = h_shift;
  frame->components[1].v_shift = v_shift;

  stride = ROUND_UP_SHIFT(frame->width, h_shift);
  len = stride*(ROUND_UP_SHIFT(frame->height, v_shift));
  tmp = malloc(len);
  if (tmp == NULL)
    caml_raise_out_of_memory();
  frame->components[2].format = format;
  frame->components[2].data = tmp;
  frame->components[2].stride = stride;
  frame->components[2].width = ROUND_UP_SHIFT(frame->width, h_shift);
  frame->components[2].height = ROUND_UP_SHIFT(frame->height, v_shift);
  frame->components[2].length = len;
  frame->components[2].h_shift = h_shift;
  frame->components[2].v_shift = v_shift;

  schro_frame_set_free_callback(frame,frame_planar_free,NULL);

  return frame;
}

static value val_of_schro_frame(SchroFrame *frame)
{
  CAMLparam0();
  CAMLlocal4(ret, planes, plane, data);
  intnat len;
  void *tmp;
  int j;

  /* Init result and planes array */
  ret = caml_alloc_tuple(4);
  planes = caml_alloc_tuple(3);
  /* Store planes array */
  Store_field (ret, 0, planes);
  /* Store frame infos */
  Store_field (ret, 1, Val_int(frame->width));
  Store_field (ret, 2, Val_int(frame->height));
  Store_field (ret, 3, Val_int(frame->format));

  /* Store data */
  for (j=0; j<3; j++) {
    len = frame->components[j].stride*frame->components[j].height;
    tmp = malloc(len);
    if (tmp == NULL)
      caml_raise_out_of_memory();
    memcpy(tmp,frame->components[j].data,len);
    data = caml_ba_alloc(CAML_BA_MANAGED | CAML_BA_C_LAYOUT | CAML_BA_UINT8, 1, tmp, &len);
    plane = caml_alloc_tuple(2);
    Store_field(plane, 0, data);
    Store_field(plane, 1, Val_int(frame->components[j].stride));
    Store_field(planes, j, plane);
  }

  CAMLreturn(ret);
}

CAMLprim value caml_schroedinger_init(value unit)
{
  CAMLparam0();
  schro_init();
  CAMLreturn(Val_unit);
}

CAMLprim value ocaml_schroedinger_int_of_define(value v)
{
  CAMLparam1(v);
  char *s = String_val(v);
  if (!strcmp(s,"SCHRO_FRAME_FORMAT_U8_444"))
    CAMLreturn(Val_int(SCHRO_FRAME_FORMAT_U8_444)) ;
  if (!strcmp(s,"SCHRO_FRAME_FORMAT_U8_422"))
    CAMLreturn(Val_int(SCHRO_FRAME_FORMAT_U8_422)) ;
  if (!strcmp(s,"SCHRO_FRAME_FORMAT_U8_420"))
    CAMLreturn(Val_int(SCHRO_FRAME_FORMAT_U8_420)) ;
  if (!strcmp(s,"SCHRO_VIDEO_FORMAT_CUSTOM"))
    CAMLreturn(Val_int(SCHRO_VIDEO_FORMAT_CUSTOM)) ;
  if (!strcmp(s,"SCHRO_VIDEO_FORMAT_QSIF"))
    CAMLreturn(Val_int(SCHRO_VIDEO_FORMAT_QSIF)) ;
  if (!strcmp(s,"SCHRO_VIDEO_FORMAT_QCIF"))
    CAMLreturn(Val_int(SCHRO_VIDEO_FORMAT_QCIF)) ;
  if (!strcmp(s,"SCHRO_VIDEO_FORMAT_SIF"))
    CAMLreturn(Val_int(SCHRO_VIDEO_FORMAT_SIF)) ;
  if (!strcmp(s,"SCHRO_VIDEO_FORMAT_CIF"))
    CAMLreturn(Val_int(SCHRO_VIDEO_FORMAT_CIF)) ;
  if (!strcmp(s,"SCHRO_VIDEO_FORMAT_4SIF"))
    CAMLreturn(Val_int(SCHRO_VIDEO_FORMAT_4SIF)) ;
  if (!strcmp(s,"SCHRO_VIDEO_FORMAT_4CIF"))
    CAMLreturn(Val_int(SCHRO_VIDEO_FORMAT_4CIF)) ;
  if (!strcmp(s,"SCHRO_VIDEO_FORMAT_SD480I_60"))
    CAMLreturn(Val_int(SCHRO_VIDEO_FORMAT_SD480I_60)) ;
  if (!strcmp(s,"SCHRO_VIDEO_FORMAT_SD576I_50"))
    CAMLreturn(Val_int(SCHRO_VIDEO_FORMAT_SD576I_50)) ;
  if (!strcmp(s,"SCHRO_VIDEO_FORMAT_HD720P_60"))
    CAMLreturn(Val_int(SCHRO_VIDEO_FORMAT_HD720P_60)) ;
  if (!strcmp(s,"SCHRO_VIDEO_FORMAT_HD720P_50"))
    CAMLreturn(Val_int(SCHRO_VIDEO_FORMAT_HD720P_50)) ;
  if (!strcmp(s,"SCHRO_VIDEO_FORMAT_HD1080I_60"))
    CAMLreturn(Val_int(SCHRO_VIDEO_FORMAT_HD1080I_60)) ;
  if (!strcmp(s,"SCHRO_VIDEO_FORMAT_HD1080I_50"))
    CAMLreturn(Val_int(SCHRO_VIDEO_FORMAT_HD1080I_50)) ;
  if (!strcmp(s,"SCHRO_VIDEO_FORMAT_HD1080P_60"))
    CAMLreturn(Val_int(SCHRO_VIDEO_FORMAT_HD1080P_60)) ;
  if (!strcmp(s,"SCHRO_VIDEO_FORMAT_HD1080P_50"))
    CAMLreturn(Val_int(SCHRO_VIDEO_FORMAT_HD1080P_50)) ;
  if (!strcmp(s,"SCHRO_VIDEO_FORMAT_DC2K_24"))
    CAMLreturn(Val_int(SCHRO_VIDEO_FORMAT_DC2K_24)) ;
  if (!strcmp(s,"SCHRO_VIDEO_FORMAT_DC4K_24"))
    CAMLreturn(Val_int(SCHRO_VIDEO_FORMAT_DC4K_24)) ;
  if (!strcmp(s,"SCHRO_CHROMA_444"))
    CAMLreturn(Val_int(SCHRO_CHROMA_444)) ;
  if (!strcmp(s,"SCHRO_CHROMA_422"))
    CAMLreturn(Val_int(SCHRO_CHROMA_422)) ;
  if (!strcmp(s,"SCHRO_CHROMA_420"))
    CAMLreturn(Val_int(SCHRO_CHROMA_420)) ;
  if (!strcmp(s,"SCHRO_COLOUR_PRIMARY_HDTV"))
    CAMLreturn(Val_int(SCHRO_COLOUR_PRIMARY_HDTV)) ;
  if (!strcmp(s,"SCHRO_COLOUR_PRIMARY_SDTV_525"))
    CAMLreturn(Val_int(SCHRO_COLOUR_PRIMARY_SDTV_525)) ;
  if (!strcmp(s,"SCHRO_COLOUR_PRIMARY_SDTV_625"))
    CAMLreturn(Val_int(SCHRO_COLOUR_PRIMARY_SDTV_625)) ;
  if (!strcmp(s,"SCHRO_COLOUR_PRIMARY_CINEMA"))
    CAMLreturn(Val_int(SCHRO_COLOUR_PRIMARY_CINEMA)) ;
  if (!strcmp(s,"SCHRO_COLOUR_MATRIX_HDTV"))
    CAMLreturn(Val_int(SCHRO_COLOUR_MATRIX_HDTV)) ;
  if (!strcmp(s,"SCHRO_COLOUR_MATRIX_SDTV"))
    CAMLreturn(Val_int(SCHRO_COLOUR_MATRIX_SDTV)) ;
  if (!strcmp(s,"SCHRO_COLOUR_MATRIX_REVERSIBLE"))
    CAMLreturn(Val_int(SCHRO_COLOUR_MATRIX_REVERSIBLE)) ;
  if (!strcmp(s,"SCHRO_TRANSFER_CHAR_TV_GAMMA"))
    CAMLreturn(Val_int(SCHRO_TRANSFER_CHAR_TV_GAMMA)) ;
  if (!strcmp(s,"SCHRO_TRANSFER_CHAR_EXTENDED_GAMUT"))
    CAMLreturn(Val_int(SCHRO_TRANSFER_CHAR_EXTENDED_GAMUT)) ;
  if (!strcmp(s,"SCHRO_TRANSFER_CHAR_LINEAR"))
    CAMLreturn(Val_int(SCHRO_TRANSFER_CHAR_LINEAR)) ;
  if (!strcmp(s,"SCHRO_TRANSFER_CHAR_DCI_GAMMA"))
    CAMLreturn(Val_int(SCHRO_TRANSFER_CHAR_DCI_GAMMA)) ;
  if (!strcmp(s,"SCHRO_SIGNAL_RANGE_CUSTOM"))
    CAMLreturn(Val_int(SCHRO_SIGNAL_RANGE_CUSTOM)) ;
  if (!strcmp(s,"SCHRO_SIGNAL_RANGE_8BIT_FULL"))
    CAMLreturn(Val_int(SCHRO_SIGNAL_RANGE_8BIT_FULL)) ;
  if (!strcmp(s,"SCHRO_SIGNAL_RANGE_8BIT_VIDEO"))
    CAMLreturn(Val_int(SCHRO_SIGNAL_RANGE_8BIT_VIDEO)) ;
  if (!strcmp(s,"SCHRO_SIGNAL_RANGE_10BIT_VIDEO"))
    CAMLreturn(Val_int(SCHRO_SIGNAL_RANGE_10BIT_VIDEO)) ;
  if (!strcmp(s,"SCHRO_SIGNAL_RANGE_12BIT_VIDEO"))
    CAMLreturn(Val_int(SCHRO_SIGNAL_RANGE_12BIT_VIDEO)) ;


  caml_failwith("unknown value");
}

/* Takes internal_video_format */
static SchroVideoFormat *schro_video_format_of_val(value v, SchroVideoFormat *format)
{
  int i = 0;
  format->index = Int_val(Field(v, i++));
  format->width = Int_val(Field(v, i++));
  format->height = Int_val(Field(v, i++));
  format->chroma_format = Int_val(Field(v, i++));
  format->interlaced = Bool_val(Field(v, i++));
  format->top_field_first = Bool_val(Field(v, i++));
  format->frame_rate_numerator = Int_val(Field(v, i++));
  format->frame_rate_denominator = Int_val(Field(v, i++));
  format->aspect_ratio_numerator = Int_val(Field(v, i++));
  format->aspect_ratio_denominator = Int_val(Field(v, i++));
  format->clean_width = Int_val(Field(v, i++));
  format->clean_height = Int_val(Field(v, i++));
  format->left_offset = Int_val(Field(v, i++));
  format->top_offset = Int_val(Field(v, i++));
  format->luma_offset = Int_val(Field(v, i++));
  format->luma_excursion = Int_val(Field(v, i++));
  format->chroma_offset = Int_val(Field(v, i++));
  format->chroma_excursion = Int_val(Field(v, i++));
  format->colour_primaries = Int_val(Field(v, i++));
  format->colour_matrix = Int_val(Field(v, i++));
  format->transfer_function = Int_val(Field(v, i++));
  format->interlaced_coding = Bool_val(Field(v, i++)); 
  schro_video_format_set_std_signal_range(format, Int_val(Field(v, i++)));

  return format;
}

static value value_of_video_format(SchroVideoFormat *format)
{
  CAMLparam0();
  CAMLlocal1(v);
  int i = 0;
  v = caml_alloc_tuple(23);
  Store_field (v, i++, Val_int(format->index));
  Store_field (v, i++, Val_int(format->width));
  Store_field (v, i++, Val_int(format->height));
  Store_field (v, i++, Val_int(format->chroma_format));
  Store_field (v, i++, Val_bool(format->interlaced));
  Store_field (v, i++, Val_bool(format->top_field_first));
  Store_field (v, i++, Val_int(format->frame_rate_numerator));
  Store_field (v, i++, Val_int(format->frame_rate_denominator));
  Store_field (v, i++, Val_int(format->aspect_ratio_numerator));
  Store_field (v, i++, Val_int(format->aspect_ratio_denominator));
  Store_field (v, i++, Val_int(format->clean_width));
  Store_field (v, i++, Val_int(format->clean_height));
  Store_field (v, i++, Val_int(format->left_offset));
  Store_field (v, i++, Val_int(format->top_offset));
  Store_field (v, i++, Val_int(format->luma_offset));
  Store_field (v, i++, Val_int(format->luma_excursion));
  Store_field (v, i++, Val_int(format->chroma_offset));
  Store_field (v, i++, Val_int(format->chroma_excursion));
  Store_field (v, i++, Val_int(format->colour_primaries));
  Store_field (v, i++, Val_int(format->colour_matrix));
  Store_field (v, i++, Val_int(format->transfer_function));
  Store_field (v, i++, Val_bool(format->interlaced_coding));
  Store_field (v, i++, Val_int(schro_video_format_get_std_signal_range(format)));

  CAMLreturn(v);
}

CAMLprim value ocaml_schroedinger_get_default_video_format(value index)
{
  CAMLparam0();
  CAMLlocal1(ret);
  SchroVideoFormat format;
  
  schro_video_format_set_std_video_format(&format, Int_val(index));
  ret = value_of_video_format(&format);

  CAMLreturn(ret);
}

/* Settings */

static double double_of_setting(value n, value v)
{
  char *s = String_val(n);

  if (!strcmp(s,"rate_control")) return (double)Int_val(v);
  if (!strcmp(s,"bitrate")) return (double)Int_val(v);
  if (!strcmp(s,"max_bitrate")) return (double)Int_val(v);
  if (!strcmp(s,"min_bitrate")) return (double)Int_val(v);
  if (!strcmp(s,"buffer_size")) return (double)Int_val(v);
  if (!strcmp(s,"buffer_level")) return (double)Int_val(v);
  if (!strcmp(s,"noise_threshold")) return Double_val(v);
  if (!strcmp(s,"gop_structure")) return (double)Int_val(v);
  if (!strcmp(s,"queue_depth")) return (double)Int_val(v);
  if (!strcmp(s,"perceptual_weighting")) return (double)Int_val(v);
  if (!strcmp(s,"perceptual_distance")) return Double_val(v);
  if (!strcmp(s,"filtering")) return (double)Int_val(v);
  if (!strcmp(s,"filter_value")) return Double_val(v);
  if (!strcmp(s,"profile")) return (double)Int_val(v);
  if (!strcmp(s,"level")) return (double)Int_val(v);
  if (!strcmp(s,"au_distance")) return (double)Int_val(v);
  if (!strcmp(s,"enable_psnr")) return (double)Bool_val(v);
  if (!strcmp(s,"enable_ssim")) return (double)Bool_val(v);
  if (!strcmp(s,"ref_distance")) return (double)Int_val(v);
  if (!strcmp(s,"transform_depth")) return (double)Int_val(v);
  if (!strcmp(s,"intra_wavelet")) return (double)Int_val(v);
  if (!strcmp(s,"inter_wavelet")) return (double)Int_val(v);
  if (!strcmp(s,"mv_precision")) return (double)Int_val(v);
  if (!strcmp(s,"motion_block_size")) return (double)Int_val(v);
  if (!strcmp(s,"motion_block_overlap")) return (double)Int_val(v);
  if (!strcmp(s,"interlaced_coding")) return (double)Bool_val(v);
  if (!strcmp(s,"enable_internal_testing")) return (double)Bool_val(v);
  if (!strcmp(s,"enable_noarith")) return (double)Bool_val(v);
  if (!strcmp(s,"enable_md5")) return (double)Bool_val(v);
  if (!strcmp(s,"enable_fullscan_estimation")) return (double)Bool_val(v);
  if (!strcmp(s,"enable_hierarchical_estimation")) return (double)Bool_val(v);
  if (!strcmp(s,"enable_zero_estimation")) return (double)Bool_val(v);
  if (!strcmp(s,"enable_phasecorr_estimation")) return (double)Bool_val(v);
  if (!strcmp(s,"enable_bigblock_estimation")) return (double)Bool_val(v);
  if (!strcmp(s,"horiz_slices")) return (double)Int_val(v);
  if (!strcmp(s,"vert_slices")) return (double)Int_val(v);
  if (!strcmp(s,"magic_dc_metric_offset")) return Double_val(v);
  if (!strcmp(s,"magic_subband0_lambda_scale")) return Double_val(v);
  if (!strcmp(s,"magic_chroma_lambda_scale")) return Double_val(v);
  if (!strcmp(s,"magic_nonref_lambda_scale")) return Double_val(v);
  if (!strcmp(s,"magic_allocation_scale")) return Double_val(v);
  if (!strcmp(s,"magic_keyframe_weight")) return Double_val(v);
  if (!strcmp(s,"magic_scene_change_threshold")) return Double_val(v);
  if (!strcmp(s,"magic_inter_p_weight")) return Double_val(v);
  if (!strcmp(s,"magic_inter_b_weight")) return Double_val(v);
  if (!strcmp(s,"magic_mc_bailout_limit")) return Double_val(v);
  if (!strcmp(s,"magic_bailout_weight")) return Double_val(v);
  if (!strcmp(s,"magic_error_power")) return Double_val(v);
  if (!strcmp(s,"magic_mc_lambda")) return Double_val(v);
  if (!strcmp(s,"magic_subgroup_length")) return Double_val(v);
  if (!strcmp(s,"magic_lambda")) return Double_val(v);

  caml_failwith("unknown value");
}

static value setting_of_double(value n, double v)
{
  char *s = String_val(n);

  if (!strcmp(s,"rate_control")) return Val_int((int)v);
  if (!strcmp(s,"bitrate")) return Val_int((int)v);
  if (!strcmp(s,"max_bitrate")) return Val_int((int)v);
  if (!strcmp(s,"min_bitrate")) return Val_int((int)v);
  if (!strcmp(s,"buffer_size")) return Val_int((int)v);
  if (!strcmp(s,"buffer_level")) return Val_int((int)v);
  if (!strcmp(s,"noise_threshold")) return caml_copy_double(v);
  if (!strcmp(s,"gop_structure")) return Val_int((int)v);
  if (!strcmp(s,"queue_depth")) return Val_int((int)v);
  if (!strcmp(s,"perceptual_weighting")) return Val_int((int)v);
  if (!strcmp(s,"perceptual_distance")) return caml_copy_double(v);
  if (!strcmp(s,"filtering")) return Val_int((int)v);
  if (!strcmp(s,"filter_value")) return caml_copy_double(v);
  if (!strcmp(s,"profile")) return Val_int((int)v);
  if (!strcmp(s,"level")) return Val_int((int)v);
  if (!strcmp(s,"au_distance")) return Val_int((int)v);
  if (!strcmp(s,"enable_psnr")) return Val_bool((int)v);
  if (!strcmp(s,"enable_ssim")) return Val_bool((int)v);
  if (!strcmp(s,"ref_distance")) return Val_int((int)v);
  if (!strcmp(s,"transform_depth")) return Val_int((int)v);
  if (!strcmp(s,"intra_wavelet")) return Val_int((int)v);
  if (!strcmp(s,"inter_wavelet")) return Val_int((int)v);
  if (!strcmp(s,"mv_precision")) return Val_int((int)v);
  if (!strcmp(s,"motion_block_size")) return Val_int((int)v);
  if (!strcmp(s,"motion_block_overlap")) return Val_int((int)v);
  if (!strcmp(s,"interlaced_coding")) return Val_bool((int)v);
  if (!strcmp(s,"enable_internal_testing")) return Val_bool((int)v);
  if (!strcmp(s,"enable_noarith")) return Val_bool((int)v);
  if (!strcmp(s,"enable_md5")) return Val_bool((int)v);
  if (!strcmp(s,"enable_fullscan_estimation")) return Val_bool((int)v);
  if (!strcmp(s,"enable_hierarchical_estimation")) return Val_bool((int)v);
  if (!strcmp(s,"enable_zero_estimation")) return Val_bool((int)v);
  if (!strcmp(s,"enable_phasecorr_estimation")) return Val_bool((int)v);
  if (!strcmp(s,"enable_bigblock_estimation")) return Val_bool((int)v);
  if (!strcmp(s,"horiz_slices")) return Val_int((int)v);
  if (!strcmp(s,"vert_slices")) return Val_int((int)v);
  if (!strcmp(s,"magic_dc_metric_offset")) return caml_copy_double(v);
  if (!strcmp(s,"magic_subband0_lambda_scale")) return caml_copy_double(v);
  if (!strcmp(s,"magic_chroma_lambda_scale")) return caml_copy_double(v);
  if (!strcmp(s,"magic_nonref_lambda_scale")) return caml_copy_double(v);
  if (!strcmp(s,"magic_allocation_scale")) return caml_copy_double(v);
  if (!strcmp(s,"magic_keyframe_weight")) return caml_copy_double(v);
  if (!strcmp(s,"magic_scene_change_threshold")) return caml_copy_double(v);
  if (!strcmp(s,"magic_inter_p_weight")) return caml_copy_double(v);
  if (!strcmp(s,"magic_inter_b_weight")) return caml_copy_double(v);
  if (!strcmp(s,"magic_mc_bailout_limit")) return caml_copy_double(v);
  if (!strcmp(s,"magic_bailout_weight")) return caml_copy_double(v);
  if (!strcmp(s,"magic_error_power")) return caml_copy_double(v);
  if (!strcmp(s,"magic_mc_lambda")) return caml_copy_double(v);
  if (!strcmp(s,"magic_subgroup_length")) return caml_copy_double(v);
  if (!strcmp(s,"magic_lambda")) return caml_copy_double(v);

  caml_failwith("unknown value");
}

/* Encoding */

typedef struct {
  SchroEncoder *encoder;
  SchroVideoFormat format;
  int is_sync_point;
  int distance_from_sync;
  ogg_int64_t presentation_frame_number;
  ogg_int64_t presented_frame_number;
  ogg_int64_t encoded_frame_number;
  ogg_int64_t packet_no;
} encoder_t;

#define Schro_enc_val(v) (*((encoder_t**)Data_custom_val(v)))

static void finalize_schro_enc(value v)
{
  encoder_t *enc = Schro_enc_val(v);
  schro_encoder_free(enc->encoder);
  free(enc);
}

static struct custom_operations schro_enc_ops =
{
  "ocaml_schro_enc",
  finalize_schro_enc,
  custom_compare_default,
  custom_hash_default,
  custom_serialize_default,
  custom_deserialize_default
};

/* Granule shift is always 22 for Dirac */
static const int DIRAC_GRANULE_SHIFT = 22;

static void calculate_granulepos(encoder_t *dd, ogg_packet *op, ogg_int64_t *pts)
{
    ogg_int64_t granulepos_hi;
    ogg_int64_t granulepos_low;
    int dt, pt, dist, delay;
    int update = 0;
    if (dd->is_sync_point)
        dd->distance_from_sync = 0;
    else
        dd->distance_from_sync++;

   if (pts != NULL) 
   {
     if (dd->presented_frame_number != *pts)
       update = 1;
     dd->presented_frame_number = *pts;
   }
   dt = dd->encoded_frame_number;
   pt = dd->presented_frame_number;
   delay = pt - dt;
    if (!dd->format.interlaced_coding)
    {
      pt *= 2;
      delay *= 2;
    }
    dist = dd->distance_from_sync;

    granulepos_hi = ((pt - delay)<<9) | ((dist>>8));
    granulepos_low = (delay << 9) | (dist & 0xff);

    op->granulepos = (granulepos_hi << DIRAC_GRANULE_SHIFT) | (granulepos_low);
    op->packetno = dd->packet_no++;
    if (update == 1)
      dd->encoded_frame_number++;
}

CAMLprim value ocaml_schroedinger_frames_of_granulepos(value _granulepos, value interlaced)
{
  CAMLparam1(_granulepos);
  ogg_int64_t granulepos = Int64_val(_granulepos);
  ogg_int64_t ret;

  if (granulepos == -1)
      CAMLreturn(caml_copy_int64(-1));

  ret = (granulepos >> 31) + (granulepos >> 9 & 0x7ff);

  if (interlaced == Val_false)
  {
    ret /= 2;
  }

  CAMLreturn(caml_copy_int64(ret));
}

CAMLprim value ocaml_schroedinger_encoded_of_granulepos(value _granulepos, value _enc)
{
  CAMLparam2(_granulepos, _enc);
  ogg_int64_t granulepos = Int64_val(_granulepos);
  encoder_t *enc = Schro_enc_val(_enc);
  ogg_int64_t ret;

  if (granulepos == -1)
      CAMLreturn(caml_copy_int64(-1));

  ret = granulepos >> 31;

  if (!enc->format.interlaced_coding)
  {
    ret /= 2;
  }

  CAMLreturn(caml_copy_int64(ret));
}

encoder_t *create_enc(SchroVideoFormat *format)
{
  encoder_t *enc = malloc(sizeof(encoder_t));
  if (enc == NULL)
    caml_raise_out_of_memory(); 

  enc->encoded_frame_number = -1;
  enc->presentation_frame_number = 0;
  enc->presented_frame_number = 0;
  enc->distance_from_sync = 0;
  enc->packet_no = 0;
  enc->is_sync_point = 1;
  memcpy(&enc->format,format,sizeof(SchroVideoFormat));
 
  SchroEncoder *encoder = schro_encoder_new();
  if (encoder == NULL) 
  {
    free(enc);
    caml_failwith("schro_encoder_new");
  }

  enc->encoder = encoder;

  if (schro_video_format_validate(format) != 1)
  {
    schro_encoder_free(enc->encoder);
    free(enc);
    /* TODO: proper exc */
    caml_failwith("invalid format");
  }

  schro_encoder_set_packet_assembly(enc->encoder, TRUE);
  schro_encoder_set_video_format(enc->encoder, format);
  schro_encoder_start(enc->encoder);

  return enc;
}

CAMLprim value ocaml_schroedinger_create_enc(value f)
{
  CAMLparam1(f);
  CAMLlocal1(ret);
  SchroVideoFormat format;
  schro_video_format_of_val(f, &format);
  encoder_t *enc = create_enc(&format);

  ret = caml_alloc_custom(&schro_enc_ops, sizeof(encoder_t*), 1, 0);
  Schro_enc_val(ret) = enc;

  CAMLreturn(ret);
}

CAMLprim value ocaml_schroedinger_enc_video_format(value _enc)
{
  CAMLparam1(_enc);
  encoder_t *enc = Schro_enc_val(_enc);
  CAMLreturn(value_of_video_format(&enc->format));
}

/* This function allocates op->packet */
int enc_get_packet(encoder_t *enc, ogg_packet *op)
{
  SchroStateEnum state;
  SchroBuffer *enc_buf;
  int dts;
  void *priv = NULL;
 
  /* Add a new ogg packet */
  caml_enter_blocking_section();
  state = schro_encoder_wait(enc->encoder);
  caml_leave_blocking_section();
  switch(state)
  {
  case SCHRO_STATE_NEED_FRAME:
      return 0;
  case SCHRO_STATE_END_OF_STREAM:
      return -1;
  case SCHRO_STATE_HAVE_BUFFER:
      caml_enter_blocking_section();
      enc_buf = schro_encoder_pull_full(enc->encoder, &dts, &priv);
      caml_enter_blocking_section();
      op->b_o_s = 0;
      if (SCHRO_PARSE_CODE_IS_SEQ_HEADER(enc_buf->data[4]))
      {
          enc->is_sync_point = 1;
      }
      else
      {
          enc->is_sync_point = 0;
      }
      op->e_o_s = 0;
      op->packet = malloc(enc_buf->length);
      if (op->packet == NULL) 
        caml_raise_out_of_memory();
      memcpy(op->packet, enc_buf->data, enc_buf->length);
      op->bytes = enc_buf->length;

      calculate_granulepos(enc, op, (ogg_int64_t *)priv);
      if (priv != NULL)
        free(priv);
      schro_buffer_unref(enc_buf);
      return 1;
  case SCHRO_STATE_AGAIN:
      return 2;
  default:
      caml_failwith("unknown encoder state");
  }
}

CAMLprim value ocaml_schroedinger_enc_eos(value _enc, value _os)
{
  CAMLparam2(_enc,_os);
  encoder_t *enc = Schro_enc_val(_enc);
  ogg_stream_state *os = Stream_state_val(_os);
  ogg_packet op;  
  int ret;

  schro_encoder_end_of_stream(enc->encoder);
  ret = enc_get_packet(enc,&op);
  while (ret != -1)
  {
    if (ret == 1)
    {
      /* Put the packet in the ogg stream. */
      ogg_stream_packetin(os, &op);
      free(op.packet);
    }
    ret = enc_get_packet(enc,&op);
  }

  /* Add last packet */
  op.packet = NULL;
  op.bytes = 0;
  op.e_o_s = 1;
  op.b_o_s = 0;
  ogg_stream_packetin(os, &op);

  CAMLreturn(Val_unit);
}

CAMLprim value ocaml_schroedinger_encode_frame(value _enc, value frame, value _os)
{
  CAMLparam3(_enc, frame, _os);
  ogg_stream_state *os = Stream_state_val(_os);
  encoder_t *enc = Schro_enc_val(_enc);
  SchroFrame *f = schro_frame_of_val(frame);
  ogg_int64_t *pts = malloc(sizeof(ogg_int64_t));
  if (pts == NULL)
    caml_raise_out_of_memory();
  memcpy(pts,&enc->presentation_frame_number,sizeof(ogg_int64_t));
  ogg_packet op;
  int ret = 2;
 
  /* Put the frame into the encoder. */
  caml_enter_blocking_section();
  schro_encoder_push_frame_full(enc->encoder, f, pts);
  caml_leave_blocking_section();
  enc->presentation_frame_number++;
 
  while (ret > 0) {
    ret = enc_get_packet(enc, &op);
    if (ret == 1)
    {
      /* Put the packet in the ogg stream. */
      ogg_stream_packetin(os, &op);
      free(op.packet);
    }
  }

  CAMLreturn(Val_unit);
}

CAMLprim value ocaml_schroedinger_encode_header(value _enc, value _os)
{
  CAMLparam2(_enc, _os);
  ogg_stream_state *os = Stream_state_val(_os);
  encoder_t *enc = Schro_enc_val(_enc); 
  encoder_t *tmp_enc; 
  ogg_packet op;
  int format;
  long header_len;
  uint8_t *header;
  SchroFrame *frame;


  /* Create a new encoder with the same format */
  tmp_enc = create_enc(&enc->format);

  /* Create dummy frame */
  format = schro_frame_format_of_chroma_format(enc->format.chroma_format);

  /* Encode a frame until a packet is ready */
  do
  {
    frame = schro_frame_new_and_alloc(NULL, format, enc->format.width, enc->format.height);
    schro_encoder_push_frame(tmp_enc->encoder, frame);
  }
  while (enc_get_packet(tmp_enc, &op) != 1);   

  /* Get the encoded buffer */
  header = op.packet;
  if (header[0] != 'B' ||
      header[1] != 'B' ||
      header[2] != 'C' ||
      header[3] != 'D' ||
      header[4] != 0x0)
  {
    /* TODO: proper exception */
    schro_encoder_free(tmp_enc->encoder);
    free(tmp_enc);
    caml_failwith("invalid header identifier");
  }
  header_len = (header[5] << 24) +
               (header[6] << 16) +
               (header[7] << 8) +
                header[8];

  if (header_len <= 13)
  {
    /* TODO: proper exception */
    schro_encoder_free(tmp_enc->encoder);
    free(tmp_enc);
    caml_failwith("invalid header: length too short");
  }
  if (header_len > op.bytes)
  {
    /* TODO: proper exception */
    schro_encoder_free(tmp_enc->encoder);
    free(tmp_enc);
    caml_failwith("invalid header: length too big");
  }
  op.b_o_s = 1;
  op.e_o_s = 0;
  op.bytes = header_len;
  op.granulepos = 0;

  /* Put the packet in the ogg stream. */
  ogg_stream_packetin(os, &op);

  /* Clean temporary encoder and op.packet */
  free(op.packet);
  schro_encoder_free(tmp_enc->encoder);
  free(tmp_enc);

  CAMLreturn(Val_unit);
}

CAMLprim value ocaml_schroedinger_set_setting(value _enc, value _name, value _val)
{
  CAMLparam2(_enc,_name);
  encoder_t *enc = Schro_enc_val(_enc);
  schro_encoder_setting_set_double(enc->encoder,String_val(_name),double_of_setting(_name,_val));
  CAMLreturn(Val_unit);
}

CAMLprim value ocaml_schroedinger_get_setting(value _enc, value _name)
{
  CAMLparam2(_enc,_name);
  CAMLlocal1(ret);
  encoder_t *enc = Schro_enc_val(_enc);
  double x = schro_encoder_setting_get_double(enc->encoder,String_val(_name));
  CAMLreturn(setting_of_double(_name,x));
}

/* Decoder */

#define Schro_dec_val(v) (*((SchroDecoder **)Data_custom_val(v)))

static void finalize_schro_dec(value v)
{
  SchroDecoder *dec = Schro_dec_val(v);
  schro_decoder_free(dec);
}

static struct custom_operations schro_dec_ops =
{
  "ocaml_schro_dec",
  finalize_schro_dec,
  custom_compare_default,
  custom_hash_default,
  custom_serialize_default,
  custom_deserialize_default
};

CAMLprim value ocaml_schroedinger_create_dec(value packet)
{
  CAMLparam1(packet);
  CAMLlocal1(ret);
  ogg_packet *op = Packet_val(packet);
  unsigned char *header;
  long header_len;


  /* Get the encoded buffer */
  header = op->packet;
  if (header[0] != 'B' ||
      header[1] != 'B' ||
      header[2] != 'C' ||
      header[3] != 'D' ||
      header[4] != 0x0)
    caml_raise_constant(*caml_named_value("schro_exn_invalid_header"));

  header_len = (header[5] << 24) +
               (header[6] << 16) +
               (header[7] << 8) +
                header[8];

   if (header_len <= 13 ||
       header_len > op->bytes)
     caml_raise_constant(*caml_named_value("schro_exn_invalid_header"));

  SchroDecoder *dec = schro_decoder_new();
  SchroBuffer *buffer = schro_buffer_of_ogg_packet(op);
  schro_decoder_autoparse_push(dec, buffer);

  ret = caml_alloc_custom(&schro_dec_ops, sizeof(SchroDecoder*), 1, 0);
  Schro_dec_val(ret) = dec;

  CAMLreturn(ret);
}

CAMLprim value ocaml_schroedinger_decoder_get_format(value dec)
{
  CAMLparam1(dec);
  CAMLlocal1(ret);
  SchroDecoder *decoder = Schro_dec_val(dec);
  SchroVideoFormat *format = schro_decoder_get_video_format(decoder);
  ret = value_of_video_format(format);
  free(format);
  CAMLreturn(ret);
}

CAMLprim value ocaml_schroedinger_decoder_get_picture_number(value dec)
{
  CAMLparam1(dec);
  SchroDecoder *decoder = Schro_dec_val(dec);
  CAMLreturn(Val_int(schro_decoder_get_picture_number(decoder)));
}

CAMLprim value ocaml_schroedinger_decoder_decode_frame(value dec, value _os)
{
  CAMLparam2(dec, _os);
  CAMLlocal1(ret);
  SchroDecoder *decoder = Schro_dec_val(dec);
  ogg_stream_state *os = Stream_state_val(_os);
  SchroVideoFormat *format;
  ogg_packet op;
  SchroFrame *frame;
  int state, err;

  while (1) {
    /* Check what the decoder wants now. */
    caml_enter_blocking_section();
    state = schro_decoder_autoparse_wait(decoder);
    caml_leave_blocking_section();
    switch (state) {
      case SCHRO_DECODER_FIRST_ACCESS_UNIT:
      case SCHRO_DECODER_NEED_BITS:
        /* Grap a packet */
        err = ogg_stream_packetout(os,&op);
        if (err == 0) 
          caml_raise_constant(*caml_named_value("ogg_exn_not_enough_data"));
        if (err == -1)
          caml_raise_constant(*caml_named_value("ogg_exn_out_of_sync"));
        /* Feed the decoder */
        caml_enter_blocking_section();
        schro_decoder_autoparse_push(decoder,schro_buffer_of_ogg_packet(&op));
        caml_leave_blocking_section();
        break;
      case SCHRO_DECODER_NEED_FRAME:
        format = schro_decoder_get_video_format(decoder);
        schro_decoder_add_output_picture(decoder,
                                         schro_frame_alloc(schro_frame_format_of_chroma_format(format->chroma_format), 
                                                           format->width, format->height));
        free(format);
        break;
      case SCHRO_DECODER_OK:
        caml_enter_blocking_section();
        frame = schro_decoder_pull(decoder);
        caml_leave_blocking_section();
        if (frame->width != 0 && frame->height != 0) {
          ret = val_of_schro_frame(frame);
          schro_frame_unref(frame);
          CAMLreturn(ret);
        } else {
          schro_frame_unref(frame); 
          caml_raise_constant(*caml_named_value("schro_exn_skip"));
        }
        break;
      /* TODO: proper error raising.. */
      case SCHRO_DECODER_STALLED: 
      case SCHRO_DECODER_WAIT:
      case SCHRO_DECODER_ERROR:
      default:
        caml_raise_constant(*caml_named_value("schro_exn_dec_error"));
      }
  }

  caml_failwith("unknown error");  
}

/* Ogg skeleton interface */

/* Wrappers */
static void write32le(unsigned char *ptr,ogg_uint32_t v)
{
  ptr[0]=v&0xff;
  ptr[1]=(v>>8)&0xff;
  ptr[2]=(v>>16)&0xff;
  ptr[3]=(v>>24)&0xff;
}

static void write64le(unsigned char *ptr,ogg_int64_t v)
{
  ogg_uint32_t hi=v>>32;
  ptr[0]=v&0xff;
  ptr[1]=(v>>8)&0xff;
  ptr[2]=(v>>16)&0xff;
  ptr[3]=(v>>24)&0xff;
  ptr[4]=hi&0xff;
  ptr[5]=(hi>>8)&0xff;
  ptr[6]=(hi>>16)&0xff;
  ptr[7]=(hi>>24)&0xff;
}

/* Values from http://xiph.org/ogg/doc/skeleton.html */
#define FISBONE_IDENTIFIER "fisbone\0"
#define FISBONE_MESSAGE_HEADER_OFFSET 44
#define FISBONE_SIZE 52

CAMLprim value ocaml_schroedinger_skeleton_fisbone(value serial, value info, value start, value content)
{
  CAMLparam4(serial,info,start,content);
  CAMLlocal1(packet);
  ogg_packet op;
  SchroVideoFormat format;
  schro_video_format_of_val(info, &format);
  int len = FISBONE_SIZE+caml_string_length(content);

  memset (&op, 0, sizeof (op));
  op.packet = malloc(len);
  if (op.packet == NULL)
    caml_raise_out_of_memory();

  memset (op.packet, 0, len);
  /* it will be the fisbone packet for the theora video */
  memcpy (op.packet, FISBONE_IDENTIFIER, 8); /* identifier */
  write32le(op.packet+8, FISBONE_MESSAGE_HEADER_OFFSET); /* offset of the message header fields */
  write32le(op.packet+12, Nativeint_val(serial)); /* serialno of the theora stream */
  write32le(op.packet+16, 1); /* number of header packets */
  /* granulerate, temporal resolution of the bitstream in samples/microsecond */
  write64le(op.packet+20, (ogg_int64_t)format.frame_rate_numerator); /* granulrate numerator */
  write64le(op.packet+28, (ogg_int64_t)format.frame_rate_denominator); /* granulrate denominator */
  write64le(op.packet+36, (ogg_int64_t)Int64_val(start)); /* start granule */
  write32le(op.packet+44, 0); /* preroll, for theora its 0 */
  *(op.packet+48) = DIRAC_GRANULE_SHIFT; /* granule shift */
  memcpy(op.packet+FISBONE_SIZE, String_val(content), caml_string_length(content)); /* message header field */

  op.b_o_s = 0;
  op.e_o_s = 0;
  op.bytes = len;

  packet = value_of_packet(&op);
  free(op.packet);
  CAMLreturn(packet);
}

