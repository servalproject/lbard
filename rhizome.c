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

struct bundle_record bundles[MAX_BUNDLES];
int bundle_count=0;

size_t write_data(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    size_t written = fwrite(ptr, size, nmemb, stream);
    return written;
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
  int i;
  // XXX - Linear search through bundles!
  // Use a hash table or something so that it doesn't cost O(n^2) with number
  // of bundles.
  int bundle_number=bundle_count;
  for(i=0;i<bundle_count;i++) {
    if (!strcmp(bundles[i].bid,bid)) {
      // Updating an existing bundle
      bundle_number=i; break;
    }
  }

  if (bundle_number>=MAX_BUNDLES) return -1;
  
  if (bundle_number<bundle_count) {
    // Replace old bundle values, ...

    // ... unless we already hold a newer version
    if (bundles[bundle_number].version>=strtoll(version,NULL,10))
      return 0;
    
    free(bundles[bundle_number].service);
    free(bundles[bundle_number].author);
    free(bundles[bundle_number].filehash);
    free(bundles[bundle_number].sender);
    free(bundles[bundle_number].recipient);
  } else {    
    // New bundle
    bundles[bundle_number].bid=strdup(bid);
    // Never announced
    bundles[bundle_number].last_offset_announced=0;
    bundles[bundle_number].last_version_of_manifest_announced=0;
    bundles[bundle_number].last_announced_time=0;
    bundle_count++;
  }

  // Clear latest announcement time for bundles that get updated with a new version
  if (bundles[bundle_number].version<strtoll(version,NULL,10)) {
    bundles[bundle_number].last_offset_announced=0;
    bundles[bundle_number].last_version_of_manifest_announced=0;
    bundles[bundle_number].last_announced_time=0;
  }
  
  bundles[bundle_number].service=strdup(service);
  bundles[bundle_number].version=strtoll(version,NULL,10);
  bundles[bundle_number].author=strdup(author);
  bundles[bundle_number].originated_here_p=atoi(originated_here);
  bundles[bundle_number].length=length;
  bundles[bundle_number].filehash=strdup(filehash);
  bundles[bundle_number].sender=strdup(sender);
  bundles[bundle_number].recipient=strdup(recipient);
  
  return 0;
}

int find_highest_priority_bundle()
{
  int i;
  int highest_bundle_priority=-1;
  int highest_priority_bundle=-1;

  int this_bundle_priority;
  for(i=0;i<bundle_count;i++) {

    this_bundle_priority=0;

    // Bigger values in this list means that factor is more important in selecting
    // which bundle to announce next.
#define BUNDLE_PRIORITY_SENT_LESS_RECENTLY   0x00000080
#define BUNDLE_PRIORITY_FILE_SIZE_SMALLER    0x00000100
#define BUNDLE_PRIORITY_RECIPIENT_IS_A_PEER  0x00000200
#define BUNDLE_PRIORITY_IS_MESHMS            0x00000400
#define BUNDLE_PRIORITY_ANNOUNCE_NOW         0x40000000

    if (bundles[i].announce_now) {
      this_bundle_priority|=BUNDLE_PRIORITY_ANNOUNCE_NOW;
      bundles[i].announce_now=0;
    }
    
    if (highest_priority_bundle>=0) {
      if (bundles[i].last_announced_time
	  <bundles[highest_priority_bundle].last_announced_time)
	this_bundle_priority|=BUNDLE_PRIORITY_SENT_LESS_RECENTLY;
      if (bundles[i].length<bundles[highest_priority_bundle].length)
	this_bundle_priority|=BUNDLE_PRIORITY_FILE_SIZE_SMALLER;
    }

    if ((!strcasecmp("MeshMS1",bundles[i].service))
	||(!strcasecmp("MeshMS2",bundles[i].service))) {
      this_bundle_priority|=BUNDLE_PRIORITY_IS_MESHMS;
    }
    
    // Is bundle addressed to a peer?
    int j;
    for(j=0;j<peer_count;j++) {
      if (!strncmp(bundles[i].recipient,
		   peer_records[j]->sid_prefix,
		   (8*2))) {
	// Bundle is addressed to a peer.
	// Increase priority if we do not have positive confirmation that peer
	// has this version of this bundle.
	this_bundle_priority|=BUNDLE_PRIORITY_RECIPIENT_IS_A_PEER;

	int k;
	for(k=0;k<peer_records[j]->bundle_count;k++) {
	  if (!strncmp(peer_records[j]->bid_prefixes[k],bundles[i].bid,
		       8*2)) {
	    // Peer knows about this bundle, but which version?
	    if (peer_records[j]->versions[k]<bundles[i].version) {
	      // They only know about an older version.
	      // XXX Advance bundle announced offset to last known offset for
	      // journal bundles (MeshMS1 & MeshMS2 types, and possibly others)
	    } else {
	      // The peer has this version (or possibly a newer version!), so there
	      // is no point us announcing it.
	      this_bundle_priority&=~BUNDLE_PRIORITY_RECIPIENT_IS_A_PEER;
	    }
	  }
	}

	// We found who this bundle was addressed to, so there is no point looking
	// further.
	break;
      }
    }

    // Indicate this bundle as highes priority, unless we have found another one that
    // is higher priority.
    // Replace if priority is equal, so that newer bundles take priorty over older
    // ones.
    if (this_bundle_priority>=highest_priority_bundle) {
      highest_bundle_priority=this_bundle_priority;
      highest_priority_bundle=i;
    }
  }
  
  return highest_priority_bundle;
}

