/*
 * Copyright (c) 2004 Damien Miller <djm@mindrot.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <signal.h>
#include <syslog.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <poll.h>

#include "sys-queue.h"
#include "sys-tree.h"
#include "flowd.h"
#include "privsep.h"
#include "netflow.h"
#include "store.h"
#include "atomicio.h"
#include "peer.h"

RCSID("$Id$");

/* Dump unknown packet types */
/* #define DEBUG_UNKNOWN */

/* Reams of netflow v.9 verbosity */
/* #define DEBUG_NF9 */

/* Prototype this (can't make it static because it only #ifdef DEBUG_UNKNOWN) */
void dump_packet(const char *tag, const u_int8_t *p, int len);

/* Flags set by signal handlers */
static sig_atomic_t exit_flag = 0;
static sig_atomic_t reconf_flag = 0;
static sig_atomic_t reopen_flag = 0;
static sig_atomic_t info_flag = 0;

/* Signal handlers */
static void
sighand_exit(int signo)
{
	exit_flag = signo;
	signal(signo, sighand_exit);
}

static void
sighand_reconf(int signo)
{
	reconf_flag = 1;
	reopen_flag = 1;
	signal(signo, sighand_reconf);
}

static void
sighand_reopen(int signo)
{
	reopen_flag = 1;
	signal(signo, sighand_reopen);
}

static void
sighand_info(int signo)
{
	info_flag = 1;
	signal(signo, sighand_info);
}

/* Format data to a hex string */
static const char *
data_ntoa(const u_int8_t *p, int len)
{
	static char buf[2048];
	char tmp[3];
	int i;

	for (*buf = '\0', i = 0; i < len; i++) {
		snprintf(tmp, sizeof(tmp), "%02x%s", p[i], i % 2 ? " " : "");
		if (strlcat(buf, tmp, sizeof(buf) - 4) >= sizeof(buf) - 4) {
			strlcat(buf, "...", sizeof(buf));
			break;
		}
	}
	return (buf);
}

/* Dump a packet */
void
dump_packet(const char *tag, const u_int8_t *p, int len)
{
	if (tag == NULL)
		logit(LOG_INFO, "packet len %d: %s", len, data_ntoa(p, len));
	else {
		logit(LOG_INFO, "%s: packet len %d: %s",
		    tag, len, data_ntoa(p, len));
	}
}

static int
start_log(int monitor_fd)
{
	int fd;
	off_t pos;
	char ebuf[512];

	if ((fd = client_open_log(monitor_fd)) == -1)
		logerrx("Logfile open failed, exiting");

	/* Only write out the header if we are at the start of the file */
	switch ((pos = lseek(fd, 0, SEEK_END))) {
	case 0:
		/* New file, continue below */
		break;
	case -1:
		logerr("%s: llseek error, exiting", __func__);
	default:
		/* Logfile exists, don't write new header */
		if (lseek(fd, 0, SEEK_SET) != 0)
			logerr("%s: llseek error, exiting", __func__);
		if (store_check_header(fd, ebuf, sizeof(ebuf)) != 0)
			logerrx("%s: Exiting on %s", __func__, ebuf);
		if (lseek(fd, 0, SEEK_END) <= 0)
			logerr("%s: llseek error, exiting", __func__);
		logit(LOG_DEBUG, "Continuing with existing logfile len %lld",
		    (long long)pos);
		return (fd);
	}

	logit(LOG_DEBUG, "Writing new logfile header");

	if (store_put_header(fd, ebuf, sizeof(ebuf)) != 0)
		logerrx("%s: Exiting on %s", __func__, ebuf);

	return (fd);
}

static void
process_flow(struct store_flow_complete *flow, struct flowd_config *conf,
    int log_fd)
{
	char ebuf[512];

	/* Another sanity check */
	if (flow->src_addr.af != flow->dst_addr.af) {
		logit(LOG_WARNING, "%s: flow src(%d)/dst(%d) AF mismatch",
		    __func__, flow->src_addr.af, flow->dst_addr.af);
		return;
	}

	/* Prepare for writing */
	flow->hdr.fields = htonl(flow->hdr.fields);

	flow->recv_time.recv_secs = htonl(flow->recv_time.recv_secs);

	if (conf->opts & FLOWD_OPT_VERBOSE) {
		char fbuf[1024];

		store_format_flow(flow, fbuf, sizeof(fbuf), 0,
		    STORE_DISPLAY_ALL);
		logit(LOG_DEBUG, "%s: flow %s", __func__, fbuf);
	}

	if (filter_flow(flow, &conf->filter_list) == FF_ACTION_DISCARD)
		return;

	if (store_put_flow(log_fd, flow, conf->store_mask, ebuf,
	    sizeof(ebuf)) != 0)
		logerrx("%s: exiting on %s", __func__, ebuf);

	/* XXX reopen log file on one failure, exit on multiple */
}

