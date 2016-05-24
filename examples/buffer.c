#include <stdio.h>
#include <net/netmap_user.h>

#include <pthread.h>

/*
 * packet buffer node in FIFO queue
 */
typedef struct node{
	char *pkt;
	int len;
	struct node *next;	
} pkt_list_node_t;

struct glob_arg {
	int burst;
	pkt_list_node_t *head;
	pkt_list_node_t *tail;
	pthread_rwlock_t rwlock;
};

struct targ{
	struct glob_arg *g;
	bool attached;
	int id;

	/*
	* 0 for rx; 
	* 1 for tx; 
	* 2 for both;
	*/
	int mode;
	char ifname[20];
	struct nm_desc *nmd;

	pthread_t thread;

};

/* control-C handler */
static void
sigint_h(int sig)
{
	int i;

	(void)sig;	/* UNUSED */
	D("received control-C on thread %p", (void *)pthread_self());
	for (i = 0; i < global_nthreads; i++) {
		targs[i].attached = false;
	}
	signal(SIGINT, SIG_DFL);
}

static void
send_packets(struct netmap_ring *ring, u_int count, pkt_list_node_t **head, pkt_list_node_t **tail)
{
	u_int n, sent, cur = ring->cur;

	n = nm_ring_space(ring);
	if (n < count)
		count = n;

	pkt_list_node_t *node = *head;

	for (sent = 0; head!=NULL&&sent < count; sent++) {
		struct netmap_slot *slot = &ring->slot[cur];
		char *p = NETMAP_BUF(ring, slot->buf_idx);
		memcpy(p, *head->pkt, *head->len);
		*head = *head->next;

		free(node->pkt);
		free(node);
		node = *head;
		if(*head==NULL)
			*tail = NULL;

		cur = nm_ring_next(ring, cur);
	}
	ring->head = ring->cur = cur;
}

static void *
sender_body(void *data)
{
	struct targ *targ = (struct targ *) data;
	struct pollfd pfd = { .fd = targ->fd, .events = POLLOUT };
	struct netmap_if *nifp;
	struct netmap_ring *txring = NULL;
	int i, n = targ->g->npackets / targ->g->nthreads;
	
	D("start: sender_body");

	/* main loop.*/
	nifp = targ->nmd->nifp;
	while (targ->attached) {

	/*
	 * wait for available room in the send queue(s)
	 */
		if (poll(&pfd, 1, 2000) <= 0)
			continue;
		if (pfd.revents & POLLERR) {
			D("poll error on %d ring %d-%d", pfd.fd,
				targ->nmd->first_tx_ring, targ->nmd->last_tx_ring);
			goto quit;
		}
		/*
		 * scan our queues and send on those with room
		 */
		for (i = targ->nmd->first_tx_ring; i <= targ->nmd->last_tx_ring; i++) {
			int limit = targ->g->burst;
			txring = NETMAP_TXRING(nifp, i);
			if (nm_ring_empty(txring))
				continue;

			pthread_rwlock_wrlock(&(targ->g->rwlock));
			send_packets(txring, limit, &(targ->g->head), &(targ->g->tail));
			pthread_rwlock_unlock(&(targ->g->rwlock));
		}
		/* flush any remaining packets */
		D("flush tail %d head %d on thread %p",
			txring->tail, txring->head,
			(void *)pthread_self());
		ioctl(pfd.fd, NIOCTXSYNC, NULL);

		/* final part: wait all the TX queues to be empty. */
		for (i = targ->nmd->first_tx_ring; i <= targ->nmd->last_tx_ring; i++) {
			txring = NETMAP_TXRING(nifp, i);
			while (nm_tx_pending(txring)) {
				RD(5, "pending tx tail %d head %d on ring %d",
					txring->tail, txring->head, i);
				ioctl(pfd.fd, NIOCTXSYNC, NULL);
				usleep(1); /* wait 1 tick */
			}
		}
    } /* end DEV_NETMAP */
	return (NULL);
}

