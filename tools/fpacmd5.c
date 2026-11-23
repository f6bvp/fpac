/*
 * fpacmd5.c -- answer to the SYSop challenge of an FPAC node.
 *
 * F6BVP 2026-11-23.  The node shows a challenge such as
 *
 *	F6BVP-12> 3 7 1 12 5 [1795430000]
 *
 * The MD5 answer is the MD5 of the number in brackets, as shown (with
 * its leading zeros), followed by the sysop password (Password = in
 * fpac.conf).  It changes with every challenge and reveals nothing of
 * the password, unlike the answer with the 5 characters (-n).
 *
 * It is worked out with the MD5 code of fpacnode itself (lib/libmd.c),
 * so that both always agree.  The password is asked without echo and is
 * written nowhere; when the standard input is not a terminal it is read
 * from its first line.  Messages in French or in English, after LC_ALL,
 * LC_MESSAGES, LANGUAGE or LANG, or -l fr | -l en.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <termios.h>

/* lib/libmd.c, the MD5 of fpacnode (declared in fpac.h, which needs the AX.25 headers) */
extern void MD5String(char *dest, char *source);

static int fr;		/* messages in French */

#define M(en, frtext)	(fr ? (frtext) : (en))

static void lang_choose(const char *forced)
{
	static const char *env[] = { "LC_ALL", "LC_MESSAGES", "LANGUAGE", "LANG", NULL };
	const char *v = forced;
	int i;

	for (i = 0; v == NULL && env[i] != NULL; i++)
	{
		v = getenv(env[i]);
		if (v != NULL && (*v == '\0' || !strcmp(v, "C") || !strcmp(v, "POSIX")))
			v = NULL;
	}
	fr = (v != NULL && tolower((unsigned char)v[0]) == 'f' &&
	      tolower((unsigned char)v[1]) == 'r');
}

static void usage(FILE *f)
{
	fprintf(f, "%s", M(
	"usage: fpacmd5 [-n] [-l fr|en] [challenge]\n"
	"  Prints the MD5 answer to the SYSop challenge of an FPAC node, e.g.\n"
	"      fpacmd5 'F6BVP-12> 3 7 1 12 5 [1795430000]'\n"
	"  The challenge may also be the number in brackets alone. Without it,\n"
	"  it is asked for; the sysop password is then asked without echo.\n"
	"  -n  also prints the answer with the 5 characters\n"
	"  -l  language of the messages (default: LANG)\n",
	"usage : fpacmd5 [-n] [-l fr|en] [défi]\n"
	"  Affiche la réponse MD5 au défi SYSop d'un nœud FPAC, par exemple\n"
	"      fpacmd5 'F6BVP-12> 3 7 1 12 5 [1795430000]'\n"
	"  Le défi peut aussi être le nombre entre crochets seul. Sans défi,\n"
	"  il est demandé ; le mot de passe sysop est ensuite demandé sans écho.\n"
	"  -n  affiche aussi la réponse aux 5 caractères\n"
	"  -l  langue des messages (par défaut : LANG)\n"));
}

/* Read one line from stdin, without the end of line; echo off on a terminal */
static int read_line(char *buf, size_t size, int noecho)
{
	struct termios old, t;
	int tty = isatty(STDIN_FILENO);
	int ok;

	if (noecho && tty && tcgetattr(STDIN_FILENO, &old) == 0)
	{
		t = old;
		t.c_lflag &= ~ECHO;
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
		ok = fgets(buf, size, stdin) != NULL;
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
		fputc('\n', stderr);
	}
	else
		ok = fgets(buf, size, stdin) != NULL;

	if (ok)
		buf[strcspn(buf, "\r\n")] = '\0';
	return ok;
}

