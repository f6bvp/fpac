/*
 * route/fpacrtd.c
 *
 * FPAC project
 *
 * Dynamic ROSE routes daemon. Each node announces to its direct
 * neighbours the destinations it can reach (node addresses and whole
 * DNICs), learns the ones it lacks and installs them in the kernel.
 * The routes of fpac.routes always keep the last word.
 *
 * See doc/dynamic-routes.md for the specification.
 *
 * F6BVP 2026-10-08 - first draft for FPAC 4.2-rc1
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <time.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <net/route.h>
#include <netax25/ax25.h>
#include <netrose/rose.h>
#include <netax25/axlib.h>
#include <netax25/axconfig.h>
#include <netax25/procutils.h>

#include "config.h"
#include "fpac.h"
#include "pathnames.h"
#include "wp.h"

#define RT_VERSION		1
/* ROSE service callsign. Not "ROUTE": fpacroute (the traceroute
 * application) already listens on it on every FPAC node. */
#define RT_SERVICE		"FPACRT"
#define RT_MAX_COST		6		/* Hops ceiling */
#define RT_PERIOD		(30 * 60)	/* Full table period */
#define RT_RETRY		(30 * 60)	/* Retry a refused neighbour */
/*
 * The first call after a restart of either node often fails: the link to
 * the neighbour is not restarted yet, so the kernel tries a less specific
 * route, while it brings that link up. Call again soon, then less often.
 */
#define RT_RETRY_FIRST		60	/* After the 1st failure in a row */
#define RT_RETRY_SECOND		(5 * 60)	/* After the 2nd */
#define RT_STABLE		(10 * 60)	/* A session up that long resets the count */
#define RT_CONNECT_TIMEOUT	120
#define RT_START_DELAY		30		/* Before the first call */
#define RT_MISSED_MAX		3		/* Tables without the route */
#define RT_TRIGGER_DELAY	10		/* Groups triggered updates */
#define RT_CHECK_PERIOD		60		/* Neighbours state check */
#define RT_HOLD			60		/* Reference distance kept after a loss */

#define RT_WP_MAX_AGE		(30 * 24 * 3600)	/* Node records older are not used */

#define RT_MAX_NEIGH		32
#define RT_MAX_DEST		512
#define RT_MAX_QUEUE		1024

#define RT_STATE_FILE		AX25_LOCALSTATEDIR "fpac/fpacrt.dat"
/* Discovery of new nodes (FpacAnnounce, FpacDiscover) */
#define RT_DISC_FILE		AX25_LOCALSTATEDIR "fpac/fpacrt.discovered"
#define RT_PENDING_FILE		AX25_LOCALSTATEDIR "fpac/fpacrt.pending"
#define RT_REFUSED_FILE		AX25_LOCALSTATEDIR "fpac/fpacrt.refused"
#define RT_ANN_PERIOD		(30 * 60)	/* FPAC announcement period */
#define RT_ANN_JITTER		120		/* +/- seconds around it */
#define RT_ANN_REPLY		(10 * 60)	/* At most one direct reply per node */
#define RT_DISC_MAX		8		/* Nodes accepted provisionally */
#define RT_MAX_PORTS		8
#define DISC_NO			0
#define DISC_ASK		1
#define DISC_YES		2
#ifndef AX25_P_TEXT
#define AX25_P_TEXT		0xF0	/* PID of text frames */
#endif
#define RT_LOCK_FILE		AX25_LOCALSTATEDIR "fpac/fpacrtd.lock"

#define LEARN_NO	0
#define LEARN_NODES	1
#define LEARN_ALL	2

#define N_DOWN		0
#define N_CONNECTING	1
#define N_UP		2

/* Digits of a ROSE address (DNIC included) that make a neighbour's block */
#define RT_BLOCK_DIGITS	9

struct rt_neigh {
	char	name[20];
	char	call[10];	/* L3 callsign of the neighbour */
	char	addr[11];	/* Full ROSE address */
	char	port[10];	/* AX.25 port of its Node block */
	int	digis;		/* Path of its Node block goes through digipeaters */
	int	excluded;	/* No "Route = yes" in its Node block */
	int	provisional;	/* Heard and accepted, not in fpac.nodes */
	int	invalidated;	/* Was provisional, invalidated by the sysop */
	time_t	last_reply;	/* Last direct announcement sent to it */
	char	nr_dev[16];	/* Device of the route set to its address, "" = none */
	int	fd;
	int	state;
	int	incoming;	/* Session opened by the neighbour */
	int	hello;		/* FPACRT line received */
	int	nb_rcvd;	/* R lines since the last E */
	time_t	retry;
	int	failures;	/* Calls or sessions lost in a row */
	time_t	up_since;	/* Start of the current session */
	time_t	deadline;	/* Connect timeout */
	time_t	next_table;
	time_t	last_table;	/* Last full table received, or session start */
	char	*queue[RT_MAX_QUEUE];
	int	nb_queue;
};

struct rt_dest {
	char	addr[11];	/* 10 digits, padded with zeros */
	int	mask;		/* 10 or 4 */
	int	own;		/* This node is the origin */
	int	exit_open;	/* Own DNIC: exit link open at the last check, -1 = not checked yet */
	int	manual;		/* Same address and mask in fpac.routes */
	int	cost[RT_MAX_NEIGH];	/* Our cost through each neighbour, 0 = none */
	int	missed[RT_MAX_NEIGH];
	int	seen[RT_MAX_NEIGH];
	int	sent[RT_MAX_NEIGH];	/* Cost last announced + 1, 0 = none */
	int	wp;		/* Neighbour the White Pages record came from, -1 = none */
	int	wp_seen;	/* Record found by the last wp_scan() */
	int	wp_down;	/* Last provenance ignored (learns from us), -1 = none */
	int	installed;	/* Neighbour index of the kernel route, -1 = none */
	int	fd;		/* Reference distance: best cost we had, 0 = none */
	time_t	lost;		/* No feasible route since, 0 = we have one */
	int	adj;		/* Address of this direct neighbour, link open, -1 = no */
	char	inst_call[10];
	char	inst_dev[16];
};

static cfg_t cfg;
static struct rt_neigh neigh[RT_MAX_NEIGH];
static int nb_neigh;
static struct rt_dest dest[RT_MAX_DEST];
static int nb_dest;

static int rt_announce;			/* RouteAnnounce */
static int rt_learn = LEARN_NO;		/* RouteLearn */
static int rt_wp;			/* RouteWp */
static int rt_neighbours;		/* RouteNeighbours */
static int fpac_announce;		/* FpacAnnounce */
static int fpac_discover = DISC_YES;	/* FpacDiscover: no, ask or yes */
static volatile sig_atomic_t lists_changed;	/* SIGUSR1: fpacrtd -a or -x */
static char rt_origin[32][5];		/* RouteOrigin */
static int nb_origin;

static int verbose;
static int dry_run;			/* -n: never touch the kernel routes */
static int foreground;
static int check_only;			/* -t: show the configuration and exit */
static char *extra_conf;		/* -c: more Route keywords, for tests */
static int listen_fd = -1;
static volatile sig_atomic_t stop;
static time_t trigger_date;		/* Pending triggered update, 0 = none */

static void rt_log(int prio, const char *fmt, ...)
{
	va_list ap;
	char buf[512];

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (foreground)
		fprintf(stderr, "%s\n", buf);
	if (!check_only)
		syslog(prio, "%s", buf);
}

/************************************************************************
 * Configuration
 ************************************************************************/

/* "key = value" line, comment stripped. Returns 0 when it is not one. */
static int conf_line(char *line, char *key, size_t klen, char *val, size_t vlen)
{
	char *p, *eq;

	if ((p = strchr(line, '#')) != NULL)
		*p = '\0';
	if ((eq = strchr(line, '=')) == NULL)
	{
		/* No value: give the bare word (for instance "End") as the key */
		for (p = line; isspace((unsigned char)*p); p++)
			;
		snprintf(key, klen, "%s", p);
		*val = '\0';
		return 0;
	}
	*eq++ = '\0';
	for (p = line; isspace((unsigned char)*p); p++)
		;
	snprintf(key, klen, "%s", p);
	for (p = key + strlen(key); p > key && isspace((unsigned char)p[-1]); )
		*--p = '\0';
	while (isspace((unsigned char)*eq))
		eq++;
	snprintf(val, vlen, "%s", eq);
	for (p = val + strlen(val); p > val && isspace((unsigned char)p[-1]); )
		*--p = '\0';
	return (*key != '\0');
}

static int conf_yes(const char *val)
{
	return (strcasecmp(val, "yes") == 0 || strcmp(val, "1") == 0);
}

/*
 * The Route* keywords are read here and not by libcfg: the other FPAC
 * programs ignore a keyword they do not know, so fpac.conf stays
 * readable by a 4.1 node.
 */
