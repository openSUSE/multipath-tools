#include <stdlib.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <regex.h>
#include <libudev.h>
#include "prio.h"
#include "debug.h"
#include <unistd.h>
#include "structs.h"

//
// This prioritizer suits iSCSI needs, makes it possible to prefer one path.
//
// (It's a bit of a misnomer since supports the client side [eg. open-iscsi]
//  instead of just "iet".)
//
// Usage:
//   prio      "iet"
//   prio_args "preferredip=10.11.12.13"
//
// Assigns prio 20 (high) to the preferred IP and prio 10 (low) to the rest.
//
// by Olivier Lambert <lambert.olivier.gmail.com>
//

#define dc_log(prio, msg) condlog(prio, "%s: iet prio: " msg, sysname)

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
				result = malloc (sizeof(*result) * (size + 1));

				if (result) {
					strncpy(result, &string[start], size);
					result[size] = '\0';
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
// name: inet_prio
// @param
// @return prio
int iet_prio(struct udev_device *udev, char *args)
{
	char preferredip_buff[255] = "";
	char *preferredip = &preferredip_buff[0];
	const char *by_path;
	char *ip;
	static bool arg_logged = false;
	int prio = 10;
	const char *sysname;

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
	// check if args format is OK
	if (sscanf(args, "preferredip=%254s", preferredip) != 1) {
		if (!arg_logged) {
			dc_log(2, "unexpected prio_args format");
			arg_logged = true;
		}
		return 0;
	}
	// check if ip is not too short
	if (strlen(preferredip) <= 7) {
		if (!arg_logged) {
			dc_log(2, "prio args: preferredip too short");
			arg_logged = true;
		}
		return 0;
	}

	by_path = udev_device_get_property_value(udev, "ID_PATH");
	if (by_path == NULL) {
		dc_log(2, "failed to get BY_PATH property");
		return 0;
	}
	condlog(3, "%s: iet prio: by_path=%s", sysname, by_path);
	ip = find_regex(by_path,
			"([0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3})");
	if (ip != NULL && strncmp(ip, preferredip, strlen(ip)) == 0)
		prio = 20;

	free(ip);
	return prio;
}

int getprio(struct path * pp, char * args)
{
	return iet_prio(pp->udev, args);
}
