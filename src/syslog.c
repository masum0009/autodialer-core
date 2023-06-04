#include <syslog.h>
#include <re.h>
#include "syslog.h"


static const int lmap[] = { LOG_DEBUG, LOG_INFO, LOG_WARNING, LOG_ERR };


void init_log(int level){
   uint32_t facility = LOG_DAEMON;
 //  setlogmask (LOG_UPTO (LOG_NOTICE)); 
   openlog("easyvpn", LOG_NDELAY | LOG_PID, facility);
}

void write_log(int level,const char *fmt, ...){
  va_list ap;
	int err;
  char buffer[1024];
  va_start(ap, fmt);
	err = re_vsnprintf(buffer,1024, fmt, ap);
	//re_printf(buffer);
	syslog(lmap[MIN(level, ARRAY_SIZE(lmap)-1)], "%s", buffer);
}

void close_log(void){
   closelog();
}


