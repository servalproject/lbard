/*
Serval Low-bandwidth asychronous Rhizome Demonstrator.
Copyright (C) 2015 Serval Project Inc.

This program monitors a local Rhizome database and attempts
to synchronise it over low-bandwidth declarative transports, 
such as bluetooth name or wifi-direct service information
messages.  It is intended to give a high priority to MeshMS
converations among nearby nodes.

The design is fully asynchronous, so a call to the update_my_message()
function from time to time should be all that is required.


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

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <assert.h>
#include <fcntl.h>

#include "lbard.h"
#include "serial.h"

unsigned char my_sid[32];
char *my_sid_hex=NULL;
char *servald_server="";
char *credential="";
char *prefix="";

char *token=NULL;

int scan_for_incoming_messages()
{
  DIR *d;
  struct dirent *de;

  d=opendir(".");

  while((de=readdir(d))!=NULL) {
    int len=strlen(de->d_name);
    if (len>strlen(".lbard-message")) {
      if (!strcmp(".lbard-message",&de->d_name[len-strlen(".lbard-message")])) {
	// Found a message
	FILE *f=fopen(de->d_name,"r");
	unsigned char msg_body[8192];
	int len=fread(msg_body,1,8192,f);
	saw_message(msg_body,len,
		    my_sid_hex,prefix, servald_server,credential);
	fclose(f);
      }
    }
  }

  closedir(d);
  return 0;
}

/*
  RFD900 has 255 byte maximum frames, but some bytes get taken in overhead.
  We then Reed-Solomon the body we supply, which consumes a further 32 bytes.
  This leaves a practical limit of somewhere around 200 bytes.
  Fortunately, they are 8-bit bytes, so we can get quite a bit of information
  in a single frame. 
  We have to keep to single frames, because we will have a number of radios
  potentially transmitting in rapid succession, without a robust collision
  avoidance system.
 
*/
#define LINK_MTU 200
// One message per second on RFD900
// XXX - Randomise, and allow for fractional second delays.
int message_update_interval=1000;  // ms
long long last_message_update_time=0;

int main(int argc, char **argv)
{
  if (argc!=5) {
    fprintf(stderr,"usage: lbard <servald hostname:port> <servald credential> <my sid> <serial port>\n");
    exit(-1);
  }

  last_message_update_time=0;
  
  int serialfd=-1;
  char *serial_port = argv[4];
  serialfd = open(serial_port,O_RDWR);
  if (serialfd<0) {
    perror("Opening serial port");
    exit(-1);
  }
  if (serial_setup_port(serialfd))
    {
      fprintf(stderr,"Failed to setup serial port. Exiting.\n");
      exit(-1);
    }
  fprintf(stderr,"Serial port open as fd %d\n",serialfd);
  
  prefix=strdup(argv[3]); prefix[6]=0;
  
  if (argc>3) {
    // set my_sid from argv[3]
    for(int i=0;i<32;i++) {
      char hex[3];
      hex[0]=argv[3][i*2];
      hex[1]=argv[3][i*2+1];
      hex[2]=0;
      my_sid[i]=strtoll(hex,NULL,16);
    }
    my_sid_hex=argv[3];
  }

  printf("My SID prefix is %02X%02X%02X%02X%02X%02X\n",
	 my_sid[0],my_sid[1],my_sid[2],my_sid[3],my_sid[4],my_sid[5]);
  
  
  if (argc>2) credential=argv[2];
  if (argc>1) servald_server=argv[1];

  long long next_rhizome_db_load_time=0;
  while(1) {
    if (argc>2)
      if (next_rhizome_db_load_time<=gettime_ms()) {
	long long load_timeout=message_update_interval
	  -(gettime_ms()-last_message_update_time);
	if (load_timeout<100) load_timeout=100;
	load_rhizome_db(load_timeout,
			prefix, servald_server,credential,&token);
	next_rhizome_db_load_time=gettime_ms()+3000;
      }

    unsigned char msg_out[LINK_MTU];

    if ((gettime_ms()-last_message_update_time)>=message_update_interval) {
      // fprintf(stderr,"Updating my message...\n");
      scan_for_incoming_messages();
      radio_read_bytes(serialfd);
      update_my_message(serialfd,
			my_sid,
			LINK_MTU,msg_out,
			servald_server,credential);
      // sending the bytes is now handled in txmessages.c and radio.c
      //      write_all(serialfd,msg_out,LINK_MTU);
      last_message_update_time=gettime_ms();
    }

    usleep(100000);

    
  }
}