static void
process_netflow_v1(u_int8_t *pkt, size_t len, struct xaddr *flow_source,
    struct flowd_config *conf, struct peer_state *peer, struct peers *peers,
    int log_fd)
{
	struct NF1_HEADER *nf1_hdr = (struct NF1_HEADER *)pkt;
	struct NF1_FLOW *nf1_flow;
	struct store_flow_complete flow;
	size_t offset;
	u_int i, nflows;

	if (len < sizeof(*nf1_hdr)) {
		peer->ninvalid++;
		logit(LOG_WARNING, "short netflow v.1 packet %d bytes from %s",
		    len, addr_ntop_buf(flow_source));
		return;
	}
	nflows = ntohs(nf1_hdr->c.flows);
	if (nflows == 0 || nflows > NF1_MAXFLOWS) {
		peer->ninvalid++;
		logit(LOG_WARNING, "Invalid number of flows (%u) in netflow "
		    "v.1 packet from %s", nflows, addr_ntop_buf(flow_source));
		return;
	}
	if (len != NF1_PACKET_SIZE(nflows)) {
		peer->ninvalid++;
		logit(LOG_WARNING, "Inconsistent Netflow v.1 packet from %s: "
		    "len %u expected %u", addr_ntop_buf(flow_source), len,
		    NF1_PACKET_SIZE(nflows));
		return;
	}

	logit(LOG_DEBUG, "Valid netflow v.1 packet %d flows", nflows);
	update_peer(peers, peer, nflows, 1);

	for (i = 0; i < nflows; i++) {
		offset = NF1_PACKET_SIZE(i);
		nf1_flow = (struct NF1_FLOW *)(pkt + offset);

		bzero(&flow, sizeof(flow));

		/* NB. These are converted to network byte order later */
		flow.hdr.fields = STORE_FIELD_ALL;
		/* flow.hdr.tag is set later */
		flow.hdr.fields &= ~STORE_FIELD_TAG;
		flow.hdr.fields &= ~STORE_FIELD_SRC_ADDR6;
		flow.hdr.fields &= ~STORE_FIELD_DST_ADDR6;
		flow.hdr.fields &= ~STORE_FIELD_GATEWAY_ADDR6;
		flow.hdr.fields &= ~STORE_FIELD_AS_INFO;
		flow.hdr.fields &= ~STORE_FIELD_FLOW_ENGINE_INFO;

		flow.recv_time.recv_secs = time(NULL);

		flow.pft.tcp_flags = nf1_flow->tcp_flags;
		flow.pft.protocol = nf1_flow->protocol;
		flow.pft.tos = nf1_flow->tos;

		memcpy(&flow.agent_addr, flow_source, sizeof(flow.agent_addr));

		flow.src_addr.v4.s_addr = nf1_flow->src_ip;
		flow.src_addr.af = AF_INET;
		flow.dst_addr.v4.s_addr = nf1_flow->dest_ip;
		flow.dst_addr.af = AF_INET;
		flow.gateway_addr.v4.s_addr = nf1_flow->nexthop_ip;
		flow.gateway_addr.af = AF_INET;

		flow.ports.src_port = nf1_flow->src_port;
		flow.ports.dst_port = nf1_flow->dest_port;

#define NTO64(a) (store_htonll(ntohl(a)))
		flow.octets.flow_octets = NTO64(nf1_flow->flow_octets);
		flow.packets.flow_packets = NTO64(nf1_flow->flow_packets);
#undef NTO64

		flow.ifndx.if_index_in = nf1_flow->if_index_in;
		flow.ifndx.if_index_out = nf1_flow->if_index_out;

		flow.ainfo.sys_uptime_ms = nf1_hdr->uptime_ms;
		flow.ainfo.time_sec = nf1_hdr->time_sec;
		flow.ainfo.time_nanosec = nf1_hdr->time_nanosec;
		flow.ainfo.netflow_version = nf1_hdr->c.version;

		flow.ftimes.flow_start = nf1_flow->flow_start;
		flow.ftimes.flow_finish = nf1_flow->flow_finish;

		process_flow(&flow, conf, log_fd);
	}
}

static void
process_netflow_v5(u_int8_t *pkt, size_t len, struct xaddr *flow_source,
    struct flowd_config *conf, struct peer_state *peer, struct peers *peers,
    int log_fd)
{
	struct NF5_HEADER *nf5_hdr = (struct NF5_HEADER *)pkt;
	struct NF5_FLOW *nf5_flow;
	struct store_flow_complete flow;
	size_t offset;
	u_int i, nflows;

	if (len < sizeof(*nf5_hdr)) {
		peer->ninvalid++;
		logit(LOG_WARNING, "short netflow v.5 packet %d bytes from %s",
		    len, addr_ntop_buf(flow_source));
		return;
	}
	nflows = ntohs(nf5_hdr->c.flows);
	if (nflows == 0 || nflows > NF5_MAXFLOWS) {
		peer->ninvalid++;
		logit(LOG_WARNING, "Invalid number of flows (%u) in netflow "
		    "v.5 packet from %s", nflows, addr_ntop_buf(flow_source));
		return;
	}
	if (len != NF5_PACKET_SIZE(nflows)) {
		peer->ninvalid++;
		logit(LOG_WARNING, "Inconsistent Netflow v.5 packet from %s: "
		    "len %u expected %u", addr_ntop_buf(flow_source), len,
		    NF5_PACKET_SIZE(nflows));
		return;
	}

	logit(LOG_DEBUG, "Valid netflow v.5 packet %d flows", nflows);
	update_peer(peers, peer, nflows, 5);

	for (i = 0; i < nflows; i++) {
		offset = NF5_PACKET_SIZE(i);
		nf5_flow = (struct NF5_FLOW *)(pkt + offset);

		bzero(&flow, sizeof(flow));

		/* NB. These are converted to network byte order later */
		flow.hdr.fields = STORE_FIELD_ALL;
		/* flow.hdr.tag is set later */
		flow.hdr.fields &= ~STORE_FIELD_TAG;
		flow.hdr.fields &= ~STORE_FIELD_SRC_ADDR6;
		flow.hdr.fields &= ~STORE_FIELD_DST_ADDR6;
		flow.hdr.fields &= ~STORE_FIELD_GATEWAY_ADDR6;

		flow.recv_time.recv_secs = time(NULL);

		flow.pft.tcp_flags = nf5_flow->tcp_flags;
		flow.pft.protocol = nf5_flow->protocol;
		flow.pft.tos = nf5_flow->tos;

		memcpy(&flow.agent_addr, flow_source, sizeof(flow.agent_addr));

		flow.src_addr.v4.s_addr = nf5_flow->src_ip;
		flow.src_addr.af = AF_INET;
		flow.dst_addr.v4.s_addr = nf5_flow->dest_ip;
		flow.dst_addr.af = AF_INET;
		flow.gateway_addr.v4.s_addr = nf5_flow->nexthop_ip;
		flow.gateway_addr.af = AF_INET;

		flow.ports.src_port = nf5_flow->src_port;
		flow.ports.dst_port = nf5_flow->dest_port;

#define NTO64(a) (store_htonll(ntohl(a)))
		flow.octets.flow_octets = NTO64(nf5_flow->flow_octets);
		flow.packets.flow_packets = NTO64(nf5_flow->flow_packets);
#undef NTO64

		flow.ifndx.if_index_in = nf5_flow->if_index_in;
		flow.ifndx.if_index_out = nf5_flow->if_index_out;

		flow.ainfo.sys_uptime_ms = nf5_hdr->uptime_ms;
		flow.ainfo.time_sec = nf5_hdr->time_sec;
		flow.ainfo.time_nanosec = nf5_hdr->time_nanosec;
		flow.ainfo.netflow_version = nf5_hdr->c.version;

		flow.ftimes.flow_start = nf5_flow->flow_start;
		flow.ftimes.flow_finish = nf5_flow->flow_finish;

		flow.asinf.src_as = nf5_flow->src_as;
		flow.asinf.dst_as = nf5_flow->dest_as;
		flow.asinf.src_mask = nf5_flow->src_mask;
		flow.asinf.dst_mask = nf5_flow->dst_mask;

		flow.finf.engine_type = nf5_hdr->engine_type;
		flow.finf.engine_id = nf5_hdr->engine_id;
		flow.finf.flow_sequence = nf5_hdr->flow_sequence;

		process_flow(&flow, conf, log_fd);
	}
}

