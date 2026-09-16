#ifndef __WP_H
#define __WP_H

#include <netinet/in.h>
/*
 * #include <linux/ax25.h>
 * #include <linux/rose.h>
 */
#include <sys/socket.h>

#define WP_VECTOR_SIZE	16
#define WP_API_TIMEOUT	30	/* Timeout for access to wp server */
#define WP_OBSOLETE 14		/* Days after deleted record is not accepted */

#define WP_NODE_FLAG		1
#define WP_REVERSE_FLAG		2
#define WP_ADDRSORT_FLAG	4
#define WP_DATESORT_FLAG	8
/* F6BVP 2026-09-15: without this, wp_type_get_list returns deleted
 * records to every caller -- fine for wplist (sysop tool, shows a
 * Status column), but the "Wp" command in fpacnode (regular users)
 * doesn't show any status at all, so a deleted record looked exactly
 * like a valid one and led users straight into the "(user port)"
 * fallback bug when they tried to connect through it. Only wplist sets
 * this flag; fpacnode's "Wp" leaves it unset and no longer sees deleted
 * records at all. */
#define WP_INCLUDE_DELETED_FLAG	16

#define WP_INFO_ALL			0xffffffff

#define MIN(a,b)	((a) < (b) ? (a) : (b))

typedef struct {
	time_t date;
	struct full_sockaddr_rose address;
	char name[22];
	char city[22];
	char locator[7];
	char is_node;
	char is_deleted;
	/* F6BVP 2026-09-15: when is_deleted was first set, as 4 little-endian
	 * bytes (epoch seconds) -- LOCAL ONLY, never sent over the wire (see
	 * libwp.c wp_send_pdu/wp_receive_pdu, which pack wp_t field by field
	 * and never touch this one), so no compatibility impact on peers
	 * still running older code. wp.date alone can't tell "content last
	 * changed" and "marked deleted" apart, which made wpmaint's erase
	 * delay (e_temps) effectively meaningless once wp.date stopped being
	 * bumped at deletion time (see wpmaint.c). Zero means "unknown /
	 * deleted by pre-rc23 code or via wpedit's R command before this
	 * field existed" -- treated conservatively (not erased yet, but
	 * stamped with the current time so it starts aging normally).
	 * Kept out-of-band from `date` and NOT wired into the network PDU
	 * format on purpose, and Byte array (not time_t) so its size doesn't
	 * depend on the local time_t width, which differs across the 32 and
	 * 64-bit nodes on this network. */
	unsigned char del_date[4];
	char free[19];		/* For futur extension */
} wp_t;

typedef struct {
	time_t		date_base;
	unsigned short	version;
	unsigned short	interval;
	unsigned short	seed;
	unsigned short	crc[WP_VECTOR_SIZE];	
	int	cnt[WP_VECTOR_SIZE];	
} vector_t;

typedef struct {
	unsigned int max;
	unsigned int flags;
	char mask[10];	
} list_req_t;

typedef struct {
	int pos;
	char next;
	wp_t wp;
} list_rsp_t;

typedef struct {
	int mask;
} info_t;

typedef struct {
	unsigned int mask;
	unsigned int nbrec;
	unsigned int size;
} info_rsp_t;

typedef enum {
	wp_type_set = 0,
	wp_type_get,
	wp_type_get_response,
	wp_type_response,
	wp_type_vector_request,
	wp_type_vector_response,
	wp_type_get_list,
	wp_type_get_list_response,
	wp_type_info,
	wp_type_info_response,
	wp_type_end_transaction,
	wp_type_ascii = ':',
} wp_pdu_type;

typedef struct {
	wp_pdu_type	type:8;
	union {
		ax25_address	call;
		unsigned char	status;
		wp_t			wp;
		char			string[85];
		vector_t		vector;
		list_req_t		list_req;
		list_rsp_t		list_rsp;
		info_t			info;
		info_rsp_t		info_rsp;
	} data;
} wp_pdu;

#define WP_OK			0
#define WP_INVALID_COMMAND	1
#define WP_SET_ERROR		2
#define WP_GET_ERROR		3

#define FILE_SIGNATURE	"FPACWP_V003"

typedef struct {
	char	signature[16];
	int	nb_record;
} wp_header;

/* Public fonctions provided by libwp.a */

int ancien(wp_pdu *pdu);	/* F6BVP : record older than WP_OBSOLETE days */
int strmatch(char *string, char *pattern);
int wp_check_call(const char *callsign);
int wp_get(ax25_address *call, wp_t *wp);
int wp_listen(void);
int wp_open(char *);
int wp_is_open(void);
int wp_open_remote(char *source_call, struct full_sockaddr_rose *remote, int non_block);
int wp_receive_pdu(int s, wp_pdu *pdu);
#ifndef extern	/* pad/fpad.c does '#define extern' to define globals in main;
		 * wp_progress lives in libwp.a, so keep it a plain declaration
		 * there and avoid a duplicate definition in fpad.o */
extern int wp_progress;		/* F6BVP : 1 = show '.' while waiting for server reply */
#endif
int wp_search(ax25_address *call, struct full_sockaddr_rose *addr);
int wp_send_pdu(int s, wp_pdu *pdu);
int wp_set(wp_t *wp);
int wp_update_addr(struct full_sockaddr_rose *addr);
int wp_get_list(wp_t **wp, int *nb, int flags, char *mask);
int wp_is_node(char *callsign);
void my_date(char *buf, time_t date );

int wp_nb_records(void);

void dump_rose(char *, struct full_sockaddr_rose *);
void wp_close(void);
void wp_flush_pdu(void);
void wp_free_list(wp_t **wp);

/* del_date accessors: see the field's comment in the wp_t definition
 * above. wp_set_del_date(wp, 0) clears it back to "unknown". */
time_t wp_get_del_date(const wp_t *wp);
void wp_set_del_date(wp_t *wp, time_t t);

#endif /* __WP_H */