int load_rhizome_db(int timeout,
		    char *prefix, char *servald_server,
		    char *credential, char **token)
{
  CURL *curl;
  CURLcode result_code;
  curl=curl_easy_init();
  if (!curl) return -1;
  char url[8192];
  
  // A timeout of zero means forever. Never do this.
  if (!timeout) timeout=1;
  
  // We use the new-since-time version once we have a token
  // to make this much faster.
  if (!*token)
    snprintf(url,8192,"http://%s/restful/rhizome/bundlelist.json",
	     servald_server);
  else
    snprintf(url,8192,"http://%s/restful/rhizome/newsince/%s/bundlelist.json",
	     servald_server,*token);
    
  curl_easy_setopt(curl, CURLOPT_URL, url);
  char filename[1024];
  snprintf(filename,1024,"%sbundle.list",prefix);
  unlink(filename);
  FILE *f=fopen(filename,"w");
  if (!f) {
    curl_easy_cleanup(curl);
    fprintf(stderr,"could not open output file.\n");
    return -1;
  }
  curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
  curl_easy_setopt(curl, CURLOPT_USERPWD, credential);  
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout*1000);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
  result_code=curl_easy_perform(curl);
  fclose(f);
  if (0)
    if(result_code!=CURLE_OK) {
      curl_easy_cleanup(curl);    
      fprintf(stderr,"curl request failed. URL:%s\n",url);
      fprintf(stderr,"libcurl: %s\n",curl_easy_strerror(result_code));
      return -1;
    }

  curl_easy_cleanup(curl);
  // fprintf(stderr,"Read bundle list.\n");

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
	if (*token) free(*token);
	*token=strdup(fields[0]);
	fprintf(stderr,"Saw rhizome progressive fetch token '%s'\n",*token);
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

  fprintf(stderr,"Rhizome contains %d new bundles (token = %s). We now know about %d bundles.\n",count,*token,bundle_count);
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

  fprintf(stderr,"CHECKPOINT: %s:%d %s()\n",__FILE__,__LINE__,__FUNCTION__);

#ifdef NOT_DEFINED
  char filename[1024];
  snprintf(filename,1024,"%08lx.manifest",time(0));
  fprintf(stderr,">>> Writing manifest to %s\n",filename);
  FILE *f=fopen(filename,"w");
  fwrite(manifest_data,manifest_length,1,f);
  fclose(f);
  snprintf(filename,1024,"%08lx.payload",time(0));
  f=fopen(filename,"w");
  fwrite(body_data,body_length,1,f);
  fclose(f);
