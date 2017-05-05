// Based oncode from:
// http://stackoverflow.com/questions/10359067/unix-domain-sockets-on-linux
// which in turn was sourced from:
// 

#include "fakecsmaradio.h"

int filter_verbose=1;

int packet_drop_threshold=0;

int packet_count=0;

char *socketname="/tmp/fakecsmaradio.socket";

struct client clients[MAX_CLIENTS];
int client_count=0;

long long start_time;

long long tx_log_manifest_bytes=0;
long long tx_log_payload_bytes=0;
long long tx_log_sync_bytes=0;
long long tx_log_transmitted_bytes=0;
long long tx_log_transmitted_packets=0;

char timestamp_str_out[1024];
char *timestamp_str(unsigned char *s)
{
  struct tm tm;
  time_t now=time(0);
  localtime_r(&now,&tm);
  struct timeval tv;
  gettimeofday(&tv, NULL);
  if (!s)
    snprintf(timestamp_str_out,1024,"[%02d:%02d.%02d.%03d RADIO]",
	     tm.tm_hour,tm.tm_min,tm.tm_sec,tv.tv_usec/1000);
  else
    snprintf(timestamp_str_out,1024,"[%02d:%02d.%02d.%03d %02X%02X*]",
	     tm.tm_hour,tm.tm_min,tm.tm_sec,tv.tv_usec/1000,
	     s[0],s[1]);
    
  return timestamp_str_out;
}

int set_nonblocking(int fd)
{
  fcntl(fd,F_SETFL,fcntl(fd, F_GETFL, NULL)|O_NONBLOCK);
  return 0;
}

long long gettime_ms()
{
  struct timeval nowtv;
  // If gettimeofday() fails or returns an invalid value, all else is lost!
  if (gettimeofday(&nowtv, NULL) == -1) return -1;

  return nowtv.tv_sec * 1000LL + nowtv.tv_usec / 1000;
}

int register_client(int client_socket, int radio_type)
{
  if (client_count>=MAX_CLIENTS) {
    fprintf(stderr,"Too many clients: Increase MAX_CLIENTS?\n");
    exit(-1);
  }

  bzero(&clients[client_count],sizeof(struct client));
  clients[client_count].socket = client_socket;
  clients[client_count].radio_type = radio_type;
  client_count++;

  set_nonblocking(client_socket);
  
  return 0;
}

void print_spaces(FILE *f,int col)
{
  for(int i=0;i<col;i++)
    fprintf(f," ");  
}


int dump_bytes(int col, char *msg,unsigned char *bytes,int length)
{
  print_spaces(stderr,col);
  fprintf(stderr,"%s:\n",msg);
  for(int i=0;i<length;i+=16) {
    print_spaces(stderr,col);
    fprintf(stderr,"%04X: ",i);
    for(int j=0;j<16;j++) if (i+j<length) fprintf(stderr," %02X",bytes[i+j]);
    fprintf(stderr,"\n");
  }
  return 0;
}

struct filter_rule {
  int src;
  int dst;
  int manifestP;
  int bodyP;
};

#define MAX_FILTER_RULES 1024
struct filter_rule *filter_rules[MAX_FILTER_RULES];
int filter_rule_count=0;

struct filterable {
  int src_radio;
  int dst_radio;
  
  uint8_t sender_sid_prefix[6];
  uint8_t recipient_sid_prefix[4];

  uint8_t type;

  uint8_t bid_prefix[8];
  uint64_t version;

  uint8_t is_manifest_piece;
  uint8_t is_body_piece;
  uint32_t manifest_offset;
  uint32_t body_offset;
  uint32_t manifest_length;
  uint32_t body_length;
  uint8_t body_log_length;
  uint16_t piece_length;
  uint16_t piece_packet_offset;  // where in packet the bytes of the piece start

  uint8_t stratum;
  uint64_t timestamp_sec;
  uint64_t timestamp_usec;
  uint32_t instance_id;

