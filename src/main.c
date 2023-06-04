#include <getopt.h> 
#include <string.h>
#include <re.h>
#include "tinycthread.h"
#include "db.h"
#include "call.h"






static struct mqueue *msgq;
static struct lock *lock;
static struct list services; 
static struct list gateways;
static struct db_con *db;
static bool running = true;
static struct uri fcg_uri;
static char *audio_path = NULL;





static void conf_destructor(void *data){
  struct db_config *db_config = data;
  
  mem_deref(db_config->user.p);
  mem_deref(db_config->pass.p);
  mem_deref(db_config->db.p);
  mem_deref(db_config->host.p);
  
}

static void destination_destructor(void *data){
   struct destination *destination = data;
   mem_deref(destination->dest);
   if(destination->call) mem_deref(destination->call);
   tmr_cancel(&destination->tmr);
   list_unlink(&destination->le);
   re_printf("destination destoryed \n");
     
}

static void gateway_destructor(void *data){
   struct gateway *gateway = data;
   mem_deref(gateway->uri_str);
   
   list_unlink(&gateway->le);
   re_printf("gateway destoryed \n");     
}

static void cp_action_destructor(void *data){
   struct campaign_action *action = data;
   mem_deref(action->dest);
   list_unlink(&action->le);
   re_printf("action destoryed \n");     
}





static void campaign_destructor(void *data){
    struct campaign *cp = data;
    tmr_cancel(&cp->tmr);
    mem_deref(cp->caller_id);
    mem_deref(cp->file_name);
    mem_deref(cp->pbx_number);    
    lock_write_get(lock);
    list_unlink(&cp->le);
    lock_rel(lock);
    list_flush(&cp->destinations);    
    list_flush(&cp->processl);
    list_flush(&cp->actions);
//    db_exec(db,"update sim_groups set current_status = 0 where id = %d",fcg->group_id);
}



static void terminate(void)
{
 /* terminate session */
	/* wait for pending transactions to finish */
   running = false;
   close_sip();
   re_cancel();
   re_printf("cancelling re thread \n");
}



/* called upon reception of  SIGINT, SIGALRM or SIGTERM */
static void signal_handler(int sig)
{
  
	re_printf("terminating on signal %d...\n", sig);
  list_flush(&gateways);  
  list_flush(&services);
  
	terminate();
}

static uint32_t parseInt(const char *data){
   struct pl pl = PL_INIT;
   pl_set_str(&pl,data);
   return pl_u32(&pl); 
}

static bool campaign_find_handler(struct le *le, void *arg){
  int campaign_id = *((int*)arg);
  struct campaign *cp = list_ledata(le);
  if(cp->campaign_id == campaign_id) return true;
  return false;

}

static bool gateway_find_handler(struct le *le, void *arg){
  int gateway_id = *((int*)arg);
  struct gateway *gt = list_ledata(le);
  if(gt->gateway_id == gateway_id) return true;
  return false;

}

/*
0 - created
1 - ready
2 - running
3 - pause
4 - stopped
5 - disabled
*/

