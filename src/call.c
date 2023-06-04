// call.c
#include <re.h>
#include "tinycthread.h"
#include "call.h"


static struct sipsess_sock *sess_sock;  /* SIP session socket */
static struct sip *sip;                 /* SIP stack          */
static struct sa laddr;
static struct udp_sock *us;
static struct dnsc *dnsc = NULL;
struct list rtp_sessions;




extern void update_call_status(void *arg);
extern struct campaign_action  *find_campaign_action(void *arg,int event);

static void rtp_session_destructor(void *data)
{
  struct rtp_session *rc = data;
  mem_deref(rc->rs);
  rc->telev_recv = false;
  rc->telev = mem_deref(rc->telev);      
 
  mem_deref(rc->lock);
  
}

static void release_rtp_session(struct rtp_session *rs){
   lock_write_get(rs->lock);
   rs->in_use = false;
   rs->connected = false;
   rs->arg = NULL;     
   rs->telev_recv = false;
   rs->telev = mem_deref(rs->telev);    
   rs->remote = NULL;  
   
   lock_rel(rs->lock);
   list_append(&rtp_sessions,&rs->le,rs);
   re_printf("released rtp session \n");
}

static void call_destructor(void *data){
  struct call *call = data;
  call->ended = true;
  if(call->thrd_created)
    thrd_join(call->t, NULL);
  
  mem_deref(call->sess);
  mem_deref(call->rs);
  mem_deref(call->audio); 
  mem_deref(call->sdp);
  if(call->rtp_sess) release_rtp_session(call->rtp_sess);
  tmr_cancel(&call->tmr);
  if(call->pbx_call)   call->pbx_call = mem_deref(call->pbx_call);
  re_printf("call destroyed %d \n",call->ispbxcall);
}



static struct rtp_session  *find_rtp_session(){
  struct le *le;
  for(le=rtp_sessions.head;le;le=le->next){
  
     struct rtp_session *rs = list_ledata(le);
     
     if(rs->in_use==false){
       rs->in_use = true;
       list_unlink(&rs->le);
       return rs; 
     }
     
  }
   re_printf("all rtp socket busy at the momemnt \n");
   return NULL; 
}



static void rtp_recv_handler(const struct sa *src, struct mbuf *mb, void *arg)
{
    struct rtp_session *rtp_sess = arg;
    struct call *call = rtp_sess->arg;
    int err;
    //re_printf("recieved rtp from %j \n",src);
   
    if (mbuf_get_left(mb) < RTP_HEADER_SIZE)
       return; 
     
    
      
     if(rtp_sess->telev_recv && !call->event_received){
       uint8_t buf[2];
       uint8_t  pt;
       int event, digit;
	   bool end;
       err = mbuf_read_mem(mb, buf, sizeof(buf));
       if (err)
	     return ;
       pt = (buf[1] >> 0) & 0x7f; 
      // re_printf("pt here %d \n",pt);
       if(pt == 101){
        // re_printf("recieved tele event \n");
         mb->pos = RTP_HEADER_SIZE; 
         if (telev_recv(rtp_sess->telev, mb, &event, &end))
		      return;
         
         //digit = telev_code2digit(event);
	       if (event >= 0 && end){
               re_printf("got telev digit %d %d %d\n",digit,event,end);
               struct campaign_action *action = find_campaign_action(call->arg,event);
               if(action){
                  re_printf("got action %d \n",action->action_type);
                  call->action = action;
                  call->event_received = true;
                  call->action_key = action->action_key;
                  call->action_type = action->action_type;
                  if(action->action_type > 0) call->ended = true;
               } 
               
               return;
           }
		     
 
       }
     
       mb->pos = 0;
     
     }
     
     if(rtp_sess->remote){
        // re_printf("sending rtp from %j to %J \n",src,&rtp_sess->remote->remote_ip);
         udp_send(rtp_sess->remote->rs,&rtp_sess->remote->remote_ip,mb);
     }
     
}


/* called when challenged for credentials */
static int auth_handler(char **user, char **pass, const char *realm, void *arg)
{
	int err = 0;
   const struct uri *gateway = arg;
	(void)realm;
    
//   err |= str_dup(user, "easyvpn");
//   err |= str_dup(pass, "1234");
   err |= pl_strdup(user, &gateway->user);
   err |= pl_strdup(pass, &gateway->password);

	
	return err;
}



