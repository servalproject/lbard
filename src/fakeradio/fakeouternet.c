/*
  Simulate an Outernet satellite uplink and Outernet receiver, to allow
  automated tests of the Outernet data path.

  The outernet service send acknowledgement UDP packets that match that
  which was sent.

  The receiver side writes the received packets to a named UNIX socket.

*/

#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <sys/un.h>
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

#include "code_instrumentation.h"

int fd=-1;
int named_socket=-1;

int set_nonblock(int fd)
{
  int retVal=0;

  LOG_ENTRY;

  do {
    if (fd==-1) break;
      
    int flags;
    if ((flags = fcntl(fd, F_GETFL, NULL)) == -1)
      {
	perror("fcntl");
	LOG_ERROR("set_nonblock: fcntl(%d,F_GETFL,NULL)",fd);
	retVal=-1;
	break;
      }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
    {
      perror("fcntl");
      LOG_ERROR("set_nonblock: fcntl(%d,F_SETFL,n|O_NONBLOCK)",fd);
      return -1;
    }
  } while (0);
  LOG_EXIT;
  return retVal;
}

int setup_udp(int port)
{
  /* Open UDP socket for receiving packets for uplink, and as source
     for UDP packet announcements as we pretend to be the receiver.

  */

  int retVal=-1;
  struct sockaddr_in hostaddr;

  LOG_ENTRY;

  do {
      bzero(&hostaddr, sizeof(struct sockaddr_in));
      
      // Set up address for our side of the socket
      hostaddr.sin_family = AF_INET;
      hostaddr.sin_port = htons(port);
      hostaddr.sin_addr.s_addr = htonl(INADDR_ANY);
      
      LOG_NOTE("Checkpoint (fileno(stdin,stdout,stderr)=%d,%d,%d",fileno(stdin),fileno(stdout),fileno(stderr));
      fd=socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
      if (fd==0) {
	LOG_NOTE("Er, socket() returned fd #0, which seems odd. Trying again.");
	fd=socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	LOG_NOTE("Got fd=%d on second try",fd);
      }
      if (fd == -1)
	{
	  perror("Failed to create UDP socket");
	  LOG_ERROR("Failed to create UDP socket");
	  break;
	}
      LOG_NOTE("fd=%d",fd);
      
      if( bind(fd, (struct sockaddr*)&hostaddr, sizeof(struct sockaddr_in) ) == -1)
	{
	  perror("Failed to bind UDP socket");
	  LOG_ERROR("Failed to bind UDP socket");
	  break;
	}
      
      int broadcastEnable=1;
      int ret=setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &broadcastEnable, sizeof(broadcastEnable));
      if (ret) {
	LOG_NOTE("Failed to set SO_BROADCAST");
	break;
      }

      set_nonblock(fd);
      
      // Successfully connected
      LOG_NOTE("Established UDP socket");
      retVal=0;
  } while(0);
  
  LOG_EXIT;
  return retVal;
}

int setup_named_socket(char *sock_path)
{
  int retVal=0;
  LOG_ENTRY;
  do {
    
    named_socket = socket( AF_UNIX, SOCK_DGRAM, 0 );
    
    if( named_socket < 0 ) {
      LOG_ERROR("socket() failed: (%i) %m", errno );
      retVal=-1; break;
    }
    
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    if (setsockopt(named_socket, SOL_SOCKET, SO_RCVTIMEO,&tv,sizeof(tv)) < 0) {
      LOG_ERROR("setsockopt failed: (%i) %m", errno );
      retVal=-1; break;
    }
    
    unlink(sock_path);
    
    // Bind to unix socket
    struct sockaddr_un sun;
    sun.sun_family = AF_UNIX;
    snprintf( sun.sun_path, sizeof( sun.sun_path ), "%s", sock_path );
    
    LOG_NOTE("Checkpoint");
    if( -1 == bind( named_socket, (struct sockaddr *)&sun, sizeof( struct sockaddr_un ))) {
      LOG_ERROR("bind failed: (%i) %m", errno );
      retVal=-1; break;
    }

    LOG_NOTE("Setup named socket.");
    
  } while (0);
    
  LOG_EXIT;
  return retVal;  
}


int main(int argc,char **argv)
{
  unsigned char buffer[8192];
  struct sockaddr_in broadcast;
  bzero(&broadcast,sizeof(broadcast));
  broadcast.sin_family = AF_INET;
  broadcast.sin_port = htons(0x4f4e);  // ON in HEX for port number
  broadcast.sin_addr.s_addr = 0xffffff7f; // 127.255.255.255
  
  int retVal=-1;
  do {

    if (argc!=3) {
      LOG_ERROR("You must provide UDP port and named socket path on command line.");
      break;
    }
    
    // Get UDP port ready
    if (setup_udp(atoi(argv[1]))) break; // OU in HEX for port number
    LOG_NOTE("argv[2]='%s'",argv[2]?argv[2]:"(null)");
    if (setup_named_socket(argv[2])) break; // OU in HEX for port number

    while(1) {
      // Check for incoming UDP packets, and bounce them back out again
      // on the named socket
      // XXX - We should add latency to simulate the space segment.
      unsigned char sender_buf[1024];
      struct sockaddr_in *sender=sender_buf;
      socklen_t sender_len=sizeof(sender_buf);
      int len=0;
      len=recvfrom(fd,&buffer[0],1024,0,(struct sockaddr *)&sender,&sender_len);
      if (len>0) {
	printf("Received UDP packet of %d bytes\n",len);
	// Echo back to sender
	int result=sendto(fd,buffer,len,0,(struct sockaddr *)&sender,sender_len);
	if (result) {
	  perror("sendto(UDP) failed");
	}
	// Write to named socket
	result=sendto(named_socket,buffer,len,0,0,0);
	if (result) {
	  perror("sendto(UNIXDOMAIN) failed");
	}
	
      } else {
	usleep(10000);
      }      
    }
    
  } while(0);

  return retVal;
}

