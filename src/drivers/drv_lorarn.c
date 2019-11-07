/*
The following specially formatted comments tell the LBARD build environment about this radio.
See radio_type for the meaning of each field.
See radios.h target in Makefile to see how this comment is used to register support for the radio.

RADIO TYPE: LORARN,"lorarn","First LoRa-type radio",lorarn_radio_detect,lorarn_serviceloop,lorarn_receive_bytes,lorarn_send_packet,lorarn_check_if_ready,10
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

#define MAX_PACKET_SIZE 255
#define RADIO_RXBUFFER_SIZE 9+4+MAX_PACKET_SIZE+25
unsigned char radio_rx_buffer[RADIO_RXBUFFER_SIZE];

#define MSG_RECEPTION_START_SIZE 11
#define MSG_RECEPTION_END_SIZE 4
int processing_message;
int msg_length = 0;
char msg_reception_start[MSG_RECEPTION_START_SIZE] = "radio_rx  ";
char msg_reception_end[MSG_RECEPTION_END_SIZE] = "\n\n";

long long rx_update_time=0;


int last_rx_rssi;
extern char *my_sid_hex;
extern char *servald_server;
extern char *credential;
extern char *prefix;

void hex2str(unsigned char *input, unsigned char *output, int input_len)
{
	char hex_number[2];
	long number;

	for(int i=0;i<input_len; i+=2) {
		memcpy(hex_number, &input[i], 2);
		number = strtol(hex_number, 0, 16);
		output[i/2] = toascii(number);
	}
	output[-1] = '\0';
}

void str2hex(unsigned char *input, char *output, int input_len)
{
  for(int i=0; i<input_len; i++)
    sprintf((char*)(output+(2*i)),"%02X", input[i]);
}

int lorarn_radio_detect(int fd)
{
  if(fd==-1) return -1;

  char clearing_buffer[1024];
  char response_buffer[100];

  serial_setup_port_with_speed(fd, 57600);
  write_all(fd, "\r\n", 4);
  sleep(1);
  int count=read_nonblock(fd,clearing_buffer,1024);

  write_all(fd, "sys get ver\r\n", 15);
  sleep(1);
  count = read(fd, response_buffer, sizeof response_buffer);

  if(count == 0) return -1;

  char subbuff[7];
  memcpy( subbuff, &response_buffer[0], 6 );
  subbuff[6] = '\0';
  if( (strcmp(subbuff, "RN2903")==0) | (strcmp(subbuff, "RN2483")==0)) {
    fprintf(stderr,"LoRa Radio detected.\n");
    write_all(fd, "mac pause\r\n", 13); //initialisation
    usleep(500000);
    read(fd, response_buffer, sizeof response_buffer); // TODO : test of "invalid_param" return

    rx_update_time = gettime_ms();
    write_all(fd, "radio rx 0\r\n", 14); //initialize rx state
    usleep(500000);
    read(fd, response_buffer, sizeof response_buffer); // TODO : test of "invalid_param" return
    return 1;
  }

  return -1;

}

int lorarn_serviceloop(int serialfd)
{
  if(rx_update_time + gettime_ms() > 15000) {
    //char response_buffer[100];
    rx_update_time = gettime_ms();

    write_all(serialfd, "radio rx 0\r\n", 14); //reset rx state every 15sec
    usleep(500000);
    //read(serialfd, response_buffer, sizeof response_buffer); TODO : check if returning message of radio rx 0 interfer in the reception function
  }

  return 0;
}

int lorarn_receive_bytes(unsigned char *bytes,int count)
{ 

  for(int i=0;i<count;i++) {
    
    bcopy(&radio_rx_buffer[1],&radio_rx_buffer[0],RADIO_RXBUFFER_SIZE-1);
    radio_rx_buffer[RADIO_RXBUFFER_SIZE-1]=bytes[i];
    msg_length++;

    if(processing_message) {
      char sub[3];
    	memcpy(sub, &radio_rx_buffer[RADIO_RXBUFFER_SIZE - 2], 2);
	    if(strcmp(sub, msg_reception_end)==0) { //end of packet detected
        unsigned char message_hex[msg_length];
        memcpy(message_hex,
          &radio_rx_buffer[(RADIO_RXBUFFER_SIZE - msg_length)], msg_length); //get the useful data
        if(msg_length%2)
          printf("error: message is corrupted (size not pair)");
        else
        {
          unsigned char message[(msg_length/2) + 1]; // +1 for '\0' at the end of the message
          hex2str(message_hex, message, msg_length); // convert hex data to ascii data
          saw_packet(message, msg_length, last_rx_rssi,
			      my_sid_hex, prefix,
			      servald_server, credential); //transmit data to the upper layers
        }
	    	processing_message =0;
      }
    } else {
      char subbuff_tested[11];
      memcpy(subbuff_tested, 
        &radio_rx_buffer[RADIO_RXBUFFER_SIZE - MSG_RECEPTION_START_SIZE],
        MSG_RECEPTION_START_SIZE); // TODO: position and size check
      if(strcmp(subbuff_tested, msg_reception_start)==0) { //beginning of packet detected
        processing_message =1;
        msg_length = 0;
      }
    }
  }

  return 0;
}

int lorarn_send_packet(int serialfd,unsigned char *out, int len)
{
  int size_writed = 9 + (2*len) + 4;
  char message[size_writed];
  char temp[2*len];

  str2hex(out, temp, len);

	strcpy(message, "radio tx ");
  strcat(message, temp);
	/*for(int i=0; i < (2*len); i++)
		strcat(message, &temp[i++]);*/
	strcat(message, "\r\n");

  if(write_all(serialfd, message, size_writed) == -1) {
    serial_errors++;
    return -1;
  } else {
    //TODO : manage success and error in sending packet (radio return a message)
    serial_errors=0;
    return 0;
  }
  
}

int lorarn_check_if_ready(void)
{
  return 1;
}
