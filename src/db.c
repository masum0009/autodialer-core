/* ========================================================================== */
/*                                                                            */
/*   Filename.c                                                               */
/*   (c) 2001 Author                                                          */
/*                                                                            */
/*   Description                                                              */
/*                                                                            */
/* ========================================================================== */
#include <re.h>
#include "tinycthread.h"
#include "db.h"

static struct list sqllist;
mtx_t gMutex;

struct sqlquery{
 char *sql;
 struct le le;
};


static void query_destructor(void *data){
  struct sqlquery *rc = data;
  mem_deref(rc->sql);
}

static void db_destructor(void *data)
{
	struct db_con *rc = data;
	mem_deref(rc->config);
	mysql_close(rc->con);
	list_flush(&sqllist);
	mtx_destroy(&gMutex);
}

 

bool db_connect(struct db_con **conn,struct db_config *config){
     struct db_con *con;
     char *user;
     char *pass;
     char *db;
     char *host;
     int err = 0;
     pl_strdup(&user,&config->user);
     pl_strdup(&pass,&config->pass);
     pl_strdup(&db,&config->db);
     pl_strdup(&host,&config->host);
     
     con = mem_zalloc(sizeof(*con),db_destructor);
     
     con->con = mysql_init(NULL);

      if (con->con == NULL) 
      {
          fprintf(stderr, "%s\n", mysql_error(con->con));
          err = 1;
          goto out;
          
      }
      if (mysql_real_connect(con->con, host, user, pass, 
          db, 0, NULL, 0) == NULL) 
      {
          fprintf(stderr, "%s\n", mysql_error(con->con));
          mysql_close(con->con);
          err = 1;
          goto out;
          
      }
      
      con->config = config;
      *conn = con;
      list_init(&sqllist);
      mtx_init(&gMutex, mtx_plain);
      
      out:
      mem_deref(user);
      mem_deref(pass);
      mem_deref(host);
      mem_deref(db);
      if(err) return false;
      else return true;
         
}

bool db_reconnect(struct db_con *con){
     
     char *user;
     char *pass;
     char *db;
     char *host;
     int err = 0;
     pl_strdup(&user,&con->config->user);
     pl_strdup(&pass,&con->config->pass);
     pl_strdup(&db,&con->config->db);
     pl_strdup(&host,&con->config->host);
     
    // con = mem_zalloc(sizeof(*con),db_destructor);
     
     con->con = mysql_init(NULL);

      if (con->con == NULL) 
      {
          fprintf(stderr, "%s\n", mysql_error(con->con));
          err = 1;
          goto out;
          
      }
      if (mysql_real_connect(con->con, host, user, pass, 
          db, 0, NULL, 0) == NULL) 
      {
          fprintf(stderr, "%s\n", mysql_error(con->con));
          mysql_close(con->con);
          err = 1;
          goto out;
          
      }
      
     // con->config = config;
      
      
      
      
      out:
      mem_deref(user);
      mem_deref(pass);
      mem_deref(host);
      mem_deref(db);
      if(err) return false;
      else return true;
         
}


MYSQL_RES *get_results(struct db_con *con,const char *fmt, ...){
  va_list ap;
	int err;
  char *strp;
  MYSQL_RES *result;
	va_start(ap, fmt);
	err = re_vsdprintf(&strp, fmt, ap);
	//re_printf("here query %s\n",strp);
	 if (mysql_query(con->con, strp)) 
      {
          re_printf("error in query %s \n",strp);
          mem_deref(strp);
          return NULL;
      }
    
  result = mysql_store_result(con->con);
	
	va_end(ap);
  mem_deref(strp);
	return result;
}


bool db_exec(struct db_con *con,const char *fmt, ...){
  va_list ap;
	int err;
  char *strp;
  va_start(ap, fmt);
  
	err = re_vsdprintf(&strp, fmt, ap);
	if(err == 0){
	//re_printf("here query %s\n",strp);
	struct sqlquery *query;
	query = mem_zalloc(sizeof(*query),query_destructor);
	query->sql = strp; 
	mtx_lock(&gMutex);
	list_append(&sqllist,&query->le,query);
	mtx_unlock(&gMutex);
	}
  /*if (mysql_query(con->con, strp)) 
      {
          re_printf("error in query %s . \n",strp);
         
          fprintf(stderr, "%d %s\n", mysql_errno(con->con),mysql_error(con->con));
          mem_deref(strp);
          return false;
      }
  */    
  va_end(ap);
  //mem_deref(strp);    
  return err;    

}

