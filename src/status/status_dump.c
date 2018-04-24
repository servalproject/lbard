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

#include "sync.h"
#include "lbard.h"
#include "serial.h"
#include "version.h"

#define RESOLVE_SIDS 1
#define SHOW_BUNDLE_STORE 2

#define STATUS_FILE "/tmp/lbard_status.html"
#define STATUS_FILE_WITH_NAMES "/tmp/lbard_status_n.html"
#define STATUS_FILE_WITH_NAMES_AND_BUNDLES "/tmp/lbard_status_bn.html"
#define STATUS_FILE_WITH_BUNDLES "/tmp/lbard_status_b.html"

struct b {
  int order;
  long long priority;
};

char *msgs[1024];
long long msg_times[1024];
int msg_count=0;

int status_log(char *msg)
{
  if (msg_count<1024) {
    msgs[msg_count]=strdup(msg);
    msg_times[msg_count++]=gettime_ms();
    return 0;
  }
  return -1;
}

// The main job of this routine is to make sure that a character is safe for
// including in HTML output.  So mostly no < or >, but we play it safe.
int safechar(int c)
{
  if (c>='a'&&c<='z') return 1;
  if (c>='A'&&c<='Z') return 1;
  if (c>='0'&&c<='9') return 1;
  switch (c) {
  case '.': case ',': case '-': case '_': case '?':
  case '!': case '@': case '#': case '$': case '%':
  case '^': case '*': case '+': case '=': case '/':
    return 1;
  }

  // Unsafe character
  return 0;
}

int compare_b(const void *a,const void *b)
{
  const struct b *aa=a;
  const struct b *bb=b;

  if (aa->priority<bb->priority) return -1;
  if (aa->priority>bb->priority) return 1;
  return 0;
}

long long status_dump_epoch=0;
time_t last_peer_log=0;

int describe_bundle(int fn, FILE *f,FILE *bundlelogfile,int bn,int peerid,
		    int manifest_offset,int body_offset)
{
  char bid[10];
  char *from=bundles[bn].sender;
  char *to=bundles[bn].recipient;
  if ((!from)||(!from[0])) from="unknown";
  if ((!to)||(!to[0])) from="unknown";
  
  // Check for invalid characters in to/from
  for(int i=0;i<strlen(from);i++) {
    if (!safechar(from[i])) { from="CENSORED"; break; }
  }
  for(int i=0;i<strlen(to);i++) {
    if (!safechar(to[i])) { to="CENSORED"; break; }
  }
  
  if (!strncasecmp(bundles[bn].service,"MeshMS",6)) {
    // We show both from and to fields
  } else if (!strncasecmp(bundles[bn].service,"MeshMB",6)) {
    // Recipient is "public"
    to="public";
  } else {
    // All others have no sender or recipient
    from=NULL;
    to=NULL;
  }
  int j;
  for(j=0;j<8;j++) bid[j]=bundles[bn].bid_hex[j];
  bid[8]='*'; bid[9]=0;
  {
    fprintf(f,"%s/%lld ",
	    bid,bundles[bn].version);
    if (from&&to&&(fn&RESOLVE_SIDS)) {
      fprintf(stderr,"(%s %s -> %s)",bundles[bn].service,from,to);
    }
    if (manifest_offset>=0)
      fprintf(f," (from M=%d/P=%d)",manifest_offset,body_offset);
  }

  const time_t now=gettime_ms();
  
  if (manifest_offset>=0)
    if (bundlelogfile&&(fn==0))
      fprintf(bundlelogfile,"%lld:T+%lldms:PEERXFER:%s*:%s/%lld (from M=%d/P=%d):%s",
	      (long long)now,
	      (long long)(gettime_ms()-start_time),		  
	      peer_records[peerid]->sid_prefix,
	      bid,bundles[bn].version,
	      manifest_offset,body_offset,
	      ctime(&now));

  return 0;
}