static void pull_groups(void){
    MYSQL_RES *res;
    MYSQL_ROW row;
    int err;   
    int id;
    //struct list list;
    
    res = get_results(db,"select id,ip,port,user,password,prefix,codec from gateway ");
    if(res == NULL) return; 
    while(row=mysql_fetch_row(res)){
       int gateway_id = parseInt(row[0]);
        struct gateway *gt = list_ledata(list_apply(&gateways,true,gateway_find_handler,&gateway_id)); 
        if(!gt){
             gt = mem_zalloc(sizeof(*gt),gateway_destructor);
             gt->gateway_id = gateway_id;
             list_append(&gateways,&gt->le,gt); 
        }
        
        char uristr[1024];
         if(str_len(row[3]) > 0 && str_len(row[4]) > 0)
            re_snprintf(uristr,1024,"sip:%s:%s@%s:%s;prefix=%s;codec=%d",row[3],row[4],row[1],row[2],row[5],parseInt(row[6]));
         else if(str_len(row[3]) > 0 )
            re_snprintf(uristr,1024,"sip:%s@%s:%s;prefix=%s;codec=%d",row[3],row[1],row[2],row[5],parseInt(row[6]));
         else
            re_snprintf(uristr,1024,"sip:autodialer@%s:%s;prefix=%s;codec=%d",row[1],row[2],row[5],parseInt(row[6]));   
            
         if(str_casecmp(uristr,gt->uri_str)){
              gt->uri_str = mem_deref(gt->uri_str);
              str_dup(&gt->uri_str,uristr);
              struct pl pl = PL_INIT;
              pl_set_str(&pl,gt->uri_str);
              if(uri_decode(&gt->sip_uri,&pl)){
                 re_printf("uri decode failed %r, invalid gateway \n",&pl);
                 mem_deref(gt);
                 continue; 
                  
              }                                    
         }
        re_printf("gateway fetched %s \n",gt->uri_str);                          
    }
    mysql_free_result(res);
    
    //list_init(&list);
    res = get_results(db,"select c.id,c.client_id,c.frequency,c.max_call_duration,c.caller_id_number,a.filename,c.status,c.time_between_retries,c.max_call_retry,c.play_count,c.gateway_id,c.pbx_gateway_id,c.pbx_number FROM campaigns c LEFT JOIN audios a ON a.id = c.audios_id WHERE  c.status > 1 ");
    if(res == NULL) return; 
    while(row=mysql_fetch_row(res)){
       int campaign_id = parseInt(row[0]);
       int campaign_status = parseInt(row[6]);
       struct campaign *cp = list_ledata(list_apply(&services,true,campaign_find_handler,&campaign_id));
       if(!cp){
         if(campaign_status != 2) continue;
         
         cp = mem_zalloc(sizeof(*cp),campaign_destructor);
         list_init(&cp->destinations);
         list_init(&cp->processl); 
         
         list_append(&services,&cp->le,cp); 
         re_printf("new campaign created \n");
       }
        
       if(cp){
         cp->campaign_id = parseInt(row[0]);
         cp->client_id = parseInt(row[1]);;
         cp->call_limit = parseInt(row[2]);
         if(cp->call_limit == 0) cp->call_limit = 2; 
         cp->max_call_duration = parseInt(row[3]);
         if(cp->max_call_duration < 0)
            cp->max_call_duration = 300;
         if(cp->max_call_duration >= 600)
            cp->max_call_duration = 600;
            
        // cp->is_running = false;
         
         if(str_len(row[4]) > 0 && str_casecmp(row[4],cp->caller_id))  
         {
            cp->caller_id = mem_deref(cp->caller_id);
            str_dup(&cp->caller_id,row[4]);
         }
         
         if(str_len(row[5]) > 0 && str_casecmp(row[5],cp->file_name))  
         {
            cp->file_name = mem_deref(cp->file_name);
            if(str_len(row[5]) > 0) str_dup(&cp->file_name,row[5]);
         }else if(str_len(row[5]) == 0){
            cp->file_name = mem_deref(cp->file_name);
         }
         
         cp->campaign_status = parseInt(row[6]);
         if(cp->campaign_status > 3 && list_count(&cp->processl) == 0){
               mem_deref(cp);
               continue;
         }
         
         cp->retry_delay = parseInt(row[7]) * 60;
         
        
         
         cp->max_try = parseInt(row[8]);
         if(cp->max_try < 0)
             cp->max_try = 3;
             
         cp->max_play = parseInt(row[9]);
          if(cp->max_play <= 0)
             cp->max_play = 1;
             
         id =  parseInt(row[10]);          
         cp->gateway =  list_ledata(list_apply(&gateways,true,gateway_find_handler,&id)); 
         
         id =  parseInt(row[11]);
         cp->pbx_gateway =  list_ledata(list_apply(&gateways,true,gateway_find_handler,&id));
         
         if(str_casecmp(row[12],cp->pbx_number)){
              cp->pbx_number = mem_deref(cp->pbx_number);
              str_dup(&cp->pbx_number,row[12]);
         }
         
         
         
         re_printf("adding campaign  id %d client id %d max play %d \n",cp->campaign_id,cp->client_id,cp->max_play);
         
         
         
       }
       
    }
    mysql_free_result(res);
    
    
    struct le *le;
    for(le=services.head;le;le =le->next){      
      struct campaign *cp = list_ledata(le);
      struct pl prefix = PL_INIT;
      re_printf("getting campaign id %d \n",cp->campaign_id);
         
     // db_real_exec(db,"update campaigns set status = 2 where status = 1 AND id = %d",cp->campaign_id);

      if(cp->is_running == true) continue;
      
     // if(list_count(&cp->destinations) > 0)
     //    continue;
         
         
     // if(msg_param_decode(&cp->sip_uri,"prefix",&prefix) == 0){
     //       re_printf("gateway prefix is %r \n",&prefix);
     // }
      
      if(list_count(&cp->destinations) == 0){
          res = get_results(db,"SELECT id,contact_number from calls where call_duration = 0 AND campaign_id = %d",cp->campaign_id);
          if(res == NULL) continue;
          while(row=mysql_fetch_row(res)){
            struct destination *dest;        
            dest = mem_zalloc(sizeof(*dest),destination_destructor);
            dest->dest_id = parseInt(row[0]);
            str_dup(&dest->dest,row[1]);
            dest->cp = cp;
            dest->try_count = 0;
            list_append(&cp->destinations,&dest->le,dest);
            re_printf("adding destination %s id %d\n",dest->dest,dest->dest_id);   
          }
          mysql_free_result(res);
          
      }
      
      list_flush(&cp->actions);
      res = get_results(db,"SELECT id,key_press,action,action_data from actions where  campaigns_id = %d",cp->campaign_id);
      while(row=mysql_fetch_row(res)){
            struct campaign_action *action;
            action = mem_zalloc(sizeof(*action),cp_action_destructor);
            action->action_key = parseInt(row[1]);
            action->action_type = parseInt(row[2]);
            if(action->action_type == 0){
                struct pl pl = PL_INIT;
                pl_set_str(&pl,row[3]);
                const char *p = pl_strchr(&pl,',');
                if(p){
                  struct pl pt = PL_INIT;
                  pt.p = pl.p;
                  pt.l =  p - pl.p;
                  p++;
                  int gateway_id = pl_u32(&pt);
                  action->gateway =  list_ledata(list_apply(&gateways,true,gateway_find_handler,&gateway_id));
                  pl.p = p;
                  pl.l = pl.l - (pt.l +1); 
                  pl_strdup(&action->dest,&pl);
                  re_printf("action gateway id %d dest %r \n",gateway_id,&pl);
                  
                }
            }
            list_append(&cp->actions,&action->le,action);                                    
      }
      
          
      
      
            
    /*  list_unlink(&cp->le);
      lock_write_get(lock);
      list_append(&services,&cp->le,cp); 
      re_printf("added to services %d \n",cp->campaign_id); 
      lock_rel(lock);   */
      
      mqueue_push(msgq,cp->campaign_id,cp);
      
    }
    //list_flush(&list);
    
  
    
}

