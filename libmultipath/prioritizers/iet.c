#include <stdlib.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <regex.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include "prio.h"
#include "debug.h"
#include "mt-udev-wrap.h"
#include "structs.h"
#include "util.h"

//
// This prioritizer allows path selection based on target's IP address.
//
// (It's a bit of a misnomer since supports the client side [eg. open-iscsi]
//  instead of just "iet".)
//
// Usage:
//   prio      "iet"
//   with
//   prio_args "preferredip=<IP address>"
//		assigns priority 20 (high) to the preferred IP and priority 10 (low) to the rest
//   or
//   prio_args "preferredip=<IP|CIDR>:Prio,<IP|CIDR>:Prio,..."
//		IP can be specified directly or in CIDR notation (IP/32)
//   	CIDR can use standard n.n.n.n/p format or any valid shorthand
//   	0.0.0.0/0 to set the "no match" priority (defaults to 0)
//
//
// Matching follows network routing semantics (more specific match wins)
//
// by Olivier Lambert <lambert.olivier.gmail.com>
// Multiple priorities added by Arnaldo Viegas de Lima (arnaldo.viegasdelima.com)
//

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#define dc_log(prio, fmt, ...) condlog(prio, "%s " fmt, sysname, __VA_ARGS__)
#else /* GCC extension */
#define dc_log(prio, fmt, ...) condlog(prio, "%s " fmt, sysname, ##__VA_ARGS__)
#endif

#define DEFAULT_PRIORITY 0
#define DEFAULT_HIGH_PRIORITY 20

//
// Entry for the CIDR to priority list
struct ipprio_entry {
	uint32_t network;
	uint32_t mask;
	int priority;
	struct ipprio_entry *next;
};

//
// name: find_regex
// @param string: string you want to search into
// @param regex: the pattern used
// @return result: string found in string with regex, "none" if none
char *find_regex(const char *string, const char *regex)
{
	int err;
	regex_t preg;
	err = regcomp(&preg, regex, REG_EXTENDED);

	if (err == 0) {
		int match;
		size_t nmatch = 0;
		regmatch_t *pmatch = NULL;
		nmatch = preg.re_nsub;
		pmatch = malloc(sizeof(*pmatch) * nmatch);

		if (pmatch) {
			match = regexec(&preg, string, nmatch, pmatch, 0);
			regfree(&preg);

			if (match == 0) {
				char *result = NULL;
				int start = pmatch[0].rm_so;
				int end = pmatch[0].rm_eo;
				size_t size = end - start;
				result = malloc(sizeof(*result) * (size + 1));

				if (result) {
					strlcpy(result, &string[start], size + 1);
					;
					free(pmatch);
					return result;
				}
			}
			free(pmatch);
		}
	}
	return NULL;
}

//
// name: prefix_to_mask
// @param prefix: CIDR prefix (# of bits)
// @return: corresponding network mask (defaults to 32 bits)
static uint32_t prefix_to_mask(int prefix)
{
	if (prefix <= 0)
		return 0;
	if (prefix >= 32)
		return 0xffffffffU;
	return 0xffffffffU << (32 - prefix);
}

//
// name: parse_cidr
// @param s: string containing CIDR block or single IP
// @return network: network as integer
// @return mask: mask as integer
static int parse_cidr(const char *s, uint32_t *network, uint32_t *mask)
{
	char *slash;
	char *endptr;
	long val;
	int prefix;
	unsigned int o[4] = { 0 };
	char extra;
	int count;

	slash = strchr(s, '/');

	if (slash) {

		*slash++ = '\0';

		errno = 0;
		val = strtol(slash, &endptr, 10);

		if (errno != 0 || endptr == slash || *endptr != '\0' ||
		    val < 0 || val > 32)
			return -1;

		prefix = (int)val;

	} else
		/* no CIDR suffix -> exact host match */
		prefix = 32;

	count = sscanf(s, "%u.%u.%u.%u%c", &o[0], &o[1], &o[2], &o[3], &extra);
	// Validate shorthand.
	// prefix > count*8: the address part of the shorthand must contain
	//    at least the same number of bits as set by prefix.
	if (count < 1 || count > 4 || o[0] > 255 || o[1] > 255 || o[2] > 255 ||
	    o[3] > 255 || prefix > 8 * count)
		return -1;

	*mask = prefix_to_mask(prefix);
	*network = ((o[0] << 24) | (o[1] << 16) | (o[2] << 8) | o[3]) & *mask;

	return 0;
}

//
// name: free_ipprio_list
// @param list: pointer to list to free
// @return: void
static void free_ipprio_list(struct ipprio_entry *list)
{
	struct ipprio_entry *next;
	while (list) {
		next = list->next;
		free(list);
		list = next;
	}
}

//
// name: parse_ippriorities
// @param args: full prio_args string
// @return: 0 if ok -1 if not
static struct ipprio_entry *parse_ippriorities(char *buf)
{
	char *entry, *saveptr = NULL;
	struct ipprio_entry *ipprio_list = NULL;
	bool not_first = false;

	entry = strtok_r(buf, ",", &saveptr);

