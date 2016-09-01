/*
Serval Distributed Numbering Architecture (DNA)
Copyright (C) 2012 - 2016 Serval Project Inc.

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
#include <string.h>
#include <stdio.h>

#include "sync.h"
#include "lbard.h"

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

  if (0) fprintf(stderr,"write_all(%d) sent %d bytes.\n",
		 (int)len,(int)written);
  
  return written;
}

int serial_setup_port_with_speed(int fd,int speed)
{
  struct termios t;

  tcgetattr(fd, &t);
  fprintf(stderr,"Serial port settings before tcsetaddr: c=%08x, i=%08x, o=%08x, l=%08x\n",
	  (unsigned int)t.c_cflag,(unsigned int)t.c_iflag,
	  (unsigned int)t.c_oflag,(unsigned int)t.c_lflag);
	  
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
  t.c_cflag |= CLOCAL;

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

  fprintf(stderr,"Serial port settings attempting ot be set: c=%08x, i=%08x, o=%08x, l=%08x\n",
	  (unsigned int)t.c_cflag,(unsigned int)t.c_iflag,
	  (unsigned int)t.c_oflag,(unsigned int)t.c_lflag);
  
  tcsetattr(fd, TCSANOW, &t);

  tcgetattr(fd, &t);
  fprintf(stderr,"Serial port settings after tcsetaddr: c=%08x, i=%08x, o=%08x, l=%08x\n",
	  (unsigned int)t.c_cflag,(unsigned int)t.c_iflag,
	  (unsigned int)t.c_oflag,(unsigned int)t.c_lflag);

  
  set_nonblock(fd);
  
  return 0;
}

int serial_setup_port(int fd)
{
  /* Try to work out what type of radio we are using.
     We support RFD900-series radios at 230400bps, and also
     Codan and Barrett HF radios at 9600.  The HF radios use simple text commands for
     everything, and are always in command mode, so we can first try to detect if the
     radios are HF radios, and set our radio mode and serial speed accordingly.
  */
  unsigned char buf[8192];
  unsigned clr[3]={21,13,10};
  int verhi,verlo;

  unsigned char barrett_e0_string[6]={0x13,'E','0',13,10,0x11};
  
  fprintf(stderr,"Attempting to detect radio type.\n");
  
  // Set serial port for HF radios
  serial_setup_port_with_speed(fd,9600);
  write_all(fd,clr,3); // Clear any partial command
  sleep(1); // give the radio the chance to respond
  ssize_t count = read_nonblock(fd,buf,8192);  // read and ignore any stuff
  dump_bytes("modem response to clr string",buf,count);
  fprintf(stderr,"Autodetecting Codan/Barrett HF Radio...\n");
  write_all(fd,"VER\r",4); // ask Codan radio for version
  sleep(1); // give the radio the chance to respond
  count = read_nonblock(fd,buf,8192);  // read reply
  dump_bytes("modem response",buf,count);
  // If we get a version string -> Codan HF
  if (sscanf((char *)buf,"VER\r\nCICS: V%d.%d",&verhi,&verlo)==2) {
    fprintf(stderr,"Codan HF Radio running CICS V%d.%d\n",
	    verhi,verlo);
    if ((verhi>3)||((verhi==3)&&(verlo>=37)))
      // Codan radio supports ALE 3G (255 x 8-bit chars per message)
      radio_set_feature(RADIO_ALE_2G|RADIO_ALE_3G);
    else
      // Codan radio supports only ALE 2G (90 x 6-bit chars per message)
      radio_set_feature(RADIO_ALE_2G);
    radio_set_type(RADIO_CODAN_HF);
    return 0;
  } else if (!memcmp(buf,barrett_e0_string,6)) {
    fprintf(stderr,"Detected Barrett HF Radio.\n");
    radio_set_type(RADIO_BARRETT_HF);
    radio_set_feature(RADIO_ALE_2G);

    // Tell Barrett radio we want to know when various events occur.
    char *setup_string[7]={"ARAMDM1\r\n","ARAMDP1\r\n",
			   "ARCALL1\r\n","ARLINK1\r\n",
			   "ARLTBL1\r\n","ARMESS1\r\n",
			   "ARSTAT1\r\n",
    };
    int i;
    for(i=0;i<7;i++) {
      write(fd,setup_string[i],strlen(setup_string[i]));
      usleep(200000);
      count = read_nonblock(fd,buf,8192);  // read reply
      dump_bytes(setup_string[i],buf,count);
    }

    
    return 0;
  }
  
  // If we get a Barrett error message -> Barrett HF
  // Anything else -> assume RFD900
  fprintf(stderr,"No HF radio detected, assuming RFD900 series radio.\n");
  
  serial_setup_port_with_speed(fd,230400);
  radio_set_type(RADIO_RFD900);
  return 0;
}
