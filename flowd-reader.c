/*
 * Copyright (c) 2004 Damien Miller <djm@mindrot.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <poll.h>

#include "store.h"
#include "atomicio.h"

RCSID("$Id$");

#define PROGNAME	"flowd-reader"

static void
usage(void)
{
	fprintf(stderr, "Usage: %s [options] flow-log [flow-log ...]\n",
	    PROGNAME);
	fprintf(stderr, "This is %s version %s. Valid commandline options:\n",
	    PROGNAME, PROGVER);
	fprintf(stderr, "  -v    Display all available flow information\n");
	fprintf(stderr, "  -U    Report times in UTC rather than local time\n");
	fprintf(stderr, "  -h    Display this help\n");
}

int
main(int argc, char **argv)
{
	int ch, i, fd, utc, r, verbose;
	extern char *optarg;
	extern int optind;
	struct store_flow_complete flow;
	struct store_header hdr;
	const char *e;
	char buf[2048];

	utc = verbose = 0;
	while ((ch = getopt(argc, argv, "Uhv")) != -1) {
		switch (ch) {
		case 'h':
			usage();
			return (0);
		case 'U':
			utc = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		default:
			fprintf(stderr, "Invalid commandline option.\n");
			usage();
			exit(1);
		}
	}

	if (argc - optind < 1) {
		fprintf(stderr, "No logfile specified\n");
		usage();
		exit(1);
	}

	for (i = optind; i < argc; i++) {
		if ((fd = open(argv[i], O_RDONLY)) == -1) {
			fprintf(stderr, "Couldn't open %s\n", argv[i]);
			exit(1);
		}
		if (store_get_header(fd, &hdr, &e) == -1) {
			fprintf(stderr, "%s\n", e);
			exit(1);
		}
	
		printf("LOGFILE %s started at %s\n", argv[i],
		    iso_time(ntohl(hdr.start_time), utc));

		for (;;) {
			bzero(&flow, sizeof(flow));

			if ((r = store_get_flow(fd, &flow, &e)) == -1) {
				fprintf(stderr, "%s\n", e);
				exit(1);
			}
			if (r == 0) /* EOF */
				break;

			store_format_flow(&flow, buf, sizeof(buf), utc,
			    verbose ? STORE_DISPLAY_ALL : STORE_DISPLAY_BRIEF);
			printf("%s\n", buf);
			fflush(stdout);
		}
		close(fd);
	}

	return (0);
}
