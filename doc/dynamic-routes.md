# FPAC 4.2 — Dynamic ROSE routes / Routes ROSE dynamiques

Specification, working draft of 2026-10-13 — F6BVP.
Spécification, version de travail du 13/10/2026 — F6BVP.

Three items: dynamic routes (sections 1 to 15), White Pages
sessions closed when idle (section 16) and obsolete deleted records
left out of the White Pages vectors (section 17).
Trois chantiers : les routes dynamiques (sections 1 à 15), les
sessions White Pages fermées au repos (section 16) et les fiches
supprimées obsolètes sorties des vecteurs White Pages (section 17).

English text first, French text below. Both say the same thing.
Le texte anglais vient d'abord, le texte français ensuite. Les deux
disent la même chose.

---

# English

The `fpacrtd` daemon is being tested on three nodes; its code is
not released yet.

## 1. Purpose

Today an FPAC node only knows the routes written by hand in
`fpac.routes`. A DNIC or a node missing from that file cannot be
reached, even when a direct neighbour knows how to reach it.

The purpose is that each node learns from its direct neighbours what
they can reach, installs the routes it lacks, and removes them when
they are no longer valid. The `fpac.routes` file keeps the last word.

## 2. What the mechanism does, and does not do

It does:

- announce to the direct neighbours the destinations the node can
  reach, with a cost counted in hops;
- install in the kernel the learned routes that are missing;
- remove a learned route when the neighbour closes, stops announcing
  it, or announces it as lost.

It does not:

- replace a route written in `fpac.routes`;
- repair a link that works in one direction only;
- change the kernel rule that prefers an open neighbour;
- announce intermediate prefixes (see 4).

## 3. Architecture

A new daemon, `fpacrtd`, started by `myfpac.sh` after the other
FPAC daemons and stopped before them, so that its routes are
withdrawn while `fpad` is still running.

- It opens a ROSE circuit to each neighbour of `fpac.nodes` marked
  `Route = yes`, to the service callsign `FPACRT`, and listens on
  that same callsign. The first call is made 30 seconds after the
  start, to let the neighbours start their own daemon.
- It reads the kernel routes (`/proc/net/rose_nodes`), the neighbours
  (`/proc/net/rose_neigh`) and `fpac.routes`.
- It adds and removes its routes with `SIOCADDRT` / `SIOCDELRT`, as
  `fpacnode` already does.
- It keeps the list of its routes in `/var/ax25/fpac/fpacrt.dat`, to
  recognise and purge them.
- Only one `fpacrtd` runs on a node. The daemon holds a lock on
  `/var/ax25/fpac/fpacrtd.lock` for its whole life; a second one
  finds it locked, reports `another fpacrtd is already running` and
  exits. Without this, the second daemon purged the routes of the
  first and both called the same neighbours.

Any local script that stops AX.25 or FPAC without going through
`myfpac.sh` must stop `fpacrtd` too, and first: on 2026-10-11 a
restart of AX.25 on one node left the old daemon alive beside the
new one.

The same protection covers the four other daemons since 2026-10-12:
`fpad`, `fpacwpd`, `fpacstat` and `fpacroute` each hold a lock on
`/var/ax25/fpac/<name>.lock` and refuse to start when another copy
is running. They also check their arguments before touching
anything: `-h` or `-?` shows a short help, `-V` the version, and an
unknown option or a stray argument is an error. Until then `fpad`
and `fpacroute` ignored every argument and started: `fpad -V`,
typed on a running node to read its version, started a second
`fpad` that cleared and rebuilt the ROSE routes of the node and
listened on the same addresses as the first one. Note that
`fpacwpd -v` is still the verbose mode and starts the daemon.

A lock file that cannot be written (file system full or read-only)
is opened read-only, which is enough to hold the lock. When it
cannot be opened at all, the daemon logs a warning and starts
without the protection: a node that does not come back is worse
than a missing lock. A daemon built before the lock is not seen by
a new one; the protection holds once the new programs run.

Why a separate daemon and not `fpacwpd`: a current `fpacwpd` closes
the session when it receives a message type it does not know
(`wp_receive_pdu` returns -1). Adding a message to the White Pages
sessions would break the synchronisation with every node not yet
updated. A separate service leaves the White Pages untouched.

The service callsign is `FPACRT`, not `ROUTE`: `fpacroute`, the
traceroute application, already listens on ROUTE on every FPAC
node (found during the first test, 2026-10-08).

A node without `fpacrtd` does NOT refuse a call to `FPACRT`: its
`fpad` takes it for a user call, records the caller in the White
Pages and tries an AX.25 connection to a station named FPACRT. A
neighbour is therefore called only when its Node block carries
`Route = yes` (see 10).

## 4. What is announced

Two kinds of destinations only:

| Kind             | Mask   | Example       |
|------------------|--------|---------------|
| Node address     | 10     | 2080 175526   |
| Whole DNIC       | 4      | 5050          |

Intermediate prefixes (masks 5 to 9, for example `2080 44`) are
neither announced nor learned. Routing loops came from them; they
remain the business of `fpac.routes`.

Origin of an announcement:

- **node address**: each node is the origin of its own address and
  of its local addresses, at cost 0. Nobody else can be their origin.
