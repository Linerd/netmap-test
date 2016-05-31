#define NETMAP_WITH_LIBS
#include <stdio.h>
#include <net/netmap_user.h>
#include <sys/poll.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stddef.h>
#include <string.h>

#define RECEIVE_MODE 1
#define SEND_MODE 2
#define RELAY_MODE 3

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
	int attached;
	int cancel;
	int id;
	int fd;

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


static struct targ targs[1000];
static int global_nthreads;


/* control-C handler */
static void
sigint_h(int sig)
{
	int i;

	(void)sig;	/* UNUSED */
	D("received control-C on thread %p", (void *)pthread_self());
	for (i = 0; i < global_nthreads; i++) {
		targs[i].cancel = 0;
		targs[i].attached = 0;
	}
	signal(SIGINT, SIG_DFL);
}

static int
send_packets(struct netmap_ring *ring, u_int count, pkt_list_node_t **head, pkt_list_node_t **tail)
{
	u_int n, sent = ring->cur;

	n = nm_ring_space(ring);
	if (n < count)
		count = n;
	//D("Sending packets: %d", count);

	pkt_list_node_t *node = *head;

	for (sent = 0; (*head)!=NULL&&sent < count; sent++) {
		struct netmap_slot *slot = &ring->slot[ring->cur];
		slot->len = (*head)->len;
		char *p = NETMAP_BUF(ring, slot->buf_idx);
		if(nm_ring_empty(ring)){
			D("-- ouch, cannot send");
		}
		else{
			nm_pkt_copy((*head)->pkt, p, (*head)->len);
			*head = (*head)->next;

			ring->head = ring->cur = nm_ring_next(ring, ring->cur);
			free(node->pkt);
			free(node);
			node = (*head);
			if(*head==NULL)
				*tail = NULL;
		}
	}
	if(sent){
		D("Sent: %d", sent);
	}

	return sent;
}

static void *
sender_body(void *data)
{
	struct targ *targ = (struct targ *) data;
	struct pollfd pfd = { .fd = targ->fd, .events = POLLOUT };
	struct netmap_if *nifp;
	struct netmap_ring *txring = NULL;
	int i,m=0;
	
	D("start: sender_body");

	/* main loop.*/
	nifp = targ->nmd->nifp;

	while(!targ->cancel){

		while(!targ->attached||targ->mode==RECEIVE_MODE){};
		/*
		 * wait for available room in the send queue(s)
		 */
		if (poll(&pfd, 1, 2000) <= 0)
			continue;
		if (pfd.revents & POLLERR) {
			D("poll error on %d ring %d-%d", pfd.fd,
				targ->nmd->first_tx_ring, targ->nmd->last_tx_ring);
			break;
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
			m =	send_packets(txring, limit, &(targ->g->head), &(targ->g->tail));
			pthread_rwlock_unlock(&(targ->g->rwlock));
		}
		if(m){
			/* flush any remaining packets */
			//D("flush tail %d head %d on thread %p",
				//txring->tail, txring->head,
				//(void *)pthread_self());
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
		}
    } /* end DEV_NETMAP */
	return (NULL);
}

static void
receive_packets(struct netmap_ring *ring, u_int limit, pkt_list_node_t **head, pkt_list_node_t **tail)
{
	D("Receiving packets");
	u_int rx, n;

	n = nm_ring_space(ring);
	if (n < limit)
		limit = n;
	for (rx = 0; rx < limit; rx++) {
		struct netmap_slot *slot = &ring->slot[ring->cur];
		char *p = NETMAP_BUF(ring, slot->buf_idx);

		/* 
		 * Saving to memory, append to tail of queue
		 */
		char *pkt = (char*) malloc(slot->len);
		pkt_list_node_t *node = malloc(sizeof(pkt_list_node_t));
		node->pkt = memcpy(pkt, p, slot->len);
		//D("Packet_len: %d", slot->len);
		node->next = NULL;
		node->len = slot->len;
		if(*head==NULL){
			*head = node;
		}
		if(*tail!=NULL){
			(*tail)->next = node;
		}
		*tail = node;
		ring->head = ring->cur = nm_ring_next(ring, ring->cur);
	}
	D("Received: %d", rx);
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

	while(!targ->cancel){

		while(!targ->attached||targ->mode==SEND_MODE){};

		nifp = targ->nmd->nifp;

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
		ioctl(pfd.fd, NIOCRXSYNC, NULL);
    }

	return (NULL);
}

static void
start_thread(struct glob_arg *g, int ifnum, int mode){

	struct targ *t = &targs[ifnum];
	char ifname[20];
	struct nmreq nmr;
	int i;
	t->g = g;
	bzero(&nmr, sizeof(nmr));

	sprintf(ifname, "netmap:eth%d", ifnum);
	struct nm_desc *nmd = nm_open(ifname, &nmr, 0, NULL);

	if(nmd == NULL){
		D("Unable to open %s: %s", ifname, strerror(errno));
		return;
	}

	t->nmd = nmd;
	t->fd = t->nmd->fd;
	strcpy(t->ifname, ifname);
	nmd->self = nmd;
	t->id = ifnum;
	t->mode = mode;
	D("Wait %d secs for phy reset", 2);
	sleep(2);
	D("If %d is Ready... Mode: %d", ifnum, mode);

	for(i=0;i<2;i++){
		if(pthread_create(&t->thread, NULL, i==0?receiver_body:sender_body, t) == -1){
			D("Unable to create thread %d: %s", i, strerror(errno));
		}
	}
}