int status_dump()
{
  int fn;

  if (last_peer_log>time(0)) last_peer_log=time(0);
  
  // Periodically record list of peers in bundle log, if we are maintaining one
  FILE *bundlelogfile=NULL;
  if (debug_bundlelog) {
    if ((time(0)-last_peer_log)>=300) {
      last_peer_log=time(0);	
      bundlelogfile=fopen(bundlelog_filename,"a");
      if (bundlelogfile) {
	fprintf(bundlelogfile,"%lld:T+%lldms:PEERREPORT:%s",
		(long long)last_peer_log,
		(long long)(gettime_ms()-start_time),ctime(&last_peer_log));
      } else perror("Could not open bundle log file");
    }
  }

  if (status_dump_epoch==0) status_dump_epoch=gettime_ms();  
  
  for(fn=0;fn<=3;fn++)
    {
      FILE *f=NULL;
      char *fname=STATUS_FILE;
      switch(fn)
	{
	case RESOLVE_SIDS: fname=STATUS_FILE_WITH_NAMES;
	  break;
	case RESOLVE_SIDS+SHOW_BUNDLE_STORE: fname=STATUS_FILE_WITH_NAMES_AND_BUNDLES;
	  break;
	case SHOW_BUNDLE_STORE: fname=STATUS_FILE_WITH_BUNDLES;
	  break;
	}
      f=fopen(fname,"w");
      if (!f) {
	perror("fopen() on STATUS_FILE for write");
	return -1;
      }

      fprintf(f,
	      "<html>\n<head>\n<title>Mesh Extender Packet Radio Link Status</title>\n"
	      "<meta http-equiv=\"refresh\" content=\"2\" />\n</head>\n<body>\n"
	      "<script>\n"
	      "var seconds_since_load = 0;\n"
	      "setInterval(function() { seconds_since_load++; document.getElementById('time_since_load').innerHTML = seconds_since_load; }, 1000);\n"
	      "</script>\n"
	      "<body><h1>LBARD Status</h1>\nLBARD status dump produced @ T=%lldms (fetched <span id=time_since_load>0</span> seconds ago)\n<p>\n",
	      gettime_ms()-status_dump_epoch);

      fprintf(f,"<p>LBARD Version commit:%s branch:%s [MD5: %s] @ %s\n<p>\n",
	      GIT_VERSION_STRING,GIT_BRANCH,VERSION_STRING,BUILD_DATE);    
  
      struct b order[bundle_count];
      int i,n;
      
      for (i=0;i<bundle_count;i++) {
	order[i].order=i;
	order[i].priority=bundles[i].last_priority;
      }
      qsort(order,bundle_count,sizeof(struct b),compare_b);
      
      // Show peer reachability with indication of activity
      fprintf(f,"<h2>Mesh Extenders Reachable via Radio</h2>\n<table border=1 padding=2 spacing=2><tr><th>Mesh Extender ID</th><th>Performance</th><th>Sending</th></tr>\n");
      for (i=0;i<peer_count;i++) {
	long long age=(time(0)-peer_records[i]->last_message_time);
	float mean_rssi=-1;
	if (peer_records[i]->rssi_counter) mean_rssi=peer_records[i]->rssi_accumulator*1.0/peer_records[i]->rssi_counter;
	int missed_packets=peer_records[i]->missed_packet_count;
	int received_packets=peer_records[i]->rssi_counter;
	float percent_received=0;
	if (received_packets+missed_packets) {
	  percent_received=received_packets*100.0/(received_packets+missed_packets);
	}
	char *colour="#00ff00";
	if (percent_received<10) colour="#ff4f00";
	else if (percent_received<50) colour="#ffff00";
	else if (percent_received<80) colour="#c0c0c0";
	
	if (age<=30) {
	  time_t now=time(0);
	  fprintf(f,"<tr><td>%s*</td><td bgcolor=\"%s\">%lld sec, %d/%d received (%2.1f%% loss), mean RSSI = %.0f</td><td>",
		  peer_records[i]->sid_prefix,colour,
		  age,received_packets,received_packets+missed_packets,100-percent_received,mean_rssi);
	  if (bundlelogfile) {
	    fprintf(stderr,"Writing PEERSTATUS line...\n");
	    fprintf(bundlelogfile,"%lld:T+%lldms:PEERSTATUS:%s*:%lld:%d/%d:%.0f:%s",
		    (long long)now,
		    (long long)(gettime_ms()-start_time),		  
		    peer_records[i]->sid_prefix,
		    age,received_packets,received_packets+missed_packets,mean_rssi,
		    ctime(&now));
	    fprintf(stderr,"Wrote PEERSTATUS line.\n");
	  }
	  
	  if (peer_records[i]->tx_bundle!=-1) {
	    describe_bundle(fn,f,bundlelogfile,i,peer_records[i]->tx_bundle,
			    peer_records[i]->tx_bundle_manifest_offset_hard_lower_bound,
			    peer_records[i]->tx_bundle_body_offset_hard_lower_bound);
	  }
	  fprintf(f,"</td></tr>\n");
	}

	if (fn==3) {
	  // Reset packet RX stats for next round
	  peer_records[i]->missed_packet_count=0;
	  peer_records[i]->rssi_counter=0;
	  peer_records[i]->rssi_accumulator=0;
	}
      }
      fprintf(f,"</table>\n");
      if (bundlelogfile&&(fn==3)) fclose(bundlelogfile);
      
      // Show current transfer progress bars
      fprintf(f,"<h2>Current Bundles being received</h2>\n");
      fprintf(f,"<pre>\n");
      show_progress(f,1);
      fprintf(f,"</pre>\n");

      fprintf(f,"<h2>Mesh Extenders Radio Transmit Queues</h2>\n<table border=1 padding=2 spacing=2><tr><th>Bundle</th></tr>\n");
      for (i=0;i<peer_count;i++) {
	long long age=(time(0)-peer_records[i]->last_message_time);
	
	if (age<=30) {
	  fprintf(f,"<tr><td><b>Peer %s*</b></td></tr>\n",peer_records[i]->sid_prefix);

	  for(int j=0;j<peer_records[i]->tx_queue_len;j++) {
	    if (peer_records[i]->tx_bundle!=-1) {
	      fprintf(f,"<tr><td>");
	      describe_bundle(fn,f,NULL,i,peer_records[i]->tx_queue_bundles[j],
			      // Don't show transfer progress, just bundle info
			      -1,-1);
	      fprintf(f,"</tr>\n");
	    }
	  }
	}
      }
      fprintf(f,"</table>\n");
      
      // And EEPROM data (copy from /tmp/eeprom.data)
      {
	char buffer[16384];
	FILE *e=fopen("/tmp/eeprom.data","r");
	if (e) {
	  fprintf(f,"<h2>EEPROM Radio information</h2>\n<pre>\n");
	  int bytes=fread(buffer,1,16384,e);
	  if (bytes>0) fwrite(buffer,bytes,1,f);
	  fclose(e);
	  fprintf(f,"</pre>\n");
	}
      }

      if (fn&SHOW_BUNDLE_STORE) {
	fprintf(f,"<h2>Bundles in local store</h2>\n<table border=1 padding=2 spacing=2><tr><th>Bundle #</th><th>BID Prefix</th><th>Service</th><th>Bundle version</th><th>Bundle length</th><th>Last calculated priority</th><th># peers who don't have this bundle</th></tr>\n");
	for (n=0;n<bundle_count;n++) {
	  i=order[n].order;
	  fprintf(f,"<tr><td>#%d</td><td>%s</td><td>%s</td><td>%lld</td><td>%lld</td><td>0x%08llx (%lld)</td><td>%d</td></tr>\n",
		  i,
		  bundles[i].bid_hex,
		  bundles[i].service,
		  bundles[i].version,
		  bundles[i].length,
		  bundles[i].last_priority,bundles[i].last_priority,
		  bundles[i].num_peers_that_dont_have_it);
	}
	fprintf(f,"</table>\n");
	fflush(f);
	
#ifdef SYNC_BY_BAR
	fprintf(f,"<h2>Bundles held by peers</h2>\n<table border=1 padding=2 spacing=2><tr><th>Peer</th><th>Bundle prefix</th><th>Bundle version</th></tr>\n");

	for(peer=0;peer<peer_count;peer++) {
	  // Don't show timed out peers
	  if ((time(0)-peer_records[peer]->last_message_time)>PEER_KEEPALIVE_INTERVAL)
	    continue;
	  
	  char *peer_prefix=peer_records[peer]->sid_prefix;
	  for(i=0;i<peer_records[peer]->bundle_count;i++) {
	    if (peer_records[peer]->partials[i].bid_prefix) {
	      // Here is a bundle in flight
	      char *bid_prefix=peer_records[peer]->bid_prefixes[i];
	      long long version=peer_records[peer]->versions[i];
	      fprintf(f,"<tr><td>%s*</td><td>%s*</td><td>%-18lld</td></tr>\n",
		      peer_prefix?peer_prefix:"<no peer prefix>",
		      bid_prefix?bid_prefix:"<no bid prefix>",version);
	    }
	  }
	}
	fprintf(f,"</table>\n");
	fflush(f);
#endif

	fprintf(f,"<h2>Bundles in flight</h2>\n<table border=1 padding=2 spacing=2><tr><th>Bundle prefix</th><th>Bundle version</th><th>Progress<th></tr>\n");
	
	for(i=0;i<MAX_BUNDLES_IN_FLIGHT;i++) {
	  if (partials[i].bid_prefix) {
	    // Here is a bundle in flight
	    char *bid_prefix=partials[i].bid_prefix;
	    long long version=partials[i].bundle_version;
	    char progress_string[80];
	    generate_progress_string(&partials[i],
				     progress_string,sizeof(progress_string));
	    fprintf(f,"<tr><td>%s*</td><td>%-18lld</td><td>[%s]</td></tr>\n",
		    bid_prefix,version,
		    progress_string);
	  }
	}
	fprintf(f,"</table>\n");
	fflush(f);
	
	fprintf(f,"<h2>Announced material</h2>\n<table border=1 padding=2 spacing=2><tr><th>Time</th><th>Announced content</th></tr>\n");
	long long now=gettime_ms();
	for(i=0;i<msg_count;i++) {
	  fprintf(f,"<tr><td>T-%lldms</td><td>%s</td></tr>\n",
		  now-msg_times[i],msgs[i]);
	  free(msgs[i]); msgs[i]=NULL;
	}
	fprintf(f,"</table>\n");
      }

      msg_count=0;

      show_time_accounting(f);
      
      fprintf(f,"</body>\n");
      
      fclose(f);
    }

  return 0;
}

