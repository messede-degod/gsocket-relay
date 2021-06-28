#include "common.h"
#include "gsrnd.h"
#include "engine.h"
#include "packet.h"
#include "utils.h"
#include "protocol.h"
#include "peer.h"
#include "gopt.h"


static void cb_evt_linger(int fd_notused, short event, void *arg);
static void cb_evt_shortwait(int fd_notused, short event, void *arg);

static void
tree_stats(void)
{
	int i;

	DEBUGF("Tree Stats (%d nodes)\n", gopt.t_peers.n_nodes);
	for (i = 0; i < MAX_LISTS_BY_ADDR; i++)
	{
		DEBUGF("LIST-ID=#%d(%s)  Entries=%d  Unique=%d\n", i, PEER_L_name(i), gopt.t_peers.n_entries[i], gopt.t_peers.n_uniq[i]);
	}
}

static void
cb_peers_printall(struct _peer *p, struct _peer_l_root *plr, void *arg)
{
	if (p == NULL)
	{
		if (plr == NULL)
			return;
		
		DEBUGF("List-#%d(%s) (%d entries):\n", PLR_L_get_id(plr), PEER_L_name(PLR_L_get_id(plr)), plr->n_entries);
		return;
	}

	DEBUGF("  [%u] %c addr=%s\n", p->id, IS_CS(p), strx128x(p->addr));
}

static struct _peer_l_mgr *
t_peer_get_mgr(const void *nodep, const VISIT which)
{
	if ((which == preorder) || (which == endorder))
		return NULL;

	if (nodep == NULL)
		return NULL;

	return *(struct _peer_l_mgr **)nodep;
}

void
cb_t_peers_walk(const void *nodep, const VISIT which, const int depth)
{
	struct _peer_l_mgr *pl_mgr = t_peer_get_mgr(nodep, which);
	if (pl_mgr == NULL)
		return;

	int i;
	for (i = 0; i < MAX_LISTS_BY_ADDR; i++)
	{
		// Call *func for each new list but with peer set to NULL.
		(*gopt.t_peers.walk_peers_func)(NULL, &pl_mgr->plr[i], gopt.t_peers.walk_peers_func_arg);
		struct _peer *p;
		TAILQ_FOREACH(p, &pl_mgr->plr[i].head, ll)
			(*gopt.t_peers.walk_peers_func)(p, &pl_mgr->plr[i], gopt.t_peers.walk_peers_func_arg);
	}
}

// Call *func(peer, plr, arg) for each peer in list.
//   *func is called with peer set to NULL for each list and then for each list
//   entry peer is set to the peer in that list. This allows *func to be called
//   even if the list is empty.
// A good example how to use PEER_walk() is in "cb_peers_printall" or "cb_cli_list".
void
PEERS_walk(walk_peers_func_t func, void *arg)
{
	gopt.t_peers.walk_peers_func = func;
	gopt.t_peers.walk_peers_func_arg = arg;

	twalk(gopt.t_peers.tree, cb_t_peers_walk);
}


// Each leaf is a double linked list of peer entries.
int
cb_t_peer_by_addr(const void *a, const void *b)
{
	struct _peer_l_mgr *pla = (struct _peer_l_mgr *)a;
	struct _peer_l_mgr *plb = (struct _peer_l_mgr *)b;

	if (pla->addr < plb->addr)
		return -1;

	if (pla->addr > plb->addr)
		return 1; // tree right

	return 0; // (found/exists)
}

// A little hack to make insertion faster when list
// does not exists.
int
cb_t_peer_find(const void *needle_a, const void *stack_b)
{
	struct _peer_l_mgr *pl_mgr_b = (struct _peer_l_mgr *)stack_b;
	uint128_t *addr_a = (uint128_t *)needle_a;

	DEBUGF_C("looking for %s @ %p\n", strx128x(*addr_a), stack_b);

	if (*addr_a < pl_mgr_b->addr)
		return -1;

	if (*addr_a > pl_mgr_b->addr)
		return 1; // tree right

	return 0; // (found/exists)

}

