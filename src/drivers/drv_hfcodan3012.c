
/*
The following specially formatted comments tell the LBARD build environment about this radio.
See radio_type for the meaning of each field.
See radios.h target in Makefile to see how this comment is used to register support for the radio.

RADIO TYPE: HFCODAN3012,"hfcodan3012","Codan HF with 3012 Data Modem",hfcodan3012_radio_detect,hfcodan3012_serviceloop,hfcodan3012_receive_bytes,hfcodan3012_send_packet,hfcodan32_radio_check_if_ready,10

*/

#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <time.h>
#include <termios.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>

#include "sync.h"
#include "lbard.h"
#include "hf.h"
#include "radios.h"

int hfcodan3021_initialise(int serialfd);

int hfcodan3012_radio_detect(int fd)
{
  // We require a serial port
  if (fd==-1) return -1;
  
  return -1;
}

int hfcodan3012_serviceloop(int serialfd)
{
  char cmd[1024];
  
  switch(hf_state) {
  case HF_DISCONNECTED:
    break;
  case HF_CALLREQUESTED:
    break;
  case HF_CONNECTING:
    break;
  case HF_ALELINK:
    break;
  case HF_DISCONNECTING:
    break;
  default:
    break;
  }
  
  return 0;
}

int hfcodan3012_process_line(char *l)
{
  int channel,caller,callee,day,month,hour,minute;
  
  return 0;
}

int hfcodan3012_receive_bytes(unsigned char *bytes,int count)
{ 
  int i;
  for(i=0;i<count;i++) {
    if (bytes[i]==13||bytes[i]==10) {
      hf_response_line[hf_rl_len]=0;
      if (hf_rl_len) hfcodan3012_process_line(hf_response_line);
      hf_rl_len=0;
    } else {
      if (hf_rl_len<1024) hf_response_line[hf_rl_len++]=bytes[i];
    }
  }
  
  return 0;
}

int hfcodan3012_send_packet(int serialfd,unsigned char *out, int len)
{
  
  return 0;
}

