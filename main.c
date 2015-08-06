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
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <strings.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <curl/curl.h>
#include <dirent.h>
#include <assert.h>

#include "lbard.h"

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
#define LINK_MTU 100
// The time it takes for a Bluetooth scan
//int message_update_interval=12;
int message_update_interval=2;      // faster for debugging
time_t last_message_update_time=0;

int main(int argc, char **argv)
{
  if (argc>4) prefix=strdup(argv[4]);
  
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

  if (argc>2) credential=argv[2];
  if (argc>1) servald_server=argv[1];

  while(1) {
    if (argc>2) load_rhizome_db(message_update_interval
				-(time(0)-last_message_update_time),
				prefix, servald_server,credential,&token);

    unsigned char msg_out[LINK_MTU];

    if ((time(0)-last_message_update_time)>=message_update_interval) {
      update_my_message(my_sid_hex,
			LINK_MTU,msg_out,
			servald_server,credential);
      last_message_update_time=time(0);
    }

    scan_for_incoming_messages();
    
    usleep(100000);
  }
}