int http_report_network_status(int socket,int flags)
{
  if (socket!=-1)
    {
      switch(flags) {
      case RESOLVE_SIDS:
	return http_send_file(socket,STATUS_FILE_WITH_NAMES,"text/html");
      case RESOLVE_SIDS|SHOW_BUNDLE_STORE:
	return http_send_file(socket,STATUS_FILE_WITH_NAMES_AND_BUNDLES,"text/html");
      case SHOW_BUNDLE_STORE:
	return http_send_file(socket,STATUS_FILE_WITH_BUNDLES,"text/html");
      default:
	return http_send_file(socket,STATUS_FILE,"text/html");
      }
    }
  else return 0;
}

time_t last_json_network_status_call=0;
int http_report_network_status_json(int socket)
{
  if (((time(0)-last_json_network_status_call)>1)||
      ((time(0)-last_json_network_status_call)<0))
    {
      last_json_network_status_call=time(0);
      FILE *f=fopen("/tmp/networkstatus.json","w");
      if (!f) {
	char *m="HTTP/1.0 500 Couldn't create temporary file\nServer: Serval LBARD\n\nCould not create temporariy file";
	write_all(socket,m,strlen(m));
	
	return -1;
      }

      // List peers
      fprintf(f,"{\n\"neighbours\": [\n     ");
      
      int i;
      int count=0;
      for (i=0;i<peer_count;i++) {
	long long age=(time(0)-peer_records[i]->last_message_time);
	if (age<20) {
	  if (count) fprintf(f,",");
	  fprintf(f,"{ \"id\": \"%s\", \"time-since-last\": %lld }\n",
		  peer_records[i]->sid_prefix,age);
	  count++;
	}
      }
      fprintf(f,"   ],\n\n\n");
      
      
      // Show current transfer progress bars
      fprintf(f,"\n\"transfers\": [\n     ");
      show_progress_json(f,1);
      fprintf(f,"   ]\n}\n\n");      
      
      fclose(f);
    }
  return http_send_file(socket,"/tmp/networkstatus.json","application/json");
}

