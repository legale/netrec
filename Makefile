CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS += -D_GNU_SOURCE -DYAML_VERSION_MAJOR=0 -DYAML_VERSION_MINOR=2 -DYAML_VERSION_PATCH=5 -DYAML_VERSION_STRING=\"0.2.5\" -Ideps/libyaml/include -Ideps/libyaml/src
WARN := -Wall -Wextra

APP := netrec
APP_OBJS := main.o yaml.o state.o netlink.o verify.o
YAML_SRCS := \
	deps/libyaml/src/api.c \
	deps/libyaml/src/reader.c \
	deps/libyaml/src/scanner.c \
	deps/libyaml/src/parser.c \
	deps/libyaml/src/loader.c \
	deps/libyaml/src/writer.c \
	deps/libyaml/src/emitter.c \
	deps/libyaml/src/dumper.c
YAML_OBJS := $(YAML_SRCS:.c=.o)

all: $(APP)

$(APP): $(APP_OBJS) $(YAML_OBJS)
	$(CC) $(CFLAGS) -o $@ $(APP_OBJS) $(YAML_OBJS)

deps/libyaml/src/%.o: deps/libyaml/src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -c -o $@ $<

clean:
	rm -f $(APP) $(APP_OBJS) $(YAML_OBJS)

.PHONY: all clean
