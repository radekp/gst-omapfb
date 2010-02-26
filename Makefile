CROSS_COMPILE ?= arm-linux-
CC := $(CROSS_COMPILE)gcc

CFLAGS := -O2 -ggdb -Wall -Wextra -Wno-unused-parameter -ansi -std=c99
LDFLAGS := -Wl,--no-undefined

GST_CFLAGS := $(shell pkg-config --cflags gstreamer-0.10 gstreamer-base-0.10)
GST_LIBS := $(shell pkg-config --libs gstreamer-0.10 gstreamer-base-0.10)

KERNEL := /data/public/dev/omap/linux-omap

prefix := /usr

all:

libgstomapfb.so: omapfb.o
libgstomapfb.so: override CFLAGS += $(GST_CFLAGS) \
	-I$(KERNEL)/arch/arm/plat-omap/include
libgstomapfb.so: override LIBS += $(GST_LIBS)

targets += libgstomapfb.so

all: $(targets)

# pretty print
ifndef V
QUIET_CC    = @echo '   CC         '$@;
QUIET_LINK  = @echo '   LINK       '$@;
QUIET_CLEAN = @echo '   CLEAN      '$@;
endif

D = $(DESTDIR)

%.o:: %.c
	$(QUIET_CC)$(CC) $(CFLAGS) -MMD -o $@ -c $<

%.so::
	$(QUIET_CC)$(CC) $(LDFLAGS) -shared -o $@ $^ $(LIBS)

install: $(targets)
	install -D libgstomapfb.so $(D)/$(prefix)/lib/gstreamer-0.10/libgstomapfb.so

clean:
	$(QUIET_CLEAN)$(RM) $(targets) *.o *.d

-include *.d
