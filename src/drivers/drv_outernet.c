
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
#include <arpa/inet.h>
#include <netdb.h>

#include "sync.h"
#include "lbard.h"
#include "hf.h"
#include "radios.h"
#include "code_instrumentation.h"

// Import serial_port string from main.c
extern char *serial_port;

int outernet_radio_detect(int fd)
{
  /*
    The outernet.is satellite service has a UDP packet injection interface,
    and the receivers also provide received packets from the satellite via
    UDP packets, so we can use a common configuration scheme for both.
    Well, actually, we want LBARD to accept packets from a nearby Outernet 
    receiver, even if it is using a different radio driver, so we really
    only want a driver for the uplink side of the Outernet service.

    Basically we want to open a UDP socket to the outernet server, which we
    will parse from the serial_port string.    
  */

  char hostname[1024]="";
  int port=-1;

  struct in_addr hostaddr={0};
  
  if (sscanf(serial_port,"outernet://%[^/]:%d",hostname,&port)==2) {
    fprintf(stderr,"Parsed Outernet URI. Host='%s', port='%d'\n",hostname,port);
    LOG_NOTE("Parsed Outernet URI. Host='%s', port='%d'\n",hostname,port);

    if (!inet_aton(hostname,&hostaddr)) {
      LOG_NOTE("Parsed hostname as IPv4 address");
    } else {
    
      struct hostent *he=gethostbyname(hostname);
      
      if (!he) {
	fprintf(stderr,"Failed to resolve hostname '%s' to IP",hostname);
	LOG_ERROR("Failed to resolve hostname '%s' to IP",hostname);
	exit(-1);
      }
      struct in_addr **addr_list=(struct in_addr **) he->h_addr_list;
      
      if (!addr_list) {
	fprintf(stderr,"Could not get IP for hostname '%s' (h_addr_list empty)",hostname);
	LOG_ERROR("Could not get IP for hostname '%s' (h_addr_list empty)",hostname);
	exit(-1);
      }

      hostaddr=*addr_list[0];
    }
    
    struct sockaddr_in addr_us,addr_them;

    bzero((char *) &addr_us, sizeof(struct sockaddr_in));
    bzero((char *) &addr_them, sizeof(struct sockaddr_in));

    // Set up address for our side of the socket
    addr_us.sin_family = AF_INET;
    addr_us.sin_port = htons(port);
    addr_us.sin_addr.s_addr = htonl(INADDR_ANY);

    // Setup address for Outernet's server
    addr_them.sin_family = AF_INET;
    addr_them.sin_port = htons(port);
    addr_them.sin_addr.s_addr = hostaddr.s_addr;
    
    int s;
    if ((s=socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
    {
      perror("Failed to create UDP socket");
      LOG_ERROR("Failed to create UDP socket");
      exit(-1);
    }
    
  }
  
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

