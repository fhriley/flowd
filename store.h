/*	$Id$	*/

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

/* On-disk storage format */

#ifndef _STORE_H
#define _STORE_H

#include "common.h"
#include "addr.h"

/* On-disk address formats for v4 and v6 addresses */
struct store_addr6 {
	u_int8_t	d[16];
} __packed;
struct store_addr4 {
	u_int8_t	d[4];
} __packed;

#define STORE_MAGIC			0x012cf047
#define STORE_VERSION			0x00000002
/* Start of flow log file */
struct store_header {
	u_int32_t		magic;
	u_int32_t		version;
	u_int32_t		start_time;
	u_int32_t		flags;	/* Currently 0 */
} __packed;

/*
 * Optional flow fields, specify what is stored for the flow
 * NB - the flow records appear in this order on disk
 */
#define STORE_FIELD_TAG			(1U)
#define STORE_FIELD_RECV_TIME		(1U<<1)
#define STORE_FIELD_PROTO_FLAGS_TOS	(1U<<2)
#define STORE_FIELD_AGENT_ADDR4		(1U<<3)
#define STORE_FIELD_AGENT_ADDR6		(1U<<4)
#define STORE_FIELD_SRC_ADDR4		(1U<<5)
#define STORE_FIELD_SRC_ADDR6		(1U<<6)
#define STORE_FIELD_DST_ADDR4		(1U<<7)
#define STORE_FIELD_DST_ADDR6		(1U<<8)
#define STORE_FIELD_GATEWAY_ADDR4	(1U<<9)
#define STORE_FIELD_GATEWAY_ADDR6	(1U<<10)
#define STORE_FIELD_SRCDST_PORT		(1U<<11)
#define STORE_FIELD_PACKETS		(1U<<12)
#define STORE_FIELD_OCTETS		(1U<<13)
#define STORE_FIELD_IF_INDICES		(1U<<14)
#define STORE_FIELD_AGENT_INFO		(1U<<15)
#define STORE_FIELD_FLOW_TIMES		(1U<<16)
#define STORE_FIELD_AS_INFO		(1U<<17)
#define STORE_FIELD_FLOW_ENGINE_INFO	(1U<<18)
/* ... more one day */

#define STORE_FIELD_CRC32		(1U<<30)
#define STORE_FIELD_RESERVED		(1U<<31) /* For extension header */
#define STORE_FIELD_ALL			(((1U<<19)-1)|STORE_FIELD_CRC32)

/* Useful combinations */
#define STORE_FIELD_AGENT_ADDR		(STORE_FIELD_AGENT_ADDR4|\
					 STORE_FIELD_AGENT_ADDR6)
#define STORE_FIELD_SRC_ADDR		(STORE_FIELD_SRC_ADDR4|\
					 STORE_FIELD_SRC_ADDR6)
#define STORE_FIELD_DST_ADDR		(STORE_FIELD_DST_ADDR4|\
					 STORE_FIELD_DST_ADDR6)
#define STORE_FIELD_SRCDST_ADDR		(STORE_FIELD_SRC_ADDR|\
					 STORE_FIELD_DST_ADDR)
#define STORE_FIELD_GATEWAY_ADDR	(STORE_FIELD_GATEWAY_ADDR4|\
					 STORE_FIELD_GATEWAY_ADDR6)

#define STORE_DISPLAY_ALL		STORE_FIELD_ALL
#define STORE_DISPLAY_BRIEF		(STORE_FIELD_TAG|\
					 STORE_FIELD_RECV_TIME|\
					 STORE_FIELD_PROTO_FLAGS_TOS|\
					 STORE_FIELD_SRCDST_PORT|\
					 STORE_FIELD_PACKETS|\
					 STORE_FIELD_OCTETS|\
					 STORE_FIELD_SRCDST_ADDR|\
					 STORE_FIELD_AGENT_ADDR4|\
					 STORE_FIELD_AGENT_ADDR6)

/* Start of flow record - present for every flow */
struct store_flow {
	u_int32_t		fields;
} __packed;

/*
 * Optional flow records
 * NB. suffixes must match the corresponding STORE_FIELD_ define (see store.c)
 */

/* Optional flow field - present if STORE_FIELD_TAG */
struct store_flow_TAG {
	u_int32_t		tag; /* set by filter */
} __packed;

/* Optional flow field - present if STORE_FIELD_RECV_TIME */
struct store_flow_RECV_TIME {
	u_int32_t		recv_secs;
} __packed;

/* Optional flow field - present if STORE_FIELD_PROTO_FLAGS_TOS */
struct store_flow_PROTO_FLAGS_TOS {
	u_int8_t		tcp_flags;
	u_int8_t		protocol;
	u_int8_t		tos;
	u_int8_t		pad;
} __packed;

/* Optional flow field - present if STORE_FIELD_AGENT_ADDR */
struct store_flow_AGENT_ADDR4 {
	struct store_addr4	flow_agent_addr;
} __packed;
struct store_flow_AGENT_ADDR6 {
	struct store_addr6	flow_agent_addr;
} __packed;