static int offer_handler(struct mbuf **mbp, const struct sip_msg *msg,
			 void *arg)
{
/*	const bool got_offer = mbuf_get_left(msg->mb); 
	int err;
	(void)arg;

	if (got_offer) {

		err = sdp_decode(sdp, msg->mb, true);
		if (err) {
			re_fprintf(stderr, "unable to decode SDP offer: %s\n",
				   strerror(err));
			return err;
		}

		re_printf("SDP offer received\n");
		update_media();
	}
	else {
		re_printf("sending SDP offer\n");
	}

	return sdp_encode(mbp, sdp, !got_offer); */
	re_printf("SDP offer received\n");
	return 1;
}

/* called when an SDP answer is received */
static int answer_handler(const struct sip_msg *msg, void *arg)
{
  	return 0;

}




/*
static int rtpThread(void * aArg){
  struct call *call = aArg;  
  struct mbuf *mb;
  
  while(1){
    mb = mbuf_alloc(40 + 12);  
    mb->pos = 0;
    rtp_encode(call->rs,false, 18, (uint32_t)call->ts, mb);
    if(silence)
      mbuf_write_mem(mb,mbuf_buf(silence),40);
    else
     mbuf_write_mem(mb,0,40);
      
    mb->pos = 0;
    udp_send(us,&call->rtp_dest,mb);
    mem_deref(mb);
    call->ts += 320;
    sys_msleep(40);
    if(call->ended) break;
  }
  
  re_printf("ended rtp thread!!\n");
  return 0;
} */



static int rtpThread(void * aArg){
  struct call *call = aArg; 
  struct destination *dest = call->arg;
  struct mbuf *mb;
  FILE *f_audio;
  char path[512];      
  char audio[512];
  static char cwd[512];  
  uint8_t codec = call->pt;
  uint32_t frame_size = 40;
  uint32_t frame_sleep = 40;
  uint32_t frame_ts = 320;
  
  
  struct pl pl = PL_INIT;
  struct pl pt = PL_INIT;
  pl_set_str(&pl,call->audio);
  const char *p = pl_strchr(&pl,'.');
  pt.p = pl.p;
  pt.l =  p - pl.p;
  
  if(codec == 0 || codec == 8){
     frame_size = 160;
     frame_ts = 160;
     frame_sleep = 20;
     if(codec == 0)
       re_snprintf(audio,512,"%r.ulaw",&pt);
     else 
       re_snprintf(audio,512,"%r.alaw",&pt);
  
  }else
    re_snprintf(audio,512,"%r.g729",&pt);
  
  uint8_t  serial[frame_size + 2]; 
  
  
  
  
  //char *audioname = basename(call->audio); //"audio.g729";
  re_printf("calling rtp thread with file %s frame size %d ts %d sleep %d codec %d \n",audio,frame_size,frame_ts,frame_sleep,codec);
  /*if(!udpsock){
    re_printf("udp sock not init \n");
    return 0; 
  } */
  
  /*if(getcwd(cwd, sizeof(cwd)) != NULL)
          sprintf(path,"%s/%s",cwd,audio);
  else       
          sprintf(path,"%s",audio);
  

  re_printf("playing file %s \n",path); */         
  while(1){
    //re_printf("call num play %d max play %d \n",call->num_play,dest->cp->max_play);
    if( access( audio, F_OK ) == -1  || call->num_play >= dest->cp->max_play){
        mb = mbuf_alloc(frame_size + 12);  
        mb->pos = 0;
        rtp_encode(call->rs,false, codec, (uint32_t)call->ts, mb);
       // if(silence)
        //  mbuf_write_mem(mb,mbuf_buf(silence),40);
       // else
         mbuf_write_mem(mb,0,frame_size);
          
        mb->pos = 0;
        udp_send(call->rtp_sess->rs,&call->rtp_dest,mb);
        mem_deref(mb);
        call->ts += frame_ts;
        sys_msleep(frame_sleep);
    }else{
              
       
            
            
        f_audio = fopen(audio, "rb");     
       
        if(f_audio == NULL) {
            re_printf("file pointer is null \n");
            break;
         }   
        
       mb = mbuf_alloc(frame_size + 12);
      
       while( fread(serial, sizeof(uint8_t), frame_size , f_audio) == frame_size)
       {
           mb->pos = 12;
           mbuf_write_mem(mb,serial,frame_size);
           mb->pos = 0;
           rtp_encode(call->rs,false, codec,(uint32_t)call->ts, mb);
           mb->pos = 0;
           //re_printf("sending rtp here to %J len %d \n",&call->rtp_dest,mbuf_get_left(mb));
           udp_send(call->rtp_sess->rs,&call->rtp_dest,mb);
           //udp_send_anon(&call->rtp_dest,mb);
           call->ts += frame_ts;
           if(call->ended) break;
           if(call->event_received) break;
           sys_msleep(frame_sleep);
           
           mbuf_reset(mb);
       }
       
       mem_deref(mb);
       fclose(f_audio);
       call->num_play++;
    }
    
    
    if(call->ended) break;
    if(call->event_received) break;
  }
  
  re_printf("ended rtp thread!!\n");
  return 0;
}