  // The segment of the packet covered by this fragment
  int packet_start;
  int fragment_length;
};

void filterable_erase_fragment(struct filterable *f,int offset)
{
  struct filterable ff;
  memcpy(&ff,f,sizeof(struct filterable));
  memset(f,0,sizeof(struct filterable));
  memcpy(f->sender_sid_prefix,ff.sender_sid_prefix,6);
  f->src_radio=ff.src_radio;
  f->dst_radio=ff.dst_radio;
  
  f->packet_start=offset;
}

void filterable_parse_timestamp(struct filterable *f,
				 const uint8_t *packet,int *offset)
{
  f->stratum=packet[(*offset)++];
  for(int i=0;i<8;i++) f->timestamp_sec|=((long long)packet[(*offset)+i])<<(i*8LL);
  (*offset)+=8;
  for(int i=0;i<3;i++) f->timestamp_usec|=((long long)packet[(*offset)+i])<<(i*8LL);
  (*offset)+=3;
}

void filterable_parse_bid_prefix(struct filterable *f,
				 const uint8_t *packet,int *offset)
{
  memcpy(f->bid_prefix,&packet[*offset],8); (*offset)+=8;
}

void filterable_parse_recipient_prefix_4(struct filterable *f,
				       const uint8_t *packet,int *offset)
{
  memcpy(f->recipient_sid_prefix,&packet[*offset],4); (*offset)+=4;
}

void filterable_parse_recipient_prefix_2(struct filterable *f,
					 const uint8_t *packet,int *offset)
{
  memcpy(f->recipient_sid_prefix,&packet[*offset],2); (*offset)+=2;
}


void filterable_parse_bundle_log_length(struct filterable *f,
					const uint8_t *packet,int *offset)
{
  f->body_log_length=packet[*offset]; (*offset)++;
}

void filterable_parse_version(struct filterable *f,const uint8_t *packet,
			      int *offset)
{
  for(int i=0;i<8;i++) f->version|=((long long)packet[(*offset)+i])<<(i*8LL);
  (*offset)+=8;
}

void filterable_parse_manifest_offset(struct filterable *f,const uint8_t *packet,
				      int *offset)
{
  f->manifest_offset=packet[*offset]|(packet[(*offset)+1]<<8); (*offset)+=2;
}

void filterable_parse_body_offset(struct filterable *f,const uint8_t *packet,
				  int *offset)
{
  f->body_offset=packet[*offset]|(packet[(*offset)+1]<<8)
    |(packet[(*offset)+2]<<16)|(packet[(*offset)+3]<<24);
  (*offset)+=4;
}

void filterable_parse_instance_id(struct filterable *f,const uint8_t *packet,
				  int *offset)
{
  for(int i=0;i<4;i++) f->instance_id|=((long long)packet[(*offset)+i])<<(i*8LL);
  (*offset)+=4;
}

void filterable_parse_offset_compound(struct filterable *f,const uint8_t *packet,
				      int *offset)
{
  uint64_t offset_compound=0;
  if ((f->type!='L')&&(!(f->type&0x20))) {
    for(int i=0;i<6;i++) offset_compound|=((long long)packet[(*offset)+i])<<(i*8LL);
    (*offset)+=6;
  } else {
    for(int i=0;i<4;i++) offset_compound|=((long long)packet[(*offset)+i])<<(i*8LL);
    (*offset)+=4;
  }
  int piece_bytes=(offset_compound>>20)&0x7ff;
  int piece_offset=(offset_compound&0xfffff)|((offset_compound>>12LL)&0xfff00000LL);
  if (offset_compound&0x80000000) {
    // Manifest offset
    f->manifest_offset=piece_offset;
    f->is_manifest_piece=1;
  } else {
    // Piece offset
    f->body_offset=piece_offset;
    f->is_body_piece=1;
  }
  f->piece_length=piece_bytes;
  f->piece_packet_offset=*offset;
  (*offset)+=piece_bytes;
  
}

