/***************************************************************

  Copyright (c) 2012, Nicola Bonelli 
  All rights reserved. 

  Redistribution and use in source and binary forms, with or without 
  modification, are permitted provided that the following conditions are met: 

 * Redistributions of source code must retain the above copyright notice, 
 this list of conditions and the following disclaimer. 
 * Redistributions in binary form must reproduce the above copyright 
 notice, this list of conditions and the following disclaimer in the 
 documentation and/or other materials provided with the distribution. 
 * Neither the name of University of Pisa nor the names of its contributors 
 may be used to endorse or promote products derived from this software 
 without specific prior written permission. 

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" 
 AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE 
 LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR 
 CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF 
 SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) 
 ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE 
 POSSIBILITY OF SUCH DAMAGE.

 ***************************************************************/

#include <linux/if_ether.h>
#include <linux/pf_q.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <net/if.h>
#include <net/ethernet.h>
#include <arpa/inet.h>

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <poll.h>

/* pfq descriptor */

typedef char * pfq_iterator_t;

struct pfq_net_queue
{	
	pfq_iterator_t queue; 	  		/* net queue */
	size_t         len;       		/* number of packets in the queue */
    	size_t         slot_size;
	unsigned int   index; 	  		/* current queue index */ 
};


typedef struct 
{
	void * queue_addr;

	size_t queue_tot_mem;
	size_t queue_slots; 
	size_t queue_caplen;
	size_t queue_offset;
	size_t slot_size;

	const char * error;

	int fd;
	int id;
	int gid;

	struct pfq_net_queue netq;
	pfq_iterator_t 	it;
} pfq_t;


#define PFQ_LIBRARY 
#include <pfq.h>

#define  ALIGN8(value) ((value + 7) & ~(__typeof__(value))7)

#define max(a,b) \
	({ __typeof__ (a) _a = (a); \
	   __typeof__ (b) _b = (b); \
	  _a > _b ? _a : _b; })

#define min(a,b) \
	({ __typeof__ (a) _a = (a); \
	   __typeof__ (b) _b = (b); \
	  _a < _b ? _a : _b; })


/* return the string error */

static __thread const char * __error;

const char *pfq_error(pfq_t *q)
{
	return q == NULL ? __error : q->error;
}


/* costructor */

pfq_t *
pfq_open(size_t caplen, size_t offset, size_t slots)        
{
	return pfq_open_group(Q_CLASS_DEFAULT, Q_GROUP_PRIVATE, caplen, offset, slots); 
}


pfq_t *
pfq_open_nogroup(size_t caplen, size_t offset, size_t slots)
{
	return pfq_open_group(Q_CLASS_DEFAULT, Q_GROUP_UNDEFINED, caplen, offset, slots); 
}


pfq_t *
pfq_open_group(unsigned long class_mask, int group_policy, size_t caplen, size_t offset, size_t slots)
{
	int fd = socket(PF_Q, SOCK_RAW, htons(ETH_P_ALL));
	pfq_t * q;

	if (fd == -1) {
		return __error = "PFQ: module not loaded", NULL;
	}

	q = (pfq_t *) malloc(sizeof(pfq_t));
	if (q == NULL) {
		return __error = "PFQ: out of memory", NULL;
	}

	q->fd 	= fd;  
	q->id 	= -1;
	q->gid 	= -1;

	q->queue_addr 	 = NULL;
	q->queue_tot_mem = 0;
	q->queue_slots   = 0;
	q->queue_caplen  = 0;
	q->queue_offset  = offset;
	q->slot_size     = 0;
	q->error 	 = NULL;
        memset(&q->netq, 0, sizeof(q->netq));

	/* get id */
	socklen_t size = sizeof(q->id);
	if (getsockopt(fd, PF_Q, Q_SO_GET_ID, &q->id, &size) == -1) {
		return __error = "PFQ: GET_ID error", free(q), NULL;
	}

	/* set queue slots */
	if (setsockopt(fd, PF_Q, Q_SO_SET_SLOTS, &slots, sizeof(slots)) == -1) {
		return __error = "PFQ: SET_SLOTS error", free(q), NULL;
	}

	q->queue_slots = slots;

	/* set caplen */
	if (setsockopt(fd, PF_Q, Q_SO_SET_CAPLEN, &caplen, sizeof(caplen)) == -1) {
		return __error = "PFQ: SET_CAPLEN error", free(q), NULL;
	}

	q->queue_caplen = caplen;

	/* set offset */
	if (setsockopt(fd, PF_Q, Q_SO_SET_OFFSET, &offset, sizeof(offset)) == -1) {
		return __error = "PFQ: SET_OFFSET error", free(q), NULL;
	}

	q->slot_size = ALIGN8(sizeof(struct pfq_hdr) + q->queue_caplen);
	
	if (group_policy != Q_GROUP_UNDEFINED)
	{
		q->gid = pfq_join_group(q, Q_ANY_GROUP, class_mask, group_policy);
		if (q->gid == -1) {
			return __error = q->error, free(q), NULL;
		}
	}
	
	return __error = NULL, q;
}