int DbThread(void * aArg)
{
  
  while(running){
   db_sync(db);
   pull_groups();
  //re_printf("sleeping in db tread \n");
   if(running==false) break;
   sleep(10);
  }
  re_printf("db thread terminated\n");
  
}

struct campaign_action  *find_campaign_action(void *arg,int event){
    struct destination *dest = arg;
    struct campaign_action *action;
    struct le *le;
    for(le=dest->cp->actions.head;le;le=le->next){
        action = list_ledata(le);
        if(action->action_key == event)
          return action; 
    }
    return NULL;
}

void update_call_status(void *arg);
void update_call_status(void *arg){
   struct destination *dest = arg;
   int duration = 0;
  /* if(dest->call->call_status == CALL_ESTABLISHED){
         duration = time(NULL) - dest->call->establish_tm;
         
   } */
   db_exec(db,"update calls set  call_status=%d,call_duration=%d,connect_time=%ld,establish_time=%ld where id = %d",dest->call->call_status,duration,(long)dest->call->connect_tm,(long)dest->call->establish_tm,dest->dest_id);       
  
}

static void on_call_end(void *arg){
   int err;
   int duration = 0;
   struct destination *dest = arg;
   
   
   if(dest->call->ended == false) {
        if(list_count(&dest->cp->actions) == 0 && dest->call->call_status == CALL_ESTABLISHED && dest->call->num_play >= dest->cp->max_play){
          
          re_printf("force destroying call, num_play %d greater than max play %d \n",dest->call->num_play,dest->cp->max_play);
          dest->call->sess = mem_deref(dest->call->sess);
          dest->call->ended = true;
          dest->call->resp_code = 1; 
        
        }else if(dest->call->call_status == CALL_ESTABLISHED && (dest->call->event_received || dest->call->join_pbx )){
            if(dest->call->forwarded == false){
                if(dest->call->event_received)
                    err = call_alloc(&dest->call->pbx_call,dest->call->action->dest,&dest->call->action->gateway->sip_uri,true,dest);
                 else
                    err = call_alloc(&dest->call->pbx_call,dest->cp->pbx_number,&dest->cp->pbx_gateway->sip_uri,true,dest);   
                if(!err){
                   dest->call->pbx_call->ispbxcall = true;
                   dest->call->forwarded = true;
                   dest->call->pbx_call->dest = dest;
                   re_printf("call is forwarded \n");
                }
            }else if((time(NULL) - dest->call->establish_tm) > 30 * 60){
                     dest->call->sess = mem_deref(dest->call->sess);
                     dest->call->ended = true;
                     dest->call->resp_code = 1;
            
            }else{
               if(dest->call->pbx_call && dest->call->pbx_call->ended){
                     dest->call->sess = mem_deref(dest->call->sess);
                     dest->call->ended = true;
                     dest->call->resp_code = 1; 
               }
            }
            
        
        }else if(dest->call->forwarded == false && dest->call->call_status == CALL_ESTABLISHED && dest->cp->max_call_duration > 0 &&   (time(NULL) - dest->call->establish_tm) > dest->cp->max_call_duration){
          re_printf("force destroying call, duration %d greater than max duration %d \n",(time(NULL) - dest->call->establish_tm),dest->cp->max_call_duration);
          dest->call->sess = mem_deref(dest->call->sess);
          dest->call->ended = true;
          dest->call->resp_code = 1; 
          
        }else if(dest->call->call_status < CALL_ESTABLISHED && (time(NULL) - dest->call->connect_tm) > 40){
          re_printf("force destroying call as in connecting for more than 40 sec\n");
          dest->call->sess = mem_deref(dest->call->sess);
          dest->call->ended = true;
          dest->call->resp_code = 1;
        }
        
         
   }else{
       
       if(dest->call->sess)  dest->call->sess = mem_deref(dest->call->sess);
        dest->call->resp_code = 1;
       
   }
   
   //re_printf("dest debug %d %d %d \n",dest->call->ended,dest->dest_id,(time(NULL) - dest->call->connect_tm));
   
   if(dest->call->call_status == CALL_ESTABLISHED){
       duration = time(NULL) - dest->call->establish_tm;    
   }else{
       duration = 0;
   }
   
   if(dest->call->ended == true){
        dest->call->call_status = CALL_DISCONNECTED;
        db_exec(db,"update calls set  retry_count=%d,call_status=%d,call_duration=%d,connect_time=%ld,establish_time=%ld,disconnect_time=%ld,disconnect_cause=%d,call_action_key=%d,call_action_type=%d,event_received=%d  where id = %d",
        dest->try_count,dest->call->call_status,duration,(long)dest->call->connect_tm,(long)dest->call->establish_tm,(long)time(NULL),dest->call->resp_code,dest->call->action_key,dest->call->action_type,dest->call->event_received,dest->dest_id
        );         
        if(dest->call->pbx_call){
             if(dest->call->pbx_call->call_status == CALL_ESTABLISHED){
                 duration = time(NULL) - dest->call->pbx_call->establish_tm;    
             }else{
                 duration = 0;
             }
           db_exec(db,"INSERT INTO call_forwarding set campaigns_id=%d, gateway_id=%d, call_from=%s, call_to=%s,call_status=%d,call_duration=%d,connect_time=%ld,establish_time=%ld,disconnect_time=%ld,disconnect_cause=%d",
              dest->cp->campaign_id,(dest->call->event_received)?dest->call->action->gateway->gateway_id:dest->cp->gateway->gateway_id,dest->dest,(dest->call->event_received)?dest->call->action->dest:dest->cp->pbx_number,dest->call->pbx_call->call_status,duration,(long)dest->call->pbx_call->connect_tm,(long)dest->call->pbx_call->establish_tm,(long)time(NULL),dest->call->pbx_call->resp_code
           );
        }
         
   }     
    
                                                         
   if(dest->call->ended == true ){
       re_printf("trying  destorying destination as try count %d where max try %d or duration %d \n",dest->try_count,dest->cp->max_try,duration);
 
       if(dest->cp->max_try >0 && dest->try_count >= dest->cp->max_try){
           mem_deref(dest);
          return;
       }else if(duration > 0){
           mem_deref(dest);
          return;
       }else{
           dest->call = mem_deref(dest->call);
           list_unlink(&dest->le);    
           list_append(&dest->cp->destinations,&dest->le,dest);
           re_printf("adding at list append \n");
           return;
       } 
   }
   tmr_start(&dest->tmr,1000,on_call_end,dest);
   
}

