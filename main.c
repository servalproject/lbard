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

struct peer_state {
  unsigned char *last_message;
  time_t last_message_time;
};

size_t write_data(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    size_t written = fwrite(ptr, size, nmemb, stream);
    return written;
}

int parse_json_line(char *line,char fields[][8192],int num_fields)
{
  int field_count=0;
  int offset=0;
  if (line[offset]!='[') return -1; else offset++;

  while(line[offset]&&line[offset]!=']') {
    if (field_count>=num_fields) return -2;
    if (line[offset]=='"') {
      // quoted field
      int j=0,i;
      for(i=offset+1;(line[i]!='"')&&(i<8191);i++)
	fields[field_count][j++]=line[i];
      fields[field_count++][j]=0;
      offset=i+1;
    } else {
      // naked field
      int j=0,i;
      for(i=offset;(line[i]!=',')&&(line[i]!=']')&&(i<8191);i++)
	fields[field_count][j++]=line[i];
      fields[field_count++][j]=0;
      if (offset==i) return -4;
      offset=i;
    }
    if (line[offset]&&(line[offset]!=',')&&(line[offset]!=']')) return -3;
    if (line[offset]==',') offset++;
  }
  
  return field_count;
}

int register_bundle(char *service,
		    char *bid,
		    char *version,
		    char *author,
		    char *originated_here,
		    long long length,
		    char *filehash,
		    char *sender,
		    char *recipient)
{
  return 0;
}

int load_rhizome_db(char *servald_server,char *credential)
{
  CURL *curl;
  CURLcode result_code;
  curl=curl_easy_init();
  if (!curl) return -1;
  char url[8192];
  snprintf(url,8192,"http://%s/restful/rhizome/bundlelist.json",
	   servald_server);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  FILE *f=fopen("bundle.list","w");
  if (!f) {
    curl_easy_cleanup(curl);
    fprintf(stderr,"could not open output file.\n");
    return -1;
  }
  curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
  curl_easy_setopt(curl, CURLOPT_USERPWD, credential);  
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
  result_code=curl_easy_perform(curl);
  fclose(f);
  if(result_code!=CURLE_OK) {
    curl_easy_cleanup(curl);
    fprintf(stderr,"curl request failed. URL:%s\n",url);
    fprintf(stderr,"libcurl: %s\n",curl_easy_strerror(result_code));
    return -1;
  }

  curl_easy_cleanup(curl);
  fprintf(stderr,"Read bundle list.\n");

  // Now read database into memory.
  f=fopen("bundle.list","r");
  if (!f) return -1;
  char line[8192];
  int count=0;

  char fields[14][8192];
  
  line[0]=0; fgets(line,8192,f);
  while(line[0]) {
    int n=parse_json_line(line,fields,14);
    if (n==14) {
      if (strcmp(fields[0],"null")) {
	// We have a token that will allow us to ask for only newer bundles in a
	// future call. Remember it and use it.
      }
      
      // Now we have the fields, so register the bundles into our internal list.
      register_bundle(fields[2] // service (file/meshms1/meshsm2)
		      ,fields[3] // bundle id (BID)
		      ,fields[4] // version
		      ,fields[7] // author
		      ,fields[8] // originated here
		      ,strtoll(fields[9],NULL,10) // size of data/file
		      ,fields[10] // file hash
		      ,fields[11] // sender
		      ,fields[12] // recipient
		      );
      count++;
    }

    line[0]=0; fgets(line,8192,f);
  }

  fprintf(stderr,"Found %d bundles.\n",count);
  fclose(f);
  
  return 0;
}

int update_my_message()
{
  return 0;
}

int main(int argc, char **argv)
{
  while(1) {
    if (argc>2) load_rhizome_db(argv[1],argv[2]);
    
    update_my_message();
    // The time it takes for a Bluetooth scan
    sleep(12);
  }
}
