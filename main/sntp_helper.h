#ifndef sntp_helper_h
#define sntp_helper_h

#include <sys/time.h>

void obtain_time(void);
void initialize_sntp(void);
void time_sync_notification_cb(struct timeval *tv);

#endif