static int
pl_link(struct _peer_l_mgr *pl_mgr, struct _peer *p, peer_l_id_t pl_id, uint8_t *token)
{
	static uint8_t token_zero[GS_TOKEN_SIZE];
	struct _peer_l_root *plr = &pl_mgr->plr[pl_id];

	if (pl_id == PEER_L_LISTENING)
	{
		if (pl_mgr->flags & FL_PL_IS_TOKEN_SET)
		{
			// HERE: Token is set. Check token from peer.
			if ((token == NULL) || (memcmp(pl_mgr->token, token, sizeof pl_mgr->token) != 0))
			{
				DEBUGF_R("BAD TOKEN\n");
				return -1; // Bad Token
			}
			evtimer_del(pl_mgr->evt_linger);
		} else {
			// HERE: Token is not set for this manager
			if ((token != NULL) && (memcmp(token, token_zero, sizeof token_zero) != 0))
			{
				// Peer's token is set (not 0x000...00). Save it.
				pl_mgr->flags |= FL_PL_IS_TOKEN_SET;
				memcpy(pl_mgr->token, token, sizeof pl_mgr->token);
				if (pl_mgr->evt_linger == NULL)
					pl_mgr->evt_linger = evtimer_new(gopt.evb, cb_evt_linger, pl_mgr);
			}
		}
		if (pl_mgr->evt_shortwait != NULL)
			evtimer_del(pl_mgr->evt_shortwait);
	}

	// First time we add something to this list...
	if (TAILQ_EMPTY(&plr->head))
		gopt.t_peers.n_uniq[pl_id] += 1;

	TAILQ_INSERT_HEAD(&plr->head, p, ll);
	plr->n_entries += 1;
	pl_mgr->n_entries += 1;
	gopt.t_peers.n_entries[pl_id] += 1;

	p->plr = plr;

	p->state_usec = GS_usec();

	return 0;
}


struct _peer_l_mgr *
PEER_get_mgr(uint128_t addr)
{
	void *vptr;

	vptr = tfind(&addr, &gopt.t_peers.tree, cb_t_peer_find);
	if (vptr == NULL)
		return NULL;

	return *(struct _peer_l_mgr **)vptr;
}

void
PEER_by_addr(uint128_t addr, peer_func_t cb_peer, void *arg)
{
	struct _peer_l_mgr *pl_mgr;

	pl_mgr = PEER_get_mgr(addr);
	if (pl_mgr == NULL)
		return;

	// For each peer under this address call the callback....
	int i;
	int n;
	for (i = 0; i < MAX_LISTS_BY_ADDR; i++)
	{
		// The callback (cb_peer) might call PEER_free() which may in turn
		// free the tree node (pl_mgr). On return from cb_peer the tree node (pl_mgr)
		// might thus no longer be valid.
		// Solved by: Check n_entries and break the loop if only 1 entry was left
		// and dont touch pl_mgr thereafter (it might unallocated memory).
		struct _peer_l_root *plr = &pl_mgr->plr[i];
		if (TAILQ_EMPTY(&plr->head))
			continue;

		struct _peer *p;
		TAILQ_FOREACH(p, &plr->head, ll)
		{
			n = pl_mgr->n_entries;
			(*cb_peer)(p, arg);
			// WARNING: pl_mgr might have been freed.
			if (n <= 1)
				break; // This was the last entry
		}
		if (n <= 1)
			break;
	}
}

static struct _peer_l_root *
peer_find_root(uint128_t addr, peer_l_id_t pl_id, struct _peer_l_mgr **pl_mgr_ptr)
{
	struct _peer_l_mgr *pl_mgr;

	pl_mgr = PEER_get_mgr(addr);
	if (pl_mgr_ptr != NULL)
		*pl_mgr_ptr = pl_mgr; // NULL or pl_mgr

	if (pl_mgr == NULL)
		return NULL;

	struct _peer_l_root *plr;
	plr = &pl_mgr->plr[pl_id];
	if (TAILQ_EMPTY(&plr->head))
		return NULL;

	return plr;
}

// Add a Peer by peer->addr to a double-linked list which is
// linked to a binary tree
int
PEER_add(struct _peer *p, peer_l_id_t pl_id, uint8_t *token)
{
	struct _peer_l_mgr *pl_mgr = NULL;

	XASSERT(p->plr == NULL, "Oops. Peer already in a linked list\n");
	pl_mgr = PEER_get_mgr(p->addr);

	if (pl_mgr == NULL)
	{
		// HERE: New leaf in binary tree
		DEBUGF("Creating a new linked list. (list-mgr=%p, list=%d) tree=%p\n", pl_mgr, pl_id, gopt.t_peers.tree);

		pl_mgr = calloc(1, sizeof (struct _peer_l_mgr));
		pl_mgr->addr = p->addr;
		pl_mgr->evt_shortwait = evtimer_new(gopt.evb, cb_evt_shortwait, pl_mgr);

		int i;
		for (i = 0; i < MAX_LISTS_BY_ADDR; i++)
		{
			TAILQ_INIT(&pl_mgr->plr[i].head);
			pl_mgr->plr[i].pl_mgr = pl_mgr;
		}

		tsearch(pl_mgr, &gopt.t_peers.tree, cb_t_peer_by_addr);  // Add to binary tree
		gopt.t_peers.n_nodes += 1;
	} else {
		// HERE: Existing linked list (peer with same addr already exists)
		DEBUGF_W("Using existing list-mgr=%p. Already has %d entries\n", pl_mgr, pl_mgr->n_entries);
	}
	int ret;
	ret = pl_link(pl_mgr, p, pl_id, token);
	if (ret != 0)
		return ret; // Bad Token;

	return 0;
}