static void conf_scan(const char *file)
{
	FILE *fp;
	char line[256], key[64], val[128];
	char node[20] = "";
	int i;

	if ((fp = fopen(file, "r")) == NULL)
		return;

	while (fgets(line, sizeof(line), fp))
	{
		if (!conf_line(line, key, sizeof(key), val, sizeof(val)))
		{
			if (strncasecmp(key, "end", 3) == 0)
				*node = '\0';
			continue;
		}
		if (strcasecmp(key, "node") == 0)
			snprintf(node, sizeof(node), "%s", val);
		else if (strcasecmp(key, "end") == 0)
			*node = '\0';
		else if (strcasecmp(key, "routeannounce") == 0)
			rt_announce = conf_yes(val);
		else if (strcasecmp(key, "routelearn") == 0)
		{
			if (strcasecmp(val, "nodes") == 0)
				rt_learn = LEARN_NODES;
			else if (strcasecmp(val, "all") == 0)
				rt_learn = LEARN_ALL;
			else
				rt_learn = LEARN_NO;
		}
		else if (strcasecmp(key, "routewp") == 0)
			rt_wp = conf_yes(val);
		else if (strcasecmp(key, "routeneighbours") == 0)
			rt_neighbours = conf_yes(val);
		else if (strcasecmp(key, "fpacannounce") == 0)
			fpac_announce = conf_yes(val);
		else if (strcasecmp(key, "fpacdiscover") == 0)
		{
			if (strcasecmp(val, "ask") == 0)
				fpac_discover = DISC_ASK;
			else if (conf_yes(val))
				fpac_discover = DISC_YES;
			else
				fpac_discover = DISC_NO;
		}
		else if (strcasecmp(key, "routeorigin") == 0)
		{
			if (strlen(val) == 4 && strspn(val, "0123456789") == 4 && nb_origin < 32)
				strcpy(rt_origin[nb_origin++], val);
			else
				rt_log(LOG_WARNING, "RouteOrigin = %s ignored: a DNIC is 4 digits", val);
		}
		else if (strcasecmp(key, "route") == 0 && *node)
		{
			/* A neighbour takes part only when its Node block says so:
			 * a node without fpacrtd hands a call to the FPACRT service
			 * to fpad, which treats it as a user call (White Pages
			 * record, AX.25 connection attempt). */
			for (i = 0; i < nb_neigh; i++)
				if (strcasecmp(neigh[i].name, node) == 0)
					neigh[i].excluded = !conf_yes(val);
		}
	}
	fclose(fp);
}

/* Address and mask of a fpac.routes entry equal to this destination */
static int is_manual(const char *addr, int mask)
{
	route_t *r;

	for (r = cfg.route; r != NULL; r = r->next)
		if ((int)strlen(r->addr) == mask && strncmp(r->addr, addr, mask) == 0)
			return 1;
	return 0;
}

static struct rt_dest *dest_find(const char *addr, int mask, int create)
{
	int i;

	for (i = 0; i < nb_dest; i++)
		if (dest[i].mask == mask && strncmp(dest[i].addr, addr, mask) == 0)
			return &dest[i];

	if (!create || nb_dest == RT_MAX_DEST)
		return NULL;

	memset(&dest[nb_dest], 0, sizeof(dest[0]));
	snprintf(dest[nb_dest].addr, sizeof(dest[0].addr), "%.*s%.*s",
		 mask, addr, 10 - mask, "0000000000");
	dest[nb_dest].mask = mask;
	dest[nb_dest].wp = -1;
	dest[nb_dest].wp_down = -1;
	dest[nb_dest].installed = -1;
	dest[nb_dest].adj = -1;
	dest[nb_dest].manual = is_manual(addr, mask);
	return &dest[nb_dest++];
}

static int config_load(void)
{
	node_t *n;
	addrp_t *al;
	struct rt_dest *d;
	char full[16];
	char *p;
	int i;

	if (cfg_open(&cfg) != 0)
	{
		rt_log(LOG_ERR, "cannot read the FPAC configuration");
		return -1;
	}

	/* Neighbours: the adjacent nodes of fpac.nodes */
	for (n = cfg.node; n != NULL && nb_neigh < RT_MAX_NEIGH; n = n->next)
	{
		struct rt_neigh *v = &neigh[nb_neigh];

		memset(v, 0, sizeof(*v));
		snprintf(v->name, sizeof(v->name), "%s", n->name);
		snprintf(v->call, sizeof(v->call), "%s", n->call);
		if ((p = strpbrk(v->call, " ,\t")) != NULL)
			*p = '\0';
		v->digis = (p = strpbrk(n->call, " ,\t")) != NULL && p[strspn(p, " ,\t")] != '\0';
		snprintf(v->port, sizeof(v->port), "%s", n->port);
		for (p = v->call; *p; p++)
			*p = toupper((unsigned char)*p);
		snprintf(v->addr, sizeof(v->addr), "%s%s", n->dnic, n->addr);
		v->fd = -1;
		v->excluded = 1;
		/* Let the neighbours start their own fpacrtd first: a call to
		 * a node where it does not run yet is answered by fpad */
		v->retry = time(NULL) + RT_START_DELAY;
		if (strlen(v->addr) != 10 || !*v->call)
			continue;
		nb_neigh++;
	}

	conf_scan(FPACCONF);
	conf_scan(FPACNODES);
	if (extra_conf)
		conf_scan(extra_conf);

	/* Own addresses: origin at cost 0 */
	if ((d = dest_find(cfg.fulladdr, 10, 1)) != NULL)
		d->own = 1;
	for (al = cfg.addrp; al != NULL; al = al->next)
	{
		snprintf(full, sizeof(full), "%s%s", cfg.dnic, al->addr);
		if (strlen(full) == 10 && (d = dest_find(full, 10, 1)) != NULL)
			d->own = 1;
	}

	/* DNICs this node is the exit gateway of */
	for (i = 0; i < nb_origin; i++)
	{
		if (!is_manual(rt_origin[i], 4))
		{
			rt_log(LOG_WARNING, "RouteOrigin = %s ignored: no route to this DNIC in fpac.routes",
			       rt_origin[i]);
			continue;
		}
		if ((d = dest_find(rt_origin[i], 4, 1)) != NULL)
		{
			d->own = 1;
			d->exit_open = -1;
		}
	}
	return 0;
}

/************************************************************************
 * Kernel
 ************************************************************************/

/* Is this neighbour restarted ("open")? Gives its device. */
static int kernel_neigh_open(const char *call, char *dev, size_t len)
{
	struct proc_rs_neigh *list, *p;
	int open = 0;

	list = read_proc_rs_neigh();
	for (p = list; p != NULL; p = p->next)
		if (strcasecmp(p->call, call) == 0)
		{
			open = (strcmp(p->restart, "yes") == 0);
			if (dev)
				snprintf(dev, len, "%s", p->dev);
			break;
		}
	free_proc_rs_neigh(list);
	return open;
}

/* Callsign of the first neighbour of the kernel route to addr/mask */
static int kernel_next_hop(const char *addr, int mask, char *call, size_t len)
{
	struct proc_rs_nodes *nodes, *pn;
	struct proc_rs_neigh *list, *pv;
	int found = 0;

	nodes = read_proc_rs_nodes();
	list = read_proc_rs_neigh();
	for (pn = nodes; pn != NULL && !found; pn = pn->next)
	{
		if (pn->mask != mask || strncmp(pn->address, addr, mask) != 0)
			continue;
		for (pv = list; pv != NULL; pv = pv->next)
			if ((unsigned int)pv->addr == pn->neigh1)
			{
				snprintf(call, len, "%s", pv->call);
				found = 1;
				break;
			}
	}
	free_proc_rs_nodes(nodes);
	free_proc_rs_neigh(list);
	return found;
}

/*
 * Does the kernel route to addr/mask have a restarted ("open") neighbour
 * other than this one? The kernel lets a call in transit leave only through
 * an open neighbour (a closed one is tried for a call of this node only), so
 * a DNIC whose exit link is closed leads nowhere for the other nodes.
 * except may be NULL.
 */
static int kernel_exit_open(const char *addr, int mask, const char *except)
{
	struct proc_rs_nodes *nodes, *pn;
	struct proc_rs_neigh *list, *pv;
	int open = 0;

	nodes = read_proc_rs_nodes();
	list = read_proc_rs_neigh();
	for (pn = nodes; pn != NULL && !open; pn = pn->next)
	{
		if (pn->mask != mask || strncmp(pn->address, addr, mask) != 0)
			continue;
		for (pv = list; pv != NULL && !open; pv = pv->next)
		{
			unsigned int a = (unsigned int)pv->addr;

			if ((pn->n < 1 || a != pn->neigh1) && (pn->n < 2 || a != pn->neigh2)
			    && (pn->n < 3 || a != pn->neigh3))
				continue;
			if (except && strcasecmp(pv->call, except) == 0)
				continue;
			open = (strcmp(pv->restart, "yes") == 0);
		}
	}
	free_proc_rs_nodes(nodes);
	free_proc_rs_neigh(list);
	return open;
}

static int kernel_route(int add, const char *addr, int mask, const char *call, const char *dev)
{
	struct rose_route_struct r;
	int s, rc;

	rt_log(LOG_INFO, "%s route %.*s/%d via %s%s", add ? "add" : "del",
	       mask, addr, mask, call, dry_run ? " (dry run)" : "");
	if (dry_run)
		return 0;

	memset(&r, 0, sizeof(r));
	if (rose_aton(addr, r.address.rose_addr) != 0
	    || ax25_aton_entry(call, r.neighbour.ax25_call) != 0)
		return -1;
	r.mask = mask;
	snprintf(r.device, sizeof(r.device), "%s", dev);
	r.ndigis = 0;

	if ((s = socket(AF_ROSE, SOCK_SEQPACKET, 0)) < 0)
		return -1;
	rc = ioctl(s, add ? SIOCADDRT : SIOCDELRT, &r);
	if (rc == -1)
		rt_log(LOG_WARNING, "%s route %.*s/%d via %s: %s", add ? "add" : "del",
		       mask, addr, mask, call, strerror(errno));
	close(s);
	return rc;
}