/* Optional flow field - present if STORE_FIELD_SRC_ADDR4 */
struct store_flow_SRC_ADDR4 {
	struct store_addr4	src_addr;
} __packed;

/* Optional flow field - present if STORE_FIELD_DST_ADDR4 */
struct store_flow_DST_ADDR4 {
	struct store_addr4	dst_addr;
} __packed;

/* Optional flow field - present if STORE_FIELD_SRC_ADDR6 */
struct store_flow_SRC_ADDR6 {
	struct store_addr6	src_addr;
} __packed;

/* Optional flow field - present if STORE_FIELD_DST_ADDR6 */
struct store_flow_DST_ADDR6 {
	struct store_addr6	dst_addr;
} __packed;

/* Optional flow field - present if STORE_FIELD_GATEWAY_ADDR */
struct store_flow_GATEWAY_ADDR4 {
	struct store_addr4	gateway_addr;
} __packed;
struct store_flow_GATEWAY_ADDR6 {
	struct store_addr6	gateway_addr;
} __packed;

/* Optional flow field - present if STORE_FIELD_SRCDST_PORT */
struct store_flow_SRCDST_PORT {
	u_int16_t		src_port;
	u_int16_t		dst_port;
} __packed;

/* Optional flow field - present if STORE_FIELD_PACKETS */
struct store_flow_PACKETS {
	u_int64_t		flow_packets;
} __packed;

/* Optional flow field - present if STORE_FIELD_OCTETS */
struct store_flow_OCTETS {
	u_int64_t		flow_octets;
} __packed;

/* Optional flow field - present if STORE_FIELD_IF_INDICES */
struct store_flow_IF_INDICES {
	u_int16_t		if_index_in;
	u_int16_t		if_index_out;
} __packed;

/* Optional flow field - present if STORE_FIELD_AGENT_INFO */
struct store_flow_AGENT_INFO {
	u_int32_t		sys_uptime_ms;
	u_int32_t		time_sec;
	u_int32_t		time_nanosec;
	u_int16_t		netflow_version;
	u_int16_t		pad;
} __packed;

/* Optional flow field - present if STORE_FIELD_FLOW_TIMES */
struct store_flow_FLOW_TIMES {
	u_int32_t		flow_start;
	u_int32_t		flow_finish;
} __packed;

/* Optional flow field - present if STORE_FIELD_AS_INFO */
struct store_flow_AS_INFO {
	u_int16_t		src_as;
	u_int16_t		dst_as;
	u_int8_t		src_mask;
	u_int8_t		dst_mask;
	u_int16_t		pad;
} __packed;

/* Optional flow field - present if STORE_FIELD_FLOW_ENGINE_INFO */
struct store_flow_FLOW_ENGINE_INFO {
	u_int8_t		engine_type;
	u_int8_t		engine_id;
	u_int16_t		pad;
	u_int32_t		flow_sequence;
} __packed;

/* Optional flow field - present if STORE_FIELD_CRC32 */
struct store_flow_CRC32 {
	u_int32_t		crc32;
} __packed;

/* A abstract flow record (all fields included) */
struct store_flow_complete {
	struct store_flow			hdr;
	struct store_flow_TAG			tag;
	struct store_flow_RECV_TIME		recv_time;
	struct store_flow_PROTO_FLAGS_TOS	pft;
	struct xaddr				agent_addr;
	struct xaddr				src_addr;
	struct xaddr				dst_addr;
	struct xaddr				gateway_addr;
	struct store_flow_SRCDST_PORT		ports;
	struct store_flow_PACKETS		packets;
	struct store_flow_OCTETS		octets;
	struct store_flow_IF_INDICES		ifndx;
	struct store_flow_AGENT_INFO		ainfo;
	struct store_flow_FLOW_TIMES		ftimes;
	struct store_flow_AS_INFO		asinf;
	struct store_flow_FLOW_ENGINE_INFO	finf;
	struct store_flow_CRC32			crc32;
} __packed;

int store_get_header(int fd, struct store_header *hdr, const char **errptr);
int store_get_flow(int fd, struct store_flow_complete *f, const char **errptr);
int store_check_header(int fd, const char **errptr);
int store_put_header(int fd, const char **errptr);
int store_put_flow(int fd, struct store_flow_complete *flow,
    u_int32_t fieldmask, const char **errptr);
int store_validate_header(struct store_header *hdr, const char **errptr);
int store_calc_flow_len(struct store_flow *hdr);
int store_flow_deserialise(u_int8_t *buf, int len,
    struct store_flow_complete *f, const char **errptr);
int store_flow_serialise(struct store_flow_complete *f, u_int8_t *buf, int len, 
    const char **errptr);

const char *iso_time(time_t t, int utc_flag);
const char *interval_time(time_t t);
void store_format_flow(struct store_flow_complete *flow, char *buf,
    size_t len, int utc_flag, u_int32_t display_mask);
u_int64_t store_ntohll(u_int64_t v);
u_int64_t store_htonll(u_int64_t v);

#endif /* _STORE_H */
