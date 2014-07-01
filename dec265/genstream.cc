/*
 * H.265 video codec.
 * Copyright (c) 2013-2014 struktur AG, Dirk Farin <farin@struktur.de>
 *
 * This file is part of libde265.
 *
 * libde265 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * libde265 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with libde265.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "libde265/nal-parser.h"
#include "libde265/decctx.h"
#include "libde265/encode.h"
#include "libde265/slice.h"
#include "libde265/scan.h"
#include <assert.h>

error_queue errqueue;

video_parameter_set vps;
seq_parameter_set   sps;
pic_parameter_set   pps;
slice_segment_header shdr;

CABAC_encoder writer;

de265_image img;
encoder_context ectx;


void encode_image()
{
  int w = sps.pic_width_in_luma_samples;
  int h = sps.pic_height_in_luma_samples;

  img.alloc_image(w,h, de265_chroma_420, &sps, true, NULL /* no decctx */);
  img.alloc_encoder_data(&sps);

  initialize_CABAC_models(ectx.ctx_model, shdr.initType, shdr.SliceQPY);

  int Log2CtbSize = sps.Log2CtbSizeY;

#if 0
  for (int y=0;y<sps.PicHeightInCtbsY;y++)
    for (int x=0;x<sps.PicWidthInCtbsY;x++)
      {
        int x0 = x<<Log2CtbSize;
        int y0 = y<<Log2CtbSize;
        img.set_ctDepth(x0,y0, Log2CtbSize, 1); //((x+y)&1)==0);  // all CBs 16x16
        img.set_pred_mode(x0,y0, Log2CtbSize, MODE_INTRA);
        img.set_PartMode(x0,y0, PART_2Nx2N);
        img.set_IntraPredMode(x0,y0, Log2CtbSize, (enum IntraPredMode)1);
        //img.set_IntraChromaPredMode(x0,y0, Log2CtbSize, INTRA_CHROMA_LIKE_LUMA);
      }


  for (int y=0;y<sps.PicHeightInCtbsY*2;y++)
    for (int x=0;x<sps.PicWidthInCtbsY*2;x++)
      {
        int x0 = x<<(Log2CtbSize-1);
        int y0 = y<<(Log2CtbSize-1);

        img.set_cbf_luma(x0,y0, Log2CtbSize-1);
        img.set_cbf_cb  (x0,y0, Log2CtbSize-1);
        img.set_cbf_cr  (x0,y0, Log2CtbSize-1);
      }
#endif

  // encode CTB by CTB

  for (int y=0;y<sps.PicHeightInCtbsY;y++)
    for (int x=0;x<sps.PicWidthInCtbsY;x++)
      {
        enc_cb* cb = ectx.enc_cb_pool.get_new();
        cb->split_cu_flag = false;

        cb->cu_transquant_bypass_flag = false;
        cb->PredMode = MODE_INTRA;
        cb->PartMode = PART_2Nx2N;

        enc_pb_intra* pb = ectx.enc_pb_intra_pool.get_new();
        cb->intra_pb[0] = pb;
        pb->pred_mode = INTRA_DC;
        pb->pred_mode_chroma = INTRA_DC;


        enc_tb* tb = ectx.enc_tb_pool.get_new();
        cb->transform_tree = tb;

        tb->parent = NULL;
        tb->split_transform_flag = false;
        tb->cbf_luma = true;
        tb->cbf_cb   = false;
        tb->cbf_cr   = false;

        int16_t coeff[16*16];
        memset(coeff,0,16*16*sizeof(int16_t));
        coeff[0] = 5; //((x+y)/*&1*/) * 5 - 101;
        coeff[7] = 7;
        if (x+y==0) coeff[0]=-26;
        tb->coeff[0] = coeff;

        cb->write_to_image(&img, x<<Log2CtbSize, y<<Log2CtbSize, Log2CtbSize, true);
        encode_ctb(&ectx, cb, x,y);

        int last = (y==sps.PicHeightInCtbsY-1 &&
                    x==sps.PicWidthInCtbsY-1);

        printf("wrote CTB at %d;%d\n",x*16,y*16);
        printf("write term bit: %d\n",last);
        writer.write_CABAC_term_bit(last);


        ectx.enc_cb_pool.free_all();
        ectx.enc_tb_pool.free_all();
        ectx.enc_pb_intra_pool.free_all();
      }
}

extern void split_last_significant_position(int pos, int* prefix, int* suffix, int* nSuffixBits);

static void test_last_significant_coeff_pos()
{
  for (int i=0;i<64;i++)
    {
      int prefix,suffix,bits;
      split_last_significant_position(i,&prefix,&suffix,&bits);

      int v;
      if (prefix>3) {
        int b = (prefix>>1)-1;
        assert(b==bits);
        v = ((2+(prefix&1))<<b) + suffix;
      }
      else {
        v = prefix;
      }

      printf("%d : %d %d %d  -> %d\n",i,prefix,suffix,bits,v);
      assert(v==i);
    }
}


void write_stream_1()
{
  nal_header nal;

  // VPS

  vps.set_defaults(Profile_Main, 6,2);


  // SPS

  sps.set_defaults();
  sps.set_CB_log2size_range(4,4);
  sps.set_TB_log2size_range(4,4);
  sps.set_resolution(352,288);
  sps.compute_derived_values();

  // PPS

  pps.set_defaults();
  pps.set_derived_values(&sps);


  // slice

  shdr.set_defaults(&pps);

  img.vps  = vps;
  img.sps  = sps;
  img.pps  = pps;

  ectx.img = &img;
  ectx.shdr = &shdr;
  ectx.cabac_encoder = &writer;

  //context_model ctx_model[CONTEXT_MODEL_TABLE_LENGTH];



  // write headers

  writer.write_startcode();
  nal.set(NAL_UNIT_VPS_NUT);
  nal.write(&writer);
  vps.write(&errqueue, &writer);
  writer.flush_VLC();

  writer.write_startcode();
  nal.set(NAL_UNIT_SPS_NUT);
  nal.write(&writer);
  sps.write(&errqueue, &writer);
  writer.flush_VLC();

  writer.write_startcode();
  nal.set(NAL_UNIT_PPS_NUT);
  nal.write(&writer);
  pps.write(&errqueue, &writer, &sps);
  writer.flush_VLC();

  writer.write_startcode();
  nal.set(NAL_UNIT_IDR_W_RADL);
  nal.write(&writer);
  shdr.write(&errqueue, &writer, &sps, &pps, nal.nal_unit_type);
  writer.skip_bits(1);
  writer.flush_VLC();

  encode_image();

  //encode_image(&ectx);
  writer.flush_CABAC();
}



int main(int argc, char** argv)
{
  init_scan_orders();
  alloc_and_init_significant_coeff_ctxIdx_lookupTable();

  de265_set_verbosity(3);

  write_stream_1();

  FILE* fh = fopen("out.bin","wb");
  fwrite(writer.data(), 1,writer.size(), fh);
  fclose(fh);

  return 0;
}
