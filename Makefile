BINDIR=.
EXECS = $(BINDIR)/lbard $(BINDIR)/manifesttest $(BINDIR)/fakecsmaradio

all:	$(EXECS)

clean:
	rm -rf version.h $(EXECS) echotest

SRCDIR=src
INCLUDEDIR=include

SRCS=	$(SRCDIR)/main.c \
	$(SRCDIR)/rhizome.c \
	$(SRCDIR)/txmessages.c \
	$(SRCDIR)/rxmessages.c \
	$(SRCDIR)/bundle_cache.c \
	$(SRCDIR)/json.c \
	$(SRCDIR)/peers.c \
	\
	$(SRCDIR)/serial.c \
	$(SRCDIR)/radio.c \
	$(SRCDIR)/golay.c \
	$(SRCDIR)/httpclient.c \
	$(SRCDIR)/progress.c \
	$(SRCDIR)/rank.c \
	$(SRCDIR)/bundles.c \
	$(SRCDIR)/partials.c \
	\
	$(SRCDIR)/manifests.c \
	$(SRCDIR)/monitor.c \
	$(SRCDIR)/timesync.c \
	$(SRCDIR)/httpd.c \
	$(SRCDIR)/meshms.c \
	\
	$(SRCDIR)/energy_experiment.c \
	$(SRCDIR)/status_dump.c \
	\
	$(SRCDIR)/fec-3.0.1/ccsds_tables.c \
	$(SRCDIR)/fec-3.0.1/encode_rs_8.c \
	$(SRCDIR)/fec-3.0.1/init_rs_char.c \
	$(SRCDIR)/fec-3.0.1/decode_rs_8.c \
	\
	$(SRCDIR)/bundle_tree.c \
	$(SRCDIR)/sha1.c \
	$(SRCDIR)/sync.c \
	\
	$(SRCDIR)/eeprom.c \
	$(SRCDIR)/sha3.c \
	$(SRCDIR)/otaupdate.c \
	\
	$(SRCDIR)/drivers/hfcontroller.c \
	$(SRCDIR)/drivers/uhfcontroller.c \

HDRS=	$(INCLUDEDIR)/lbard.h \
	$(INCLUDEDIR)/serial.h \
	Makefile \
	$(INCLUDEDIR)/sync.h \
	$(INCLUDEDIR)/sha3.h \
	$(SRCDIR)/miniz.c

#CC=/usr/local/Cellar/llvm/3.6.2/bin/clang
#LDFLAGS= -lgmalloc
#CFLAGS= -fno-omit-frame-pointer -fsanitize=address
#CC=clang
#LDFLAGS= -lefence
LDFLAGS=
# -I$(SRCDIR) is required for fec-3.0.1
CFLAGS= -g -std=gnu99 -Wall -fno-omit-frame-pointer -D_GNU_SOURCE=1 -I$(INCLUDEDIR) -I$(SRCDIR)

$(INCLUDEDIR)/version.h:	$(SRCS) $(HDRS)
	echo "#define VERSION_STRING \""`./md5 $(SRCS)`"\"" >$(INCLUDEDIR)/version.h
	echo "#define GIT_VERSION_STRING \""`git describe --always --abbrev=10 --dirty=+DIRTY`"\"" >>$(INCLUDEDIR)/version.h
	echo "#define GIT_BRANCH \""`git rev-parse --abbrev-ref HEAD`"\"" >>$(INCLUDEDIR)/version.h
	echo "#define BUILD_DATE \""`date`"\"" >>$(INCLUDEDIR)/version.h

lbard:	$(SRCS) $(HDRS) $(INCLUDEDIR)/version.h
	$(CC) $(CFLAGS) -o lbard $(SRCS) $(LDFLAGS)

echotest:	Makefile echotest.c
	$(CC) $(CFLAGS) -o echotest echotest.c

FAKERADIOSRCS=	$(SRCDIR)/fakeradio/fakecsmaradio.c \
		$(SRCDIR)/drivers/fake_rfd900.c \
		$(SRCDIR)/drivers/fake_hfcodan.c \
		\
		$(SRCDIR)/fec-3.0.1/ccsds_tables.c \
		$(SRCDIR)/fec-3.0.1/encode_rs_8.c \
		$(SRCDIR)/fec-3.0.1/init_rs_char.c \
		$(SRCDIR)/fec-3.0.1/decode_rs_8.c
fakecsmaradio:	\
	Makefile $(FAKERADIOSRCS) $(INCLUDEDIR)/fakecsmaradio.h
	$(CC) $(CFLAGS) -o fakecsmaradio $(FAKERADIOSRCS)

$(BINDIR)/manifesttest:	Makefile $(SRCDIR)/manifests.c
	$(CC) $(CFLAGS) -DTEST -o $(BINDIR)/manifesttest $(SRCDIR)/manifests.c
