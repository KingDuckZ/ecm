/***************************************************************************/
/*
** ECM - Encoder for ECM (Error Code Modeler) format.
** Version 1.0
** Copyright (C) 2002 Neill Corlett
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the GNU General Public License
** as published by the Free Software Foundation; either version 2
** of the License, or (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/
/***************************************************************************/
/*
** Portability notes:
**
** - Assumes a 32-bit or higher integer size
** - No assumptions about byte order
** - No assumptions about struct packing
** - No unaligned memory access
*/
/***************************************************************************/
#define _FILE_OFFSET_BITS 64
/*#define ENABLE_EXTRA_CHECKS*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ENABLE_EXTRA_CHECKS
#include <limits.h>
#endif

#if !defined(_FILE_OFFSET_BITS) || _FILE_OFFSET_BITS != 64
#define ORIGINAL_MODE
#endif
/***************************************************************************/

/* Data types */
typedef unsigned char ecc_uint8;
typedef unsigned short ecc_uint16;
typedef unsigned int ecc_uint32;
typedef signed int ecc_int32;

/***************************************************************************/

void banner(void) {
  fprintf(stderr,
#ifdef ORIGINAL_MODE
    "ECM - Encoder for Error Code Modeler format v1.0\n"
    "Copyright (C) 2002 Neill Corlett\n\n"
#else
    "ECM - Encoder for Error Code Modeler format v1.0 64bit\n"
    "Copyright (C) 2002 Neill Corlett\n"
    "64bit version 2010 Michele Santullo\n\n"
#endif
  );
}

/***************************************************************************/

char* GetByteSize(off_t size, char* dst) {
  static const char* postfix[] = {"byte", "KiB", "MiB", "GiB", "TiB", "PiB"};
  int chosenpostfix = 0;
  off_t divisorshift = 0;

  while ((size >> divisorshift) >= 1024 && chosenpostfix < sizeof(postfix)) {
	divisorshift += 10;
    ++chosenpostfix;
  }

  const int result = (int)(size >> divisorshift);
  const off_t rest = size - ((off_t)result << divisorshift);
  const int decimalpart = (int)(rest * 100 >> divisorshift);
  sprintf(dst, "%d.%d %s", result, decimalpart, postfix[chosenpostfix]);
  return dst;
}

/***************************************************************************/

/* LUTs used for computing ECC/EDC */
static ecc_uint8 ecc_f_lut[256];
static ecc_uint8 ecc_b_lut[256];
static ecc_uint32 edc_lut[256];

/* Init routine */
static void eccedc_init(void) {
  ecc_uint32 i, j, edc;
  for(i = 0; i < 256; i++) {
    j = (i << 1) ^ (i & 0x80 ? 0x11D : 0);
    ecc_f_lut[i] = j;
    ecc_b_lut[i ^ j] = i;
    edc = i;
    for(j = 0; j < 8; j++) edc = (edc >> 1) ^ (edc & 1 ? 0xD8018001 : 0);
    edc_lut[i] = edc;
  }
}

/***************************************************************************/
/*
** Compute EDC for a block
*/
ecc_uint32 edc_computeblock(
        ecc_uint32 edc,
        const ecc_uint8 *src,
        ecc_uint16 size
) {
  while(size--) edc = (edc >> 8) ^ edc_lut[(edc ^ (*src++)) & 0xFF];
  return edc;
}

/***************************************************************************/
/*
** Compute ECC for a block (can do either P or Q)
*/
static int ecc_computeblock(
  ecc_uint8 *src,
  ecc_uint32 major_count,
  ecc_uint32 minor_count,
  ecc_uint32 major_mult,
  ecc_uint32 minor_inc,
  ecc_uint8 *dest
) {
  ecc_uint32 size = major_count * minor_count;
  ecc_uint32 major, minor;
  for(major = 0; major < major_count; major++) {
    ecc_uint32 index = (major >> 1) * major_mult + (major & 1);
    ecc_uint8 ecc_a = 0;
    ecc_uint8 ecc_b = 0;
    for(minor = 0; minor < minor_count; minor++) {
      ecc_uint8 temp = src[index];
      index += minor_inc;
      if(index >= size) index -= size;
      ecc_a ^= temp;
      ecc_b ^= temp;
      ecc_a = ecc_f_lut[ecc_a];
    }
    ecc_a = ecc_b_lut[ecc_f_lut[ecc_a] ^ ecc_b];
    if(dest[major              ] != (ecc_a        )) return 0;
    if(dest[major + major_count] != (ecc_a ^ ecc_b)) return 0;
  }
  return 1;
}

