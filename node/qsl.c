/*
 * qsl.c -- QSl command of fpacnode: a QSL card confirming the connection of
 * the user to this node, in the manner of the card sent after a radio contact.
 *
 * F6BVP 2026-10-13.  The card is first shown as text in the session, which
 * works everywhere, by radio as well.  When the sysop has set QslDir and
 * QslUrl, the helper fpacqsl also draws it as a JPEG image -- the picture of
 * the node on the front (QslImage, QSL_fpac.jpg by default), the text on the
 * back -- in a directory served by a web server, and the user is given the
 * link.  A session only carries text: the image cannot go through it.
 *
 * Keywords, read here in fpac.conf and then in fpacqsl.conf (same directory,
 * optional), so that libcfg and the other programs are left alone:
 *   QslDir   = directory where the cards are written
 *   QslUrl   = address under which that directory is seen from the web
 *   QslImage = picture of the front side
 *   QslKeep  = days a card is kept in QslDir (30 by default, 0 = for ever)
 *
 * The cards older than QslKeep are erased at each QSl command, a user who
 * asks again within QSL_REUSE seconds is given the card already made, and
 * no image is made while QslDir holds QSL_MAX_FILES cards.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <dirent.h>

#include "config.h"
#include "node.h"
#include "io.h"
#include "pathnames.h"

#define QSL_CONF	AX25_SYSCONFDIR "fpacqsl.conf"
#define QSL_IMAGE	AX25_SYSCONFDIR "QSL_fpac.jpg"
#define QSL_HELPER	"/usr/local/sbin/fpacqsl"
#define QSL_LOG		AX25_LOCALSTATEDIR "fpac/qsl.log"
#define QSL_TIMEOUT	30		/* Seconds left to the helper */
#define QSL_KEEP	30		/* Days a card stays in QslDir */
#define QSL_REUSE	600		/* Seconds: same card for the same user */
#define QSL_MAX_FILES	500		/* Cards in QslDir: no more images */
#define QSL_MAX_LINES	32
#define QSL_WIDTH	64

static char qsl_dir[200];
static char qsl_url[200];
static char qsl_image[200];
static int qsl_keep = QSL_KEEP;

static char qsl_line[QSL_MAX_LINES][QSL_WIDTH + 1];
static int qsl_nb;

static void qsl_add(const char *label, const char *fmt, ...)
{
	char val[128];
	va_list ap;

	if (qsl_nb == QSL_MAX_LINES)
		return;
	va_start(ap, fmt);
	vsnprintf(val, sizeof(val), fmt, ap);
	va_end(ap);
	if (*label)
		snprintf(qsl_line[qsl_nb], sizeof(qsl_line[0]), "%-16s : %.44s", label, val);
	else
		snprintf(qsl_line[qsl_nb], sizeof(qsl_line[0]), "%.63s", val);
	qsl_nb++;
}

/* "Keyword = value" lines; a later file overrides an earlier one */
static void qsl_conf_scan(const char *file)
{
	FILE *fp;
	char line[300], *key, *val, *p;

	if ((fp = fopen(file, "r")) == NULL)
		return;
	while (fgets(line, sizeof(line), fp) != NULL)
	{
		if ((p = strchr(line, '#')) != NULL)
			*p = '\0';
		if ((val = strchr(line, '=')) == NULL)
			continue;
		*val++ = '\0';
		for (key = line; isspace((unsigned char)*key); key++)
			;
		for (p = key + strlen(key); p > key && isspace((unsigned char)p[-1]); )
			*--p = '\0';
		while (isspace((unsigned char)*val))
			val++;
		for (p = val + strlen(val); p > val && isspace((unsigned char)p[-1]); )
			*--p = '\0';
		if (strcasecmp(key, "qsldir") == 0)
			snprintf(qsl_dir, sizeof(qsl_dir), "%s", val);
		else if (strcasecmp(key, "qslurl") == 0)
			snprintf(qsl_url, sizeof(qsl_url), "%s", val);
		else if (strcasecmp(key, "qslimage") == 0)
			snprintf(qsl_image, sizeof(qsl_image), "%s", val);
		else if (strcasecmp(key, "qslkeep") == 0 && isdigit((unsigned char)*val))
			qsl_keep = atoi(val);
	}
	fclose(fp);
}

/* The lines of the card: who, when, then what Status shows down to the
 * FPAC version (its uptime and the following lines are left out) */