static int sdp_sess_alloc(struct sdp_session **sdpp,struct sdp_media **sdp_mediap,struct rtp_session **rtp_sessp,uint8_t pt){
       struct sdp_session *sdpsess;
       struct sdp_media *sdp_media;
       struct rtp_session *rtpsess;
       int err;
       
       rtpsess = find_rtp_session();
       if(!rtpsess)
         return 1;
      
            	// create SDP session 
    	err = sdp_session_alloc(&sdpsess, &rtpsess->local_ip);
    	if (err) {
    	//	re_fprintf(stderr, "sdp session error: %s\n", strerror(err));
    		goto out;
    	}
    
    	// add audio sdp media, using dummy port: 4242 
    	err = sdp_media_add(&sdp_media, sdpsess, "audio", sa_port(&rtpsess->local_ip), "RTP/AVP");
    	if (err) {
    	//	re_fprintf(stderr, "sdp media error: %s\n", strerror(err));
    		goto out;
    	}
        
        re_printf("payload adding %d \n",pt);
        if(pt == 18){
           // add G.729 sdp media format 
        	err = sdp_format_add(NULL, sdp_media, false, "18", "G729", 8000, 1,
        			     NULL, NULL, false, NULL);
        	if (err) {
        	//	re_fprintf(stderr, "sdp format error: %s\n", strerror(err));
        		goto out;
        	}
        }    	        
        else if(pt == 8){ 
        	// add G.711 PCMA sdp media format 
        	err = sdp_format_add(NULL, sdp_media, false, "8", "PCMA", 8000, 1,
        			     NULL, NULL, false, NULL);
        	if (err) {
        	//	re_fprintf(stderr, "sdp format error: %s\n", strerror(err));
        		goto out;
        	}
        }else{        
            	// add G.711 PCMU sdp media format 
        	err = sdp_format_add(NULL, sdp_media, false, "0", "PCMU", 8000, 1,
        			     NULL, NULL, false, NULL);
        	if (err) {
        	//	re_fprintf(stderr, "sdp format error: %s\n", strerror(err));
        		goto out;
        	}
        }
        
        
        
        
        err = sdp_format_add(NULL, sdp_media, false, "101", "telephone-event", 8000, 1,
                    NULL, NULL, NULL, false, NULL);
        if (err) {
          //  re_fprintf(stderr, "sdp format error: \n");
            goto out;                                               
        }
        
        out:
        // add more codec here 
        if(err){
           mem_deref(sdpsess);
        }else{
           lock_write_get(rtpsess->lock); 
           rtpsess->connected = true;
           lock_rel(rtpsess->lock);
           re_printf("got sdp as %J \n",&rtpsess->local_ip);
           *sdpp = sdpsess;
           *sdp_mediap = sdp_media;
           *rtp_sessp = rtpsess;
            
        }
        
      
        return err;
        
}

/*
static int pbx_call_alloc(struct call *call,struct uri *uri){
   char *remote,*url,*fname;
   struct mbuf *mb;
   struct call *call;
   struct pl pl = PL_INIT;
   int err;
   call->pbx_call = mem_zalloc(sizeof(*pbx_call), pbx_call_destructor);
   if(!call->pbx_call)
      return 1;
   // call->rtpsess = find_rtp_session(); 
      
    pl_strdup(&fname,call->dest->user);
    re_sdprintf(&remote,"sip:%s@%r:%d",tel,&uri->host,uri->port);
    re_sdprintf(&url,"sip:%r@%J",&call->dest->user,&laddr);
   
    re_printf("sending call to %s url %s \n",remote,url);
   
   	
    err = sipsess_connect(&call->pbx_call->sess, sess_sock, remote ,fname ,
			      (const char *) url , fname,
			      NULL, 0, "application/sdp", mb,
			      auth_handler, uri , false,
			      offer_handler, answer_handler,
			      pbx_progress_handler, pbx_establish_handler,
			      NULL, NULL, pbx_close_handler, call, NULL);



     mem_deref(remote);
     mem_deref(url);
     mem_deref(mb);
     mem_deref(fname);  
     
    return err;        
      
}     */


