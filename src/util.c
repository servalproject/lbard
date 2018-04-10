
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <termios.h>

#include "sync.h"
#include "lbard.h"
#include "radios.h"

#ifdef WIN32
#include <windows.h>
#elif _POSIX_C_SOURCE >= 199309L
#include <time.h>   // for nanosleep
#else
#include <unistd.h> // for usleep
#endif

void sleep_ms(int milliseconds) // cross-platform sleep function
{
#ifdef WIN32
    Sleep(milliseconds);
#elif _POSIX_C_SOURCE >= 199309L
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;
    nanosleep(&ts, NULL);
#else
    usleep(milliseconds * 1000);
#endif
}


// From os.c in serval-dna
long long gettime_us()
{
  struct timeval nowtv;
  // If gettimeofday() fails or returns an invalid value, all else is lost!
  if (gettimeofday(&nowtv, NULL) == -1)
    return -1;
  if (nowtv.tv_sec < 0 || nowtv.tv_usec < 0 || nowtv.tv_usec >= 1000000)
    return -1;
  return nowtv.tv_sec * 1000000LL + nowtv.tv_usec;
}

// From os.c in serval-dna
long long gettime_ms()
{
  struct timeval nowtv;
  // If gettimeofday() fails or returns an invalid value, all else is lost!
  if (gettimeofday(&nowtv, NULL) == -1)
    return -1;
  if (nowtv.tv_sec < 0 || nowtv.tv_usec < 0 || nowtv.tv_usec >= 1000000)
    return -1;
  return nowtv.tv_sec * 1000LL + nowtv.tv_usec / 1000;
}

int chartohex(int c)
{
  if ((c>='0')&&(c<='9')) return c-'0';
  if ((c>='A')&&(c<='F')) return c-'A'+10;
  return -1;
}

int hextochar(int h)
{
  if ((h>=0)&&(h<10)) return h+'0';
  if ((h>=10)&&(h<16)) return h+'A'-10;
  return '?';
}

int nybltohexchar(int v)
{
  if (v<10) return '0'+v;
  return 'A'+v-10;
}

int ishex(int c)
{
  if (c>='0'&&c<='9') return 1;
  if (c>='A'&&c<='F') return 1;
  if (c>='a'&&c<='f') return 1;
  return 0;
}

int chartohexnybl(int c)
{
  if (c>='0'&&c<='9') return c-'0';
  if (c>='A'&&c<='F') return c-'A'+10;
  if (c>='a'&&c<='f') return c-'a'+10;
  return 0;
}


int hex_encode(unsigned char *in, char *out, int in_len, int radio_type)
{
  int out_ofs=0;
  int i;
  for(i=0;i<in_len;i++) {
    out[out_ofs++]=nybltohexchar(in[i]>>4);
    out[out_ofs++]=nybltohexchar(in[i]&0xf);
  }
  out[out_ofs]=0;
  return out_ofs;
}

int hex_decode(char *in, unsigned char *out, int out_len,int radio_type)
{
  int i;
  int out_count=0;

  for(i=0;i<strlen(in);i+=2) {
    int v=hextochar(in[i+0])<<4;
    v|=hextochar(in[i+1]);
    out[out_count++]=v;
  }
  out[out_count]=0;
  return out_count;
}

int ascii64_encode(unsigned char *in, char *out, int in_len, int radio_type)
{
  // ASCII-64 is use by HF ALE radio links. It is just ASCII codes 0x20 - 0x5f
  // On Barrett, nothing is escaped.
  // On Codan, spaces must be escaped

  int out_ofs=0;
  int i,j;
  for(i=0;i<in_len;i+=3) {
    // Encode 3 bytes using 4
    unsigned char ob[4];
    ob[0]=0x20+(in[i+0]&0x3f);
    ob[1]=0x20+((in[i+0]&0xc0)>>6)+((in[i+1]&0x0f)<<2);
    ob[2]=0x20+((in[i+1]&0xf0)>>4)+((in[i+2]&0x03)<<4);
    ob[3]=0x20+((in[i+2]&0xfc)>>2);

    // XXX - Character escaping policy should be set in radio driver
    // not in here!
    for(j=0;j<4;j++) {
      if ((ob[j]==' ')&&(radio_type==RADIOTYPE_HFCODAN)) {
	out[out_ofs++]='\\';
      }
      out[out_ofs++]=ob[j];
    }
  }
  out[out_ofs]=0;
  return 0;
}

int ascii64_decode(char *in, unsigned char *out, int out_len,int radio_type)
{
  int i;
  int out_ofs=0;
  for(i=0;in[i];i+=4) {
    unsigned char ob[3];
    ob[0]=(in[i+0]-0x20)&0x3f;
    ob[0]|=(((in[i+1]-0x20)&0x03)<<6);
    ob[1]=(((in[i+1]-0x20)&0x3c)>>2);
    ob[1]|=(((in[i+2]-0x20)&0x0f)<<4);
    ob[2]=(((in[i+2]-0x20)&0x30)>>4);
    ob[2]|=(((in[i+3]-0x20)&0x3f)<<2);
    out_ofs+=3;
  }

  return out_ofs;
}

int dump_bytes(FILE *f,char *msg, unsigned char *bytes, int length)
{
  fprintf(f,"%s:\n", msg);
  for (int i = 0; i < length; i += 16)
  {
    fprintf(f,"%04X: ", i);
    for (int j = 0; j < 16; j++)
      if (i + j < length)
        fprintf(f," %02X", bytes[i + j]);
    fprintf(f,"  ");
    for (int j = 0; j < 16; j++)
    {
      int c;
      if (i + j < length)
        c = bytes[i + j];
      else
        c = ' ';
      if (c < ' ')
        c = '.';
      if (c > 0x7d)
        c = '.';
      fprintf(f,"%c", c);
    }
    fprintf(f,"\n");
  }
  return 0;
}

/* Display a timestamp and current SID.
   This is used so that logs from multiple LBARD instances can be viewed together
   in the output logs of the automated tests, to help identify protocol problems,
   by making visible the sequence of events in chronological order.
   @author PGS 20180329
*/
char timestamp_str_out[1024];
char *timestamp_str(void)
{
  struct tm tm;
  time_t now=time(0);
  struct timeval tv;
  gettimeofday(&tv, NULL);
  localtime_r(&now,&tm);
  snprintf(timestamp_str_out,1024,"[%02d:%02d.%02d.%03d %c%c%c%c*]",
          tm.tm_hour,tm.tm_min,tm.tm_sec,(int)tv.tv_usec/1000,
          my_sid_hex[0],my_sid_hex[1],my_sid_hex[2],my_sid_hex[3]);
  return timestamp_str_out;
}

