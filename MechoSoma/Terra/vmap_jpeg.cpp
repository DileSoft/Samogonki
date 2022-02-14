//
// Created by caiiiycuk on 14.02.22.
//
#include "TERRA.H"
#include "port.h"
#include "mechosoma.h"

#define XMD_H

#include <stdio.h>
#include <stddef.h>
#include <setjmp.h>
#include <jpeglib.h>

struct my_error_mgr {
  struct jpeg_error_mgr pub;

  jmp_buf setjmp_buffer;
};

typedef struct my_error_mgr * my_error_ptr;

METHODDEF(void)
my_error_exit (j_common_ptr cinfo)
{
my_error_ptr myerr = (my_error_ptr) cinfo->err;

(*cinfo->err->output_message) (cinfo);

longjmp(myerr->setjmp_buffer, 1);
}


int vrtMap::loadClBufJ(int X,int Y,int num_track)
{
  X=0;Y=YS_Buf;//ТОлько для начала !

  if(num_track==-1)num_track=0;
  int i,j=0;
  XBuffer buft;
  char tmpstr[10];
  buft < "track" < port_itoa(num_track%10,tmpstr,10) < ".jpg";


  struct jpeg_decompress_struct cinfo;
  struct my_error_mgr jerr;
  JSAMPARRAY buffer;
  int row_stride;

  /*	if ((infile = fopen(GetTargetName(buft), "rb")) == NULL) {
                  return 0;
          }
  */
  XStream fileExsistin(0);
#ifdef _MECHOSOMA_
  if(!vmapOpenResource(GetTargetName(buft), fileExsistin) ) {
#else
    if(!fileExsistin.open(GetTargetName(buft), XS_IN) ) {
#endif
    fileExsistin.close();
    return 0;
  }

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = my_error_exit;
  if (setjmp(jerr.setjmp_buffer)) {
    jpeg_destroy_decompress(&cinfo);
    fileExsistin.close();
    return 0;
  }
  jpeg_create_decompress(&cinfo);

  //jpeg_stdio_src(&cinfo, infile);
  extern void my_jpeg_stdio_src (j_decompress_ptr cinfo, XStream * Xfile);
  my_jpeg_stdio_src(&cinfo, &fileExsistin);

  (void) jpeg_read_header(&cinfo, TRUE);

  (void) jpeg_start_decompress(&cinfo);

  row_stride = cinfo.output_width * cinfo.output_components;
  buffer = (*cinfo.mem->alloc_sarray)
      ((j_common_ptr) &cinfo, JPOOL_IMAGE, row_stride, 1);

  while (cinfo.output_scanline < cinfo.output_height) {
    (void) jpeg_read_scanlines(&cinfo, buffer, 1);
    unsigned char* p=buffer[0];
    for (i=0; i< cinfo.output_width; i++){
      short col;
      col=(*p++ <<8)&0x0F800;
      col+=(*p++ <<3)&0x7E0;
      col+=(*p++ >>3)&0x1F;
      ClBuf[0][j][i]=col;
    }
    j++;
  }

  (void) jpeg_finish_decompress(&cinfo);

  jpeg_destroy_decompress(&cinfo);

  fileExsistin.close();
  //fclose(infile);
  return 1;
}