int pfq_close(pfq_t *q)
{
	if (q->fd != -1) 
	{
		if (q->queue_addr) 
			pfq_disable(q);
		
		if (close(q->fd) < 0)
			return q->error = "PFQ: close error", -1;
	}
	return q->error = "PFQ: socket not open", -1;
}


int
pfq_enable(pfq_t *q)
{
	int one = 1;

	if(setsockopt(q->fd, PF_Q, Q_SO_TOGGLE_QUEUE, &one, sizeof(one)) == -1) {
		return q->error = "PFQ: TOGGLE_QUEUE error", -1;
	}

	size_t tot_mem; socklen_t size = sizeof(tot_mem);

	if (getsockopt(q->fd, PF_Q, Q_SO_GET_QUEUE_MEM, &tot_mem, &size) == -1) {
		return q->error = "PFQ: GET_QUEUE_MEM error", -1;
	}

	q->queue_tot_mem = tot_mem;

	if ((q->queue_addr = mmap(NULL, tot_mem, PROT_READ|PROT_WRITE, MAP_SHARED, q->fd, 0)) == MAP_FAILED) { 
		return q->error = "PFQ: mmap error", -1;
	}
        return q->error = NULL, 0;
}


int 
pfq_disable(pfq_t *q)
{
	if (munmap(q->queue_addr,q->queue_tot_mem) == -1) {
		return q->error = "PFQ: munmap error", -1;
	}

	q->queue_addr = NULL;
	q->queue_tot_mem = 0;

	int one = 0;
	if(setsockopt(q->fd, PF_Q, Q_SO_TOGGLE_QUEUE, &one, sizeof(one)) == -1) {
		return q->error = "PFQ: TOGGLE_QUEUE error", -1;
	}
	return q->error = NULL, 0;
}


int
pfq_is_enabled(pfq_t const *q)
{
	pfq_t * mutable = (pfq_t *)q;
	if (q->fd != -1)
	{
		int ret; socklen_t size = sizeof(ret);
		if (getsockopt(q->fd, PF_Q, Q_SO_GET_STATUS, &ret, &size) == -1) {
			return mutable->error = "PFQ: GET_STATUS error", -1;
		}
		return mutable->error = NULL, ret;
	}
	return mutable->error = NULL, 0;
}


int
pfq_set_timestamp(pfq_t *q, int value)
{
	int ts = value;
	if (setsockopt(q->fd, PF_Q, Q_SO_SET_TSTAMP, &ts, sizeof(ts)) == -1) {
		return q->error = "PFQ: SET_TSTAMP error", -1;
	}
	return q->error = NULL, 0;
}


int
pfq_get_timestamp(pfq_t const *q)
{
	pfq_t * mutable = (pfq_t *)q;
	int ret; socklen_t size = sizeof(int);
	
	if (getsockopt(q->fd, PF_Q, Q_SO_GET_TSTAMP, &ret, &size) == -1) {
	        return mutable->error = "PFQ: GET_TSTAMP error", -1;
	}
	return mutable->error = NULL, ret;
}