static void
receive_packets(struct netmap_ring *ring, u_int limit, pkt_list_node_t **head, pkt_list_node_t **tail)
{
	u_int cur, rx, n;
	uint64_t b = 0;

	cur = ring->cur;
	n = nm_ring_space(ring);
	if (n < limit)
		limit = n;
	for (rx = 0; rx < limit; rx++) {
		struct netmap_slot *slot = &ring->slot[cur];
		char *p = NETMAP_BUF(ring, slot->buf_idx);

		/* 
		 * Saving to memory, append to tail of queue
		 */
		char *pkt = (char*) malloc(slot->len);
		pkt_list_node_t *node = malloc(sizeof(pkt_list_node_t));
		node->pkt = memcpy(pkt, p, slot->len);
		node->next = NULL;
		node->len = slot->len;
		if(*head==NULL){
			*head = node;
		}
		if(*tail!=NULL){
			*tail->next = node;
		}
		*tail = node;

		cur = nm_ring_next(ring, cur);
	}
	ring->head = ring->cur = cur;
}

static void *
receiver_body(void *data)
{
	struct targ *targ = (struct targ *) data;
	struct pollfd pfd = { .fd = targ->fd, .events = POLLIN };
	struct netmap_if *nifp;
	struct netmap_ring *rxring;
	int i;

	D("reading from %s",
		targ->ifname);
	/* unbounded wait for the first packet. */
	for (;!targ->cancel;) {
		i = poll(&pfd, 1, 1000);
		if (i > 0 && !(pfd.revents & POLLERR))
			break;
		RD(1, "waiting for initial packets, poll returns %d %d",
			i, pfd.revents);
	}
	/* main loop, exit after 1s silence */
	nifp = targ->nmd->nifp;

	while (targ->attached) {

		if( poll(&pfd, 1, 1000) <=0)
			continue;

		/* Once we started to receive packets, wait at most 1 seconds
		   before quitting. */
		if (pfd.revents & POLLERR) {
			D("poll err");
			break;
		}
		for (i = targ->nmd->first_rx_ring; i <= targ->nmd->last_rx_ring; i++) {
			rxring = NETMAP_RXRING(nifp, i);
			if (nm_ring_empty(rxring))
				continue;

			pthread_rwlock_wrlock(&(targ->g->rwlock)); 
			receive_packets(rxring, targ->g->burst, &(targ->g->head), &(targ->g->tail));
			pthread_rwlock_unlock(&(targ->g->rwlock)); 
		}
    }

	return (NULL);
}

// static void
// start_thread(struct glob_arg *g)
// {
// 	int i;

// 	struct targ t;
// 	struct nmreq nmr;

// 	bzero(&t, sizeof(t));
// 	t->fd = -1;
// 	t->g = g;


// 	bzero(&nmr, sizeof(nmr));

// 	nmr.nr_flags |= NR_ACCEPT_VNET_HRD;

// 	struct nm_desc nmd = nm_open(ifname, &nmr, 0, NULL);
// 	nmd.self = &nmd;	
// 	if(nmd == NULL){
// 		D("Unable to open %s: %s", ifname, strerror(errno));
// 		exit(0);
// 	}
// 	/*
// 	 * Now create the desired number of threads, each one
// 	 * using a single descriptor.
//  	 */
// 	for (i = 0; i < g->nthreads; i++) {
// 		struct targ *t = &targs[i];

// 		bzero(t, sizeof(*t));
// 		t->fd = -1; /* default, with pcap */
// 		t->g = g;

// 	    if (g->dev_type == DEV_NETMAP) {
// 		struct nm_desc nmd = *g->nmd; /* copy, we overwrite ringid */
// 		uint64_t nmd_flags = 0;
// 		nmd.self = &nmd;