/* The routes this daemon installed, to purge them after a crash */
static void state_save(void)
{
	FILE *fp;
	int i;

	if (dry_run)
		return;
	if ((fp = fopen(RT_STATE_FILE, "w")) == NULL)
		return;
	for (i = 0; i < nb_dest; i++)
		if (dest[i].installed >= 0)
			fprintf(fp, "%s %d %s %s\n", dest[i].addr, dest[i].mask,
				dest[i].inst_call, dest[i].inst_dev);
	fclose(fp);
}

/*
 * A neighbour of fpac.nodes marked "Route = yes" gets a kernel route to its
 * own address, so that a new node needs no line in fpac.routes: without it,
 * this daemon cannot call the neighbour, and the kernel ignores the ROSE
 * frames of a neighbour that no route uses (it is not in rose_neigh).
 * A route of fpac.routes, or one through another neighbour, is left alone.
 *
 * The route is part of the configuration, like a line of fpac.routes: it is
 * kept when this daemon stops, and taken over when it starts again. Removing
 * it would remove the neighbour from the kernel, with every circuit through
 * it, at each restart of fpacrtd. A restart of fpad clears it with the rest.
 */
static void neigh_route_set(struct rt_neigh *v)
{
	char call[16];
	char *dev;

	if (v->excluded || v->nr_dev[0])
		return;
	if (is_manual(v->addr, 10))
		return;
	dev = ax25_config_get_dev(v->port[0] ? v->port : NULL);
	if (kernel_next_hop(v->addr, 10, call, sizeof(call)))
	{
		if (strcasecmp(call, v->call) == 0 && dev != NULL)
		{
			rt_log(LOG_INFO, "neighbour %s %s: route kept", v->call, v->addr);
			snprintf(v->nr_dev, sizeof(v->nr_dev), "%s", dev);
		}
		else
			rt_log(LOG_INFO, "neighbour %s %s: kernel route via %s already there", v->call, v->addr, call);
		return;
	}
	if (v->digis)
	{
		rt_log(LOG_WARNING, "neighbour %s %s: path through digipeaters, no route set (write it in fpac.routes)",
		       v->call, v->addr);
		return;
	}
	if (dev == NULL)
	{
		rt_log(LOG_WARNING, "neighbour %s %s: unknown AX.25 port \"%s\", no route set",
		       v->call, v->addr, v->port);
		return;
	}
	if (kernel_route(1, v->addr, 10, v->call, dev) == 0)
		snprintf(v->nr_dev, sizeof(v->nr_dev), "%s", dev);
}

static void neigh_routes_set(void)
{
	int i;

	if (ax25_config_load_ports() <= 0)
		return;
	for (i = 0; i < nb_neigh; i++)
		neigh_route_set(&neigh[i]);
}

static void state_purge(void)
{
	FILE *fp;
	char addr[16], call[16], dev[32];
	int mask;

	if (dry_run || (fp = fopen(RT_STATE_FILE, "r")) == NULL)
		return;
	while (fscanf(fp, "%15s %d %15s %31s", addr, &mask, call, dev) == 4)
		if (!is_manual(addr, mask))
			kernel_route(0, addr, mask, call, dev);
	fclose(fp);
	unlink(RT_STATE_FILE);
}

/************************************************************************
 * Routing table
 ************************************************************************/

static int learn_allowed(int mask)
{
	if (rt_learn == LEARN_ALL)
		return (mask == 10 || mask == 4);
	if (rt_learn == LEARN_NODES)
		return (mask == 10);
	return 0;
}

/*
 * Best neighbour for a destination, -1 when none.
 *
 * Feasibility condition (as in EIGRP or Babel): a neighbour is used only
 * when the cost it announces (our cost through it minus 1) is strictly
 * lower than the reference distance, the best cost we had for this
 * destination. Such a neighbour cannot be routing through us. Without it,
 * in a triangle of nodes that all announce, the loss of a route made two
 * nodes fall back on each other for up to a minute (counting to the
 * ceiling of 6), and a ROSE call made then went round in circles. The
 * reference is forgotten RT_HOLD seconds after the destination has no
 * feasible route left, once the withdrawals have gone round.
 */
static int dest_best(struct rt_dest *d)
{
	int i, best = -1;

	for (i = 0; i < nb_neigh; i++)
	{
		if (neigh[i].state != N_UP || d->cost[i] == 0)
			continue;
		if (d->fd && d->cost[i] - 1 >= d->fd)
			continue;
		if (best < 0 || d->cost[i] < d->cost[best]
		    || (d->cost[i] == d->cost[best] && i == d->installed))
			best = i;
	}
	return best;
}

/* Our cost to a destination, -1 when we do not really route it */
static int dest_cost(struct rt_dest *d)
{
	int best;

	if (d->own)
		return 0;
	if (d->adj >= 0)
		return 1;
	if ((best = dest_best(d)) < 0)
		return -1;
	if (d->installed < 0 && !d->manual)
		return -1;
	return d->cost[best];
}

/* Keep the reference distance of a destination up to date */
static void dest_feasible(struct rt_dest *d)
{
	int i, best, held = 0;

	if (d->own)
		return;
	if ((best = dest_best(d)) >= 0)
	{
		if (!d->fd || d->cost[best] < d->fd)
			d->fd = d->cost[best];
		d->lost = 0;
		return;
	}
	if (!d->fd || d->lost)
		return;
	d->lost = time(NULL);
	for (i = 0; i < nb_neigh; i++)
		if (neigh[i].state == N_UP && d->cost[i])
			held++;
	if (held)
		rt_log(LOG_INFO, "%.*s/%d: %d route(s) left that could loop through us, not used for %d s",
		       d->mask, d->addr, d->mask, held, RT_HOLD);
}

static void dest_update(struct rt_dest *d)
{
	char dev[16] = "";
	int want = -1;
	int best;

	dest_feasible(d);

	if (!d->own && !d->manual && learn_allowed(d->mask))
	{
		best = dest_best(d);
		if (best >= 0 && kernel_neigh_open(neigh[best].call, dev, sizeof(dev)))
			want = best;
	}

	/* Last: the neighbour the White Pages record of the node came from */
	if (want < 0 && rt_wp && !d->own && !d->manual && d->mask == 10 && d->wp >= 0
	    && kernel_neigh_open(neigh[d->wp].call, dev, sizeof(dev)))
		want = d->wp;

	/* The address of one of our own direct neighbours, whose link is
	 * open: the node reaches it directly, through whatever route its
	 * sysop wrote. A learned /10 through another neighbour would go
	 * before that route and send the calls round about. */
	if (want >= 0 && d->mask == 10)
	{
		int j;

		for (j = 0; j < nb_neigh; j++)
			if (j != want && strcmp(neigh[j].addr, d->addr) == 0
			    && kernel_neigh_open(neigh[j].call, NULL, 0))
			{
				want = -1;
				break;
			}
		/* The neighbour itself, which announces its own address: the
		 * route neigh_routes_set() gave it is already there */
		if (want >= 0 && strcmp(neigh[want].addr, d->addr) == 0 && neigh[want].nr_dev[0])
			want = -1;
	}

	if (want == d->installed)
		return;

	if (d->installed >= 0)
		kernel_route(0, d->addr, d->mask, d->inst_call, d->inst_dev);
	d->installed = -1;
	if (want >= 0 && kernel_route(1, d->addr, d->mask, neigh[want].call, dev) == 0)
	{
		d->installed = want;
		snprintf(d->inst_call, sizeof(d->inst_call), "%s", neigh[want].call);
		snprintf(d->inst_dev, sizeof(d->inst_dev), "%s", dev);
	}
	state_save();
	if (trigger_date == 0)
		trigger_date = time(NULL) + RT_TRIGGER_DELAY;
}

/*
 * Is <addr> in the block of addresses of neighbour <i> alone (same DNIC
 * and first five digits, ROSE mask 9), shared neither with this node nor
 * with another neighbour? Such a node stands behind that neighbour, which
 * does not learn it from us: F3KT-6 2080444503 behind F3KT-11 2080444501.
 * A block we share (2080 17552x: f6bvp-8, F6BVP-5, F6BVP-11) gives no
 * such certainty.
 */
static int in_neigh_block(const char *addr, int i)
{
	int j;

	if (strncmp(addr, neigh[i].addr, RT_BLOCK_DIGITS) != 0
	    || strncmp(addr, cfg.fulladdr, RT_BLOCK_DIGITS) == 0)
		return 0;
	for (j = 0; j < nb_neigh; j++)
		if (j != i && strncmp(addr, neigh[j].addr, RT_BLOCK_DIGITS) == 0)
			return 0;
	return 1;
}

/*
 * Routes drawn from the White Pages (RouteWp, specification section 19).
 * fpacwpd notes in each node record the adjacent server it took the record
 * from (wp_get_origin()); that neighbour leads to the node, since it had
 * the record before us. The file is read directly, as wpserv does: the
 * provenance is in the local part of the record, which the White Pages
 * protocol does not carry.
 */
