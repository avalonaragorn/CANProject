#ifndef can_utils_h
#define can_utils_h

#include <linux/can.h>
#include <linux/can/raw.h>

#include "lib.h"

#define true 1
#define false 0

#define CAN_FRAME_TIME_INTERVAL 2000

int canSocket;

int can_init(char const *can_itf);
void can_deinit();
int can_send(unsigned int can_id, unsigned char* payload, unsigned char len);

#endif
