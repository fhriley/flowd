# Copyright (c) 2004 Damien Miller <djm@mindrot.org>
#
# Permission to use, copy, modify, and distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

# $Id$

VERSION = "0.4.1"

import struct
import time
import socket
import sys
import zlib

def iso_time(secs, utc = 0):
	if utc:
		tm = time.gmtime(secs)
	else:
		tm = time.localtime(secs)

	return "%04u-%02u-%02uT%02u:%02u:%02u" % tm[:6]

def interval_time(t):
	intervals = [	[ "s", 60 ], [ "m", 60 ], [ "h", 24 ], 
			[ "d", 7 ], [ "w", 52 ]]
	ret = ""
	for interval in intervals:
		r = t % interval[1]
		t = int(t / interval[1])
		if r != 0 or interval[0] == "s":
			ret = "%u%s%s" % (r, interval[0], ret)
	if t > 0:
		ret = "%uy%s", (t, ret)

	return ret


def interval_time_ms(tms):
	return "%s.%03u" % ( interval_time(int(tms / 1000)), tms % 1000 )

class log:
	def __init__(self, path):
		self.path = path
		self.flow_file = open(path, "r")
		
		# Read header
		hdr = self.flow_file.read(16)
		if len(hdr) != 16:
			raise ValueError, "Short read on flow header"
		(self.magic, self.version, self.start_time, self.flags) = \
		    struct.unpack(">IIII", hdr)

		if self.magic != 0x012cf047:
			raise ValueError, "Bad magic"
		if self.version != 0x00000002:
			raise ValueError, "Unsupported version"

	def finish(self):
		self.flow_file.close()
		self.flow_file = None

	def readflow(self):
		try:
			f = flow(self.flow_file)
		except EOFError:
			f = None
		return f

