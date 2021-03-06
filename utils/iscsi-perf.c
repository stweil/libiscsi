/*
   Copyright (C) 2014 by Peter Lieven <pl@kamp.de>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <poll.h>
#include <getopt.h>
#include <time.h>
#include <signal.h>
#include "iscsi.h"
#include "scsi-lowlevel.h"

#ifndef HAVE_CLOCK_GETTIME
#include <sys/time.h>
#endif

#define VERSION "0.1"

const char *initiator = "iqn.2010-11.libiscsi:iscsi-perf";
int max_in_flight = 32;
int blocks_per_io = 8;
uint64_t runtime = 0;
uint64_t finished = 0;

struct client {
	int finished;
	int in_flight;
	int random;

	struct iscsi_context *iscsi;
	int lun;
	int blocksize;
	uint64_t num_blocks;
	uint64_t pos;
	uint64_t last_ns;
	uint64_t first_ns;
	uint64_t iops;
	uint64_t last_iops;

	struct iscsi_context *dst_iscsi;
	int ignore_errors;
};

u_int64_t get_clock_ns(void) {
	int res;
	u_int64_t ns;

#ifdef HAVE_CLOCK_GETTIME
	struct timespec ts;
	res = clock_gettime (CLOCK_MONOTONIC, &tp);
	ns = ts.tv_sec * 1000000000 + ts.tv_nsec;
#else
	struct timeval tv;
	res = gettimeofday(&tv, NULL);
	ns = tv.tv_sec * 1000000000 + tv.tv_usec * 1000;
#endif
	if (res == -1) {
		fprintf(stderr,"could not get requested clock\n");
		exit(10);
	}
	return ns;
}

void fill_read_queue(struct client *client);

void progress(struct client *client) {
	uint64_t now = get_clock_ns();
	if (now - client->last_ns < 1000000000) return;

	uint64_t _runtime = (now - client->first_ns) / 1000000000UL;
	if (runtime) _runtime = runtime - _runtime;

	printf ("\r");
	uint64_t aiops = 1000000000UL * (client->iops) / (now - client->first_ns);
	if (!_runtime) {
		finished = 1;
		printf ("iops average %" PRIu64 " (%" PRIu64 " MB/s)                                                        ", aiops, (aiops * blocks_per_io * client->blocksize) >> 20);
	} else {
		uint64_t iops = 1000000000UL * (client->iops - client->last_iops) / (now - client->last_ns);
		printf ("%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64 " - ", (_runtime % 3600) / 60, _runtime / 60, _runtime % 60);
		printf ("lba %" PRIu64 ", iops current %" PRIu64 " (%" PRIu64 " MB/s), ", client->pos, iops, (iops * blocks_per_io * client->blocksize) >> 20);
		printf ("iops average %" PRIu64 " (%" PRIu64 " MB/s)         ", aiops, (aiops * blocks_per_io * client->blocksize) >> 20);
	}
	fflush(stdout);
	client->last_ns = now;
	client->last_iops = client->iops;
}

void cb(struct iscsi_context *iscsi _U_, int status, void *command_data, void *private_data)
{
	struct client *client = (struct client *)private_data;
	struct scsi_task *task = command_data;

	if (status == SCSI_STATUS_CHECK_CONDITION) {
		printf("Read10/16 failed with sense key:%d ascq:%04x\n", task->sense.key, task->sense.ascq);
		scsi_free_scsi_task(task);
		if (!client->ignore_errors) {
			exit(10);
		}
	}

	scsi_free_scsi_task(task);

	if (status != SCSI_STATUS_GOOD) {
		printf("Read10/16 failed with %s\n", iscsi_get_error(iscsi));
		if (!client->ignore_errors) {
			exit(10);
		}
	}

	client->iops++;
	client->in_flight--;
	progress(client);
	fill_read_queue(client);
}


void fill_read_queue(struct client *client)
{
	int num_blocks;

	if (finished) return;

	if (client->pos >= client->num_blocks) client->pos = 0;
	while(client->in_flight < max_in_flight && client->pos < client->num_blocks) {
		struct scsi_task *task;
		client->in_flight++;

		if (client->random) {
			client->pos = rand() % client->num_blocks;
		}

		num_blocks = client->num_blocks - client->pos;
		if (num_blocks > blocks_per_io) {
			num_blocks = blocks_per_io;
		}

		task = iscsi_read16_task(client->iscsi,
								client->lun, client->pos,
								num_blocks * client->blocksize,
								client->blocksize, 0, 0, 0, 0, 0,
								cb, client);

		if (task == NULL) {
			printf("failed to send read16 command\n");
			exit(10);
		}
		client->pos += num_blocks;
	}
}

void usage(void) {
	fprintf(stderr,"Usage: iscsi-perf [-i <initiator-name>] [-m <max_requests>] [-b blocks_per_request] [-t timeout] [-r|--random] [-n|--ignore-errors] <LUN>\n");
	exit(1);
}

void sig_handler (int signum _U_) {
	finished = 1;
}

int main(int argc, char *argv[])
{
	char *url = NULL;
	struct iscsi_url *iscsi_url;
	struct scsi_task *task;
	struct scsi_readcapacity16 *rc16;
	int c;
	struct pollfd pfd[1];
	struct client client;

	static struct option long_options[] = {
		{"initiator-name", required_argument,    NULL,        'i'},
		{"max",            required_argument,    NULL,        'm'},
		{"blocks",         required_argument,    NULL,        'b'},
		{"runtime",        required_argument,    NULL,        't'},
		{"random",         no_argument,          NULL,        'r'},
		{"ignore-errors",  no_argument,          NULL,        'n'},
		{0, 0, 0, 0}
	};
	int option_index;

	memset(&client, 0, sizeof(client));

	srand(time(NULL));

	printf("iscsi-perf version %s - (c) 2014 by Peter Lieven <pl@ĸamp.de>\n\n", VERSION);

	while ((c = getopt_long(argc, argv, "i:m:b:t:nr", long_options,
			&option_index)) != -1) {
		switch (c) {
		case 'i':
			initiator = optarg;
			break;
		case 'm':
			max_in_flight = atoi(optarg);
			break;
		case 't':
			runtime = atoi(optarg);
			break;
		case 'b':
			blocks_per_io = atoi(optarg);
			break;
		case 'n':
			client.ignore_errors = 1;
			break;
		case 'r':
			client.random = 1;
			break;
		default:
			fprintf(stderr, "Unrecognized option '%c'\n\n", c);
			usage();
		}
	}

	if (optind != argc -1 ) usage();

	if (argv[optind] != NULL) {
		url = strdup(argv[optind]);
	}

	if (url == NULL) usage();

	client.iscsi = iscsi_create_context(initiator);
	if (client.iscsi == NULL) {
		fprintf(stderr, "Failed to create context\n");
		exit(10);
	}
	iscsi_url = iscsi_parse_full_url(client.iscsi, url);
	if (iscsi_url == NULL) {
		fprintf(stderr, "Failed to parse URL: %s\n",
			iscsi_get_error(client.iscsi));
		exit(10);
	}

	iscsi_set_targetname(client.iscsi, iscsi_url->target);
	iscsi_set_session_type(client.iscsi, ISCSI_SESSION_NORMAL);
	iscsi_set_header_digest(client.iscsi, ISCSI_HEADER_DIGEST_NONE_CRC32C);

	if (iscsi_url->user[0] != '\0') {
		if (iscsi_set_initiator_username_pwd(client.iscsi, iscsi_url->user, iscsi_url->passwd) != 0) {
			fprintf(stderr, "Failed to set initiator username and password\n");
			exit(10);
		}
	}

	if (iscsi_full_connect_sync(client.iscsi, iscsi_url->portal, iscsi_url->lun) != 0) {
		fprintf(stderr, "Login Failed. %s\n", iscsi_get_error(client.iscsi));
		iscsi_destroy_url(iscsi_url);
		iscsi_destroy_context(client.iscsi);
		exit(10);
	}

	printf("connected to %s\n", url);

	client.lun = iscsi_url->lun;
	iscsi_destroy_url(iscsi_url);

	task = iscsi_readcapacity16_sync(client.iscsi, client.lun);
	if (task == NULL || task->status != SCSI_STATUS_GOOD) {
		fprintf(stderr, "failed to send readcapacity command\n");
		exit(10);
	}

	rc16 = scsi_datain_unmarshall(task);
	if (rc16 == NULL) {
		fprintf(stderr, "failed to unmarshall readcapacity16 data\n");
		exit(10);
	}

	client.blocksize  = rc16->block_length;
	client.num_blocks  = rc16->returned_lba + 1;

	scsi_free_scsi_task(task);

	printf("capacity is %" PRIu64 " blocks or %" PRIu64 " byte (%" PRIu64 " MB)\n", client.num_blocks, client.num_blocks * client.blocksize,
	                                                        (client.num_blocks * client.blocksize) >> 20);

	printf("performing %s READ with %d parallel requests\nfixed transfer size of %d blocks (%d byte)\n",
	       client.random ? "random" : "sequential", max_in_flight, blocks_per_io, blocks_per_io * client.blocksize);

	if (runtime) {
		printf("will run for %" PRIu64 " seconds.\n", runtime);
	} else {
		printf("infinite runtime - press CTRL-C to abort.\n");
	}

	struct sigaction sa;
	sa.sa_handler = &sig_handler;
	sa.sa_flags = SA_RESTART;

	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	printf("\n");

	client.first_ns = client.last_ns = get_clock_ns();

	fill_read_queue(&client);

	while (client.in_flight) {
		pfd[0].fd = iscsi_get_fd(client.iscsi);
		pfd[0].events = iscsi_which_events(client.iscsi);

		if (poll(&pfd[0], 1, -1) < 0) {
			continue;
		}
		if (iscsi_service(client.iscsi, pfd[0].revents) < 0) {
			printf("iscsi_service failed with : %s\n", iscsi_get_error(client.iscsi));
			break;
		}
	}

	progress(&client);

	printf ("\n\nfinished.\n");

	iscsi_logout_sync(client.iscsi);
	iscsi_destroy_context(client.iscsi);
	free(url);

	return 0;
}