static void
set_mode(int ifnum, int mode){
	targs[ifnum].mode = mode;
}

static void
set_attach(int ifnum, int attach){
	targs[ifnum].attached = attach;
}


// the max connection number of the server
#define MAX_CONNECTION_NUMBER 1

/* * Create a server endpoint of a connection. * Returns fd if all OK, <0 on error. */
int
unix_socket_listen(const char *servername){
	int fd;
	struct sockaddr_un un; 
	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0){
		return(-1); 
	}
	int len, rval; 
	unlink(servername);               /* in case it already exists */ 
	memset(&un, 0, sizeof(un)); 
	un.sun_family = AF_UNIX; 
	strcpy(un.sun_path, servername); 
	len = offsetof(struct sockaddr_un, sun_path) + strlen(servername); 
	/* bind the name to the descriptor */ 
	if (bind(fd, (struct sockaddr *)&un, len) < 0){
		rval = -2; 
	} 
	else{
		if (listen(fd, MAX_CONNECTION_NUMBER) < 0){
			rval =  -3; 
		}
		else{
			return fd;
		}
	}
	int err;
	err = errno;
	close(fd); 
	errno = err;
	return rval;	
}

int
unix_socket_accept(int listenfd, uid_t *uidptr){
	int clifd, rval; 
	socklen_t len;
	//time_t staletime; 
	struct sockaddr_un un;
	struct stat statbuf; 
	len = sizeof(un); 
	if ((clifd = accept(listenfd, (struct sockaddr *)&un, &len)) < 0) {
			return(-1);     
	}
	/* obtain the client's uid from its calling address */ 
	len -= offsetof(struct sockaddr_un, sun_path);  /* len of pathname */
	un.sun_path[len] = 0; /* null terminate */ 
	if (stat(un.sun_path, &statbuf) < 0) {
			rval = -2;
	} 
	else{
		if (S_ISSOCK(statbuf.st_mode) ) {
			if (uidptr != NULL) 
				*uidptr = statbuf.st_uid;    /* return uid of caller */ 
			unlink(un.sun_path);       /* we're done with pathname now */ 
			return clifd;		 
		} 
		else {
			rval = -3;     /* not a socket */ 
		}
	}
	int err;
	err = errno; 
	close(clifd); 
	errno = err;
	return(rval);
}

void  
unix_socket_close(int fd){
	close(fd);     
}

static void
main_thread(struct glob_arg *g){
	int i, j;
	int ifnum=-1, mode=-1;
	int listenfd,connfd; 
	listenfd = unix_socket_listen("pbs.buffer1");
	if(listenfd<0){
		printf("Error[%d] when listening...\n",errno);
		return;
	}
	printf("Finished listening...\n");
	uid_t uid;
	connfd = unix_socket_accept(listenfd, &uid);
	unix_socket_close(listenfd);  
	if(connfd<0){
		printf("Error[%d] when accepting...\n",errno);
		return; 
	}  
	printf("Begin to recv/send...\n");
	int size;
	int rcvbuf[2];

	for(;;){
		ifnum=-1;
		mode=-1;
		size = recv(connfd, (char*)rcvbuf, 8, 0);   
		if(size>=0){
		// rvbuf[size]='\0';
			printf("Received Data[%d]:%d,%d\n",size,rcvbuf[0],rcvbuf[1]);
			ifnum = rcvbuf[0];
			mode = rcvbuf[1];
		}
		if(size==-1) {
			printf("Error[%d] when receiving Data:%s.\n",errno,strerror(errno));	 
			break;		
		}
		if(ifnum>=0 && mode>0){
			if(mode<10){
				if(!targs[ifnum].fd){
					start_thread(g, ifnum, mode);
					if(global_nthreads>0)
						global_nthreads++;
					else
						global_nthreads=1;
				}
				else{
					set_mode(ifnum, mode);
				}
			}
			if(mode>=10){
				set_attach(ifnum, mode-10);
			}
		}

		// close threads if necessary
		j = 0;
		for(i=0;i<global_nthreads;i++){
			if(targs[i].cancel){
				j++;
				pthread_join(targs[i].thread,NULL);
				munmap(targs[i].nmd->mem, targs[i].nmd->req.nr_memsize);
				close(targs[i].fd);
			}
		}
		if(j==global_nthreads){
			break;
		}
		sleep(2);
	}
	unix_socket_close(connfd);
}

int
main(int arc, char **argv){
	
	//int i;//,j;
	int ch;
	struct glob_arg g;
	g.burst = 512;

	while ( ( ch = getopt(arc, argv, 
		"b") ) != -1) {
		switch(ch) {
			/*
			case 'i':
				D("interface1 is %s", optarg);
				sprintf(ifname[0], "netmap:%s", optarg );
				break;
			case 'I':
				D("interface2 is %s", optarg);
				sprintf(ifname[1], "netmap:%s", optarg );
				break;
				*/
			case 'b':
				g.burst = atoi(optarg);
				break;
			default:
				break;
		}
	}
	global_nthreads = -1;

	pkt_list_node_t *head = NULL;
	g.tail = g.head = head;

	pthread_rwlock_t rwlock;
	if(pthread_rwlock_init(&rwlock, NULL)!=0){
		D("\nrwlock init failed\n");
		exit(1);
	}

	g.rwlock = rwlock;
	signal(SIGINT, sigint_h);

	main_thread(&g);
	pthread_rwlock_destroy(&(g.rwlock));	
	return 0;
}
