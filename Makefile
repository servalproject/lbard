all:	lbard

SRCS=	main.c rhizome.c txmessages.c rxmessages.c bundle_cache.c json.c peers.c \
	serial.c \
	fec-3.0.1/ccsds_tables.c \
	fec-3.0.1/encode_rs_8.c \
	fec-3.0.1/init_rs_char.c \
	fec-3.0.1/decode_rs_8.c \

HRdS=	lbard.h serial.h Makefile


lbard:	$(SRCS) $(HRDS)
	cc -g -Wall -o lbard $(SRCS) -lcurl

