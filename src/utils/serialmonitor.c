/*
Serval Distributed Numbering Architecture (DNA)
Copyright (C) 2012 - 2015 Serval Project Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
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

int set_nonblock(int fd)
{
  int flags;
  if ((flags = fcntl(fd, F_GETFL, NULL)) == -1)
    { perror("set_nonblock: fcntl(n,F_GETFL,NULL)"); return -1; }
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
    { perror("set_nonblock: fcntl(n,F_SETFL,n|O_NONBLOCK)"); return -1; }
  return 0;
}

int set_block(int fd)
{
  int flags;
  if ((flags = fcntl(fd, F_GETFL, NULL)) == -1)
    { perror("set_block: fcntl(n,F_GETFL,NULL)"); return -1; }
  if (fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == -1)
    { perror("set_block: fcntl(n,F_SETFL,n~O_NONBLOCK)"); return -1; }
  return 0;
}

ssize_t read_nonblock(int fd, void *buf, size_t len)
{
  ssize_t nread = read(fd, buf, len);
  if (nread == -1) {
    switch (errno) {
      case EINTR:
      case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
      case EWOULDBLOCK:
#endif
        return 0;
    }
    return -1;
  }
  return nread;
}

ssize_t write_all(int fd, const void *buf, size_t len)
{
  ssize_t written = write(fd, buf, len);
  if (written == -1)
    { perror("write_all(): written == -1"); return -1; }

  if ((size_t)written != len)
    { perror("write_all(): written != len"); return -1; }
  return written;
}

int serial_setup_port(int fd,int speed)
{
  struct termios t;

  tcgetattr(fd, &t);
  // XXX Speed and options should be configurable
  cfsetispeed(&t, speed);
  cfsetospeed(&t, speed);
  // 8N1
  t.c_cflag &= ~PARENB;
  t.c_cflag &= ~CSTOPB;
  t.c_cflag &= ~CSIZE;
  t.c_cflag |= CS8;

  t.c_lflag &= ~(ICANON | ISIG | IEXTEN | ECHO | ECHOE);
  /* Noncanonical mode, disable signals, extended
   input processing, and software flow control and echoing */
  
  t.c_iflag &= ~(BRKINT | ICRNL | IGNBRK | IGNCR | INLCR |
		 INPCK | ISTRIP | IXON | IXOFF | IXANY | PARMRK);
  /* Disable special handling of CR, NL, and BREAK.
   No 8th-bit stripping or parity error handling.
   Disable START/STOP output flow control. */
  
  // Enable CTS/RTS flow control (for now)
#ifndef CNEW_RTSCTS
  t.c_cflag |= CRTSCTS;
#else
  t.c_cflag |= CNEW_RTSCTS;
#endif

  // no output processing
  t.c_oflag &= ~OPOST;

  tcsetattr(fd, TCSANOW, &t);

  set_nonblock(fd);

  return 0;
}

#define MAX_PORTS 256
int port_count=0;
int fds[MAX_PORTS];

int dump_bytes(FILE *f,char *msg, unsigned char *bytes, int length)
{
  int retVal = -1;
  
  do 
  {
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

  return retVal;
}


int main(int argc,char **argv)
{
  int speed=atoi(argv[1]);

  for(int i=2;i<argc;i++) {  
    char *serial_port = argv[i];
    int serialfd = open(serial_port,O_RDWR);
    fprintf(stderr,"serialfd=%d\n",serialfd);
    serial_setup_port(serialfd,speed);
    fds[port_count++]=serialfd;
    fprintf(stderr,"Opening serial port '%s'\n",serial_port);
  }
  
  while(1) {
    int activity=0;
    unsigned char buffer[8192];
    for(int p=0;p<port_count;p++) {
      int n=read_nonblock(fds[p],buffer,8192);    
      if (n>0) {
	activity++;
	dump_bytes(stdout,argv[p+2],buffer,n);
      }
    }
    if (!activity) usleep(0);
  }
  
}
