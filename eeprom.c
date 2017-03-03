#include <stdio.h>
#include <stdlib.h>
#include <sys/fcntl.h>
#include <strings.h>
#include <string.h>
#include <unistd.h>
#include "sha3.h"

int eeprom_write_page(int fd, int address,unsigned char *readblock);

int eeprom_decode_data(char *msg,unsigned char *datablock)
{

  // Parse radio parameter block
  sha3_Init256();
  sha3_Update(&datablock[2048-64],48);
  sha3_Finalize();
  int i;
  for(i=0;i<16;i++)
    if (datablock[2048-16+i]!=ctx.s[i>>3][i&7]) break;
  if (i==16) {
    fprintf(stderr,"Radio parameter block checksum valid.\n");
    fprintf(stderr,"       TX power = %d dBm\n",
	    (datablock[2048-32+0]<<8)+(datablock[2048-32+1]<<0));
    fprintf(stderr,"      Air speed = %d Kbit/sec\n",
	    (datablock[2048-32+2]<<8)+(datablock[2048-32+3]<<0));
    fprintf(stderr,"      Frequency = %d MHz\n",
	    (datablock[2048-32+4]<<8)+(datablock[2048-32+5]<<0));
    fprintf(stderr,"  Firmware lock = %c\n",
	    datablock[2048-32+13]);
    fprintf(stderr,"       ISO code = %c%c\n",
	    datablock[2048-32+14],datablock[2048-32+15]);
  }
  else fprintf(stderr,
	       "ERROR: Radio parameter block checksum is wrong:\n"	       
	       "       Radio will ignore EEPROM data!\n");

  // Parse extended regulatory information (country list etc)
  sha3_Init256();
  sha3_Update(&datablock[1024],1024-64-16);
  sha3_Finalize();
  for(i=0;i<16;i++)
    if (datablock[2048-64-16+i]!=ctx.s[i>>3][i&7]) break;
  if (i==16) {
    fprintf(stderr,
	    "Radio regulatory information text checksum is valid.\n"
	    "The information text is as follows:\n  > ");
    for(i=1024;i<2048-64-16;i++) {
      if (datablock[i]) fprintf(stderr,"%c",datablock[i]);
      if ((datablock[i]=='\r')||(datablock[i]=='\n')) fprintf(stderr,"  > ");
    }
    fprintf(stderr,"\n");
  } else fprintf(stderr,
		 "ERROR: Radio regulatory information text checksum is wrong:\n"	       
		 "       LBARD will report only ISO code from radio parameter block.\n");

  // Parse user extended information area
  sha3_Init256();
  sha3_Update(&datablock[0],1024-16);
  sha3_Finalize();
  for(i=0;i<16;i++)
    if (datablock[1024-16+i]!=ctx.s[i>>3][i&7]) break;
  if (i==16) {
    fprintf(stderr,
	    "Extended user-supplied information text checksum is valid.\n"
	    "The information text is as follows:\n  > ");
    for(i=0;i<1024-16;i++) {
      if (datablock[i]) fprintf(stderr,"%c",datablock[i]);
      if ((datablock[i]=='\r')||(datablock[i]=='\n')) fprintf(stderr,"  > ");
    }
    fprintf(stderr,"\n");
  } else
    fprintf(stderr,
	    "ERROR: Extended user-supplied information text checksum is wrong:\n");
  
  return 0;
}

int eeprom_parse_line(char *line,unsigned char *datablock)
{
  int address;
  int b[16];
  int err;
  if (sscanf(line,"EPR:%x : READ ERROR #%d",&address,&err)==2)
    {
      fprintf(stderr,"EEPROM read error #%d @ 0x%x\n",err,address);
      for(int i=0;i<16;i++) datablock[address+i]=0xee;
    }

  if (sscanf(line,"EPR:%x : %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x",
	     &address,
	     &b[0],&b[1],&b[2],&b[3],
	     &b[4],&b[5],&b[6],&b[7],
	     &b[8],&b[9],&b[10],&b[11],
	     &b[12],&b[13],&b[14],&b[15])==17) {
    for(int i=0;i<16;i++) datablock[address+i]=b[i];
  }
  
  return 0;
}

char line[1024];
int line_len=0;

int eeprom_parse_output(int fd,unsigned char *datablock)
{
  char buffer[16384];
  int count=read(fd,buffer,16384);
  
  for(int i=0;i<count;i++) {
    if (line_len) {
      if (buffer[i]!='\r')
	{ if (line_len<1000) line[line_len++]=buffer[i]; }
      else {
	line[line_len]=0;
	eeprom_parse_line(line,datablock);
	line_len=0;
      }
    } else {
      if ((buffer[i]=='E')&&(buffer[i+1]=='P')) {
	line[0]='E'; line_len=1;
      }
    }
  }
  if (line_len) eeprom_parse_line(line,datablock);
  
  return 0;
}

int eeprom_read(int fd)
{
  unsigned char readblock[2048];
  char cmd[1024];
  
  fprintf(stderr,"Reading data from EEPROM"); fflush(stderr);
  for(int address=0;address<0x800;address+=0x80) {
    snprintf(cmd,1024,"%x!g",address);
    write(fd,(unsigned char *)cmd,strlen(cmd));
    usleep(20000);
    snprintf(cmd,1024,"!E");
    write(fd,(unsigned char *)cmd,strlen(cmd));
    usleep(300000);
    eeprom_parse_output(fd,readblock);
    fprintf(stderr,"."); fflush(stderr);
  }
  fprintf(stderr,"\n"); fflush(stderr);
    
  eeprom_decode_data("Datablock read from EEPROM",readblock);
  
  
  return 0;      
}
