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
#include <netax25/procutils.h>

#include "config.h"
#include "fpac.h"
#include "pathnames.h"

#define RT_VERSION		1
/* ROSE service callsign. Not "ROUTE": fpacroute (the traceroute
 * application) already listens on it on every FPAC node. */
#define RT_SERVICE		"FPACRT"
#define RT_MAX_COST		6		/* Hops ceiling */
#define RT_PERIOD		(30 * 60)	/* Full table period */
#define RT_RETRY		(30 * 60)	/* Retry a refused neighbour */
#define RT_CONNECT_TIMEOUT	120
#define RT_START_DELAY		30		/* Before the first call */
#define RT_MISSED_MAX		3		/* Tables without the route */
#define RT_TRIGGER_DELAY	10		/* Groups triggered updates */
#define RT_CHECK_PERIOD		60		/* Neighbours state check */

#define RT_MAX_NEIGH		32
#define RT_MAX_DEST		512
#define RT_MAX_QUEUE		1024

#define RT_STATE_FILE		AX25_LOCALSTATEDIR "fpac/fpacrt.dat"
#define RT_LOCK_FILE		AX25_LOCALSTATEDIR "fpac/fpacrtd.lock"

#define LEARN_NO	0
#define LEARN_NODES	1
#define LEARN_ALL	2

#define N_DOWN		0
#define N_CONNECTING	1
#define N_UP		2

struct rt_neigh {
	char	name[20];
	char	call[10];	/* L3 callsign of the neighbour */
	char	addr[11];	/* Full ROSE address */
	int	excluded;	/* No "Route = yes" in its Node block */
	int	fd;
	int	state;
	int	incoming;	/* Session opened by the neighbour */
	int	hello;		/* FPACRT line received */
	int	nb_rcvd;	/* R lines since the last E */
	time_t	retry;
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
	int	installed;	/* Neighbour index of the kernel route, -1 = none */
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
	dest[nb_dest].installed = -1;
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

/* Best neighbour for a destination, -1 when none */
static int dest_best(struct rt_dest *d)
{
	int i, best = -1;

	for (i = 0; i < nb_neigh; i++)
	{
		if (neigh[i].state != N_UP || d->cost[i] == 0)
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
	if ((best = dest_best(d)) < 0)
		return -1;
	if (d->installed < 0 && !d->manual)
		return -1;
	return d->cost[best];
}

static void dest_update(struct rt_dest *d)
{
	char dev[16] = "";
	int want = -1;
	int best;

	if (!d->own && !d->manual && learn_allowed(d->mask))
	{
		best = dest_best(d);
		if (best >= 0 && kernel_neigh_open(neigh[best].call, dev, sizeof(dev)))
			want = best;
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

static void neigh_close(struct rt_neigh *v, const char *why)
{
	int idx = v - neigh;
	int i;

	if (v->fd >= 0)
		close(v->fd);
	if (v->state != N_DOWN)
		rt_log(LOG_INFO, "%s (%s) closed: %s", v->call, v->addr, why);
	v->fd = -1;
	v->state = N_DOWN;
	v->hello = 0;
	v->incoming = 0;
	v->retry = time(NULL) + RT_RETRY;
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
		if (cost + 1 > RT_MAX_COST)
			d->cost[idx] = 0;
		else
			d->cost[idx] = cost + 1;
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

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static void timers(void)
{
	static time_t next_check;
	time_t now = time(NULL);
	int i;

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
				if (err)
					neigh_close(v, strerror(err));
				else
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

	rt_log(LOG_INFO, "node %s %s: announce %s, learn %s%s", cfg.callsign, cfg.fulladdr,
	       rt_announce ? "yes" : "no",
	       rt_learn == LEARN_ALL ? "all" : rt_learn == LEARN_NODES ? "nodes" : "no",
	       dry_run ? ", dry run" : "");
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

	while ((c = getopt(argc, argv, "c:fntvh")) != -1)
	{
		switch (c)
		{
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

	if (!rt_announce && rt_learn == LEARN_NO)
	{
		rt_log(LOG_INFO, "neither RouteAnnounce nor RouteLearn is set in fpac.conf: nothing to do");
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

	state_purge();

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