/*
** Generate ECC P and Q codes for a block
*/
static int ecc_generate(
  ecc_uint8 *sector,
  int        zeroaddress,
  ecc_uint8 *dest
) {
  int r;
  ecc_uint8 address[4], i;
  /* Save the address and zero it out */
  if(zeroaddress) for(i = 0; i < 4; i++) {
    address[i] = sector[12 + i];
    sector[12 + i] = 0;
  }
  /* Compute ECC P code */
  if(!(ecc_computeblock(sector + 0xC, 86, 24,  2, 86, dest + 0x81C - 0x81C))) {
    if(zeroaddress) for(i = 0; i < 4; i++) sector[12 + i] = address[i];
    return 0;
  }
  /* Compute ECC Q code */
  r = ecc_computeblock(sector + 0xC, 52, 43, 86, 88, dest + 0x8C8 - 0x81C);
  /* Restore the address */
  if(zeroaddress) for(i = 0; i < 4; i++) sector[12 + i] = address[i];
  return r;
}

/***************************************************************************/

/*
** sector types:
** 00 - literal bytes
** 01 - 2352 mode 1         predict sync, mode, reserved, edc, ecc
** 02 - 2336 mode 2 form 1  predict redundant flags, edc, ecc
** 03 - 2336 mode 2 form 2  predict redundant flags, edc
*/

int check_type(unsigned char *sector, int canbetype1) {
  int canbetype2 = 1;
  int canbetype3 = 1;
  ecc_uint32 myedc;
  /* Check for mode 1 */
  if(canbetype1) {
    if(
      (sector[0x00] != 0x00) ||
      (sector[0x01] != 0xFF) ||
      (sector[0x02] != 0xFF) ||
      (sector[0x03] != 0xFF) ||
      (sector[0x04] != 0xFF) ||
      (sector[0x05] != 0xFF) ||
      (sector[0x06] != 0xFF) ||
      (sector[0x07] != 0xFF) ||
      (sector[0x08] != 0xFF) ||
      (sector[0x09] != 0xFF) ||
      (sector[0x0A] != 0xFF) ||
      (sector[0x0B] != 0x00) ||
      (sector[0x0F] != 0x01) ||
      (sector[0x814] != 0x00) ||
      (sector[0x815] != 0x00) ||
      (sector[0x816] != 0x00) ||
      (sector[0x817] != 0x00) ||
      (sector[0x818] != 0x00) ||
      (sector[0x819] != 0x00) ||
      (sector[0x81A] != 0x00) ||
      (sector[0x81B] != 0x00)
    ) {
      canbetype1 = 0;
    }
  }
  /* Check for mode 2 */
  if(
    (sector[0x0] != sector[0x4]) ||
    (sector[0x1] != sector[0x5]) ||
    (sector[0x2] != sector[0x6]) ||
    (sector[0x3] != sector[0x7])
  ) {
    canbetype2 = 0;
    canbetype3 = 0;
    if(!canbetype1) return 0;
  }

  /* Check EDC */
  myedc = edc_computeblock(0, sector, 0x808);
  if(canbetype2) if(
    (sector[0x808] != ((myedc >>  0) & 0xFF)) ||
    (sector[0x809] != ((myedc >>  8) & 0xFF)) ||
    (sector[0x80A] != ((myedc >> 16) & 0xFF)) ||
    (sector[0x80B] != ((myedc >> 24) & 0xFF))
  ) {
    canbetype2 = 0;
  }
  myedc = edc_computeblock(myedc, sector + 0x808, 8);
  if(canbetype1) if(
    (sector[0x810] != ((myedc >>  0) & 0xFF)) ||
    (sector[0x811] != ((myedc >>  8) & 0xFF)) ||
    (sector[0x812] != ((myedc >> 16) & 0xFF)) ||
    (sector[0x813] != ((myedc >> 24) & 0xFF))
  ) {
    canbetype1 = 0;
  }
  myedc = edc_computeblock(myedc, sector + 0x810, 0x10C);
  if(canbetype3) if(
    (sector[0x91C] != ((myedc >>  0) & 0xFF)) ||
    (sector[0x91D] != ((myedc >>  8) & 0xFF)) ||
    (sector[0x91E] != ((myedc >> 16) & 0xFF)) ||
    (sector[0x91F] != ((myedc >> 24) & 0xFF))
  ) {
    canbetype3 = 0;
  }
  /* Check ECC */
  if(canbetype1) { if(!(ecc_generate(sector       , 0, sector + 0x81C))) { canbetype1 = 0; } }
  if(canbetype2) { if(!(ecc_generate(sector - 0x10, 1, sector + 0x80C))) { canbetype2 = 0; } }
  if(canbetype1) return 1;
  if(canbetype2) return 2;
  if(canbetype3) return 3;
  return 0;
}

