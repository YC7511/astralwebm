CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?=

APP = astralwebm
SRC = astralwebm.c

FCGI_CFLAGS := $(shell pkg-config --cflags fcgi 2>/dev/null)
FCGI_LIBS := $(shell pkg-config --libs fcgi 2>/dev/null)
FCGI_AVAILABLE := $(shell printf '#include <fcgiapp.h>\n' | $(CC) $(FCGI_CFLAGS) -E - >/dev/null 2>&1 && echo 1 || echo 0)

ifeq ($(FCGI_AVAILABLE),1)
ifeq ($(strip $(FCGI_LIBS)),)
FCGI_LIBS := -lfcgi
endif
else
FCGI_CFLAGS += -DASTRALWEBM_NO_FCGI
FCGI_LIBS :=
endif

.PHONY: all clean

all: $(APP)

$(APP): $(SRC)
	$(CC) $(CFLAGS) $(FCGI_CFLAGS) -o $@ $^ $(LDFLAGS) $(FCGI_LIBS) -lpthread

clean:
	rm -f $(APP)