/* called when the session fails to connect or is terminated from peer */
static void close_handler(int err, const struct sip_msg *msg, void *arg)
{
    struct call *call = arg;
    call->ended = true;
    
    if(call->call_status == CALL_ESTABLISHED)
       call->resp_code = 2;
    else if(err == 0 && msg->scode > 0)
        call->resp_code = msg->scode;
    else if(err)
        call->resp_code = 400;   
    
    re_printf("call got disconenct signal \n"); 
 
}



/* called when SIP progress (like 180 Ringing) responses are received */
static void progress_handler(const struct sip_msg *msg, void *arg)
{
  struct call *call = arg;
  call->resp_code = msg->scode;
  if(call->ispbxcall) return;
  update_call_status(call->arg); 

}



/* called when the session is established */
static void establish_handler(const struct sip_msg *msg, void *arg)
{
  struct call *call = arg;
  const struct sdp_format *fmt;
  int err;
  call->resp_code = msg->scode;
  if(call->call_status == CALL_ESTABLISHED){
     return;
  }
  
  call->establish_tm = time(NULL);
  call->call_status = CALL_ESTABLISHED;  
  
  err = sdp_decode(call->sdp, msg->mb, true);
  if (err) {
  	re_fprintf(stderr, "unable to decode SDP offer\n");
    goto out;
  }
  
  fmt = sdp_media_rformat(call->sdp_media, NULL);
  if(!fmt || fmt->pt == 101){
  	re_printf("no common media format found\n");
    err = 1;
	goto out;
  }
  
  re_printf("call payload is %d \n",fmt->pt);
  call->pt = fmt->pt;
  
  
  sa_cpy(&call->rtp_dest,sdp_media_raddr(call->sdp_media)); 
  sa_cpy(&call->rtp_sess->remote_ip,sdp_media_raddr(call->sdp_media));
  
  
  if(call->ispbxcall) {
  
    call->dest->call->rtp_sess->remote = call->rtp_sess;
    call->rtp_sess->remote = call->dest->call->rtp_sess;
    return;
  } 
  tmr_cancel(&call->tmr);
  //tmr_start(&call->tmr,40,call_rtp_timer,call);
  
  
  if(!call->join_pbx){
    if (thrd_create(&call->t, rtpThread, call) == thrd_success)
    {
      re_printf("started rtp thread \n");
      call->establish_tm = time(NULL);
      
      call->thrd_created = true;
    }  
    if(!call->rtp_sess->telev) telev_alloc(&call->rtp_sess->telev,TELEV_PTIME);
    call->rtp_sess->telev_recv = true;
        
  }
  
      
  update_call_status(call->arg); 

 
  
  re_printf("call established %d %J\n",msg->scode,&call->rtp_dest);
  
  out:
  if(err){
      call->ended = true;
      call->sess = mem_deref(call->sess);
  }
  return;
}




/* called upon incoming calls */
static void connect_handler(const struct sip_msg *msg, void *arg)
{
   // incoming_call_alloc(sip,sess_sock,msg);

}

static bool sip_register_handler(const struct sip_msg *msg, void *arg){
      if (!pl_strcmp(&msg->met, "REGISTER")){
           sip_reply(sip,msg,200,"OK");
           return false;
      }
      
      return false;
}




