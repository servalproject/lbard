/*
  The Outernet Uplink driver is a bit different to the other drives.
  Whereas the others send one bundle at a time, and use the normal
  mechanism for doing so, we must cope with the fact that we have 
  a uni-directional communications link.  This means that we need to
  periodically assess the highest priority bundles we hold, and know
  which ones we have recently uplinked, and which we have not.

  There is some care required to tune this appropriately.
  It is important that small new bundles with high priority are transmitted
  as soon as possible, so that early warning of disaster messages can
  be received in a timely manner.
  Also, because the satellite link may end up with lost packets, we need
  to both interleave and apply some level of redundancy.  We will use 3+1
  RAID5-style parity, together with a 1:5 interleave, i.e., we will uplink
  five bundles simultaneously, with one packet from each being sent, and for
  every 3 packets, we will provide one parity packet.  This means that at
  least six consecutive packets must be lost before there will be a problem
  with reception.  However, it is of course possible that problems will still
  occur, and so we must retransmit high priority bundles repeatedly.  For now,
  this will be managed by having the Rhizome database for the uplink side having
  to exercise restraint at the number of bundles that it is pushing.  To also
  help minimise latency, the five simultaneous upload lanes will be allocated
  to different bundle sizes, similar to how Rhizome over Wi-Fi works, so that
  a large bundle can continue to be uplinked without preventing the immediate
  uplink of new small bundles.  

  Thus, for each of the five uplink lanes, we should produce a list of the bundles
  in it, and uplink them endlessly in a loop.  When a new bundle is detected (or
  a new version of an old bundle), it should most likely be immediately uplinked.

  The remaining question is whether we should use the priority score of the bundles
  within a lane to affect the frequency of uplink of each, i.e., so that high
  priority bundles can be uplinked repeatedly over a relatively short period of time.
*/
/*
The following specially formatted comments tell the LBARD build environment about this radio.
See radio_type for the meaning of each field.
See radios.h target in Makefile to see how this comment is used to register support for the radio.

RADIO TYPE: OUTERNET,"outernet","Outernet.is broadcast satellite",outernet_radio_detect,outernet_serviceloop,outernet_receive_bytes,outernet_send_packet,outernet_check_if_ready,10

*/

#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <time.h>
#include <termios.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "sync.h"
#include "lbard.h"
#include "hf.h"
#include "radios.h"
#include "code_instrumentation.h"

// Import serial_port string from main.c
extern char *serial_port;

// Address of IP link
struct sockaddr_in addr_uplink;

// TX queues for each lane.
struct outernet_lane_tx_queue {
  int bundle_numbers[MAX_BUNDLES];
  int queue_len;

  // Size of bundles this lane handles
  int min_size;
  int max_size;

  // Flattened form of bundle we are currently uplinking.
  int serialised_bundle_number;
  unsigned char *serialised_bundle;
  int serialised_len;
  int serialised_offset;
};

#define UPLINK_LANES 5
struct outernet_lane_tx_queue *lane_queues[UPLINK_LANES]={NULL};

int outernet_lane_queue_setup(void)
{
  LOG_ENTRY;

  int retVal=0;
  
  do {
    // Allocate queues
    for(int i=0;i<UPLINK_LANES;i++) {
      if (!lane_queues[i]) {
	lane_queues[i]=calloc(sizeof(struct outernet_lane_tx_queue),1);
	if (!lane_queues[i]) {
	  LOG_ERROR("calloc() of lane_queue[%d] failed. Out of memory?",i);
	  retVal=-1;
	}
	else {
	  /* Set size thresholds for each lane.
	     We will use the following cut points:
	     Lane 0 - < 1KiB
	     Lane 1 - < 4KiB
	     Lane 2 - < 16KiB
	     Lane 3 - < 64KiB
	     Lane 4 - All others.
	     (i.e., 1KiB << (lane * 2))
	  */
	  if (i) lane_queues[i]->min_size=1+ (1<<(10+i+i));
	  else lane_queues[i]->min_size=0;
	  if (i<4) 
	    lane_queues[i]->max_size=(1<<(10+(i+1)+(i+1)))-1;
	  else
	    lane_queues[i]->max_size=0x7fffffff;	    
	  // Not currently uplinking anything
	  lane_queues[i]->serialised_bundle_number=-1;

	  LOG_NOTE("Outernet uplink lane #%d successfully initialised.",i);
	}
      }
    }

  } while(0);
  return retVal;
  LOG_EXIT;
}

