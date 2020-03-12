/*
  Simple virtual TCP COM port interface code with test program.

*/

#include <stdio.h>
#include <stdlib.h>
#include <stdio.h> 
#include <netdb.h> 
#include <netinet/in.h> 
#include <string.h> 
#include <sys/socket.h> 
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include "serial.h"

int open_vcom(char *hostname,int port,int baud)
{
  int fd;
  struct sockaddr_in addr;
  struct hostent *he;

  he=gethostbyname(hostname);
  if (!he) {
    perror("gethostbyname failed");
    return -1;
  }
  
  fd = socket(AF_INET, SOCK_STREAM, 0); 
  if (fd==-1) {
    perror("socket() failed");
    return -1;
  }
  
  bzero(&addr,sizeof(addr));
  addr.sin_family = AF_INET; 
  addr.sin_port = htons(port);
  memcpy(&addr.sin_addr, he->h_addr, he->h_length);

 if (connect(fd, (struct sockaddr *)&addr, sizeof(struct sockaddr)) == -1) {
   perror("connect() failed");
   return -1;
 }

 unsigned char buf[8192]={
   // Set serial port option
   0xFF, 0xFA, 0x2C,
   // serial port option 1 = SET BAUD RATE
   // value  is big endian 32-bit value. 0 = read value from server
   0x01,(baud>>24),(baud>>16)&0xff,(baud>>8)&0xff,baud&0xff,
   // IAC + SE to finish it
   0xFF,0xF0,

   // Set serial port option
   0xFF, 0xFA, 0x2C,
   // serial port option 5 = SET OPTION
   // Option 5, value 0 = disable flow control
   0x05,0x00,
   // IAC + SE to finish it
   0xFF,0xF0


   
 };
 // Tell other side to set the above options
 write(fd,buf,17);
 
 int flags;
 if ((flags = fcntl(fd, F_GETFL, NULL)) == -1)
   {
     perror("fcntl");
     return -1;
    }
 if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
   {
     perror("fcntl");
     return -1;
   }
 
 // Now do telnet options to set serial speed etc as required
 while(1) {
   int count=read(fd,buf,8192);
   if (count>0) {
     printf("[%d]",count); fflush(stdout);
     for(int i=0;i<count;i++) printf(" %02x",buf[i]);
     fflush(stdout);
   } else {
     usleep(10000);
   }
 }
 
 
 return fd;
 
}


#ifdef STANDALONE
int main(int argc,char *argv[])
{
  int fd=open_vcom(argv[1],atoi(argv[2]),atoi(argv[3]));

  return 0;
}
#endif