int call_alloc(struct call **callp,const char *tel,const struct uri *gateway,bool isPbx,void *arg){
   
   char *remote,*url,*fname;
   struct mbuf *mb;
   struct call *call;
   struct pl pl = PL_INIT;
   struct pl pl1 = PL_INIT;
   uint8_t pt = 0;
   int err;
   struct destination *dest = arg;
   call = mem_zalloc(sizeof(*call), call_destructor);
   if(!call)
      return 1;
      
   if(isPbx)
     pt = dest->call->pt;
   else{
     pl_set_str(&pl,"codec"); 
     err = uri_param_get(&gateway->params,&pl,&pl1);
     if(!err)
         pt = pl_u32(&pl1);  
     re_printf("gateway codec is %r %d\n",&pl1,pt);            
   }
   
   err =   sdp_sess_alloc(&call->sdp,&call->sdp_media,&call->rtp_sess,pt);
   
   if(err)
     goto out;
  
      
   /* create SDP offer */
	err = sdp_encode(&mb, call->sdp, true);
	if (err) {
		re_printf( "sdp encode error \n");
		goto out;
	}

   
  
   pl_set_str(&pl,"prefix");
   pl1.l = 0; 
   err = uri_param_get(&gateway->params,&pl,&pl1);
   re_printf("gateway prefix is %r \n",&pl1);
   if(pl_isset(&pl1)){
     
      //pl.p = gateway->params.p;
      //pl.l = gateway->params.l;
      //pl_advance(&pl,1);
      //re_printf("gateway prefix is %r \n",&pl);
      re_sdprintf(&remote,"sip:%r%s@%r:%d",&pl1,tel,&gateway->host,gateway->port);
   }else
        re_sdprintf(&remote,"sip:%s@%r:%d",tel,&gateway->host,gateway->port);
  /* if(isPbx){
        str_dup(&fname,dest->dest);
        re_sdprintf(&url,"sip:%s@%J",dest->dest,&laddr);
   }else{ */
      pl_strdup(&fname,&gateway->user);
      re_sdprintf(&url,"sip:%r@%J",&gateway->user,&laddr);    
  // }
        
   
   re_printf("sending call to %s url %s \n",remote,url);
   
   	
   err = sipsess_connect(&call->sess, sess_sock, remote ,fname ,
			      (const char *) url , fname,
			      NULL, 0, "application/sdp", mb,
			      auth_handler, gateway , false,
			      offer_handler, answer_handler,
			      progress_handler, establish_handler,
			      NULL, NULL, close_handler, call, NULL);



     mem_deref(remote);
     mem_deref(url);
     mem_deref(mb);
     mem_deref(fname);
     err = rtp_alloc(&call->rs);  
     call->rtp_sess->arg = call;
     
     
     out:
     if(err == 0){
       // re_printf("allocate new call for %s \n",remote);
        call->connect_tm = time(NULL);
        call->call_status = CALL_CONNECTING;
        call->ended = false;
        
        tmr_init(&call->tmr);
        sa_init(&call->rtp_dest,AF_INET);
        call->ts = rand();
        call->pt = pt;
        call->arg = arg; 
        *callp = call;
        call->dest = gateway;
     }else{
       mem_deref(call);
       *callp = NULL;
     }
     
     
     
     return err;
     
   
}


/*
static int rtp_session_start(const struct sa *localip){
      int err;
      struct sa sa;
      
      sa_init(&sa,AF_INET);
      sa_cpy(&sa,localip);
      sa_set_port(&sa,0);
      err = udp_listen(&us,&sa,rtp_recv_handler,NULL);
      if(err) goto out;
      if(err == 0){
           err = udp_local_get(us,&sa);
           if(err) goto out; 
      }
      
            	// create SDP session 
    	err = sdp_session_alloc(&sdpsess, &sa);
    	if (err) {
    		re_fprintf(stderr, "sdp session error: %s\n", strerror(err));
    		goto out;
    	}
    
    	// add audio sdp media, using dummy port: 4242 
    	err = sdp_media_add(&sdp_media, sdpsess, "audio", sa_port(&sa), "RTP/AVP");
    	if (err) {
    		re_fprintf(stderr, "sdp media error: %s\n", strerror(err));
    		goto out;
    	}
    
    	// add G.711 sdp media format 
    	err = sdp_format_add(NULL, sdp_media, false, "18", "G729", 8000, 1,
    			     NULL, NULL, false, NULL);
    	if (err) {
    		re_fprintf(stderr, "sdp format error: %s\n", strerror(err));
    		goto out;
    	}
      
      const char *silence_file = "1.g729";
      char path[512];      
      char cwd[512];
      uint8_t  serial[50];
      FILE *f_serial;  
      if( access( silence_file, F_OK ) != -1 ){
           if (getcwd(cwd, sizeof(cwd)) != NULL)
              sprintf(path,"%s/%s",cwd,silence_file);
           else       
              sprintf(path,"%s",silence_file);
      }
      
      if ( (f_serial = fopen(path, "rb")) == NULL) {
        re_printf("Error opening file  for silence %s !!\n",path);       
        goto out;     
      }
      
      silence = mbuf_alloc(50);
      
      if(fread(serial, sizeof(uint8_t), 50 , f_serial) == 50)
        mbuf_write_mem(silence,serial,50); 
        
      fclose(f_serial);
      silence->pos = 0;
      re_printf("rtp session started at %J \n",&sa);
        
      
        
        
      out:
      return err;
} 

static void rtp_session_destroy(void){
     mem_deref(us);
     mem_deref(sdpsess);
     mem_deref(silence);
} */