- **whole DNIC**: only a node carrying `RouteOrigin = <dnic>` in its
  `fpac.conf` is its origin, at cost 0. The sysop of the node that
  has the real link to that country writes it.

Everything else is relaying: a node re-announces a learned
destination at the received cost plus 1, and nothing else. A route
of `fpac.routes` never produces an announcement by itself: a wrong
or stale line therefore has no effect outside the node.

A node relays only what its own `RouteLearn` lets it retain: with
`RouteLearn = no` it relays nothing, and with `RouteLearn = nodes`
it relays no DNIC.

A node declaring `RouteOrigin = 5050` must itself have a route to
that DNIC in `fpac.routes`. Otherwise `fpacrtd` reports it in the
log and does not announce that DNIC.

The DNIC is announced only while its exit link is open, that is
while a neighbour of that route is "open" in `rose_neigh`. When the
link closes the DNIC is withdrawn (`W`), and announced again when
it reopens; the state is checked every minute and each change is
written in the log. The link with the neighbour the announcement
goes to does not count: a DNIC is never announced to a node on the
strength of the link with that same node.

## 5. Exchanges

The dialogue is plain text, one line per message, so that it stays
readable with `listen`.

| Message            | Meaning                                |
|--------------------|----------------------------------------|
| `FPACRT 1 <call>`  | opening: version, L3 callsign          |
| `R <adr> <m> <c>`  | route: address, mask, cost             |
| `W <adr> <m>`      | withdrawal of that route               |
| `E <n>`            | end of table, n routes sent            |

Timing:

- full table when the circuit opens;
- full table every 30 minutes;
- single `R` or `W` messages about 10 seconds after a change (route
  learned or lost); changes close together are sent in one go;
- the state of the neighbours (open or closed) is checked every
  minute.

## 6. Reception rules

For each route received from a neighbour V, with cost c:

1. retained cost = c + 1; above 6 the route is ignored;
2. if the destination is one of the node's own addresses: ignored;
3. if `fpac.routes` already holds that address with that mask: the
   route is not installed, the manual route stays in place; the
   destination is still relayed to the other neighbours;
4. if V is not "open" in `rose_neigh`: not installed;
5. otherwise it is installed if no learned route exists for that
   destination, or if its cost is strictly better.

A route that `RouteLearn` does not admit is not retained at all: it
is neither installed nor relayed (see 4).

One learned route per destination: the best one. No backup
neighbour in version 1.

## 7. Transmission rules

- **Split horizon**: a destination is never announced to the
  neighbour through which the node really reaches it, whether by a
  learned route or by a manual route. This protects against the
  two-node loop.
- **Poisoned withdrawal**: when a learned route is lost, a `W` is
  sent to the other neighbours about 10 seconds later.
- **Ceiling**: no route with a cost above 6 is announced.

## 8. Lifetime

A learned route is removed from the kernel when:

- the `FPACRT` circuit with the neighbour closes, or the neighbour
  has sent no table for 90 minutes (see below);
- the neighbour becomes "closed" in `rose_neigh` (checked every
  minute);
- the neighbour sends `W`, or sends 3 full tables in a row without
  it (90 minutes at the normal rate);
- `fpacrtd` stops: it removes all its routes.

At startup, `fpacrtd` first removes the routes left by its previous
run, according to `fpacrt.dat`. Every minute it also checks that its
routes are still in the kernel and puts back any that disappeared.

A neighbour that stays silent is handled too: when a node holds
routes learned from a neighbour and has received no full table from
it for 90 minutes, it closes the circuit. The routes learned from
that neighbour are removed, and it is called again 30 minutes
later. A neighbour that never announced anything (`RouteAnnounce =
no`) is silent by design and is left alone.

## 9. Manual routes and learned routes

- A route of `fpac.routes` is never removed nor modified.
- With the same address and mask, the manual route wins.
- A learned route more specific than a manual one applies, like any
  more specific route: a learned node address goes before a manual
  `2080 = …`. This is intended (agreed by F6BVP, 2026-10-08), but it
  is a change of behaviour to be aware of.

## 10. Configuration

In `fpac.conf` and `fpac.nodes`, four settings, all inactive by
default:

| Keyword          | Values            | Effect                   |
|------------------|-------------------|--------------------------|
| `RouteAnnounce`  | `no`, `yes`       | sends its table          |
| `RouteLearn`     | `no`, `nodes`,    | learns nothing, nodes,   |
|                  | `all`             | or nodes and DNICs       |
| `RouteOrigin`    | one DNIC per line | exit gateway of the DNIC |
| `Route`          | `yes`, in a Node  | this neighbour takes     |
|                  | block             | part                     |

Without these lines, a 4.2 node behaves like a 4.1 node. A
neighbour without `Route = yes` is never called and its calls are
refused.

Example for f6bvp-8, which has the real links abroad:

    RouteAnnounce = yes
    RouteLearn    = all
    RouteOrigin   = 2020
    RouteOrigin   = 2400
    RouteOrigin   = 3020
    RouteOrigin   = 3100
    RouteOrigin   = 5050

and, in `fpac.nodes`, one more line in the block of each neighbour
that runs `fpacrtd` (here f6bvp-12). Nothing is exchanged without
it:

    Node = F6BVP-13
     ...
     Route = yes
    End

