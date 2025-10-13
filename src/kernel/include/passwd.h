#ifndef PASSWD_H
#define PASSWD_H

int password_cmp(const char* passwd);
int setup_passwd(void);
void destroy_passwd(void);

#endif