static void wp_scan(void)
{
	FILE *fp;
	wp_header wph;
	wp_t wp;
	rose_address origin;
	struct rt_dest *d;
	char addr[11], from[11];
	time_t now = time(NULL);
	int i, n, found;

	if ((fp = fopen(FPACWP, "r")) == NULL)
		return;		/* keep what we have: fpacwpd may be rewriting it */
	if (fread(&wph, sizeof(wph), 1, fp) != 1
	    || strncmp(wph.signature, FILE_SIGNATURE, sizeof(wph.signature)) != 0)
	{
		fclose(fp);
		return;
	}
	for (i = 0; i < nb_dest; i++)
		dest[i].wp_seen = 0;
	for (n = 0; n < wph.nb_record && fread(&wp, sizeof(wp), 1, fp) == 1; n++)
	{
		if (!wp.is_node || wp.is_deleted || now - wp.date > RT_WP_MAX_AGE
		    || !wp_get_origin(&wp, &origin))
			continue;
		snprintf(addr, sizeof(addr), "%s", rose_ntoa(&wp.address.srose_addr));
		snprintf(from, sizeof(from), "%s", rose_ntoa(&origin));
		if (strlen(addr) != 10 || strspn(addr, "0123456789") != 10)
			continue;
		for (found = -1, i = 0; i < nb_neigh; i++)
			if (strcmp(neigh[i].addr, from) == 0)
				found = i;
		if (found < 0)
			continue;	/* came from a server that is not a neighbour */
		if ((d = dest_find(addr, 10, 1)) == NULL || d->own)
			continue;
		/*
		 * A neighbour we announce our routes to takes its routes from
		 * us: that it relayed the record does not show it leads to the
		 * node, and it may route the node back through us (loop).
		 * Keep the provenance we had, if any. Except for a node in a
		 * block of addresses that belongs to that neighbour alone, see
		 * in_neigh_block().
		 */
		if (rt_announce && !neigh[found].excluded
		    && !in_neigh_block(addr, found))
		{
			if (d->wp_down != found)
			{
				rt_log(LOG_INFO, "White Pages: %s %s/10 came from %s, which learns its routes from us: not used",
				       ax25_ntoa(&wp.address.srose_call), addr, neigh[found].call);
				d->wp_down = found;
			}
			if (d->wp >= 0)
				d->wp_seen = 1;
			continue;
		}
		d->wp_down = -1;
		d->wp_seen = 1;
		if (d->wp != found)
		{
			rt_log(LOG_INFO, "White Pages: %s %s/10 came from %s",
			       ax25_ntoa(&wp.address.srose_call), addr, neigh[found].call);
			d->wp = found;
		}
	}
	fclose(fp);

	for (i = 0; i < nb_dest; i++)
		if (dest[i].wp >= 0 && !dest[i].wp_seen)
		{
			rt_log(LOG_INFO, "White Pages: %.10s/10 no longer usable", dest[i].addr);
			dest[i].wp = -1;
		}
}

/*
 * Direct neighbours announced at cost 1 (RouteNeighbours = yes,
 * specification, section 20): a
 * neighbour of fpac.nodes, whether it runs fpacrtd or not, whose address
 * the kernel routes through that very neighbour (fpac.routes), while its
 * link is open. F3KT announces F5ZHO-8 (2080817501) that way: F5ZHO does
 * not run fpacrtd, and no other node could reach it without a fixed
 * route. A route this daemon installed itself does not count.
 */
static void adj_scan(void)
{
	char hop[10];
	struct rt_dest *d;
	int i, ok;

	if (!rt_announce || !rt_neighbours)
		return;
	for (i = 0; i < nb_neigh; i++)
	{
		ok = kernel_neigh_open(neigh[i].call, NULL, 0)
		     && kernel_next_hop(neigh[i].addr, 10, hop, sizeof(hop))
		     && strcasecmp(hop, neigh[i].call) == 0;
		if ((d = dest_find(neigh[i].addr, 10, ok)) == NULL || d->own)
			continue;
		if (ok && d->installed == i)
			ok = 0;
		if (ok && d->adj < 0)
		{
			d->adj = i;
			rt_log(LOG_INFO, "direct neighbour %s %s/10: link open, announced", neigh[i].call, d->addr);
		}
		else if (!ok && d->adj == i)
		{
			d->adj = -1;	/* another neighbour with this address may take over next time */
			rt_log(LOG_INFO, "direct neighbour %s %s/10: no longer announced", neigh[i].call, d->addr);
		}
		else
			continue;
		if (trigger_date == 0)
			trigger_date = time(NULL) + RT_TRIGGER_DELAY;
	}
}

static void table_update(void)
{
	int i;

	for (i = 0; i < nb_dest; i++)
		dest_update(&dest[i]);
}

/************************************************************************
 * Sessions
 ************************************************************************/

static void neigh_flush(struct rt_neigh *v)
{
	int n = 0;

	while (n < v->nb_queue)
	{
		if (send(v->fd, v->queue[n], strlen(v->queue[n]), 0) < 0)
			break;
		free(v->queue[n++]);
	}
	if (n)
	{
		memmove(v->queue, v->queue + n, (v->nb_queue - n) * sizeof(v->queue[0]));
		v->nb_queue -= n;
	}
}

static void neigh_send(struct rt_neigh *v, const char *fmt, ...)
{
	va_list ap;
	char line[80];

	if (v->state != N_UP || v->nb_queue == RT_MAX_QUEUE)
		return;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line) - 1, fmt, ap);
	va_end(ap);
	if (verbose)
		rt_log(LOG_DEBUG, "> %s : %s", v->call, line);
	strcat(line, "\r");
	v->queue[v->nb_queue++] = strdup(line);
	neigh_flush(v);
}

/* When to call a neighbour again after a failed call or a lost session */
static time_t next_retry(struct rt_neigh *v)
{
	time_t now = time(NULL);

	if (v->state == N_UP && now - v->up_since >= RT_STABLE)
		v->failures = 0;
	switch (v->failures++) {
	case 0:
		return now + RT_RETRY_FIRST;
	case 1:
		return now + RT_RETRY_SECOND;
	default:
		v->failures = 2;
		return now + RT_RETRY;
	}
}

static void neigh_close(struct rt_neigh *v, const char *why)
{
	int idx = v - neigh;
	int i;

	if (v->fd >= 0)
		close(v->fd);
	v->retry = next_retry(v);
	if (v->state != N_DOWN)
		rt_log(LOG_INFO, "%s (%s) closed: %s, next call in %ld s", v->call, v->addr,
		       why, (long)(v->retry - time(NULL)));
	v->fd = -1;
	v->state = N_DOWN;
	v->hello = 0;
	v->incoming = 0;
	for (i = 0; i < v->nb_queue; i++)
		free(v->queue[i]);
	v->nb_queue = 0;

	/* Everything learned from this neighbour is lost */
	for (i = 0; i < nb_dest; i++)
	{
		dest[i].cost[idx] = 0;
		dest[i].missed[idx] = 0;
		dest[i].seen[idx] = 0;
		dest[i].sent[idx] = 0;
	}
	table_update();
}

/*
 * Announce the table to a neighbour: everything (full) or only what
 * changed since the last announcement. Split horizon: never to the
 * neighbour the destination was learned from, nor to the one the
 * kernel really uses for it.
 */
static void neigh_announce(struct rt_neigh *v, int full)
{
	int idx = v - neigh;
	char hop[10];
	int i, cost, n = 0;

	if (!rt_announce || v->state != N_UP || !v->hello)
		return;

	for (i = 0; i < nb_dest; i++)
	{
		struct rt_dest *d = &dest[i];

		cost = dest_cost(d);
		if (cost > RT_MAX_COST)
			cost = -1;
		if (cost >= 0 && !d->own && dest_best(d) == idx)
			cost = -1;
		if (cost >= 0 && d->adj == idx)
			cost = -1;
		if (cost >= 0 && (d->manual || (d->own && d->mask == 4))
		    && kernel_next_hop(d->addr, d->mask, hop, sizeof(hop))
		    && strcasecmp(hop, v->call) == 0)
			cost = -1;
		/* A DNIC this node is the exit of is announced only while its
		 * exit link is open, and never on the strength of the link with
		 * the neighbour it is announced to. */
		if (cost >= 0 && d->own && d->mask == 4
		    && !kernel_exit_open(d->addr, d->mask, v->call))
			cost = -1;

		if (cost >= 0)
		{
			if (full || d->sent[idx] != cost + 1)
				neigh_send(v, "R %s %d %d", d->addr, d->mask, cost);
			d->sent[idx] = cost + 1;
			n++;
		}
		else if (d->sent[idx])
		{
			neigh_send(v, "W %s %d", d->addr, d->mask);
			d->sent[idx] = 0;
		}
	}
	if (full)
	{
		neigh_send(v, "E %d", n);
		v->next_table = time(NULL) + RT_PERIOD;
		if (verbose)
			rt_log(LOG_INFO, "table sent to %s: %d routes", v->call, n);
	}
}

static void neigh_up(struct rt_neigh *v)
{
	v->state = N_UP;
	v->hello = 0;
	v->nb_rcvd = 0;
	v->last_table = time(NULL);
	v->up_since = v->last_table;
	rt_log(LOG_INFO, "%s (%s) connected%s", v->call, v->addr,
	       v->incoming ? " (incoming)" : "");
	neigh_send(v, "FPACRT %d %s", RT_VERSION, cfg.callsign);
}