static void
process_netflow_v7(u_int8_t *pkt, size_t len, struct xaddr *flow_source,
    struct flowd_config *conf, struct peer_state *peer, struct peers *peers,
    int log_fd)
{
	struct NF7_HEADER *nf7_hdr = (struct NF7_HEADER *)pkt;
	struct NF7_FLOW *nf7_flow;
	struct store_flow_complete flow;
	size_t offset;
	u_int i, nflows;

	if (len < sizeof(*nf7_hdr)) {
		peer->ninvalid++;
		logit(LOG_WARNING, "short netflow v.7 packet %d bytes from %s",
		    len, addr_ntop_buf(flow_source));
		return;
	}
	nflows = ntohs(nf7_hdr->c.flows);
	if (nflows == 0 || nflows > NF7_MAXFLOWS) {
		peer->ninvalid++;
		logit(LOG_WARNING, "Invalid number of flows (%u) in netflow "
		    "v.7 packet from %s", nflows, addr_ntop_buf(flow_source));
		return;
	}
	if (len != NF7_PACKET_SIZE(nflows)) {
		peer->ninvalid++;
		logit(LOG_WARNING, "Inconsistent Netflow v.7 packet from %s: "
		    "len %u expected %u", addr_ntop_buf(flow_source), len,
		    NF7_PACKET_SIZE(nflows));
		return;
	}

	logit(LOG_DEBUG, "Valid netflow v.7 packet %d flows", nflows);
	update_peer(peers, peer, nflows, 7);

	for (i = 0; i < nflows; i++) {
		offset = NF7_PACKET_SIZE(i);
		nf7_flow = (struct NF7_FLOW *)(pkt + offset);

		bzero(&flow, sizeof(flow));

		/* NB. These are converted to network byte order later */
		flow.hdr.fields = STORE_FIELD_ALL;
		/* flow.hdr.tag is set later */
		flow.hdr.fields &= ~STORE_FIELD_TAG;
		flow.hdr.fields &= ~STORE_FIELD_SRC_ADDR6;
		flow.hdr.fields &= ~STORE_FIELD_DST_ADDR6;
		flow.hdr.fields &= ~STORE_FIELD_GATEWAY_ADDR6;

		/*
		 * XXX: we can parse the (undocumented) flags1 and flags2
		 * fields of the packet to disable flow fields not set by
		 * the Cat5k (e.g. destination-only mls nde mode)
		 */

		flow.recv_time.recv_secs = time(NULL);

		flow.pft.tcp_flags = nf7_flow->tcp_flags;
		flow.pft.protocol = nf7_flow->protocol;
		flow.pft.tos = nf7_flow->tos;

		memcpy(&flow.agent_addr, flow_source, sizeof(flow.agent_addr));

		flow.src_addr.v4.s_addr = nf7_flow->src_ip;
		flow.src_addr.af = AF_INET;
		flow.dst_addr.v4.s_addr = nf7_flow->dest_ip;
		flow.dst_addr.af = AF_INET;
		flow.gateway_addr.v4.s_addr = nf7_flow->nexthop_ip;
		flow.gateway_addr.af = AF_INET;

		flow.ports.src_port = nf7_flow->src_port;
		flow.ports.dst_port = nf7_flow->dest_port;

#define NTO64(a) (store_htonll(ntohl(a)))
		flow.octets.flow_octets = NTO64(nf7_flow->flow_octets);
		flow.packets.flow_packets = NTO64(nf7_flow->flow_packets);
#undef NTO64

		flow.ifndx.if_index_in = nf7_flow->if_index_in;
		flow.ifndx.if_index_out = nf7_flow->if_index_out;

		flow.ainfo.sys_uptime_ms = nf7_hdr->uptime_ms;
		flow.ainfo.time_sec = nf7_hdr->time_sec;
		flow.ainfo.time_nanosec = nf7_hdr->time_nanosec;
		flow.ainfo.netflow_version = nf7_hdr->c.version;

		flow.ftimes.flow_start = nf7_flow->flow_start;
		flow.ftimes.flow_finish = nf7_flow->flow_finish;

		flow.asinf.src_as = nf7_flow->src_as;
		flow.asinf.dst_as = nf7_flow->dest_as;
		flow.asinf.src_mask = nf7_flow->src_mask;
		flow.asinf.dst_mask = nf7_flow->dst_mask;

		flow.finf.flow_sequence = nf7_hdr->flow_sequence;

		process_flow(&flow, conf, log_fd);
	}
}