class flow:
	TAG			= 0x00000001
	RECV_TIME		= 0x00000002
	PROTO_FLAGS_TOS		= 0x00000004
	AGENT_ADDR4		= 0x00000008
	AGENT_ADDR6		= 0x00000010
	SRC_ADDR4		= 0x00000020
	SRC_ADDR6		= 0x00000040
	DST_ADDR4		= 0x00000080
	DST_ADDR6		= 0x00000100
	GATEWAY_ADDR4		= 0x00000200
	GATEWAY_ADDR6		= 0x00000400
	SRCDST_PORT		= 0x00000800
	PACKETS			= 0x00001000
	OCTETS			= 0x00002000
	IF_INDICES		= 0x00004000
	AGENT_INFO		= 0x00008000
	FLOW_TIMES		= 0x00010000
	AS_INFO			= 0x00020000
	FLOW_ENGINE_INFO	= 0x00040000
	CRC32			= 0x40000000

	# Some useful combinations
	AGENT_ADDR		= 0x00000018
	SRC_ADDR		= 0x00000060
	DST_ADDR		= 0x00000180
	SRCDST_ADDR		= 0x000001e0
	GATEWAY_ADDR		= 0x00000600
	BRIEF			= 0x000039ff
	ALL			= 0x4007ffff

	_fieldspec = [
#	  Field 		Len   packspec	Unpacked fields
	[ "TAG",		4,    ">I", 	[ "tag" ] ],
	[ "RECV_TIME",		4,    ">I", 	[ "recv_secs" ] ],
	[ "PROTO_FLAGS_TOS",	4,    ">BBBx",	[ "tcp_flags", "protocol", "tos" ] ],
	[ "AGENT_ADDR4",	4,    None, 	[ "agent_addr" ] ],
	[ "AGENT_ADDR6",	16,   None, 	[ "agent_addr" ] ],
	[ "SRC_ADDR4",		4,    None, 	[ "src_addr" ] ],
	[ "SRC_ADDR6",		16,   None, 	[ "src_addr" ] ],
	[ "DST_ADDR4",		4,    None, 	[ "dst_addr" ] ],
	[ "DST_ADDR6",		16,   None, 	[ "dst_addr" ] ],
	[ "GATEWAY_ADDR4",	4,    None, 	[ "gateway_addr" ] ],
	[ "GATEWAY_ADDR6",	16,   None, 	[ "gateway_addr" ] ],
	[ "SRCDST_PORT",	4,   ">HH", 	[ "src_port", "dst_port" ] ],
	[ "PACKETS",		8,   ">Q", 	[ "flow_packets" ] ],
	[ "OCTETS",		8,   ">Q", 	[ "flow_octets" ] ],
	[ "IF_INDICES",		4,   ">HH", 	[ "if_index_in", "if_index_out" ] ],
	[ "AGENT_INFO",		16,  ">IIIHxx",	[ "sys_uptime_ms", "time_sec", "time_nanosec", "netflow_version" ] ],
	[ "FLOW_TIMES",		8,   ">II", 	[ "flow_start", "flow_finish" ] ],
	[ "AS_INFO",		8,   ">HHBBH", 	[ "src_as", "dst_as", "src_masklen", "dst_masklen" ] ],
	[ "FLOW_ENGINE_INFO",	8,   ">BBxxI",	[ "engine_type", "engine_id", "flow_sequence" ] ],
	[ "CRC32",		4,   ">I", 	[ "crc" ] ],
]

	def __init__(self, flow_file):
		self.crc32 = crc32()
		self.fields = {}
		self.rawfields = {} 

		# Read flow header
		hdr = flow_file.read(4)
		if len(hdr) == 0:
			raise EOFError
		if len(hdr) != 4:
			raise ValueError, "Short read on flow header"
		self.crc32.update(hdr)
		( fields, ) = struct.unpack(">I", hdr)

		self.rawfields["fields"] = hdr
		self.fields["fields"] = fields

		for finf in self.__class__._fieldspec:
			if fields & self.__class__.__dict__[finf[0]] == 0:
				continue

			fdata = flow_file.read(finf[1])

			if len(fdata) != finf[1]:
				raise ValueError, \
				    "Short read on flow field" + finf[0]

			if self.__class__.__dict__[finf[0]] != flow.CRC32:
				self.crc32.update(fdata)

			if finf[2] is None:
				# Addresses
				if len(fdata) == 4:
					af = socket.AF_INET
				elif len(fdata) == 16:
					af = socket.AF_INET6
				else:
					raise ValueError, \
					    "Unexpected address length"
				addr = socket.inet_ntop(af, fdata)

				self.fields[finf[3][0]] = addr
				self.fields[finf[3][0] + "_af"] = af
			else:
				ffields = struct.unpack(finf[2], fdata)
				for mm in zip(finf[3], ffields):
					self.fields[mm[0]] = mm[1]

		if fields & flow.CRC32:
			if self.crc32.final() != self.fields["crc"]:
				raise ValueError, "Checksum mismatch"

	def format(self, field_mask = BRIEF, utc = 0):
		fields = self.fields["fields"] & field_mask
		ret = "FLOW "

		if fields & self.__class__.TAG != 0:
			ret = ret + "tag %u " % self.fields["tag"]
		if fields & self.__class__.RECV_TIME != 0:
			ret = ret + "recv_time %s " % \
			    iso_time(self.fields["recv_secs"], utc)
		if fields & self.__class__.PROTO_FLAGS_TOS != 0:
			ret = ret + "proto %u " % self.fields["protocol"]
			ret = ret + "tcpflags %02x " % self.fields["tcp_flags"]
			ret = ret + "tos %02x " % self.fields["tos"]
		if fields & self.__class__.AGENT_ADDR != 0: 
			ret = ret + "agent %s " % self.fields["agent_addr"]
		if fields & self.__class__.SRC_ADDR != 0: 
			ret = ret + "src %s" % self.fields["src_addr"];
			if fields & self.__class__.SRCDST_PORT != 0: 
				ret = ret + ":%u" % self.fields["src_port"];
			ret = ret + " ";
		if fields & self.__class__.DST_ADDR != 0: 
			ret = ret + "dst %s" % self.fields["dst_addr"];
			if fields & self.__class__.SRCDST_PORT != 0: 
				ret = ret + ":%u" % self.fields["dst_port"];
			ret = ret + " ";
		if fields & self.__class__.GATEWAY_ADDR != 0: 
			ret = ret + "gateway %s " % self.fields["gateway_addr"];
		if fields & self.__class__.PACKETS != 0: 
			ret = ret + "packets %s " % self.fields["flow_packets"];
		if fields & self.__class__.OCTETS != 0: 
			ret = ret + "octets %s " % self.fields["flow_octets"];
		if fields & self.__class__.IF_INDICES != 0: 
			ret = ret + "in_if %u " % self.fields["if_index_in"];
			ret = ret + "out_if %u " % self.fields["if_index_out"];
		if fields & self.__class__.AGENT_INFO != 0: 
			ret = ret + "sys_uptime_ms %s " % \
			    interval_time_ms(self.fields["sys_uptime_ms"]);
			ret = ret + "time_sec %s " % \
			    iso_time(self.fields["time_sec"], utc);
			ret = ret + "time_nanosec %u " % \
			    self.fields["time_nanosec"];
			ret = ret + "netflow ver %u " % \
				self.fields["netflow_version"];
		if fields & self.__class__.FLOW_TIMES != 0: 
			ret = ret + "flow_start %s " % \
			    interval_time_ms(self.fields["flow_start"]);
			ret = ret + "flow_finish %s " % \
			    interval_time_ms(self.fields["flow_finish"]);
		if fields & self.__class__.AS_INFO != 0: 
			ret = ret + "src_AS %u " % self.fields["src_as"];
			ret = ret + "src_masklen %u " % \
			    self.fields["src_masklen"];
			ret = ret + "dst_AS %u " % self.fields["dst_as"];
			ret = ret + "dst_masklen %u " % \
			    self.fields["dst_masklen"];
		if fields & self.__class__.FLOW_ENGINE_INFO != 0: 
			ret = ret + "engine_type %u " % \
			    self.fields["engine_type"];
			ret = ret + "engine_id %u " % self.fields["engine_id"];
			ret = ret + "seq %u " % self.fields["flow_sequence"];
		if fields & self.__class__.CRC32 != 0: 
			ret = ret + "crc32 %08x " % self.fields["crc"];

		return ret;

