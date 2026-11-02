/*
 *
 * extcmd.c
 * FPAC project 
 *
 */
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <grp.h>

#include "node.h"
#include "io.h"

#include "ax25compat.h"

#define ECMD_PIPE	1		/* Run through pipe 		*/
#define ECMD_RECONN	2		/* */

static int norm_extcmd(struct cmd *cmdp, char **argv)
{
	int pid;

	alarm(0L);
	pid = fork();
	if (pid == -1) {
		/* fork error */
		node_perror("norm_extcmd: fork", errno);
		return 0;
	}
	if (pid == 0) 
	{
		/* child */
		setgroups(0, NULL);
		/*
		setgid(cmdp->gid);
		setuid(cmdp->uid);
		*/
		execvp(argv[0], argv);
		node_perror("norm_extcmd: execve", errno);
		exit(1);
	}
	/* parent */
	waitpid(pid, NULL, 0);
	return 0;
}

/*
 * A monitor (monitor, listen) run from a session that comes in over
 * AX.25, NetRom or ROSE sends what it sees over the links it
 * watches: each frame shown makes a new frame, a storm without end
 * (f6bvp-8, 2 November 2026, about 2000 frames/s). Such a session may
 * not run it; the local console and telnet may.
 */
static int is_monitor(const char *prog)
{
	const char *base = strrchr(prog, '/');

	base = (base != NULL) ? base + 1 : prog;
	return (strcmp(base, "monitor") == 0) || (strcmp(base, "listen") == 0);
}

static void list_ax25_ports(void)
{
	char *p = NULL;

	while ((p = ax25_config_get_next(p)) != NULL)
		tprintf(" %s", p);
	tprintf("\n");
}

/*
 * "-p <port>" of a monitor: with no port given (MONitor typed alone,
 * so "-p %1" expands to "-p"), take the first port of axports and say
 * so; refuse a port that axports does not know. argv comes from
 * parse_args() and has room for 31 words. Returns -1 to stop.
 */
static int monitor_port(struct cmd *cmdp, char **argv)
{
	static char port[32];
	char *first;
	int i;

	for (i = 1; argv[i] != NULL; i++)
		if (strcmp(argv[i], "-p") == 0)
			break;
	if (argv[i] == NULL)
		return 0;

	if (argv[i + 1] != NULL)
	{
		if (ax25_config_get_dev(argv[i + 1]) != NULL)
			return 0;
		node_msg(T("%s: unknown port %s. Ports:"), cmdp->name, argv[i + 1]);
		list_ax25_ports();
		return -1;
	}

	first = ax25_config_get_next(NULL);
	if ((first == NULL) || (i + 1 >= 31))
	{
		node_msg(T("%s: no AX.25 port to watch"), cmdp->name);
		return -1;
	}
	snprintf(port, sizeof(port), "%s", first);
	argv[i + 1] = port;
	argv[i + 2] = NULL;
	node_msg(T("%s: watching port %s (default). Other ports: %s <port>, among:"),
		 cmdp->name, port, cmdp->name);
	list_ax25_ports();
	return 0;
}

int extcmd(struct cmd *cmdp, char **argv)
{
	int ret;

	if (is_monitor(argv[0]) &&
	    ((User.ul_type == AF_AX25) || (User.ul_type == AF_NETROM) ||
	     (User.ul_type == AF_ROSE)))
	{
		node_msg(T("%s: not allowed in an AX.25, NetRom or ROSE session (the monitor would see its own output)"),
			 cmdp->name);
		return 0;
	}
	if (is_monitor(argv[0]) && (monitor_port(cmdp, argv) == -1))
		return 0;

	User.state = STATE_EXTCMD;
	User.dl_type = AF_UNSPEC;
	strcpy(User.dl_name, cmdp->name);
	strupr(User.dl_name);
	update_user();

 	ret = norm_extcmd(cmdp, argv);

 	return ret;
}