#endif
  
  CURL *curl;
  CURLcode result_code;
  curl=curl_easy_init();

  struct curl_httppost* post = NULL;
  struct curl_httppost* last = NULL;
  
  curl_formadd(&post, &last, CURLFORM_COPYNAME, "manifest",
               CURLFORM_PTRCONTENTS, manifest_data,
               CURLFORM_CONTENTSLENGTH, (long)manifest_length,
               CURLFORM_CONTENTTYPE, "rhizome/manifest",
	       CURLFORM_END);
  curl_formadd(&post, &last, CURLFORM_COPYNAME, "payload",
               CURLFORM_PTRCONTENTS, body_data,
               CURLFORM_CONTENTSLENGTH, (long)body_length,
               CURLFORM_CONTENTTYPE, "binary/data", CURLFORM_END);

  char url[8192];
  snprintf(url,8192,"http://%s/rhizome/import",servald_server);
    
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
  curl_easy_setopt(curl, CURLOPT_USERPWD, credential);  
  // 2 minute timeout, since inserting a big file can take a long time
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 120000);
  curl_easy_setopt(curl, CURLOPT_HTTPPOST, post);

  fprintf(stderr,"Submitting rhizome bundle: manifest len=%d, body len=%d\n",
	  manifest_length,body_length);
  
  result_code=curl_easy_perform(curl);
  if(result_code!=CURLE_OK) {
    curl_easy_cleanup(curl);
    // fprintf(stderr,"curl request failed. URL:%s\n",url);
    // fprintf(stderr,"libcurl: %s\n",curl_easy_strerror(result_code));
    return -1;
  }

  curl_easy_cleanup(curl);
  
  return 0;
}

int clear_partial(struct partial_bundle *p)
{
  while(p->manifest_segments) {
    struct segment_list *s=p->manifest_segments;
    p->manifest_segments=s->next;
    if (s->data) free(s->data);
    free(s);    
  }
  while(p->body_segments) {
    struct segment_list *s=p->body_segments;
    p->body_segments=s->next;
    if (s->data) free(s->data);
    free(s);    
  }

  bzero(p,sizeof(struct partial_bundle));
  return -1;
}

int dump_segment_list(struct segment_list *s)
{
  if (!s) return 0;
  while(s) {
    fprintf(stderr,"    [%d,%d)\n",s->start_offset,s->start_offset+s->length);
    s=s->next;
  }
  return 0;
}

int dump_partial(struct partial_bundle *p)
{
  fprintf(stderr,"Progress receiving BID=%s* version %lld:\n",
	  p->bid_prefix,p->bundle_version);
  fprintf(stderr,"  manifest is %d bytes long, and body %d bytes long.\n",
	  p->manifest_length,p->body_length);
  fprintf(stderr,"  Manifest pieces received:\n");
  dump_segment_list(p->manifest_segments);
  fprintf(stderr,"  Body pieces received:\n");
  dump_segment_list(p->body_segments);
  return 0;
}

int merge_segments(struct segment_list **s)
{
  if (!s) return -1;
  if (!(*s)) return -1;

  // Segments are sorted in descending order
  while((*s)&&(*s)->next) {
    struct segment_list *me=*s;
    struct segment_list *next=(*s)->next;
    if (me->start_offset<=(next->start_offset+next->length)) {
      // Merge this piece onto the end of the next piece
      fprintf(stderr,"Merging [%d..%d) and [%d..%d)\n",
	      me->start_offset,me->start_offset+me->length,
	      next->start_offset,next->start_offset+next->length);
      int extra_bytes
	=(me->start_offset+me->length)-(next->start_offset+next->length);
      int new_length=next->length+extra_bytes;
      next->data=realloc(next->data,new_length);
      assert(next->data);
      bcopy(&me->data[me->length-extra_bytes],&next->data[next->length],
	    extra_bytes);
      next->length=new_length;

      // Excise redundant segment from list
      *s=next;
      next->prev=me->prev;
      if (me->prev) me->prev->next=next;

      // Free redundant segment.
      free(me->data);
      free(me);
    } else 
      s=&(*s)->next;
  }
  return 0;
}
