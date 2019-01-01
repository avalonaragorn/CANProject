#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include "can_utils.h"

int can_init(char const *can_itf)
{
	struct sockaddr_can addr;
	struct ifreq ifr;

	/* open socket */
	if ((canSocket = socket(PF_CAN, SOCK_RAW, CAN_RAW)) < 0) {
		perror("socket");
		return 1;
	}

	strncpy(ifr.ifr_name, can_itf, IFNAMSIZ - 1);
	ifr.ifr_name[IFNAMSIZ - 1] = '\0';
	ifr.ifr_ifindex = if_nametoindex(ifr.ifr_name);
	if (!ifr.ifr_ifindex) {
		perror("if_nametoindex");
		return 1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.can_family = AF_CAN;
	addr.can_ifindex = ifr.ifr_ifindex;

	// zhj: set filter rule, this line means, doesn't recevie anything
	// setsockopt(canSocket, SOL_CAN_RAW, CAN_RAW_FILTER, NULL, 0);

	if (bind(canSocket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		return 1;
	}

	return 0;
}

void can_deinit()
{
	close(canSocket);
}

int can_send(unsigned int can_id, unsigned char* payload, unsigned char len)
{
	int frame_size;
	struct canfd_frame frame;

	frame.can_id = can_id;
	frame.len = len;
	memcpy(frame.data, payload, len);
	
	frame_size = sizeof(struct canfd_frame) - sizeof(frame.data) + frame.len;

	printf("Send CAN Msg:\t%03X    [%d]    %02X %02X %02X %02X %02X %02X %02X %02X",
          frame.can_id,
          frame.len,
          frame.data[0],
          frame.data[1],
          frame.data[2],
          frame.data[3],
          frame.data[4],
          frame.data[5],
          frame.data[6],
          frame.data[7]);

	//zhj: send can frame
	if (write(canSocket, &frame, frame_size) != frame_size)
	{
		printf("\t\tFail\n");
		return -1;
	}

	printf("\t\tSucc\n");

	// zhj: for test, step by step
	// while (1)
	// {
	// 	if (getchar())
	// 		break;
	// 	sleep(1);
	// }

	usleep(CAN_FRAME_TIME_INTERVAL); // zhj: can't too fast

	return 0;
}
