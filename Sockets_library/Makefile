CC=g++
CXX=g++
RANLIB=ranlib

CONTSRC = container.cpp sockets.cpp

INCS=-I.
CFLAGS = -Wall -std=c++11 -g $(INCS)
CXXFLAGS = -Wall -std=c++11 -g $(INCS)

TAR=tar
TARFLAGS=-cvf
TARNAME=ex5.tar
TARSRCS=$(LIBSRC) Makefile README

all: $(CONTSRC)
	$(CC) $(CFLAGS) sockets.cpp -o sockets

tar:
	$(TAR) $(TARFLAGS) $(TARNAME) $(CONTSRC)

#$(TARGETS): $(LIBOBJ)
#	$(AR) $(ARFLAGS) $@ $^
#	$(RANLIB) $@
#
#clean:
#	$(RM) $(TARGETS) $(OSMLIB) $(OBJ) $(LIBOBJ) *~ *core
#
#depend:
#	makedepend -- $(CFLAGS) -- $(SRC) $(LIBSRC)
#