int outernet_uplink_next_in_queue(int lane)
{
  /* Pick the first item in the queue, serialise it, and mark
     it ready for uplinking.

     We then put that item to the end of the queue, so that we 
     round-robin through all the items in the lane's queue.

     XXX -- We should have a prioritisation scheme, once we have
     a means of setting the priority of the bundles. For now this
     has to be managed by not having too many bundles in the
     queue for the lane.
  */

  LOG_ENTRY;
  int retVal=0;
  
  do {
    int bundle=-1;
    
    // Work out next bundle
    if (lane<0||lane>4) { retVal=-1; break; } // lane exists?
    if (!lane_queues[lane]->queue_len) break; // lane has a queue?

    if (lane_queues[lane]->serialised_bundle_number!=-1) {
      LOG_ERROR("Must dequeue bundle being transmitted before calling outernet_upline_next_in_queue() for lane #%d",lane);
      retVal=-1;
      break;
    }
    
    // Get bundle # of head of queue
    bundle=lane_queues[lane]->bundle_numbers[0];
    // Move head of queue to tail of queue
    int n;
    for(n=0;n<lane_queues[lane]->queue_len-1;n++)
      lane_queues[lane]->bundle_numbers[n]
	=lane_queues[lane]->bundle_numbers[n+1];
    lane_queues[lane]->bundle_numbers[n]=bundle;
    
    // Get requested bundle in the bundle cache
    if (prime_bundle_cache(bundle,
			   my_sid_hex,servald_server,credential))
      {
	LOG_ERROR("Failed to prime bundle cache for bundle #%d",bundle);
      }
    if (cached_body_len<0||cached_manifest_encoded_len<0) {
      retVal=-1;
      break;
    }
    
    /* Build serialised version.
       We use a very simple file format:
       2 bytes = length of encoded manifest,
       followed by manifest and body.
    */       
    int serialised_len=2+cached_manifest_encoded_len+cached_body_len;
    unsigned char *serialised_data=malloc(serialised_len);
    if (!serialised_data) {
      LOG_ERROR("Could not allocate buffer for serialised data for bundle #%d (manifest len=%d, body len=%d)",
		bundle,cached_manifest_encoded_len,cached_body_len);
      retVal=-1;
      break;
    }

    // Set the fields in the serialised data
    serialised_data[0]=(cached_manifest_encoded_len>>0)&0xff;
    serialised_data[1]=(cached_manifest_encoded_len>>0)&0xff;
    bcopy(cached_manifest_encoded,&serialised_data[2],cached_manifest_encoded_len);
    bcopy(cached_body,&serialised_data[2+cached_manifest_encoded_len],cached_body_len);

    // Store in lane
    lane_queues[lane]->serialised_bundle=serialised_data;
    lane_queues[lane]->serialised_len=serialised_len;
    lane_queues[lane]->serialised_offset=0;
    lane_queues[lane]->serialised_bundle_number=bundle;
    LOG_NOTE("Bundle #%d serialised and ready for uplink in lane #%d",
	     bundle,lane);
    
  } while(0);
  return retVal;
  LOG_EXIT;
}