## 11. Display

Planned, not coded yet. The `Routes` command of `fpacnode` will
mark the learned routes (`dyn`), and a `routes dyn` command will
list the learned routes only, with the neighbour that announces
them.

Today the learned routes can be read in the log of `fpacrtd` (lines
"add route" and "del route") and in `fpacrt.dat`.

## 12. Compatibility

- A 4.1.x node must never be called on the `FPACRT` callsign (see 3):
  `Route = yes` is written in a Node block only once the sysop of
  that neighbour runs `fpacrtd`. The two sysops agree first.
- A neighbour marked `Route = yes` that does not answer is retried
  every 30 minutes.
- The kernel is not modified.
- The White Pages are not modified.

## 13. Test plan

1. Pair f6bvp-12 / f6bvp-8 (f6bvp-8 is the best connected node):
   f6bvp-8 with `RouteAnnounce = yes` and its `RouteOrigin` lines,
   f6bvp-12 with `RouteLearn = all`. First remove a distant DNIC,
   for example 5050, from `fpac.routes` of f6bvp-12: it must come
   back by itself. Also check the removal when the neighbour closes
   and the purge at restart.
2. Enable announcing both ways on the pair, then add f6bvp-10 with
   `RouteLearn = all`: it must receive 5050, which it lacks today.
3. Provoked loop test: cut the exit link of a DNIC and check that
   the route disappears everywhere in less than 2 minutes, with no
   frame going round (`rose-controle.py`).
4. One week of observation before extending to the other nodes.

Status on 2026-10-12: step 1 passed on 2026-10-08, including the
removal when the neighbour's daemon stops and the purge at restart.
Since 2026-10-09 three nodes take part: f6bvp-8 announces, f6bvp-12
and f6bvp-10 learn, each directly from f6bvp-8. After each of four
nightly restarts of FPAC the sessions and the learned routes came
back by themselves within 50 seconds, and the table of 6 routes has
been received every 30 minutes without a miss. Announcing both ways
(step 2) is not enabled yet. Not tested on the air yet: ageing, the
90 minute rule, relaying through a third node, and a real call
through a learned route.

The daemons with the single instance lock (see 3) have been running
on seven nodes since 2026-10-12, built from the 4.1.6-rc7 sources
plus this change. On each node the locks are held and a second
start of each daemon is refused.

## 14. Decisions taken and open points

Decided (F6BVP, 2026-10-08):

- the exit gateway of a DNIC is declared with `RouteOrigin`; a
  manual route alone is not enough to announce it;
- a learned node address goes before a wider manual route;
- ceiling of 6 hops and period of 30 minutes as starting values;
- first tests on the pair f6bvp-12 / f6bvp-8.

Still open:

- should version 2 keep a backup neighbour per destination?
- what if two nodes declare `RouteOrigin` for the same DNIC: it is
  allowed (two real exits), each node takes the nearest. To be
  confirmed in use.

Decided (F6BVP, 2026-10-13):

- a DNIC declared with `RouteOrigin` is announced only while its
  exit link is open, and withdrawn when that link closes. Reason:
  the kernel lets a call in transit leave only through an open
  neighbour (a closed one is tried for a local call only), so a
  DNIC announced over a closed exit leads nowhere. The kernel rule
  is not changed. Coded on 2026-10-13 (see 4) and on trial since
  then on the announcing node, whose table went from 6 routes to 2:
  four of its five DNICs had a closed exit.

## 15. Numbering

This feature adds a daemon, a protocol between nodes and
configuration keywords: it justifies FPAC 4.2. Proposed order:

1. release the stable 4.1.6 with the fixes of rc7;
2. open 4.2-rc1 with `fpacrtd` and `WpIdle` (see 16), both
   inactive by default, and the change to the vectors (see 17).

The code under test is public: branch `fpac-4.2` of
<https://github.com/f6bvp/fpac>. It is experimental and not a
release.

## 16. Second item: White Pages sessions closed when idle

Decided by F6BVP on 2026-10-08. Nothing is coded yet.

Today `fpacwpd` keeps one ROSE circuit open for ever with each
neighbour (WP-0 to WP-0), although it only carries a record now and
then and one vector exchange per hour.

New keyword in `fpac.conf`:

| Keyword   | Value            | Effect                         |
|-----------|------------------|--------------------------------|
| `WpIdle`  | minutes, 0 = off | closes an idle WP session      |

Without the keyword, or with 0, nothing changes: the session stays
open as in 4.1. Recommended value: `WpIdle = 10`.

Behaviour with `WpIdle = 10`:

- a session with a neighbour is closed after 10 minutes without any
  record or vector exchanged;
- it is reopened on demand only: when a record has to be pushed to
  that neighbour (local change or relay), or when the hourly vector
  exchange is due;
- a session opened by the neighbour is always accepted;
- the blind retry every 30 minutes remains only after a failed
  connection, not after an idle close.

What the code needs:

- the list of records waiting for a neighbour must belong to the
  neighbour and no longer to the session, so that it survives a
  close;
- the neighbour must be recognised as a WP server (fix of 4.1.6-rc7);
- today a session that opens starts a full vector exchange;
  reopening one to push a single record must not start it each
  time.

