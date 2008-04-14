/*
 * Copyright (c) 2006 Paolo Abeni (Italy)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote 
 * products derived from this software without specific prior written 
 * permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Bluetooth sniffing API implementation for Linux platform
 * By Paolo Abeni <paolo.abeni@email.it>
 *
 */
#ifndef lint
static const char rcsid[] _U_ =
    "@(#) $Header: /tcpdump/master/libpcap/pcap-bt-linux.c,v 1.9.2.4 2008-04-14 20:41:51 guy Exp $ (LBL)";
#endif
 
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pcap-int.h"
#include "pcap-bt-linux.h"
#include "pcap/bluetooth.h"

#ifdef NEED_STRERROR_H
#include "strerror.h"
#endif

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>

#define BT_IFACE "bluetooth"
#define BT_CTRL_SIZE 128

/* forward declaration */
static int bt_activate(pcap_t *);
static int bt_read_linux(pcap_t *, int , pcap_handler , u_char *);
static int bt_inject_linux(pcap_t *, const void *, size_t);
static int bt_setfilter_linux(pcap_t *, struct bpf_program *);
static int bt_setdirection_linux(pcap_t *, pcap_direction_t);
static int bt_stats_linux(pcap_t *, struct pcap_stat *);
static void bt_close_linux(pcap_t *);

int 
bt_platform_finddevs(pcap_if_t **alldevsp, char *err_str)
{
	pcap_if_t *found_dev = *alldevsp;
	struct hci_dev_list_req *dev_list;
	struct hci_dev_req *dev_req;
	int i, sock;
	int ret = 0;
	
	sock  = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
	if (sock < 0)
	{
		/* if bluetooth is not supported this this is not fatal*/ 
		if (errno == EAFNOSUPPORT)
			return 0;
		snprintf(err_str, PCAP_ERRBUF_SIZE, "Can't open raw Bluetooth socket %d:%s",
			errno, strerror(errno));
		return -1;
	}

	dev_list = malloc(HCI_MAX_DEV * sizeof(*dev_req) + sizeof(*dev_list));
	if (!dev_list) 
	{
		snprintf(err_str, PCAP_ERRBUF_SIZE, "Can't allocate %zu bytes for Bluetooth device list",
			HCI_MAX_DEV * sizeof(*dev_req) + sizeof(*dev_list));
		ret = -1;
		goto done;
	}

	dev_list->dev_num = HCI_MAX_DEV;

	if (ioctl(sock, HCIGETDEVLIST, (void *) dev_list) < 0) 
	{
		snprintf(err_str, PCAP_ERRBUF_SIZE, "Can't get Bluetooth device list via ioctl %d:%s",
			errno, strerror(errno));
		ret = -1;
		goto free;
	}

	dev_req = dev_list->dev_req;
	for (i = 0; i < dev_list->dev_num; i++, dev_req++) {
		char dev_name[20], dev_descr[30];
		
		snprintf(dev_name, 20, BT_IFACE"%d", dev_req->dev_id);
		snprintf(dev_descr, 30, "Bluetooth adapter number %d", i);
			
		if (pcap_add_if(&found_dev, dev_name, 0, 
		       dev_descr, err_str) < 0)
		{
			ret = -1;
			break;
		}

	}

free:
	free(dev_list);

done:
	close(sock);
	return ret;
}

pcap_t *
bt_create(const char *device, char *ebuf)
{
	pcap_t *p;

	p = pcap_create_common(device, ebuf);
	if (p == NULL)
		return (NULL);

	p->activate_op = bt_activate;
	return (p);
}

