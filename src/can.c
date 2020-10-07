#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <unistd.h>

/* Send can frame (-1: error, 1: Success) */
int can_send(const char *dev_name, struct can_frame *f)
{
	int s;
	int nbytes;
	struct sockaddr_can addr;
	struct can_frame frame;
	struct ifreq ifr;

	/* open socket */
	if((s = socket(PF_CAN, SOCK_RAW, CAN_RAW)) < 0) {
		perror("Error while opening socket");
		return -1;
	}

	strcpy(ifr.ifr_name, dev_name);
	ioctl(s, SIOCGIFINDEX, &ifr);

	addr.can_family = AF_CAN;
	addr.can_ifindex = ifr.ifr_ifindex;

	if(bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("Error in socket bind");
		return -1;
	}

	frame = *f;

	if(write(s, &frame, sizeof(struct can_frame)) == -1) {
		perror("Error in write");
		return -1;
	}

	if(close(s) == -1){
		perror("Error in close socket");
		return -1;
	}

	return 1;
}