With a neighbour still running 4.1, which reopens its session every
30 minutes and keeps it, the circuit is closed again 10 minutes
later. This is harmless but shows in the logs.

The kernel can clear an idle ROSE circuit by itself (socket option
ROSE_IDLE). Closing from `fpacwpd` is preferred, because it knows
whether an exchange is in progress; to be settled when coding.

Test: on a pair, check that an idle session closes, that a record
changed with `wpedit` reopens it and reaches the neighbour, and that
the hourly vector exchange still takes place.

## 17. Third item: obsolete deleted records left out of the vectors

Found on 2026-10-11. Two nodes kept logging `Vectors still differ`
every hour although both ran the same version.

A deleted record older than 14 days (`WP_OBSOLETE`) is neither sent
nor accepted between nodes, and a deleted record for an unknown
callsign is refused. Yet these records count in the vectors, whose
checksum covers the date and the address of every record. So when
two nodes mark a record deleted, or erase it, on different days,
their vectors differ and the exchange cannot repair it: the records
that would settle the difference are exactly the ones that are not
exchanged. It lasts until both nodes have erased them.

Here one node ran its nightly `wpmaint -d 100` while the others ran
`wpmaint -d 30`: it held 12 deleted records the others had erased,
and two records still valid that were deleted elsewhere.

Decided (F6BVP, 2026-10-11): leave out of the vector checksum the
deleted records too old to be exchanged. Coded on 2026-10-13, not
yet in service.

A node with the change and a node without it compute different
vectors and would differ for ever. Decided (F6BVP, 2026-10-13): the
vector version number changes with the new checksum; this is what
it is for.

`fpacwpd` up to 4.1.6 closes the White Pages session when it
receives a vector of another version, and calls again 30 minutes
later. With nothing more, a 4.2 node and a 4.1.6 node would no
longer keep a session. Decided (F6BVP, 2026-10-13): 4.2 goes on
speaking the old version, with the old checksum, to a neighbour
that is still older; the new version is used between 4.2 nodes
only. A network may then be upgraded node by node.

As coded on 2026-10-13: the new vector version is 0x112, the old one
0x111. A node answers a vector request in the version it came in.
For its own requests it first takes every neighbour for a 4.2 node;
if the neighbour hangs up before answering, it is spoken to in the
old version from then on, with one line in the log. A vector in the
new version received from that neighbour proves it was upgraded.
Since a session may also drop for another reason at that very
moment, the new version is tried again once a week. The price with
a 4.1 neighbour: one session closed at the first exchange, then
once a week; it reopens within 30 minutes.

Not covered: a recently deleted record that the other node never
knew is refused there and still counts in the vectors, until it
becomes obsolete in turn.

Until then: use the same `wpmaint` delays on every node of a
network. The difference is harmless - three vector rounds an hour
at most since 4.1.6-rc7 - but it fills the log.

---

# Français

Le démon `fpacrtd` est à l'essai sur trois nœuds ; son code n'est
pas encore publié.

## 1. But

Aujourd'hui, un nœud FPAC ne connaît que les routes écrites à la main
dans `fpac.routes`. Un DNIC ou un nœud oublié dans ce fichier est
injoignable, même si un voisin direct sait l'atteindre.

Le but est que chaque nœud apprenne de ses voisins directs ce qu'ils
savent joindre, installe les routes qui lui manquent, et les retire
quand elles ne sont plus valables. Le fichier `fpac.routes` garde le
dernier mot.

## 2. Ce que le mécanisme fait, et ne fait pas

Il fait :

- annoncer aux voisins directs les destinations que le nœud sait
  joindre, avec un coût en nombre de sauts ;
- installer dans le noyau les routes apprises qui manquent ;
- retirer une route apprise quand le voisin se ferme, cesse de
  l'annoncer, ou l'annonce comme perdue.

Il ne fait pas :

- remplacer une route écrite dans `fpac.routes` ;
- réparer un lien qui ne marche que dans un sens ;
- changer la règle du noyau qui préfère un voisin ouvert ;
- annoncer des préfixes intermédiaires (voir 4).

## 3. Architecture

Un nouveau démon, `fpacrtd`, lancé par `myfpac.sh` après les autres
démons FPAC et arrêté avant eux, pour que ses routes soient retirées
tant que `fpad` tourne encore.

- Il ouvre un circuit ROSE vers chaque voisin de `fpac.nodes`
  marqué `Route = yes`, à l'indicatif de service `FPACRT`, et écoute
  sur ce même indicatif. Le premier appel part 30 secondes après le
  démarrage, pour laisser aux voisins le temps de lancer leur démon.
- Il lit les routes du noyau (`/proc/net/rose_nodes`), les voisins
  (`/proc/net/rose_neigh`) et `fpac.routes`.
- Il ajoute et retire ses routes par `SIOCADDRT` / `SIOCDELRT`, comme
  le fait déjà `fpacnode`.
- Il tient la liste de ses routes dans
  `/var/ax25/fpac/fpacrt.dat`, pour les reconnaître et les purger.
- Un seul `fpacrtd` tourne sur un nœud. Le démon tient un verrou sur
  `/var/ax25/fpac/fpacrtd.lock` pendant toute sa vie ; un second le
  trouve verrouillé, écrit `another fpacrtd is already running` et
  sort. Sans cela, le second démon purgeait les routes du premier et
  tous deux appelaient les mêmes voisins.

