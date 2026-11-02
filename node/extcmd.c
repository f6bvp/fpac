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

	User.state = STATE_EXTCMD;
	User.dl_type = AF_UNSPEC;
	strcpy(User.dl_name, cmdp->name);
	strupr(User.dl_name);
	update_user();

 	ret = norm_extcmd(cmdp, argv);

 	return ret;
}