static void qsl_build(const char *user, time_t now)
{
	struct utsname name;
	struct tm *tm;
	char userports[160];
	char when[40], since[40];
	port_t *pp;

	qsl_nb = 0;

	userports[0] = '\0';
	for (pp = cfg.port; pp != NULL; pp = pp->next)
	{
		if (userports[0] != '\0')
			strncat(userports, " ", sizeof(userports) - strlen(userports) - 1);
		strncat(userports, pp->name, sizeof(userports) - strlen(userports) - 1);
	}
	if (userports[0] == '\0')
		strcpy(userports, "-");

	tm = gmtime(&now);
	strftime(when, sizeof(when), "%Y-%m-%d %H:%M UTC", tm);
	tm = gmtime(&User.logintime);
	strftime(since, sizeof(since), "%Y-%m-%d %H:%M UTC", tm);

	qsl_add("To", "%s", user);
	qsl_add("Date and time", "%s", when);
	qsl_add("Connected since", "%s", since);
	qsl_add("", "%s", "");
	if (uname(&name) == -1)
		memset(&name, 0, sizeof(name));
	qsl_add("Hostname", "%s", name.nodename);
	qsl_add("L3 callsign", "%s", cfg.callsign);
	qsl_add("L2 callsign", "%s", cfg.alt_callsign);
	qsl_add("Traceroute call", "%s", cfg.trt_callsign);
	qsl_add("DNIC, address", "%s,%s", cfg.dnic, cfg.address);
	if (cfg.inetport != 0)
		qsl_add("UDP/TCP/IP port", "%d", cfg.inetport);
	qsl_add("Default port", "%s", cfg.def_port);
	qsl_add("User port", "%s", userports);
	qsl_add("Connected via", "%s", User.ul_name);
	qsl_add("Inet address", "%s", cfg.def_addr);
	qsl_add("City", "%s", cfg.city);
	qsl_add("Zip - State", "%s", cfg.state);
	qsl_add("Country", "%s", cfg.country);
	qsl_add("Locator", "%s", cfg.locator);
	qsl_add("", "%s", "");
	qsl_add("Operating system", "%s %s (%s)", name.sysname, name.release, name.machine);
	qsl_add("FPAC version", "%s", VERSION);
}

/* Runs the helper: 0 when the image was written */
static int qsl_draw(const char *data, const char *image)
{
	void (*old)(int);
	struct stat st;
	pid_t pid, r;
	int status = -1, i, rc = -1;

	if (access(QSL_HELPER, X_OK) != 0)
		return -1;
	unlink(image);
	/* ax25d starts fpacnode with SIGCHLD ignored: the kernel then reaps
	 * the children by itself and waitpid() never sees the helper end */
	old = signal(SIGCHLD, SIG_DFL);
	if ((pid = fork()) < 0)
	{
		signal(SIGCHLD, old);
		return -1;
	}
	if (pid == 0)
	{
		int fd;

		for (fd = 0; fd < 64; fd++)
			close(fd);
		execl(QSL_HELPER, "fpacqsl", data, qsl_image, image, (char *)NULL);
		_exit(127);
	}
	for (i = 0; i < QSL_TIMEOUT * 10; i++)
	{
		if ((r = waitpid(pid, &status, WNOHANG)) == pid)
		{
			if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
				rc = 0;
			break;
		}
		if (r < 0)		/* Already gone */
			break;
		usleep(100000);
	}
	if (i == QSL_TIMEOUT * 10)
	{
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
	}
	signal(SIGCHLD, old);
	/* The helper writes the image under another name and renames it at
	 * the very end: the file is there only when the image is complete */
	if (i < QSL_TIMEOUT * 10 && stat(image, &st) == 0 && st.st_size > 0)
		rc = 0;
	else
		rc = -1;
	return rc;
}

/*
 * Housekeeping of QslDir, where only the "QSL-*.jpg" files are looked at:
 * erases the cards older than QslKeep days and returns how many are left.
 * When this user was given a card less than QSL_REUSE seconds ago, its
 * name is copied to recent[] (empty string otherwise).
 */
