all:	lbard

SRCS=	main.c rhizome.c txmessages.c rxmessages.c bundle_cache.c json.c peers.c \
	serial.c radio.c golay.c httpclient.c progress.c \
	fec-3.0.1/ccsds_tables.c \
	fec-3.0.1/encode_rs_8.c \
	fec-3.0.1/init_rs_char.c \
	fec-3.0.1/decode_rs_8.c \

HDRS=	lbard.h serial.h Makefile
#CC=/usr/local/Cellar/llvm/3.6.2/bin/clang
#LDFLAGS= -lgmalloc
#CFLAGS= -fno-omit-frame-pointer -fsanitize=address
CC=clang
LDFLAGS= -lefence
CFLAGS= -fno-omit-frame-pointer

lbard:	$(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -g -std=gnu99 -Wall -o lbard $(SRCS) $(LDFLAGS)

echotest:	Makefile echotest.c
	$(CC) -g -std=gnu99 -Wall -o echotest echotest.c