Tout script local qui arrête AX.25 ou FPAC sans passer par
`myfpac.sh` doit aussi arrêter `fpacrtd`, et en premier : le
11/10/2026, une relance d'AX.25 sur un nœud a laissé l'ancien démon
en vie à côté du nouveau.

La même protection couvre les quatre autres démons depuis le
12/10/2026 : `fpad`, `fpacwpd`, `fpacstat` et `fpacroute` tiennent
chacun un verrou sur `/var/ax25/fpac/<nom>.lock` et refusent de
démarrer si une autre copie tourne. Ils contrôlent aussi leurs
arguments avant toute action : `-h` ou `-?` affiche une aide courte,
`-V` la version, et une option inconnue ou un argument isolé est une
erreur. Jusque-là `fpad` et `fpacroute` ignoraient tout argument et
démarraient : `fpad -V`, tapé sur un nœud en service pour lire sa
version, a démarré un second `fpad` qui a vidé puis recréé les
routes ROSE du nœud et écouté sur les mêmes adresses que le premier.
Attention, `fpacwpd -v` reste le mode verbeux et démarre le démon.

Un fichier verrou qui ne peut pas être écrit (disque plein ou en
lecture seule) est ouvert en lecture seule, ce qui suffit à tenir le
verrou. S'il ne peut pas être ouvert du tout, le démon l'écrit au
journal et démarre sans cette protection : un nœud qui ne remonte
pas est pire qu'un verrou absent. Un démon compilé avant le verrou
n'est pas vu par un nouveau ; la protection vaut une fois les
nouveaux programmes en service.

Pourquoi un démon séparé et non `fpacwpd` : une version actuelle de
`fpacwpd` ferme la session quand elle reçoit un type de message
qu'elle ne connaît pas (`wp_receive_pdu` rend -1). Ajouter un message
aux sessions White Pages casserait donc la synchronisation avec tout
nœud non mis à jour. Un service séparé laisse les White Pages
intactes.

L'indicatif de service est `FPACRT`, et non ROUTE : `fpacroute`,
l'application de traceroute, écoute déjà sur ROUTE sur tous les
nœuds FPAC (constaté au premier essai, le 08/10/2026).

Un nœud sans `fpacrtd` ne refuse PAS un appel vers `FPACRT` : son
`fpad` le prend pour un appel d'utilisateur, inscrit l'appelant dans
les White Pages et tente une connexion AX.25 vers une station nommée
FPACRT. Un voisin n'est donc appelé que si son bloc Node porte
`Route = yes` (voir 10).

## 4. Ce qui est annoncé

Deux sortes de destinations seulement :

| Sorte            | Masque | Exemple       |
|------------------|--------|---------------|
| Adresse de nœud  | 10     | 2080 175526   |
| DNIC entier      | 4      | 5050          |

Les préfixes intermédiaires (masques 5 à 9, par exemple `2080 44`)
ne sont ni annoncés ni appris. C'est avec eux que les boucles sont
apparues ; ils restent du ressort de `fpac.routes`.

Origine d'une annonce :

- **adresse de nœud** : chaque nœud est l'origine de sa propre
  adresse et de ses adresses locales, au coût 0. Personne d'autre
  ne peut en être l'origine.
- **DNIC entier** : seul un nœud qui porte `RouteOrigin = <dnic>`
  dans son `fpac.conf` en est l'origine, au coût 0. C'est le sysop
  du nœud qui a le lien réel vers ce pays qui l'écrit.

Tout le reste est du relais : un nœud réannonce une destination
apprise au coût reçu plus 1, et rien d'autre. Une route de
`fpac.routes` ne donne jamais lieu à une annonce par elle-même :
une ligne fausse ou périmée reste donc sans effet hors du nœud.

Un nœud ne relaie que ce que son propre `RouteLearn` lui fait
retenir : avec `RouteLearn = no` il ne relaie rien, et avec
`RouteLearn = nodes` il ne relaie aucun DNIC.

Un nœud qui déclare `RouteOrigin = 5050` doit avoir lui-même une
route vers ce DNIC dans `fpac.routes`. Sinon, `fpacrtd` le signale
dans le journal et n'annonce pas ce DNIC.

Le DNIC n'est annoncé que tant que son lien de sortie est ouvert,
c'est-à-dire tant qu'un voisin de cette route est « ouvert » dans
`rose_neigh`. Quand le lien se ferme le DNIC est retiré (`W`), puis
annoncé de nouveau quand il se rouvre ; l'état est contrôlé chaque
minute et chaque changement est écrit au journal. Le lien avec le
voisin à qui l'annonce est destinée ne compte pas : un DNIC n'est
jamais annoncé à un nœud sur la foi du lien avec ce même nœud.

## 5. Échanges

Le dialogue est en texte, une ligne par message, pour rester lisible
avec `listen`.

| Message            | Sens                                   |
|--------------------|----------------------------------------|
| `FPACRT 1 <appel>` | ouverture : version, indicatif L3      |
| `R <adr> <m> <c>`  | route : adresse, masque, coût          |
| `W <adr> <m>`      | retrait de cette route                 |
| `E <n>`            | fin de table, n routes envoyées        |

