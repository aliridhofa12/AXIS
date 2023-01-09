#include <syslog.h>
int main(int argc, char **argv)

{
  /* Open the syslog to report messages for "hello_world" */
  openlog("hello_world", LOG_PID | LOG_CONS, LOG_USER);

  /* Choose between { LOG_INFO, LOG_CRIT, LOG_WARN, LOG_ERR }*/
  syslog(LOG_INFO, "Hello World!");

  /* Close application logging to syslog */
  closelog();

  return 0;
}