static int qsl_housekeeping(const char *prefix, time_t now, char *recent, size_t len)
{
	DIR *dir;
	struct dirent *de;
	struct stat st;
	char path[520];
	time_t best = 0;
	size_t l;
	int count = 0;

	recent[0] = '\0';
	if ((dir = opendir(qsl_dir)) == NULL)
		return 0;
	while ((de = readdir(dir)) != NULL)
	{
		l = strlen(de->d_name);
		if (strncmp(de->d_name, "QSL-", 4) != 0 || l < 9 || l > 200
		    || strcmp(de->d_name + l - 4, ".jpg") != 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s", qsl_dir, de->d_name);
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		if (qsl_keep > 0 && now - st.st_mtime > (time_t)qsl_keep * 86400L)
		{
			unlink(path);
			continue;
		}
		count++;
		/* prefix + "YYYYMMDD-HHMM.jpg": F6BVP must not match F6BVP-2 */
		if (l == strlen(prefix) + 17 && strncmp(de->d_name, prefix, strlen(prefix)) == 0
		    && now - st.st_mtime < QSL_REUSE && st.st_mtime > best)
		{
			best = st.st_mtime;
			snprintf(recent, len, "%s", de->d_name);
		}
	}
	closedir(dir);
	return count;
}

int do_qsl(int argc, char **argv)
{
	char user[16], node[16], heading[100], stamp[20];
	char data[] = "/tmp/fpacqsl.XXXXXX";
	char file[210], path[420], prefix[60];
	int cards;
	time_t now = time(NULL);
	struct tm *tm;
	FILE *fp;
	int i, fd, drawn = 0;
	size_t n;

	if ((argc > 1) && (*argv[1] == '?'))
	{
		node_msg("usage : qsl");
		return 0;
	}

	/* Callsigns are also used in a file name: letters, digits and "-" only */
	for (i = 0, n = 0; User.call[i] && n < sizeof(user) - 1; i++)
		if (isalnum((unsigned char)User.call[i]) || User.call[i] == '-')
			user[n++] = toupper((unsigned char)User.call[i]);
	user[n] = '\0';
	for (i = 0, n = 0; cfg.callsign[i] && n < sizeof(node) - 1; i++)
		if (isalnum((unsigned char)cfg.callsign[i]) || cfg.callsign[i] == '-')
			node[n++] = toupper((unsigned char)cfg.callsign[i]);
	node[n] = '\0';
	if (!*user)
	{
		node_msg(T("QSL: no callsign for this session"));
		return 0;
	}

	qsl_dir[0] = qsl_url[0] = '\0';
	qsl_keep = QSL_KEEP;
	snprintf(qsl_image, sizeof(qsl_image), "%s", QSL_IMAGE);
	qsl_conf_scan(FPACCONF);
	qsl_conf_scan(QSL_CONF);

	qsl_build(user, now);
	snprintf(heading, sizeof(heading), "FPAC node %s confirming connection with %s", node, user);

	/* The card as text, always */
	tprintf("\n+%.*s+\n", QSL_WIDTH, "----------------------------------------------------------------");
	tprintf("  QSL   %s\n\n", heading);
	for (i = 0; i < qsl_nb; i++)
		tprintf("  %s\n", qsl_line[i]);
	tprintf("  73 !\n");
	tprintf("+%.*s+\n", QSL_WIDTH, "----------------------------------------------------------------");

	/* The card as an image, when the sysop has set it up */
	if (*qsl_dir && *qsl_url)
	{
		usflush(STDIN_FILENO);	/* The text first: drawing takes a few seconds */
		snprintf(prefix, sizeof(prefix), "QSL-%s-%s-", node, user);
		cards = qsl_housekeeping(prefix, now, file, sizeof(file));
		if (*file)
			drawn = 1;	/* Asked again: the card already made */
		else
		{
			tm = gmtime(&now);
			strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M", tm);
			snprintf(file, sizeof(file), "QSL-%s-%s-%s.jpg", node, user, stamp);
		}
		snprintf(path, sizeof(path), "%s/%s", qsl_dir, file);

		if (!drawn && cards < QSL_MAX_FILES
		    && (fd = mkstemp(data)) >= 0 && (fp = fdopen(fd, "w")) != NULL)
		{
			fprintf(fp, "%s\n", heading);
			for (i = 0; i < qsl_nb; i++)
				fprintf(fp, "%s\n", qsl_line[i]);
			fclose(fp);
			drawn = (qsl_draw(data, path) == 0);
			unlink(data);
		}
		if (drawn)
		{
			n = strlen(qsl_url);
			node_msg(T("Your QSL card as an image: %s%s%s"), qsl_url,
				 (n && qsl_url[n - 1] == '/') ? "" : "/", file);
		}
		else
			node_msg(T("The QSL image could not be made on this node"));
	}

	if ((fp = fopen(QSL_LOG, "a")) != NULL)
	{
		tm = gmtime(&now);
		strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M", tm);
		fprintf(fp, "%s UTC %-9s via %s%s%s\n", stamp, user, User.ul_name,
			drawn ? " " : "", drawn ? file : "");
		fclose(fp);
	}
	return 0;
}