Rythme :

- table complète à l'ouverture du circuit ;
- table complète toutes les 30 minutes ;
- messages `R` ou `W` isolés environ 10 secondes après un changement
  (route apprise ou perdue) ; des changements rapprochés partent
  ensemble ;
- l'état des voisins (ouvert ou fermé) est contrôlé chaque minute.

## 6. Règles de réception

Pour chaque route reçue d'un voisin V, de coût c :

1. coût retenu = c + 1 ; au-delà de 6, la route est ignorée ;
2. si la destination est l'une des adresses du nœud : ignorée ;
3. si `fpac.routes` contient déjà cette adresse avec ce masque :
   la route n'est pas installée, la route manuelle reste en place ;
   la destination est quand même relayée aux autres voisins ;
4. si V n'est pas « ouvert » dans `rose_neigh` : pas installée ;
5. sinon, elle est installée si aucune route apprise n'existe pour
   cette destination, ou si son coût est strictement meilleur.

Une route que `RouteLearn` n'admet pas n'est pas retenue du tout :
elle n'est ni installée ni relayée (voir 4).

Une seule route apprise par destination : la meilleure. Pas de voisin
de secours en version 1.

## 7. Règles d'émission

- **Horizon coupé** : une destination n'est jamais annoncée au voisin
  par lequel le nœud la joint réellement, que ce soit par une route
  apprise ou par une route manuelle. C'est la protection contre la
  boucle à deux nœuds.
- **Retrait empoisonné** : quand une route apprise est perdue, un `W`
  part vers les autres voisins environ 10 secondes plus tard.
- **Plafond** : aucune route de coût supérieur à 6 n'est annoncée.

## 8. Durée de vie

Une route apprise est retirée du noyau quand :

- le circuit `FPACRT` avec le voisin se ferme, ou le voisin n'a plus
  envoyé de table depuis 90 minutes (voir plus bas) ;
- le voisin passe à « fermé » dans `rose_neigh` (contrôlé chaque
  minute) ;
- le voisin envoie `W`, ou envoie 3 tables complètes de suite sans
  elle (90 minutes au rythme normal) ;
- `fpacrtd` s'arrête : il retire toutes ses routes.

Au démarrage, `fpacrtd` retire d'abord les routes restées de sa
dernière exécution, d'après `fpacrt.dat`. Chaque minute, il contrôle
aussi que ses routes sont toujours dans le noyau et remet celles qui
auraient disparu.

Un voisin qui se tait est traité lui aussi : quand un nœud détient
des routes apprises d'un voisin et n'a reçu de lui aucune table
complète depuis 90 minutes, il ferme le circuit. Les routes apprises
de ce voisin sont retirées, et il est rappelé 30 minutes plus tard.
Un voisin qui n'a jamais rien annoncé (`RouteAnnounce = no`) se tait
par construction et n'est pas concerné.

## 9. Routes manuelles et routes apprises

- Une route de `fpac.routes` n'est jamais retirée ni modifiée.
- À adresse et masque égaux, la route manuelle gagne.
- Une route apprise plus précise qu'une route manuelle s'applique,
  comme toute route plus précise : une adresse de nœud apprise
  passe devant un `2080 = …` manuel. C'est voulu (accord F6BVP,
  08/10/2026), mais c'est un changement de comportement à connaître.

## 10. Configuration

Dans `fpac.conf` et `fpac.nodes`, quatre réglages, tous inactifs
par défaut :

| Mot-clé          | Valeurs           | Effet                    |
|------------------|-------------------|--------------------------|
| `RouteAnnounce`  | `no`, `yes`       | envoie sa table          |
| `RouteLearn`     | `no`, `nodes`,    | n'apprend rien, apprend  |
|                  | `all`             | les nœuds, ou nœuds et   |
|                  |                   | DNIC                     |
| `RouteOrigin`    | un DNIC par ligne | porte de sortie du DNIC  |
| `Route`          | `yes`, dans un    | ce voisin participe      |
|                  | bloc Node         |                          |

Sans ces lignes, un nœud en 4.2 se comporte comme en 4.1. Un voisin
sans `Route = yes` n'est jamais appelé, et ses appels sont refusés.

Exemple pour f6bvp-8, qui a les liens réels vers l'étranger :

    RouteAnnounce = yes
    RouteLearn    = all
    RouteOrigin   = 2020
    RouteOrigin   = 2400
    RouteOrigin   = 3020
    RouteOrigin   = 3100
    RouteOrigin   = 5050

et, dans `fpac.nodes`, une ligne de plus dans le bloc de chaque
voisin qui fait tourner `fpacrtd` (ici f6bvp-12). Sans elle, rien
n'est échangé :

    Node = F6BVP-13
     ...
     Route = yes
    End

## 11. Affichage

Prévu, pas encore codé. La commande `Routes` de `fpacnode` marquera
les routes apprises (`dyn`), et une commande `routes dyn` listera
les seules routes apprises, avec le voisin qui les annonce.

Aujourd'hui, les routes apprises se lisent dans le journal de
`fpacrtd` (lignes « add route » et « del route ») et dans
`fpacrt.dat`.