void filterable_parse_segment_offset(struct filterable *f,const uint8_t *packet,
				     int *offset)
{
  uint32_t offset_compound=0;
  for(int i=0;i<3;i++) offset_compound|=((long long)packet[(*offset)+i])<<(i*8LL);
  (*offset)+=3;

  if (offset_compound&0x800000) {
    // Manifest offset
    f->manifest_offset=offset_compound&0x7fffff;
  } else {
    // Body offset
    f->body_offset=offset_compound&0x7fffff;
  }
}

char *fragment_name(int type)
{
  switch(type) {
  case 'p': return "Bundle end piece (offset < 1MB)";
  case 'P': return "Bundle end piece (offset >= 1MB)";
  case 'q': return "Bundle piece (offset < 1MB)";
  case 'Q': return "Bundle piece (offset >= 1MB)";
  case 'G': return "LBARD instance identifier";
  case 'T': return "Time stamp";
  case 'A': return "Bundle transfer progress acknowledgement";
  case 'a': return "Bundle transfer redirect and acknowledgement";
  default: return "unknown";
  }
}


/*
  Apply filters against the supplied fragment.
  If the fragment is not dropped, it should be appended to packet out
*/
int filter_fragment(uint8_t *packet_in,uint8_t *packet_out,int *out_len,
		    struct filterable *f, int log_pieces)
{  
  if (filter_verbose) {
    fprintf(stderr,"T+%lldms : from sid:%02X%02X%02X%02X%02X%02X* to sid:%02X%02X[%02X%02X]*\n",
	    gettime_ms()-start_time,
	    f->sender_sid_prefix[0],f->sender_sid_prefix[1],f->sender_sid_prefix[2],
	    f->sender_sid_prefix[3],f->sender_sid_prefix[4],f->sender_sid_prefix[5],
	    f->recipient_sid_prefix[0],f->recipient_sid_prefix[1],
	    f->recipient_sid_prefix[2],f->recipient_sid_prefix[3]);

    fprintf(stderr,"          Fragment type '%c' : %s\n",
	    f->type,fragment_name(f->type));

    if ((f->type!='G')&&(f->type!='T')) {
      fprintf(stderr,"          bid=%02X%02X%02X%02X%02X%02X%02X%02X*, version=%llx\n",
	      f->bid_prefix[0],f->bid_prefix[1],f->bid_prefix[2],f->bid_prefix[3],
	      f->bid_prefix[4],f->bid_prefix[5],f->bid_prefix[6],f->bid_prefix[7],
	      f->version);
      if (f->body_log_length)
	fprintf(stderr,"          manifest length=%d, body length=%d (or 2^%d)\n",
		f->manifest_length,f->body_length,f->body_log_length
		);
    }

    switch(f->type) {
    case 'P': case 'p':
      if (f->is_manifest_piece)
	fprintf(stderr,"          manifest length = %d\n",
		f->manifest_offset+f->piece_length-1);
      if (f->is_body_piece)
	fprintf(stderr,"          body length = %d\n",
		f->body_offset+f->piece_length-1);
      if (log_pieces)
	fprintf(stderr,">>> %s %02X%02X%02X%02X*/%lld %s %d..%d (packet #%d offset %d)\n",
		timestamp_str(f->sender_sid_prefix),
		f->bid_prefix[0],f->bid_prefix[1],f->bid_prefix[2],f->bid_prefix[3],
		f->version,
		f->is_manifest_piece?"manifest":(f->is_body_piece?"body":"unknown"),
		f->is_manifest_piece?f->manifest_offset:f->body_offset,
		f->is_manifest_piece?(f->manifest_offset+f->piece_length-1)
		:(f->body_offset+f->piece_length-1),
		packet_count,f->packet_start
		);
      break;
    case 'q': case 'Q':
      if (f->is_manifest_piece)
	fprintf(stderr,"          manifest bytes [%d..%d]\n",
		f->manifest_offset,f->manifest_offset+f->piece_length-1);
      if (f->is_body_piece)
	fprintf(stderr,"          body bytes [%d..%d] (%d bytes) @ T+%lldms\n",
		f->body_offset,f->body_offset+f->piece_length-1,f->piece_length,
		gettime_ms()-start_time);
      if (log_pieces)
	fprintf(stderr,">>> %s %02X%02X%02X%02X*/%lld %s %d..%d (packet #%d offset %d)\n",
		timestamp_str(f->sender_sid_prefix),
		f->bid_prefix[0],f->bid_prefix[1],f->bid_prefix[2],f->bid_prefix[3],
		f->version,
		f->is_manifest_piece?"manifest":(f->is_body_piece?"body":"unknown"),
		f->is_manifest_piece?f->manifest_offset:f->body_offset,
		f->is_manifest_piece?(f->manifest_offset+f->piece_length-1)
		:(f->body_offset+f->piece_length-1),
		packet_count,f->packet_start
		);
      // Display the actual bytes included
      dump_bytes(12,"Bytes of piece",
		 &packet_in[f->piece_packet_offset],f->piece_length);
      break;
    case 'A': case 'a':
      fprintf(stderr,"          Acknowledging to manifest offset %d, body offset %d\n",
	      f->manifest_offset,f->body_offset);
      if (f->type=='a') fprintf(stderr,"          Requesting redirection to a random offset thereafter.\n");
      break;
    }
  
  }