int
pfq_ifindex(pfq_t const *q, const char *dev)
{
	struct ifreq ifreq_io;
	pfq_t * mutable = (pfq_t *)q;
	
	memset(&ifreq_io, 0, sizeof(struct ifreq));
	strncpy(ifreq_io.ifr_name, dev, IFNAMSIZ);
	if (ioctl(q->fd, SIOCGIFINDEX, &ifreq_io) == -1) {
		return mutable->error = "PFQ: ioctl SIOCGIFINDEX error", -1;
	}
	return mutable->error = NULL, ifreq_io.ifr_ifindex;
}


int 
pfq_set_promisc(pfq_t const *q, const char *dev, int value)
{
	struct ifreq ifreq_io;
	pfq_t * mutable = (pfq_t *)q;

	memset(&ifreq_io, 0, sizeof(struct ifreq));
	strncpy(ifreq_io.ifr_name, dev, IFNAMSIZ);

	if(ioctl(q->fd, SIOCGIFFLAGS, &ifreq_io) == -1) { 
		return mutable->error = "PFQ: ioctl SIOCGIFFLAGS error", -1;
	}

	if (value)
		ifreq_io.ifr_flags |= IFF_PROMISC;
	else 
		ifreq_io.ifr_flags &= ~IFF_PROMISC;

	if(ioctl(q->fd, SIOCSIFFLAGS, &ifreq_io) == -1) {
		return mutable->error = "PFQ: ioctl SIOCSIFFLAGS error", -1;
	}
	return mutable->error = NULL, 0;
}


int 
pfq_set_caplen(pfq_t *q, size_t value)
{
	int enabled = pfq_is_enabled(q);
	if (enabled == 1) {
		return q->error =  "PFQ: enabled (caplen could not be set)", -1;
	}

	if (setsockopt(q->fd, PF_Q, Q_SO_SET_CAPLEN, &value, sizeof(value)) == -1) {
		return q->error = "PFQ: SET_CAPLEN error", -1;
	}

	q->slot_size = ALIGN8(sizeof(struct pfq_hdr)+ value);
	return q->error = NULL, 0;
}


ssize_t
pfq_get_caplen(pfq_t const *q)
{
	size_t ret; socklen_t size = sizeof(ret);
	pfq_t * mutable = (pfq_t *)q;
	
	if (getsockopt(q->fd, PF_Q, Q_SO_GET_CAPLEN, &ret, &size) == -1) {
		return mutable->error = "PFQ: GET_CAPLEN error", -1;
	}
	return mutable->error = NULL, (ssize_t)ret;
}


int
pfq_set_offset(pfq_t *q, size_t value)
{
	int enabled = pfq_is_enabled(q);
	if (enabled == 1) {
		return q->error =  "PFQ: enabled (offset could not be set)", -1;
	}

	if (setsockopt(q->fd, PF_Q, Q_SO_SET_OFFSET, &value, sizeof(value)) == -1) {
		return q->error = "PFQ: SET_OFFSET error", -1;
	}
	return q->error = NULL, 0;
}


ssize_t
pfq_get_offset(pfq_t const *q)
{
	pfq_t * mutable = (pfq_t *)q;
	size_t ret; socklen_t size = sizeof(ret);
	
	if (getsockopt(q->fd, PF_Q, Q_SO_GET_OFFSET, &ret, &size) == -1) {
		return mutable->error = "PFQ: GET_OFFSET error", -1;
	}
	return mutable->error = NULL, (ssize_t)ret;
}


int
pfq_set_slots(pfq_t *q, size_t value) 
{             
	int enabled = pfq_is_enabled(q);
	if (enabled == 1) {
		return q->error =  "PFQ: enabled (slots could not be set)", -1;
	}
	if (setsockopt(q->fd, PF_Q, Q_SO_SET_SLOTS, &value, sizeof(value)) == -1) {
		return q->error = "PFQ: SET_SLOTS error", -1;
	}

	q->queue_slots = value;
	return q->error = NULL, 0;
}

size_t
pfq_get_slots(pfq_t const *q) 
{   
	return q->queue_slots;
}


size_t
pfq_get_slot_size(pfq_t const *q) 
{   
	return q->slot_size;
}