static int
nf9_rec_to_flow(struct peer_nf9_record *rec, struct store_flow_complete *flow,
    u_int8_t *data)
{
/* Copy an int (possibly shorter than the target) keeping their LSBs aligned */
#define BE_COPY(a) memcpy((u_char*)&a + (sizeof(a) - rec->len), data, rec->len);
	/* XXX: use a table-based interpreter */
	switch (rec->type) {
	case NF9_IN_BYTES:
		flow->hdr.fields |= STORE_FIELD_OCTETS;
		BE_COPY(flow->octets.flow_octets);
		break;
	case NF9_IN_PACKETS:
		flow->hdr.fields |= STORE_FIELD_PACKETS;
		BE_COPY(flow->packets.flow_packets);
		break;
	case NF9_IN_PROTOCOL:
		flow->hdr.fields |= STORE_FIELD_PROTO_FLAGS_TOS;
		BE_COPY(flow->pft.protocol);
		break;
	case NF9_SRC_TOS:
		flow->hdr.fields |= STORE_FIELD_PROTO_FLAGS_TOS;
		BE_COPY(flow->pft.tos);
		break;
	case NF9_TCP_FLAGS:
		flow->hdr.fields |= STORE_FIELD_PROTO_FLAGS_TOS;
		BE_COPY(flow->pft.tcp_flags);
		break;
	case NF9_L4_SRC_PORT:
		flow->hdr.fields |= STORE_FIELD_SRCDST_PORT;
		BE_COPY(flow->ports.src_port);
		break;
	case NF9_IPV4_SRC_ADDR:
		flow->hdr.fields |= STORE_FIELD_SRC_ADDR4;
		memcpy(&flow->src_addr.v4, data, rec->len);
		flow->src_addr.af = AF_INET;
		break;
	case NF9_SRC_MASK:
		flow->hdr.fields |= STORE_FIELD_AS_INFO;
		BE_COPY(flow->asinf.src_mask);
		break;
	case NF9_INPUT_SNMP:
		flow->hdr.fields |= STORE_FIELD_IF_INDICES;
		BE_COPY(flow->ifndx.if_index_in);
		break;
	case NF9_L4_DST_PORT:
		flow->hdr.fields |= STORE_FIELD_SRCDST_PORT;
		BE_COPY(flow->ports.dst_port);
		break;
	case NF9_IPV4_DST_ADDR:
		flow->hdr.fields |= STORE_FIELD_DST_ADDR4;
		memcpy(&flow->dst_addr.v4, data, rec->len);
		flow->dst_addr.af = AF_INET;
		break;
	case NF9_DST_MASK:
		flow->hdr.fields |= STORE_FIELD_AS_INFO;
		BE_COPY(flow->asinf.dst_mask);
		break;
	case NF9_OUTPUT_SNMP:
		flow->hdr.fields |= STORE_FIELD_IF_INDICES;
		BE_COPY(flow->ifndx.if_index_out);
		break;
	case NF9_IPV4_NEXT_HOP:
		flow->hdr.fields |= STORE_FIELD_GATEWAY_ADDR4;
		memcpy(&flow->gateway_addr.v4, data, rec->len);
		flow->gateway_addr.af = AF_INET;
		break;
	case NF9_SRC_AS:
		flow->hdr.fields |= STORE_FIELD_AS_INFO;
		BE_COPY(flow->asinf.src_as);
		break;
	case NF9_DST_AS:
		flow->hdr.fields |= STORE_FIELD_AS_INFO;
		BE_COPY(flow->asinf.dst_as);
		break;
	case NF9_LAST_SWITCHED:
		flow->hdr.fields |= STORE_FIELD_FLOW_TIMES;
		BE_COPY(flow->ftimes.flow_start);
		break;
	case NF9_FIRST_SWITCHED:
		flow->hdr.fields |= STORE_FIELD_FLOW_TIMES;
		BE_COPY(flow->ftimes.flow_finish);
		break;
	case NF9_IPV6_SRC_ADDR:
		flow->hdr.fields |= STORE_FIELD_SRC_ADDR6;
		memcpy(&flow->src_addr.v6, data, rec->len);
		flow->src_addr.af = AF_INET6;
		break;
	case NF9_IPV6_DST_ADDR:
		flow->hdr.fields |= STORE_FIELD_DST_ADDR6;
		memcpy(&flow->dst_addr.v6, data, rec->len);
		flow->dst_addr.af = AF_INET6;
		break;
	case NF9_IPV6_SRC_MASK:
		flow->hdr.fields |= STORE_FIELD_AS_INFO;
		BE_COPY(flow->asinf.src_mask);
		break;
	case NF9_IPV6_DST_MASK:
		flow->hdr.fields |= STORE_FIELD_AS_INFO;
		BE_COPY(flow->asinf.dst_mask);
		break;
	case NF9_ENGINE_TYPE:
		flow->hdr.fields |= STORE_FIELD_FLOW_ENGINE_INFO;
		BE_COPY(flow->finf.engine_type);
		break;
	case NF9_ENGINE_ID:
		flow->hdr.fields |= STORE_FIELD_FLOW_ENGINE_INFO;
		BE_COPY(flow->finf.engine_id);
		break;
	case NF9_IPV6_NEXT_HOP:
		flow->hdr.fields |= STORE_FIELD_GATEWAY_ADDR6;
		memcpy(&flow->gateway_addr.v6, data, rec->len);
		flow->gateway_addr.af = AF_INET6;
		break;
	}
#undef BE_COPY
	return (0);
}

