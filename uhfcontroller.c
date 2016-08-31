

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

#include "sync.h"
#include "lbard.h"

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

// About one message per second on RFD900
// We add random()%250 ms to this, so we deduct half of that from the base
// interval, so that on average we obtain one message per second.
// 128K air speed / 230K serial speed means that we can in principle send
// about 128K / 256 = 512 packets per second. However, the FTDI serial USB
// drivers for Mac crash well before that point.
int message_update_interval=INITIAL_AVG_PACKET_TX_INTERVAL-(INITIAL_PACKET_TX_INTERVAL_RANDOMNESS/2);  // ms
int message_update_interval_randomness=INITIAL_PACKET_TX_INTERVAL_RANDOMNESS;
long long last_message_update_time=0;
long long congestion_update_time=0;

#define MAX_PACKET_SIZE 255

// This need only be the maximum control header size + maximum packet size
#define RADIO_RXBUFFER_SIZE 16+MAX_PACKET_SIZE
unsigned char radio_rx_buffer[RADIO_RXBUFFER_SIZE];

int radio_temperature=-1;
int last_rx_rssi=-1;
unsigned char *packet_data=NULL;


#include "fec-3.0.1/fixed.h"
void encode_rs_8(data_t *data, data_t *parity,int pad);
int decode_rs_8(data_t *data, int *eras_pos, int no_eras, int pad);
#define FEC_LENGTH 32
#define FEC_MAX_BYTES 223

extern unsigned char my_sid[32];
extern char *my_sid_hex;
extern char *servald_server;
extern char *credential;
extern char *prefix;

int uhf_serviceloop(int serialfd)
{
  // Deal with clocks running backwards sometimes
  if ((congestion_update_time-gettime_ms())>4000)
    congestion_update_time=gettime_ms()+4000;
  
  if (gettime_ms()>congestion_update_time) {
    /* Very 4 seconds count how many radio packets we have seen, so that we can
       dynamically adjust our packet rate based on our best estimate of the channel
       utilisation.  In other words, if there are only two devices on channel, we
       should be able to send packets very often. But if there are lots of stations
       on channel, then we should back-off.
    */
    
    double ratio = (radio_transmissions_seen+radio_transmissions_byus)
      *1.0/TARGET_TRANSMISSIONS_PER_4SECONDS;
    // printf("--- Congestion ratio = %.3f\n",ratio);
    if (ratio<0.95) {
      // Speed up: If we are way too slow, then double our rate
      // If not too slow, then just trim 10ms from our interval
      if (ratio<0.25) message_update_interval/=2;
      else {
	int adjust=10;
	if ((ratio<0.80)&&(message_update_interval>300)) adjust=20;
	if ((ratio<0.50)&&(message_update_interval>300)) adjust=50;
	if (ratio>0.90) adjust=3;
	// Only increase our packet rate, if we are not already hogging the channel
	// i.e., we are allowed to send at most 1/n of the packets.
	float max_packets_per_second=1;
	int active_peers=active_peer_count();
	if (active_peers) {
	  max_packets_per_second=(TARGET_TRANSMISSIONS_PER_4SECONDS/active_peers)
	    /4.0;
	}
	int minimum_interval=1000.0/max_packets_per_second;
	if (radio_transmissions_byus<=radio_transmissions_seen)
	  message_update_interval-=adjust;
	if (message_update_interval<minimum_interval)
	  message_update_interval=minimum_interval;
      }
    } else if (ratio>1.0) {
      // Slow down!  We slow down quickly, so as to try to avoid causing
      // too many colissions.
      message_update_interval*=(ratio+0.4);
      if (!message_update_interval) message_update_interval=50;
      if (message_update_interval>4000) message_update_interval=4000;
    }
    
    if (!radio_transmissions_seen) {
      // If we haven't seen anyone else transmit anything, then only transmit
      // at a slow rate, so that we don't jam the channel and flatten our battery
      // while waiting for a peer
      message_update_interval=1000;
    }
    
    // Make randomness 1/4 of interval, or 25ms, whichever is greater.
    // The addition of the randomness means that we should never actually reach
    // our target capacity.
    message_update_interval_randomness = message_update_interval >> 2;
    if (message_update_interval_randomness<25)
      message_update_interval_randomness=25;
    
    // Force message interval to be at least 150ms + randomness
    // This keeps duty cycle < about 10% always.
    // 4 - 5 packets per second is therefore the fastest that we will go
    // (256 byte packet @ 128kbit/sec takes ~20ms)
    if (message_update_interval<150)
      message_update_interval=150;
    
    printf("*** TXing every %d+1d%dms, ratio=%.3f (%d+%d)\n",
	   message_update_interval,message_update_interval_randomness,ratio,
	   radio_transmissions_seen,radio_transmissions_byus);
    congestion_update_time=gettime_ms()+4000;
    
    if (radio_transmissions_seen) {
      radio_silence_count=0;
    } else {
      radio_silence_count++;
      if (radio_silence_count>3) {
	// Radio silence for 4x4sec = 16 sec.
	// This might be due to a bug with the UHF radios where they just stop
	// receiving packets from other radios. Or it could just be that there is
	// no one to talk to. Anyway, resetting the radio is cheap, and fast, so
	// it is best to play it safe and just reset the radio.
	write_all(serialfd,"!Z",2);
	radio_silence_count=0;
      }
    }
    
    radio_transmissions_seen=0;
    radio_transmissions_byus=0;
  }
  
  return 0;
}