// 		if (i > 0) {
// 			/* the first thread uses the fd opened by the main
// 			 * thread, the other threads re-open /dev/netmap
// 			 */
// 			if (g->nthreads > 1) {
// 				nmd.req.nr_flags =
// 					g->nmd->req.nr_flags & ~NR_REG_MASK;
// 				nmd.req.nr_flags |= NR_REG_ONE_NIC;
// 				nmd.req.nr_ringid = i;
// 			}
// 			/* Only touch one of the rings (rx is already ok) */
// 			//if (g->td_type == TD_TYPE_RECEIVER)
// 				// nmd_flags |= NETMAP_NO_TX_POLL;

// 			/* register interface. Override ifname and ringid etc. */
// 			t->nmd = nm_open(t->g->ifname, NULL, nmd_flags |
// 				NM_OPEN_IFNAME | NM_OPEN_NO_MMAP, &nmd);
// 			if (t->nmd == NULL) {
// 				D("Unable to open %s: %s",
// 					t->g->ifname, strerror(errno));
// 				continue;
// 			}
// 		} else {
// 			t->nmd = g->nmd;
// 		}
// 		t->fd = t->nmd->fd;

// 	    } else {
// 			targs[i].fd = g->main_fd;
// 	    }
// 		t->used = 1;
// 		t->me = i;
// 		if (g->affinity >= 0) {
// 			t->affinity = (g->affinity + i) % g->system_cpus;
// 		} else {
// 			t->affinity = -1;
// 		}
// 		/* default, init packets */
// 		// initialize_packet(t);

// 		if (pthread_create(&t->thread, NULL, g->td_body, t) == -1) {
// 			D("Unable to create thread %d: %s", i, strerror(errno));
// 			t->used = 0;
// 		}
// 	}
// }


static struct targ *targs;
static int global_nthreads;

int
main(int arc, char **argv){
	
	int i;
	int ch;
	struct glob_arg g;
	char ifname[2][20];

	while ( ( ch = getopt(arc, argv, 
		"i:b") ) != -1) {
		switch(ch) {
			case 'i1':
				D("interface1 is %s", optarg);
				ifname[0] = optarg;
				break;
			case 'i2':
				D("interface2 is %s", optarg);
				ifname[1] = optarg;
				break;
			case 'b':
				g.burst = atoi(optarg);
				break;
		}
	}
	global_nthreads = 2;

	pkt_list_node_t *head = NULL;
	g.tail = g.head = head;

	pthread_rwlock_t rwlock;
	if(pthread_rwlock_init(&rwlock, NULL)!=0){
		D("\nrwlock init failed\n");
		exit(1);
	}

	g.rwlock = rwlock;

	targs = calloc(2, sizeof(*targs));

	signal(SIGINT, sigint_h);

	// struct nmreq base_nmd;

	// bzero(&base_nmd, sizeof(base_nmd));

	// base_nmd.nr_flags |=NR_ACCEPT_VNET_HRD;

	// struct nm_desc nmd = nm_open()

	for(i=0;i<2;i++){
		struct targ *t = &targs[i];
		bzero(t, sizeof(*t));
		t->g = g;

		struct nmreq nmr;

		bzero(&nmr, sizeof(nmr));

		nmr.nr_flags |= NR_ACCEPT_VNET_HRD;

		struct nm_desc nmd = nm_open(ifname[i], &nmr, 0, NULL);
		if(nmd == NULL){
			D("Unable to open %s: %s", ifname, strerror(errno));
			exit(0);
		}
		t->nmd = &nmd;
		t->ifname = ifname[i];
		nmd.self = &nmd;
		t->id = i;

		D("Wait %d secs for phy reset", 2);
		sleep(2);
		D("Ready...");

		if(pthread_create(&t->thread, NULL, i==0?receiver_body:sender_body, t) == -1){
			D("Unable to create thread %d: %s", i, strerror(errno));
		}
	}
}