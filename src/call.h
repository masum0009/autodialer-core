// call.h
#ifndef _CALL_H
#define _CALL_H

#define AUTODIALER_VERSION "0.1.0"

enum
{
		CALL_CONNECTING = 0,
		CALL_TRYING = 1,
		CALL_RINGING = 2,
		CALL_ESTABLISHED = 3,
		CALL_DISCONNECTED = 4,	
};
//typedef void call_end_h(void *arg);

struct gateway{
  uint32_t gateway_id; 
  char *uri_str;
  struct uri sip_uri;   
  struct le le;
};

struct campaign{
  uint32_t campaign_id;
  uint32_t client_id;
  uint8_t campaign_status;
  struct gateway *gateway;
  struct gateway *pbx_gateway;
  char *pbx_number;
  char *caller_id;
  char *file_name;
  time_t stat_time;
  uint16_t call_limit;
  uint16_t retry_delay;
  uint16_t max_call_duration;
  uint8_t max_try;
  bool is_running;
  uint8_t max_play;
  struct list destinations;
  struct list processl;
  struct tmr tmr;
  struct list  actions;
  struct le le;
};

struct campaign_action{
   uint8_t action_key;
   uint8_t action_type;
   struct gateway *gateway; 
   char *dest;
   struct le le;
};

struct destination{
  int dest_id;
  char *dest;
  time_t last_try;  
  struct call *call; 
  struct campaign *cp; 
  uint8_t try_count;
  struct tmr tmr;
  struct le le;
};

struct call{
  uint16_t resp_code; 
  struct sipsess *sess; 
  struct rtp_sock *rs;
  struct rtp_session *rtp_sess;
  struct sdp_session *sdp; 
  struct sdp_media *sdp_media;
  struct call *pbx_call;
  char *audio;
  struct sa rtp_dest;
  struct destination *dest;
  uint32_t ts;
  time_t connect_tm;
  time_t establish_tm;
  struct tmr tmr;
  thrd_t t;
  uint8_t call_status;
  uint8_t num_play;
  uint8_t action_key;
  uint8_t action_type;
  uint8_t pt;
  struct campaign_action *action;
  bool join_pbx;
  bool thrd_created;
  bool ended;
  bool ispbxcall;
  bool event_received;
  bool forwarded;
  void *arg;
};



/*struct call_channel{
  struct sa remote_ip;
  time_t expires;
  struct tmr tmr;
  struct le le;
};*/


struct rtp_session{
  struct udp_sock *rs;
  struct sa local_ip;
  struct sa remote_ip;
  bool connected;
  bool in_use;
  bool local;
  struct rtp_session *remote;  
  struct lock *lock;   
  bool telev_recv;
  struct telev *telev;
  void *arg;
  struct le le;
};


int sipcall_alloc(const struct sa *sip_ip);
void close_sip(void);
int call_alloc(struct call **callp,const char *tel,const struct uri *gateway,bool isPbx,void *arg);
void rtp_session_allocate(const struct sa *addr);
void rtp_session_destroy();
void check_sdp_null(void);

#endif /* call.h */