/***************************************************************************/
/*
** Encode a type/count combo
*/
void write_type_count(
  FILE *out,
  int type,
  off_t count
) {
  /*Workaround to replace the side effect of calling this function with count=0,
  when it was of type uint32.*/
  if (count == 0)
    count = 0xFFFFFFFF;
  else
    count--;
  int charToWrite = (int)(((count >= 32 ? 1 : 0) << 7) | ((count & 31) << 2));
  fputc(charToWrite | type, out);
  count >>= 5;
  while(count) {
    charToWrite = (int)(((count >= 128 ? 1 : 0) << 7) | (count & 127));
    fputc(charToWrite, out);
    count >>= 7;
  }
}

/***************************************************************************/

off_t mycounter_analyze;
off_t mycounter_encode;
off_t mycounter_total;

void resetcounter(off_t total) {
  mycounter_analyze = 0;
  mycounter_encode = 0;
  mycounter_total = total;
}

void setcounter_analyze(off_t n) {
  if((n >> 20) != (mycounter_analyze >> 20)) {
    off_t a = (n+64)/128;
    off_t e = (mycounter_encode+64)/128;
    off_t d = (mycounter_total+64)/128;
    if(!d) d = 1;
    fprintf(stderr,
#ifdef ORIGINAL_MODE
      "Analyzing (%02d%%) Encoding (%02d%%)\r",
#else
      "Analyzing (%02lld%%) Encoding (%02lld%%)\r",
#endif
      (100*a) / d, (100*e) / d
    );
  }
  mycounter_analyze = n;
}

void setcounter_encode(off_t n) {
  if((n >> 20) != (mycounter_encode >> 20)) {
    off_t a = (mycounter_analyze+64)/128;
    off_t e = (n+64)/128;
    off_t d = (mycounter_total+64)/128;
    if(!d) d = 1;
    fprintf(stderr,
#ifdef ORIGINAL_MODE
      "Analyzing (%02d%%) Encoding (%02d%%)\r",
#else
      "Analyzing (%02lld%%) Encoding (%02lld%%)\r",
#endif
      (100*a) / d, (100*e) / d
    );
  }
  mycounter_encode = n;
}