## 12. Compatibilité

- Un nœud en 4.1.x ne doit jamais être appelé sur l'indicatif
  `FPACRT` (voir 3) : `Route = yes` ne s'écrit dans un bloc Node
  qu'une fois que le sysop de ce voisin fait tourner `fpacrtd`. Les
  deux sysops se mettent d'accord d'abord.
- Un voisin marqué `Route = yes` qui ne répond pas est réessayé
  toutes les 30 minutes.
- Le noyau n'est pas modifié.
- Les White Pages ne sont pas modifiées.

## 13. Plan d'essai

1. Paire f6bvp-12 / f6bvp-8 (f6bvp-8 est le nœud le plus connecté) :
   f6bvp-8 en `RouteAnnounce = yes` avec ses `RouteOrigin`,
   f6bvp-12 en `RouteLearn = all`.
   Retirer d'abord de `fpac.routes` de f6bvp-12 un DNIC lointain,
   par exemple 5050 : il doit revenir seul. Vérifier aussi le
   retrait à la fermeture du voisin et la purge au redémarrage.
2. Activer l'annonce dans les deux sens sur la paire, puis ajouter
   f6bvp-10 en `RouteLearn = all` : il doit recevoir 5050, qui lui
   manque aujourd'hui.
3. Essai de boucle provoquée : couper le lien de sortie d'un DNIC et
   vérifier que la route disparaît partout en moins de 2 minutes,
   sans trame qui tourne (`rose-controle.py`).
4. Une semaine d'observation avant d'étendre aux autres nœuds.

État au 12/10/2026 : l'étape 1 est passée le 08/10/2026, y compris
le retrait à l'arrêt du démon du voisin et la purge au redémarrage.
Depuis le 09/10/2026, trois nœuds participent : f6bvp-8 annonce,
f6bvp-12 et f6bvp-10 apprennent, chacun directement de f6bvp-8.
Après chacun des quatre redémarrages nocturnes de FPAC, les sessions
et les routes apprises sont revenues seules en moins de 50 secondes,
et la table de 6 routes est reçue toutes les 30 minutes sans manque.
L'annonce dans les deux sens (étape 2) n'est pas encore activée.
Pas encore éprouvés en réel : le vieillissement, la règle des
90 minutes, le relais par un troisième nœud et un appel réel par
une route apprise.

Les démons munis du verrou d'instance unique (voir 3) sont en
service sur sept nœuds depuis le 12/10/2026, compilés à partir des
sources de la 4.1.6-rc7 et de ce seul changement. Sur chaque nœud
les verrous sont tenus et un second lancement de chaque démon est
refusé.

## 14. Décisions prises et points ouverts

Décidé (F6BVP, 08/10/2026) :

- la porte de sortie d'un DNIC est déclarée par `RouteOrigin` ; une
  route manuelle seule ne suffit pas à l'annoncer ;
- une adresse de nœud apprise passe devant une route manuelle plus
  large ;
- plafond de 6 sauts et période de 30 minutes comme valeurs de
  départ ;
- premiers essais sur la paire f6bvp-12 / f6bvp-8.

Encore ouvert :

- faut-il un voisin de secours par destination en version 2 ?
- que faire si deux nœuds déclarent `RouteOrigin` pour le même DNIC :
  c'est permis (deux sorties réelles), chacun choisit la plus proche.
  À confirmer à l'usage.

Décidé (F6BVP, 13/10/2026) :

- un DNIC déclaré par `RouteOrigin` n'est annoncé que tant que son
  lien de sortie est ouvert, et retiré quand ce lien se ferme.
  Raison : le noyau ne fait sortir un appel en transit que par un
  voisin ouvert (un voisin fermé n'est essayé que pour un appel
  local), donc un DNIC annoncé sur une sortie fermée ne mène nulle
  part. La règle du noyau n'est pas changée. Codé le 13/10/2026
  (voir 4) et à l'essai depuis sur le nœud qui annonce, dont la
  table est passée de 6 routes à 2 : quatre de ses cinq DNIC
  avaient une sortie fermée.

## 15. Numérotation

Cette nouveauté ajoute un démon, un protocole entre nœuds et des
mots-clés de configuration : elle justifie FPAC 4.2. Ordre proposé :

1. sortir la 4.1.6 stable avec les correctifs de la rc7 ;
2. ouvrir la 4.2-rc1 avec `fpacrtd` et `WpIdle` (voir 16), tous
   deux inactifs par défaut, et le changement des vecteurs (voir 17).

Le code à l'essai est public : branche `fpac-4.2` de
<https://github.com/f6bvp/fpac>. Il est expérimental, ce n'est pas
une version publiée.

## 16. Second chantier : sessions White Pages fermées au repos

Décidé par F6BVP le 08/10/2026. Rien n'est encore codé.

Aujourd'hui, `fpacwpd` garde ouvert en permanence un circuit ROSE
avec chaque voisin (de WP-0 à WP-0), alors qu'il ne transporte
qu'une fiche de temps en temps et un échange de vecteurs par heure.

Nouveau mot-clé dans `fpac.conf` :

| Mot-clé   | Valeur             | Effet                          |
|-----------|--------------------|--------------------------------|
| `WpIdle`  | minutes, 0 = jamais| ferme une session WP au repos  |