static int
nf9_check_rec_len(u_int type, u_int len)
{
	/* Sanity check */
	if (len == 0 || len > 0x4000)
		return (0);

	/* XXX: use a table-based interpreter */
	switch (type) {
	case NF9_IN_BYTES:
		return (len <= 8);
	case NF9_IN_PACKETS:
		return (len <= 8);
	case NF9_IN_PROTOCOL:
		return (len == 1);
	case NF9_SRC_TOS:
		return (len == 1);
	case NF9_TCP_FLAGS:
		return (len == 1);
	case NF9_L4_SRC_PORT:
		return (len == 2);
	case NF9_IPV4_SRC_ADDR:
		return (len == 4);
	case NF9_SRC_MASK:
		return (len == 1);
	case NF9_INPUT_SNMP:
		return (len <= 2);
	case NF9_L4_DST_PORT:
		return (len == 2);
	case NF9_IPV4_DST_ADDR:
		return (len == 4);
	case NF9_DST_MASK:
		return (len == 1);
	case NF9_OUTPUT_SNMP:
		return (len <= 2);
	case NF9_IPV4_NEXT_HOP:
		return (len == 4);
	case NF9_SRC_AS:
		return (len <= 2);
	case NF9_DST_AS:
		return (len <= 2);
	case NF9_LAST_SWITCHED:
		return (len <= 4);
	case NF9_FIRST_SWITCHED:
		return (len <= 4);
	case NF9_IPV6_SRC_ADDR:
		return (len == 16);
	case NF9_IPV6_DST_ADDR:
		return (len == 16);
	case NF9_IPV6_SRC_MASK:
		return (len == 1);
	case NF9_IPV6_DST_MASK:
		return (len == 1);
	case NF9_ENGINE_TYPE:
		return (len == 1);
	case NF9_ENGINE_ID:
		return (len == 1);
	case NF9_IPV6_NEXT_HOP:
		return (len == 16);
	default:
		return (1);
	}
}

static int
nf9_flowset_to_store(u_int8_t *pkt, size_t len, struct xaddr *flow_source,
    struct NF9_HEADER *nf9_hdr, struct peer_nf9_template *template,
    struct store_flow_complete *flow)
{
	u_int offset, i;

	if (template->total_len > len)
		return (-1);

	bzero(flow, sizeof(*flow));

	flow->hdr.fields = STORE_FIELD_RECV_TIME | STORE_FIELD_AGENT_INFO |
	    STORE_FIELD_AGENT_ADDR;
	flow->ainfo.sys_uptime_ms = nf9_hdr->uptime_ms;
	flow->ainfo.time_sec = nf9_hdr->time_sec;
	flow->ainfo.netflow_version = nf9_hdr->c.version;
	flow->finf.flow_sequence = nf9_hdr->package_sequence;
	flow->recv_time.recv_secs = time(NULL);
	memcpy(&flow->agent_addr, flow_source, sizeof(flow->agent_addr));

	offset = 0;
	for (i = 0; i < template->num_records; i++) {
#ifdef DEBUG_NF9
		logit(LOG_DEBUG, "    record %d: type %d len %d: %s",
		    i, template->records[i].type, template->records[i].len,
		    data_ntoa(pkt + offset, template->records[i].len));
#endif
		nf9_rec_to_flow(&template->records[i], flow, pkt + offset);
		offset += template->records[i].len;
	}
	return (0);
}

static int
process_netflow_v9_template(u_int8_t *pkt, size_t len, struct peer_state *peer,
    struct peers *peers, u_int source_id)
{
	struct NF9_TEMPLATE_FLOWSET_HEADER *tmplh;
	struct NF9_TEMPLATE_FLOWSET_RECORD *tmplr;
	u_int i, count, offset, template_id, total_size;
	struct peer_nf9_record *recs;
	struct peer_nf9_template *template;

	logit(LOG_DEBUG, "netflow v.9 tempate flowset");
	/* dump_packet(__func__, pkt, len); */

	tmplh = (struct NF9_TEMPLATE_FLOWSET_HEADER *)pkt;
	if (len < sizeof(*tmplh)) {
		peer->ninvalid++;
		logit(LOG_WARNING, "short netflow v.9 flowset template header "
		    "%d bytes from %s", len, addr_ntop_buf(&peer->from));
		/* XXX ratelimit */
		return (-1);
	}

	template_id = ntohs(tmplh->template_id);
	count = ntohs(tmplh->count);

	if ((recs = calloc(count, sizeof(*recs))) == NULL)
		logerrx("%s: calloc failed (num %d)", __func__, count);

	logit(LOG_DEBUG, "NetFlow v.9 template with %d records:", count);

	offset = sizeof(*tmplh);
	total_size = 0;

