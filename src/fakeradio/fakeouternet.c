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
#include <sys/time.h>

#include "code_instrumentation.h"

int fd=-1;
int named_socket=-1;

int dump_bytes(FILE *f,char *msg, unsigned char *bytes, int length)
{
  int retVal = -1;

  LOG_ENTRY;

  do 
  {
#if COMPILE_TEST_LEVEL >= TEST_LEVEL_LIGHT
    if (! msg) 
    {
      LOG_ERROR("msg is null");
      break;
    }
    if (! bytes) 
    {
      LOG_ERROR("bytes is null");
      break;
    }
#endif
    fprintf(f, "%s:\n", msg);
    for (int i = 0; i < length; i += 16)
    {
      fprintf(f, "%04X: ", i);
      for (int j = 0; j < 16; j++)
        if (i + j < length)
          fprintf(f, " %02X", bytes[i + j]);
      fprintf(f, "  ");
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
       fprintf(f, "%c", c);
      }
      fprintf(f, "\n");
    }
    retVal = 0;
  }
  while (0);

  LOG_EXIT;

  return retVal;
}


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

long long gettime_ms()
{
  long long retVal = -1;

  LOG_ENTRY;

  do
  {
    struct timeval nowtv;

    // If gettimeofday() fails or returns an invalid value, all else is lost!
    if (gettimeofday(&nowtv, NULL) == -1)
    {
      LOG_ERROR("gettimeofday returned -1");
      break;
    }

    if (nowtv.tv_sec < 0 || nowtv.tv_usec < 0 || nowtv.tv_usec >= 1000000)
    {
      LOG_ERROR("gettimeofday returned invalid value");
      break;      
    }

    retVal = nowtv.tv_sec * 1000LL + nowtv.tv_usec / 1000;
  }
  while (0);

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

  unsigned char sender_buf[1024];
  struct sockaddr_in *sender=(struct sockaddr_in *)sender_buf;
  socklen_t sender_len=sizeof(sender_buf);
  
  int queued_ack_len=0;
  unsigned char queued_ack_body[8192];
  long long queued_ack_release_time=0;
  unsigned char queued_sender[1024];
  int queued_sender_len;
  
  int retVal=-1;
  do {

    if (argc!=3) {
      LOG_ERROR("You must provide UDP port and named socket path on command line.");
      break;
    }
    
    // Get UDP port ready
    if (setup_udp(atoi(argv[1]))) break; 
    LOG_NOTE("argv[2]='%s'",argv[2]?argv[2]:"(null)");
    if (setup_named_socket(argv[2])) break; 

    while(1) {
      // Check for incoming UDP packets, and bounce them back out again
      // on the named socket
      sender_len=sizeof(sender_buf);
      int len=0;
      len=recvfrom(fd,&buffer[0],1024,0,(struct sockaddr *)&sender_buf[0],&sender_len);
      if (len>0) {
	printf("Received UDP packet of %d bytes\n",len);
	dump_bytes(stderr,"The sender",&sender_buf[0],sender_len);
	dump_bytes(stderr,"The packet",buffer,len);

	// Wait 1000ms before acking packet
	queued_ack_len=len;
	memcpy(queued_ack_body,buffer,len);
	queued_ack_release_time=gettime_ms()+1000;
	memcpy(queued_sender,sender_buf,1024);
	queued_sender_len=sender_len;

	dump_bytes(stderr,"Queued sender",&queued_sender[0],queued_sender_len);
	dump_bytes(stderr,"Queued packet",queued_ack_body,queued_ack_len);
	
      } else {
	usleep(10000);

	if (queued_ack_len&&gettime_ms()>=queued_ack_release_time) {
	  LOG_NOTE("Sending ACK of last packet: len=%d, slen=%d",queued_ack_len,queued_sender_len);
	  dump_bytes(stderr,"Queued sender",&queued_sender[0],queued_sender_len);
	  dump_bytes(stderr,"Queued packet",queued_ack_body,queued_ack_len);
	  dump_bytes(stderr,"The packet",queued_ack_body,queued_ack_len);
	  // Echo back to sender

	  int result=sendto(fd,queued_ack_body,queued_ack_len,0,
			    (struct sockaddr *)&queued_sender[0],queued_sender_len);
	  if (result!=queued_ack_len) {
	    perror("sendto(UDP) failed");
	  } else LOG_NOTE("UDP socket send ok.");
	  // Write to named socket
	  result=sendto(named_socket,queued_ack_body,queued_ack_len,0,0,0);
	  if (result) {
	    perror("sendto(UNIXDOMAIN) failed");
	    LOG_NOTE("result=%d",result);
	  } else LOG_NOTE("UNIX socket send ok.");
	  queued_ack_len=0;
	}

	
      }      
    }
    
  } while(0);

  return retVal;
}