int uhf_receive_bytes(unsigned char *bytes,int count)
{
  int i;
  for(i=0;i<count;i++) {
      
    bcopy(&radio_rx_buffer[1],&radio_rx_buffer[0],RADIO_RXBUFFER_SIZE-1);
    radio_rx_buffer[RADIO_RXBUFFER_SIZE-1]=bytes[i];
    
    if ((radio_rx_buffer[RADIO_RXBUFFER_SIZE-1]==0xdd)
	&&(radio_rx_buffer[RADIO_RXBUFFER_SIZE-8]==0xec)
	&&(radio_rx_buffer[RADIO_RXBUFFER_SIZE-9]==0xce))
      {
	if (debug_gpio) {
	  printf("GPIO ADC values = ");
	  for(int j=0;j<6;j++) {
	    printf("%s0x%02x",
		   j?",":"",
		   radio_rx_buffer[RADIO_RXBUFFER_SIZE-7+j]);
	  }
	  printf(".  Radio TX interval = %dms, TX seen = %d, TX us = %d\n",
		 message_update_interval,
		 radio_transmissions_seen,
		 radio_transmissions_byus);
	}
      } else if ((radio_rx_buffer[RADIO_RXBUFFER_SIZE-1]==0x55)
		 &&(radio_rx_buffer[RADIO_RXBUFFER_SIZE-8]==0x55)
		 &&(radio_rx_buffer[RADIO_RXBUFFER_SIZE-9]==0xaa))
      {
	// Found RFD900 CSMA envelope: packet follows after this
	int packet_bytes=radio_rx_buffer[RADIO_RXBUFFER_SIZE-4];
	radio_temperature=radio_rx_buffer[RADIO_RXBUFFER_SIZE-5];
	last_rx_rssi=radio_rx_buffer[RADIO_RXBUFFER_SIZE-7];
	
	int buffer_space=radio_rx_buffer[RADIO_RXBUFFER_SIZE-3];
	buffer_space+=radio_rx_buffer[RADIO_RXBUFFER_SIZE-2]*256;	
	
	if (packet_bytes>MAX_PACKET_SIZE) packet_bytes=0;
	packet_data = &radio_rx_buffer[RADIO_RXBUFFER_SIZE-9-packet_bytes];
	radio_transmissions_seen++;
	
	if (packet_bytes) {
	  // Have whole packet
	  if (debug_radio)
	    message_buffer_length+=
	      snprintf(&message_buffer[message_buffer_length],
		       message_buffer_size-message_buffer_length,
		       "Saw RFD900 CSMA Data frame: temp=%dC, last rx RSSI=%d, frame len=%d\n",
		       radio_temperature, last_rx_rssi,
		       packet_bytes);
	  
	  if (debug_radio) dump_bytes("packet before decode_rs",packet_data,packet_bytes);
	  
	  int rs_error_count = decode_rs_8(packet_data,NULL,0,
					   FEC_MAX_BYTES-packet_bytes+FEC_LENGTH);
	  
	  if (debug_radio) dump_bytes("received packet",packet_data,packet_bytes);
	  
	  if (rs_error_count>=0&&rs_error_count<8) {
	    if (0) printf("CHECKPOINT: %s:%d %s() error counts = %d for packet of %d bytes.\n",
			  __FILE__,__LINE__,__FUNCTION__,
			  rs_error_count,packet_bytes);
	    
	    saw_message(packet_data,packet_bytes-FEC_LENGTH,
			my_sid_hex,prefix,servald_server,credential);
	    
	    // attach presumed SID prefix
	    if (debug_radio) {
	      if (message_buffer_length) message_buffer_length--; // chop NL
	      message_buffer_length+=
		snprintf(&message_buffer[message_buffer_length],
			 message_buffer_size-message_buffer_length,
			 ", FEC OK : sender SID=%02x%02x%02x%02x%02x%02x*\n",
			 packet_data[0],packet_data[1],packet_data[2],
			 packet_data[3],packet_data[4],packet_data[5]);
	    }
	    
	    if (monitor_mode)
	      {
		char sender_prefix[128];
		char monitor_log_buf[1024];
		bytes_to_prefix(&packet_data[0],sender_prefix);
		snprintf(monitor_log_buf,sizeof(monitor_log_buf),
			 "CSMA Data frame: temp=%dC, last rx RSSI=%d,"
			 " frame len=%d, FEC OK",
			 radio_temperature, last_rx_rssi,
			 packet_bytes);
		
		monitor_log(sender_prefix,NULL,monitor_log_buf);
	      }
	  } else {
	    if (debug_radio) {
	      if (message_buffer_length) message_buffer_length--; // chop NL
	      message_buffer_length+=
		snprintf(&message_buffer[message_buffer_length],
			 message_buffer_size-message_buffer_length,
			 ", FEC FAIL (rs_error_count=%d)\n",
			 rs_error_count);
	    }
	  }
	  
	  packet_bytes=0;
	}
      }
  }
  return 0;
}