/* Number in brackets (or the line alone if it is a number), and the 5 positions */
static int parse_challenge(const char *s, char *num, size_t size, int pos[5], int *havepos)
{
	const char *b = strchr(s, '['), *e;
	const char *gt;
	size_t n;

	if (b != NULL)
	{
		b++;
		e = strchr(b, ']');
		if (e == NULL)
			return 0;
	}
	else
	{
		b = s;
		while (isspace((unsigned char)*b))
			b++;
		e = b + strlen(b);
		while (e > b && isspace((unsigned char)e[-1]))
			e--;
	}
	n = e - b;
	if (n == 0 || n >= size || strspn(b, "0123456789") < n)
		return 0;
	memcpy(num, b, n);
	num[n] = '\0';

	gt = strchr(s, '>');
	*havepos = (gt != NULL && sscanf(gt + 1, "%d %d %d %d %d",
			&pos[0], &pos[1], &pos[2], &pos[3], &pos[4]) == 5);
	return 1;
}

int main(int argc, char **argv)
{
	char line[512], num[32], pass[256], source[300], md5[40];
	const char *lang = NULL;
	int netrom = 0, pos[5], havepos, c, i;
	size_t plen, used;

	for (i = 1; i < argc; i++)	/* -l first, so that the usage is in the right language */
		if (!strcmp(argv[i], "-l") && i + 1 < argc)
			lang = argv[i + 1];
	lang_choose(lang);

	while ((c = getopt(argc, argv, "nl:h")) != -1)
	{
		switch (c)
		{
		case 'n':
			netrom = 1;
			break;
		case 'l':
			break;
		case 'h':
			usage(stdout);
			return 0;
		default:
			usage(stderr);
			return 2;
		}
	}

	line[0] = '\0';
	if (optind < argc)
	{
		for (used = 0, i = optind; i < argc; i++)
		{
			if (used + strlen(argv[i]) + 2 > sizeof(line))
				break;
			if (used)
				line[used++] = ' ';
			strcpy(line + used, argv[i]);
			used += strlen(argv[i]);
		}
	}
	else
	{
		fprintf(stderr, "%s", M("Challenge line (or number in brackets): ",
					"Ligne du défi (ou nombre entre crochets) : "));
		if (!read_line(line, sizeof(line), 0))
			return 1;
	}

	if (!parse_challenge(line, num, sizeof(num), pos, &havepos))
	{
		fprintf(stderr, M("fpacmd5: no number in brackets in \"%s\"\n",
				  "fpacmd5 : pas de nombre entre crochets dans « %s »\n"), line);
		return 1;
	}

	if (isatty(STDIN_FILENO))
		fprintf(stderr, "%s", M("Sysop password: ", "Mot de passe sysop : "));
	if (!read_line(pass, sizeof(pass), 1))
		return 1;

	/* fpac.conf: the value is read without the spaces around it */
	{
		char *p = pass;
		while (isspace((unsigned char)*p))
			p++;
		memmove(pass, p, strlen(p) + 1);
		plen = strlen(pass);
		while (plen > 0 && isspace((unsigned char)pass[plen - 1]))
			pass[--plen] = '\0';
	}
	if (plen == 0)
	{
		fprintf(stderr, "%s", M("fpacmd5: empty password\n", "fpacmd5 : mot de passe vide\n"));
		return 1;
	}

	snprintf(source, sizeof(source), "%s%s", num, pass);
	MD5String(md5, source);
	printf("%s\n", md5);

	if (netrom)
	{
		if (!havepos)
			fprintf(stderr, "%s", M("fpacmd5: -n needs the whole challenge line, with the 5 positions\n",
						"fpacmd5 : -n demande la ligne entière du défi, avec les 5 positions\n"));
		else
		{
			for (i = 0; i < 5; i++)
				if (pos[i] < 1 || (size_t)pos[i] > plen)
				{
					fprintf(stderr, "%s", M("fpacmd5: a position is beyond the length of the password\n",
								"fpacmd5 : une position dépasse la longueur du mot de passe\n"));
					break;
				}
			if (i == 5)
			{
				for (i = 0; i < 5; i++)
					putchar(pass[pos[i] - 1]);
				putchar('\n');
			}
		}
	}

	memset(pass, 0, sizeof(pass));
	memset(source, 0, sizeof(source));
	return 0;
}
