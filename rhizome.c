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
  int i,peer;

  long long versionll=strtoll(version,NULL,10);

  // Remove bundle from partial lists of all peers if we have other transmissions
  // to us in progress of this bundle
  // XXX - Linear searches! Replace with hash table etc
  for(peer=0;peer<peer_count;peer++) {
    for(i=0;i<MAX_BUNDLES_IN_FLIGHT;i++) {
      if (peer_records[peer]->partials[i].bid_prefix) {
	// Here is a bundle in flight
	char *bid_prefix=peer_records[peer]->partials[i].bid_prefix;
	long long bid_version=peer_records[peer]->partials[i].bundle_version;

	if (versionll>=bid_version)
	  if (!strncasecmp(bid,bid_prefix,strlen(bid_prefix)))
	    {
	      fprintf(stderr,"--- Culling in-progress transfer for bundle that has shown up in Rhizome.\n");
	      clear_partial(&peer_records[peer]->partials[i]);
	      break;
	    }
      }
    }
  }

  
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
    if (bundles[bundle_number].version>=versionll)
      return 0;
    
    free(bundles[bundle_number].service);
    bundles[bundle_number].service=NULL;
    free(bundles[bundle_number].author);
    bundles[bundle_number].author=NULL;
    free(bundles[bundle_number].filehash);
    bundles[bundle_number].filehash=NULL;
    free(bundles[bundle_number].sender);
    bundles[bundle_number].sender=NULL;
    free(bundles[bundle_number].recipient);
    bundles[bundle_number].recipient=NULL;
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

int peer_has_this_bundle_or_newer(int peer,char *bid_or_bidprefix, long long version)
{
  // XXX - Another horrible linear search!
  // XXX - Bundle lists need to be stored in a hash table or similar.

  int bundle;
  for(bundle=0;bundle<peer_records[peer]->bundle_count;bundle++)
    {
      // Check version first, because it is faster.
      if (version<=peer_records[peer]->versions[bundle])
	if (!strncasecmp(bid_or_bidprefix,peer_records[peer]->bid_prefixes[bundle],
			 strlen(peer_records[peer]->bid_prefixes[bundle])))
	  return 1;
    }
  
  return 0;
}

int bundle_bar_counter=0;
int find_highest_priority_bar()
{
  int bar_number;
  // XXX This can probably get stuck in a loop announcing the same
  // BAR over and over in an attempt to stop another node announcing
  // segments of a bundle to a different peer who doesn't have it yet.
  // So for now we have the crude mechanism of only making such announcements
  // 1/2 the time, so that we can keep announcing new BARs to our peers.
  if (random()&1) {
    for(bar_number=0;bar_number<bundle_count;bar_number++) 
      if (bundles[bar_number].announce_bar_now) {
	bundles[bar_number].announce_bar_now=0;
	return bar_number;
      }
  }
  
  bundle_bar_counter++;
  if (bundle_bar_counter>=bundle_count) bundle_bar_counter=0;
  return bundle_bar_counter;
}

int lengthToPriority(long long value)
{
  long long original_value=value;
  
  int result=0;
  while(value) {
    result++; value=value>>1;
  }

  int shift=4;
  if (result<=shift) shift=result-1;
  long long part=((original_value-(1<<result))>>(result-shift))&0xf;

  part=15-part;
  result=63-result;
  
  return (result<<4)|part;
}

