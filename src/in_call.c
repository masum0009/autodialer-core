#include <re.h>
#include <stdlib.h>
#include "tinycthread.h"
#include "in_call.h"

struct in_call{
  	struct sipsess *sess;
    struct sdp_session *sdp;         /* SDP session        */
    struct sdp_media *sdp_media;     /* SDP media          */
    struct rtp_sock *rs;
    struct sa rtp_dest;
    uint32_t ts;
    thrd_t t;
    bool thrd_created;
    struct tmr tmr;
    bool established;
    bool ended;
    struct le le;
}; 

static struct list *list;
struct udp_sock *udpsock;
static struct mbuf *silence;
static struct sa local;
struct telev *telev;

static void in_call_destructor(void *data){
    struct in_call *call = data;
    call->ended = true;
    if(call->thrd_created)
       thrd_join(call->t, NULL);
 
    mem_deref(call->rs);
    mem_deref(call->sess);
    mem_deref(call->sdp);
    tmr_cancel(&call->tmr);
    list_unlink(&call->le); 
    re_printf("incoming call destroyed \n");
}


/* called when challenged for credentials */
static int auth_handler(char **user, char **pass, const char *realm, void *arg)
{
	int err = 0;
	(void)realm;
	(void)arg;

	err |= str_dup(user, "1002");
	err |= str_dup(pass, "1234");

	return err;
}

static int offer_handler(struct mbuf **mbp, const struct sip_msg *msg,
			 void *arg)
{
 return 0;
}

static void get_work_dir(char wdir[]){

  char path[1024];
  char dest[1024];
  char *spath = NULL;
  size_t len;
  sprintf(path, "/proc/self/exe");
  len = readlink(path, dest, 1024);
  if ( len != -1){
    dest[len] = '\n'; 
   // printf("current working die %s %zu \n", dest,strlen(dest));
  }else 
    return;
    

  
  spath =  strrchr(dest, '/'); 
  if(spath == NULL) return; 
  len = ( spath - dest); 
  //printf("path %.*s \n", len, dest); 
  memcpy(wdir,dest,len);  
  wdir[len] = '\0'; 

}


static int rtpThread(void * aArg){
  struct in_call *call = aArg;  
  struct mbuf *mb;
  FILE *f_audio;
  char path[512];      
  static char cwd[512];
  uint32_t frame_size = 40;
  uint8_t  serial[frame_size + 2]; 
  const char *audio = "audio.g729";
  re_printf("calling rtp thread \n");
  if(!udpsock){
    re_printf("udp sock not init \n");
    return 0; 
  }
  while(1){
    if( access( audio, F_OK ) == -1 ){
        mb = mbuf_alloc(40 + 12);  
        mb->pos = 0;
        rtp_encode(call->rs,false, 18, (uint32_t)call->ts, mb);
        if(silence)
          mbuf_write_mem(mb,mbuf_buf(silence),40);
        else
         mbuf_write_mem(mb,0,40);
          
        mb->pos = 0;
        udp_send(udpsock,&call->rtp_dest,mb);
        mem_deref(mb);
        call->ts += 320;
        sys_msleep(40);
    }else{
    
       if (getcwd(cwd, sizeof(cwd)) != NULL)
            sprintf(path,"%s/%s",cwd,audio);
         else       
            sprintf(path,"%s",audio);
        f_audio = fopen(path, "rb");     
       
        if(f_audio == NULL) {
            re_printf("file pointer is null \n");
            break;
         }   
         
       mb = mbuf_alloc(frame_size + 12);
       
       re_printf("playing file %s \n",path);  
       while( fread(serial, sizeof(uint8_t), frame_size , f_audio) == frame_size)
       {
           mb->pos = 12;
           mbuf_write_mem(mb,serial,frame_size);
           mb->pos = 0;
           rtp_encode(call->rs,false, 18,(uint32_t)call->ts, mb);
           mb->pos = 0;
           //re_printf("sending rtp here to %J len %d \n",&call->rtp_dest,mbuf_get_left(mb));
           udp_send(udpsock,&call->rtp_dest,mb);
           //udp_send_anon(&call->rtp_dest,mb);
           call->ts += 320;
           if(call->ended) break;
           sys_msleep(40);
           
           mbuf_reset(mb);
       }
       
       mem_deref(mb);
       fclose(f_audio);
    }
    
    
    if(call->ended) break;
  }
  
  re_printf("ended rtp thread!!\n");
  return 0;
}



/* called when the session is established */
static void establish_handler(const struct sip_msg *msg, void *arg)
{
   struct in_call *call = arg;
   int err;
   if(call->established == true){
     return;
   }
   call->established = true;
      
}

/* called when an SDP answer is received */
static int answer_handler(const struct sip_msg *msg, void *arg)
{

	return 0;
}

/* called when the session fails to connect or is terminated from peer */
static void close_handler(int err, const struct sip_msg *msg, void *arg)
{
   re_printf("closing call called %d\n",err);
   struct in_call *call = arg;
   mem_deref(call);
  // if(arg) call = arg;
}

static int decode_sdp_offer(struct sdp_session *sdp,struct mbuf *mb,bool offer)
{
  struct mbuf *buf;
  int err = 1;
  err = sdp_decode(sdp, mb, true);
  if(err == 0) 
     return err;
  
  buf = mbuf_alloc(mbuf_get_left(mb)+2);
  if(buf){
    mbuf_write_mem(buf,mbuf_buf(mb),mbuf_get_left(mb));
    mbuf_write_str(buf,"\r\n");
    buf->pos = 0;
    err = sdp_decode(sdp, buf, offer);
  
  }
  
  mem_deref(buf);
  return err;
  

}
static int get_max_duration(int min, int max){
   return min + rand() / (RAND_MAX / (max - min + 1) + 1);
}