/* One line received from a neighbour */
static void neigh_line(struct rt_neigh *v, char *line)
{
	int idx = v - neigh;
	struct rt_dest *d;
	char addr[16];
	int mask, cost, i, n;

	if (verbose)
		rt_log(LOG_DEBUG, "< %s : %s", v->call, line);

	if (!v->hello)
	{
		if (sscanf(line, "FPACRT %d", &n) != 1 || n != RT_VERSION)
		{
			neigh_close(v, "not a FPACRT peer of this version");
			return;
		}
		v->hello = 1;
		neigh_announce(v, 1);
		return;
	}

	switch (*line)
	{
	case 'R':
		if (sscanf(line, "R %10s %d %d", addr, &mask, &cost) != 3
		    || strlen(addr) != 10 || strspn(addr, "0123456789") != 10
		    || (mask != 10 && mask != 4) || cost < 0)
			break;
		v->nb_rcvd++;
		if (!learn_allowed(mask))
			break;
		if ((d = dest_find(addr, mask, 1)) == NULL || d->own)
			break;
		n = d->cost[idx];
		if (cost + 1 > RT_MAX_COST)
			d->cost[idx] = 0;
		else
			d->cost[idx] = cost + 1;
		/* A cost that changes through the neighbour already in use
		 * changes ours without changing the installed route: announce
		 * it too, or it waited for the next full table (30 minutes)
		 * and a loop lasted that long (simulation of 2 Nov 2026). */
		if (d->cost[idx] != n && trigger_date == 0)
			trigger_date = time(NULL) + RT_TRIGGER_DELAY;
		d->seen[idx] = 1;
		d->missed[idx] = 0;
		dest_update(d);
		break;
	case 'W':
		if (sscanf(line, "W %10s %d", addr, &mask) != 2)
			break;
		if ((d = dest_find(addr, mask, 0)) == NULL)
			break;
		d->cost[idx] = 0;
		dest_update(d);
		if (trigger_date == 0)
			trigger_date = time(NULL) + RT_TRIGGER_DELAY;
		break;
	case 'E':
		/* The periodic table left no trace in the log. The count is the
		 * one its sender gives: nb_rcvd also counts the R lines of the
		 * updates sent between two tables. */
		n = -1;
		if (verbose && sscanf(line, "E %d", &n) == 1)
			rt_log(LOG_INFO, "table from %s: %d routes", v->call, n);
		/* End of a full table: age what was not in it */
		for (i = 0; i < nb_dest; i++)
		{
			d = &dest[i];
			if (d->cost[idx] && !d->seen[idx]
			    && ++d->missed[idx] >= RT_MISSED_MAX)
			{
				d->cost[idx] = 0;
				d->missed[idx] = 0;
			}
			d->seen[idx] = 0;
		}
		v->nb_rcvd = 0;
		v->last_table = time(NULL);
		table_update();
		break;
	}
}

static void neigh_read(struct rt_neigh *v)
{
	char buf[512];
	char *p, *next;
	int len;

	len = recv(v->fd, buf, sizeof(buf) - 1, 0);
	if (len <= 0)
	{
		if (len < 0 && (errno == EAGAIN || errno == EINTR))
			return;
		neigh_close(v, len ? strerror(errno) : "disconnected");
		return;
	}
	buf[len] = '\0';
	for (p = buf; p && *p && v->state == N_UP; p = next)
	{
		if ((next = strpbrk(p, "\r\n")) != NULL)
			*next++ = '\0';
		if (*p)
			neigh_line(v, p);
	}
}

static int rose_sockaddr(struct full_sockaddr_rose *sa, const char *addr)
{
	memset(sa, 0, sizeof(*sa));
	sa->srose_family = AF_ROSE;
	sa->srose_ndigis = 0;
	if (ax25_aton_entry(RT_SERVICE, sa->srose_call.ax25_call) != 0)
		return -1;
	return rose_aton(addr, sa->srose_addr.rose_addr);
}

static void neigh_connect(struct rt_neigh *v)
{
	struct full_sockaddr_rose local, remote;
	int flag = 1;

	v->retry = time(NULL) + RT_RETRY;
	if (rose_sockaddr(&local, cfg.fulladdr) != 0 || rose_sockaddr(&remote, v->addr) != 0)
		return;
	if ((v->fd = socket(AF_ROSE, SOCK_SEQPACKET, 0)) < 0)
		return;
	if (bind(v->fd, (struct sockaddr *)&local, sizeof(local)) == -1
	    || ioctl(v->fd, FIONBIO, &flag) == -1
	    || (connect(v->fd, (struct sockaddr *)&remote, sizeof(remote)) == -1
		&& errno != EINPROGRESS))
	{
		if (verbose)
			rt_log(LOG_DEBUG, "%s (%s): connect: %s", v->call, v->addr, strerror(errno));
		close(v->fd);
		v->fd = -1;
		v->retry = next_retry(v);
		return;
	}
	v->state = N_CONNECTING;
	v->incoming = 0;
	v->deadline = time(NULL) + RT_CONNECT_TIMEOUT;
}

static void listen_accept(void)
{
	struct full_sockaddr_rose peer;
	socklen_t len = sizeof(peer);
	struct rt_neigh *v = NULL;
	char *addr;
	int fd, i, flag = 1;

	if ((fd = accept(listen_fd, (struct sockaddr *)&peer, &len)) < 0)
		return;
	addr = rose_ntoa(&peer.srose_addr);
	for (i = 0; i < nb_neigh; i++)
		if (strcmp(neigh[i].addr, addr) == 0)
			v = &neigh[i];

	if (v == NULL || v->excluded)
	{
		rt_log(LOG_INFO, "call from %s refused: not a declared neighbour", addr);
		close(fd);
		return;
	}
	/* Crossed calls: the session opened by the higher address is kept */
	if (v->state != N_DOWN && !v->incoming && strcmp(cfg.fulladdr, v->addr) > 0)
	{
		close(fd);
		return;
	}
	if (v->state != N_DOWN)
		neigh_close(v, "replaced by the incoming session");
	ioctl(fd, FIONBIO, &flag);
	v->fd = fd;
	v->incoming = 1;
	neigh_up(v);
}

static int listen_open(void)
{
	struct full_sockaddr_rose local;
	int fd;

	if (rose_sockaddr(&local, cfg.fulladdr) != 0)
		return -1;
	if ((fd = socket(AF_ROSE, SOCK_SEQPACKET, 0)) < 0)
		return -1;
	if (bind(fd, (struct sockaddr *)&local, sizeof(local)) == -1 || listen(fd, 5) == -1)
	{
		close(fd);
		return -1;
	}
	return fd;
}

/************************************************************************
 * Main loop
 ************************************************************************/

/* Does this node hold a route learned from this neighbour? */
static int neigh_has_routes(int idx)
{
	int i;

	for (i = 0; i < nb_dest; i++)
		if (dest[i].cost[idx])
			return 1;
	return 0;
}

/************************************************************************
 * Discovery of new nodes (FpacAnnounce, FpacDiscover)
 *
 * A node with FpacAnnounce = yes sends an FPAC announcement, a UI frame
 * with PID 0xF0 from its L3 callsign to QST-0, on the ports of its
 * neighbours: at start, then every 30 min. A node that hears an
 * announcement from a node it does not know, and runs with
 * FpacDiscover = yes (the default), accepts it provisionally: it sets the
 * kernel route to its address, calls it as a "Route = yes" neighbour and
 * answers with its own announcement, sent to the new node only. The sysop
 * can invalidate it later (fpacrtd -x CALL). With FpacDiscover = ask the
 * node waits for fpacrtd -a CALL; with no, it is only logged.
 *
 * The announcement:
 *	FPAC/1 L2=F4KLO-12 L3=F4KLO-13 NUA=2080175512 V=4.2-rc4 RT=1
 ************************************************************************/

struct rt_port {
	char	name[16];	/* AX.25 port, as in axports */
	char	call[10];	/* Its callsign, to bind on it */
	int	qst_fd;		/* Raw socket bound to QST-0: announcements */
	int	l3_fd;		/* Raw socket bound to our L3 call: direct replies */
	int	tx_fd;		/* Datagram socket bound to our L3 call: sending */
};

static struct rt_port ports[RT_MAX_PORTS];
static int nb_ports;
static time_t next_announce;

/* One line per callsign in a list file */
static int list_has(const char *file, const char *call)
{
	char line[128], word[16];
	FILE *fp;
	int found = 0;

	if ((fp = fopen(file, "r")) == NULL)
		return 0;
	while (!found && fgets(line, sizeof(line), fp))
		found = sscanf(line, "%15s", word) == 1 && strcasecmp(word, call) == 0;
	fclose(fp);
	return found;
}

/* Remove the lines of a callsign, keep the others */
static void list_remove(const char *file, const char *call)
{
	char line[128], word[16], tmp[256];
	FILE *in, *out;

	if ((in = fopen(file, "r")) == NULL)
		return;
	snprintf(tmp, sizeof(tmp), "%s.new", file);
	if ((out = fopen(tmp, "w")) == NULL)
	{
		fclose(in);
		return;
	}
	while (fgets(line, sizeof(line), in))
		if (sscanf(line, "%15s", word) != 1 || strcasecmp(word, call) != 0)
			fputs(line, out);
	fclose(in);
	fclose(out);
	rename(tmp, file);
}

static void list_add(const char *file, const char *line)
{
	FILE *fp;

	if ((fp = fopen(file, "a")) == NULL)
		return;
	fprintf(fp, "%s\n", line);
	fclose(fp);
}

static int ax25_bind_on(int fd, const char *call, const char *portcall)
{
	struct full_sockaddr_ax25 sa;

	memset(&sa, 0, sizeof(sa));
	sa.fsa_ax25.sax25_family = AF_AX25;
	sa.fsa_ax25.sax25_ndigis = 1;
	if (ax25_aton_entry(call, sa.fsa_ax25.sax25_call.ax25_call) != 0
	    || ax25_aton_entry(portcall, sa.fsa_digipeater[0].ax25_call) != 0)
		return -1;
	return bind(fd, (struct sockaddr *)&sa, sizeof(sa));
}