bool db_real_exec(struct db_con *con,const char *fmt, ...){
  va_list ap;
	int err;
  char *strp;
  va_start(ap, fmt);
	err = re_vsdprintf(&strp, fmt, ap);
	va_end(ap);
  
  if (mysql_query(con->con, strp)) 
      {
          re_printf("error in query %s . \n",strp);
         
          fprintf(stderr, "%d %s\n", mysql_errno(con->con),mysql_error(con->con));
          mem_deref(strp);
          return false;
      }
      
  mem_deref(strp);    
  return true;    

}

bool get_field(struct db_con *con,struct pl *val,int field,const char *fmt, ...){

  va_list ap;
	int err;
  char *strp;
  
  va_start(ap, fmt);
	err = re_vsdprintf(&strp, fmt, ap);
	if(err == 0){
	//re_printf("here query %s\n",strp);
	
	}
	if (mysql_query(con->con, strp)) 
      {
          re_printf("error in query %s . \n",strp);
         
          fprintf(stderr, "%d %s\n", mysql_errno(con->con),mysql_error(con->con));
          mem_deref(strp);
          return false;
      }
      
  va_end(ap);
  mem_deref(strp);
  
   MYSQL_RES *results;
   results = mysql_store_result(con->con);
   MYSQL_ROW row;
   if(row = mysql_fetch_row(results)){
     pl_set_str(val,row[field]);  
   }

  mysql_free_result(results);
	return true;

}


void db_sync(struct db_con *con){
  struct le *le;
  
  mtx_lock(&gMutex);
  le = sqllist.head;
  list_init(&sqllist);
  mtx_unlock(&gMutex);
  
   if (mysql_query(con->con, "SELECT 1")){
      re_printf("try reconnect here \n");
      mysql_close(con->con);
      con->con = NULL;
      if(db_reconnect(con)==false){
         re_printf("try reconnect failed \n");
         return;
      }
   }else{
    MYSQL_RES *results;
    results = mysql_store_result(con->con);
    mysql_free_result(results);
   }
   //re_printf("syncing db here total query %zu \n",list_count(&sqllist));
  while(le){
    struct sqlquery *query = list_ledata(le);
    
    if (mysql_query(con->con, query->sql)) 
      {
          re_printf("error in query %s . \n",query->sql);
         
          fprintf(stderr, "%d %s\n", mysql_errno(con->con),mysql_error(con->con));
         
      }
     
   
    le = le->next;
    //list_unlink(&query->le);
     mem_deref(query);
  }

}


/*
void db_sync(struct db_con *con){
  struct le *le;
  struct mbuf *mb;
  size_t size = 0;
   if (mysql_query(con->con, "SELECT 1")){
      re_printf("try reconnect here \n");
      mysql_close(con->con);
      con->con = NULL;
      if(db_reconnect(con)==false){
         re_printf("try reconnect failed \n");
         return;
      }
   }else{
    MYSQL_RES *results;
    results = mysql_store_result(con->con);
    mysql_free_result(results);
   }
   mb = mbuf_alloc(10);
  
  le = sqllist.head;
  re_printf("syncing db here total query %zu \n",list_count(&sqllist));
  while(le){
    struct sqlquery *query = list_ledata(le);
    size += str_len(query->sql) + 1;
    mbuf_resize(mb,size);
    mbuf_write_str(mb,query->sql);
    mbuf_write_str(mb,";"); 
   
    le = le->next;
    list_unlink(&query->le);
    mem_deref(query);
  }
  mbuf_set_pos(mb,0);
  if(mbuf_get_left(mb) >0){
  
  char buf[size];
  
  //mbuf_resize(mb,size);
  //mbuf_trim(mb);
  mbuf_read_str(mb,buf,size);
  buf[size] = '\0';
  re_printf(" query to exec %s . \n",buf);
  //mysql_query(con->con,"LOCK TABLES active_calls WRITE;LOCK TABLES failed_calls WRITE;LOCK TABLES s_calls WRITE;");
  if (mysql_query(con->con, buf)) 
      {
          re_printf("error in query %s . \n",buf);
         
          fprintf(stderr, "%d %s\n", mysql_errno(con->con),mysql_error(con->con));
          //mem_deref(strp);
          
          //return false;
      }
   //mysql_query(con->con,"UNLOCK TABLES;");   
      
   }
   mem_deref(mb);   
      
      

}
*/




