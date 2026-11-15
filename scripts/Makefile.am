sbin_SCRIPTS = ax25parms versions myfpac.sh
# Helper of the QSl command of fpacnode (Python, needs python3-pil)
# Writes the minimal configuration of a new node (no fixed route, FPAC 4.2)
dist_sbin_SCRIPTS = fpacqsl fpac-node-setup

fpacdir = $(AX25_LOCALSTATEDIR)fpac
fpac_DATA = loggedin

flexdir = $(AX25_LOCALSTATEDIR)flex
flex_DATA = gateways destinations

install-data-hook:
	chmod a+w $(DESTDIR)$(AX25_LOCALSTATEDIR)fpac/loggedin

EXTRA_DIST = $(fpac_DATA) $(flex_DATA)

AX25_LOCALSTATEDIR=@localstatedir@/ax25/