	for (i = 0; i < count; i++) {
		if (offset >= len) {
			free(recs);
			peer->ninvalid++;
			logit(LOG_WARNING, "short netflow v.9 flowset template "
			    "packet %d bytes from %s", len,
			    addr_ntop_buf(&peer->from));
			/* XXX ratelimit */
			return (-1);
		}
		tmplr = (struct NF9_TEMPLATE_FLOWSET_RECORD *)(pkt + offset);

		recs[i].type = ntohs(tmplr->type);
		recs[i].len = ntohs(tmplr->length);
#ifdef DEBUG_NF9
		logit(LOG_DEBUG, "  record %d: type %d len %d",
		    i, recs[i].type, recs[i].len);
#endif
		total_size += recs[i].len;
		if (total_size > peers->max_template_len) {
			free(recs);
			peer->ninvalid++;
			logit(LOG_WARNING, "netflow v.9 flowset template "
			    "from %s too large len %d > max %d",
			    addr_ntop_buf(&peer->from), total_size,
			    peers->max_template_len);
			/* XXX ratelimit */
			return (-1);
		}
		if (!nf9_check_rec_len(recs[i].type, recs[i].len)) {
			free(recs);
			peer->ninvalid++;
			logit(LOG_WARNING, "Invalid field length in netflow v.9 "
			    "flowset template %d from %s/%08x type %d len %d",
			    template_id, addr_ntop_buf(&peer->from), source_id,
			    recs[i].type, recs[i].len);
			/* XXX ratelimit */
			return (-1);
		}
		/* XXX kill existing template on error! */
		offset += sizeof(*tmplr);
	}

	template = peer_nf9_find_template(peer, source_id, template_id);
	if (template == NULL) {
		template = peer_nf9_new_template(peer, peers,
		    source_id, template_id);
	}

	if (template->records != NULL)
		free(template->records);

	template->records = recs;
	template->num_records = i;
	template->total_len = total_size;

	return (0);
}

static int
process_netflow_v9_data(u_int8_t *pkt, size_t len, struct peer_state *peer,
    u_int source_id, struct NF9_HEADER *nf9_hdr, struct flowd_config *conf,
    int log_fd, u_int *num_flows)
{
	struct store_flow_complete *flows;
	struct peer_nf9_template *template;
	struct NF9_DATA_FLOWSET_HEADER *dath;
	u_int flowset_id, i, offset, num_flowsets;

	*num_flows = 0;

	logit(LOG_DEBUG, "netflow v.9 data flowset");

	dath = (struct NF9_DATA_FLOWSET_HEADER *)pkt;
	if (len < sizeof(*dath)) {
		peer->ninvalid++;
		logit(LOG_WARNING, "short netflow v.9 data flowset header "
		    "%d bytes from %s", len, addr_ntop_buf(&peer->from));
		/* XXX ratelimit */
		return (-1);
	}

	flowset_id = ntohs(dath->c.flowset_id);

	if ((template = peer_nf9_find_template(peer, source_id,
	    flowset_id)) == NULL) {
	    	peer->no_template++;
		logit(LOG_DEBUG, "netflow v.9 data flowset without template "
		    "%s/%08x/%04x", addr_ntop_buf(&peer->from), source_id,
		    flowset_id);
		return (0);
	}

	if (template->records == NULL)
		logerrx("%s: template->records == NULL", __func__);

	offset = sizeof(*dath);
	num_flowsets = (len - offset) / template->total_len;

	if (num_flowsets == 0 || num_flowsets > 0x4000) {
		logit(LOG_WARNING, "invalid netflow v.9 data flowset "
		    "from %s: strange number of flows %d",
		    addr_ntop_buf(&peer->from), num_flowsets);
		return (-1);
	}

	if ((flows = calloc(num_flowsets, sizeof(*flows))) == NULL)
		logerrx("%s: calloc failed (num %d)", __func__, num_flowsets);

	for (i = 0; i < num_flowsets; i++) {
		if (nf9_flowset_to_store(pkt + offset, template->total_len,
		    &peer->from, nf9_hdr, template, &flows[i]) == -1) {
			peer->ninvalid++;
			free(flows);
			logit(LOG_WARNING, "invalid netflow v.9 data flowset "
			    "from %s", addr_ntop_buf(&peer->from));
			/* XXX ratelimit */
			return (-1);
		}

		offset += template->total_len;
	}
	*num_flows = i;

	for (i = 0; i < *num_flows; i++)
		process_flow(&flows[i], conf, log_fd);

	free(flows);

	return (0);
}