/* The ports of the neighbours, where announcements are sent and heard */
static void ports_open(void)
{
	int i, j, flag = 1;
	char *call, *name;

	for (i = 0; i < nb_neigh && nb_ports < RT_MAX_PORTS; i++)
	{
		struct rt_port *p = &ports[nb_ports];

		/* No Port in the Node block: the first port of axports, as for
		 * the route of the neighbour */
		name = neigh[i].port[0] ? neigh[i].port : ax25_config_get_next(NULL);
		if (name == NULL)
			continue;
		for (j = 0; j < nb_ports; j++)
			if (strcmp(ports[j].name, name) == 0)
				break;
		if (j < nb_ports)
			continue;
		if ((call = ax25_config_get_addr(name)) == NULL)
			continue;
		snprintf(p->name, sizeof(p->name), "%s", name);
		snprintf(p->call, sizeof(p->call), "%s", call);
		p->qst_fd = socket(AF_AX25, SOCK_RAW, AX25_P_TEXT);
		p->l3_fd = socket(AF_AX25, SOCK_RAW, AX25_P_TEXT);
		p->tx_fd = socket(AF_AX25, SOCK_DGRAM, 0);
		if (p->qst_fd < 0 || ax25_bind_on(p->qst_fd, "QST-0", p->call) != 0
		    || p->l3_fd < 0 || ax25_bind_on(p->l3_fd, cfg.callsign, p->call) != 0
		    || p->tx_fd < 0 || ax25_bind_on(p->tx_fd, cfg.callsign, p->call) != 0)
		{
			rt_log(LOG_WARNING, "port %s: cannot open the FPAC announcement sockets: %s",
			       p->name, strerror(errno));
			if (p->qst_fd >= 0)
				close(p->qst_fd);
			if (p->l3_fd >= 0)
				close(p->l3_fd);
			if (p->tx_fd >= 0)
				close(p->tx_fd);
			continue;
		}
		ioctl(p->qst_fd, FIONBIO, &flag);
		ioctl(p->l3_fd, FIONBIO, &flag);
		ioctl(p->tx_fd, FIONBIO, &flag);
		rt_log(LOG_INFO, "FPAC announcements heard%s on port %s (%s)",
		       fpac_announce ? " and sent" : "", p->name, p->call);
		nb_ports++;
	}
}

/* Send our announcement on one port, to QST-0 or to one node */
static void announce_send(struct rt_port *p, const char *to)
{
	struct full_sockaddr_ax25 sa;
	char text[200];
	int n;

	n = snprintf(text, sizeof(text), "FPAC/1 L2=%s L3=%s NUA=%s V=%s RT=%d\r",
		     cfg.alt_callsign, cfg.callsign, cfg.fulladdr, VERSION,
		     (rt_announce || rt_learn != LEARN_NO) ? 1 : 0);
	memset(&sa, 0, sizeof(sa));
	sa.fsa_ax25.sax25_family = AF_AX25;
	if (ax25_aton_entry(to, sa.fsa_ax25.sax25_call.ax25_call) != 0)
		return;
	if (sendto(p->tx_fd, text, n, 0, (struct sockaddr *)&sa, sizeof(struct sockaddr_ax25)) < 0)
		rt_log(LOG_WARNING, "port %s: FPAC announcement to %s: %s", p->name, to, strerror(errno));
	else if (verbose)
		rt_log(LOG_DEBUG, "port %s: FPAC announcement sent to %s", p->name, to);
}

static int nb_provisional(void)
{
	int i, n = 0;

	for (i = 0; i < nb_neigh; i++)
		n += neigh[i].provisional;
	return n;
}

static struct rt_neigh *neigh_by_call(const char *call)
{
	int i;

	for (i = 0; i < nb_neigh; i++)
		if (strcasecmp(neigh[i].call, call) == 0)
			return &neigh[i];
	return NULL;
}

/* Why this node cannot be accepted, or NULL */
static const char *disc_refusal(const char *call, const char *nua)
{
	struct rt_dest *d;
	int i;

	if (list_has(RT_REFUSED_FILE, call))
		return "invalidated by the sysop (fpacrtd -a to accept it again)";
	if (is_manual(nua, 10))
		return "its address has a route in fpac.routes";
	for (i = 0; i < nb_neigh; i++)
		if (!neigh[i].excluded && strcmp(neigh[i].addr, nua) == 0)
			return "its address belongs to another neighbour";
	if ((d = dest_find(nua, 10, 0)) != NULL)
		for (i = 0; i < nb_neigh; i++)
			if (d->cost[i])
				return "its address is already announced by another neighbour";
	for (i = 0; i < nb_neigh; i++)
		if (!neigh[i].excluded && in_neigh_block(nua, i))
			return "its address is in the block of another neighbour";
	if (nb_provisional() >= RT_DISC_MAX)
		return "too many nodes accepted provisionally";
	if (nb_neigh >= RT_MAX_NEIGH)
		return "no room for one more neighbour";
	return NULL;
}

/* Take a heard node as a provisional "Route = yes" neighbour */
static struct rt_neigh *neigh_add_heard(const char *call, const char *nua, const char *port)
{
	struct rt_neigh *v = neigh_by_call(call);

	if (v == NULL)
	{
		if (nb_neigh >= RT_MAX_NEIGH)
			return NULL;
		v = &neigh[nb_neigh++];
		memset(v, 0, sizeof(*v));
		v->fd = -1;
	}
	snprintf(v->name, sizeof(v->name), "*%s", call);
	snprintf(v->call, sizeof(v->call), "%s", call);
	snprintf(v->addr, sizeof(v->addr), "%s", nua);
	snprintf(v->port, sizeof(v->port), "%s", port);
	v->excluded = 0;
	v->provisional = 1;
	v->invalidated = 0;
	v->failures = 0;
	v->retry = time(NULL);
	neigh_route_set(v);
	return v;
}

/* The sysop invalidated a provisional neighbour */
static void neigh_drop_heard(struct rt_neigh *v)
{
	if (v->state != N_DOWN)
		neigh_close(v, "invalidated by the sysop");
	if (v->nr_dev[0])
		kernel_route(0, v->addr, 10, v->call, v->nr_dev);
	v->nr_dev[0] = '\0';
	v->excluded = 1;
	v->provisional = 0;
	v->invalidated = 1;
	rt_log(LOG_WARNING, "node %s %s invalidated by the sysop", v->call, v->addr);
}

/* Nodes accepted before a restart, and the decisions of fpacrtd -a / -x */
static void disc_lists_load(void)
{
	char line[128], call[16], nua[16], port[16];
	struct rt_neigh *v;
	FILE *fp;
	int i;

	for (i = 0; i < nb_neigh; i++)
		if (neigh[i].provisional && list_has(RT_REFUSED_FILE, neigh[i].call))
			neigh_drop_heard(&neigh[i]);
	if ((fp = fopen(RT_DISC_FILE, "r")) == NULL)
		return;
	while (fgets(line, sizeof(line), fp))
	{
		if (sscanf(line, "%15s %15s %15s", call, nua, port) != 3)
			continue;
		if (list_has(RT_REFUSED_FILE, call))
			continue;
		v = neigh_by_call(call);
		if (v != NULL && !v->provisional && !v->invalidated)
			continue;	/* in fpac.nodes: the sysop decided */
		if (v == NULL || !v->provisional)
		{
			if ((v = neigh_add_heard(call, nua, port)) != NULL)
				rt_log(LOG_WARNING, "node %s %s on %s accepted provisionally (fpacrtd -x %s to invalidate it)",
				       call, nua, port, call);
		}
	}
	fclose(fp);
}

/* An FPAC announcement was heard on a port */
static void disc_heard(struct rt_port *p, const char *from, char *text, int direct)
{
	char l3[16] = "", nua[16] = "", line[128];
	const char *why;
	struct rt_neigh *v;
	char *tok, *save = NULL;
	int rt = 0, i;
	time_t now = time(NULL);

	if (strncmp(text, "FPAC/1 ", 7) != 0)
		return;
	for (tok = strtok_r(text + 7, " \r\n", &save); tok; tok = strtok_r(NULL, " \r\n", &save))
	{
		if (strncmp(tok, "L3=", 3) == 0)
			snprintf(l3, sizeof(l3), "%s", tok + 3);
		else if (strncmp(tok, "NUA=", 4) == 0)
			snprintf(nua, sizeof(nua), "%s", tok + 4);
		else if (strncmp(tok, "RT=", 3) == 0)
			rt = atoi(tok + 3);
	}
	for (i = 0; l3[i]; i++)
		l3[i] = toupper((unsigned char)l3[i]);
	if (strlen(nua) != 10 || strspn(nua, "0123456789") != 10 || !*l3)
		return;
	/* The announcement is sent by the L3 callsign it gives */
	if (strcasecmp(l3, from) != 0)
		return;
	if (strcasecmp(l3, cfg.callsign) == 0 || strcmp(nua, cfg.fulladdr) == 0)
		return;

	v = neigh_by_call(l3);
	if (v != NULL && (!v->excluded || v->provisional))
	{
		/* Known neighbour: call it now if its session is down */
		if (v->state == N_DOWN && v->retry > now + 5)
		{
			rt_log(LOG_INFO, "%s (%s) heard on %s, calling it now", v->call, v->addr, p->name);
			v->retry = now;
			v->failures = 0;
		}
		if (fpac_announce && now - v->last_reply >= RT_ANN_REPLY)
		{
			v->last_reply = now;
			announce_send(p, l3);
		}
		return;
	}
	if (v != NULL && !v->invalidated)
		return;		/* in fpac.nodes without "Route = yes": the sysop decided */

	if (fpac_discover == DISC_NO || !rt)
	{
		if (verbose)
			rt_log(LOG_DEBUG, "FPAC node %s %s heard on %s%s", l3, nua, p->name,
			       rt ? "" : " (no fpacrtd)");
		return;
	}
	if ((why = disc_refusal(l3, nua)) != NULL)
	{
		rt_log(LOG_INFO, "FPAC node %s %s heard on %s, not accepted: %s", l3, nua, p->name, why);
		return;
	}
	snprintf(line, sizeof(line), "%s %s %s %ld", l3, nua, p->name, (long)now);
	if (fpac_discover == DISC_ASK)
	{
		if (!list_has(RT_PENDING_FILE, l3))
		{
			list_add(RT_PENDING_FILE, line);
			rt_log(LOG_WARNING, "FPAC node %s %s heard on %s: waiting for the sysop (fpacrtd -a %s)",
			       l3, nua, p->name, l3);
		}
		return;
	}
	if ((v = neigh_add_heard(l3, nua, p->name)) == NULL)
		return;
	list_remove(RT_DISC_FILE, l3);
	list_add(RT_DISC_FILE, line);
	rt_log(LOG_WARNING, "FPAC node %s %s heard on %s%s: accepted provisionally (fpacrtd -x %s to invalidate it)",
	       l3, nua, p->name, direct ? " (direct)" : "", l3);
	if (fpac_announce)
	{
		v->last_reply = now;
		announce_send(p, l3);
	}
}