// Unlink from list (but do not remove binary tree entry
static void
pl_unlink(struct _peer *p)
{
	peer_l_id_t pl_id = PEER_L_get_id(p);
	struct _peer_l_mgr *pl_mgr = PEER_L_get_mgr(p);
	p->plr->n_entries -= 1;
	TAILQ_REMOVE(&p->plr->head, p, ll);

	if (TAILQ_EMPTY(&p->plr->head))
	{
		gopt.t_peers.n_uniq[pl_id] -= 1;
		if (pl_id == PEER_L_LISTENING)
		{
			// Give listening gsocket time to re-connect and delay client
			// disconnects. We start the timer when the _last_ listening
			// server is no longer listening (e.g disconnects or is moved into
			// another state). Meanwhile if any clients connect while no 
			// server is listening and withitn SHORTWAIT_TIMEOUT then we put
			// those clients into a short-wait holding pattern before disconnecting them.
			evtimer_add(pl_mgr->evt_shortwait, TVSEC(GSRN_SHORTWAIT_TIMEOUT));

			// This was the last listening server. Prevent others from impersonating
			// this server for a while and allow the original server to connect again
			// to register another listening gsocket.
			if (pl_mgr->flags & FL_PL_IS_TOKEN_SET)
			{
				DEBUGF_C("Last listening peer. Token was set...\n");
				evtimer_add(pl_mgr->evt_linger, TVSEC(GSRN_TOKEN_LINGER));
			}
		}
	}

	pl_mgr->n_entries -= 1;

	gopt.t_peers.n_entries[pl_id] -= 1;

}

// Move from 1 list to another or add if currently in no list.
void
PEER_L_mv(struct _peer *p, peer_l_id_t pl_id)
{
	if (p->plr == NULL)
	{
		DEBUGF_R("WARN: Peer is not in any list yet. Adding to %d...\n", pl_id);
		PEER_add(p, pl_id, NULL);
		return;
	}

	// Check if already in correct list.
	if (PEER_L_get_id(p) == pl_id)
		return;

	// Remove from current list.
	pl_unlink(p);

	// Add to new list
	pl_link(p->plr->pl_mgr, p, pl_id, NULL);
}

// Return oldest listening peer.
struct _peer *
PEER_get(uint128_t addr, peer_l_id_t pl_id, struct _peer_l_mgr **pl_mgr_ptr)
{
	struct _peer_l_root *plr;

	plr = peer_find_root(addr, pl_id, pl_mgr_ptr);
	if (plr == NULL)
		return NULL;

	return (struct _peer *)TAILQ_LAST(&plr->head, _listhead); // Oldest from the tailq
}

static void
pl_mgr_t_free(struct _peer_l_mgr *pl_mgr)
{
	if (pl_mgr->n_entries > 0)
		DEBUGF_R("WARN: tree-node still has %d entries\n", pl_mgr->n_entries);

	if (pl_mgr->evt_linger != NULL)
	{
		evtimer_del(pl_mgr->evt_linger);
		event_free(pl_mgr->evt_linger);
	}

	if (pl_mgr->evt_shortwait != NULL)
	{
		evtimer_del(pl_mgr->evt_shortwait);
		event_free(pl_mgr->evt_shortwait);
	}

	tdelete(pl_mgr, &gopt.t_peers.tree, cb_t_peer_by_addr);
	gopt.t_peers.n_nodes -= 1;
	free(pl_mgr);
}

