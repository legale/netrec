CC = musl-gcc
CFLAGS ?= -Wall -Wextra -Werror -Os -fno-common -ffunction-sections -fdata-sections -D_FORTIFY_SOURCE=2 -fno-omit-frame-pointer -fstack-protector-strong -fPIC

CPPFLAGS += -D_GNU_SOURCE
CPPFLAGS += -I.
CPPFLAGS += -I..
CPPFLAGS += -idirafter /usr/include
CPPFLAGS += -idirafter /usr/include/$(shell $(CC) -dumpmachine 2>/dev/null)
WARN := -Wall -Wextra

MODULES = ../nlmon ../syslog2
APP_LIBS = ../nlmon/libnlmon.a ../syslog2/libsyslog2.a
CFG_TEST_LIBS = ../syslog2/libsyslog2.a

APP := netrec
APP_STATIC := $(APP)-static
APP_OBJS := main.o run.o watch.o yaml.o cfg.o uci.o state.o netlink.o verify.o
CFG_TEST := cfg_path_check
CFG_TEST_OBJS := tests/cfg_path_check.o cfg.o
LDLIBS += -lc -lpthread

.PHONY: all clean check static deps

all: $(APP)

static: $(APP_STATIC)

check: $(APP) $(CFG_TEST)
	./$(CFG_TEST)
	tests/smoke_check.sh
	tests/netns_check.sh

deps: $(APP_LIBS)

$(MODULES):
	$(MAKE) -C $@

$(APP_LIBS):
	$(MAKE) -B -C $(dir $@) $(notdir $@)

$(CFG_TEST): $(CFG_TEST_OBJS) $(CFG_TEST_LIBS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(CFG_TEST_OBJS) $(CFG_TEST_LIBS) $(LDLIBS)

$(APP): deps $(APP_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(APP_OBJS) $(APP_LIBS) $(LDLIBS)

$(APP_STATIC): deps $(APP_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -static -o $@ $(APP_OBJS) $(APP_LIBS) $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -c -o $@ $<

clean:
	rm -f $(APP) $(APP_STATIC) $(APP_OBJS) $(CFG_TEST) $(CFG_TEST_OBJS)
	$(foreach mod,$(MODULES),$(MAKE) -C $(mod) clean;)