static bool destination_sort_handler(struct le *le1, struct le *le2, void *arg){
  struct destination *dest1 = list_ledata(le1);
  struct destination *dest2 = list_ledata(le2); 
  
  if(dest1->try_count > dest2->try_count)
     return false;
   
  return true;   
  
}

static void start_fake_calling(void *arg){
   struct le *le;
   int err;
   uint8_t count = 0;
   struct campaign *cp = arg;
                                                  
   if(cp->campaign_status != 2) 
   {
    cp->is_running = false;
    return; 
   }
     
   
   if(list_count(&cp->processl) <  cp->call_limit){
     list_sort(&cp->destinations,destination_sort_handler,NULL);
     re_printf("goind to add process list call limit %d, total destinations %d \n",cp->call_limit,list_count(&cp->destinations));
  
     le = cp->destinations.head;
     while(le){   
       struct destination *dest = list_ledata(le);
       le=le->next;
      
       if(dest->try_count >0 && (time(NULL) - dest->last_try) < cp->retry_delay ){
           re_printf("destination try count %d , last try before %d , retry after val %d, can not proceed \n",dest->try_count,(time(NULL) - dest->last_try) , cp->retry_delay);
           continue;
       }
            
       list_unlink(&dest->le); 
       
          
       list_append(&cp->processl,&dest->le,dest);
       
       if(list_count(&cp->processl) >=  cp->call_limit) 
          break;
     }
   }
   
   re_printf("campaign %d process list contains %d call  , call limit %d \n",cp->campaign_id,list_count(&cp->processl),cp->call_limit);
   for(le=cp->processl.head;le;le=le->next){
         struct destination *dest = list_ledata(le);
         if(dest->call) continue;  // call already running 
         if(!dest->cp->gateway) continue;
         err = call_alloc(&dest->call,dest->dest,&dest->cp->gateway->sip_uri,false,dest);       
         if(err){         
             mem_deref(dest);
             re_printf("for error dest destoryed \n");
         }else{
             dest->try_count++;
             dest->last_try = time(NULL); 
             re_printf("sent call for dest id %d try count %d max play %d\n",dest->dest_id,dest->try_count,dest->cp->max_play); 
             if(dest->cp->file_name){      
                struct pl pl = PL_INIT;
                struct pl pt = PL_INIT;
                pl_set_str(&pl,dest->cp->file_name);
                const char *p = pl_strchr(&pl,'.');
                pt.p = pl.p;
                pt.l =  p - pl.p;
                re_sdprintf(&dest->call->audio,"%s/%r.wav",audio_path,&pt);
                if( access( dest->call->audio, F_OK ) == -1){
                   re_printf("call destoryed as audio file not found %s \n",dest->call->audio);
                  dest->call = mem_deref(dest->call);
                  
                }
                  
             }else{
                if(dest->cp->pbx_gateway && dest->cp->pbx_number){
                    dest->call->join_pbx = true;
                }else
                  dest->call = mem_deref(dest->call);
             }
             if(dest->call){
                tmr_start(&dest->tmr,1000,on_call_end,dest); 
                update_call_status(dest);
             }else
               dest->try_count--;   
         }                    
   }
          
  tmr_start(&cp->tmr,10000,start_fake_calling,cp);
}