/***************************************************************************/
/*
** Encode a run of sectors/literals of the same type
*/
ecc_uint32 in_flush(
  ecc_uint32 edc,
  int type,
  off_t count,
  FILE *in,
  FILE *out
) {
  unsigned char buf[2352];
  write_type_count(out, type, count);
  if(!type) {
    while(count) {
      ecc_uint16 b = (count > 2352 ? 2352 : (ecc_uint16)count);
      fread(buf, 1, b, in);
      edc = edc_computeblock(edc, buf, b);
      fwrite(buf, 1, b, out);
      count -= b;
      setcounter_encode(ftell(in));
    }
    return edc;
  }
  while(count--) {
    switch(type) {
    case 1:
      fread(buf, 1, 2352, in);
      edc = edc_computeblock(edc, buf, 2352);
      fwrite(buf + 0x00C, 1, 0x003, out);
      fwrite(buf + 0x010, 1, 0x800, out);
      setcounter_encode(ftell(in));
      break;
    case 2:
      fread(buf, 1, 2336, in);
      edc = edc_computeblock(edc, buf, 2336);
      fwrite(buf + 0x004, 1, 0x804, out);
      setcounter_encode(ftell(in));
      break;
    case 3:
      fread(buf, 1, 2336, in);
      edc = edc_computeblock(edc, buf, 2336);
      fwrite(buf + 0x004, 1, 0x918, out);
      setcounter_encode(ftell(in));
      break;
    }
  }
  return edc;
}

/***************************************************************************/

unsigned char inputqueue[1048576 * 5 + 4];

int ecmify(FILE *in, FILE *out) {
  ecc_uint32 inedc = 0;
  int curtype = -1;
  off_t curtypecount = 0;
  off_t curtype_in_start = 0;
  int detecttype;
  off_t incheckpos = 0;
  off_t inbufferpos = 0;
  off_t intotallength;
  ecc_uint32 inqueuestart = 0;
  ecc_int32 dataavail = 0;
  off_t typetally[4];
  fseek(in, 0, SEEK_END);
  intotallength = ftell(in);
  resetcounter(intotallength);
  typetally[0] = 0;
  typetally[1] = 0;
  typetally[2] = 0;
  typetally[3] = 0;
  /* Magic identifier */
  fputc('E', out);
  fputc('C', out);
  fputc('M', out);
  fputc(0x00, out);
  for(;;) {
    if((dataavail < 2352) && (intotallength - inbufferpos > dataavail)) {
      const off_t diffLenPos = intotallength - inbufferpos;
      ecc_int32 willread;
      if (diffLenPos > sizeof(inputqueue) - 4 - dataavail) {
#ifdef ENABLE_EXTRA_CHECKS
        if (INT_MAX < sizeof(inputqueue) - 4 - dataavail) {
          printf("Sorry, \"sizeof(inputqueue) - 4 - dataavail\" is too big to be casted to int32 (%ld)\n", sizeof(inputqueue) - 4 - dataavail);
          exit(0);
        }
#endif
        willread = (ecc_int32)(sizeof(inputqueue) - 4 - dataavail);
	  }
      else {
        willread = (ecc_int32)diffLenPos;
	  }

      if(inqueuestart) {
        memmove(inputqueue + 4, inputqueue + 4 + inqueuestart, dataavail);
        inqueuestart = 0;
      }
      if(willread) {
        setcounter_analyze(inbufferpos);
        fseek(in, inbufferpos, SEEK_SET);
        fread(inputqueue + 4 + dataavail, 1, willread, in);
        inbufferpos += willread;
#ifdef ENABLE_EXTRA_CHECKS
        if (LLONG_MAX - dataavail < willread) {
          printf("Sorry, I can't increase dataavail by willread: intmax-dataavail=%d, willread=%d", LLONG_MAX - dataavail, willread);
          exit(0);
        }
#endif
        dataavail += willread;
      }
    }
    if(dataavail == 0) break;
    if(dataavail < 2336) {
      detecttype = 0;
    }
    else {
      detecttype = check_type(inputqueue + 4 + inqueuestart, dataavail >= 2352);
    }
    if(detecttype != curtype) {
      if(curtypecount) {
        fseek(in, curtype_in_start, SEEK_SET);
        typetally[curtype] += curtypecount;
        inedc = in_flush(inedc, curtype, curtypecount, in, out);
      }
      curtype = detecttype;
      curtype_in_start = incheckpos;
      curtypecount = 1;
    }
    else {
#ifdef ENABLE_EXTRA_CHECKS
      if (curtypecount == LLONG_MAX) {
        printf("Sorry, I can't increase curtypecount by one, it got to LLONG_MAX!\n");
		exit(0);
      }
#endif
      curtypecount++;
    }
    switch(curtype) {
    case 0: incheckpos +=    1; inqueuestart +=    1; dataavail -=    1; break;
    case 1: incheckpos += 2352; inqueuestart += 2352; dataavail -= 2352; break;
    case 2: incheckpos += 2336; inqueuestart += 2336; dataavail -= 2336; break;
    case 3: incheckpos += 2336; inqueuestart += 2336; dataavail -= 2336; break;
    }
  }
  if(curtypecount) {
    fseek(in, curtype_in_start, SEEK_SET);
    typetally[curtype] += curtypecount;
    inedc = in_flush(inedc, curtype, curtypecount, in, out);
  }
  /* End-of-records indicator */
  write_type_count(out, 0, 0);
  /* Input file EDC */
  fputc((int)((inedc >>  0) & 0xFF), out);
  fputc((int)((inedc >>  8) & 0xFF), out);
  fputc((int)((inedc >> 16) & 0xFF), out);
  fputc((int)((inedc >> 24) & 0xFF), out);
  /* Show report */
  char strbuff1[64];
  char strbuff2[64];
#ifdef ORIGINAL_MODE
  fprintf(stderr, "Literal bytes........... %10d\n", typetally[0]);
  fprintf(stderr, "Mode 1 sectors.......... %10d\n", typetally[1]);
  fprintf(stderr, "Mode 2 form 1 sectors... %10d\n", typetally[2]);
  fprintf(stderr, "Mode 2 form 2 sectors... %10d\n", typetally[3]);
#else
  fprintf(stderr, "Literal bytes........... %10lld\n", typetally[0]);
  fprintf(stderr, "Mode 1 sectors.......... %10lld\n", typetally[1]);
  fprintf(stderr, "Mode 2 form 1 sectors... %10lld\n", typetally[2]);
  fprintf(stderr, "Mode 2 form 2 sectors... %10lld\n", typetally[3]);
#endif
  const off_t finalsize = ftell(out);
  fprintf(stderr, "Encoded %s -> %s\n", GetByteSize(intotallength, strbuff1), GetByteSize(finalsize, strbuff2));
  if (finalsize <= intotallength)
    fprintf(stderr, "Stripped file is %s smaller (%d%%)\n", GetByteSize(intotallength - finalsize, strbuff1), (int)(100 * (intotallength - finalsize) / intotallength));
  fprintf(stderr, "Done.\n");
  return 0;
}

