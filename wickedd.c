/*
 * No REST for the wicked!
 *
 * This command line utility provides a daemon interface to the network
 * configuration/information facilities.
 *
 * It uses a RESTful interface (even though it's a command line utility).
 * The idea is to make it easier to extend this to some smallish daemon
 * with a AF_LOCAL socket interface.
 *
 * Copyright (C) 2010 Olaf Kirch <okir@suse.de>
 */

#include <sys/poll.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <errno.h>

#include <wicked/netinfo.h>
#include <wicked/logging.h>
#include <wicked/wicked.h>
#include <wicked/xml.h>
#include <wicked/socket.h>

enum {
	OPT_CONFIGFILE,
	OPT_DEBUG,
	OPT_FOREGROUND,
	OPT_NOFORK,
};

static struct option	options[] = {
	{ "config",		required_argument,	NULL,	OPT_CONFIGFILE },
	{ "debug",		required_argument,	NULL,	OPT_DEBUG },
	{ "foreground",		no_argument,		NULL,	OPT_FOREGROUND },
	{ "no-fork",		no_argument,		NULL,	OPT_NOFORK },

	{ NULL }
};

static int		opt_foreground = 0;
static int		opt_nofork = 0;

static int		wicked_accept_connection(ni_socket_t *, uid_t, gid_t);
static void		wicked_interface_event(ni_handle_t *, ni_interface_t *, ni_event_t);
static void		wicked_process_network_restcall(ni_socket_t *);

int
main(int argc, char **argv)
{
	ni_socket_t *sock;
	int c;

	while ((c = getopt_long(argc, argv, "+", options, NULL)) != EOF) {
		switch (c) {
		default:
		usage:
			fprintf(stderr,
				"./wickedd [options]\n"
				"This command understands the following options\n"
				"  --config filename\n"
				"        Read configuration file <filename> instead of system default.\n"
				"  --debug facility\n"
				"        Enable debugging for debug <facility>.\n"
			       );
			return 1;

		case OPT_CONFIGFILE:
			ni_set_global_config_path(optarg);
			break;

		case OPT_DEBUG:
			if (!strcmp(optarg, "help")) {
				printf("Supported debug facilities:\n");
				ni_debug_help(stdout);
				return 0;
			}
			if (ni_enable_debug(optarg) < 0) {
				fprintf(stderr, "Bad debug facility \"%s\"\n", optarg);
				return 1;
			}
			break;

		case OPT_FOREGROUND:
			opt_foreground = 1;
			break;

		case OPT_NOFORK:
			opt_nofork = 1;
			break;

		}
	}

	if (ni_init() < 0)
		return 1;

	if (optind != argc)
		goto usage;

	if ((sock = ni_server_listen()) < 0)
		ni_fatal("unable to initialize server socket");
	sock->accept = wicked_accept_connection;

	/* open global RTNL socket to listen for kernel events */
	if (ni_server_listen_events(wicked_interface_event) < 0)
		ni_fatal("unable to initialize netlink listener");

	if (!opt_foreground) {
		if (ni_server_background() < 0)
			return 1;
		ni_log_destination_syslog("wickedd");
	}

	while (1) {
		if (ni_socket_wait(-1) < 0)
			ni_fatal("ni_socket_wait failed");
	}

	exit(0);
}

/*
 * Accept an incoming connection.
 * Return value of -1 means close the socket.
 */
static int
wicked_accept_connection(ni_socket_t *sock, uid_t uid, gid_t gid)
{
	if (uid != 0) {
		ni_error("refusing attempted connection by user %u", uid);
		return -1;
	}

	ni_debug_wicked("accepted connection from uid=%u", uid);
	if (opt_nofork) {
		wicked_process_network_restcall(sock);
	} else {
		pid_t pid;

		/* Fork the worker child */
		pid = fork();
		if (pid < 0) {
			ni_error("unable to fork worker child: %m");
			return -1;
		}

		if (pid == 0) {
			wicked_process_network_restcall(sock);
			exit(0);
		}
	}

	return -1;
}

void
wicked_process_network_restcall(ni_socket_t *sock)
{
	ni_wicked_request_t req;
	int rv;

	/* Read the request coming in from the socket. */
	ni_wicked_request_init(&req);
	rv = ni_wicked_request_parse(sock, &req);

	/* Process the call */
	if (rv >= 0)
		rv = ni_wicked_call_direct(&req);

	/* ... and send the response back. */
	ni_wicked_response_print(sock, &req, rv);

	ni_wicked_request_destroy(&req);
}

/*
 * Handle network layer events.
 * FIXME: There should be some locking here, which prevents us from
 * calling event handlers on an interface that the admin is currently
 * mucking with manually.
 */
void
wicked_interface_event(ni_handle_t *nih, ni_interface_t *ifp, ni_event_t event)
{
	static const char *evtype[__NI_EVENT_MAX] =  {
		[NI_EVENT_LINK_CREATE]	= "link-create",
		[NI_EVENT_LINK_DELETE]	= "link-delete",
		[NI_EVENT_LINK_UP]	= "link-up",
		[NI_EVENT_LINK_DOWN]	= "link-down",
		[NI_EVENT_NETWORK_UP]	= "network-up",
		[NI_EVENT_NETWORK_DOWN]	= "network-down",
	};
	xml_node_t *evnode = NULL;
	xml_node_t *ifnode = NULL;
	ni_policy_t *policy;

	if (event >= __NI_EVENT_MAX || !evtype[event])
		return;

	ni_debug_events("%s: %s event", ifp->name, evtype[event]);

	evnode = xml_node_new("event", NULL);
	xml_node_add_attr(evnode, "type", evtype[event]);

	ifnode = ni_syntax_xml_from_interface(ni_default_xml_syntax(), nih, ifp);
	if (!ifnode)
		goto out;

	xml_node_replace_child(evnode, ifnode);
	policy = ni_policy_match_event(ni_default_policies(), evnode);
	if (!policy)
		goto out;

	ni_debug_events("matched a policy (action=%s)", policy->action);
	if (ni_policy_apply(policy, ifnode) < 0)
		goto out;

#if 0
	ni_debug_events("Policy transformation: apply %s to %s", policy->action, ifp->name);
	xml_node_print(ifnode, stderr);
#endif

	/* Finally, invoke REST function */
	{
		char restpath[256];

		snprintf(restpath, sizeof(restpath), "/system/interface/%s", ifp->name);
		/* wicked_rest_call(policy->action, restpath, ifnode); */
	}

out:
	/* No need to free ifnode; it's a child of evnode */
	if (evnode)
		xml_node_free(evnode);
}