/* called when all sip transactions are completed */
static void exit_handler(void *arg)
{
	(void)arg;

	/* stop libre main loop */
	re_cancel();
}




static int udp_range_listen(struct rtp_session *rtp_proxy, const struct sa *ip,
			    uint16_t min_port, uint16_t max_port)
{
	
	int tries = 64;
	int err = 0;

	rtp_proxy->local_ip = *ip;

	/* try hard */
	while (tries--) {
		struct udp_sock *us_rtp;
		uint16_t port;

		port = (min_port + (rand_u16() % (max_port - min_port)));
		port &= 0xfffe;

		sa_set_port(&rtp_proxy->local_ip, port);
		err = udp_listen(&us_rtp, &rtp_proxy->local_ip, rtp_recv_handler, rtp_proxy);
		if (err)
			continue;

		

		/* OK */
		rtp_proxy->rs = us_rtp;
        rtp_proxy->connected = false;
        rtp_proxy->in_use = false;
		break;
	}

	return err;
}



void rtp_session_allocate(const struct sa *addr){
   int i=0;
   int err;
   list_init(&rtp_sessions);
   
   for(i=0;i<100;i++){
      struct rtp_session *rs;
      rs = mem_zalloc(sizeof(*rs), rtp_session_destructor);
      if(rs){
         lock_alloc(&rs->lock);
         err = udp_range_listen(rs,addr,8000,20000);
         if(err==0) list_append(&rtp_sessions,&rs->le,rs);
         else mem_deref(rs);
      }
        
   }
   re_printf("rtp allocation successfull %d\n",list_count(&rtp_sessions)); 

}

void rtp_session_destroy(){


 list_flush(&rtp_sessions); 

}


int sipcall_alloc(const struct sa *sip_ip){
	
   struct sa nsv[16];
   
   uint32_t nsc;
   int err;
   nsc = ARRAY_SIZE(nsv);

	/* fetch list of DNS server IP addresses */
	err = dns_srv_get(NULL, 0, nsv, &nsc);
	if (err) {
		re_fprintf(stderr, "unable to get dns servers: %s\n",
			   strerror(err));
		return 1;
	}

	/* create DNS client */
	err = dnsc_alloc(&dnsc, NULL, nsv, nsc);
	if (err) {
		re_fprintf(stderr, "unable to create dns client: %s\n",
			   strerror(err));
			return 1;
	}


    /* create SIP stack instance */
	err = sip_alloc(&sip, dnsc, 32, 32, 32,
			"AUTODIALER " AUTODIALER_VERSION " (" ARCH "/" OS ")",
			exit_handler, NULL);
	if (err) {
		re_fprintf(stderr, "sip error: %s\n", strerror(err));
			return 1;
	}
  
    /* add supported SIP transports */
  err = sip_transp_add(sip, SIP_TRANSP_UDP, sip_ip);
  if(err){
      	re_fprintf(stderr, "conf tcg server ip  listen error: %J \n",sip_ip);
				return 1;
  }
  



	/* create SIP session socket */
	err = sipsess_listen(&sess_sock, sip, 32, connect_handler, NULL);
	if (err) {
		re_fprintf(stderr, "session listen error: %s\n",
			   strerror(err));
		return 1;
	}
  
  //sip_listen(NULL,sip,true,sip_register_handler,NULL);
  re_printf("sip listening successfull at %J \n",sip_ip);
  sa_init(&laddr,AF_INET);
  sa_cpy(&laddr,sip_ip); 
  rtp_session_allocate(sip_ip);
  
  //check_sdp_null();
  
  return 0;
}


void close_sip(void){
  sip_close(sip, false);
  mem_deref(dnsc);
  mem_deref(sip);
  mem_deref(sess_sock);
  rtp_session_destroy();
}



