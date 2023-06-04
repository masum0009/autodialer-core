#ifndef _SYSLOG_H
#define _SYSLOG_H


void init_log(int level);
void write_log(int level,const char *fmt, ...);
void close_log(void);



#endif /* syslog.h */

