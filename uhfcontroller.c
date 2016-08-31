

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
