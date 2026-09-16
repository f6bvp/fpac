/********************************************************
 * wp/wplist.c                                       	*
 * FPAC project.            FPAC WP LIST             	*
 * list WP data base records local Node memory image	*
 * derived from wpedit.c and command.c	F6BVP		*
 *                                                    	*
 ********************************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <syslog.h>
#include <ctype.h>
#include <time.h>
#include <sys/file.h>
#include <sys/ioctl.h>

#include "ax25compat.h"
#include "wp.h"

void now_date(char *buf);

int main(int argc, char **argv)
{
	int nb = 1000;
	unsigned int flags = WP_INCLUDE_DELETED_FLAG;	/* sysop tool: shows a Status column, see wp.h */
	int p;
	int i, j;
	wp_t *wp;
	char *add;
	char *call;
	char dnic[5];
	char buf[30];

	if (argc < 2)
	   {
	   printf ("\nWplist (version %s)\n",__DATE__);
	   printf ("Usage: wplist [-acdnrl number] <callsign index>\n");
	   printf ("options :  -n = nodes only  -l = max number of answers\n");
	   printf ("sort by :  -a address  -c callsign (default)  -d date  -r reverse\n");
	   printf("\n");
	   return (1);
	   }

/* Print the Current Date/time */
	        now_date(buf);
	        printf ("\n\tWPlist - %s",buf);

	optind = 0;

	while ((p = getopt(argc, argv, "acdl:nr")) != -1)
	{
		switch (p)
		{
		case 'c':
			flags &= ~(WP_ADDRSORT_FLAG | WP_DATESORT_FLAG);
			break;
		case 'l':
			nb = strtoul(optarg, NULL, 10);
			break;
		case 'n':
			flags |= WP_NODE_FLAG;
			break;
		case 'r':
			flags |= WP_REVERSE_FLAG;
			break;
		case 'a':
			flags &= ~(WP_DATESORT_FLAG);
			flags |= WP_ADDRSORT_FLAG;
			break;
		case 'd':
			flags &= ~(WP_ADDRSORT_FLAG);
			flags |= WP_DATESORT_FLAG;
			break;
		case '?':
		default:
			/* unknown option or missing argument (e.g. -l without number) */
			printf ("\nUsage: wplist [-acdnrl number] <callsign index>\n");
			printf ("options :  -n = nodes only  -l = max number of answers\n");
			printf ("sort by :  -a address  -c callsign (default)  -d date  -r reverse\n\n");
			return (1);
		}
	}

	if (optind == argc)
		argv[optind] = "*";
	else
		strcat(argv[argc-1], "*");

	if (wp_open("NODE") == 0) {

	if (wp_get_list(&wp, &nb, flags, argv[optind]) != -1)
	{
		int shown = 0;
		int wdigi = (int) strlen("Digi");
		int wloc  = (int) strlen("Locator");
		int wcity = (int) strlen("City");
		char (*dlist)[64] = NULL;
		int len;

		/* The list stops at the first record with a null date. */
		for (i = 0; i < nb; i++)
		{
			if (wp[i].date == 0L)
				break;
			shown++;
		}

		if (shown > 0)
			dlist = malloc(sizeof(*dlist) * shown);
		if (dlist == NULL)
			shown = 0;

		/* First pass : build the digipeaters strings (space        */
		/* separated, one column) and measure the variable-length   */
		/* columns (Digi, City) so the table lines up whatever the   */
		/* records hold.                                             */
		for (i = 0; i < shown; i++)
		{
			dlist[i][0] = '\0';
			for (j = wp[i].address.srose_ndigis - 1; j >= 0; j--)
			{
				char *digi = ax25_ntoa(&wp[i].address.srose_digis[j]);
				if (strstr(digi,"-") == NULL)
					strcat(digi,"-0");
				if (dlist[i][0] != '\0')
					strncat(dlist[i], " ", sizeof(dlist[i]) - strlen(dlist[i]) - 1);
				strncat(dlist[i], digi, sizeof(dlist[i]) - strlen(dlist[i]) - 1);
			}

			len = (int) strlen(dlist[i]);
			if (len > wdigi)
				wdigi = len;
			len = (int) strlen(wp[i].locator);
			if (len > wloc)
				wloc = len;
			len = (int) strlen(wp[i].city);
			if (len > wcity)
				wcity = len;
		}

		printf("%-9s %-14s %-4s %-7s %-4s %-*s %-*s %-*s %s\n",
		       "Callsign", "Update UTC", "DNIC", "Address", "N/U",
		       wdigi, "Digi", wloc, "Locator", wcity, "City", "Status");

		/* Second pass : print the records. Empty strings show up as */
		/* spaces in their fixed-width column, keeping every line    */
		/* aligned.                                                  */
		for (i = 0; i < shown; i++)
		{
			add = rose_ntoa(&wp[i].address.srose_addr);
			call = ax25_ntoa(&wp[i].address.srose_call);

			if (strstr(call,"-") == NULL)
				strcat(call,"-0");

			strncpy(dnic, add, 4);
			dnic[4] = '\0';

			my_date(buf, wp[i].date);

			printf("%-9s %-14s %-4s %-7s %-4s %-*s %-*s %-*s %s\n",
			       call,
			       buf,
			       dnic,
			       add + 4,
			       wp[i].is_node ? "Node" : "User",
			       wdigi, dlist[i],
			       wloc, wp[i].locator,
			       wcity, wp[i].city,
			       wp[i].is_deleted ? "DELETED" : "Ok");
		}

		free(dlist);
	}

	if (nb == 0)
	{
		printf("No WP matching \"%s\" !\n", argv[optind]);
		return (1);
	}

	printf("\nFPAC White Pages database : %d callsigns\n", wp_nb_records());

	wp_free_list(&wp);
	wp_close();
	}
	else {
		printf("Cannot open WP \n");
		return (1);
	}
	return (0);
}