int
pfq_bind_group(pfq_t *q, int gid, const char *dev, int queue)
{
	int index = pfq_ifindex(q, dev);
	if (index == -1) {
		return q->error = "PFQ: device not found", -1;
	}

	struct pfq_binding b = { gid, index, queue };
	if (setsockopt(q->fd, PF_Q, Q_SO_ADD_BINDING, &b, sizeof(b)) == -1) {
		return q->error = "PFQ: ADD_BINDING error", -1;
	}
	return q->error = NULL, 0;
}


int 
pfq_bind(pfq_t *q, const char *dev, int queue)
{
	int gid = q->gid;
	if (gid < 0) {
		return q->error = "PFQ: default group undefined", -1;
	}
	return pfq_bind_group(q, gid, dev, queue);
}                              


int
pfq_unbind_group(pfq_t *q, int gid, const char *dev, int queue) /* Q_ANY_QUEUE */
{
	int index = pfq_ifindex(q, dev);
	if (index == -1) {
		return q->error = "PFQ: device not found", -1;
	}
	struct pfq_binding b = { gid, index, queue };
	if (setsockopt(q->fd, PF_Q, Q_SO_REMOVE_BINDING, &b, sizeof(b)) == -1) {
		return q->error = "PFQ: REMOVE_BINDING error", -1;
	}
	return q->error = NULL, 0;
}


int
pfq_unbind(pfq_t *q, const char *dev, int queue)
{
	int gid = q->gid;
	if (gid < 0) {
		return q->error = "PFQ: default group undefined", -1;
	}
	return pfq_unbind_group(q, gid, dev, queue);
}  


int 
pfq_groups_mask(pfq_t const *q, unsigned long *_mask) 
{
	unsigned long mask; socklen_t size = sizeof(mask);
	pfq_t * mutable = (pfq_t *)q;
	
	if (getsockopt(q->fd, PF_Q, Q_SO_GET_GROUPS, &mask, &size) == -1) {
		return mutable->error = "PFQ: GET_GROUPS error", -1;
	}
	*_mask = mask;
	return mutable->error = NULL, 0;
}


int
pfq_steering_function(pfq_t *q, int gid, const char *fun_name)
{
	struct pfq_steering s = { fun_name, gid };
	if (setsockopt(q->fd, PF_Q, Q_SO_GROUP_STEER_FUN, &s, sizeof(s)) == -1) {
		return q->error = "PFQ: GROUP_STEER_FUN error", -1;
	}
	return q->error = NULL, 0;
}


int
pfq_group_state(pfq_t *q, int gid, const void *state, size_t size)
{
	struct pfq_group_state s  = { state, size, gid };
	if (setsockopt(q->fd, PF_Q, Q_SO_GROUP_STATE, &s, sizeof(s)) == -1) {
		return q->error = "PFQ: GROUP_STATE error", -1;
	}
	return q->error = NULL, 0;
}


int
pfq_join_group(pfq_t *q, int gid, unsigned long class_mask, int group_policy)
{
	if (group_policy == Q_GROUP_UNDEFINED) {
         	return q->error = "PFQ: join with undefined policy!", -1;
	}

	struct pfq_group_join group = { gid, group_policy, class_mask };

	socklen_t size = sizeof(group);
	if (getsockopt(q->fd, PF_Q, Q_SO_GROUP_JOIN, &group, &size) == -1) {
	        return q->error = "PFQ: GROUP_JOIN error", -1;
	}
	return q->error = NULL, group.gid;
}


int
pfq_leave_group(pfq_t *q, int gid)                  
{
	if (setsockopt(q->fd, PF_Q, Q_SO_GROUP_LEAVE, &gid, sizeof(gid)) == -1) {
	        return q->error = "PFQ: GROUP_LEAVE error", -1;
	}
	return q->error = NULL, 0;
}

        
int 
pfq_poll(pfq_t *q, long int microseconds /* = -1 -> infinite */)
{
	if (q->fd == -1) {
		return q->error = "PFQ: not open", -1;
	}

	struct pollfd fd = {q->fd, POLLIN, 0 };
	struct timespec timeout = { microseconds/1000000, (microseconds%1000000) * 1000};
	
	int ret = ppoll(&fd, 1, microseconds < 0 ? NULL : &timeout, NULL);
	if (ret < 0 &&
	    	errno != EINTR) {
	    return q->error = "PFQ: ppoll error", -1;
	}
	return q->error = NULL, 0; 
}


