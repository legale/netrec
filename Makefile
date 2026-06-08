CC = musl-gcc
CFLAGS ?= -Wall -Wextra -Werror -Os -fno-common -ffunction-sections -fdata-sections -D_FORTIFY_SOURCE=2 -fno-omit-frame-pointer -fstack-protector-strong -fPIC

CPPFLAGS += -D_GNU_SOURCE
CPPFLAGS += -I.
CPPFLAGS += -idirafter /usr/include
CPPFLAGS += -idirafter /usr/include/$(shell $(CC) -dumpmachine 2>/dev/null)
WARN := -Wall -Wextra

APP := netrec
APP_STATIC := $(APP)-static
APP_OBJS := main.o yaml.o cfg.o uci.o state.o netlink.o verify.o
CFG_TEST := cfg_path_check
CFG_TEST_OBJS := tests/cfg_path_check.o cfg.o

all: $(APP)

static: $(APP_STATIC)

check: $(APP) $(CFG_TEST)
	./$(CFG_TEST)
	tests/smoke_check.sh
	tests/netns_check.sh

$(CFG_TEST): $(CFG_TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $(CFG_TEST_OBJS)

$(APP): $(APP_OBJS)
	$(CC) $(CFLAGS) -o $@ $(APP_OBJS)

$(APP_STATIC): $(APP_OBJS)
	$(CC) $(CFLAGS) -static -o $@ $(APP_OBJS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -c -o $@ $<

clean:
	rm -f $(APP) $(APP_STATIC) $(APP_OBJS) $(CFG_TEST) $(CFG_TEST_OBJS)

.PHONY: all clean check static