// The MGR for the gsocket addr had no listening gsockets and no new
// listening gsockets were created within the timeout period.
// => Remove the MGR. This will allow clients with different tokens
//    to create a listening gsocket using the same address (e.g. impersonate
//    the old listening server).
static void
cb_evt_linger(int fd_notused, short event, void *arg)
{
	struct _peer_l_mgr *pl_mgr = (struct _peer_l_mgr *)arg;

	DEBUGF_C("addr=%s Timeout(%d sec). No listening socket created. Deleting token.\n", strx128x(pl_mgr->addr), GSRN_TOKEN_LINGER);
	// Allow others to use the this addr to create listening gsockets
	pl_mgr->flags &= ~FL_PL_IS_TOKEN_SET;

	if (pl_mgr->n_entries <= 0)
	{
		pl_mgr_t_free(pl_mgr);
		pl_mgr = NULL;
	}
}

static void
cb_evt_shortwait(int fd_notused, short event, void *arg)
{
	struct _peer_l_mgr *pl_mgr = (struct _peer_l_mgr *)arg;
	struct _peer_l_root *plr = &pl_mgr->plr[PEER_L_WAITING];

	if (plr == NULL)
		return;

	if (plr->n_entries <= 0)
		return;

	DEBUGF_W("addr=%s Timeout(%d sec). No server connected. Disconnecting %d clients\n", strx128x(pl_mgr->addr), GSRN_SHORTWAIT_TIMEOUT, plr->n_entries);

	struct _peer *p;
	TAILQ_FOREACH(p, &plr->head, ll)
	{
		if (!(PEER_IS_SHORTWAIT(p)))
			continue;
		DEBUGF_W("  [%u] Disconnecting\n", p->id);
		GSRN_send_status_fatal(p, GS_STATUS_CODE_CONNREFUSED, NULL);
		PEER_goodbye(p);	
	}
}

// Remove peer from double linked list and if this is the last peer then also
// remove the list-root from the binary tree.
static void
peer_t_del(struct _peer *p)
{
	if ((p == NULL) || (p->plr == NULL))
		return; // Not in any list

	pl_unlink(p);
	struct _peer_l_mgr *pl_mgr = PEER_L_get_mgr(p);

	if ((pl_mgr->evt_linger != NULL) && (evtimer_pending(pl_mgr->evt_linger, NULL)))
		goto done;

	if (pl_mgr->n_entries <= 0)
	{
		DEBUGF_C("list=%ld This was the last peer with addr=%s\n", PEER_L_get_id(p), strx128x(p->addr));
		pl_mgr_t_free(pl_mgr);
		pl_mgr = NULL;
	}

done:
	p->plr = NULL;
}


// When there are no pending data in the 'out' buffer then free the peer now.
// Otherwise give I/O time to send the data to the peer and free peer when
// a. all data has been written (in cb_bev_write)
// b. the write timeout has expired (in cb_bev_status)
void
PEER_goodbye(struct _peer *p)
{
	struct evbuffer *out = bufferevent_get_output(p->bev);
	size_t sz;

	// Remove myself from lists. No longer available to any GS-messages
	peer_t_del(p);

	sz = evbuffer_get_length(out);

	if (sz <= 0)
	{
		PEER_free(p, 1);
		return;
	}

	DEBUGF("%zu bytes left to write\n", sz);
	p->flags |= FL_PEER_IS_GOODBYE;
	bufferevent_set_timeouts(p->bev, NULL, TVSEC(GSRN_FLUSH_TV_TIMEOUT));
	bufferevent_enable(p->bev, EV_WRITE);
	PKT_set_void(&p->pkt);
}

// Free a peer and also free its buddy if is_free_buddy is set.
void
PEER_free(struct _peer *p, int is_free_buddy)
{
	DEBUGF_G("%s peer=%p\n", __func__, p);

#ifdef DEBUG
	// tree_stats();
	// PEERS_walk(cb_peers_printall, NULL);
#endif

	// Remove myself from lists
	peer_t_del(p);

	size_t sz = evbuffer_get_length(bufferevent_get_output(p->bev));
	if (sz > 0)
		DEBUGF_R("WARN: Free'ing peer with %zu left in output buffer\n", sz);
	XBEV_FREE(p->bev);

	// XEVT_FREE(p->evt_shutdown);

	// Unlink myself from my buddy
	if ((is_free_buddy != 0) && (p->buddy))
	{
		// p->buddy->buddy = NULL;
		PEER_free(p->buddy, 0); // Disconnect my buddy.
		p->buddy = NULL;
	}

	XCLOSE(p->fd);
	XFREE(p);
}


#define GSRN_BPS_WINDOW       GS_SEC_TO_USEC(10)