class crc32:
	_CRC32TAB = [
		0x00000000L, 0x77073096L, 0xee0e612cL, 0x990951baL,
		0x076dc419L, 0x706af48fL, 0xe963a535L, 0x9e6495a3L,
		0x0edb8832L, 0x79dcb8a4L, 0xe0d5e91eL, 0x97d2d988L,
		0x09b64c2bL, 0x7eb17cbdL, 0xe7b82d07L, 0x90bf1d91L,
		0x1db71064L, 0x6ab020f2L, 0xf3b97148L, 0x84be41deL,
		0x1adad47dL, 0x6ddde4ebL, 0xf4d4b551L, 0x83d385c7L,
		0x136c9856L, 0x646ba8c0L, 0xfd62f97aL, 0x8a65c9ecL,
		0x14015c4fL, 0x63066cd9L, 0xfa0f3d63L, 0x8d080df5L,
		0x3b6e20c8L, 0x4c69105eL, 0xd56041e4L, 0xa2677172L,
		0x3c03e4d1L, 0x4b04d447L, 0xd20d85fdL, 0xa50ab56bL,
		0x35b5a8faL, 0x42b2986cL, 0xdbbbc9d6L, 0xacbcf940L,
		0x32d86ce3L, 0x45df5c75L, 0xdcd60dcfL, 0xabd13d59L,
		0x26d930acL, 0x51de003aL, 0xc8d75180L, 0xbfd06116L,
		0x21b4f4b5L, 0x56b3c423L, 0xcfba9599L, 0xb8bda50fL,
		0x2802b89eL, 0x5f058808L, 0xc60cd9b2L, 0xb10be924L,
		0x2f6f7c87L, 0x58684c11L, 0xc1611dabL, 0xb6662d3dL,
		0x76dc4190L, 0x01db7106L, 0x98d220bcL, 0xefd5102aL,
		0x71b18589L, 0x06b6b51fL, 0x9fbfe4a5L, 0xe8b8d433L,
		0x7807c9a2L, 0x0f00f934L, 0x9609a88eL, 0xe10e9818L,
		0x7f6a0dbbL, 0x086d3d2dL, 0x91646c97L, 0xe6635c01L,
		0x6b6b51f4L, 0x1c6c6162L, 0x856530d8L, 0xf262004eL,
		0x6c0695edL, 0x1b01a57bL, 0x8208f4c1L, 0xf50fc457L,
		0x65b0d9c6L, 0x12b7e950L, 0x8bbeb8eaL, 0xfcb9887cL,
		0x62dd1ddfL, 0x15da2d49L, 0x8cd37cf3L, 0xfbd44c65L,
		0x4db26158L, 0x3ab551ceL, 0xa3bc0074L, 0xd4bb30e2L,
		0x4adfa541L, 0x3dd895d7L, 0xa4d1c46dL, 0xd3d6f4fbL,
		0x4369e96aL, 0x346ed9fcL, 0xad678846L, 0xda60b8d0L,
		0x44042d73L, 0x33031de5L, 0xaa0a4c5fL, 0xdd0d7cc9L,
		0x5005713cL, 0x270241aaL, 0xbe0b1010L, 0xc90c2086L,
		0x5768b525L, 0x206f85b3L, 0xb966d409L, 0xce61e49fL,
		0x5edef90eL, 0x29d9c998L, 0xb0d09822L, 0xc7d7a8b4L,
		0x59b33d17L, 0x2eb40d81L, 0xb7bd5c3bL, 0xc0ba6cadL,
		0xedb88320L, 0x9abfb3b6L, 0x03b6e20cL, 0x74b1d29aL,
		0xead54739L, 0x9dd277afL, 0x04db2615L, 0x73dc1683L,
		0xe3630b12L, 0x94643b84L, 0x0d6d6a3eL, 0x7a6a5aa8L,
		0xe40ecf0bL, 0x9309ff9dL, 0x0a00ae27L, 0x7d079eb1L,
		0xf00f9344L, 0x8708a3d2L, 0x1e01f268L, 0x6906c2feL,
		0xf762575dL, 0x806567cbL, 0x196c3671L, 0x6e6b06e7L,
		0xfed41b76L, 0x89d32be0L, 0x10da7a5aL, 0x67dd4accL,
		0xf9b9df6fL, 0x8ebeeff9L, 0x17b7be43L, 0x60b08ed5L,
		0xd6d6a3e8L, 0xa1d1937eL, 0x38d8c2c4L, 0x4fdff252L,
		0xd1bb67f1L, 0xa6bc5767L, 0x3fb506ddL, 0x48b2364bL,
		0xd80d2bdaL, 0xaf0a1b4cL, 0x36034af6L, 0x41047a60L,
		0xdf60efc3L, 0xa867df55L, 0x316e8eefL, 0x4669be79L,
		0xcb61b38cL, 0xbc66831aL, 0x256fd2a0L, 0x5268e236L,
		0xcc0c7795L, 0xbb0b4703L, 0x220216b9L, 0x5505262fL,
		0xc5ba3bbeL, 0xb2bd0b28L, 0x2bb45a92L, 0x5cb36a04L,
		0xc2d7ffa7L, 0xb5d0cf31L, 0x2cd99e8bL, 0x5bdeae1dL,
		0x9b64c2b0L, 0xec63f226L, 0x756aa39cL, 0x026d930aL,
		0x9c0906a9L, 0xeb0e363fL, 0x72076785L, 0x05005713L,
		0x95bf4a82L, 0xe2b87a14L, 0x7bb12baeL, 0x0cb61b38L,
		0x92d28e9bL, 0xe5d5be0dL, 0x7cdcefb7L, 0x0bdbdf21L,
		0x86d3d2d4L, 0xf1d4e242L, 0x68ddb3f8L, 0x1fda836eL,
		0x81be16cdL, 0xf6b9265bL, 0x6fb077e1L, 0x18b74777L,
		0x88085ae6L, 0xff0f6a70L, 0x66063bcaL, 0x11010b5cL,
		0x8f659effL, 0xf862ae69L, 0x616bffd3L, 0x166ccf45L,
		0xa00ae278L, 0xd70dd2eeL, 0x4e048354L, 0x3903b3c2L,
		0xa7672661L, 0xd06016f7L, 0x4969474dL, 0x3e6e77dbL,
		0xaed16a4aL, 0xd9d65adcL, 0x40df0b66L, 0x37d83bf0L,
		0xa9bcae53L, 0xdebb9ec5L, 0x47b2cf7fL, 0x30b5ffe9L,
		0xbdbdf21cL, 0xcabac28aL, 0x53b39330L, 0x24b4a3a6L,
		0xbad03605L, 0xcdd70693L, 0x54de5729L, 0x23d967bfL,
		0xb3667a2eL, 0xc4614ab8L, 0x5d681b02L, 0x2a6f2b94L,
		0xb40bbe37L, 0xc30c8ea1L, 0x5a05df1bL, 0x2d02ef8dL
	]

	def __init__(self, arg = None):
		self.crc = 0
		if arg is not None:
			self.update(arg)

	def update(self, data):
		for i in range(0, len(data)):
			chunk = data[i:i + 1]
			( c, ) = struct.unpack(">B", chunk)
			self.crc = self._CRC32TAB[(self.crc ^ c) & 0xff] ^ \
			    (self.crc >> 8)
		return self.crc

	def final(self):
		return self.crc