/***************************************************************************/

int main(int argc, char **argv) {
  FILE *fin, *fout;
  char *infilename;
  char *outfilename;
  banner();
  /*
  ** Initialize the ECC/EDC tables
  */
  eccedc_init();
  /*
  ** Check command line
  */
  if((argc != 2) && (argc != 3)) {
    fprintf(stderr, "usage: %s cdimagefile [ecmfile]\n", argv[0]);
    return 1;
  }
  infilename = argv[1];
  /*
  ** Figure out what the output filename should be
  */
  if(argc == 3) {
    outfilename = argv[2];
  } else {
    outfilename = malloc(strlen(infilename) + 5);
    if(!outfilename) abort();
    sprintf(outfilename, "%s.ecm", infilename);
  }
  fprintf(stderr, "Encoding %s to %s.\n", infilename, outfilename);
  /*
  ** Open both files
  */
  fin = fopen(infilename, "rb");
  if(!fin) {
    perror(infilename);
    return 1;
  }
  fout = fopen(outfilename, "wb");
  if(!fout) {
    perror(outfilename);
    fclose(fin);
    return 1;
  }
  /*
  ** Encode
  */
  ecmify(fin, fout);
  /*
  ** Close everything
  */
  fclose(fout);
  fclose(fin);
  return 0;
}
