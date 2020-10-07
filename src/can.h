#ifdef CAN_H
#define CAN_H

int can_send(const char *dev_name, struct can_frame *f);

#endif