  int match=0;
  int r;
  fprintf(stderr,"There are %d filter rules.\n",filter_rule_count);
  for(r=0;r<filter_rule_count;r++) {
    match=1;
#if 0
    if ((f->type=='p')||(f->type=='P')||(f->type=='q')||(f->type=='Q')) {
      fprintf(stderr,"FILTER: rule: src=%d, dst=%d, mP=%d, pP=%d  -- fragment: src=%d, dst=%d, mP=%d, pP=%d\n",
	      filter_rules[r]->src,filter_rules[r]->dst,
	      filter_rules[r]->manifestP,filter_rules[r]->bodyP,
	      f->src_radio,f->dst_radio,
	      f->is_manifest_piece,f->is_body_piece);
    }
#endif
    if ((filter_rules[r]->src!=-1)&&(filter_rules[r]->src!=f->src_radio)) match=2;
    if ((filter_rules[r]->dst!=-1)&&(filter_rules[r]->dst!=f->dst_radio)) match=3;
    if (filter_rules[r]->manifestP&&(!f->is_manifest_piece)) match=4;
    if (filter_rules[r]->bodyP&&(!f->is_body_piece)) match=5;
    if (match>1) {
      if (0) fprintf(stderr,"  rule not matched due to criterion #%d\n",match);
      match=0; }
    if (match) break;
  }

  if (match) {
    fprintf(stderr,"         *** Fragment dropped due to filter rule #%d\n",r);
    return 1;
  }
  
  memcpy(&packet_out[*out_len],&packet_in[f->packet_start],f->fragment_length);
  (*out_len)+=f->fragment_length;
  
  return 0;
}

