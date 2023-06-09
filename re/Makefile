#
# Makefile
#
# Copyright (C) 2010 Creytiv.com
#

# Master version number
VER_MAJOR := 0
VER_MINOR := 4
VER_PATCH := 1

PROJECT   := re
VERSION   := 0.4.1

MK	:= mk/re.mk

include $(MK)

# List of modules
MODULES += sip sipevent sipreg sipsess
MODULES += uri httpauth
MODULES += stun turn ice
MODULES += natbd
MODULES += rtp sdp jbuf telev
MODULES += dns
MODULES += md5 crc32 sha hmac base64
MODULES += udp sa net tcp tls
MODULES += list mbuf hash
MODULES += fmt tmr main mem dbg sys lock mqueue
MODULES += mod conf
MODULES += bfcp aes srtp http msg

INSTALL := install
ifeq ($(DESTDIR),)
PREFIX  := /usr/local
else
PREFIX  := /usr
endif
ifeq ($(LIBDIR),)
LIBDIR  := $(PREFIX)/lib
endif
INCDIR  := $(PREFIX)/include/re
MKDIR   := $(PREFIX)/share/re
CFLAGS	+= -Iinclude

MODMKS	:= $(patsubst %,src/%/mod.mk,$(MODULES))
SHARED  := libre$(LIB_SUFFIX)
STATIC	:= libre.a

include $(MODMKS)


OBJS	?= $(patsubst %.c,$(BUILD)/%.o,$(SRCS))


all: $(SHARED) $(STATIC)


-include $(OBJS:.o=.d)


$(SHARED): $(OBJS)
	@echo "  LD      $@"
	@$(LD) $(LFLAGS) $(SH_LFLAGS) $^ $(LIBS) -o $@


$(STATIC): $(OBJS)
	@echo "  AR      $@"
	@$(AR) $(AFLAGS) $@ $^
ifneq ($(RANLIB),)
	@$(RANLIB) $@
endif

$(BUILD)/%.o: src/%.c $(BUILD) Makefile $(MK) $(MODMKS)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) -c $< -o $@ $(DFLAGS)


$(BUILD): Makefile $(MK) $(MODMKS)
	@mkdir -p $(patsubst %,$(BUILD)/%,$(sort $(dir $(SRCS))))
	@touch $@


.PHONY: clean
clean:
	@rm -rf $(SHARED) $(STATIC) test.d test.o test $(BUILD)/


install: $(SHARED) $(STATIC)
	@mkdir -p $(DESTDIR)$(LIBDIR) $(DESTDIR)$(INCDIR) $(DESTDIR)$(MKDIR)
	$(INSTALL) -m 0644 $(shell find include -name "*.h") \
		$(DESTDIR)$(INCDIR)
	$(INSTALL) -m 0755 $(SHARED) $(DESTDIR)$(LIBDIR)
	$(INSTALL) -m 0755 $(STATIC) $(DESTDIR)$(LIBDIR)
	$(INSTALL) -m 0644 $(MK) $(DESTDIR)$(MKDIR)

uninstall:
	@rm -rf $(DESTDIR)$(INCDIR)
	@rm -rf $(DESTDIR)$(MKDIR)
	@rm -f $(DESTDIR)$(LIBDIR)/$(SHARED)
	@rm -f $(DESTDIR)$(LIBDIR)/$(STATIC)

-include test.d

test.o:	test.c Makefile $(MK)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) -c $< -o $@ $(DFLAGS)

test$(BIN_SUFFIX): test.o $(SHARED) $(STATIC)
	@echo "  LD      $@"
	@$(LD) $(LFLAGS) $< -L. -lre $(LIBS) -o $@

sym:	$(SHARED)
	@nm $(SHARED) | grep " U " | perl -pe 's/\s*U\s+(.*)/$${1}/' \
		> docs/symbols.txt
	@echo "$(SHARED) is using `cat docs/symbols.txt | wc -l ` symbols"