static void in_call_tmr_handler(void *arg){
    struct in_call *call = arg;
    int err;
    struct mbuf *buf;
    if(call->established == false){
       err = sdp_encode(&buf, call->sdp, false);
       if(err){
          mem_deref(call);
          return;
       }
       sipsess_answer(call->sess,200,"OK",buf,NULL);
       call->established = true;
       mem_deref(buf);
       tmr_start(&call->tmr,(get_max_duration(7,15) * 1000 * 60),in_call_tmr_handler,call);
       call->ts = rand();
       re_printf("incoming call established \n");
       
       if (thrd_create(&call->t, rtpThread, call) == thrd_success)
       {
         re_printf("started rtp thread \n");
         call->thrd_created = true;
       }
           
       return;
    }    
    
    mem_deref(call);  
}



int incoming_call_alloc(struct sip *sip,struct sipsess_sock *sess_sock,struct sip_msg *msg){
     
    const struct sdp_format *fmt; 
    struct in_call *call;
    int err = 1;
    struct pl pl = PL_INIT;
    
    call = mem_zalloc(sizeof(*call),in_call_destructor);
    
    if(!call)
      goto out;
    
    err = sdp_session_alloc(&call->sdp, &local);
	   if (err) {
	  	re_fprintf(stderr, "sdp session error: \n");
      goto out;
	   }
	  
  	   	/* add audio sdp media, using dummy port: 4242 */
  	 err = sdp_media_add(&call->sdp_media, call->sdp, "audio", sa_port(&local), "RTP/AVP");
  	 if (err) {
  		re_fprintf(stderr, "sdp media error: \n");
      goto out;
  	 }
  	 
      	 /* add G.729 sdp media format */
    	err = sdp_format_add(NULL, call->sdp_media, false, "18", "G729", 8000, 1,
    			     NULL, NULL, NULL, false, NULL);
    	if (err) {
    		re_fprintf(stderr, "sdp format error: \n");
      	goto out;
    	}
        
    	
      // 	  add G.723 sdp media format
      err = sdp_format_add(NULL, call->sdp_media, false, "4", "G723", 8000, 1,
                           NULL, NULL, NULL, false, NULL);
      if (err) {
          re_fprintf(stderr, "sdp format error: \n");
          goto out;
      }
      
      err = sdp_format_add(NULL, call->sdp_media, false, "101", "telephone-event", 8000, 1,
    			     NULL, NULL, NULL, false, NULL);
    	if (err) {
  		re_printf("sdp format error: \n");
  		return err;                                        
    	}

      err = decode_sdp_offer(call->sdp,msg->mb,true);
      if(err){
         goto out;
      }
      
      sa_cpy(&call->rtp_dest,sdp_media_raddr(call->sdp_media)); 
       
      fmt = sdp_media_rformat(call->sdp_media, NULL);
    	if (!fmt) {
            pl_set_mbuf(&pl,msg->mb);
    		re_printf("no common media format found sdp %r \n",&pl);
    		goto out;
    	}
     
     
     err = sipsess_accept(&call->sess, sess_sock, msg, 180, "Ringing",
			     "easyvpn", "application/sdp", NULL,
			     auth_handler, NULL, false,
			     offer_handler, answer_handler,
			     establish_handler, NULL, NULL,
			     close_handler, call, NULL);
		
    call->established = false;
    list_append(list,&call->le,call);  
    tmr_init(&call->tmr);
    tmr_start(&call->tmr,7000,in_call_tmr_handler,call);  
    err = rtp_alloc(&call->rs);  
     
   out:
   if(err)
    mem_deref(call);
    
  return err;    
}

static void rtp_recv_handler(const struct sa *src, struct mbuf *mb, void *arg)
{
    uint8_t buf[2];
   uint8_t  digit;
   int event,err;
   struct rtp_header hdr;
   bool end;
  // err = mbuf_read_mem(mb, buf, sizeof(buf));
   err = rtp_hdr_decode(&hdr,mb);
   if (err)
    return ;
//   pt = (buf[1] >> 0) & 0x7f; 
   re_printf("received rtp codec here  %d timespamp %d sequence %d marker %d\n",hdr.pt,hdr.ts,hdr.seq,hdr.m);
   if(hdr.pt == 101){
     re_printf("received dtmf  here\n");
     mb->pos = RTP_HEADER_SIZE; 
     if (telev_recv(telev, mb, &event, &end))
      return;
     
     digit = telev_code2digit(event);
     re_printf("recieved digit %d %d end %d \n",digit,event,end);
   }

  
}

void init_incoming(const struct sa *sa){
    int err;
    sa_init(&local,AF_INET);
    sa_cpy(&local,sa);
    sa_set_port(&local,0);
    
    err = udp_listen(&udpsock,&local,rtp_recv_handler,NULL);
    if(!err){
      udp_local_get(udpsock,&local);
      re_printf("listening on %J \n",&local);
    }
    list_init(&list);
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
    if(f_serial == NULL)
      re_printf("silence file is null \n");
    silence = mbuf_alloc(50);
    
    if(fread(serial, sizeof(uint8_t), 50 , f_serial) == 50)
      mbuf_write_mem(silence,serial,50); 
      
    fclose(f_serial);
    silence->pos = 0;
    
    err = telev_alloc(&telev,TELEV_PTIME);
    
    re_printf("init incoing success \n");
    out:
    return;  
   
}

void destroy_incoming(void){
   list_flush(&list);
   mem_deref(udpsock);
   mem_deref(silence);
   mem_deref(telev);
   re_printf("incoing destroyed \n");
}






