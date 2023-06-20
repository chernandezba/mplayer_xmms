CC= gcc
LD=ld
CFLAGS=-Wall -ggdb
LDFLAGS=-shared -lc 

INCGLIB=`glib-config glib --cflags`
LIBGLIB=`glib-config glib --libs`

INCGTK=`gtk-config gtk --cflags`
LIBGTK=`gtk-config gtk --libs`



PROGRAMS=libmplayer_xmms.so

all: $(PROGRAMS)


libmplayer_xmms.so: libmplayer_xmms.c
	$(CC) $(INCGLIB) $(INCGTK) $(CFLAGS) -o libmplayer_xmms.so $(LDFLAGS) $(LIBGLIB) $(LIBGTK) libmplayer_xmms.c apprentice.o  apptype.o  ascmagic.o  compress.o  fsmagic.o  funcs.o  is_tar.o  magic.o  print.o  readelf.o  softmagic.o


install: libmplayer_xmms.so
	cp -f $(PROGRAMS) /usr/lib/xmms/Input

clean:
	rm -f $(PROGRAMS)
	rm -f core
	rm -f *.so
