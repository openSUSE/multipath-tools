#ifndef _ALIAS_H
#define _ALIAS_H

int valid_alias(const char *alias);
int get_user_friendly_wwid(const char *alias, char *buff, const char *file);
char *get_user_friendly_alias(const char *wwid, const char *file,
			      const char *alias_old,
			      const char *prefix, bool bindings_read_only);

struct config;
int check_alias_settings(const struct config *);

#endif /* _ALIAS_H */