// Return an estimation of Bytes/Second that is not to jumpy.
// The 'old' recorded value (before current 10 seconds)
// Bytes are the number of bytes transfered within current period of length usec.
static uint32_t
bps_calc(uint32_t old, uint64_t bytes, uint64_t usec)
{
	uint32_t bps = 0;

	if (usec > 0)
		bps = (bytes * 1000000) / usec;

	if (usec >= GSRN_BPS_WINDOW)
		return bps;

	// Calculate how much the current bytes / usec are relevant.
	// For example after 0.1 seconds they only have 1% relevance
	// (assuming a 10 second measuement interval) but after 9 seconds
	// they are 90% relevant!
	return old * (GS_SEC_TO_USEC(10) - usec) / GS_SEC_TO_USEC(10) + (bps * usec / GS_SEC_TO_USEC(10));
}

uint32_t
PEER_get_bps(struct _peer *p)
{
	return bps_calc(p->bps_last, (p->in_n + p->out_n) - p->bps_last_inout, gopt.usec_now - p->bps_last_usec);
}

void
PEER_stats_update(struct _peer *p, struct evbuffer *eb)
{
	size_t len = evbuffer_get_length(eb);
	gopt.usec_now = GS_usec();

	// For stats try to figure out if peers negotiate an SSL connection
	if (p->in_n == 0)
	{
		// Check for 0x16 (22 ClientHelo) being the first octet.
		// TLS 1.2/1.2 = 16 03 03 ...
		uint8_t c;
		evbuffer_copyout(eb, &c, 1);
		if (c == 0x16)
			p->flags |= FL_PEER_IS_SAW_CLIENTHELO;
	}
#ifdef DEBUG
	if (p->in_n == 0)
	{
		uint8_t buf[MIN(128, len)];
		evbuffer_copyout(eb, buf, MIN(128, len));
		HEXDUMP(buf, MIN(128, len));
	}	
#endif
	p->in_n += len;
	p->in_last_usec = gopt.usec_now;

	// -----BEGIN BPS STATS-----
	if (p->bps_last_usec == 0)
		p->bps_last_usec = gopt.usec_now;
	else if (gopt.usec_now - p->bps_last_usec >= GSRN_BPS_WINDOW)
	{
		p->bps_last = bps_calc(p->bps_last, (p->in_n + p->out_n) - p->bps_last_inout, gopt.usec_now - p->bps_last_usec);
		p->bps_last_usec = gopt.usec_now;
		p->bps_last_inout = p->in_n + p->out_n;
	}
	// -----END BPS STATS-----

	if (p->buddy == NULL)
		return;
	
	p->buddy->out_n += len;
	p->buddy->out_last_usec = p->in_last_usec;
}

struct _peer *
PEER_new(int fd, SSL *ssl)
{
	struct _peer *p;

	p = calloc(1, sizeof *p);
	if (p == NULL)
	{
		// FIXME: Log this failure
		return NULL;
	}

	PKT_init(&p->pkt);

	// Assign a uniq PEER id to each instance (start with 1)
	gd.peer_id += 1;
	p->id = gd.peer_id;
	p->fd = fd;

	socklen_t slen = sizeof (struct sockaddr_in);
	getpeername(fd, (struct sockaddr *)&p->addr_in, &slen);
	DEBUGF_B("peer=%p fd=%d ip=%s:%u\n", p, fd, int_ntoa(p->addr_in.sin_addr.s_addr), ntohs(p->addr_in.sin_port));

	int ev_opt = BEV_OPT_DEFER_CALLBACKS;
	if (ssl != NULL)
	{
		p->ssl = ssl;
		SSL_set_fd(p->ssl, fd);
		p->bev = bufferevent_openssl_socket_new(gopt.evb, -1, p->ssl, BUFFEREVENT_SSL_ACCEPTING, ev_opt);
	} else {
		p->bev = bufferevent_socket_new(gopt.evb, fd, ev_opt);
	}

	bufferevent_setcb(p->bev, cb_bev_read, cb_bev_write /*NULL*/, cb_bev_status, p);
	bufferevent_set_timeouts(p->bev, TVSEC(GSRN_1STMSG_TIMEOUT) /*read*/, NULL /*write*/);

	if (ssl == NULL)
		cb_bev_status(p->bev, BEV_EVENT_CONNECTED, p); // Immediately go into 'connected' state

	GSRN_change_state(p, GSRN_STATE_INIT);

	if (ssl != NULL)
	{
		// When using SSL then also allow auth
		// PKT_setcb(&p->pkt, GS_PKT_TYPE_AUTH, 0 /*variable length*/, p);
	}

	return p;
}


