EXE=canproxy

STUB_EXE=rmq_sender

CC=gcc

RABBITMQ_C_DIR=../../rabbitmq-c

INCPATH=$(RABBITMQ_C_DIR)/librabbitmq

LIBPATH=$(RABBITMQ_C_DIR)/build/librabbitmq

CFLAGS=-g -Wall -lrabbitmq -I. -I$(INCPATH) -L$(LIBPATH) -Wl,-rpath=$(LIBPATH)

SRC=rmq_listener.c \
     utils.c \
     unix/platform_utils.c


STUB_SRC=rmq_sender.c \
     utils.c \
     unix/platform_utils.c


all:$(EXE) $(STUB_EXE)

$(EXE):
	$(CC) $(SRC) $(CFLAGS) -o $(EXE)

$(STUB_EXE):
	$(CC) $(STUB_SRC) $(CFLAGS) -o $(STUB_EXE)

clean:
	@rm $(EXE) $(STUB_EXE) -f
