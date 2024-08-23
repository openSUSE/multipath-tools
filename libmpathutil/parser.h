/*
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 *
 * Part:        cfreader.c include file.
 *
 * Version:     $Id: parser.h,v 1.0.3 2003/05/11 02:28:03 acassen Exp $
 *
 * Author:      Alexandre Cassen, <acassen@linux-vs.org>
 *
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *              See the GNU General Public License for more details.
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 */

#ifndef PARSER_H_INCLUDED
#define PARSER_H_INCLUDED

/* system includes */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <syslog.h>
#include <ctype.h>

/* local includes */
#include "vector.h"
struct strbuf;
struct config;

/* Global definitions */
#define EOB  "}"
#define MAXBUF	1024


/* keyword definition */
typedef int print_fn(struct config *, struct strbuf *, const void *);
typedef int handler_fn(struct config *, vector, const char *file, int line_nr);

struct keyword {
	char *string;
	handler_fn *handler;
	print_fn *print;
	vector sub;
	int unique;
};

/* Reloading helpers */
#define SET_RELOAD      (reload = 1)
#define UNSET_RELOAD    (reload = 0)
#define RELOAD_DELAY    5

/* iterator helper */
#define iterate_sub_keywords(k,p,i) \
	for (i = 0; i < (k)->sub->allocated && ((p) = (k)->sub->slot[i]); i++)

/* Prototypes */
int keyword_alloc(vector keywords, char *string, handler_fn *handler,
		  print_fn *print, int unique);
#define install_keyword_root(str, h) keyword_alloc(keywords, str, h, NULL, 1)
void install_sublevel(void);
void install_sublevel_end(void);

int install_keyword__(vector keywords, char *string, handler_fn *handler,
		      print_fn *print, int unique);
#define install_keyword(str, vec, pri) install_keyword__(keywords, str, vec, pri, 1)
#define install_keyword_multi(str, vec, pri) install_keyword__(keywords, str, vec, pri, 0)
void dump_keywords(vector keydump, int level);
void free_keywords(vector keywords);
vector alloc_strvec(char *string);
void *set_value(vector strvec);
int process_file(struct config *conf, const char *conf_file);
struct keyword * find_keyword(vector keywords, vector v, char * name);
int snprint_keyword(struct strbuf *buff, const char *fmt, struct keyword *kw,
		    const void *data);
bool is_quote(const char* token);

#endif
