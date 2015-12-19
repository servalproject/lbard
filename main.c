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
#include <time.h>
#include <sys/time.h>

#include "lbard.h"
#include "serial.h"

int debug_pieces=0;
int debug_announce=0;
int debug_pull=0;
int debug_insert=0;

int udp_time=0;
int time_slave=0;
int time_server=0;
char time_broadcast_addr[1024]="255.255.255.255";

int reboot_when_stuck=0;
extern int serial_errors;

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
// About one message per second on RFD900
// We add random()%250 ms to this, so we deduct half of that from the base
// interval, so that on average we obtain one message per second.
// 128K air speed / 230K serial speed means that we can in principle send
// about 128K / 256 = 512 packets per second. However, the FTDI serial USB
// drivers for Mac crash well before that point.
int message_update_interval=AVG_PACKET_TX_INTERVAL-(PACKET_TX_INTERVAL_RANDOMNESS/2);  // ms
int message_update_interval_randomness=PACKET_TX_INTERVAL_RANDOMNESS;
long long last_message_update_time=0;

time_t last_summary_time=0;
time_t last_status_time=0;

int monitor_mode=0;

int main(int argc, char **argv)
{

  // For Watcharachai's PhD experiments.  Everyone else can safely ignore this option
  if ((argc==7)&&(!strcasecmp(argv[1],"energysample"))) {
    char *port=argv[2];
    float pulse_width_ms=atof(argv[3]);
    float pulse_frequency=atoi(argv[4]);
    int wifiup_hold_time_ms=atoi(argv[5]);
    char *interface=argv[6];
    return energy_experiment(port,pulse_frequency,pulse_width_ms,wifiup_hold_time_ms,
			     interface);
  }

  char *serial_port = "/dev/null";

  if ((argc==3)&&(!strcasecmp(argv[1],"monitor"))) {
    monitor_mode=1;
    serial_port=argv[2];
  } else {  
    if (argc<5) {
      fprintf(stderr,"usage: lbard <servald hostname:port> <servald credential> <my sid> <serial port> [options ...]\n");
      fprintf(stderr,"usage: lbard monitor <serial port>\n");
      exit(-1);
    }
    serial_port = argv[4];
  }

  int serialfd=-1;
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

      
  int n=5;
  while (n<argc) {
    if (argv[n]) {
      if (!strcasecmp("monitor",argv[n])) monitor_mode=1;
      else if (!strcasecmp("meshmsonly",argv[n])) { meshms_only=1;
	fprintf(stderr,"Only MeshMS bundles will be carried.\n");
      }
      else if (!strncasecmp("minversion=",argv[n],11)) {
	int day,month,year;
	min_version=strtoll(&argv[n][11],NULL,10)*1000LL;
	if (sscanf(argv[n],"minversion=%d/%d/%d",&year,&month,&day)==3) {
	  // Minimum date has been specified using year/month/day
	  // Calculate min_version from that.
	  struct tm tm;
	  bzero(&tm,sizeof(struct tm));
	  tm.tm_mday=day;
	  tm.tm_mon=month-1;
	  tm.tm_year=year-1900;
	  time_t thetime = mktime(&tm);
	  min_version=((long long)thetime)*1000LL;
	}
	time_t mv=(min_version/1000LL);

	// Get minimum time as non NL terminated string
	char stringtime[1024];
	snprintf(stringtime,1024,"%s",ctime(&mv));
	if (stringtime[strlen(stringtime)-1]=='\n') stringtime[strlen(stringtime)-1]=0;

	fprintf(stderr,"Only bundles newer than epoch+%lld msec (%s) will be carried.\n",
		(long long)min_version,stringtime);
      }
      else if (!strcasecmp("rebootwhenstuck",argv[n])) reboot_when_stuck=1;
      else if (!strcasecmp("timeslave",argv[n])) time_slave=1;
      else if (!strcasecmp("timemaster",argv[n])) time_server=1;
      else if (!strncasecmp("timebroadcast=",argv[n],14))
	       strcpy(time_broadcast_addr,&argv[n][14]);
      else if (!strcasecmp("logrejects",argv[n])) debug_insert=1;
      else if (!strcasecmp("pull",argv[n])) debug_pull=1;
      else if (!strcasecmp("pieces",argv[n])) debug_pieces=1;
      else if (!strcasecmp("announce",argv[n])) debug_announce=1;
      else if (!strcasecmp("udptime",argv[n])) udp_time=1;
      else if (!strcasecmp("rapidfire",argv[n])) {
	// Send packets fast in this mode -- primarily used for bench testing
	message_update_interval=200;
	message_update_interval_randomness=100;
      } else {
	fprintf(stderr,"Illegal mode '%s'\n",argv[n]);
	exit(-3);
      }
    }
    n++;
  }

  if (message_update_interval<0) message_update_interval=0;
  
  last_message_update_time=0;
  
  my_sid_hex="00000000000000000000000000000000";
  prefix="000000";
  if (!monitor_mode) {
    prefix=strdup(argv[3]);
    if (strlen(prefix)<32) {
      fprintf(stderr,"You must provide a valid SID for the ID of the local node.\n");
      exit(-1);
    }
    prefix[6]=0;  
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
  }

  printf("My SID prefix is %02X%02X%02X%02X%02X%02X\n",
	 my_sid[0],my_sid[1],my_sid[2],my_sid[3],my_sid[4],my_sid[5]);
  
  if (argc>2) credential=argv[2];
  if (argc>1) servald_server=argv[1];

  // Open UDP socket to listen for time updates from other LBARD instances
  // (poor man's NTP for LBARD nodes that lack internal clocks)
  int timesocket=-1;
  if (udp_time) {
    timesocket=socket(AF_INET, SOCK_DGRAM, 0);
    if (timesocket!=-1) {
      struct sockaddr_in addr;
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = INADDR_ANY;
      addr.sin_port = htons(0x5401);
      bind(timesocket, (struct sockaddr *) &addr, sizeof(addr));
      set_nonblock(timesocket);
    }
  }
  
  long long next_rhizome_db_load_time=0;
  while(1) {    
    if (argc>2)
      if (next_rhizome_db_load_time<=gettime_ms()) {
	long long load_timeout=message_update_interval
	  -(gettime_ms()-last_message_update_time);

	// Don't wait around forever for rhizome -- we want to receive inbound
	// messages within a reasonable timeframe to prevent input buffer overflow,
	// and peers wasting time sending bundles that we already know about, and that
	// we can inform them about.
	if (load_timeout>1500) load_timeout=1500;
	
	if (load_timeout<500) load_timeout=500;
	if (!monitor_mode)
	  load_rhizome_db(load_timeout,
			  prefix, servald_server,credential,&token);
	next_rhizome_db_load_time=gettime_ms()+3000;
      }

    unsigned char msg_out[LINK_MTU];

    scan_for_incoming_messages();
    radio_read_bytes(serialfd,monitor_mode);

    // timesync.c can cause the system time to be altered, in which case
    // our message update timer might end up being badly wrong, e.g., waiting
    // 45 years before sending the next packet is sent, if time is stepped
    // backwards.  So limit to 5 seconds always.
    if ((gettime_ms()-last_message_update_time)>5000) {
      last_message_update_time=gettime_ms()+5000;
    }
    
    if ((gettime_ms()-last_message_update_time)>=message_update_interval) {

      if (!time_server) {
	// Decay my time stratum slightly
	if (my_time_stratum<0xffff)
	  my_time_stratum++;
      } else my_time_stratum=1;
      // Send time packet
      if (udp_time) {
	{
	  // Occassionally announce our time
	  // T + (our stratum) + (64 bit seconds since 1970) +
	  // + (24 bit microseconds)
	  // = 1+1+8+3 = 13 bytes
	  struct timeval tv;
	  gettimeofday(&tv,NULL);    
	  
	  unsigned char msg_out[1024];
	  int offset=0;
	  msg_out[offset++]='T';
	  msg_out[offset++]=my_time_stratum>>8;
	  for(int i=0;i<8;i++)
	    msg_out[offset++]=(tv.tv_sec>>(i*8))&0xff;
	  for(int i=0;i<3;i++)
	    msg_out[offset++]=(tv.tv_usec>>(i*8))&0xff;
	  // Now broadcast on every interface to port 0x5401
	  // Oh that's right, UDP sockets don't have an easy way to do that.
	  // We could interrogate the OS to ask about all interfaces, but we
	  // can instead get away with having a single simple broadcast address
	  // supplied as part of the timeserver command line argument.
	  struct sockaddr_in addr;
	  bzero(&addr, sizeof(addr)); 
	  addr.sin_family = PF_INET; 
	  addr.sin_port = htons(0x5401); 
	  addr.sin_addr.s_addr = inet_addr(time_broadcast_addr); 
	  sendto(timesocket,msg_out,
		 MSG_DONTROUTE|MSG_DONTWAIT
#ifdef MSG_NOSIGNAL
		 |MSG_NOSIGNAL
#endif	       
		 ,offset,(const struct sockaddr *)&addr,sizeof(addr));
	}
	// Check for time packet
	{
	  unsigned char msg[1024];
	  int offset=0;
	  int r=recvfrom(timesocket,msg,1024,0,NULL,0);
	  if (r==(1+1+8+3)) {
	    // see rxmessages.c for more explanation
	    int stratum=msg[offset++];
	    struct timeval tv;
	    bzero(&tv,sizeof (struct timeval));
	    for(int i=0;i<8;i++) tv.tv_sec|=msg[offset++]<<(i*8);
	    for(int i=0;i<3;i++) tv.tv_usec|=msg[offset++]<<(i*8);
	    // ethernet delay is typically 0.1 - 5ms, so assume 5ms
	    tv.tv_usec+=5000;
	    saw_timestamp("UDP",stratum,&tv);
	  }
	}
      }
      if (!monitor_mode)
	update_my_message(serialfd,
			  my_sid,
			  LINK_MTU,msg_out,
			  servald_server,credential);

      // Vary next update time by upto 250ms, to prevent radios getting lock-stepped.
      last_message_update_time=gettime_ms()+(random()%message_update_interval_randomness);

      // Update the state file to help debug things
      // (but not too often, since it is SLOW on the MR3020s
      //  XXX fix all those linear searches, and it will be fine!)
      if (time(0)>last_status_time) {
	last_status_time=time(0)+3;
	status_dump();
      }
    }

    if ((serial_errors>20)&&reboot_when_stuck) {
      // If we are unable to write to the serial port repeatedly for a while,
      // we could be facing funny serial port behaviour bugs that we see on the MR3020.
      // In which case, if authorised, ask the MR3020 to reboot
      system("reboot");
    }
    
    usleep(10000);

    if (time(0)>last_summary_time) {
      last_summary_time=time(0);
      show_progress();
    }

  }
  }
