/*  Copyright (C) 2024 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <getopt.h>
#include <libgen.h>
#include <stdio.h>

#include "contrib/strtonum.h"
#include "contrib/time.h"
#include "contrib/tolower.h"
#include "libknot/libknot.h"
#include "knot/common/log.h"
#include "knot/zone/semantic-check.h"
#include "knot/zone/zone-load.h"
#include "utils/common/msg.h"
#include "utils/common/params.h"
#include "utils/kzonecheck/zone_check.h"

#define PROGRAM_NAME "kzonecheck"

#define STDIN_SUBST "-"
#define STDIN_REPL "/dev/stdin"

static void print_help(void)
{
	printf("Usage: %s [options] <filename>\n"
	       "\n"
	       "Options:\n"
	       " -o, --origin <zone_origin>  Zone name.\n"
	       "                              (default filename without .zone)\n"
	       " -d, --dnssec <on|off>       Enforce check of DNSSEC records.\n"
	       "                              (default autodetection)\n"
	       " -j, --jobs <num>            Number of threads.\n"
	       "                              (default all CPU threads available)\n"
	       " -z, --zonemd                Also check ZONEMD.\n"
	       " -t, --time <timestamp>      Current time specification.\n"
	       "                              (default current UNIX time)\n"
	       " -p, --print                 Print the zone on stdout.\n"
	       " -v, --verbose               Enable debug output.\n"
	       " -h, --help                  Print the program help.\n"
	       " -V, --version               Print the program version.\n",
	       PROGRAM_NAME);
}

static bool str2bool(const char *s)
{
	switch (knot_tolower(s[0])) {
	case '1':
	case 'y':
	case 't':
		return true;
	case 'o':
		return knot_tolower(s[1]) == 'n';
	default:
		return false;
	}
}

int main(int argc, char *argv[])
{
	const char *origin = NULL;
	bool zonemd = false, verbose = false, print = false;
	uint16_t threads = 0;
	semcheck_optional_t optional = SEMCHECK_DNSSEC_AUTO; // default value for --dnssec
	knot_time_t check_time = (knot_time_t)time(NULL);

	/* Long options. */
	struct option opts[] = {
		{ "origin",  required_argument, NULL, 'o' },
		{ "time",    required_argument, NULL, 't' },
		{ "dnssec",  required_argument, NULL, 'd' },
		{ "jobs",    required_argument, NULL, 'j' },
		{ "zonemd",  no_argument,       NULL, 'z' },
		{ "print",   no_argument,       NULL, 'p' },
		{ "verbose", no_argument,       NULL, 'v' },
		{ "help",    no_argument,       NULL, 'h' },
		{ "version", optional_argument, NULL, 'V' },
		{ NULL }
	};

	/* Set the time zone. */
	tzset();

	/* Parse command line arguments */
	int opt = 0;
	while ((opt = getopt_long(argc, argv, "o:t:d:j:zpvV::h", opts, NULL)) != -1) {
		switch (opt) {
		case 'o':
			origin = optarg;
			break;
		case 'p':
			print = true;
			break;
		case 'j':
			if (str_to_u16(optarg, &threads) != KNOT_EOK) {
				ERR2("invalid number of threads '%s'", optarg);
				return EXIT_FAILURE;
			}
			break;
		case 'v':
			verbose = true;
			break;
		case 'h':
			print_help();
			return EXIT_SUCCESS;
		case 'V':
			print_version(PROGRAM_NAME, optarg != NULL);
			return EXIT_SUCCESS;
		case 'd':
			optional = str2bool(optarg) ? SEMCHECK_DNSSEC_ON : SEMCHECK_DNSSEC_OFF;
			break;
		case 'z':
			zonemd = true;
			break;
		case 't':
			if (knot_time_parse("YMDhms|#|+-#U|+-#",
			                    optarg, &check_time) != KNOT_EOK) {
				ERR2("unknown time format");
				return EXIT_FAILURE;
			}
			break;
		default:
			print_help();
			return EXIT_FAILURE;
		}
	}

	/* Check if there's at least one remaining non-option. */
	if (optind >= argc) {
		ERR2("expected zone file name");
		print_help();
		return EXIT_FAILURE;
	}

	char *filename = argv[optind];
	if (strncmp(filename, STDIN_SUBST, sizeof(STDIN_SUBST)) == 0) {
		filename = STDIN_REPL;
	}

	char *zonename;
	if (origin == NULL) {
		/* Get zone name from file name. */
		const char *ext = ".zone";
		zonename = basename(filename);
		if (strcmp(zonename + strlen(zonename) - strlen(ext), ext) == 0) {
			zonename = strndup(zonename, strlen(zonename) - strlen(ext));
		} else {
			zonename = strdup(zonename);
		}
	} else {
		zonename = strdup(origin);
	}

	knot_dname_storage_t zone;
	if (knot_dname_from_str(zone, zonename, sizeof(zone)) == NULL) {
		ERR2("invalid zone name");
		free(zonename);
		return EXIT_FAILURE;
	}
	free(zonename);
	knot_dname_to_lower(zone);

	log_init();
	log_levels_set(LOG_TARGET_STDOUT, LOG_SOURCE_ANY, 0);
	log_levels_set(LOG_TARGET_STDERR, LOG_SOURCE_ANY, 0);
	log_levels_set(LOG_TARGET_SYSLOG, LOG_SOURCE_ANY, 0);
	log_flag_set(LOG_FLAG_NOTIMESTAMP | LOG_FLAG_NOINFO);
	if (verbose) {
		log_levels_add(LOG_TARGET_STDOUT, LOG_SOURCE_ANY, LOG_UPTO(LOG_DEBUG));
	}

	int ret = zone_check(filename, zone, zonemd, DEFAULT_TTL, optional,
	                     (time_t)check_time, print, threads);
	log_close();
	if (ret == KNOT_EOK) {
		if (verbose && !print) {
			INFO2("No error found");
		}
		return EXIT_SUCCESS;
	} else {
		return EXIT_FAILURE;
	}
}
