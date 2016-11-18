AM_CFLAGS = -D_GNU_SOURCE -DCHECK_AIS -DSBINDIR=\"$(sbindir)\"
AM_CPPFLAGS =  -I$(includedir)/pacemaker  \
	       -I$(includedir)/heartbeat  \
		$(glib_CFLAGS)

sbin_PROGRAMS = sbd

sbd_SOURCES = sbd-common.c sbd-inquisitor.c sbd-pacemaker.c sbd-cluster.c setproctitle.c

if SUPPORT_SHARED_DISK
sbd_SOURCES += sbd-md.c
endif

sbd_LDFLAGS = $(glib_LIBS) $(libcoroipcc_LIBS)

