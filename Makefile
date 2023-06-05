#
# Makefile
#
# Copyright (C) 2023 infosoftbd.com
#

PROJECT	  := autodialer
VERSION   := 2.0.0

LIBRE_MK  := $(shell [ -f ./re/mk/re.mk ] && \
	echo "./re/mk/re.mk")

include $(LIBRE_MK)

INSTALL := install
ifeq ($(DESTDIR),)
PREFIX  := /usr/local
else
PREFIX  := /usr
endif
BINDIR	:= $(PREFIX)/bin
CFLAGS	+= -I$(LIBRE_INC) -Iinclude `mysql_config --cflags`
BIN	:= $(PROJECT)$(BIN_SUFFIX)
APP_MK	:= src/srcs.mk


LIBS    += `mysql_config  --libs` -lpthread -lrt -lre


include $(APP_MK)

OBJS	?= $(patsubst %.c,$(BUILD)/src/%.o,$(SRCS))

all: $(BIN)

-include $(OBJS:.o=.d)

$(BIN): $(OBJS)
	@echo "  LD      $@"
	@$(LD) $(LFLAGS) $^ -L$(LIBRE_SO) -lre $(LIBS) -o $@

$(BUILD)/%.o: %.c $(BUILD) Makefile $(APP_MK)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) -o $@ -c $< $(DFLAGS)

$(BUILD): Makefile
	@mkdir -p $(BUILD)/src
	@touch $@

clean:
	@rm -rf $(BIN) $(BUILD)

install: $(BIN)
	@mkdir -p $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 0755 $(BIN) $(DESTDIR)$(BINDIR)
