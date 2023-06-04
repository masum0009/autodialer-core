#ifndef _IN_CALL_H
#define _IN_CALL_H



int incoming_call_alloc(struct sip *sip,struct sipsess_sock *sess_sock,struct sip_msg *msg);
void init_incoming(const struct sa *sa);
void destroy_incoming(void);




#endif /* in_call.h */