int
pfq_get_stats(pfq_t const *q, struct pfq_stats *stats) 
{
	pfq_t *mutable = (pfq_t *)q;
	socklen_t size = sizeof(struct pfq_stats);
	if (getsockopt(q->fd, PF_Q, Q_SO_GET_STATS, stats, &size) == -1) {
		return mutable->error = "PFQ: GET_STATS error", -1;
	}
	return mutable->error = NULL, 0;
}


int 
pfq_get_group_stats(pfq_t const *q, int gid, struct pfq_stats *stats) 
{
	pfq_t *mutable = (pfq_t *)q;
	socklen_t size = sizeof(struct pfq_stats);
	
	stats->recv = (unsigned int)gid;
	if (getsockopt(q->fd, PF_Q, Q_SO_GET_GROUP_STATS, stats, &size) == -1) {
		return mutable->error = "PFQ: GET_GROUP_STATS error", -1;
	}
	return mutable->error = NULL, 0;
}


int
pfq_read(pfq_t *q, struct pfq_net_queue *nq, long int microseconds) 
{
	size_t q_size = q->queue_slots * q->slot_size;
	struct pfq_queue_descr * qd;
	unsigned int index, data;

        if (q->queue_addr == NULL) {
         	return q->error = "PFQ: read on pfq socket not enabled", -1;
	}

	qd = (struct pfq_queue_descr *)(q->queue_addr);
	data   = qd->data;
	index  = DBMP_QUEUE_INDEX(data);

	/*  watermark for polling... */

	if( DBMP_QUEUE_LEN(data) < (q->queue_slots >> 1) ) {
		if (pfq_poll(q, microseconds) < 0)
		{
			return -1;
		}
	}

	/* reset the next buffer... */

	data = __sync_lock_test_and_set(&qd->data, ((index+1) << 24));

	size_t queue_len = min(DBMP_QUEUE_LEN(data), q->queue_slots);

	nq->queue = (char *)(q->queue_addr) + 
			    sizeof(struct pfq_queue_descr) + 
			    (index & 1) * q_size;
	nq->index = index;
	nq->len   = queue_len;
        nq->slot_size = q->slot_size; 

	return q->error = NULL, (int)queue_len;
}


int 
pfq_recv(pfq_t *q, void *buf, size_t buflen, struct pfq_net_queue *nq, long int microseconds)
{
       	if (pfq_read(q, nq, microseconds) < 0)
		return -1;

	if (buflen < (q->queue_slots * q->slot_size)) {
		return q->error = "PFQ: buffer too small", -1;
	}

	memcpy(buf, nq->queue, q->slot_size * nq->len);
	return q->error = NULL, 0;
}


int
pfq_dispatch(pfq_t *q, pfq_handler_t cb, long int microseconds, char *user, int max_packet)
{
	int n = 0;

	if (q->it == pfq_net_queue_end(&q->netq))
	{
		if (pfq_read(q, &q->netq, microseconds) < 0)
		{
			return -1;
		}
	
		q->it = pfq_net_queue_begin(&q->netq);
	}

	pfq_iterator_t it_end = pfq_net_queue_end(&q->netq);
	
	printf("Read a queue is empty? %d\n", q->it == it_end);
	
	for(; q->it != it_end; q->it = pfq_net_queue_next(&q->netq, q->it))
	{
		while (!pfq_iterator_ready(&q->netq, q->it))
			pfq_yield();

		cb(pfq_iterator_header(q->it), pfq_iterator_data(q->it), user);
		n++;

		if (max_packet > 0 && (n == max_packet)) {
		       	q->it = pfq_net_queue_next(&q->netq, q->it);
			break;
		}
	}
        return n;
}


size_t
pfq_mem_size(pfq_t const *q) 
{
	return q->queue_tot_mem;
}


const void *
pfq_mem_addr(pfq_t const *q) 
{
	return q->queue_addr;
}


int
pfq_id(pfq_t *q)
{
 	return q->id;
}


int
pfq_group_id(pfq_t *q)
{
 	return q->gid;
}


int pfq_get_fd(pfq_t *q)
{
	return q->fd;
}