int filter_process_packet(int from,int to,
			  uint8_t *packet,int *packet_len)
{
  // XXX - Implement packet decode and filter
  // Ideally we will just delete any parts of the packet that are to be filtered,
  // and fix up the FEC code to be correct after.

  struct filterable f; 
  
  int len=*packet_len;
  uint8_t packet_out[256];
  int out_len=0;

  int offset=0;

  memset(&f,0,sizeof(f));
  f.src_radio=from; f.dst_radio=to;

  // Extract SID prefix of sender
  memcpy(f.sender_sid_prefix,&packet[offset],6); offset+=6;

  if (to==-1) {
    packet_count++;
    fprintf(stderr,">>> %s Packet #%d : length=%d bytes\n",
	    timestamp_str(f.sender_sid_prefix),
	    packet_count,*packet_len);
  }  
  
  // Ignore msg number and is_retransmission flag bytes
  offset+=2;

  // And copy those fields across to output packet
  memcpy(packet_out,packet,6+1+1);
  out_len=6+1+1;

  len-=FEC_LENGTH; // FEC length
  
  while(offset<len) {
    switch(packet[offset]) {
    case 'A': case 'a':
      // Ack of bundle transfer
      filterable_erase_fragment(&f,offset);
      f.type=packet[offset++];
      filterable_parse_bid_prefix(&f,packet,&offset);
      filterable_parse_manifest_offset(&f,packet,&offset);
      filterable_parse_body_offset(&f,packet,&offset);
      f.fragment_length=offset-f.packet_start;
      filter_fragment(packet,packet_out,&out_len,&f,to==-1);
      break;
    case 'B':
      // BAR announcement
      filterable_erase_fragment(&f,offset);
      f.type=packet[offset++];
      filterable_parse_bid_prefix(&f,packet,&offset);
      filterable_parse_version(&f,packet,&offset);
      filterable_parse_recipient_prefix_4(&f,packet,&offset);
      filterable_parse_bundle_log_length(&f,packet,&offset);
      f.fragment_length=offset-f.packet_start;
      filter_fragment(packet,packet_out,&out_len,&f,to==-1);
      break;
    case 'G':  // 32-bit instance ID of peer
      filterable_erase_fragment(&f,offset);
      f.type=packet[offset++];
      filterable_parse_instance_id(&f,packet,&offset);
      f.fragment_length=offset-f.packet_start;
      filter_fragment(packet,packet_out,&out_len,&f,to==-1);
      break;
    case 'L': // Length of bundle
      filterable_erase_fragment(&f,offset);
      f.type=packet[offset++];
      filterable_parse_bid_prefix(&f,packet,&offset);
      filterable_parse_version(&f,packet,&offset);
      filterable_parse_offset_compound(&f,packet,&offset);
      f.fragment_length=offset-f.packet_start;
      filter_fragment(packet,packet_out,&out_len,&f,to==-1);
      break;
    case 'P': case 'p': case 'q': case 'Q':
      // Piece of body or manifest
      filterable_erase_fragment(&f,offset);
      f.type=packet[offset++];
      filterable_parse_recipient_prefix_2(&f,packet,&offset);
      filterable_parse_bid_prefix(&f,packet,&offset);
      filterable_parse_version(&f,packet,&offset);
      filterable_parse_offset_compound(&f,packet,&offset);
      f.fragment_length=offset-f.packet_start;      
      if (!filter_fragment(packet,packet_out,&out_len,&f,to==-1)) {
	if (to==-1) {
	  // Log rhizome bytes actually sent
	  if (f.is_manifest_piece) tx_log_manifest_bytes+=f.piece_length;
	  if (f.is_body_piece) tx_log_payload_bytes+=f.piece_length;
	}
      }
      break;
    case 'R': // segment request
      // 2 bytes target SID
      // 8 bytes BID prefix
      // 3 bytes offset
      filterable_erase_fragment(&f,offset);
      f.type=packet[offset++];
      filterable_parse_recipient_prefix_2(&f,packet,&offset);
      filterable_parse_bid_prefix(&f,packet,&offset);
      filterable_parse_segment_offset(&f,packet,&offset);
      f.fragment_length=offset-f.packet_start;
      filter_fragment(packet,packet_out,&out_len,&f,to==-1);
      break;
    case 'S': // sync-tree message
      // We don't filter these, just copy the bytes
      memcpy(&packet_out[out_len],&packet[offset],packet[offset+1]);
      out_len+=packet[offset+1];
      if (to==-1) {
	// Log rhizome bytes actually sent
	tx_log_sync_bytes+=packet[offset+1];	
      }
      offset+=packet[offset+1];
      break;
    case 'T': // time stamp
      filterable_erase_fragment(&f,offset);
      f.type=packet[offset++];
      filterable_parse_timestamp(&f,packet,&offset);
      f.fragment_length=offset-f.packet_start;
      filter_fragment(packet,packet_out,&out_len,&f,to==-1);
      break;
    default:
      fprintf(stderr,"WARNING: Saw unknown fragment type 0x%02x @ %d -- Ignoring packet\n",
	      packet[offset],offset);
      dump_bytes(2,"Packet",packet,*packet_len);
      return -1;
    }
  }

#if 0
  if ((out_len!=len)||memcmp(packet,packet_out,out_len)) {
    fprintf(stderr,"Filtered packet contains %d/%d bytes.\n",
	    out_len,len);
    dump_bytes(2,"Filtered",packet_out,out_len);
    dump_bytes(2,"Original",packet,len);
  }
#endif

  // Append valid FEC
  unsigned char parity[FEC_LENGTH];
  encode_rs_8(packet_out,parity,FEC_MAX_BYTES-out_len);
  memcpy(&packet_out[out_len],parity,FEC_LENGTH);
  out_len+=FEC_LENGTH;

#if 0
  if (out_len!=len) dump_bytes(0,"With FEC",packet_out,out_len);
#endif
  
  // Now update packet
  memcpy(packet,packet_out,out_len);
  *packet_len=out_len;

  // Then build and attach envelope
  packet[(*packet_len)++]=0xaa;
  packet[(*packet_len)++]=0x55;
  packet[(*packet_len)++]=200; // RSSI of this frame
  packet[(*packet_len)++]=100; // Average RSSI remote side
  packet[(*packet_len)++]=28; // Temperature of this radio
  packet[(*packet_len)++]=out_len; // length of this packet
  packet[(*packet_len)++]=0xff;  // 16-bit RX buffer space (always claim 4095 bytes)
  packet[(*packet_len)++]=0x0f;
  packet[(*packet_len)++]=0x55;	

  if ((to==-1)&&out_len) {
    tx_log_transmitted_bytes+=out_len;
    tx_log_transmitted_packets++;
  }
  
  return 0;
}

