all:	lbard

SRCS=	main.c rhizome.c txmessages.c rxmessages.c bundle_cache.c json.c peers.c \
	serial.c
HRdS=	lbard.h Makefile

lbard:	$(SRCS) $(HRDS)
	cc -g -Wall -o lbard $(SRCS) -lcurl