int outernet_uplink_lane_dequeue_current(int lane)
{
  LOG_ENTRY;

  lane_queues[lane]->serialised_bundle_number=-1;
  if (lane_queues[lane]->serialised_bundle)
    free(lane_queues[lane]->serialised_bundle);
  lane_queues[lane]->serialised_bundle=NULL;
  
  outernet_uplink_next_in_queue(lane);
  
  return 0;
  LOG_EXIT;
}

int outernet_uplink_lane_dequeue_bundle(int lane,int bundle)
{
  LOG_ENTRY;
  // Remove from active uplink if it was being uplinked.
  if (lane_queues[lane]->serialised_bundle_number==bundle) {
    LOG_NOTE("Stopping uplink of bundle #%d in lane #%d",
	     bundle,lane);
    outernet_uplink_lane_dequeue_current(lane);
  }

  // Remove the bundle from the queue, if present.
  for(int n=0;n<lane_queues[lane]->queue_len;n++)
    {
      if (lane_queues[lane]->bundle_numbers[n]==bundle) {
	LOG_NOTE("Removing bundle #%d from outernet uplink lane #%d",
		 bundle,lane);
	for(int m=n;m<(lane_queues[lane]->queue_len-1);m++)
	  lane_queues[lane]->bundle_numbers[m]=
	    lane_queues[lane]->bundle_numbers[m+1];
	lane_queues[lane]->queue_len--;
	break;
      }
    }

  return 0;
  LOG_EXIT;
}

int outernet_upline_queue_triage(void)
{
  /* Look at new/updated bundles, and update the uplink lane queues.
     NOTE: As the uplink server will typically run on well resourced hardware,
     we don't have to be quite so careful about run time and memory use here.
  */

  LOG_ENTRY;
  int retVal=0;

  // Go through newly arrived/updated bundles
  LOG_NOTE("Examining %d fresh bundles.",fresh_bundle_count);
  for(int i=0;i<fresh_bundle_count;i++) {
    int b=fresh_bundles[i];
    int lane;
    for(lane=0;lane<5;lane++) {
      if ((bundles[b].length>=lane_queues[lane]->min_size)
	  &&(bundles[b].length<=lane_queues[lane]->max_size)) {
	LOG_NOTE("Newly received bundle #%d of length %d goes in lane #%d\n",
		 b,bundles[b].length,lane);
	if (lane_queues[lane]->queue_len>=MAX_BUNDLES) {
	  LOG_ERROR("Uplink lane #%d is full. This should not be possible.",lane);
	  break;
	}
	int bb;
	// Check if already queued
	for(bb=0;bb<lane_queues[lane]->queue_len;bb++)
	  if (b==lane_queues[lane]->bundle_numbers[bb]) {
	    LOG_NOTE("Bundle #%d remains in lane #%d after update.",
		     b,lane);
	    break;
	  }
	if (bb==lane_queues[lane]->queue_len) {
	    LOG_NOTE("Bundle #%d added to uplink lane #%d.",
		     b,lane);
	    lane_queues[lane]->bundle_numbers[lane_queues[lane]->queue_len++]=b;
	} else {
	  // This bundle wasn't previously in this lane, so check
	  // the other lanes, in case it has changed size and needs
	  // to move from one lane to another
	  for(int l=0;l<5;l++)
	    if (l!=lane) outernet_uplink_lane_dequeue_bundle(l,b);
	}
	break;
      }
    }
    if (lane==5) {
      LOG_WARN("Newly received bundle #%d of length %d doesn't match any lane, so will not be uplinked\n",
	       b,bundles[b].length);
      retVal++;
      if (retVal<1) retVal=1;
    }
  }

  fresh_bundle_count=0;

  return retVal;
  LOG_EXIT;
}