int filter_and_enqueue_packet_for_client(int from,int to, long long delivery_time,
					 uint8_t *packet_in,int packet_len)
{
  fprintf(stderr,"Filter and enqueue %d bytes from %d -> %d\n",
	  packet_len,from,to);

  uint8_t packet[256];
  memcpy(packet,packet_in,packet_len);
  
  filter_process_packet(from,to,packet,&packet_len);

  if (to==-1)
    fprintf(stderr,">>> %s @ T+%lldms: %lld bytes, %lld packets, %lld sync bytes, %lld manifest bytes, %lld body bytes.\n",	    
	    timestamp_str(NULL),
	    gettime_ms()-start_time,
	    tx_log_transmitted_bytes,
	    tx_log_transmitted_packets,
	    tx_log_sync_bytes,
	    tx_log_manifest_bytes,
	    tx_log_payload_bytes);
  
  if (to==-1) {
    // Collect statistics only for this packet.
    // (We pass through the filters because we want to keep note of how many bytes
    // are sent of bundle manifests and payloads for working out our various
    // efficiency meansures.
    
    return 0;
  }

  if (!packet_len) {
    // Entire packet was filtered, so do nothing
    return 0;
  }
  
  bcopy(packet,clients[to].rx_queue,packet_len);
  long long now=gettime_ms();
  if (clients[to].rx_queue_len) {
    printf("WARNING: RX colission for radio #%d (embargo time = T%+lldms, last packet = %d bytes)\n",
	   to,clients[to].rx_embargo-now,clients[to].rx_queue_len);
    clients[to].rx_colission=1;
  } else clients[to].rx_colission=0;
  clients[to].rx_queue_len=packet_len;
  clients[to].rx_embargo=delivery_time;
  return 0;
}