int find_highest_priority_bundle()
{
  long long this_bundle_priority=0;
  long long highest_bundle_priority=0;
  int i;
  int highest_priority_bundle=-1;
  int highest_priority_bundle_peers_dont_have_it=0;

  for(i=0;i<bundle_count;i++) {

    this_bundle_priority=0;

    // Bigger values in this list means that factor is more important in selecting
    // which bundle to announce next.
    // Sent less recently is less important than size or whether it is meshms.
    // This ensures that we rotate through the bundles we have.
    // If a peer comes along who doesn't have a smaller or meshms bundle, then
    // the less-peers-have-it priority will kick in.

    // Size-based priority is a value between 0 - 0x3FF
    // Number of peers who lack a bundle is added to this to boost priority a little
    // for bundles which are wanted by more peers
#define BUNDLE_PRIORITY_SENT_LESS_RECENTLY    0x00000400
#define BUNDLE_PRIORITY_RECIPIENT_IS_A_PEER   0x00002000
#define BUNDLE_PRIORITY_IS_MESHMS             0x00004000
    // Transmit now only gently escalates priority, so that we can override it if
    // we think we have a bundle that they should receive
#define BUNDLE_PRIORITY_TRANSMIT_NOW          0x00000040

    long long time_delta=0;
    
    if (highest_priority_bundle>=0) {
      time_delta=bundles[highest_priority_bundle].last_announced_time
	-bundles[i].last_announced_time;
      
      this_bundle_priority = lengthToPriority(bundles[i].length);
    } else time_delta=0;

    if (bundles[i].transmit_now)
      if (bundles[i].transmit_now>=time(0)) {
	this_bundle_priority+=BUNDLE_PRIORITY_TRANSMIT_NOW;
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

    int num_peers_that_dont_have_it=0;
    int peer;
    time_t peer_observation_time_cutoff=time(0)-PEER_KEEPALIVE_INTERVAL;
    for(peer=0;peer<peer_count;peer++) {
      if (peer_records[peer]->last_message_time>=peer_observation_time_cutoff)
	if (!peer_has_this_bundle_or_newer(peer,
					   bundles[i].bid,
					   bundles[i].version))
	  num_peers_that_dont_have_it++;
    }

    // Add to priority according to the number of peers that don't have the bundle
    this_bundle_priority+=num_peers_that_dont_have_it;
    if (num_peers_that_dont_have_it>bundles[i].num_peers_that_dont_have_it) {
      // More peer(s) have arrived who have not got this bundle yet, so reset the
      // last sent time for this bundle.
      bundles[i].last_announced_time=0;
    }
    bundles[i].num_peers_that_dont_have_it=num_peers_that_dont_have_it;

    if ((time_delta>=0LL)&& num_peers_that_dont_have_it) {
      // We only apply the less-recently-sent priority flag if there are peers who
      // don't yet have it.
      // XXX - This is still a bit troublesome, because we may not have announced the
      // bar, and the segment headers don't have enough information for the far end
      // to start actively requesting the bundle. To solve this, we should provide the
      // BAR of a bundle at least some of the time when presenting pieces of it.
      this_bundle_priority+=BUNDLE_PRIORITY_SENT_LESS_RECENTLY;
    }
    
    if (0)
      fprintf(stderr,"  bundle %s was last announced %ld seconds ago.  "
	      "Priority = 0x%llx, %d peers don't have it.\n",
	      bundles[i].bid,time(0)-bundles[i].last_announced_time,
	      this_bundle_priority,num_peers_that_dont_have_it);

    // Indicate this bundle as highest priority, unless we have found another one that
    // is higher priority.
    // Replace if priority is equal, so that newer bundles take priorty over older
    // ones.
    {
      if ((i==0)||(this_bundle_priority>highest_bundle_priority)) {
	if (0) fprintf(stderr,"  bundle %d is higher priority than bundle %d"
		       " (%08llx vs %08llx)\n",
		       i,highest_priority_bundle,
		       this_bundle_priority,highest_bundle_priority);
	highest_bundle_priority=this_bundle_priority;
	highest_priority_bundle=i;
	highest_priority_bundle_peers_dont_have_it=num_peers_that_dont_have_it;
      }
    }

    // Remember last calculated priority so that we can help debug problems with
    // priority calculation.
    bundles[i].last_priority=this_bundle_priority;
    
  }
  
  return highest_priority_bundle;
}

int load_rhizome_db(int timeout,
		    char *prefix, char *servald_server,
		    char *credential, char **token)
{
  char path[8192];
  
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
    fprintf(stderr,"could not open output file.\n");
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
    fprintf(stderr,"rhizome HTTP API request failed. URLPATH:%s\n",path);
    return -1;
  }

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
	// XXX - This token is only reliable if we read the complete list in this call.

	if (!ignore_token) {
	  if (*token) free(*token);
	  *token=strdup(fields[0]);
	  if (1) fprintf(stderr,"Saw rhizome progressive fetch token '%s'\n",*token);
	} else {
	  if (1) fprintf(stderr,"Ignoring rhizome progressive fetch token '%s' because of timeout\n",*token);
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
  
  fprintf(stderr,"Submitting rhizome bundle: manifest len=%d, body len=%d\n",
	  manifest_length,body_length);

  int result_code=http_post_bundle(servald_server,credential,
				   "/rhizome/import",
				   manifest_data,manifest_length,
				   body_data,body_length,
				   15000);
  
  if(result_code<200|| result_code>202) {
    fprintf(stderr,"POST bundle to rhizome failed: http result = %d\n",result_code);
    
    return -1;
  }
  
  return 0;
}

int clear_partial(struct partial_bundle *p)
{
  fprintf(stderr,"+++++ clearing partial\n");
  while(p->manifest_segments) {
    struct segment_list *s=p->manifest_segments;
    p->manifest_segments=s->next;
    if (s->data) free(s->data); s->data=NULL;
    free(s);
    s=NULL;
  }
  while(p->body_segments) {
    struct segment_list *s=p->body_segments;
    p->body_segments=s->next;
    if (s->data) free(s->data); s->data=NULL;
    free(s);
    s=NULL;
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
      if (debug_pieces)
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
      free(me->data); me->data=NULL;
      free(me); me=NULL;
    } else 
      s=&(*s)->next;
  }
  return 0;
}