static void disc_read(struct rt_port *p, int fd, int direct)
{
	struct full_sockaddr_ax25 from;
	socklen_t len = sizeof(from);
	char buf[300], call[16];
	int n, k;
	char *text;

	while ((n = recvfrom(fd, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &len)) > 0)
	{
		buf[n] = '\0';
		/*
		 * A raw socket also gets what is left of the control byte and
		 * the PID: ax25_recvmsg() removes one byte, the control byte of
		 * a UI frame, and keeps the PID. Look for the keyword in the
		 * first bytes rather than count them.
		 */
		for (k = 0; k < 3 && k < n; k++)
			if (strncmp(buf + k, "FPAC/1 ", 7) == 0)
				break;
		if (k == 3 || k >= n)
			continue;
		text = buf + k;
		snprintf(call, sizeof(call), "%s", ax25_ntoa(&from.fsa_ax25.sax25_call));
		disc_heard(p, call, text, direct);
		len = sizeof(from);
	}
}

/* fpacrtd -l, -a CALL, -x CALL */
static void show_file(const char *title, const char *file)
{
	char line[128];
	FILE *fp;

	printf("%s (%s):\n", title, file);
	if ((fp = fopen(file, "r")) == NULL)
		return;
	while (fgets(line, sizeof(line), fp))
		printf("  %s", line);
	fclose(fp);
}

static int sysop_command(int cmd, const char *arg)
{
	char call[16], line[128], word[16], nua[16], port[16];
	long when;
	FILE *fp;
	int i, n, found = 0;

	if (cmd == 'l')
	{
		show_file("Accepted provisionally", RT_DISC_FILE);
		show_file("Waiting for the sysop", RT_PENDING_FILE);
		show_file("Invalidated", RT_REFUSED_FILE);
		return 0;
	}
	snprintf(call, sizeof(call), "%s", arg);
	for (i = 0; call[i]; i++)
		call[i] = toupper((unsigned char)call[i]);
	if (cmd == 'x')
	{
		list_remove(RT_DISC_FILE, call);
		list_remove(RT_PENDING_FILE, call);
		if (!list_has(RT_REFUSED_FILE, call))
			list_add(RT_REFUSED_FILE, call);
		printf("%s invalidated\n", call);
	}
	else
	{
		list_remove(RT_REFUSED_FILE, call);
		if ((fp = fopen(RT_PENDING_FILE, "r")) != NULL)
		{
			while (!found && fgets(line, sizeof(line), fp))
				if (sscanf(line, "%15s %15s %15s %ld", word, nua, port, &when) == 4
				    && strcasecmp(word, call) == 0)
					found = 1;
			fclose(fp);
		}
		if (found)
		{
			list_remove(RT_PENDING_FILE, call);
			list_remove(RT_DISC_FILE, call);
			snprintf(line, sizeof(line), "%s %s %s %ld", call, nua, port, (long)time(NULL));
			list_add(RT_DISC_FILE, line);
			printf("%s %s accepted\n", call, nua);
		}
		else
			printf("%s may be accepted again when it is heard\n", call);
	}
	/* Tell the running daemon */
	if ((fp = fopen(RT_LOCK_FILE, "r")) != NULL)
	{
		if (fscanf(fp, "%d", &n) == 1 && n > 0)
			kill(n, SIGUSR1);
		fclose(fp);
	}
	return 0;
}

static void on_signal(int sig)
{
	if (sig == SIGUSR1)
		lists_changed = 1;
	else
		stop = 1;
}

static void timers(void)
{
	static time_t next_check;
	time_t now = time(NULL);
	int i;

	if (fpac_announce && nb_ports && now >= next_announce)
	{
		for (i = 0; i < nb_ports; i++)
			announce_send(&ports[i], "QST-0");
		next_announce = now + RT_ANN_PERIOD - RT_ANN_JITTER
				+ rand() % (2 * RT_ANN_JITTER + 1);
	}

	for (i = 0; i < nb_neigh; i++)
	{
		struct rt_neigh *v = &neigh[i];

		if (v->excluded)
			continue;
		if (v->state == N_DOWN && now >= v->retry)
			neigh_connect(v);
		else if (v->state == N_CONNECTING && now >= v->deadline)
			neigh_close(v, "connect timeout");
		else if (v->state == N_UP && v->hello && rt_announce && now >= v->next_table)
			neigh_announce(v, 1);

		/* Ageing only counts the tables that come in: a neighbour that
		 * stops sending them while its circuit stays open (daemon hung)
		 * would keep its routes here for ever. After the time of
		 * RT_MISSED_MAX tables without one, close the session: its
		 * routes are dropped and it is called again later. A neighbour
		 * that never announced anything is silent by design
		 * (RouteAnnounce = no) and is left alone. */
		if (v->state == N_UP && now - v->last_table >= RT_MISSED_MAX * RT_PERIOD
		    && neigh_has_routes(i))
			neigh_close(v, "no table received for too long");
	}

	/* Withdrawals have gone round: forget the reference distance of a
	 * destination without a route, so it can come back. Checked at every
	 * turn: below 20 s loops came back in the simulation, RT_HOLD keeps
	 * a margin of 3. */
	for (i = 0; i < nb_dest; i++)
		if (dest[i].fd && dest[i].lost && now - dest[i].lost >= RT_HOLD)
		{
			dest[i].fd = 0;
			dest[i].lost = 0;
			dest_update(&dest[i]);
		}

	/* A neighbour that closed or reopened changes what is installed */
	if (now >= next_check)
	{
		next_check = now + RT_CHECK_PERIOD;
		/* A route this daemon installed may have left the kernel behind
		 * its back (fpad restarted, sysop command): forget it, so that
		 * table_update() puts it back. */
		for (i = 0; i < nb_dest; i++)
		{
			char hop[10];

			if (dest[i].installed < 0 || dry_run)
				continue;
			if (!kernel_next_hop(dest[i].addr, dest[i].mask, hop, sizeof(hop))
			    || strcasecmp(hop, dest[i].inst_call) != 0)
			{
				rt_log(LOG_WARNING, "route %.*s/%d via %s is no longer in the kernel",
				       dest[i].mask, dest[i].addr, dest[i].mask, dest[i].inst_call);
				dest[i].installed = -1;
			}
		}
		if (rt_wp)
			wp_scan();
		adj_scan();
		table_update();

		/* The exit link of a DNIC this node is the origin of opened or
		 * closed: announce or withdraw the DNIC without waiting for the
		 * next full table. */
		for (i = 0; i < nb_dest; i++)
		{
			int open;

			if (!dest[i].own || dest[i].mask != 4)
				continue;
			open = kernel_exit_open(dest[i].addr, dest[i].mask, NULL);
			if (open == dest[i].exit_open)
				continue;
			rt_log(LOG_INFO, "exit link of %.*s/%d %s: DNIC %s", dest[i].mask, dest[i].addr,
			       dest[i].mask, open ? "open" : "closed",
			       !rt_announce ? "not announced (RouteAnnounce = no)"
			       : open ? "announced" : "withdrawn");
			dest[i].exit_open = open;
			if (trigger_date == 0)
				trigger_date = now + RT_TRIGGER_DELAY;
		}
	}

	if (trigger_date && now >= trigger_date)
	{
		trigger_date = 0;
		for (i = 0; i < nb_neigh; i++)
			neigh_announce(&neigh[i], 0);
	}
}

