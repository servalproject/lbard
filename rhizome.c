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
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <assert.h>

#include "sync.h"
#include "lbard.h"

size_t write_data(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    size_t written = fwrite(ptr, size, nmemb, stream);
    return written;
}

// By default don't filter bundles.
int meshms_only=0;
long long min_version=0;

int rhizome_log(char *service,
		char *bid,
		char *version,
		char *author,
		char *originated_here,
		long long length,
		char *filehash,
		char *sender,
		char *recipient,

		char *message)
{
  if (debug_insert) {
    FILE *f=fopen("/tmp/lbard-rhizome.log","w+");
    if (!f) return -1;
    time_t now = time(0);
    fprintf(f,"--------------------\n%sservice=%s, bid=%s,\nversion=%s, author=%s,\noriginated_here=%s, length=%lld,\nfilehash=%s,\nsender=%s, recipient=%s:\n\n%s\n\n",
	    ctime(&now),
	    service,bid,version,author,originated_here,length,filehash,sender,recipient,
	    message);
    fclose(f);
  }
  return 0;
}


int load_rhizome_db(int timeout,
		    char *prefix, char *servald_server,
		    char *credential, char **token)
{
  char path[8192];

  // printf("%s(): ENTERED\n",__FUNCTION__);
  
  // A timeout of zero means forever. Never do this.
  if (!timeout) timeout=1;
  
  // We use the new-since-time version once we have a token
  // to make this much faster.
  if ((!*token)||(!(random()&0xf))) {
      snprintf(path,8192,"/restful/rhizome/bundlelist.json");
      // Allow a bit more time to read all the bundles this time around
      timeout=2000;
  } else
    snprintf(path,8192,"/restful/rhizome/newsince/%s/bundlelist.json",
	     *token);
    
  char filename[1024];
  snprintf(filename,1024,"%sbundle.list",prefix);
  unlink(filename);
  FILE *f=fopen(filename,"w");
  if (!f) {
    printf("could not open output file '%s'.\n",filename);
    perror("fopen");
    return -1;
  }

  int ignore_token=0;
  long long last_read_time=0LL;  
  int result_code=http_get_simple(servald_server,
				  credential,path,f,timeout,&last_read_time);
  // Did we keep reading upto the last fraction of a second?
  if ((gettime_ms()-last_read_time)<100) {
    // Yes, rhizome list fetch consumed all available time, so ignore tokens
    ignore_token=1;
  }
  
  fclose(f);
  if(result_code!=200) {
    printf("rhizome HTTP API request failed. URLPATH:%s\n",path);
    return -1;
  } else {
    // printf("rhizome HTTP API request succeeded. URLPATH:%s\n",path);
  }

  // printf("Read bundle list.\n");

  // Now read database into memory.
  f=fopen(filename,"r");
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
	// XXX - This token is only reliable if we read the complete list in this call.

	if (!ignore_token) {
	  if (*token) free(*token);
	  *token=strdup(fields[0]);
	  if (1) printf("Saw rhizome progressive fetch token '%s'\n",*token);
	} else {
	  if (1) printf("Ignoring rhizome progressive fetch token '%s' because of timeout\n",*token);
	}
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

  if (0)
    message_buffer_length+=
      snprintf(&message_buffer[message_buffer_length],
	       message_buffer_size-message_buffer_length,
	       "Querying rhizome DB for new content (timeout %d ms)\n"
	       "Rhizome contains %d new bundles (token = %s). We now know about %d bundles.\n",
	       timeout,count,*token,bundle_count);
  fclose(f);
  unlink(filename);
  
  return 0;
}

int hex_byte_value(char *hexstring)
{
  char hex[3];
  hex[0]=hexstring[0];
  hex[1]=hexstring[1];
  hex[2]=0;
  return strtoll(hex,NULL,16);
}

int rhizome_update_bundle(unsigned char *manifest_data,int manifest_length,
			  unsigned char *body_data,int body_length,
			  char *servald_server,char *credential)
{
  /* Push to rhizome.

     We don't need to mark the associated bundle as needing immediate announcement,
     as this will happen as a natural consequence of the Rhizome database being 
     updated.  Similarly, if the insert fails for some reason, then the sender will
     automatically keep trying to send it according to its regular rotation.
   */

  printf("CHECKPOINT: %s:%d %s()\n",__FILE__,__LINE__,__FUNCTION__);

#ifdef NOT_DEFINED
  char filename[1024];
  snprintf(filename,1024,"%08lx.manifest",time(0));
  printf(">>> Writing manifest to %s\n",filename);
  FILE *f=fopen(filename,"w");
  fwrite(manifest_data,manifest_length,1,f);
  fclose(f);
  snprintf(filename,1024,"%08lx.payload",time(0));
  f=fopen(filename,"w");
  fwrite(body_data,body_length,1,f);
  fclose(f);
#endif
  
  printf("Submitting rhizome bundle: manifest len=%d, body len=%d\n",
	  manifest_length,body_length);

  int result_code=http_post_bundle(servald_server,credential,
				   "/rhizome/import",
				   manifest_data,manifest_length,
				   body_data,body_length,
				   15000);  
  
  if(result_code<200|| result_code>202) {
    printf("POST bundle to rhizome failed: http result = %d\n",result_code);

    if (debug_insert) {
      char filename[1024];
      snprintf(filename,1024,"/tmp/lbard.rejected.manifest");
      FILE *f=fopen(filename,"w");
      if (f) { fwrite(manifest_data,manifest_length,1,f); fclose(f); }
      snprintf(filename,1024,"/tmp/lbard.rejected.body");
      f=fopen(filename,"w");
      if (f) { fwrite(body_data,body_length,1,f); fclose(f); }
      snprintf(filename,1024,"/tmp/lbard.rejected.result");
      f=fopen(filename,"w");
      if (f) {
	fprintf(f,"http result code = %d\n",result_code);
	fclose(f);
      }
    }
    return -1;
  }
  else
    printf("http result code = %d\n",result_code);
    
  
  return 0;
}


int manifest_extract_bid(unsigned char *manifest_data,char *bid_hex)
{
  // Find ID= at start of manifest and return the BID
  if (!strncasecmp("ID=",(char *)manifest_data,3))
    {
      for(int i=0;i<32*2;i++) {
	bid_hex[i]=manifest_data[i+3];
      }
      bid_hex[64]=0;
      return 0;	  
    }
  return -1;
}
