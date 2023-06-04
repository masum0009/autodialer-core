#ifndef _DB_H
#define _DB_H

#include <string.h>
#include <my_global.h>
#include <mysql.h>


struct db_config{
  struct pl user;
  struct pl pass;
  struct pl host;
  struct pl db;
};

struct db_con{
  struct db_config *config;
  MYSQL *con;
};

struct db_field{
  struct pl field;
  void *data;
  struct le le;
};







bool db_connect(struct db_con **conn,struct db_config *config);
bool db_reconnect(struct db_con *con);
bool db_exec(struct db_con *con,const char *fmt, ...);
bool db_real_exec(struct db_con *con,const char *fmt, ...);
void db_sync(struct db_con *con);
MYSQL_RES *get_results(struct db_con *con,const char *fmt, ...);
bool get_field(struct db_con *con,struct pl *val,int field,const char *fmt, ...);




#endif /* db.h */