static void main_loop(void)
{
	fd_set rfds, wfds;
	struct timeval tv;
	int i, maxfd, err;
	socklen_t len;

	while (!stop)
	{
		/* Before the sets are built: it may close sessions */
		if (lists_changed)
		{
			lists_changed = 0;
			disc_lists_load();
		}
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		FD_SET(listen_fd, &rfds);
		maxfd = listen_fd;
		for (i = 0; i < nb_neigh; i++)
		{
			struct rt_neigh *v = &neigh[i];

			if (v->fd < 0)
				continue;
			if (v->state == N_UP)
				FD_SET(v->fd, &rfds);
			if (v->state == N_CONNECTING || v->nb_queue)
				FD_SET(v->fd, &wfds);
			if (v->fd > maxfd)
				maxfd = v->fd;
		}
		for (i = 0; i < nb_ports; i++)
		{
			FD_SET(ports[i].qst_fd, &rfds);
			FD_SET(ports[i].l3_fd, &rfds);
			FD_SET(ports[i].tx_fd, &rfds);
			if (ports[i].qst_fd > maxfd)
				maxfd = ports[i].qst_fd;
			if (ports[i].l3_fd > maxfd)
				maxfd = ports[i].l3_fd;
			if (ports[i].tx_fd > maxfd)
				maxfd = ports[i].tx_fd;
		}
		tv.tv_sec = 5;
		tv.tv_usec = 0;
		if (select(maxfd + 1, &rfds, &wfds, NULL, &tv) < 0)
		{
			if (errno == EINTR)
				continue;
			break;
		}
		if (FD_ISSET(listen_fd, &rfds))
			listen_accept();
		for (i = 0; i < nb_ports; i++)
		{
			char junk[300];

			if (FD_ISSET(ports[i].qst_fd, &rfds))
				disc_read(&ports[i], ports[i].qst_fd, 0);
			if (FD_ISSET(ports[i].l3_fd, &rfds))
				disc_read(&ports[i], ports[i].l3_fd, 1);
			/* The sending socket also gets the UI frames for our L3
			 * call: the raw socket above reads them */
			if (FD_ISSET(ports[i].tx_fd, &rfds))
				while (recv(ports[i].tx_fd, junk, sizeof(junk), 0) > 0)
					;
		}
		for (i = 0; i < nb_neigh; i++)
		{
			struct rt_neigh *v = &neigh[i];

			if (v->fd < 0)
				continue;
			if (v->state == N_CONNECTING && FD_ISSET(v->fd, &wfds))
			{
				err = 0;
				len = sizeof(err);
				getsockopt(v->fd, SOL_SOCKET, SO_ERROR, &err, &len);
				if (err) {
					struct rose_cause_struct rc;
					char why[80];

					/* Cause and diagnostic of the Clear Request, if any */
					memset(&rc, 0, sizeof(rc));
					if (ioctl(v->fd, SIOCRSGCAUSE, &rc) == 0 && (rc.cause || rc.diagnostic))
						snprintf(why, sizeof(why), "%s (cause 0x%02x, diagnostic 0x%02x)",
							 strerror(err), rc.cause, rc.diagnostic);
					else
						snprintf(why, sizeof(why), "%s", strerror(err));
					neigh_close(v, why);
				} else
					neigh_up(v);
				continue;
			}
			if (v->state == N_UP && FD_ISSET(v->fd, &wfds))
				neigh_flush(v);
			if (v->state == N_UP && FD_ISSET(v->fd, &rfds))
				neigh_read(v);
		}
		timers();
	}
}

static void show_config(void)
{
	int i;

	rt_log(LOG_INFO, "node %s %s: announce %s, learn %s, White Pages %s, direct neighbours %s%s",
	       cfg.callsign, cfg.fulladdr, rt_announce ? "yes" : "no",
	       rt_learn == LEARN_ALL ? "all" : rt_learn == LEARN_NODES ? "nodes" : "no",
	       rt_wp ? "yes" : "no", rt_neighbours ? "yes" : "no", dry_run ? ", dry run" : "");
	rt_log(LOG_INFO, "FPAC announcement %s, discovery of new nodes %s",
	       fpac_announce ? "yes" : "no",
	       fpac_discover == DISC_YES ? "yes" : fpac_discover == DISC_ASK ? "ask" : "no");
	for (i = 0; i < nb_neigh; i++)
		rt_log(LOG_INFO, "neighbour %-9s %s%s", neigh[i].call, neigh[i].addr,
		       neigh[i].excluded ? " (not taking part)" : " (Route = yes)");
	for (i = 0; i < nb_dest; i++)
		if (dest[i].own && dest[i].mask == 4)
			rt_log(LOG_INFO, "origin of %.*s/%d, exit link %s", dest[i].mask, dest[i].addr,
			       dest[i].mask,
			       kernel_exit_open(dest[i].addr, dest[i].mask, NULL) ? "open" : "closed");
		else if (dest[i].own)
			rt_log(LOG_INFO, "origin of %.*s/%d", dest[i].mask, dest[i].addr, dest[i].mask);
}

/* Only one fpacrtd per node: a second one would purge the routes of the
 * first (state_purge) and both would call the same neighbours. The lock is
 * held on an open file for the whole life of the daemon, so the kernel
 * drops it whenever the process ends, even on kill -9: no stale pid file.
 * Returns 0 when the lock is ours, -1 when another fpacrtd holds it. */
static int single_instance(void)
{
	static int lock_fd = -1;
	char buf[32];
	int n;

	if ((lock_fd = open(RT_LOCK_FILE, O_RDWR | O_CREAT | O_CLOEXEC, 0644)) < 0)
	{
		rt_log(LOG_ERR, "cannot open %s: %s", RT_LOCK_FILE, strerror(errno));
		return -1;
	}
	if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0)
	{
		n = read(lock_fd, buf, sizeof(buf) - 1);
		buf[n > 0 ? n : 0] = 0;
		rt_log(LOG_ERR, "another fpacrtd is already running (pid %d): not started", atoi(buf));
		if (!foreground)
			fprintf(stderr, "fpacrtd: another fpacrtd is already running (pid %d): not started\n", atoi(buf));
		close(lock_fd);
		lock_fd = -1;
		return -1;
	}
	return 0;
}

/* The pid in the lock file is only there to name the holder in the message
 * above; it is written once the final process (after daemon()) is known. */
static void single_instance_pid(void)
{
	char buf[32];
	int fd, n;

	if ((fd = open(RT_LOCK_FILE, O_WRONLY | O_TRUNC)) < 0)
		return;
	n = snprintf(buf, sizeof(buf), "%d\n", (int) getpid());
	if (write(fd, buf, n) != n)
		rt_log(LOG_WARNING, "cannot write %s: %s", RT_LOCK_FILE, strerror(errno));
	close(fd);
}

int main(int argc, char **argv)
{
	int c, i;

	while ((c = getopt(argc, argv, "c:fntvhla:x:")) != -1)
	{
		switch (c)
		{
		case 'l':
			return sysop_command('l', NULL);
		case 'a':
		case 'x':
			return sysop_command(c, optarg);
		case 'c':
			extra_conf = optarg;
			break;
		case 'f':
			foreground = 1;
			break;
		case 'n':
			dry_run = 1;
			break;
		case 't':
			check_only = foreground = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		default:
			fprintf(stderr, "Usage: fpacrtd [-c file] [-f] [-n] [-t] [-v]\n"
				"       fpacrtd -l | -a CALL | -x CALL\n"
				"  -l  list the nodes heard: accepted, waiting, invalidated\n"
				"  -a  accept a node (FpacDiscover = ask), or again after -x\n"
				"  -x  invalidate a node accepted provisionally\n"
				"  -c  also read the Route keywords of this file\n"
				"  -f  stay in the foreground, log to stderr\n"
				"  -n  dry run: never change the kernel routes\n"
				"  -t  show the configuration and exit\n"
				"  -v  verbose\n");
			return 1;
		}
	}

	openlog("fpacrtd", LOG_PID, LOG_DAEMON);

	if (config_load() != 0)
		return 1;
	show_config();
	if (check_only)
		return 0;

	if (!rt_announce && rt_learn == LEARN_NO && !rt_wp && !fpac_announce
	    && fpac_discover == DISC_NO)
	{
		rt_log(LOG_INFO, "none of RouteAnnounce, RouteLearn, RouteWp, FpacAnnounce, FpacDiscover is set in fpac.conf: nothing to do");
		return 0;
	}

	/* Before daemon(), so that the refusal also shows on the terminal, and
	 * before state_purge(), which would remove the routes of the running
	 * daemon. The lock follows the open file across the fork of daemon(). */
	if (single_instance() != 0)
		return 1;

	if (!foreground && daemon(0, 0) != 0)
	{
		perror("fpacrtd: daemon");
		return 1;
	}
	single_instance_pid();

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGUSR1, on_signal);

	state_purge();
	neigh_routes_set();
	ports_open();
	disc_lists_load();
	srand(time(NULL) ^ getpid());
	next_announce = time(NULL) + 5;

	if ((listen_fd = listen_open()) < 0)
	{
		rt_log(LOG_ERR, "cannot listen on %s @ %s: %s", RT_SERVICE, cfg.fulladdr, strerror(errno));
		return 1;
	}
	rt_log(LOG_WARNING, "Starting version %s (built %s-%s) protocol %d", VERSION,
	       __DATE__, __TIME__, RT_VERSION);

	main_loop();

	/* Leave nothing behind in the kernel */
	for (i = 0; i < nb_neigh; i++)
		if (neigh[i].state != N_DOWN)
			neigh_close(&neigh[i], "fpacrtd stops");
	for (i = 0; i < nb_dest; i++)
		if (dest[i].installed >= 0)
			kernel_route(0, dest[i].addr, dest[i].mask, dest[i].inst_call, dest[i].inst_dev);
	if (!dry_run)
		unlink(RT_STATE_FILE);
	close(listen_fd);
	rt_log(LOG_WARNING, "stopped");
	return 0;
}
