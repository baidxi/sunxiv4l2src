
ifeq ($(CC),)
CC=$(CROSS_COMPILE)gcc
endif
ifeq ($(LD),)
LD=$(CROSS_COMPILE)ld
endif
TARGET:=libgstsunxiv4l2src.so
LDFLAGS+=$(shell pkg-config --libs gstreamer-base-1.0 gstreamer-1.0 gstreamer-video-1.0)
CFLAGS+=$(shell pkg-config --cflags gstreamer-base-1.0 gstreamer-1.0 gstreamer-video-1.0)
CFLAGS+=-fPIC
LDFLAGS+=-shared
SRC:=gstsunxiv4l2.c 
SRC+=gstsunxiv4l2src.c
SRC+=gstsunxiv4l2allocator.c

OBJ:=$(SRC:%.c=%.o)

$(TARGET):$(OBJ)
	$(LD) $(LDFLAGS) $^ -o $@

*.o:$(SRC)
	$(CC) -c $< $(CFLAGS) -o $@

install:$(TARGET)
	install $< /usr/lib/x86_64-linux-gnu/gstreamer-1.0

.PHONY: clean

clean:
	@rm *.so -rf
	@rm *.o -rf
	