Sans ce mot-clé, ou avec 0, rien ne change : la session reste
ouverte comme en 4.1. Valeur recommandée : `WpIdle = 10`.

Comportement avec `WpIdle = 10` :

- une session avec un voisin est fermée après 10 minutes sans
  aucune fiche ni vecteur échangé ;
- elle n'est rouverte qu'à la demande : quand une fiche doit être
  poussée vers ce voisin (modification locale ou relais), ou quand
  l'échange horaire de vecteurs est dû ;
- une session ouverte par le voisin est toujours acceptée ;
- le nouvel essai aveugle toutes les 30 minutes ne subsiste qu'après
  un échec de connexion, pas après une fermeture au repos.

Ce que le code demande :

- la liste des fiches en attente pour un voisin doit appartenir au
  voisin et non plus à la session, pour survivre à une fermeture ;
- le voisin doit être reconnu comme serveur WP (correctif de la
  4.1.6-rc7) ;
- aujourd'hui, une session qui s'ouvre lance un échange de vecteurs
  complet ; en rouvrir une pour pousser une seule fiche ne doit pas
  le lancer à chaque fois.

Avec un voisin resté en 4.1, qui rouvre sa session toutes les
30 minutes et la garde, le circuit est refermé 10 minutes plus tard.
C'est sans gravité, mais cela se voit dans les journaux.

Le noyau sait fermer seul un circuit ROSE au repos (option de socket
ROSE_IDLE). Une fermeture décidée par `fpacwpd` est préférée, parce
qu'il sait si un échange est en cours ; à trancher au moment du code.

Essai : sur une paire, vérifier qu'une session au repos se ferme,
qu'une fiche modifiée par `wpedit` la rouvre et arrive chez le
voisin, et que l'échange horaire de vecteurs a toujours lieu.

## 17. Troisième chantier : fiches supprimées obsolètes sorties des vecteurs

Constaté le 11/10/2026. Deux nœuds écrivaient `Vectors still
differ` chaque heure alors qu'ils avaient la même version.

Une fiche supprimée de plus de 14 jours (`WP_OBSOLETE`) n'est ni
envoyée ni acceptée entre nœuds, et une fiche supprimée d'un
indicatif inconnu est refusée. Ces fiches comptent pourtant dans les
vecteurs, dont la somme de contrôle couvre la date et l'adresse de
chaque fiche. Quand deux nœuds marquent une fiche supprimée, ou
l'effacent, à des jours différents, leurs vecteurs diffèrent et
l'échange ne peut pas le réparer : les fiches qui régleraient l'écart
sont justement celles qui ne s'échangent pas. Cela dure jusqu'à ce
que les deux nœuds les aient effacées.

Ici, un nœud lançait chaque nuit `wpmaint -d 100` quand les autres
lançaient `wpmaint -d 30` : il gardait 12 fiches supprimées que les
autres avaient effacées, et deux fiches encore valides, supprimées
ailleurs.

Décidé (F6BVP, 11/10/2026) : sortir de la somme de contrôle des
vecteurs les fiches supprimées trop anciennes pour être échangées.
Codé le 13/10/2026, pas encore en service.

Un nœud modifié et un nœud non modifié calculent des vecteurs
différents et divergeraient sans fin. Décidé (F6BVP, 13/10/2026) :
le numéro de version des vecteurs change avec la nouvelle somme de
contrôle ; c'est son rôle.

`fpacwpd` jusqu'à la 4.1.6 ferme la session White Pages quand il
reçoit un vecteur d'une autre version, et rappelle 30 minutes plus
tard. Sans rien de plus, un nœud 4.2 et un nœud 4.1.6 ne garderaient
plus de session. Décidé (F6BVP, 13/10/2026) : la 4.2 continue de
parler l'ancienne version, avec l'ancienne somme de contrôle, à un
voisin resté plus ancien ; la nouvelle version ne sert qu'entre
nœuds 4.2. Un réseau peut ainsi être mis à jour nœud par nœud.

Tel que codé le 13/10/2026 : la nouvelle version des vecteurs est
0x112, l'ancienne 0x111. Un nœud répond à une demande de vecteur
dans la version où elle est arrivée. Pour ses propres demandes, il
prend d'abord chaque voisin pour un nœud 4.2 ; si le voisin
raccroche avant de répondre, il lui parle ensuite dans l'ancienne
version, avec une ligne au journal. Un vecteur en nouvelle version
reçu de ce voisin prouve qu'il a été mis à jour. Comme une session
peut aussi tomber pour une autre raison à ce moment précis, la
nouvelle version est réessayée une fois par semaine. Le prix avec un
voisin 4.1 : une session fermée au premier échange, puis une fois
par semaine ; elle se rouvre dans les 30 minutes.

Non couvert : une fiche supprimée récemment, que l'autre nœud n'a
jamais connue, y est refusée et compte encore dans les vecteurs,
jusqu'à ce qu'elle devienne obsolète à son tour.

D'ici là : régler les mêmes délais de `wpmaint` sur tous les nœuds
d'un réseau. L'écart est sans gravité - trois tours de vecteurs par
heure au plus depuis la 4.1.6-rc7 - mais il remplit le journal.