	while (entry) {
		char *colon;
		char *cidr;
		char *prio_str;
		char *endptr;
		uint32_t network = 0;
		uint32_t mask = 0;
		long val;
		int priority;
		struct ipprio_entry *e;

		/* A single IP without CIDR mask and priority attain backward compatibility */
		colon = strrchr(entry, ':');

		if (!colon) {
			if (not_first || strchr(entry, '/') != NULL ||
			    strtok_r(NULL, ",", &saveptr) != NULL)
				goto error_cleanup;

			cidr = entry;
			priority = DEFAULT_HIGH_PRIORITY;
			entry = NULL; /* consume the only entry */
		} else {
			*colon = '\0';
			cidr = entry;
			prio_str = colon + 1;

			if (*cidr == '\0' || *prio_str == '\0')
				goto error_cleanup;

			errno = 0;
			val = strtol(prio_str, &endptr, 10);

			if (errno != 0 || endptr == prio_str ||
			    *endptr != '\0' || val == 0 || val > INT_MAX)
				goto error_cleanup;

			priority = (int)val;
		}

		if (parse_cidr(cidr, &network, &mask) != 0)
			goto error_cleanup;

		e = calloc(1, sizeof(*e));
		if (!e)
			goto error_cleanup;

		e->network = network;
		e->mask = mask;
		e->priority = priority;
		e->next = ipprio_list;
		ipprio_list = e;

		not_first = true;

		if (entry)
			entry = strtok_r(NULL, ",", &saveptr);
	}

	return ipprio_list;

error_cleanup:
	free_ipprio_list(ipprio_list);
	return NULL;
}

//
// name: find_priority - lookup IP in list of CIDRs
// @param ipstr: IP to lookup as a string
// @param ipprio_list: list of CIDR blocks for the search
// @return: priority
static int find_priority(const char *sysname, const char *ipstr,
			 struct ipprio_entry *ipprio_list)
{
	struct ipprio_entry *e;
	struct in_addr addr;
	struct in_addr netaddr = { 0 };
	struct in_addr maskaddr = { 0 };

	uint32_t ip;
	int best_prio = DEFAULT_PRIORITY;
	int best_prefix = -1;

	if (!ipstr) {
		dc_log(3, "find_priority: NULL IP, returning default=%d",
		       DEFAULT_PRIORITY);
		return DEFAULT_PRIORITY;
	}

	if (inet_pton(AF_INET, ipstr, &addr) != 1) {
		dc_log(3, "find_priority: invalid IP '%s', returning default=%d",
		       ipstr, DEFAULT_PRIORITY);
		return DEFAULT_PRIORITY;
	}

	ip = ntohl(addr.s_addr);

	for (e = ipprio_list; e; e = e->next) {
		char net_buf[INET_ADDRSTRLEN];
		char mask_buf[INET_ADDRSTRLEN];

		int prefix = __builtin_popcount(e->mask);

		if (libmp_verbosity > 3) {
			netaddr.s_addr = htonl(e->network);
			maskaddr.s_addr = htonl(e->mask);
			inet_ntop(AF_INET, &netaddr, net_buf, sizeof(net_buf));
			inet_ntop(AF_INET, &maskaddr, mask_buf, sizeof(mask_buf));
		}

		if ((ip & e->mask) == e->network) {

			dc_log(4, "find_priority: match ip=%s network=%s mask=%s priority=%d",
			       ipstr, net_buf, mask_buf, e->priority);

			if (prefix > best_prefix) {
				best_prefix = prefix;
				best_prio = e->priority;

				dc_log(4, "find_priority: selected priority=%d for ip=%s",
				       best_prio, ipstr);
			}

			if (prefix == 32)
				break;

		} else
			dc_log(4, "find_priority: no match ip=%s network=%s mask=%s",
			       ipstr, net_buf, mask_buf);
	}

	dc_log(4, "find_priority: final priority for %s is %d", ipstr, best_prio);

	return best_prio;
}

//
// name: inet_prio
// @param
// @return prio
int iet_prio(struct udev_device *udev, char *args)
{
	char preferredip_buff[256] = "";
	char *preferredip = &preferredip_buff[0];
	const char *by_path;
	char *ip;
	static bool arg_logged = false;
	int prio = 10;
	const char *sysname;
	struct ipprio_entry *ipprio_list = NULL;

	if (!udev)
		return 0;
	sysname = udev_device_get_sysname(udev);
	if (!sysname)
		sysname = "UNKNOWN";

	// Phase 1 : checks. If anyone fails, return prio 0.
	// check if args exists
	if (!args) {
		if (!arg_logged) {
			dc_log(2, "need prio_args with preferredip set");
			arg_logged = true;
		}
		return 0;
	}

	// check args format and initialize list of CIDR/IPs
	if (sscanf(args, "preferredip=%255s", preferredip) == 1) {

		ipprio_list = parse_ippriorities(preferredip);
		if (!ipprio_list) {
			if (!arg_logged) {
				dc_log(2, "invalid prio_args preferredip");
				arg_logged = true;
			}
			return 0;
		}

	} else {

		if (!arg_logged) {
			dc_log(2, "unexpected prio_args format");
			arg_logged = true;
		}
		return 0;
	}

	// Phase 2 : get udev IP and check against list
	by_path = udev_device_get_property_value(udev, "ID_PATH");
	if (by_path == NULL) {
		dc_log(2, "failed to get BY_PATH property");
		free_ipprio_list(ipprio_list);
		return 0;
	}
	condlog(3, "%s: iet prio: by_path=%s", sysname, by_path);
	ip = find_regex(by_path,
			"([0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3})");

	if (ip)
		prio = find_priority(sysname, ip, ipprio_list);
	else
		prio = DEFAULT_PRIORITY;

	free(ip);
	free_ipprio_list(ipprio_list);
	return prio;
}

//
// Prioritizer entry point
int getprio(struct path *pp, char *args)
{
	return iet_prio(pp->udev, args);
}