int filter_rule_add(char *rule)
{
  if (filter_rule_count>=MAX_FILTER_RULES) {
    fprintf(stderr,"Too many filter rules. Increase MAX_FILTER_RULES.\n");
    exit(-1);
  }

  while(rule[0]==' ') rule++;
  
  char thing[1024];
  int src_radio;
  int offset;
  if (sscanf(rule,"drop %s from %d%n",thing,&src_radio,&offset)==2) {
    if (offset<strlen(rule)) {
      fprintf(stderr,"Could not parse filter rule '%s': Extraneous material at character %d\n",rule,offset);
    }
    struct filter_rule *r=calloc(sizeof(struct filter_rule),1);
    r->src=src_radio;
    r->dst=-1;
    if (!strcmp(thing,"manifest")) r->manifestP=1;
    else if (!strcmp(thing,"manifests")) r->manifestP=1;
    else if (!strcmp(thing,"body")) r->bodyP=1;
    else if (!strcmp(thing,"bodies")) r->bodyP=1;
    else {
      fprintf(stderr,"Could not parse filter rule '%s': Unknown object '%s'\n",rule,thing);
      return -1;
    }
    filter_rules[filter_rule_count++]=r;
    fprintf(stderr,"Added filter rule '%s'\n",rule);
    return 0;
  } else {
    fprintf(stderr,"Could not parse filter rule '%s'\n",rule);
    return -1;
  }
}

int filter_rules_parse(char *text)
{
  char rule[1024];
  int len=0;
  for(int i=0;i<=strlen(text);i++) {
    if (text[i]==';'||text[i]==0) {
      if (len) if (filter_rule_add(rule)) return -1;
      len=0; rule[0]=0;
    } else {
      if (len<1024) { rule[len++]=text[i]; rule[len]=0; }
    }
  }
  return 0;
}

