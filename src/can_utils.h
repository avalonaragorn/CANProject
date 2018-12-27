#ifndef can_utils_h
#define can_utils_h

#include <linux/can.h>
#include <linux/can/raw.h>

#include "lib.h"

int canSocket;

int can_init(char* can_itf);
void can_deinit();
int can_send(unsigned int can_id, unsigned char* payload, unsigned char len);

#endif
