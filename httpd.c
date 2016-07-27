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
#include <ctype.h>

#include "sync.h"
#include "lbard.h"

char *inreach_gateway_ip=NULL;
time_t inreach_gateway_time=0;

int urldecode(char *s)
{
  int i,o=0;
  int len=strlen(s);

  for(i=0;i<len;i++) {
    switch (s[i]) {
    case '+':
      s[o++]=' ';
      break;
    case '%':
      {
	int c;
	c=chartohex(toupper(s[i+1]))<<4;
	c|=chartohex(toupper(s[i+2]));
	
	s[o++]=c;
	i+=2;
      }
      break;
    default:
      s[o++]=s[i];
    }
  }
  s[o]=0;
  return o;
  
}

int http_process(struct sockaddr *cliaddr,
		 char *servald_server,char *credential,
		 char *my_sid_hex,
		 int socket)
{
  char buffer[8192];
  char uri[8192];
  int version_major, version_minor;
  int offset;
  int request_len = read(socket,buffer,8192);
  printf("Read %d bytes of request.\n",request_len);
  int r=sscanf(buffer,"GET %[^ ] HTTP/%d.%d\n%n",
	       uri,&version_major,&version_minor,&offset);
  printf("  scanned %d fields.\n",r);
  printf("    uri=%s\n",uri);
  printf("    request version=%d.%d\n",version_major,version_minor);
  if (r==3)
    {
      char location[8192]="";
      char message[8192]="";
      if (sscanf(uri,"/submitmessage?location=%[^&]&message=%[^&]",
		    location,message)==2) {
	urldecode(location);
	urldecode(message);
      
	printf("  scanned URI fields.\n");
	printf("    location=[%s]\n",location);
	printf("    message=[%s]\n",message);
	
	char *m="HTTP/1.0 200 OK\nServer: Serval LBARD\n\nYour message has been submitted.";
	
	// Try to actually send meshms
	// First: compose the string safely.  It might contain UTF-8 text, so we should
	// try to be nice about that.
	{
	  unsigned char combined[8192+8192+1024];
	  snprintf((char *)combined,sizeof(combined),
		   "Message from message_form.html: Location = %s,"
		   " Message as follows: %s",
		   location,message);
	  
	  // XXX - Read recipient
	  {
	    int successful=0;
	    int failed=0;
	    char recipient[1024];
	    
	    FILE *f=fopen("/dos/helpdesk.sid","r");
	    recipient[0]=0; fgets(recipient,1024,f);
	    while(recipient[0]) {
	      // Trim new lines / carriage returns from end of lines.
	      while(recipient[0]&&(recipient[strlen(recipient)-1]<' '))
		recipient[strlen(recipient)-1]=0;
	      
	      int res = http_post_meshms(servald_server,credential,
					 (char *)combined,
					 my_sid_hex,recipient,5000);
	      
	      if (res!=200) {
		m="HTTP/1.0 500 ERROR\nServer: Serval LBARD\n\nYour message could not be submitted.";
		failed++;
	      } else successful++;
	      
	      recipient[0]=0; fgets(recipient,1024,f);
	    }
	    fclose(f);
	  }
	  
	}
	
	write_all(socket,m,strlen(m));
	close(socket);
	return 0;      
      } else if (!strcasecmp(uri,"/inreachgateway/register")) {
	if (inreach_gateway_ip) free(inreach_gateway_ip);
	inreach_gateway_ip=NULL;
	inreach_gateway_ip
	  = strdup(inet_ntoa(((struct sockaddr_in *)cliaddr)->sin_addr));
	inreach_gateway_time=time(0);
	char m[1024];
	snprintf(m,1024,"HTTP/1.0 201 OK\nServer: Serval LBARD\n\n");
	write_all(socket,m,strlen(m));
	close(socket);
	return 0;	
      } else if (!strcasecmp(uri,"/inreachgateway/query")) {
	char m[1024];
	printf("inreach gateway age %lld\n",(long long)(time(0)-inreach_gateway_time));
	if (inreach_gateway_ip&&((time(0)-inreach_gateway_time)<20)) 
	  snprintf(m,1024,"HTTP/1.0 200 OK\nServer: Serval LBARD\nContent-length: %d\n\n%s",
		   (int)strlen(inreach_gateway_ip),inreach_gateway_ip);
	else
	  snprintf(m,1024,"HTTP/1.0 204 OK\nServer: Serval LBARD\n\n");
	write_all(socket,m,strlen(m));
	close(socket);
	return 0;	
      }
    }
  char *m="HTTP/1.0 400 Couldn't parse message\nServer: Serval LBARD\n\n";
  write_all(socket,m,strlen(m));
  close(socket);
  return 0;
}
