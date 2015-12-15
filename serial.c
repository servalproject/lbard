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

  fprintf(stderr,"write_all(%d) sent %d bytes.\n",
	  (int)len,(int)written);

  FILE *f=fopen("/tmp/serial.dat","w");
  if (f) {
    fwrite(buf,len,1,f);
    fclose(f);
  }
  
  return written;
}

int serial_setup_port_with_speed(int fd,int speed)
{
  struct termios t;

  tcgetattr(fd, &t);

  fprintf(stderr,"Before serial port setup: t.c_cflag&CTSRTS=0x%lx\n",
	  (long unsigned int)
#ifndef CNEW_RTSCTS
	  t.c_cflag & CRTSCTS
#else
	  t.c_cflag & CNEW_RTSCTS
#endif
	  );
	  
  speed_t baud_rate;
  switch(speed){
  case 0: baud_rate = B0; break;
  case 50: baud_rate = B50; break;
  case 75: baud_rate = B75; break;
  case 110: baud_rate = B110; break;
  case 134: baud_rate = B134; break;
  case 150: baud_rate = B150; break;
  case 200: baud_rate = B200; break;
  case 300: baud_rate = B300; break;
  case 600: baud_rate = B600; break;
  case 1200: baud_rate = B1200; break;
  case 1800: baud_rate = B1800; break;
  case 2400: baud_rate = B2400; break;
  case 4800: baud_rate = B4800; break;
  case 9600: baud_rate = B9600; break;
  case 19200: baud_rate = B19200; break;
  case 38400: baud_rate = B38400; break;
  default:
  case 57600: baud_rate = B57600; break;
  case 115200: baud_rate = B115200; break;
  case 230400: baud_rate = B230400; break;
  }

  // XXX Speed and options should be configurable
  if (cfsetospeed(&t, baud_rate)) return -1;    
  if (cfsetispeed(&t, baud_rate)) return -1;

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
  
  // Disable CTS/RTS flow control
#ifndef CNEW_RTSCTS
  t.c_cflag &= ~CRTSCTS;
#else
  t.c_cflag &= ~CNEW_RTSCTS;
#endif

  // no output processing
  t.c_oflag &= ~OPOST;

  tcsetattr(fd, TCSANOW, &t);
  
  set_nonblock(fd);

    tcgetattr(fd, &t);

  fprintf(stderr,"After serial port setup: t.c_cflag&CTSRTS=0x%lx\n",
	  (long unsigned int)
#ifndef CNEW_RTSCTS
	  t.c_cflag & CRTSCTS
#else
	  t.c_cflag & CNEW_RTSCTS
#endif
	  );

  return 0;
}

int serial_setup_port(int fd)
{
  return serial_setup_port_with_speed(fd,B230400);
}