static void
process_netflow_v9(u_int8_t *pkt, size_t len, struct xaddr *flow_source,
    struct flowd_config *conf, struct peer_state *peer, struct peers *peers,
    int log_fd)
{
	struct NF9_HEADER *nf9_hdr = (struct NF9_HEADER *)pkt;
	struct NF9_FLOWSET_HEADER_COMMON *flowset;
	u_int i, count, flowset_id, flowset_len, flowset_flows, total_flows;
	u_int offset, source_id;

	if (len < sizeof(*nf9_hdr)) {
		peer->ninvalid++;
		logit(LOG_WARNING, "short netflow v.9 header %d bytes from %s",
		    len, addr_ntop_buf(flow_source));
		return;
	}

	count = ntohs(nf9_hdr->c.flows);
	source_id = ntohl(nf9_hdr->source_id);

	offset = sizeof(*nf9_hdr);
	total_flows = 0;

	for (i = 0;; i++) {
		/* Make sure we don't run off the end of the flow */
		if (offset >= len) {
			peer->ninvalid++;
			logit(LOG_WARNING,
			    "short netflow v.9 flowset header %d bytes from %s",
			    len, addr_ntop_buf(flow_source));
			return;
		}

		flowset = (struct NF9_FLOWSET_HEADER_COMMON *)(pkt + offset);
		flowset_id = ntohs(flowset->flowset_id);
		flowset_len = ntohs(flowset->length);

#ifdef DEBUG_NF9
		logit(LOG_DEBUG, "offset=%d i=%d len=%d count=%d", offset, i, len, count);
		logit(LOG_DEBUG, "netflow v.9 flowset %d: type %d(0x%04x) len %d(0x%04x)",
		    i, flowset_id, flowset_id, flowset_len, flowset_len);
#endif

		/*
		 * Yes, this is a near duplicate of the short packet check
		 * above, but this one validates the flowset length from in
		 * the packet before we pass it to the flowset-specific
		 * handlers below.
		 */
		if (offset + flowset_len > len) {
			peer->ninvalid++;
			logit(LOG_WARNING,
			    "short netflow v.9 flowset length %d bytes from %s",
			    len, addr_ntop_buf(flow_source));
			return;
		}

		switch (flowset_id) {
		case NF9_TEMPLATE_FLOWSET_ID:
			if (process_netflow_v9_template(pkt + offset,
			    flowset_len, peer, peers, source_id) != 0)
				return;
			break;
		case NF9_OPTIONS_FLOWSET_ID:
			/* XXX: implement this (maybe) */
			logit(LOG_DEBUG, "netflow v.9 options flowset");
			break;
		default:
			if (flowset_id < NF9_MIN_RECORD_FLOWSET_ID) {
				logit(LOG_WARNING, "Received unknown netflow "
				    "v.9 reserved flowset type %d from %s",
				    flowset_id, addr_ntop_buf(flow_source));
				/* XXX ratelimit */
				break;
			}
			if (process_netflow_v9_data(pkt + offset, flowset_len,
			    peer, source_id, nf9_hdr, conf, log_fd,
			    &flowset_flows) != 0)
				return;
			total_flows += flowset_flows;
			break;
		}
		offset += flowset_len;
		if (offset == len)
			break;
		/* XXX check header->count against what we got */
	}

	/* Don't update peer unless we actually receive data from it */
	if (total_flows > 0)
		update_peer(peers, peer, total_flows, 9);
}

static void
process_input(struct flowd_config *conf, struct peers *peers,
    int net_fd, int log_fd)
{
	struct sockaddr_storage from;
	struct peer_state *peer;
	socklen_t fromlen;
	u_int8_t buf[2048];
	ssize_t len;
	struct NF_HEADER_COMMON *hdr;
	struct xaddr flow_source;

 retry:
	fromlen = sizeof(from);
	if ((len = recvfrom(net_fd, buf, sizeof(buf), 0,
	    (struct sockaddr *)&from, &fromlen)) < 0) {
		if (errno == EINTR)
			goto retry;
		if (errno != EAGAIN) {
			logit(LOG_WARNING, "recvfrom(fd = %d)", net_fd);
		}
		/* XXX ratelimit errors */
		return;
	}
	if (addr_sa_to_xaddr((struct sockaddr *)&from, fromlen,
	    &flow_source) == -1) {
		logit(LOG_WARNING, "Invalid agent address");
		return;
	}

	if ((peer = find_peer(peers, &flow_source)) == NULL)
		peer = new_peer(peers, conf, &flow_source);
	if (peer == NULL) {
		logit(LOG_DEBUG, "packet from unauthorised agent %s",
		    addr_ntop_buf(&flow_source));
		return;
	}

	if ((size_t)len < sizeof(*hdr)) {
		peer->ninvalid++;
		logit(LOG_WARNING, "short packet %d bytes from %s", len,
		    addr_ntop_buf(&flow_source));
		return;
	}

	hdr = (struct NF_HEADER_COMMON *)buf;
	switch (ntohs(hdr->version)) {
	case 1:
		process_netflow_v1(buf, len, &flow_source, conf, peer,
		    peers, log_fd);
		break;
	case 5:
		process_netflow_v5(buf, len, &flow_source, conf, peer,
		    peers, log_fd);
		break;
	case 7:
		process_netflow_v7(buf, len, &flow_source, conf, peer,
		    peers, log_fd);
		break;
	case 9:
		process_netflow_v9(buf, len, &flow_source, conf, peer,
		    peers, log_fd);
		break;
	default:
		logit(LOG_INFO, "Unsupported netflow version %u from %s",
		    ntohs(hdr->version), addr_ntop_buf(&flow_source));
#ifdef DEBUG_UNKNOWN
		dump_packet("Unknown packet type", buf, len);
#endif
		return;
	}
}

static void
init_pfd(struct flowd_config *conf, struct pollfd **pfdp, int mfd, int *num_fds)
{
	struct pollfd *pfd = *pfdp;
	struct listen_addr *la;
	int i;

	logit(LOG_DEBUG, "%s: entering (num_fds = %d)", __func__, *num_fds);

	if (pfd != NULL)
		free(pfd);

	*num_fds = 1; /* fd to monitor */

	/* Count socks */
	TAILQ_FOREACH(la, &conf->listen_addrs, entry)
		(*num_fds)++;

	if ((pfd = calloc((*num_fds) + 1, sizeof(*pfd))) == NULL) {
		logerrx("%s: calloc failed (num %d)",
		    __func__, *num_fds + 1);
	}

	pfd[0].fd = mfd;
	pfd[0].events = POLLIN;

	i = 1;
	TAILQ_FOREACH(la, &conf->listen_addrs, entry) {
		pfd[i].fd = la->fd;
		pfd[i].events = POLLIN;
		i++;
	}

	*pfdp = pfd;

	logit(LOG_DEBUG, "%s: done (num_fds = %d)", __func__, *num_fds);
}

