
/*
The following specially formatted comments tell the LBARD build environment about this radio.
See radio_type for the meaning of each field.
See radios.h target in Makefile to see how this comment is used to register support for the radio.

RADIO TYPE: OUTERNET,"outernet","Outernet.is broadcast satellite",outernet_radio_detect,outernet_serviceloop,outernet_receive_bytes,outernet_send_packet,outernet_check_if_ready,10

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

int outernet_radio_detect(int fd)
{
  return -1;
}

int outernet_check_if_ready(void)
{
  return -1;
}

int outernet_serviceloop(int serialfd)
{

  
  return 0;
}

int outernet_receive_bytes(unsigned char *bytes,int count)
{ 

  return 0;
}

int outernet_send_packet(int serialfd,unsigned char *out, int len)
{
  
  return 0;
}