static int
bt_activate(pcap_t* handle)
{
	struct sockaddr_hci addr;
	int opt;
	int		dev_id;
	struct hci_filter	flt;
	int err = PCAP_ERROR;

	/* get bt interface id */
	if (sscanf(handle->opt.source, BT_IFACE"%d", &dev_id) != 1)
	{
		snprintf(handle->errbuf, PCAP_ERRBUF_SIZE,
			"Can't get Bluetooth device index from %s", 
			 handle->opt.source);
		return PCAP_ERROR;
	}

	/* Initialize some components of the pcap structure. */
	handle->bufsize = handle->snapshot+BT_CTRL_SIZE+sizeof(pcap_bluetooth_h4_header);
	handle->offset = BT_CTRL_SIZE;
	handle->linktype = DLT_BLUETOOTH_HCI_H4_WITH_PHDR;

	handle->read_op = bt_read_linux;
	handle->inject_op = bt_inject_linux;
	handle->setfilter_op = bt_setfilter_linux;
	handle->setdirection_op = bt_setdirection_linux;
	handle->set_datalink_op = NULL;	/* can't change data link type */
	handle->getnonblock_op = pcap_getnonblock_fd;
	handle->setnonblock_op = pcap_setnonblock_fd;
	handle->stats_op = bt_stats_linux;
	handle->md.ifindex = dev_id;
	
	/* Create HCI socket */
	handle->fd = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
	if (handle->fd < 0) {
		snprintf(handle->errbuf, PCAP_ERRBUF_SIZE, "Can't create raw socket %d:%s",
			errno, strerror(errno));
		return PCAP_ERROR;
	}

	handle->buffer = malloc(handle->bufsize);
	if (!handle->buffer) {
		snprintf(handle->errbuf, PCAP_ERRBUF_SIZE, "Can't allocate dump buffer: %s",
			pcap_strerror(errno));
		goto close_fail;
	}

	opt = 1;
	if (setsockopt(handle->fd, SOL_HCI, HCI_DATA_DIR, &opt, sizeof(opt)) < 0) {
		snprintf(handle->errbuf, PCAP_ERRBUF_SIZE, "Can't enable data direction info %d:%s",
			errno, strerror(errno));
		goto close_fail;
	}

	opt = 1;
	if (setsockopt(handle->fd, SOL_HCI, HCI_TIME_STAMP, &opt, sizeof(opt)) < 0) {
		snprintf(handle->errbuf, PCAP_ERRBUF_SIZE, "Can't enable time stamp %d:%s",
			errno, strerror(errno));
		goto close_fail;
	}

	/* Setup filter, do not call hci function to avoid dependence on 
	 * external libs	*/
	memset(&flt, 0, sizeof(flt));
	memset((void *) &flt.type_mask, 0xff, sizeof(flt.type_mask));	
	memset((void *) &flt.event_mask, 0xff, sizeof(flt.event_mask));
	if (setsockopt(handle->fd, SOL_HCI, HCI_FILTER, &flt, sizeof(flt)) < 0) {
		snprintf(handle->errbuf, PCAP_ERRBUF_SIZE, "Can't set filter %d:%s",
			errno, strerror(errno));
		goto close_fail;
	}


	/* Bind socket to the HCI device */
	addr.hci_family = AF_BLUETOOTH;
	addr.hci_dev = handle->md.ifindex;
	if (bind(handle->fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		snprintf(handle->errbuf, PCAP_ERRBUF_SIZE, "Can't attach to device %d %d:%s",
			handle->md.ifindex, errno, strerror(errno));
		goto close_fail;
	}

	if (p->opt.rfmon) {
		/*
		 * Monitor mode doesn't apply to Bluetooth devices.
		 */
		err = PCAP_ERROR_RFMON_NOTSUP;
		goto close_fail;
	}

	if (handle->opt.buffer_size == 0) {
		/*
		 * Set the socket buffer size to the specified value.
		 */
		if (setsockopt(handle->fd, SOL_SOCKET, SO_RCVBUF,
		    &handle->opt.buffer_size,
		    sizeof(handle->opt.buffer_size)) == -1) {
			snprintf(handle->errbuf, PCAP_ERRBUF_SIZE,
				 "SO_RCVBUF: %s", pcap_strerror(errno));
			goto close_fail;
		}
	}

	handle->selectable_fd = handle->fd;
	return 0;

close_fail:
	pcap_cleanup_live_common(p);
	return err;
}

static int
bt_read_linux(pcap_t *handle, int max_packets, pcap_handler callback, u_char *user)
{
	struct cmsghdr *cmsg;
	struct msghdr msg;
	struct iovec  iv;
	struct pcap_pkthdr pkth;
	pcap_bluetooth_h4_header* bthdr;

	bthdr = (pcap_bluetooth_h4_header*) &handle->buffer[handle->offset];
	iv.iov_base = &handle->buffer[handle->offset+sizeof(pcap_bluetooth_h4_header)];
	iv.iov_len  = handle->snapshot;
	
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iv;
	msg.msg_iovlen = 1;
	msg.msg_control = handle->buffer;
	msg.msg_controllen = handle->offset;

	/* ignore interrupt system call error */
	do {
		pkth.caplen = recvmsg(handle->fd, &msg, 0);
		if (handle->break_loop)
		{
			handle->break_loop = 0;
			return -2;
		}
	} while ((pkth.caplen == -1) && (errno == EINTR));

		
	if (pkth.caplen < 0) {
		snprintf(handle->errbuf, PCAP_ERRBUF_SIZE, "Can't receive packet %d:%s",
			errno, strerror(errno));
		return -1;
	}

	/* get direction and timestamp*/ 
	cmsg = CMSG_FIRSTHDR(&msg);
	int in=0;
	while (cmsg) {
		switch (cmsg->cmsg_type) {
			case HCI_CMSG_DIR:
				in = *((int *) CMSG_DATA(cmsg));
				break;
                      	case HCI_CMSG_TSTAMP:
				pkth.ts = *((struct timeval *) CMSG_DATA(cmsg));
				break;
		}
		cmsg = CMSG_NXTHDR(&msg, cmsg);
	}
	if ((in && (handle->direction == PCAP_D_OUT)) || 
				((!in) && (handle->direction == PCAP_D_IN)))
		return 0;

	bthdr->direction = htonl(in != 0);
	pkth.caplen+=sizeof(pcap_bluetooth_h4_header);
	pkth.len = pkth.caplen;
	callback(user, &pkth, &handle->buffer[handle->offset]);
	return 1;
}

static int
bt_inject_linux(pcap_t *handle, const void *buf, size_t size)
{
	snprintf(handle->errbuf, PCAP_ERRBUF_SIZE, "inject not supported on "
    		"bluetooth devices");
	return (-1);
}                           


static int 
bt_stats_linux(pcap_t *handle, struct pcap_stat *stats)
{
	int ret;
	struct hci_dev_info dev_info;
	struct hci_dev_stats * s = &dev_info.stat;
	dev_info.dev_id = handle->md.ifindex;
	
	/* ingnore eintr */
	do {
		ret = ioctl(handle->fd, HCIGETDEVINFO, (void *)&dev_info);
	} while ((ret == -1) && (errno == EINTR));
	    
	if (ret < 0) {
		snprintf(handle->errbuf, PCAP_ERRBUF_SIZE, "can get stats"
			" via ioctl %d:%s", errno, strerror(errno));
		return (-1);
		
	}

	/* we receive both rx and tx frames, so comulate all stats */	
	stats->ps_recv = s->evt_rx + s->acl_rx + s->sco_rx + s->cmd_tx + 
		s->acl_tx +s->sco_tx;
	stats->ps_drop = s->err_rx + s->err_tx;
	stats->ps_ifdrop = 0;
	return 0;
}

static int 
bt_setfilter_linux(pcap_t *p, struct bpf_program *fp)
{
	return 0;
}


static int 
bt_setdirection_linux(pcap_t *p, pcap_direction_t d)
{
	p->direction = d;
	return 0;
}