static void
flowd_mainloop(struct flowd_config *conf, struct peers *peers, int monitor_fd)
{
	int i, log_fd, num_fds = 0;
	struct listen_addr *la;
	struct pollfd *pfd = NULL;

	init_pfd(conf, &pfd, monitor_fd, &num_fds);

	/* Main loop */
	log_fd = -1;
	for(;exit_flag == 0;) {
		if (reopen_flag && log_fd != -1) {
			logit(LOG_INFO, "log reopen requested");
			close(log_fd);
			log_fd = -1;
			reopen_flag = 0;
		}
		if (reconf_flag) {
			logit(LOG_INFO, "reconfiguration requested");
			if (client_reconfigure(monitor_fd, conf) == -1)
				logerrx("reconfigure failed, exiting");
			init_pfd(conf, &pfd, monitor_fd, &num_fds);
			scrub_peers(conf, peers);
			reconf_flag = 0;
		}
		if (log_fd == -1)
			log_fd = start_log(monitor_fd);

		if (info_flag) {
			struct filter_rule *fr;

			info_flag = 0;
			TAILQ_FOREACH(fr, &conf->filter_list, entry)
				logit(LOG_INFO, "%s", format_rule(fr));
			dump_peers(peers);
		}

		i = poll(pfd, num_fds, INFTIM);
		if (i <= 0) {
			if (i == 0 || errno == EINTR)
				continue;
			logerr("%s: poll", __func__);
		}

		/* monitor exited */
		if (pfd[0].revents != 0) {
			logit(LOG_DEBUG, "%s: monitor closed", __func__);
			break;
		}

		i = 1;
		TAILQ_FOREACH(la, &conf->listen_addrs, entry) {
			if ((pfd[i].revents & POLLIN) != 0)
				process_input(conf, peers, pfd[i].fd, log_fd);
			i++;
		}
	}

	if (exit_flag != 0)
		logit(LOG_NOTICE, "Exiting on signal %d", exit_flag);
}

static void
startup_listen_init(struct flowd_config *conf)
{
	struct listen_addr *la;

	TAILQ_FOREACH(la, &conf->listen_addrs, entry) {
		if ((la->fd = open_listener(&la->addr, la->port)) == -1) {
			logerrx("Listener setup of [%s]:%d failed",
			    addr_ntop_buf(&la->addr), la->port);
		}
	}
}

/* Display commandline usage information */
static void
usage(void)
{
	fprintf(stderr, "Usage: %s [options]\n", PROGNAME);
	fprintf(stderr, "This is %s version %s. Valid commandline options:\n",
	    PROGNAME, PROGVER);
	fprintf(stderr, "  -d              Run in the foreground and print debug information\n");
	fprintf(stderr, "  -g              Run in the foreground and log to stderr\n");
	fprintf(stderr, "  -h              Display this help\n");
	fprintf(stderr, "  -f path         Configuration file (default: %s)\n",
	    DEFAULT_CONFIG);
	fprintf(stderr, "\n");
}

int
main(int argc, char **argv)
{
	int ch;
	extern char *optarg;
	extern int optind;
	const char *config_file = DEFAULT_CONFIG;
	struct flowd_config conf;
	int monitor_fd;
	struct peers peers;

#ifndef HAVE_SETPROCTITLE
	compat_init_setproctitle(argc, &argv);
#endif
	umask(0077);
	closefrom(STDERR_FILENO + 1);

#ifdef HAVE_TZSET
	tzset();
#endif
	loginit(PROGNAME, 1, 0);

	bzero(&conf, sizeof(conf));
	bzero(&peers, sizeof(peers));
	peers.max_peers = DEFAULT_MAX_PEERS;
	peers.max_templates = DEFAULT_MAX_TEMPLATES;
	peers.max_sources = DEFAULT_MAX_SOURCES;
	peers.max_template_len = DEFAULT_MAX_TEMPLATE_LEN;
	SPLAY_INIT(&peers.peer_tree);
	TAILQ_INIT(&peers.peer_list);

	while ((ch = getopt(argc, argv, "dghD:f:")) != -1) {
		switch (ch) {
		case 'd':
			conf.opts |= FLOWD_OPT_DONT_FORK;
			conf.opts |= FLOWD_OPT_VERBOSE;
			loginit(PROGNAME, 1, 1);
			break;
		case 'g':
			conf.opts |= FLOWD_OPT_DONT_FORK;
			loginit(PROGNAME, 1, 1);
			break;
		case 'h':
			usage();
			return (0);
		case 'D':
			if (cmdline_symset(optarg) < 0)
				logerrx("Could not parse macro "
				    "definition %s", optarg);
			break;
		case 'f':
			config_file = optarg;
			break;
		default:
			fprintf(stderr, "Invalid commandline option.\n");
			usage();
			exit(1);
		}
	}

	if (read_config(config_file, &conf) == -1)
		logerrx("Config file has errors");

	/* Start listening (do this early to report errors before privsep) */
	startup_listen_init(&conf);

	/* Start the monitor - we continue as the unprivileged child */
	privsep_init(&conf, &monitor_fd, config_file);

	signal(SIGINT, sighand_exit);
	signal(SIGTERM, sighand_exit);
	signal(SIGHUP, sighand_reconf);
	signal(SIGUSR1, sighand_reopen);
	signal(SIGUSR2, sighand_info);
#ifdef SIGINFO
	signal(SIGINFO, sighand_info);
#endif

	flowd_mainloop(&conf, &peers, monitor_fd);

	return (0);
}
