#include "fakecsmaradio.h"

// Emulate this bitrate on the radios
// (emulate some (but not all) collission modes for
// simultaneous transmission).
int emulated_bitrate = 128000;

int rfd900_read_byte(int client,unsigned char byte)
{
  switch(clients[client].rx_state) {
  case STATE_BANG:
    clients[client].rx_state=STATE_NORMAL;
    switch(byte) {
    case '!': // TX now
      {
	unsigned char packet[10000];
	int packet_len=0;
	int send_bytes=clients[client].buffer_count;
	if (send_bytes>255) send_bytes=255;

	// First the packet body, upto 255 bytes
	bcopy(&clients[client].buffer[0],
	      &packet[packet_len],
	      send_bytes);
	packet_len+=send_bytes;
	bcopy(&clients[client].buffer[send_bytes],
	      &clients[client].buffer[0],
	      clients[client].buffer_count-send_bytes);
	clients[client].buffer_count-=send_bytes;
	
	// Work out when the packet should be delivered
	// (include 8 bytes time for the preamble)
	// Calculate first in usec, then divide down to ms
	int transmission_time = 1000000*8*(8+send_bytes)/emulated_bitrate;
	transmission_time/=1000;
	total_transmission_time+=transmission_time;
	long long delivery_time = gettime_ms()+transmission_time;
	
	// Queue bytes for RX by remote side.
	// Set delay according to length of packet and chosen bit rate.
	// Note that this approach means that colliding packets will cause them to
	// fail to be delivered, which is probably a good thing
	printf("Radio #%d sends a packet of %d bytes at T+%lldms (TX will take %dms)\n",
	       client,packet_len,gettime_ms()-start_time,transmission_time);

	// Client == -1 tells filter process to log packet details for statistics
	// for post-analysis.
	filter_and_enqueue_packet_for_client(client,-1,delivery_time,packet,packet_len);
	
	// dump_bytes("packet",packet,packet_len);

	for(int j=0;j<client_count;j++) {
	  if (j!=client) {
	    filter_and_enqueue_packet_for_client(client,j,delivery_time,
						 packet,packet_len);
	  }	  
	}
      }
      break;
    case 'C':
      clients[client].buffer_count=0;
      break;
    case 'F': // Report flash version
      // Not required
      break;
    case 'H': // set TX power high
      // Not required
      printf("Setting radio #%d to high TX power\n",client);
      break;
    case 'L': // set TX power high
      // Not required
      printf("Setting radio #%d to low TX power\n",client);
      break;
    case 'R': // Reset radio paramegers
      // Not required
      break;
    case 'Z': // Reset radio
      clients[client].buffer_count=0;
      break;
    case 'V': // version
      write(clients[client].socket,"1",1);
      break;
    case '.': // escaped !
      if (clients[client].buffer_count<CLIENT_BUFFER_SIZE)
	clients[client].buffer[clients[client].buffer_count++]='!';
      break;
    default: // unknown escape
      write(clients[client].socket,"E",1);
      break;
    }
    
    break;
  case STATE_NORMAL:
    if (byte!='!') {
      if (clients[client].buffer_count<CLIENT_BUFFER_SIZE)
	clients[client].buffer[clients[client].buffer_count++]=byte;
    } else {
      clients[client].rx_state=STATE_BANG;
    }
    break;
  }

  return 0;
}