int outernet_radio_detect(int fd)
{
  /*
    The outernet.is satellite service has a UDP packet injection interface,
    and the receivers also provide received packets from the satellite via
    UDP packets, so we can use a common configuration scheme for both.
    Well, actually, we want LBARD to accept packets from a nearby Outernet 
    receiver, even if it is using a different radio driver, so we really
    only want a driver for the uplink side of the Outernet service.

    Basically we want to open a UDP socket to the outernet server, which we
    will parse from the serial_port string.    
  */

  LOG_NOTE("Beginning Outernet auto detection");
  
  int retVal=-1;
  
  char hostname[1024]="";
  int port=-1;

  struct in_addr hostaddr={0};

  LOG_ENTRY;
  
  do {
    int fields=sscanf(serial_port,"outernet://%[^:]:%d",hostname,&port);
    LOG_NOTE("Parsed %d fields",fields);
    if (fields==2) {
      fprintf(stderr,"Parsed Outernet URI. Host='%s', port='%d'\n",hostname,port);
      LOG_NOTE("Parsed Outernet URI. Host='%s', port='%d'\n",hostname,port);
      
      if (inet_aton(hostname,&hostaddr)==1) {
	LOG_NOTE("Parsed hostname as IPv4 address");
      } else {

	LOG_NOTE("Attempting to resolve hostname '%s' to IP address",hostname);
	struct hostent *he=gethostbyname(hostname);
	
	if (!he) {
	  LOG_ERROR("Failed to resolve hostname '%s' to IP",hostname);
	  break;
	}
	struct in_addr **addr_list=(struct in_addr **) he->h_addr_list;
	
	if (!addr_list) {
	  fprintf(stderr,"Could not get IP for hostname '%s' (h_addr_list empty)",hostname);
	  LOG_ERROR("Could not get IP for hostname '%s' (h_addr_list empty)",hostname);
	  break;
	}

	// XXX - We assume IPv4 addressing here! We should support IPv6 as well
	if (he->h_addrtype!=AF_INET) {
	  LOG_ERROR("Address of '%s' is not IPv4",hostname);
	  break;
	}
	
	hostaddr=*addr_list[0];
      }

      fprintf(stderr,"Host address of '%s' is %08x\n",hostname,hostaddr.s_addr);
      
      struct sockaddr_in addr_us;
      
      bzero((char *) &addr_us, sizeof(struct sockaddr_in));
      bzero((char *) &addr_uplink, sizeof(struct sockaddr_in));
      
      // Set up address for our side of the socket
      addr_us.sin_family = AF_INET;
      addr_us.sin_port = htons(port);
      addr_us.sin_addr.s_addr = htonl(INADDR_ANY);
      
      // Setup address for Outernet's server
      addr_uplink.sin_family = AF_INET;
      addr_uplink.sin_port = htons(port);
      addr_uplink.sin_addr.s_addr = hostaddr.s_addr;
      
      if ((fd=socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
	{
	  perror("Failed to create UDP socket");
	  LOG_ERROR("Failed to create UDP socket");
	  break;
	}    

      if( bind(fd, (struct sockaddr*)&addr_us, sizeof(struct sockaddr_in) ) == -1)
	{
	  perror("Failed to bind UDP socket");
	  LOG_ERROR("Failed to bind UDP socket");
	  break;
	}
      
      // XXX to the other missing steps
      
      // Successfully connected
      LOG_NOTE("Detected radio as Outernet");
      radio_set_type(RADIOTYPE_OUTERNET);
      retVal=1; // successfully autodetected, stop auto-detect process
    } else {
      LOG_NOTE("URI is not for outernet uplink: '%s'",serial_port);
    }
  }
  while(0);
  
  LOG_EXIT;
  return retVal;
}

int outernet_check_if_ready(void)
{
  return -1;
}

int outernet_serviceloop(int serialfd)
{
  LOG_ENTRY;
  
  outernet_upline_queue_triage();
  
  return 0;
  LOG_EXIT;
}

int outernet_receive_bytes(unsigned char *bytes,int count)
{ 

  return 0;
}

int outernet_send_packet(int serialfd,unsigned char *out, int len)
{
  
  return 0;
}