static void campaign_service_handler(int id, void *data,void *arg){
   struct campaign *cp = data;
   cp->is_running =  true;
   tmr_init(&cp->tmr);
   re_printf("started fcg server %d %d\n",cp->campaign_id,id);
   start_fake_calling(cp);
} 


static void timer_call(void *arg){
   struct le *le;
   struct tmr *tmr = arg;
   //lock_read_get(lock);
   /*for(le=services.head;le;le=le->next){
      struct campaign *cp = list_ledata(le);
      if(cp->is_running == false){
          cp->is_running =  true;
          tmr_init(&cp->tmr);
          start_fake_calling(cp); 
        //  re_printf("started fake call service %d\n",fcg->group_id);         
      }    
         
   }   */
 // check_sdp_null(); 
   
   //lock_rel(lock);
   tmr_start(tmr,1000,timer_call,tmr);
}




int main(int argc, char *argv[])
{

  struct sa sa;
  struct conf *conf;
  struct pl pl;
  struct uri uri;
  struct db_config *db_config;
  struct tmr tmr;
  thrd_t t;
  

  
  bool run_daemon = false;
  
  int err; /* errno return values */
  
  
  /* enable coredumps to aid debugging */
	(void)sys_coredump_set(true);

	/* initialize libre state */
	err = libre_init();
	if (err) {
		re_fprintf(stderr, "re init failed: %s\n", strerror(err));
		goto out;
	}
  
 
  
  
  #ifdef HAVE_GETOPT
	for (;;) {
		const int c = getopt(argc, argv, "dv");
		if (0 > c)
			break;
      
    switch(c){
     case 'd':
       run_daemon = true;
       break;
                
     case 'v':
       re_printf("Autodialer  V. %s\n",AUTODIALER_VERSION);
       return 0;
       break;          
     default:
			 break;  
    }
  }    
  #endif
  
  
  
  char cwd[1024];
  char conf_path[1024];
  if (getcwd(cwd, sizeof(cwd)) != NULL)
    re_snprintf(conf_path,1024,"%s/autodialer.conf",cwd);
  else
   re_snprintf(conf_path,1024,"/etc/autodailer.conf");   
  
  re_printf("conf path %s \n",conf_path);
  err = conf_alloc(&conf,conf_path);
  if(err){
    re_printf("conf alloc failed . error %d \n",err);
    conf = NULL;
    goto out;
  }
  
   err = conf_get(conf,"mysql",&pl);
   if(err){
      	re_fprintf(stderr, "conf error: %s\n",
				   strerror(err));
			goto out;
   }
   uri_decode(&uri,&pl);
   //re_printf("mysql config user %r pass %r host %r \n",&uri.user,&uri.password,&uri.host);
  
  db_config = mem_zalloc(sizeof(*db_config),conf_destructor);
  pl_dup(&db_config->user,&uri.user);
  pl_dup(&db_config->pass,&uri.password);
  pl_dup(&db_config->db,&uri.scheme);
  pl_dup(&db_config->host,&uri.host);
  

  if(db_connect(&db,db_config)==true){
    re_printf("mysql connect success \n");
  }else{
    re_printf("mysql connect failed \n");
    goto out;
  }
  
  
  //db_real_exec(db,"UPDATE  `sim_groups` SET  `last_started` =  0,current_status = 0 WHERE  `current_status` = 1");
  
  err = conf_get(conf,"server",&pl);
   if(err){
      	re_fprintf(stderr, "conf error: fcg server ip not defined.\n");
			goto out;
   }
   
   err = uri_decode(&fcg_uri,&pl);
   
   if(err){
      	re_fprintf(stderr, "conf fcg server   error: \n");
			goto out;
   }
  sa_init(&sa,AF_INET); 
  //re_printf("host %r port %d \n",&fcg_uri.host,fcg_uri.port); 
  
  err  = sa_set(&sa,&fcg_uri.host,fcg_uri.port);
  if(err){
      	re_fprintf(stderr, "conf fcg server ip  error: %J \n",&sa);
			goto out;
  }
  //sa_set_port(&sa,fcg_uri.port);
  
  re_printf("fcg server trying listed at %J \n",&sa);
  
  err = conf_get(conf,"audio_path",&pl);
  if(!err){
     pl_strdup(&audio_path,&pl);
  } 
  
  
  re_thread_init();
  if(run_daemon == true){ 
    err = sys_daemon();
      if (err)
        goto out;
  }
  
  
  
  sipcall_alloc(&sa);
  
 
  list_init(&services);
  
  lock_alloc(&lock);
  
  if (thrd_create(&t, DbThread, (void*)0) == thrd_success)
  {
    re_printf("started db thread \n");
  }else
    goto out;
  
  err = mqueue_alloc(&msgq,campaign_service_handler,NULL);
  if(err)
    goto out;
  
  init_incoming(&sa);
  tmr_init(&tmr);
  tmr_start(&tmr,1000,timer_call,&tmr);
  err = re_main(signal_handler);
  
  destroy_incoming();
  thrd_join(t, NULL);
  
  
  out:
  
  re_thread_close();
  mem_deref(msgq);

//  mem_deref(sdp);
  mem_deref(db);
  mem_deref(conf);
  mem_deref(audio_path);
  mem_deref(lock);
  	/* free librar state */
	libre_close();

	/* check for memory leaks */
	tmr_debug();
	mem_debug();

	return err;

}  