
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>

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


int dump_bytes(char *msg, unsigned char *bytes, int length)
{
  printf("%s:\n", msg);
  for (int i = 0; i < length; i += 16)
  {
    printf("%04X: ", i);
    for (int j = 0; j < 16; j++)
      if (i + j < length)
        printf(" %02X", bytes[i + j]);
    printf("  ");
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
      printf("%c", c);
    }
    printf("\n");
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