int main(int argc,char **argv)
{
  int radio_count=2;
  FILE *tty_file=NULL;

  start_time=gettime_ms();

  char *radio_types="rfd900,rfd900";
  
  if (argv&&argv[1]) radio_types=argv[1];
  radio_count=1;
  for(int i=0;radio_types[i];i++) if (radio_types[i]==',') radio_count++;
  fprintf(stderr,"radio_count=%d\n",radio_count);
  
  if (argc>2) tty_file=fopen(argv[2],"w");
  if ((argc<3)||(argc>4)||(!tty_file)||(radio_count<2)||(radio_count>=MAX_CLIENTS)) {
    fprintf(stderr,"usage: fakecsmaradio <radio_type,...> <tty file> [packet drop probability|filter rules]\n");
    fprintf(stderr,"\nNumber of radios must be between 2 and %d.\n",MAX_CLIENTS-1);
    fprintf(stderr,"The name of each tty will be written to <tty file>\n");
    fprintf(stderr,"The optional packet drop probability allows the simulation of packet loss.\n");
    fprintf(stderr,"Filter rules take the form of:  \"drop <manifest|body> <from|to> <radio id>; ...\"\n");
    exit(-1);
  }
  if (argc>3) 
    {
      if (argv[3][0]=='d') {
	// Filter rules
	if (filter_rules_parse(argv[3])) {
	  fprintf(stderr,"Invalid filter rules.\n");
	  exit(-1);
	}
      } else {
	float p=atof(argv[3]);
	if (p<0||p>1) {
	  fprintf(stderr,"Packet drop probability must be in range [0..1]\n");
	  exit(-1);
	}
	packet_drop_threshold = p*0x7fffffff;
	fprintf(stderr,"Simulating %3.2f%% packet loss (threshold = 0x%08x)\n",
		p*100.0,packet_drop_threshold);
      }
    }
  srandom(time(0));

  char *r=radio_types;
  
  for(int i=0;i<radio_count;i++) {
    char radio_type[1024];
    int rt_len=0;
    for(int i=0;r[i]!=','&&r[i];i++)
      radio_type[rt_len++]=r[i];
    radio_type[rt_len]=0;
    r+=rt_len+1;
    fprintf(stderr,"Radio #%d is a '%s'\n",i,radio_type);
    
    int fd=posix_openpt(O_RDWR|O_NOCTTY);
    if (fd<0) {
      perror("posix_openpt");
      exit(-1);
    }
    grantpt(fd);
    unlockpt(fd);
    fcntl(fd,F_SETFL,fcntl(fd, F_GETFL, NULL)|O_NONBLOCK);
    fprintf(tty_file,"%s\n",ptsname(fd));
    printf("Radio #%d is available at %s\n",client_count,ptsname(fd));
    int radio_type_id=-1;
    if (!strcasecmp(radio_type,"rfd900")) radio_type_id=RADIO_RFD900;
    if (!strcasecmp(radio_type,"hfcodan")) radio_type_id=RADIO_HFCODAN;
    if (!strcasecmp(radio_type,"hfbarrett")) radio_type_id=RADIO_HFBARRETT;
    if (radio_type_id==-1) {
      fprintf(stderr,"Unknown radio type '%s'\n",radio_type);
      exit(-1);
    }
    register_client(fd,radio_type_id);
  }
  fclose(tty_file);
  
  long long last_heartbeat_time=0;
  
  // look for new clients, and for traffic from each client.
  while(1) {
    int activity=0;
    
    // Read from each client, and see if we have a packet to release
    long long now = gettime_ms();
    for(int i=0;i<client_count;i++) {
      unsigned char buffer[8192];
      int count = read(clients[i].socket,buffer,8192);
      if (count>0) {
	for(int j=0;j<count;j++) {
	  switch(clients[i].radio_type) {
	  case RADIO_RFD900: rfd900_read_byte(i,buffer[j]); break;
	  case RADIO_HFCODAN: hfcodan_read_byte(i,buffer[j]); break;
	  case RADIO_HFBARRETT: break;
	  }
	  activity++;
	}
      }
      
      // Release any queued packet once we pass the embargo time
      if (clients[i].rx_queue_len&&(clients[i].rx_embargo<now))
	{
	  if (!clients[i].rx_colission) {
	    if ((random()&0x7fffffff)>=packet_drop_threshold) {
	      write(clients[i].socket,
		    clients[i].rx_queue,
		    clients[i].rx_queue_len);
	      printf("Radio #%d receives a packet of %d bytes\n",
		     i,clients[i].rx_queue_len);
	    } else
	      printf("Radio #%d misses a packet of %d bytes due to simulated packet loss\n",
		     i,clients[i].rx_queue_len);
	      
	  }
	  printf("Radio #%d ready to receive.\n",i);
	  clients[i].rx_queue_len=0;
	  clients[i].rx_colission=0;
	  activity++;
	} else {
	if (clients[i].rx_embargo&&clients[i].rx_queue_len)
	  printf("Radio #%d WAITING until T+%lldms for a packet of %d bytes\n",
		 i,clients[i].rx_embargo-now,clients[i].rx_queue_len);

      }
    }

    if (last_heartbeat_time<(now-500)) {
      // Pretend to be reporting GPIO status so that lbard thinks the radio is alive.
      unsigned char heartbeat[9]={0xce,0xec,0xff,0xff,0xff,0xff,0xff,0xff,0xdd};
      for(int i=0;i<client_count;i++) {
	write(clients[i].socket,heartbeat, sizeof(heartbeat));
      }
      last_heartbeat_time=now;
    }

    // Sleep for 10ms if there has been no activity, else look for more activity
    if (!activity) usleep(10000);      
  }
  
}
