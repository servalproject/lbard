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

// Table of fields and transformations
struct manifest_field {
  unsigned char token;
  char *name;
  // How shall the field be encoded?
  // The first applicable strategy is always used for the field type.
  unsigned char hex_bytes; // 0 = non-hex
  unsigned char int_bytes; // 0 = non-integer, 0xff = variable length encoding
  unsigned char is_enum; // if non-zero, then encode as enum
  char *enum_options;
};

struct manifest_field fields[]={
  // Hex fields
  {0x80,"id",0x20,0,0,NULL},
  {0x81,"bk",0x20,0,0,NULL},
  {0x82,"sender",0x20,0,0,NULL},
  {0x83,"recipient",0x20,0,0,NULL},

  {0x90,"filehash",0x40,0,0,NULL},

  // Numeric fields
  {0xa0,"version",0,0xff,0,NULL},
  {0xa1,"filesize",0,0xff,0,NULL},
  {0xa2,"date",0,0xff,0,NULL},
  {0xa3,"crypt",0,0,1,NULL},

  // enums (CASE SENSITIVE!)
  {0xb0,"service",0,0,1,"file,MeshMS1,MeshMS2"},
  
  {0,NULL,0,0}
};

int field_encode(int field_number,unsigned char *key,unsigned char *value,
		 unsigned char *bin_out,int *out_offset)
{
  if (fields[field_number].hex_bytes) {
  } else if (fields[field_number].int_bytes) {
  } else if (fields[field_number].is_enum) {
  } else {
    // Unknown field type: should never happen
    return -1;
  }
  return -1;
}

int field_decode(int field_number,unsigned char *bin_in,int *in_offset,
		 unsigned char *text_out,int *out_offset)
{
  return -1;
}

/*
  Decode binary format manifest.
  This really consists of looking for tokens and expanding them.
  The only complications are that we need to stop looking for
  tokens once we hit the signature block, and that we should not look
  for tokens except at the start of lines, so that values can contain
  UTF-8 encoding.
 */
int manifest_binary_to_text(unsigned char *bin_in, int len_in,
			    unsigned char *text_out, int *len_out)
{
  int offset;
  int out_offset=0;
  int start_of_line=1;
  for(offset=0;offset<len_in;offset++) {
    if (!bin_in[offset]) {
    } else {
      if (start_of_line&&(bin_in[offset]&0x80)) {
	// It's a token
	int field;
	for(field=0;fields[field].token;field++) {
	  if (bin_in[offset]==fields[field].token) break;
	}
	// Fail decode if we hit an unknown token
	if (!fields[field].token) return -1;
	// Also fail if we cannot decode a token
	if (field_decode(field,bin_in,&offset,text_out,&out_offset)) return -1;
      } else if (bin_in[offset]=='\n') {
	// new line, so remember it is the start of a line
	start_of_line=1;
	text_out[out_offset++]=bin_in[offset];
      } else {
	// not a new line, so clear start of line flag, so that
	// we don't try to interpret UTF-8 value strings as tokens.
	start_of_line=0;
	text_out[out_offset++]=bin_in[offset];
      }
    }
  }

  return 0;
}

// Produce a more compact manifest representation
// XXX - doesn't currently compress free-text fields
int manifest_text_to_binary(unsigned char *text_in, int len_in,
			    unsigned char *bin_out, int *len_out)
{
  // Manifests must be <1KB
  if (len_in>1024) return -1;

  int out_offset=0;
  int offset;
  for(offset=0;offset<len_in;offset++) {
    // See if we are at a line of KEY=VALUE format.
    unsigned char key[1024], value[1024];
    int length;
    if (sscanf((const char *)&text_in[offset],"%[^=]=%[^\n]%n",
	       key,value,&length)==2) {
      // We think we have a field
      printf("line: [%s]=[%s]\n",key,value);
      // See if we know about this field to binary encode it:
      int f=0;
      for(f=0;fields[f].token;f++)
	if (!strcasecmp((char *)key,fields[f].name)) {
	  // It is this field
	  break;
	}
      if ((!fields[f].token)
	  ||(field_encode(f,key,value,bin_out,&out_offset)))
	{
	  // Could not encode the field compactly, so just copy it out.
	  int count=sprintf((char *)&bin_out[out_offset],"%s=%s\n",(char *)key,(char *)value);
	  out_offset+=count;
	}
      // Skip remainder of the line
      // (the for loop will add one, which skips the \n character)
      offset+=length;
    } else {
      // Is not a key value pair: just copy the character ...
      // ... unless it is 0x00, in which case it marks the start of the
      // binary section, which we should just copy verbatim, and then
      // break out of the loop.
      if (text_in[offset]==0x00) {
	int count=len_in-offset;
	bcopy(&text_in[offset],&bin_out[out_offset],count);
	break;
      } else {
	bin_out[out_offset++]=text_in[offset];
      }
    }
  } 

  printf("Text input length = %d, binary version length = %d\n",
	 len_in,out_offset);

  // Now verify that we can decompress it losslessly (otherwise signatures will
  // fail)
  unsigned char verify_out[1024];
  int verify_length=0;
  manifest_binary_to_text(bin_out,out_offset,verify_out,&verify_length);
  if ((verify_length!=len_in)
      ||bcmp(text_in,verify_out,len_in)) {
    printf("Verify error with binary manifest: reverting to plain text.\n");
    bcopy(text_in,bin_out,len_in);
    *len_out=len_in;
    return -1;
  } else {
    // Compression was successful
    *len_out=out_offset;
    return 0;
  }

}

