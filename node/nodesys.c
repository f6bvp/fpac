/*
 *  node/nodesys.c
 *
 *  FPAC project
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
/*#include <sys/utsname.h>*/
/*#include <sys/types.h>*/
/*#include <sys/socket.h>*/

#include "node.h"
#include "io.h"
#include "global.h"
#include "md5.h"

static int sys_mode = 0;

int is_sysop(void)
{
	return(sys_mode);
}

int do_sysop(int argc, char **argv)
{
	int i, len;
	char *p;
	int net_pos[5];
	time_t md5key;
	char md5str[40];
	char source[256];
	char netrom[6];

	len = strlen (cfg.password);
	if (len == 0)
	{
		/* random() % 0 would kill the session */
		node_msg("%s", T("No sysop password in fpac.conf (Password =)"));
		return(0);
	}

	md5key = time(NULL);
	srandom(md5key);

	for (i = 0; i < 5; i++)
	{
		net_pos[i] = (random () % len);
		netrom[i] = cfg.password[net_pos[i]];
	}
	netrom[i] = '\0';

	node_msg("%s> %d %d %d %d %d [%010ld]", 
			cfg.alt_callsign, 
			net_pos[0] + 1,
			net_pos[1] + 1,
			net_pos[2] + 1,
			net_pos[3] + 1,
			net_pos[4] + 1,
			md5key);

	sprintf(source, "%010ld%s", md5key, cfg.password);
	MD5String(md5str, source);

	usflush(User.fd);
	User.state = STATE_IDLE;
	time(&User.cmdtime);
	update_user();
	alarm(IdleTimeout);
	for (;;)
	{
		if ((p = readline(User.fd)) == NULL) 
		{
			if (errno == EINTR)
				continue;
			logout("User disconnected");
		}
		break;
	}

	/* NetRom */
	if (strcasecmp(p, netrom) == 0)
	{
		sys_mode = 1;
		node_msg("Nrom OK");
	}
	/* MD5 */
	else if (strcasecmp(p, md5str) == 0)
	{
		sys_mode = 1;
		node_msg("MD5 OK");
	}
	else
	{
		/*
		 * The password itself is no longer accepted: typed in plain
		 * text it goes on the air, where anybody may read it.  Tell
		 * the sysop in the log, not on the link, so that a listener
		 * does not learn that it was the right one.
		 */
		if (strcasecmp(p, cfg.password) == 0)
			fpaclog(LOGLVL_ERROR, "%s sent the sysop password in plain text: refused, change Password in fpac.conf", User.call);
		node_msg("%s", T("Refused: answer with the 5 characters or the MD5 key (see HELP SYSop)"));
	}

	return(0);
}

int do_dir(int argc, char **argv)
{
	return(0);
}

int do_yapp(int argc, char **argv)
{
	return(0);
}

