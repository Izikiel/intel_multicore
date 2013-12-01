//+++2003-11-18
//    Copyright (C) 2001,2002,2003  Mike Rieker, Beverly, MA USA
//
//    This program is free software; you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation; version 2 of the License.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program; if not, write to the Free Software
//    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//---2003-11-18

/************************************************************************/
/*									*/
/*  TCP/UDP/ICMP/IP driver						*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_dev_ipdns.h"
#include "oz_dev_timer.h"
#include "oz_io_ether.h"
#include "oz_io_ip.h"
#include "oz_knl_cache.h"
#include "oz_knl_devio.h"
#include "oz_knl_event.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_log.h"
#include "oz_knl_phymem.h"
#include "oz_knl_printk.h"
#include "oz_knl_process.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_shuthand.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"
#include "oz_sys_dateconv.h"

#define THREADID(iopex) oz_knl_thread_getid (oz_knl_ioop_getthread ((iopex) -> ioop))		// get request's thread-id
#define ACCESSBUFFS(iopex) oz_knl_process_setcur (oz_knl_ioop_getprocess ((iopex) -> ioop))	// access request's buffers

#define TDP if (iopex -> u.tcptransmit.p.debugme) oz_knl_printk					// TCP-debug-print

#define DEFWINSIZE (1460*3)
#define MINWINSIZE 512
#define MINMTU 576

#define IPHDRSIZE (20)
#define TCPHDRSIZE (IPHDRSIZE+20)

/* Short synonyms for stuff in oz_io_ether.h and oz_io_ip.h */

#define IPADDRSIZE OZ_IO_IP_ADDRSIZE	/* number of bytes for IP addresses */
#define DATASIZE   OZ_IO_ETHER_MAXDATA	/* max size an ethernet controller can send or receive */

#define PORTSIZE OZ_IO_IP_PORTSIZE	/* number of bytes in a UDP or TCP port number */

typedef uLong Ipad;			/* a type that can hold an IP address */
typedef uWord Sock;			/* a type than can hold a socket number */
typedef uWord Eproto;			/* a type that can hold ethernet protocol number */

#define ARPPKT(__hw, __cxb) ((Arppkt *)(__cxb + __hw -> ether_getinfo1.dataoffset))	/* where the arp packet is within the cxb */
#define IPPKT(__hw, __cxb) ((Ippkt *)(__cxb + __hw -> ether_getinfo1.dataoffset))	/* where the ip packet is within the cxb */

#define NUM_BUFFS_RCV_ARP (3)		/* number of ARP receive buffers per interface */
#define NUM_BUFFS_RCV_IP (10)		/* number of IP receive buffers per interface */
#define NUM_BUFFS_SNDU (12)		/* number of user send buffers per interface */

/* Buffer types */

typedef enum { BUF_TYPE_SNDK    = 1, 	/* internally originating buffers - there is an 'unlimited' supply of these */
               BUF_TYPE_SNDU    = 2, 	/* externally originating buffers - there is a limited supply of these */
               BUF_TYPE_RCV_ARP = 3, 	/* ARP receive buffers */
               BUF_TYPE_RCV_IP  = 4, 	/* IP receive buffers */
               BUF_TYPE_RCV_IPX = 5, 	/* extended IP receive buffer (No! It doesn't work with NetWare!) */
             } Buftype;

/* Timer resolution (ticks per second) and milliseconds per tick */

#define TPS 10
#define MSPT (1000/TPS)

/* Time to delay sending a lone ack anticipating that we may be able */
/* to piggyback it on some data that we are about to send anyway     */

#define ACKTIME 1

/* Debugging flag values */

#define debug oz_dev_ip_debug

#define DEBUG_FAILRATE 0x000000F
#define DEBUG_ARPCACHE 0x0000010
#define DEBUG_ETHERHDR 0x0000020
#define DEBUG_ARPPKT   0x0000040
#define DEBUG_IPHDR    0x0000080
#define DEBUG_ICMPHDR  0x0000100
#define DEBUG_UDPHDR   0x0000200
#define DEBUG_TCPHDR   0x0000400
#define DEBUG_ERRORS   0x0000800
#define DEBUG_TCPXMTQ  0x0004000
#define DEBUG_RECEIVE  0x0008000
#define DEBUG_TCPSUM   0x0010000
#define DEBUG_FORWARD  0x0020000
#define DEBUG_TCPFLOW  0x0040000
#define DEBUG_TCPRECV  0x0100000
#define DEBUG_THREAD   0x0200000
#define DEBUG_TCPTERM  0x0400000
#define DEBUG_BOUNCE   0x0800000
#define DEBUG_IOPOST   0x1000000

/* Host byte order <-> Network byte order conversions */

#define H2NW(h,n) { (n)[0] = (h) >> 8; (n)[1] = (h) & 0xff; }
#define H2NL(h,n) { (n)[0] = (h) >> 24; (n)[1] = (h) >> 16; (n)[2] = (h) >> 8; (n)[3] = (h); }
#define N2HW(n) (((n)[0] << 8) | (n)[1])
#define N2HL(n) (((n)[0] << 24) | ((n)[1] << 16) | ((n)[2] << 8) | (n)[3])

/* Ethernet protocol numbers */

#define PROTO_ARP 0x0806
#define PROTO_IP  0x0800

/* ARP related stuff */

#define ARP_OP_REQ 1		/* arp request */
#define ARP_OP_RPL 2		/* arp reply */

#define ARP_RETRY (TPS / 3)	/* number of ticks between retries */
#define ARP_REQ_LIFE (15 * TPS)	/* number of ticks to wait for a */
				/* reply before aborting the transmit */
#define ARPCACHE_LIFE (60 * TPS) /* number of ticks to */
				/* keep an arpcache entry */

/* IP related stuff */

#define PROTO_IP_ICMP 1		/* ICMP message type */
#define PROTO_IP_IGMP 2		/* IGMP message type (not used) */
#define PROTO_IP_TCP 6		/* TCP message type */
#define PROTO_IP_UDP 17		/* UDP message type */

#define IP_FLAGS_CE   0x8000	/* congestion */
#define IP_FLAGS_DF   0x4000	/* don't fragment packet */
#define IP_FLAGS_MF   0x2000	/* more fragments follow */
#define IP_FLAGS_OFFS 0x1FFF	/* fragment offset */
#define IP_FLAGS_SHFT 3		/* fragment offset shift */
#define IP_FRAG_LIFE (3 * TPS)	/* fragment lifetime (ticks) */

/* ICMP related stuff */

#define PROTO_IP_ICMP_PINGRPL  0	/* ping reply message */
#define PROTO_IP_ICMP_DESTUNR  3	/* destination unreachable */
#define PROTO_IP_ICMP_SQUENCH  4	/* source quench */
#define PROTO_IP_ICMP_REDIRECT 5	/* re-direct */
#define PROTO_IP_ICMP_PINGREQ  8	/* ping request message */
#define PROTO_IP_ICMP_TIMEEXC 11	/* time-to-live exceeded message */
#define PROTO_IP_ICMP_PRMPROB 12	/* parameter problem message */
#define PROTO_IP_ICMP_TIMEREQ 13	/* current system time request */
#define PROTO_IP_ICMP_TIMERPL 14	/* current system time reply */

#define ICMP_BOUNCE_TOTALEN (576)	/* maximum total length on an ICMP bounce */
#define ICMP_BOUNCE_TTL (127)		/* ICMP bounce message time-to-live */
#define ICMP_BOUNCE_INTERVAL (1)	/* wait this many ticks between sending */

/* UDP and TCP related stuff */

#define EPHEMMIN 5000		/* lowest ephemeral socket number (inclusive) */
#define EPHEMMAX 6000		/* highest ephemeral socket number (exclusive) */

/* TCP related stuff */

#define TCP_FLAGS_FIN 0x0001	/* sender is finished sending data */
#define TCP_FLAGS_SYN 0x0002	/* synchronize sequence numbers */
#define TCP_FLAGS_RST 0x0004	/* reset the connection */
#define TCP_FLAGS_PSH 0x0008	/* push data to application asap */
#define TCP_FLAGS_ACK 0x0010	/* the acknowledgment number is valid */
#define TCP_FLAGS_URG 0x0020	/* the urgent pointer is valid */
#define TCP_FLAGS_HLNMSK 0xF000	/* header length (in uLongs) */
#define TCP_FLAGS_HLNSHF 12
#define TCP_FLAGX_KA  0x00010000 /* internal flag - keepalive being sent */

#define TCP_OPT_END 0		/* tcp option - end of list */
#define TCP_OPT_NOP 1		/* tcp option - no-op (padding) */
#define TCP_OPT_MSS 2		/* tcp option - max segment size */

#define TCP_MAXSENDSIZE(__hw) ((uByte *)IPPKT (__hw, wscxb) - IPPKT (__hw, wscxb) -> dat.tcp.raw + __hw -> mtu)

/* Miscellaneous macros */

#define CLRIPAD(d) *(Ipad *)(d) = 0						/* clear an ip address */
#define ZERIPAD(d) (*(Ipad *)(d) == 0)						/* test ip address for zero */
#define CEQIPAD(x,y) (*(Ipad *)(x) == *(Ipad *)(y))				/* compare ip addresses */
#define CGTIPAD(x,y) (memcmp (x, y, IPADDRSIZE) > 0)				/* compare ip addresses */
#define CEQIPADM(x,y,m) (((*(Ipad *)(x) ^ *(Ipad *)(y)) & *(Ipad *)(m)) == 0)	/* compare ip addresses with mask */
#define CPYIPAD(d,s) *(Ipad *)(d) = *(Ipad *)(s)				/* copy ip address */
#define CPYIPADM(d,s,m) *(Ipad *)(d) = *(Ipad *)(s) & *(Ipad *)(m)		/* copy ip address and mask it */

#define CLRPORT(d) *(Sock *)(d) = 0						/* clear a port number */
#define ZERPORT(d) (*(Sock *)(d) == 0)						/* test port number for zero */
#define CEQPORT(x,y) (*(Sock *)(x) == *(Sock *)(y))				/* compare port numbers */
#define CPYPORT(d,s) *(Sock *)(d) = *(Sock *)(s)				/* copy a port number */

#define CPYENAD(d,s,hw) memcpy (d, s, hw -> ether_getinfo1.addrsize)		/* copy ethernet address */
#define CEQENAD(d,s,hw) (memcmp (d, s, hw -> ether_getinfo1.addrsize) == 0)	/* compare ethernet address */

#define memclr(b,l) memset ((b), 0, (l))

/* printf - diagnostic print routine */

#define PRINTF oz_knl_log_print (oz_dev_ip_log, __FILE__, __LINE__,

/* Struct typedefs */

typedef struct Filter Filter;
typedef struct Hwipam Hwipam;
typedef struct Hw Hw;
typedef struct Arpc Arpc;
typedef struct Route Route;
typedef struct Buf Buf;
typedef struct Tcpcon Tcpcon;
typedef struct Msubp Msubp;

typedef struct Abort Abort;
typedef struct Chnex Chnex;
typedef struct Iopex Iopex;

/************************************************************************/
/*									*/
/*  Message formats							*/
/*									*/
/************************************************************************/

/* Format of an ICMP packet */

#define Icmppkt OZ_IO_ip_icmppkt

/* Format of an UDP packet */

#define Udppkt OZ_IO_ip_udppkt

/* Format of a TCP packet */

#define Tcppkt OZ_IO_ip_tcppkt

/* Format of an IP packet */

#define Ippkt OZ_IO_ip_ippkt

/* Format of an ARP packet */

typedef struct { uByte hardtype[2];			// hardware address type
                 uByte prottype[2];			// protocol address type
                 uByte hardsize;			// hardware address size
                 uByte protsize;			// protocol address size
                 uByte op[2];				// opcode
                 uByte var[(OZ_IO_ETHER_MAXADDR+IPADDRSIZE)*2];	// sender's ethernet address
							// sender's ip address
							// target's ethernet address
							// target's ip address
               } Arppkt;

/************************************************************************/
/*									*/
/*  Internal structures							*/
/*									*/
/************************************************************************/

/* Packet filters */

typedef struct { uLong data;
                 uLong mask;
               } Filtera;

struct Filter { Filter *next;
                uLong action;
                int acount;
                int astart;
                Filtera a[1];
              };

/* Requests waiting for an user buffer */

struct Msubp { Msubp *next;
               Msubp **prev;
               void (*entry) (Buf *buf, void *param);
               void *param;
             };

/* Hardware interface struct */

struct Hwipam { Hwipam *next;
                uByte hwipaddr[IPADDRSIZE];		/* ip address of the hardware interface */
                uByte nwipaddr[IPADDRSIZE];		/* network's ip address */
                uByte nwipmask[IPADDRSIZE];		/* network's ip mask */
              };

struct Hw { Hw *next;					/* next in 'hws' list */
            const char *devname;			/* devunit's name string pointer */
            OZ_Iochan *iochan_arp;			/* arp protocol I/O channel */
            OZ_Iochan *iochan_ip;			/* ip protocol I/O channel */
            Hwipam *hwipams;				/* list of my ip addresses and masks */
            Arpc *arpcs;				/* list of known ip <-> ethernet address pairs */
            int arpcount;				/* number of items on arpcs queue (debugging only) */
            Buf  *arpwaits;				/* list of buf's waiting to transmit when arp cache entry is ready */
            Msubp *waitsndubufh;			/* list of requests waiting for user send buffer */
            Msubp **waitsndubuft;
            int waitsndubufc;				/* number of msubps on waitsndubufh (debugging only) */
            Buf *freesndubufs;				/* list of free user send buffers */
            int freesndubufc;				/* number of buffs on freesndubufs (debugging only) */
            int terminated;				/* set when it has been terminated */
            Long receivesinprog;			/* number of recieve i/o's in progress on ethernet device */
            Long transmitsinprog;			/* number of transmit i/o's in progress on ethernet device */
            uLong lastbounce;				/* tick that the last icmp bounce was sent */
            OZ_IO_ether_getinfo1 ether_getinfo1;	/* ethernet device's getinfo1 data */
            uWord mtu;					/* maximum message size (not incl en hdr and crc, including ip hdr and data) */
            uByte myenaddr[OZ_IO_ETHER_MAXADDR];	/* ethernet address of the interface */
          };

/* Arp Cache entries */

struct Arpc { Arpc *next;				/* pointer to next in arpcs list */
              uLong timeout;				/* when this entry expires (ticks) */
							/* - it is deleted from arpcs list */
              uByte ipaddr[IPADDRSIZE];			/* ip address */
              uByte enaddr[OZ_IO_ETHER_MAXADDR];	/* ethernet address */
            };

/* Routing table entries */

struct Route { Route *next;
               uByte gwipaddr[IPADDRSIZE];	/* gateway's ip address */
               uByte nwipaddr[IPADDRSIZE];	/* network's ip address */
               uByte nwipmask[IPADDRSIZE];	/* network's ip mask */
               Hw *hw;				/* hardware interface that the router is connected to */
						/* (or NULL if not currently reachable) */
               uByte hwipaddr[IPADDRSIZE];	/* the corresponding hwipaddr that it is connected to */
             };

/* Ethernet packet messages */

#define Cxb uByte

/* General send / receive buffer */

struct Buf { Cxb *cxb;				/* I/O buffer pointer */
             uLong iosts;			/* I/O status */
             uLong cxb_dlen;			/* length of data in cxb */
             Buftype type;			/* buffer type (BUF_TYPE_...) */
             Hw *hw;				/* hardware interface */
             Buf *next;				/* next in list - frags, arpwaits, rcvdoutofordr */
             Buf *frag;				/* next fragment - sorted by ascending fragment offset */
             uLong timeout;			/* when this entry expires (ticks) - */
						/* - frags    : all fragments are freed off */
						/* - arpwaits : a new arp request is sent */
						/* - trans... : the buffer is re-sent */
						/* - recei... : a lone ack is sent */
             uLong tcprawlen;			/* length of usable tcp data */
             uByte *tcprawpnt;			/* pointer to usable tcp data within cxb */
             uLong tcprawseq;			/* sequence number of byte that tcprawpnt points to */
             void *rcvdrv;			/* if receive type, pointer to ethernet driver's internal buffer */
             void *xmtdrv;			/* if transmit type, pointer to ethernet driver's internal buffer */
             void (*donentry) (void *donparam, uLong status);
						/* NULL: do nothing when transmit completes */
						/* else: call routine when transmit completes with status */
             void *donparam;			/* param to pass to xmtdonentry routine */
             int tcphasfin;			/* 0: tcp message had TCP_FLAGS_FIN clear */
						/* 1: tcp message had TCP_FLAGS_FIN set */
             uByte sendipaddr[IPADDRSIZE];	/* ip address to send to (might be router, not necessarily the one in cxb->dstipaddr) */
           };

/* TCP connection context block */

struct Tcpcon { Tcpcon *next;			/* next in tcpcons list */
                Chnex *rcvchnex;		/* receive data channel (or NULL if none - it is being closed down) */
                uLong windowsize;		/* window size from connect or listen request */
                uByte *windowbuff;		/* window buffer from connect or listen request */
                int window99s;			/* window will be filled with 0x99s */
                OZ_Seclock *windowlock;		/* keeps windowbuff locked en memorie */
                OZ_Process *windowprocess;	/* process the thread was mapped to at the time */
                uLong rcvwindowrem;		/* offset in windowbuff to remove data */
						/* - ie, offset of start of valid contiguous data */
						/* - data at or after this offset (up to rcvwindowins) are */
						/*   protected from being overwritten by new incoming data */
						/* - range: 0..windowsize-1 */
                uLong rcvwindownxt;		/* offset in windowbuff for next receive */
						/* - range: rcvwindowrem..rcvwindowins */
                uLong rcvwindowins;		/* offset in windowbuff to insert data */
						/* - data byte at this offset has sequence number seq_receivedok */
						/* - data at or after this offset can be overwritten by incoming data */
						/* - range: rcvwindowrem..rcvwindowrem+windowsize */
                uLong seq_lastacksent;		/* last 'ack' sequence number transmitted */
						/* - not valid until rcvdsyn is set */
                uLong seq_lastacksent2;		/* - and the one sent just beofre that */
                uLong seq_receivedok;		/* seq of the next contiguous byte to be received */
						/* - ie, seq number of byte pointed to by rcvwindowins */
						/* - not valid until rcvdsyn is set */
                uLong seq_lastrcvdack;		/* last received ack sequence number */
                uLong seq_lastrcvdwsize;	/* seq number that remote end is allowing us to send up to */
                uLong seq_nextusersendata;	/* seq number to assign to next user data queued */
                uLong smoothedroundtrip;	/* smoothed round trip time (ticks * 8) */
                uLong roundtripdeviation;	/* round trip deviations (ticks * 8) */
                uLong retransmitinterval;	/* resultant retransmit interval (ticks) */
                uLong lasttimemsgsent;		/* last time a message was sent */
                uLong lasttimemsgrcvd;		/* last time a message was received */
                uLong lasttimenzwsizercvd;	/* last time msg rcvd with non-zero wsize */
                uLong slowstarthresh;		/* slow-start threshold */
						/* - when we have transmitted more than this amount, do congestion avoidance */
						/* - until then, do slow-start processing */
						/* - it initially contains 65535 */
						/* - when congestion occurs (timeout or duplicate acks), */
						/*   it gets set to congestionwindow / 2 */
						/* - results in exponential increase to half the congestion point */
						/*   then linear increase from that point on */
                uLong congestionwindow;		/* congestion window */
						/* - we only have at most this number of unacked bytes on network */
						/* - slow start algorithm: */
						/* - - it initially contains one segment worth (1460 bytes) */
						/* - - it increments by one segment everytime other end acks a buf */
						/* - - it resets to one segment if we get a retransmit timeout */
						/* - - results in an exponential increase in rate until an error happens */
						/* - congestion avoidance algorithm: */
						/* - - it increments by 1/cwnd + ssize/8 everytime other end acks a buf */
						/* - - results in an arithmetic increase in rate until an error happens */
                int dupacksinarow;		/* number of duplicate acks in a row */
                Buf *rcvdoutofordr;		/* queue of buffers received out of sequence */
                uByte lipaddr[IPADDRSIZE];	/* local (this) host's ip address */
                uByte ripaddr[IPADDRSIZE];	/* remote host's ip address */
                Sock lsockno;			/* local (this) host's socket number */
                Sock rsockno;			/* remote host's socket number */
                uLong timeout;			/* when to timeout connection attempt (ticks) */
                uLong lastwsizesent;		/* last window size sent */
                uLong maxsendsize;		/* max segment size to send */
                uLong nextxmtseq;		/* sequence of next byte in nextackreq list that needs to be transmitted */
                Iopex *nextackreq;		/* next request that has data that needs to be acked */
                Iopex **lastxmtreq;		/* last request in nextackreq chain */
                uLong numxmtreqs;		/* number of requests on nextackreq list (debug only) */
                uLong expressreceive;		/* number of packets processed by express routine */
                uLong normalreceive;		/* number of packets processed by normal routine */
                uLong sendpend;			/* number of ip packets whose transmission is in progress */
                Iopex *lisiopex;		/* listening I/O request */
                Iopex *tcprcv_iopqh;		/* list of pending receive requests */
                Iopex **tcprcv_iopqt;
                Msubp msubp;			/* used to wait for user buffers */
                uLong reset;			/*    0 : hasn't been reset */
						/* else : reset error status */
                char sentsyn;			/* 0 : haven't sent TCP_FLAGS_SYN yet */
						/* 1 : have sent TCP_FLAGS_SYN */
                char rcvdsyn;			/* 0 : haven't received TCP_FLAGS_SYN yet */
						/* 1 : have received TCP_FLAGS_SYN */
                char sentfin;			/* 0 : haven't sent TCP_FLAGS_FIN */
						/* 1 : have sent TCP_FLAGS_FIN */
                char rcvdfin;			/* 0 : haven't rcvd TCP_FLAGS_FIN */
						/* 1 : have rcvd TCP_FLAGS_FIN */
                char tcpclosed;			/* 0 : be_tcpclose hasn't been called yet */
						/* 1 : be_tcpclose has been called */
                char sentrst;			/* set when tcpterminate has sent a TCP_FLAGS_RST message */
              };

/* Put on abortq to abort the I/O on a channel */

struct Abort { Abort *next;				/* next in abortq */
               int deassign;				/* 0: ip_abort call, 1: ip_deassign call */
               OZ_Iochan *iochan;			/* iochan being aborted */
               OZ_Procmode procmode;			/* procmode of the abort */
               OZ_Ioop *ioop;				/* ioop being aborted */
               Chnex *chnex;				/* chnex being aborted */
             };

/* I/O driver data definitions */

struct Chnex { OZ_Iochan *iochan;			/* corresponding I/O channel */

               Chnex *ipbind_next;			/* next in 'ipbinds' list */
               uByte  ipbind_lclipaddr[IPADDRSIZE];	/* local IP address */
               uByte  ipbind_remipaddr[IPADDRSIZE];	/* remote IP address */
               Iopex *ipbind_iopqh, **ipbind_iopqt;	/* list of pending receive requests */
               uLong  ip_transcount;			/* number of transmits queued */
               uLong  ip_recvcount;			/* number of receives queued */
               OZ_Threadid ip_threadid;			/* thread id of last io */
               uByte  ipbind_proto;			/* ip protocol number */
               char   ipbind_passiton;			/* 1: pass IP packet on for normal processing */
               char   pad1[2];

               Chnex *udpbind_next;			/* next in 'udpbinds' list */
               uByte  udpbind_lclipaddr[IPADDRSIZE];	/* local IP address */
               uByte  udpbind_remipaddr[IPADDRSIZE];	/* remote IP address */
               uByte  udpbind_lclportno[PORTSIZE];	/* local port number */
               uByte  udpbind_remportno[PORTSIZE];	/* remote port number */
               Iopex *udpbind_iopqh, **udpbind_iopqt;	/* list of pending receive requests */
               int    udpbind_ephem;			/* ephemeral socket number */
               uLong  udp_transcount;			/* number of transmits queued */
               uLong  udp_recvcount;			/* number of receives queued */
               OZ_Threadid udp_threadid;		/* thread id of last io */

               Tcpcon *tcpcon;				/* tcp connection context */
               uLong  tcp_transcount;			/* number of transmits queued */
               uLong  tcp_recvcount;			/* number of receives queued */
               OZ_Threadid tcp_threadid;		/* thread id of last io */
             };

struct Iopex { Iopex   *next;				/* general purpose 'next iopex' pointer */
               OZ_Ioop *ioop;				/* corresponding ioop pointer */
               OZ_Procmode procmode;
               Chnex   *chnex;				/* corresponding chnex pointer */
               uLong  (*be) (Iopex *iopex);		/* back-end processing routine */
               Msubp    msubp;				/* used when we get hung waiting for a user transmit buffer */
               union { struct { OZ_IO_ip_hwadd       p; char devname[OZ_DEVUNIT_NAMESIZE];     } hwadd;
                       struct { OZ_IO_ip_hwrem       p; char devname[OZ_DEVUNIT_NAMESIZE];     } hwrem;
                       struct { OZ_IO_ip_hwipamadd   p; char devname[OZ_DEVUNIT_NAMESIZE]; uByte hwipaddr[IPADDRSIZE]; uByte nwipaddr[IPADDRSIZE]; uByte nwipmask[IPADDRSIZE]; } hwipamadd;
                       struct { OZ_IO_ip_hwipamrem   p; char devname[OZ_DEVUNIT_NAMESIZE]; uByte hwipaddr[IPADDRSIZE]; uByte nwipaddr[IPADDRSIZE]; } hwipamrem;
                       struct { OZ_IO_ip_hwlist      p;                                        } hwlist;
                       struct { OZ_IO_ip_hwipamlist  p; char devname[OZ_DEVUNIT_NAMESIZE];     } hwipamlist;
                       struct { OZ_IO_ip_arpadd      p; char devname[OZ_DEVUNIT_NAMESIZE]; uByte ipaddr[IPADDRSIZE]; uByte enaddr[OZ_IO_ETHER_MAXADDR]; } arpadd;
                       struct { OZ_IO_ip_arplist     p; char devname[OZ_DEVUNIT_NAMESIZE];     } arplist;
                       struct { OZ_IO_ip_routeadd    p; uByte gwipaddr[IPADDRSIZE]; uByte nwipaddr[IPADDRSIZE]; uByte nwipmask[IPADDRSIZE];    } routeadd;
                       struct { OZ_IO_ip_routerem    p; uByte gwipaddr[IPADDRSIZE]; uByte nwipaddr[IPADDRSIZE]; uByte nwipmask[IPADDRSIZE];    } routerem;
                       struct { OZ_IO_ip_routelist   p;                                        } routelist;
                       struct { OZ_IO_ip_filteradd   p;                                        } filteradd;
                       struct { OZ_IO_ip_filterrem   p;                                        } filterrem;
                       struct { OZ_IO_ip_filterlist  p;                                        } filterlist;
                       struct { OZ_IO_ip_ipbind      p; uByte lclipaddr[IPADDRSIZE]; uByte remipaddr[IPADDRSIZE];                              } ipbind;
                       struct { OZ_IO_ip_iptransmit  p;                                        } iptransmit;
                       struct { OZ_IO_ip_ipreceive   p; Buf *buf;                              } ipreceive;
                       struct { OZ_IO_ip_ipgetinfo1  p; uLong as; void *ap;                    } ipgetinfo1;
                       struct { OZ_IO_ip_udpbind     p; uByte lclipaddr[IPADDRSIZE]; uByte lclportno[PORTSIZE]; uByte remipaddr[IPADDRSIZE]; uByte remportno[PORTSIZE]; } udpbind;
                       struct { OZ_IO_ip_udptransmit p; uByte myipaddr[IPADDRSIZE];  uWord ident; } udptransmit;
                       struct { OZ_IO_ip_udpreceive  p; Buf *buf;                              } udpreceive;
                       struct { OZ_IO_ip_udpgetinfo1 p; uLong as; void *ap;                    } udpgetinfo1;
                       struct { OZ_IO_ip_tcpconnect  p;                                        } tcpconnect;
                       struct { OZ_IO_ip_tcplisten   p; Iopex **prev; Tcpcon *tcpcon;          } tcplisten;
                       struct { OZ_IO_ip_tcpreceive  p;                                        } tcpreceive;
                       struct { OZ_IO_ip_tcptransmit p; uLong startseq; uLong sendstarted; uLong lasttimesent; uLong retransmits; uWord syn; } tcptransmit;
                       struct { OZ_IO_ip_tcpclose    p;                                        } tcpclose;
                       struct { OZ_IO_ip_tcpgetinfo1 p; uLong as; void *ap;                    } tcpgetinfo1;
                       struct { OZ_IO_ip_tcpwindow   p;                                        } tcpwindow;
                       struct { OZ_IO_ip_dnslookup   p; uLong numel; uByte *array; char *name; } dnslookup;
                     } u;				/* function dependent data */
             };

static uLong ip_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode);
static int ip_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv);
static void ip_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode);
static uLong ip_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                       OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);

static const OZ_Devfunc functable = { 0, sizeof (Chnex), sizeof (Iopex), 0, NULL, NULL, NULL, ip_assign, ip_deassign, ip_abort, ip_start, NULL };

static int initialized = 0;
static OZ_Devclass  *devclass;
static OZ_Devdriver *devdriver;
static OZ_Devunit   *devunit;

/* Static data */

static const Byte hextab[16] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F' };
static const OZ_Datebin tickdelta = OZ_TIMER_RESOLUTION / TPS; /* delta time for a clock tick */
static const uByte nullipaddr[IPADDRSIZE] = { -1,-1,-1,-1 }; /* null ip address */

static uByte arpenaddr[OZ_IO_ETHER_MAXADDR] = { -1,-1,-1,-1,-1,-1 }; /* arp ethernet broadcast address */

uLong oz_dev_ip_debug = 0;			/* debug enable flags */
OZ_Log *oz_dev_ip_log = NULL;

static Filter   *input_filters = NULL;
static Filter *forward_filters = NULL;
static Filter  *output_filters = NULL;

static Abort  *abortqh = NULL;			/* list of pending abort requests */
static Abort **abortqt = &abortqh;
static Buf      *frags = NULL;			/* list of incomplete incoming fragmented ip buffers */
static Buf      *rcvqh = NULL;			/* list of buffers that have been received */
static Buf     **rcvqt = &rcvqh;
static Buf      *xmtqh = NULL;			/* list of buffers that have been transmitted */
static Buf     **xmtqt = &xmtqh;
static Chnex  *ipbinds = NULL;			/* I/O channels that have current ipbinds on them */
static Chnex *udpbinds = NULL;			/* I/O channels that have current udpbinds on them */
static Cxb      *wscxb = NULL;			/* just used for TCP_MAXSENDSIZE */
static Hw         *hws = NULL;			/* hardware interfaces */
static int  scanmsubps = 0;			/* set to scan hw's for requests waiting for a free transmit buffer */
static int tcp_ephemnext = EPHEMMIN;		/* next ephemeral socket to use */
static int udp_ephemnext = EPHEMMIN;		/* next ephemeral socket to use */
static Iopex    *iopqh = NULL;			/* list of pending i/o requests */
static Iopex   **iopqt = &iopqh;
static OZ_Datebin tickdatebin;			/* time of next tick */
static OZ_Event *shuttingdownflag = NULL;	/* set when shutting down */
static OZ_Thread *ipthread = NULL;		/* kernel thread to process ip requests */
static OZ_Event   *ipevent = NULL;		/* event flag to wait for ip requests */
static OZ_Smplock smplock_vl;			/* lock for the queues */
static OZ_Timer *ticktimer = NULL;		/* tick timer struct pointer */
static Route *routes = NULL;			/* router entries */
static Tcpcon  *tcpcons = NULL;			/* list of active tcp connections */
static int     ntcpcons = 0;			/* number of tcpcons on tcpcons list (debugging only) */
static Iopex  *tcplisqh = NULL;			/* ports that tcp listening is enabled on */
static Iopex **tcplisqt = &tcplisqh;
static uByte udp_ephemsocks[EPHEMMAX-EPHEMMIN];	/* ephemeral sockets */
static uLong tcp_ephemsocks[EPHEMMAX-EPHEMMIN];	/* ephemeral sockets */
static uLong ticks = 0;				/* number clock ticks since startup (used by timeout fields) */
static uWord ident = 0;				/* ident number for ip header when sending datagrams */

/* Internal subroutine prototypes */

static uLong kthentry (void *dummy);
static void timerast (void *dummy, OZ_Timer *timer);
static void shuttingdown (void *dummy);
static uLong be_hwadd (Iopex *iopex);
static uLong be_hwrem (Iopex *iopex);
static uLong be_hwipamadd (Iopex *iopex);
static uLong be_hwipamrem (Iopex *iopex);
static uLong be_hwlist (Iopex *iopex);
static uLong be_hwipamlist (Iopex *iopex);
static int hwipamrem (Hw *hw, const uByte nwipaddr[IPADDRSIZE], const uByte hwipaddr[IPADDRSIZE]);
static void hwterminate (Hw *hw);
static void hwchanged (void);
static uLong be_arpadd (Iopex *iopex);
static uLong be_arplist (Iopex *iopex);
static uLong be_routeadd (Iopex *iopex);
static uLong be_routerem (Iopex *iopex);
static uLong be_routelist (Iopex *iopex);
static uLong be_filteradd (Iopex *iopex);
static uLong be_filterrem (Iopex *iopex);
static uLong be_filterlist (Iopex *iopex);
static uLong be_ipbind (Iopex *iopex);
static uLong be_iptransmit (Iopex *iopex);
static void iptransmit_mem (Buf *buf, void *iopexv);
static void iptransmit_done (void *iopexv, uLong status);
static uLong be_ipreceive (Iopex *iopex);
static uLong be_ipgetinfo1 (Iopex *iopex);
static uLong be_udpbind (Iopex *iopex);
static uLong be_udptransmit (Iopex *iopex);
static void udptransmit_mem (Buf *buf, void *iopexv);
static void udptransmit_done (void *iopexv, uLong status);
static uLong be_udpreceive (Iopex *iopex);
static uLong be_udpgetinfo1 (Iopex *iopex);
static uLong be_tcpconnect (Iopex *iopex);
static uLong be_tcpclose (Iopex *iopex);
static uLong be_tcplisten (Iopex *iopex);
static uLong be_tcptransmit (Iopex *iopex);
static uLong be_tcpreceive (Iopex *iopex);
static void tcpreceive_test (Tcpcon *tcpcon);
static uLong be_tcpgetinfo1 (Iopex *iopex);
static uLong be_tcpwindow (Iopex *iopex);
static Sock alloctcpephem (uLong *seq_r);
static void abortproc (Abort *abort);
static void timerproc (void);
static void startread (Buf *buf);
static void readast (void *bufv, uLong status);
static void arpreadproc (Buf *buf);
static void ipreadproc (Buf *buf);
static uWord proc_ether (Buf *buf);
static Buf *proc_arp (Buf *buf);
static Buf *proc_ip (Buf *buf);
static Buf *proc_icmp (Buf *buf);
static void printbounce (Ippkt *ip);
static void proc_destunr (Icmppkt *icmppkt);
static Buf *proc_udp (Buf *buf);
static Buf *proc_tcp (Buf *buf);
static void checkwin99s (uByte *buf, uLong siz, Tcpcon *tcpcon);
static uLong tcpbegseq (Ippkt *ippkt);
static uLong tcpendseq (Ippkt *ippkt);
static uWord tcprawlen (Ippkt *ippkt);
static void tcpreplyack (Tcpcon *tcpcon, int outofordr);
static int checktcpdead (Tcpcon *tcpcon);
static void queuetcptransmit (Iopex *iopex, Tcpcon *tcpcon);
static int starttcpsend (Buf *buf, Tcpcon *tcpcon);
static void starttcpsendagain (Buf *buf, void *tcpconv);
static void validtcpxmtq (Tcpcon *tcpcon);
static void sendtcp (Buf *buf, Tcpcon *tcpcon, uWord rawlen, uLong flags, uLong seq_receivedok);
static void sendtcpdone (void *tcpconv, uLong status);
static uLong tcpwindowsize (Tcpcon *tcpcon, uLong seq_receivedok);
static void sendicmpbounce (Buf *buf, uByte icmptype, uByte icmpcode, uLong extra);
static uLong sendip (Buf *buf, void (*donentry) (void *donparam, uLong status), void *donparam);
static void sendippkt (Buf *buf, void (*donentry) (void *donparam, uLong status), void *donparam);
static void sendarpreq (Hw *hw, const uByte hwipaddr[IPADDRSIZE], const uByte tipaddr[IPADDRSIZE]);
static Hw *find_hw_by_name (const char *devname);
static Hw *find_send_hw (const uByte *ipaddr, uByte *myipaddr, uByte *gwipaddr, int inclroutes);
static int ismyipaddr (const uByte ipaddr[IPADDRSIZE]);
static int isnwbcipad (const uByte ipaddr[IPADDRSIZE], Hw *hw);
static void sendpkt (Buf *buf, uWord totalen, const uByte enaddr[OZ_IO_ETHER_MAXADDR]);
static void writeast (void *bufv, uLong status);
static void sendcomplete (Buf *buf);
static Eproto geteproto (Buf *buf);
static void seteproto (Eproto eproto, Buf *buf);
static void addtoarpc (Hw *hw, const uByte enaddr[OZ_IO_ETHER_MAXADDR], const uByte ipaddr[IPADDRSIZE], uLong timeout);
static int filter_check (Filter **filters, Ippkt *ippkt);
static void tcpsum (char rt, Ippkt *ip);
static void printippkt (char *indents, Ippkt *ip);
static void printtcpcon (Tcpcon *tcpcon);
static void printtcpflags (uWord flags);
static void printenaddr (Hw *hw, const uByte enaddr[OZ_IO_ETHER_MAXADDR]);
static void printipaddr (const uByte ipaddr[IPADDRSIZE]);
static Arpc *mallocarpc (void);
static void freearpc (Arpc *arpc);
static uLong calloctcpcon (Iopex *iopex, uLong windowsize, uByte *windowbuff, Tcpcon **tcpcon_r);
static void tcpterminate (Tcpcon *tcpcon);
static Buf *mallocsndubuf (Hw *hw, void (*entry) (Buf *buf, void *param), void *param, Msubp *msubp);
static Buf *mallocsndkbuf (Hw *hw);
static void freecxb (Buf *oldbuf);
static void freebuf (Buf *buf);
static void valsndubufs (Hw *hw, int line);
static int debugfailrate (void);
static void validatearpcs (Hw *hw, int line);

/************************************************************************/
/*									*/
/*  Initialization routine						*/
/*									*/
/*    Input:								*/
/*									*/
/*	debugmsk  = debug flags mask					*/
/*	etherdevs = comma separaeted ethernet device name list		*/
/*	ipaddr    = ip addresses corresponding to etherdevs		*/
/*	ipid      = my process id					*/
/*	rmod      = access mode						*/
/*									*/
/************************************************************************/

void oz_dev_ip_init (void)

{
  int i;
  uLong sts;

  if (initialized) return;

  oz_knl_printk ("oz_dev_ip_init\n");
  memset (arpenaddr, -1, sizeof arpenaddr);

  /* Set up device driver definitions - we only have one devunit, "ip" */

  if (devunit == NULL) {
    oz_hw_smplock_init (sizeof smplock_vl, &smplock_vl, OZ_SMPLOCK_LEVEL_VL);
    devclass  = oz_knl_devclass_create (OZ_IO_IP_CLASSNAME, OZ_IO_IP_BASE, OZ_IO_IP_MASK, "oz_dev_ip");
    devdriver = oz_knl_devdriver_create (devclass, "oz_dev_ip");
    devunit   = oz_knl_devunit_create (devdriver, OZ_IO_IP_DEV, "ip access device", &functable, 0, oz_s_secattr_tempdev);
  }

  /* Start thread, then init dns if successful */

  sts = oz_knl_event_create (15, "oz_dev_ip event", NULL, &ipevent);
  if (sts != OZ_SUCCESS) oz_knl_printk ("oz_dev_ip_init: error %u creatting event flag\n", sts);
  else {
    sts = oz_knl_thread_create (oz_s_systemproc, oz_knl_user_getmaxbasepri (NULL), NULL, NULL, NULL, 0, 
                                kthentry, NULL, OZ_ASTMODE_INHIBIT, 16, "oz_dev_ip thread", NULL, &ipthread);
    if (sts != OZ_SUCCESS) oz_knl_printk ("oz_dev_ip_init: error %u starting kernel thread\n", sts);
    else {
      initialized = 1;
      oz_dev_ipdns_init (sizeof (Iopex));
      oz_knl_thread_orphan (ipthread);
    }
  }
}

/************************************************************************/
/*									*/
/*  An new channel was just assigned to the 'ip' device			*/
/*  Clear out the chnex struct						*/
/*									*/
/************************************************************************/

static uLong ip_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode)

{
  Chnex *chnex;

  chnex = chnexv;

  memset (chnex, 0, sizeof *chnex);

  OZ_KNL_CHKOBJTYPE (iochan, OZ_OBJTYPE_IOCHAN);
  chnex -> iochan = iochan;

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  An old channel is being deassigned from the 'ip' device		*/
/*  Make sure everything is closed on the channel			*/
/*									*/
/*    Input:								*/
/*									*/
/*	devunit,devexv = device being deassigned from ("ip")		*/
/*	iochan,chnexv  = channel being deassigned			*/
/*	smplevel = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	ip_deassign = 0 : deassign complete, channel all cleared out	*/
/*	              1 : come back later when ref count goes 0 again	*/
/*									*/
/************************************************************************/

static int ip_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv)

{
  Abort *abort;
  Chnex *chnex;
  uLong vl;

  chnex = chnexv;

  /* If anything going on in channel, queue an abort for it                                         */
  /* Theoretically, nothing new can queue (as there are supposedly no references out on the iochan) */

  if ((chnex -> ipbind_iopqt  != NULL) 				/* make sure not on ipbinds list */
   || (chnex -> udpbind_iopqt != NULL) 				/* make sure not on udpbinds list */
   || (chnex -> tcpcon        != NULL)) {			/* make sure not connected or connecting */
    abort = OZ_KNL_NPPMALLOC (sizeof *abort);			/* something happening, queue it for abort */
    abort -> next     = NULL;
    abort -> deassign = 1;
    abort -> iochan   = iochan;
    abort -> procmode = OZ_PROCMODE_KNL;
    abort -> ioop     = NULL;
    abort -> chnex    = chnex;
    oz_knl_iochan_increfc (iochan, 1);				/* inc ref count, it gets decremented at end of abortproc */
    vl = oz_hw_smplock_wait (&smplock_vl);
    *abortqt = abort;
    abortqt  = &(abort -> next);
    oz_hw_smplock_clr (&smplock_vl, vl);			/* release iopq and abortq */
    oz_knl_event_set (ipevent, 1);				/* wake kernel thread to abort whatever it has to abort */
    return (1);							/* come back when abortproc decs the iochan's ref count */
  }

  return (0);							/* nothing going on, ok to delete channel block */
}

/************************************************************************/
/*									*/
/*  Abort all applicable I/O on a given channel				*/
/*									*/
/*    Input:								*/
/*									*/
/*	devunit,devexv = device the I/O is being aborted on		*/
/*	iochan,chnexv  = channel the I/O is being aborted on		*/
/*	ioop = NULL : abort all those I/O's				*/
/*	       else : abort only this I/O				*/
/*	procmode = processor mode the I/O is being aborted for		*/
/*	smplevel = softint						*/
/*									*/
/************************************************************************/

static void ip_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode)

{
  Abort *abort;
  int aborted;
  Iopex *iopex, *iopexs, **liopex;
  uLong vl;

  iopexs = NULL;						/* haven't found anything to abort yet */

  /* Remove everything from the iopq (I/O's waiting to be processed by the thread be_... routine) */

  oz_knl_iochan_increfc (iochan, 1);				/* inc ref count (gets dec'd at end of abortproc) */
  vl = oz_hw_smplock_wait (&smplock_vl);			/* lock iopq and abortq */
  for (liopex = &iopqh; (iopex = *liopex) != NULL;) {		/* scan it */
    if (!oz_knl_ioabortok (iopex -> ioop, iochan, procmode, ioop)) liopex = &(iopex -> next);
    else {
      *liopex = iopex -> next;					/* something abortable found, unlink it */
      iopex -> next = iopexs;					/* link it to list of stuff to abort */
      iopexs = iopex;
    }
  }
  iopqt = liopex;						/* fix end-of-queue pointer */

  /* Put channel on abortq so kernel thread will abort anything applicable */

  abort = OZ_KNL_NPPMALLOC (sizeof *abort);
  abort -> next     = NULL;
  abort -> deassign = 0;
  abort -> iochan   = iochan;
  abort -> procmode = procmode;
  abort -> ioop     = ioop;
  abort -> chnex    = chnexv;
  *abortqt = abort;
  abortqt  = &(abort -> next);
  oz_hw_smplock_clr (&smplock_vl, vl);				/* release iopq and abortq */
  oz_knl_event_set (ipevent, 1);				/* ... then wake kernel thread to abort */

  /* Abort all the iopexs' that were found */

  while ((iopex = iopexs) != NULL) {
    iopexs = iopex -> next;
    oz_knl_iodone (iopex -> ioop, OZ_ABORTED, NULL, NULL, NULL);
  }

  /* Find all DNS requests and kill them, too */

  oz_dev_ipdns_abort (iochan, ioop, procmode);
}

/************************************************************************/
/*									*/
/*  An new I/O request is being started					*/
/*									*/
/*    Input:								*/
/*									*/
/*	devunit,devexv = device the request is starting on ("ip")	*/
/*	iochan,chnexv  = channel the request is starting on		*/
/*	procmode       = processor mode the requestor is in		*/
/*	ioop,iopexv    = i/o op struct					*/
/*	funcode        = function code					*/
/*	as,ap          = argument size and pointer			*/
/*	smplevel       = softint					*/
/*									*/
/*    Output:								*/
/*									*/
/*	ip_start = OZ_STARTED : operation will complete via oz_knl_iodone
/*	                 else : completion status			*/
/*									*/
/************************************************************************/

static uLong ip_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                       OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  Chnex *chnex;
  Iopex *iopex;
  uLong sts, vl;

  chnex = chnexv;
  iopex = iopexv;

  iopex -> ioop       = ioop;
  iopex -> chnex      = chnex;
  iopex -> be         = NULL;
  iopex -> msubp.prev = NULL;
  iopex -> procmode   = procmode;

  OZ_KNL_CHKOBJTYPE (chnex -> iochan, OZ_OBJTYPE_IOCHAN);

  switch (funcode) {

    /* Add hardware interface */

    case OZ_IO_IP_HWADD: {
      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      movc4 (as, ap, sizeof iopex -> u.hwadd.p, &(iopex -> u.hwadd.p));
      sts = oz_knl_section_ugetz (procmode, sizeof iopex -> u.hwadd.devname, iopex -> u.hwadd.p.devname, iopex -> u.hwadd.devname, NULL);
      if (sts != OZ_SUCCESS) return (sts);
      iopex -> be = be_hwadd;
      break;
    }

    /* Remove hardware interface */

    case OZ_IO_IP_HWREM: {
      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      movc4 (as, ap, sizeof iopex -> u.hwrem.p, &(iopex -> u.hwrem.p));
      sts = oz_knl_section_ugetz (procmode, sizeof iopex -> u.hwrem.devname, iopex -> u.hwrem.p.devname, iopex -> u.hwrem.devname, NULL);
      if (sts != OZ_SUCCESS) return (sts);
      iopex -> be = be_hwrem;
      break;
    }

    /* Add hardware interface ip address and mask */

    case OZ_IO_IP_HWIPAMADD: {
      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      movc4 (as, ap, sizeof iopex -> u.hwipamadd.p, &(iopex -> u.hwipamadd.p));
      if (iopex -> u.hwipamadd.p.addrsize != IPADDRSIZE) return (OZ_BADPARAM);
      sts = oz_knl_section_ugetz (procmode, sizeof iopex -> u.hwipamadd.devname, iopex -> u.hwipamadd.p.devname, iopex -> u.hwipamadd.devname, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_section_uget (procmode, IPADDRSIZE, iopex -> u.hwipamadd.p.hwipaddr, iopex -> u.hwipamadd.hwipaddr);
      if (sts == OZ_SUCCESS) sts = oz_knl_section_uget (procmode, IPADDRSIZE, iopex -> u.hwipamadd.p.nwipaddr, iopex -> u.hwipamadd.nwipaddr);
      if (sts == OZ_SUCCESS) sts = oz_knl_section_uget (procmode, IPADDRSIZE, iopex -> u.hwipamadd.p.nwipmask, iopex -> u.hwipamadd.nwipmask);
      if (sts != OZ_SUCCESS) return (sts);
      iopex -> be = be_hwipamadd;
      break;
    }

    /* Remove hardware interface ip address and mask */

    case OZ_IO_IP_HWIPAMREM: {
      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      movc4 (as, ap, sizeof iopex -> u.hwipamrem.p, &(iopex -> u.hwipamrem.p));
      if (iopex -> u.hwipamrem.p.addrsize != IPADDRSIZE) return (OZ_BADPARAM);
      sts = oz_knl_section_ugetz (procmode, sizeof iopex -> u.hwipamrem.devname, iopex -> u.hwipamrem.p.devname, iopex -> u.hwipamrem.devname, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_section_uget (procmode, IPADDRSIZE, iopex -> u.hwipamrem.p.hwipaddr, iopex -> u.hwipamrem.hwipaddr);
      if (sts == OZ_SUCCESS) sts = oz_knl_section_uget (procmode, IPADDRSIZE, iopex -> u.hwipamrem.p.nwipaddr, iopex -> u.hwipamrem.nwipaddr);
      if (sts != OZ_SUCCESS) return (sts);
      iopex -> be = be_hwipamrem;
      break;
    }

    /* List hardware interfaces */

    case OZ_IO_IP_HWLIST: {
      movc4 (as, ap, sizeof iopex -> u.hwlist.p, &(iopex -> u.hwlist.p));
      if (iopex -> u.hwlist.p.devnamesize == 0) return (OZ_BADPARAM);
      sts = oz_knl_ioop_lockw (ioop, iopex -> u.hwlist.p.devnamesize, iopex -> u.hwlist.p.devnamebuff, NULL, NULL, NULL);
      if (sts != OZ_SUCCESS) return (sts);
      iopex -> be = be_hwlist;
      break;
    }

    /* List hardware interface ip addresses and masks */

    case OZ_IO_IP_HWIPAMLIST: {
      movc4 (as, ap, sizeof iopex -> u.hwipamlist.p, &(iopex -> u.hwipamlist.p));		/* get arg list */
      if (iopex -> u.hwipamlist.p.addrsize != IPADDRSIZE) return (OZ_BADPARAM);			/* check caller's idea of how big an ip address is */
      sts = oz_knl_section_ugetz (procmode, sizeof iopex -> u.hwipamlist.devname, iopex -> u.hwipamlist.p.devname, iopex -> u.hwipamlist.devname, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockw (ioop, IPADDRSIZE, iopex -> u.hwipamlist.p.hwipaddr, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockw (ioop, IPADDRSIZE, iopex -> u.hwipamlist.p.nwipaddr, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockw (ioop, IPADDRSIZE, iopex -> u.hwipamlist.p.nwipmask, NULL, NULL, NULL);
      if (sts != OZ_SUCCESS) return (sts);
      iopex -> be = be_hwipamlist;
      break;
    }

    /* Add entry to arp cache */

    case OZ_IO_IP_ARPADD: {
      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      movc4 (as, ap, sizeof iopex -> u.arpadd.p, &(iopex -> u.arpadd.p));
      if (iopex -> u.arpadd.p.addrsize   != IPADDRSIZE) return (OZ_BADPARAM);
      if (iopex -> u.arpadd.p.enaddrsize > OZ_IO_ETHER_MAXADDR) return (OZ_BADPARAM);
      sts = oz_knl_section_ugetz (procmode, sizeof iopex -> u.arpadd.devname, iopex -> u.arpadd.p.devname, iopex -> u.arpadd.devname, NULL); /* get the device name string */
      if (sts == OZ_SUCCESS) sts = oz_knl_section_uget (procmode, IPADDRSIZE, iopex -> u.arpadd.p.ipaddr, iopex -> u.arpadd.ipaddr); /* get the ip address */
      if (sts == OZ_SUCCESS) sts = oz_knl_section_uget (procmode, iopex -> u.arpadd.p.enaddrsize, iopex -> u.arpadd.p.enaddr, iopex -> u.arpadd.enaddr); /* get the ethernet address */
      if (sts != OZ_SUCCESS) return (sts);
      iopex -> be = be_arpadd;
      break;
    }

    /* List hardware interface arp cache entries */

    case OZ_IO_IP_ARPLIST: {
      movc4 (as, ap, sizeof iopex -> u.arplist.p, &(iopex -> u.arplist.p));			/* get arg list */
      if (iopex -> u.arplist.p.addrsize   != IPADDRSIZE) return (OZ_BADPARAM);			/* check caller's idea of how big an ip address is */
      if (iopex -> u.arplist.p.enaddrsize  > OZ_IO_ETHER_MAXADDR) return (OZ_BADPARAM);
      sts = oz_knl_section_ugetz (procmode, sizeof iopex -> u.arplist.devname, iopex -> u.arplist.p.devname, iopex -> u.arplist.devname, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockw (ioop, IPADDRSIZE, iopex -> u.arplist.p.ipaddr, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockw (ioop, iopex -> u.arplist.p.enaddrsize, iopex -> u.arplist.p.enaddr, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockw (ioop, sizeof *(iopex -> u.arplist.p.timeout), iopex -> u.arplist.p.timeout, NULL, NULL, NULL);
      if (sts != OZ_SUCCESS) return (sts);
      iopex -> be = be_arplist;
      break;
    }

    /* Remove ARP entry */

    case OZ_IO_IP_ARPREM: {
      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      return (OZ_BADIOFUNC);
    }

    /* Add routing entry */

    case OZ_IO_IP_ROUTEADD: {
      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      movc4 (as, ap, sizeof iopex -> u.routeadd.p, &(iopex -> u.routeadd.p));
      if (iopex -> u.routeadd.p.addrsize != IPADDRSIZE) return (OZ_BADPARAM);
      sts = oz_knl_section_uget (procmode, IPADDRSIZE, iopex -> u.routeadd.p.gwipaddr, iopex -> u.routeadd.gwipaddr);
      if (sts == OZ_SUCCESS) sts = oz_knl_section_uget (procmode, IPADDRSIZE, iopex -> u.routeadd.p.nwipaddr, iopex -> u.routeadd.nwipaddr);
      if (sts == OZ_SUCCESS) sts = oz_knl_section_uget (procmode, IPADDRSIZE, iopex -> u.routeadd.p.nwipmask, iopex -> u.routeadd.nwipmask);
      if (sts != OZ_SUCCESS) return (sts);
      iopex -> be = be_routeadd;
      break;
    }

    /* Remove routing entry */

    case OZ_IO_IP_ROUTEREM: {
      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      movc4 (as, ap, sizeof iopex -> u.routerem.p, &(iopex -> u.routerem.p));
      if (iopex -> u.routerem.p.addrsize != IPADDRSIZE) return (OZ_BADPARAM);
      sts = oz_knl_section_uget (procmode, IPADDRSIZE, iopex -> u.routerem.p.gwipaddr, iopex -> u.routerem.gwipaddr);
      if (sts == OZ_SUCCESS) sts = oz_knl_section_uget (procmode, IPADDRSIZE, iopex -> u.routerem.p.nwipaddr, iopex -> u.routerem.nwipaddr);
      if (sts == OZ_SUCCESS) sts = oz_knl_section_uget (procmode, IPADDRSIZE, iopex -> u.routerem.p.nwipmask, iopex -> u.routerem.nwipmask);
      if (sts != OZ_SUCCESS) return (sts);
      iopex -> be = be_routerem;
      break;
    }

    /* List route table entries */

    case OZ_IO_IP_ROUTELIST: {
      movc4 (as, ap, sizeof iopex -> u.routelist.p, &(iopex -> u.routelist.p));			/* get arg list */
      if (iopex -> u.routelist.p.addrsize != IPADDRSIZE) return (OZ_BADPARAM);			/* check caller's idea of how big an ip address is */
      sts = oz_knl_ioop_lockw (ioop, IPADDRSIZE, iopex -> u.routelist.p.gwipaddr, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockw (ioop, IPADDRSIZE, iopex -> u.routelist.p.nwipaddr, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockw (ioop, IPADDRSIZE, iopex -> u.routelist.p.nwipmask, NULL, NULL, NULL);
      if ((sts == OZ_SUCCESS) && (iopex -> u.routelist.p.devnamesize != 0)) sts = oz_knl_ioop_lockw (ioop, iopex -> u.routelist.p.devnamesize, iopex -> u.routelist.p.devnamebuff, NULL, NULL, NULL);
      if ((sts == OZ_SUCCESS) && (iopex -> u.routelist.p.hwipaddr != NULL)) sts = oz_knl_ioop_lockw (ioop, IPADDRSIZE, iopex -> u.routelist.p.hwipaddr, NULL, NULL, NULL);
      if (sts != OZ_SUCCESS) return (sts);
      iopex -> be = be_routelist;
      break;
    }

    /* Add filter */

    case OZ_IO_IP_FILTERADD: {
      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      movc4 (as, ap, sizeof iopex -> u.filteradd.p, &(iopex -> u.filteradd.p));
      sts = oz_knl_ioop_lockr (ioop, iopex -> u.filteradd.p.size, iopex -> u.filteradd.p.data, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockr (ioop, iopex -> u.filteradd.p.size, iopex -> u.filteradd.p.mask, NULL, NULL, NULL);
      if (sts != OZ_SUCCESS) return (sts);
      iopex -> be = be_filteradd;
      break;
    }

    /* Remove filter */

    case OZ_IO_IP_FILTERREM: {
      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      movc4 (as, ap, sizeof iopex -> u.filterrem.p, &(iopex -> u.filterrem.p));
      iopex -> be = be_filterrem;
      break;
    }

    /* List filter */

    case OZ_IO_IP_FILTERLIST: {
      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      movc4 (as, ap, sizeof iopex -> u.filterlist.p, &(iopex -> u.filterlist.p));
      sts = oz_knl_ioop_lockw (ioop, iopex -> u.filterlist.p.size, iopex -> u.filterlist.p.data, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockw (ioop, iopex -> u.filterlist.p.size, iopex -> u.filterlist.p.mask, NULL, NULL, NULL);
      if (sts != OZ_SUCCESS) return (sts);
      iopex -> be = be_filterlist;
      break;
    }

    /* Bind socket for IP datagram reception */

    case OZ_IO_IP_IPBIND: {
      movc4 (as, ap, sizeof iopex -> u.ipbind.p, &(iopex -> u.ipbind.p));
      if (iopex -> u.ipbind.p.addrsize != IPADDRSIZE) return (OZ_BADPARAM);
      sts = OZ_SUCCESS;
      CLRIPAD (iopex -> u.ipbind.lclipaddr);								/* default to any local ip address */
      CLRIPAD (iopex -> u.ipbind.remipaddr);								/* default to any remote ip address */
      if ((sts == OZ_SUCCESS) && (iopex -> u.ipbind.p.lclipaddr != NULL)) sts = oz_knl_section_uget (procmode, IPADDRSIZE, iopex -> u.ipbind.p.lclipaddr, iopex -> u.ipbind.lclipaddr);
      if ((sts == OZ_SUCCESS) && (iopex -> u.ipbind.p.remipaddr != NULL)) sts = oz_knl_section_uget (procmode, IPADDRSIZE, iopex -> u.ipbind.p.remipaddr, iopex -> u.ipbind.remipaddr);
      if (sts != OZ_SUCCESS) return (sts);
      iopex -> be = be_ipbind;
      break;
    }

    /* Transmit an arbitrary IP packet */

    case OZ_IO_IP_IPTRANSMIT: {
      movc4 (as, ap, sizeof iopex -> u.iptransmit.p, &(iopex -> u.iptransmit.p));
      sts = oz_knl_ioop_lockr (ioop, iopex -> u.iptransmit.p.pktsize, iopex -> u.iptransmit.p.ippkt, NULL, NULL, NULL);
      if (sts != OZ_SUCCESS) return (sts);
      iopex -> be = be_iptransmit;
      break;
    }

    /* Receive an IP packet */

    case OZ_IO_IP_IPRECEIVE: {
      movc4 (as, ap, sizeof iopex -> u.ipreceive.p, &(iopex -> u.ipreceive.p));
      sts = oz_knl_ioop_lockw (ioop, iopex -> u.ipreceive.p.pktsize, iopex -> u.ipreceive.p.ippkt, NULL, NULL, NULL);
      if (sts != OZ_SUCCESS) return (sts);
      iopex -> be = be_ipreceive;
      break;
    }

    /* Get IP info, part 1 */

    case OZ_IO_IP_IPGETINFO1: {
      if (!OZ_HW_WRITABLE (as, ap, procmode)) return (OZ_ACCVIO);
      movc4 (as, ap, sizeof iopex -> u.ipgetinfo1.p, &(iopex -> u.ipgetinfo1.p));
      sts = OZ_SUCCESS;
      if (iopex -> u.ipgetinfo1.p.addrsize != IPADDRSIZE) sts = OZ_BADPARAM;
      if ((sts == OZ_SUCCESS) && (iopex -> u.ipgetinfo1.p.lclipaddr != NULL)) sts = oz_knl_ioop_lockw (ioop, IPADDRSIZE, iopex -> u.ipgetinfo1.p.lclipaddr, NULL, NULL, NULL);
      if ((sts == OZ_SUCCESS) && (iopex -> u.ipgetinfo1.p.remipaddr != NULL)) sts = oz_knl_ioop_lockw (ioop, IPADDRSIZE, iopex -> u.ipgetinfo1.p.remipaddr, NULL, NULL, NULL);
      if (sts != OZ_SUCCESS) return (sts);
      iopex -> u.ipgetinfo1.as = as;
      iopex -> u.ipgetinfo1.ap = ap;
      iopex -> be = be_ipgetinfo1;
      break;
    }

    /* Bind to UDP socket for incoming data */

    case OZ_IO_IP_UDPBIND: {
      movc4 (as, ap, sizeof iopex -> u.udpbind.p, &(iopex -> u.udpbind.p));
      if (iopex -> u.udpbind.p.addrsize != IPADDRSIZE) return (OZ_BADPARAM);
      if (iopex -> u.udpbind.p.portsize != PORTSIZE)   return (OZ_BADPARAM);
      CLRIPAD (iopex -> u.udpbind.lclipaddr);
      CLRPORT (iopex -> u.udpbind.lclportno);
      CLRIPAD (iopex -> u.udpbind.remipaddr);
      CLRPORT (iopex -> u.udpbind.remportno);
      sts = OZ_SUCCESS;
      if ((sts == OZ_SUCCESS) && (iopex -> u.udpbind.p.lclipaddr != NULL)) sts = oz_knl_section_uget (procmode, IPADDRSIZE, iopex -> u.udpbind.p.lclipaddr, iopex -> u.udpbind.lclipaddr);
      if ((sts == OZ_SUCCESS) && (iopex -> u.udpbind.p.lclportno != NULL)) sts = oz_knl_section_uget (procmode, PORTSIZE,   iopex -> u.udpbind.p.lclportno, iopex -> u.udpbind.lclportno);
      if ((sts == OZ_SUCCESS) && (iopex -> u.udpbind.p.remipaddr != NULL)) sts = oz_knl_section_uget (procmode, IPADDRSIZE, iopex -> u.udpbind.p.remipaddr, iopex -> u.udpbind.remipaddr);
      if ((sts == OZ_SUCCESS) && (iopex -> u.udpbind.p.remportno != NULL)) sts = oz_knl_section_uget (procmode, PORTSIZE,   iopex -> u.udpbind.p.remportno, iopex -> u.udpbind.remportno);
      if (sts != OZ_SUCCESS) return (sts);
      iopex -> be = be_udpbind;
      break;
    }

    /* Transmit UDP packet */

    case OZ_IO_IP_UDPTRANSMIT: {
      movc4 (as, ap, sizeof iopex -> u.udptransmit.p, &(iopex -> u.udptransmit.p));
      if (iopex -> u.udptransmit.p.addrsize != IPADDRSIZE) return (OZ_BADPARAM);
      if (iopex -> u.udptransmit.p.portsize != PORTSIZE)   return (OZ_BADPARAM);
      sts = oz_knl_ioop_lockr (ioop, iopex -> u.udptransmit.p.rawsize, iopex -> u.udptransmit.p.rawbuff, NULL, NULL, NULL);
      if (sts != OZ_SUCCESS) return (sts);
      iopex -> be = be_udptransmit;
      break;
    }

    /* Receive UDP packet */

    case OZ_IO_IP_UDPRECEIVE: {
      movc4 (as, ap, sizeof iopex -> u.udpreceive.p, &(iopex -> u.udpreceive.p));
      if (iopex -> u.udpreceive.p.addrsize != IPADDRSIZE) return (OZ_BADPARAM);
      if (iopex -> u.udpreceive.p.portsize != PORTSIZE)   return (OZ_BADPARAM);
      sts = oz_knl_ioop_lockw (ioop, iopex -> u.udpreceive.p.rawsize, iopex -> u.udpreceive.p.rawbuff, NULL, NULL, NULL);
      if ((sts == OZ_SUCCESS) && (iopex -> u.udpreceive.p.rawrlen   != NULL)) sts = oz_knl_ioop_lockw (ioop, sizeof *(iopex -> u.udpreceive.p.rawrlen), iopex -> u.udpreceive.p.rawrlen, NULL, NULL, NULL);
      if ((sts == OZ_SUCCESS) && (iopex -> u.udpreceive.p.srcipaddr != NULL)) sts = oz_knl_ioop_lockw (ioop, IPADDRSIZE, iopex -> u.udpreceive.p.srcipaddr, NULL, NULL, NULL);
      if ((sts == OZ_SUCCESS) && (iopex -> u.udpreceive.p.srcportno != NULL)) sts = oz_knl_ioop_lockw (ioop, PORTSIZE, iopex -> u.udpreceive.p.srcportno, NULL, NULL, NULL);
      if (sts != OZ_SUCCESS) return (sts);
      iopex -> be = be_udpreceive;
      break;
    }

    /* Get UDP info, part 1 */

    case OZ_IO_IP_UDPGETINFO1: {
      if (!OZ_HW_WRITABLE (as, ap, procmode)) return (OZ_ACCVIO);
      movc4 (as, ap, sizeof iopex -> u.udpgetinfo1.p, &(iopex -> u.udpgetinfo1.p));
      sts = OZ_SUCCESS;
      if (iopex -> u.udpgetinfo1.p.addrsize != IPADDRSIZE) sts = OZ_BADPARAM;
      if (iopex -> u.udpgetinfo1.p.portsize != PORTSIZE)   sts = OZ_BADPARAM;
      if ((sts == OZ_SUCCESS) && (iopex -> u.udpgetinfo1.p.lclipaddr != NULL)) sts = oz_knl_ioop_lockw (ioop, IPADDRSIZE, iopex -> u.udpgetinfo1.p.lclipaddr, NULL, NULL, NULL);
      if ((sts == OZ_SUCCESS) && (iopex -> u.udpgetinfo1.p.lclportno != NULL)) sts = oz_knl_ioop_lockw (ioop, PORTSIZE,   iopex -> u.udpgetinfo1.p.lclportno, NULL, NULL, NULL);
      if ((sts == OZ_SUCCESS) && (iopex -> u.udpgetinfo1.p.remipaddr != NULL)) sts = oz_knl_ioop_lockw (ioop, IPADDRSIZE, iopex -> u.udpgetinfo1.p.remipaddr, NULL, NULL, NULL);
      if ((sts == OZ_SUCCESS) && (iopex -> u.udpgetinfo1.p.remportno != NULL)) sts = oz_knl_ioop_lockw (ioop, PORTSIZE,   iopex -> u.udpgetinfo1.p.remportno, NULL, NULL, NULL);
      if (sts != OZ_SUCCESS) return (sts);
      iopex -> u.udpgetinfo1.as = as;
      iopex -> u.udpgetinfo1.ap = ap;
      iopex -> be = be_udpgetinfo1;
      break;
    }

    /* Connect to a remote TCP server */

    case OZ_IO_IP_TCPCONNECT: {
      movc4 (as, ap, sizeof iopex -> u.tcpconnect.p, &(iopex -> u.tcpconnect.p));
      if (iopex -> u.tcpconnect.p.addrsize != IPADDRSIZE) return (OZ_BADPARAM);
      if (iopex -> u.tcpconnect.p.portsize != PORTSIZE)   return (OZ_BADPARAM);
      iopex -> be = be_tcpconnect;
      break;
    }

    /* Close a TCP connection */

    case OZ_IO_IP_TCPCLOSE: {
      movc4 (as, ap, sizeof iopex -> u.tcpclose.p, &(iopex -> u.tcpclose.p));
      iopex -> be = be_tcpclose;
      break;
    }

    /* Listen for an inbound TCP connection */

    case OZ_IO_IP_TCPLISTEN: {
      movc4 (as, ap, sizeof (iopex -> u.tcplisten.p), &(iopex -> u.tcplisten.p));
      if (iopex -> u.tcplisten.p.addrsize != IPADDRSIZE) return (OZ_BADPARAM);
      if (iopex -> u.tcplisten.p.portsize != PORTSIZE)   return (OZ_BADPARAM);
      iopex -> be = be_tcplisten;
      break;
    }

    /* Transmit TCP data */

    case OZ_IO_IP_TCPTRANSMIT: {
      movc4 (as, ap, sizeof iopex -> u.tcptransmit.p, &(iopex -> u.tcptransmit.p));
      sts = oz_knl_ioop_lockr (ioop, iopex -> u.tcptransmit.p.rawsize, iopex -> u.tcptransmit.p.rawbuff, NULL, NULL, NULL);
      if (sts != OZ_SUCCESS) return (sts);
      iopex -> be = be_tcptransmit;
      break;
    }

    /* Receive TCP data */

    case OZ_IO_IP_TCPRECEIVE: {
      movc4 (as, ap, sizeof iopex -> u.tcpreceive.p, &(iopex -> u.tcpreceive.p));
      iopex -> be = be_tcpreceive;
      break;
    }

    /* Get tcp info, part 1 */

    case OZ_IO_IP_TCPGETINFO1: {
      uByte *lclipaddr, *lclportno, *remipaddr, *remportno;

      if (!OZ_HW_WRITABLE (as, ap, procmode)) return (OZ_ACCVIO);
      movc4 (as, ap, sizeof iopex -> u.tcpgetinfo1.p, &(iopex -> u.tcpgetinfo1.p));
      lclipaddr = iopex -> u.tcpgetinfo1.p.lclipaddr;
      lclportno = iopex -> u.tcpgetinfo1.p.lclportno;
      remipaddr = iopex -> u.tcpgetinfo1.p.remipaddr;
      remportno = iopex -> u.tcpgetinfo1.p.remportno;
      sts = OZ_SUCCESS;
      if ((iopex -> u.tcpgetinfo1.p.addrsize != IPADDRSIZE) || (iopex -> u.tcpgetinfo1.p.portsize != PORTSIZE)) sts = OZ_BADPARAM;
      if ((sts == OZ_SUCCESS) && (lclipaddr != NULL)) sts = oz_knl_ioop_lockw (ioop, IPADDRSIZE, lclipaddr, NULL, NULL, NULL);
      if ((sts == OZ_SUCCESS) && (lclportno != NULL)) sts = oz_knl_ioop_lockw (ioop, PORTSIZE,   lclportno, NULL, NULL, NULL);
      if ((sts == OZ_SUCCESS) && (remipaddr != NULL)) sts = oz_knl_ioop_lockw (ioop, IPADDRSIZE, remipaddr, NULL, NULL, NULL);
      if ((sts == OZ_SUCCESS) && (remportno != NULL)) sts = oz_knl_ioop_lockw (ioop, PORTSIZE,   remportno, NULL, NULL, NULL);
      if (sts != OZ_SUCCESS) return (sts);
      memclr (&(iopex -> u.tcpgetinfo1.p), sizeof iopex -> u.tcpgetinfo1.p);
      iopex -> u.tcpgetinfo1.p.addrsize  = IPADDRSIZE;
      iopex -> u.tcpgetinfo1.p.portsize  = PORTSIZE;
      iopex -> u.tcpgetinfo1.p.lclipaddr = lclipaddr;
      iopex -> u.tcpgetinfo1.p.lclportno = lclportno;
      iopex -> u.tcpgetinfo1.p.remipaddr = remipaddr;
      iopex -> u.tcpgetinfo1.p.remportno = remportno;
      iopex -> u.tcpgetinfo1.as = as;
      iopex -> u.tcpgetinfo1.ap = ap;
      iopex -> be = be_tcpgetinfo1;
      break;
    }

    /* Set a new TCP window */

    case OZ_IO_IP_TCPWINDOW: {
      movc4 (as, ap, sizeof (iopex -> u.tcpwindow.p), &(iopex -> u.tcpwindow.p));
      iopex -> be = be_tcpwindow;
      break;
    }

    /* Add DNS server to end of list */

    case OZ_IO_IP_DNSSVRADD: {
      sts = oz_dev_ipdns_dnssvradd (procmode, as, ap, ioop);
      return (sts);
    }

    /* Remove DNS server from list */

    case OZ_IO_IP_DNSSVRREM: {
      sts = oz_dev_ipdns_dnssvrrem (procmode, as, ap, ioop);
      return (sts);
    }

    /* List DNS servers */

    case OZ_IO_IP_DNSSVRLIST: {
      sts = oz_dev_ipdns_dnssvrlist (procmode, as, ap, ioop);
      return (sts);
    }

    /* Perform DNS lookup */

    case OZ_IO_IP_DNSLOOKUP: {
      sts = oz_dev_ipdns_dnslookup (procmode, as, ap, ioop, iopex);
      return (sts);
    }

    /* Who knows what */

    default: {
      return (OZ_BADIOFUNC);
    }
  }

  /* Queue non-DNS request to kernel thread */

  if (debug & DEBUG_THREAD) PRINTF "oz_dev_ip ip_start: queuing iopex %p\n", iopex);
  iopex -> next = NULL;
  vl = oz_hw_smplock_wait (&smplock_vl);
  *iopqt = iopex;
  iopqt  = &(iopex -> next);
  oz_hw_smplock_clr (&smplock_vl, vl);
  if (debug & DEBUG_THREAD) PRINTF "oz_dev_ip ip_start: iopqh %p\n", iopqh);
  oz_knl_event_set (ipevent, 1);

  return (OZ_STARTED);
}

/************************************************************************/
/*									*/
/*  Kernel thread that processes requests in a serial fashion		*/
/*									*/
/************************************************************************/

static uLong kthentry (void *dummy)

{
  Abort *abort;
  Buf *buf;
  Hw *hw;
  int i;
  Iopex *iopex;
  Msubp *msubp;
  OZ_Datebin now;
  OZ_Thread *thread;
  uLong sts, vl;

  oz_hw_cpu_setsoftint (0);

  /* Initialize tables */

  sts = oz_hw_tod_getnow ();
  if (sts == 0) sts ++;
  for (i = EPHEMMIN; i < EPHEMMAX; i ++) {
    udp_ephemsocks[i-EPHEMMIN] = 1;	/* udp just uses a flag bit saying that it is available */
    tcp_ephemsocks[i-EPHEMMIN] = sts;	/* tcp uses a non-zero 'random' number to start their sequences out at */
  }

  /* Start tick ast timer */

  ticktimer   = oz_knl_timer_alloc ();
  tickdatebin = oz_hw_tod_getnow ();
  OZ_HW_DATEBIN_ADD (tickdatebin, tickdatebin, tickdelta);
  oz_knl_timer_insert (ticktimer, tickdatebin, timerast, NULL);

  /* Queue shutdown handler to terminate tcp connections */

  oz_knl_shuthand_create (shuttingdown, NULL);

  /* Process requests forever */

  while (1) {
    oz_knl_process_setcur (oz_s_systemproc);				/* make sure we aren't attached to an old process */
    vl = oz_hw_smplock_wait (&smplock_vl);				/* lock access to lists */

    /* Process abort requests */

    abort = abortqh;							/* see if any abort queued */
    if (abort != NULL) {
      if (debug & DEBUG_THREAD) PRINTF "oz_dev_ip thread: abortqh %p\n", abort);
      abortqh = abort -> next;						/* if so, dequeue it */
      if (abortqh == NULL) abortqt = &abortqh;
      oz_hw_smplock_clr (&smplock_vl, vl);				/* release the lock */
      abortproc (abort);						/* process it */
      continue;
    }

    /* See if there are any requests that are waiting for a transmit buffer    */
    /* This gets serialized through the main loop so things don't nest deeply  */
    /* Otherwise, theoretically, the msubp routine could get called in freebuf */

    if (scanmsubps) {
      scanmsubps = 0;
      for (hw = hws; hw != NULL; hw = hw -> next) {
        valsndubufs (hw, __LINE__);
        if (((msubp = hw -> waitsndubufh) != NULL) && (hw -> freesndubufs != NULL)) {
          buf = mallocsndubuf (hw, NULL, NULL, msubp);			/* ok, unlink buffer and msubp from lists */
          (*(msubp -> entry)) (buf, msubp -> param);			/* ... and call the routine to resume processing */
        }
        valsndubufs (hw, __LINE__);
      }
    }

    /* Process new I/O requests */

    iopex = iopqh;							/* see if any I/O request queued */
    if (iopex != NULL) {
      if (debug & DEBUG_THREAD) PRINTF "oz_dev_ip thread: iopqh %p\n", iopex);
      iopqh = iopex -> next;						/* if so, dequeue it */
      if (iopqh == NULL) iopqt = &iopqh;
      oz_hw_smplock_clr (&smplock_vl, vl);				/* release the lock */
      ACCESSBUFFS (iopex);						/* attach to corresponding process */
      sts = (*(iopex -> be)) (iopex);					/* start processing it */
      if (debug & DEBUG_THREAD) PRINTF "oz_dev_ip thread: iopex %p sts %u\n", iopex, sts);
      if (sts != OZ_STARTED) oz_knl_iodone (iopex -> ioop, sts, NULL, NULL, NULL); /* maybe it finished */
      continue;
    }

    /* Process completed receives - */
    /* Since receives aren't committed to anything at this point, we immediately check for terminating hardware.  Also, we assume */
    /* the ethernet card won't even bother passing us bad packets, it only returns an error if the thing has completely died.     */

    buf = rcvqh;							/* see if any receive has completed */
    if (buf != NULL) {
      if (debug & DEBUG_THREAD) PRINTF "oz_dev_ip thread: rcvqh %p, iosts %u\n", buf, buf -> iosts);
      rcvqh = buf -> next;						/* if so, dequeue it */
      if (rcvqh == NULL) rcvqt = &rcvqh;
      oz_hw_smplock_clr (&smplock_vl, vl);				/* release the lock */
      hw = buf -> hw;
      hw -> receivesinprog --;						/* decrement receives-in-prog for the device */
      if (hw -> terminated) {						/* see if trying to terminate the device */
        freebuf (buf);							/* if so, free the buffer off */
        hwterminate (hw);						/* try to terminate for good now */
      } else if (buf -> iosts != OZ_SUCCESS) {
        oz_knl_printk ("oz_dev_ip: error %u receiving from %s - shutting it down\n", buf -> iosts, hw -> devname);
        hw -> terminated = 1;						/* (set this so next freebuf won't restart read) */
        freebuf (buf);							/* free the buffer, don't restart the read */
        hwterminate (hw);						/* finish shutting the interface down */
      }
      else if (debugfailrate ()) freebuf (buf);				/* maybe force it to fail (ie, ignore it) */
      else if (buf -> type == BUF_TYPE_RCV_ARP) arpreadproc (buf);	/* process incoming arp's */
      else ipreadproc (buf);						/* process incoming ip's */
      continue;
    }

    /* Process completed transmits - */
    /* We call the sendcomplete routine regardless if we're terminating the hardware, because the sendcomplete routine may */
    /* have many interesting things to do (like post completed I/O requests).  We keep the transmits-in-prog count incd so */
    /* the hw struct can't go away.  Then, after sendcomplete returns, we see if we can finish off the hw termination.     */

    buf = xmtqh;							/* see if any transmit has completed */
    if (buf != NULL) {
      if (debug & DEBUG_THREAD) PRINTF "oz_dev_ip thread: xmtqh %p\n", buf);
      xmtqh = buf -> next;						/* if so, dequeue it */
      if (xmtqh == NULL) xmtqt = &xmtqh;
      oz_hw_smplock_clr (&smplock_vl, vl);				/* release the lock */
      hw = buf -> hw;							/* save pointer to hw it was transmitted on */
      sendcomplete (buf);						/* process it */
      if ((-- (hw -> transmitsinprog) == 0) && (hw -> terminated)) hwterminate (hw); /* maybe hw can finish terminating now */
      continue;
    }

    /* Process tick timer */

    oz_hw_smplock_clr (&smplock_vl, vl);				/* release lock */
    now = oz_hw_tod_getnow ();						/* see what time it is now */
    if (now >= tickdatebin) {						/* see if it is at or past timer's due time */
      ticks ++;								/* ok, increment tick counter */
      OZ_HW_DATEBIN_ADD (tickdatebin, now, tickdelta);			/* increment next timer due time */
      timerproc ();							/* process timer stuff */
      oz_knl_timer_insert (ticktimer, tickdatebin, timerast, NULL);	/* wake us next time timer is due */
      continue;
    }

    /* If system is being shut down, terminate all tcp connections */

    if (shuttingdownflag != NULL) {
      if (tcpcons != NULL) oz_knl_printk ("oz_dev_ip: terminating tcp connections\n");
      while (tcpcons != NULL) {
        if (tcpcons -> reset == 0) tcpcons -> reset = OZ_SYSHUTDOWN;
        tcpterminate (tcpcons);
      }
      oz_knl_event_set (shuttingdownflag, 1);
    }

    /* ?? When over 25% cpu time, turn on kernel profiling so we can figure out why ?? */

#if 000
    if ((ticks % TPS) == 0) {
      extern int oz_hw_profile_enable;
      OZ_Datebin thiscpu, thiswhen;
      static OZ_Datebin lastcpu = 0;
      static OZ_Datebin lastwhen;

      thiscpu  = oz_knl_thread_gettis (oz_knl_thread_getcur (), OZ_THREAD_STATE_RUN);
      thiswhen = oz_hw_tod_getnow ();
      if (lastcpu != 0) {
        oz_hw_profile_enable = (((thiscpu - lastcpu) * 4) > (thiswhen - lastwhen));
      }
      lastcpu  = thiscpu;
      lastwhen = thiswhen;
    }
#endif

    /* Nothing to do, wait for something */

    oz_knl_event_waitone (ipevent);
    oz_knl_event_set (ipevent, 0);
  }
}

/* This routine gets called at softint level (but in who knows what thread) when the tick timer is up */

static void timerast (void *dummy, OZ_Timer *timer)

{
  oz_knl_event_set (ipevent, 1);
}

/* This routine gets called at softint level when the system is being shut down */

static void shuttingdown (void *dummy)

{
  if (oz_knl_event_create (16, "ip shutting down", NULL, &shuttingdownflag) == OZ_SUCCESS) {
    oz_knl_event_set (ipevent, 1);
    do oz_knl_event_waitone (shuttingdownflag);
    while (oz_knl_event_set (shuttingdownflag, 0) == 0);
  }
}

/************************************************************************/
/*									*/
/*  Hardware interface control routines					*/
/*									*/
/************************************************************************/

/****************************/
/*  Add hardware interface  */
/****************************/

static uLong be_hwadd (Iopex *iopex)

{
  Buf *buf;
  const char *devname;
  Cxb *cxb;
  Hw *hw;
  int i;
  OZ_IO_ether_getinfo1 ether_getinfo1;
  OZ_IO_ether_open ether_open;
  OZ_IO_ether_transmitalloc ether_transmitalloc;
  OZ_Iochan *iochan_arp, *iochan_ip;
  uByte enaddr[OZ_IO_ETHER_MAXADDR];
  uLong sts;

  devname = iopex -> u.hwadd.devname;

  /* Make sure we don't already have this one defined */

  for (hw = hws; hw != NULL; hw = hw -> next) {
    if (strcmp (devname, hw -> devname) == 0) {
      return (OZ_HWALRDEFINED);
    }
  }

  /* Assign I/O channels to ethernet device */

  sts = oz_knl_iochan_crbynm (devname, OZ_LOCKMODE_CW, OZ_PROCMODE_KNL, NULL, &iochan_arp);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_dev_ip hwadd: error %u assigning channel to %s\n", sts, devname);
    return (sts);
  }

  sts = oz_knl_iochan_crbynm (devname, OZ_LOCKMODE_CW, OZ_PROCMODE_KNL, NULL, &iochan_ip);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_dev_ip hwadd: error %u assigning channel to %s\n", sts, devname);
    oz_knl_iochan_increfc (iochan_arp, -1);
    return (sts);
  }

  /* Open ethernet channels */

  memset (&ether_open, 0, sizeof ether_open);

  H2NW (PROTO_ARP, ether_open.proto);

  sts = oz_knl_io (iochan_arp, OZ_IO_ETHER_OPEN, sizeof ether_open, &ether_open);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_dev_ip: error %u opening %s arp channel\n", sts, devname);
    oz_knl_iochan_increfc (iochan_arp, -1);
    oz_knl_iochan_increfc (iochan_ip,  -1);
    return (sts);
  }

  H2NW (PROTO_IP, ether_open.proto);

  sts = oz_knl_io (iochan_ip, OZ_IO_ETHER_OPEN, sizeof ether_open, &ether_open);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_dev_ip: error %u opening %s ip channel\n", sts, devname);
    oz_knl_iochan_increfc (iochan_arp, -1);
    oz_knl_iochan_increfc (iochan_ip,  -1);
    return (sts);
  }

  /* Get the device's ethernet address and its maximum packet size */

  memset (&ether_getinfo1, 0, sizeof ether_getinfo1);
  ether_getinfo1.enaddrsize = OZ_IO_ETHER_MAXADDR;
  ether_getinfo1.enaddrbuff = enaddr;
  sts = oz_knl_io (iochan_arp, OZ_IO_ETHER_GETINFO1, sizeof ether_getinfo1, &ether_getinfo1);
  if (sts != OZ_SUCCESS) oz_crash ("oz_dev_ip: error %u getting ethernet address of %s", sts, devname);

  /* We only handle interfaces with a protocol size == Eproto */

  if (ether_getinfo1.protosize != sizeof (Eproto)) {
    oz_knl_printk ("oz_dev_ip: protocol size of %s is %u, we require %u\n", devname, ether_getinfo1.protosize, sizeof (Eproto));
    oz_knl_iochan_increfc (iochan_arp, -1);
    oz_knl_iochan_increfc (iochan_ip,  -1);
    return (OZ_BADPARAM);
  }

  /* Allocate and fill in hardware device block */

  hw = OZ_KNL_PGPMALLOQ (sizeof *hw);
  if (hw == NULL) {
    oz_knl_iochan_increfc (iochan_arp, -1);
    oz_knl_iochan_increfc (iochan_ip,  -1);
    return (OZ_EXQUOTAPGP);
  }
  memset (hw, 0, sizeof *hw);

  hw -> devname        = oz_knl_devunit_devname (oz_knl_iochan_getdevunit (iochan_ip));
  hw -> iochan_arp     = iochan_arp;
  hw -> iochan_ip      = iochan_ip;
  hw -> ether_getinfo1 = ether_getinfo1;
  hw -> mtu            = ether_getinfo1.datasize;
  if (hw -> mtu > OZ_IO_ETHER_MAXDATA) hw -> mtu = OZ_IO_ETHER_MAXDATA; /* limit sends to max that buffer can hold */
  CPYENAD (hw -> myenaddr, ether_getinfo1.enaddrbuff, hw);

  PRINTF "oz_dev_ip: device %s, hw addr ", hw -> devname);
  printenaddr (hw, hw -> myenaddr);
  PRINTF ", enabled, mtu %u\n", hw -> mtu);

  /* Link it to list of available devices */

  hw -> next = hws;
  hws = hw;

  /* Start receiving ARP and IP packets - receives into ethernet driver's internal buffers */

  for (i = 0; i < NUM_BUFFS_RCV_ARP; i ++) {
    buf = OZ_KNL_PGPMALLOC (sizeof *buf);
    memset (buf, 0, sizeof *buf);
    buf -> type = BUF_TYPE_RCV_ARP;
    buf -> hw   = hw;
    startread (buf);
  }

  for (i = 0; i < NUM_BUFFS_RCV_IP; i ++) {
    buf = OZ_KNL_PGPMALLOC (sizeof *buf);
    memset (buf, 0, sizeof *buf);
    buf -> type = BUF_TYPE_RCV_IP;
    buf -> hw   = hw;
    startread (buf);
  }

  /* Set up pool of user send buffers - allocated from ethernet driver's internal buffers */

  hw -> waitsndubuft = &(hw -> waitsndubufh);

  memset (&ether_transmitalloc, 0, sizeof ether_transmitalloc);
  for (i = 0; i < NUM_BUFFS_SNDU; i ++) {
    buf = OZ_KNL_PGPMALLOC (sizeof *buf);
    memset (buf, 0, sizeof *buf);
    ether_transmitalloc.xmtdrv_r = &(buf -> xmtdrv);
    ether_transmitalloc.xmteth_r = &(buf -> cxb);
    sts = oz_knl_io (iochan_ip, OZ_IO_ETHER_TRANSMITALLOC, sizeof ether_transmitalloc, &ether_transmitalloc);
    if (sts != OZ_SUCCESS) oz_crash ("oz_dev_ip: error %u allocating transmit buffers for %s", sts, hw -> devname);
    buf -> type = BUF_TYPE_SNDU;
    buf -> hw   = hw;
    freebuf (buf);
  }
  valsndubufs (hw, __LINE__);

  hwchanged ();

  return (OZ_SUCCESS);
}

/***********************************************************************/
/*  Remove hardware interface (and abort everything trying to use it)  */
/***********************************************************************/

static uLong be_hwrem (Iopex *iopex)

{
  Hw *hw;

  hw = find_hw_by_name (iopex -> u.hwrem.devname);		/* look for corresponding hw struct */
  if (hw == NULL) return (OZ_HWNOTDEFINED);			/* if not there, error */

  hwterminate (hw);
  return (OZ_SUCCESS);
}

/***************************************************/
/*  Add ip address and mask to hardware interface  */
/***************************************************/

static uLong be_hwipamadd (Iopex *iopex)

{
  Arpc *arpc;
  Hw *hw;
  Hwipam *hwipam, **lhwipam;

  /* Find the hw that this ip address and mask are for */

  hw = find_hw_by_name (iopex -> u.hwipamadd.devname);
  if (hw == NULL) return (OZ_HWNOTDEFINED);

  /* If matching nwipaddr/hwipaddr already exists for this device, remove it */
  /* They might just be changing the mask                                    */

  hwipamrem (hw, iopex -> u.hwipamadd.nwipaddr, iopex -> u.hwipamadd.hwipaddr);

  /* Now re-add entry in order, with narrowest masks first */

  for (lhwipam = &(hw -> hwipams); (hwipam = *lhwipam) != NULL;) {
    if (CGTIPAD (iopex -> u.hwipamadd.nwipmask, hwipam -> nwipmask)) break;
  }

  hwipam = OZ_KNL_PGPMALLOC (sizeof *hwipam);
  hwipam -> next = *lhwipam;
  CPYIPADM (hwipam -> nwipaddr, iopex -> u.hwipamadd.nwipaddr, iopex -> u.hwipamadd.nwipmask);
  CPYIPAD  (hwipam -> nwipmask, iopex -> u.hwipamadd.nwipmask);
  CPYIPAD  (hwipam -> hwipaddr, iopex -> u.hwipamadd.hwipaddr);
  *lhwipam = hwipam;

  /* Now put in a permanent arpc entry so that it won't try to arp for itself */

  validatearpcs (hw, __LINE__);
  for (arpc = hw -> arpcs; arpc != NULL; arpc = arpc -> next) {	/* see if there is already an arpc entry with matching ip address */
    if (CEQIPAD (arpc -> ipaddr, hwipam -> hwipaddr)) break;
  }
  if (arpc == NULL) {
    arpc = OZ_KNL_PGPMALLOC (sizeof *arpc);			/* if not, allocate an arp cache entry */
    CPYIPAD (arpc -> ipaddr, hwipam -> hwipaddr);		/* save the ip address */
    arpc -> next = hw -> arpcs;					/* link to hardware's arp cache list */
    hw -> arpcs = arpc;
  }
  CPYENAD (arpc -> enaddr, hw -> myenaddr, hw);			/* anyway, save the ethernet address in case the old matching ip */
								/* ... address was from some other device plugged in the network */
								/* ... from long ago */
  arpc -> timeout = -1;						/* set its timeout = forever */
  hw -> arpcount ++;
  validatearpcs (hw, __LINE__);

  /* Finally, ransack stuff that references hardware in case it needs to be changed */

  hwchanged ();
  return (OZ_SUCCESS);
}

/********************************************************/
/*  Remove ip address and mask from hardware interface  */
/********************************************************/

static uLong be_hwipamrem (Iopex *iopex)

{
  Hw *hw;

  /* Find corresponding hw struct */

  hw = find_hw_by_name (iopex -> u.hwipamrem.devname);
  if (hw == NULL) return (OZ_HWNOTDEFINED);

  /* Remove hwipam struct */

  if (hwipamrem (hw, iopex -> u.hwipamrem.nwipaddr, iopex -> u.hwipamrem.hwipaddr)) return (OZ_SUCCESS);
  return (OZ_HWIPAMNOTDEF);

  /* Update any stuff that needs it */

  hwchanged ();

  return (OZ_SUCCESS);
}

/******************************/
/*  List hardware interfaces  */
/******************************/

static uLong be_hwlist (Iopex *iopex)

{
  Hw *hw;
  uLong sts;

  sts = OZ_SUCCESS;
  if (iopex -> u.hwlist.p.devnamebuff[0] == 0) hw = hws;
  else {
    hw = find_hw_by_name (iopex -> u.hwlist.p.devnamebuff);
    if (hw == NULL) sts = OZ_HWNOTDEFINED;
    else hw = hw -> next;
  }
  if (hw == NULL) iopex -> u.hwlist.p.devnamebuff[0] = 0;
  else strncpyz (iopex -> u.hwlist.p.devnamebuff, hw -> devname, iopex -> u.hwlist.p.devnamesize);
  return (sts);
}

/******************************************************/
/*  List hardware interface's ip addresses and masks  */
/******************************************************/

static uLong be_hwipamlist (Iopex *iopex)

{
  Hw *hw;
  Hwipam *hwipam;
  uLong sts;

  sts = OZ_SUCCESS;
  hw  = find_hw_by_name (iopex -> u.hwipamlist.devname);				/* find the hardware device */
  if (hw == NULL) sts = OZ_HWNOTDEFINED;						/* error if not found */
  else {
    if (ZERIPAD (iopex -> u.hwipamlist.p.hwipaddr)) hwipam = hw -> hwipams;		/* if zero ip address given, return first ipam in list */
    else {
      for (hwipam = hw -> hwipams; hwipam != NULL; hwipam = hwipam -> next) {		/* otherwise, find the given ipam */
        if (CEQIPAD (iopex -> u.hwipamlist.p.hwipaddr, hwipam -> hwipaddr) && CEQIPAD (iopex -> u.hwipamlist.p.nwipaddr, hwipam -> nwipaddr)) break;
      }
      if (hwipam == NULL) sts = OZ_HWIPAMNOTDEF;					/* error if not found */
      else hwipam = hwipam -> next;							/* point to ipam entry following that one */
    }
    if (hwipam == NULL) {
      CLRIPAD (iopex -> u.hwipamlist.p.hwipaddr);					/* end of list, return zeroes */
      CLRIPAD (iopex -> u.hwipamlist.p.nwipaddr);
      CLRIPAD (iopex -> u.hwipamlist.p.nwipmask);
    } else {
      CPYIPAD (iopex -> u.hwipamlist.p.hwipaddr, hwipam -> hwipaddr);			/* not end, return ipam entry */
      CPYIPAD (iopex -> u.hwipamlist.p.nwipaddr, hwipam -> nwipaddr);
      CPYIPAD (iopex -> u.hwipamlist.p.nwipmask, hwipam -> nwipmask);
    }
  }
  return (sts);
}

/*********************************/
/*  Remove an Hwipam from an Hw  */
/*********************************/

static int hwipamrem (Hw *hw, const uByte nwipaddr[IPADDRSIZE], const uByte hwipaddr[IPADDRSIZE])

{
  Arpc *arpc, **larpc;
  Hwipam *hwipam, **lhwipam;
  int found;

  found = 0;
  for (lhwipam = &(hw -> hwipams); (hwipam = *lhwipam) != NULL;) {
    if (!CEQIPADM (hwipam -> nwipaddr, nwipaddr, hwipam -> nwipmask) || !CEQIPAD (hwipam -> hwipaddr, hwipaddr)) {
      lhwipam = &(hwipam -> next);
    } else {
      *lhwipam = hwipam -> next;
      OZ_KNL_PGPFREE (hwipam);
      found = 1;
    }
  }

  /* Find, unlink and free (seek, locate and exterminate) any corresponding arpc struct */
  /* But leave it there if there is some other hwipam with the same hwipaddr            */

  if (found) {
    for (hwipam = hw -> hwipams; hwipam != NULL; hwipam = hwipam -> next) {
      if (CEQIPAD (hwipam -> hwipaddr, hwipaddr)) break;
    }
    if (hwipam == NULL) {
      validatearpcs (hw, __LINE__);
      for (larpc = &(hw -> arpcs); (arpc = *larpc) != NULL; larpc = &(arpc -> next)) {
        if (CEQIPAD (arpc -> ipaddr, hwipaddr)) break;
      }
      if (arpc != NULL) {
        *larpc = arpc -> next;
        OZ_KNL_PGPFREE (arpc);
        hw -> arpcount --;
        validatearpcs (hw, __LINE__);
      }
    }
  }

  return (found);
}

/*************************************/
/*  This routine nukes the given hw  */
/*************************************/

static void hwterminate (Hw *hw)

{
  Arpc *arpc;
  Buf *buf, *frag, **lbuf, **lfrag;
  Hw **lhw, *xhw;
  Hwipam *hwipam;
  Msubp *msubp;

  /* Let everyone know we're going away and unlink us from the hws list */

  hw -> terminated = 1;

  for (lhw = &hws; (xhw = *lhw) != NULL; lhw = &(xhw -> next)) {
    if (xhw == hw) {
      oz_knl_printk ("oz_dev_ip: hardware %s shutting down\n", hw -> devname);
      *lhw = xhw -> next;
      break;
    }
  }

  /* Abort all ethernet I/O.  Since hw -> terminated is set, we shouldn't get any error messages. */

  oz_knl_ioabort (hw -> iochan_arp, OZ_PROCMODE_KNL);
  oz_knl_ioabort (hw -> iochan_ip,  OZ_PROCMODE_KNL);

  /* Get rid of all ip addresses and masks for the hardware */

  while ((hwipam = hw -> hwipams) != NULL) {
    hw -> hwipams = hwipam -> next;
    OZ_KNL_PGPFREE (hwipam);
  }

  /* Get rid of all arp cache entries for the hardware */

  validatearpcs (hw, __LINE__);
  while ((arpc = hw -> arpcs) != NULL) {
    hw -> arpcs = arpc -> next;
    OZ_KNL_PGPFREE (arpc);
  }
  hw -> arpcount = 0;

  /* Get rid of all buffers waiting for arp to complete */

  while ((buf = hw -> arpwaits) != NULL) {
    hw -> arpwaits = buf -> next;
    buf -> iosts = OZ_ABORTED;
    sendcomplete (buf);
  }

  /* Get rid of all requests waiting for a SNDU buffer */

  while ((msubp = hw -> waitsndubufh) != NULL) {
    hw -> waitsndubufh = msubp -> next;
    if (hw -> waitsndubufh == NULL) hw -> waitsndubuft = &(hw -> waitsndubufh);
    msubp -> prev = NULL;
    hw -> waitsndubufc --;
    (*(msubp -> entry)) (NULL, msubp -> param);
  }

  /* Free off any left-over user send buffers */

  while ((buf = hw -> freesndubufs) != NULL) {
    hw -> freesndubufs = buf -> next;
    hw -> freesndubufc --;
    freebuf (buf);
  }

  /* Free off any incoming fragments that came from this device */

  for (lbuf = &frags; (buf = *lbuf) != NULL;) {
    for (lfrag = &(buf -> frag); (frag = *lfrag) != NULL;) {
      if (frag -> hw != hw) lfrag = &(frag -> frag);
      else {
        *lfrag = frag -> frag;
        freebuf (frag);
      }
    }
    if (buf -> hw != hw) lbuf = &(buf -> next);
    else {
      *lbuf = buf -> next;
      while ((frag = buf -> frag) != NULL) {
        buf -> frag = frag -> frag;
        freebuf (frag);
      }
      freebuf (buf);
    }
  }

  /* Other miscellany to be changed when hardware config changes */

  hwchanged ();

  /* Finally, if there are no receives or transmits in progress, finish closing it out */
  /* If there are still I/O's going, we will be called back when they complete         */

  if ((hw -> receivesinprog == 0) && (hw -> transmitsinprog == 0)) {
    oz_knl_iochan_increfc (hw -> iochan_arp, -1);	/* now that there's no I/O going, deassigning */
    oz_knl_iochan_increfc (hw -> iochan_ip,  -1);	/* ... should happen without any waiting      */
    oz_knl_printk ("oz_dev_ip: hardware %s shutdown complete\n", hw -> devname);
    OZ_KNL_PGPFREE (hw);
  }
}

/**********************************************************/
/*  Some hardware definition was changed, fix everything  */
/**********************************************************/

static void hwchanged (void)

{
  Route *route;
  uByte gwipaddr[IPADDRSIZE];

  /* Fix all the router definitions - they have a pointer to the best   */
  /* hardware to access them and what the ip address of the hardware is */

  for (route = routes; route != NULL; route = route -> next) {				/* scan router list */
    route -> hw = find_send_hw (route -> gwipaddr, route -> hwipaddr, gwipaddr, 0);	/* find the hardware to send to it */
    if (route -> hw == NULL) {								/* we dont do router->router->... */
      PRINTF "oz_dev_ip: no direct connection to router ");				/* ... to avoid routing loops */
      printipaddr (route -> gwipaddr);
      PRINTF " serving network ");
      printipaddr (route -> nwipaddr);
      PRINTF " mask ");
      printipaddr (route -> nwipmask);
      PRINTF "\n");
      route -> hw = NULL;
    }
  }
}

/************************************************************************/
/*									*/
/*  ARP control routines						*/
/*									*/
/************************************************************************/

	/* add an arp cache entry */

static uLong be_arpadd (Iopex *iopex)

{
  Hw *hw;
  uLong timeout;

  hw = find_hw_by_name (iopex -> u.arpadd.p.devname);
  if (hw == NULL) return (OZ_HWNOTDEFINED);
  if (iopex -> u.arpadd.p.enaddrsize != hw -> ether_getinfo1.addrsize) return (OZ_BADPARAM);
  timeout = iopex -> u.arpadd.p.timeout;
  if (timeout == 0) timeout = ticks + ARPCACHE_LIFE;					// zero = use default
  else if (timeout != (uLong)(-1)) timeout = timeout / MSPT + ticks;			// -1: infinite, else milliseconds
  addtoarpc (hw, iopex -> u.arpadd.p.enaddr, iopex -> u.arpadd.p.ipaddr, timeout);
  return (OZ_SUCCESS);
}

	/* list arp cache entries */

static uLong be_arplist (Iopex *iopex)

{
  Arpc *arpc;
  Hw *hw;
  uLong sts, timeout;

  hw  = find_hw_by_name (iopex -> u.arplist.devname);					/* find the hardware device */
  if (hw == NULL) sts = OZ_HWNOTDEFINED;						/* error if not found */
  else if (iopex -> u.arplist.p.enaddrsize != hw -> ether_getinfo1.addrsize) sts = OZ_BADPARAM;
  else {
    if (ZERIPAD (iopex -> u.arplist.p.ipaddr)) arpc = hw -> arpcs;			/* if zero ip address given, return first arpc in list */
    else {
      for (arpc = hw -> arpcs; arpc != NULL; arpc = arpc -> next) {			/* otherwise, find the given arpc */
        if (CEQIPAD (iopex -> u.arplist.p.ipaddr, arpc -> ipaddr)) break;
      }
      if (arpc == NULL) sts = OZ_ARPCNOTDEF;						/* error if not found */
      else arpc = arpc -> next;								/* point to arpc entry following that one */
    }
    *(iopex -> u.arplist.p.timeout) = 0;
    if (arpc == NULL) {
      CLRIPAD (iopex -> u.arplist.p.ipaddr);						/* end of list, return zeroes */
      memclr (iopex -> u.arplist.p.enaddr, hw -> ether_getinfo1.addrsize);
    } else {
      CPYIPAD (iopex -> u.arplist.p.ipaddr, arpc -> ipaddr);				/* not end, return arpc entry */
      CPYENAD (iopex -> u.arplist.p.enaddr, arpc -> enaddr, hw);
      timeout = arpc -> timeout;							/* get tick timeout */
      if (timeout < ticks) timeout = 0;							/* maybe it's already expired */
      else if (timeout != (uLong)(-1)) timeout = (timeout - ticks) * MSPT;		/* else, calc milliseconds from now */
      *(iopex -> u.arplist.p.timeout) = timeout;
    }
    sts = OZ_SUCCESS;
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  Routing table control functions					*/
/*									*/
/************************************************************************/

	/*************************/
	/*  Add a routing entry  */
	/*************************/

static uLong be_routeadd (Iopex *iopex)

{
  int i;
  Route **lroute, *route, *xroute;

  /* Create an entry for it */

  route = OZ_KNL_PGPMALLOQ (sizeof *route);
  if (route == NULL) return (OZ_EXQUOTAPGP);
  memset (route, 0, sizeof *route);
  CPYIPAD  (route -> gwipaddr, iopex -> u.routeadd.gwipaddr);
  CPYIPADM (route -> nwipaddr, iopex -> u.routeadd.nwipaddr, iopex -> u.routeadd.nwipmask);
  CPYIPAD  (route -> nwipmask, iopex -> u.routeadd.nwipmask);

  /* Insert in table - by descending address then descending mask */
  /* This will put the narrowest masks first                      */

  for (lroute = &routes; (xroute = *lroute) != NULL; lroute = &(xroute -> next)) {
    if (CGTIPAD (route -> nwipmask, xroute -> nwipmask)) break;
    if (CGTIPAD (route -> nwipaddr, xroute -> nwipaddr)) break;
  }
  route -> next = *lroute;
  *lroute = route;

  /* Print out message saying what was added */

  oz_knl_printk ("oz_dev_ip routeadd: gateway ");
  printipaddr (route -> gwipaddr);
  oz_knl_printk (" to network ");
  printipaddr (route -> nwipaddr);
  oz_knl_printk (" mask ");
  printipaddr (route -> nwipmask);
  if (route -> next == NULL) oz_knl_printk ("  (on end)\n");
  else {
    oz_knl_printk ("  (before gateway ");
    printipaddr (route -> next -> gwipaddr);
    oz_knl_printk (" to network ");
    printipaddr (route -> next -> nwipaddr);
    oz_knl_printk (" mask ");
    printipaddr (route -> next -> nwipmask);
    oz_knl_printk (")\n");
  }

  /* Fill in the hw that the router is connected via */

  hwchanged ();

  return (OZ_SUCCESS);
}

	/**************************/
	/* remove a routing entry */
	/**************************/

static uLong be_routerem (Iopex *iopex)

{
  Route **lroute, *xroute;

  /* Find first matching entry */

  for (lroute = &routes; (xroute = *lroute) != NULL; lroute = &(xroute -> next)) {
    if (CEQIPAD (xroute -> gwipaddr, iopex -> u.routerem.gwipaddr) 
     && CEQIPAD (xroute -> nwipaddr, iopex -> u.routerem.nwipaddr) 
     && CEQIPAD (xroute -> nwipmask, iopex -> u.routerem.nwipmask)) break;
  }
  if (xroute == NULL) return (OZ_NOSUCHROUTE);

  /* Remove it from list */

  xroute -> next = *lroute;
  *lroute = xroute;
  OZ_KNL_PGPFREE (xroute);

  return (OZ_SUCCESS);
}

	/************************/
	/* list routing entries */
	/************************/

static uLong be_routelist (Iopex *iopex)

{
  Route *route;
  uLong sts;

  sts = OZ_SUCCESS;
  if (ZERIPAD (iopex -> u.routelist.p.gwipaddr)) route = routes;			/* if zero ip address given, return first route in list */
  else {
    for (route = routes; route != NULL; route = route -> next) {			/* otherwise, find the given entry */
      if (CEQIPAD (iopex -> u.routelist.p.gwipaddr, route -> gwipaddr) 
       && CEQIPAD (iopex -> u.routelist.p.nwipaddr, route -> nwipaddr) 
       && CEQIPAD (iopex -> u.routelist.p.nwipmask, route -> nwipmask)) break;
    }
    if (route == NULL) sts = OZ_NOSUCHROUTE;						/* error if not found */
    else route = route -> next;								/* point to route entry following that one */
  }
  if (iopex -> u.routelist.p.devnamesize != 0) iopex -> u.routelist.p.devnamebuff[0] = 0;
  if (iopex -> u.routelist.p.hwipaddr != NULL) CLRIPAD (iopex -> u.routelist.p.hwipaddr);
  if (route == NULL) {
    CLRIPAD (iopex -> u.routelist.p.gwipaddr);						/* end of list, return zeroes */
    CLRIPAD (iopex -> u.routelist.p.nwipaddr);
    CLRIPAD (iopex -> u.routelist.p.nwipmask);
  } else {
    CPYIPAD (iopex -> u.routelist.p.gwipaddr, route -> gwipaddr);			/* not end, return route entry */
    CPYIPAD (iopex -> u.routelist.p.nwipaddr, route -> nwipaddr);
    CPYIPAD (iopex -> u.routelist.p.nwipmask, route -> nwipmask);
    if (route -> hw != NULL) {
      if (iopex -> u.routelist.p.devnamesize != 0) strncpyz (iopex -> u.routelist.p.devnamebuff, route -> hw -> devname, iopex -> u.routelist.p.devnamesize);
      if (iopex -> u.routelist.p.hwipaddr != NULL) CPYIPAD (iopex -> u.routelist.p.hwipaddr, route -> hwipaddr);
    }
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  Filter table control functions					*/
/*									*/
/************************************************************************/

	/************************/
	/*  Add a filter entry  */
	/************************/

static uLong be_filteradd (Iopex *iopex)

{
  Filter *filter, **lfilter;
  int index, acount, astart;

  /* Decode the filter list to add to */

  lfilter = NULL;
  if (iopex -> u.filteradd.p.listid == OZ_IO_IP_FILTER_INPUT)   lfilter = &input_filters;
  if (iopex -> u.filteradd.p.listid == OZ_IO_IP_FILTER_FORWARD) lfilter = &forward_filters;
  if (iopex -> u.filteradd.p.listid == OZ_IO_IP_FILTER_OUTPUT)  lfilter = &output_filters;
  if (lfilter == NULL) return (OZ_BADPARAM);

  /* Skip over the indexed ones already in the list */

  index = iopex -> u.filteradd.p.index;
  while (-- index >= 0) {
    filter = *lfilter;
    if (filter == NULL) return (OZ_SUBSCRIPT);
    lfilter = &(filter -> next);
  }

  /* Trim off any trailing zero bytes.  Mask bytes of zeroes are always a match so why bother with them. */

  while (iopex -> u.filteradd.p.size > 0) {			// repeat while there are bytes at the end to check
    if (((uByte *)(iopex -> u.filteradd.p.mask))[iopex->u.filteradd.p.size-1] != 0) break; // stop when we hit a non-zero byte
    -- (iopex -> u.filteradd.p.size);				// zero byte at end, reduce size to chop it off
  }

  /* Skip leading longwords of zeroes in the element being added */

  astart = 0;							// assume we won't skip any
  while (iopex -> u.filteradd.p.size >= 4) {			// repeat while there are whole longwords to check
    if (*(uLong *)(iopex -> u.filteradd.p.mask) != 0) break;	// stop if we have a non-zero mask longword
    iopex -> u.filteradd.p.size -= 4;				// zero, reduce the size by a longword
    ((uLong *)(iopex -> u.filteradd.p.data)) ++;		// increment pointers to next longword
    ((uLong *)(iopex -> u.filteradd.p.mask)) ++;
    astart ++;							// remember we skipped over a longword at the beginning
  }

  /* Allocate the filter block with enough 'a' elements to cover the supplied mask and data */

  acount = (iopex -> u.filteradd.p.size + 3) / 4;		// this is how many elements we need in 'a' array
  if (acount == 0) acount = 1;					// filter_check reqvires a non-zero array element count
  filter = OZ_KNL_PGPMALLOQ (sizeof *filter + acount * sizeof (filter -> a[0])); // allocate memory
  if (filter == NULL) return (OZ_EXQUOTAPGP);			// if too big, return error

  /* Fill in filter header */

  filter -> next   = *lfilter;					// what will be next in chain
  filter -> action = iopex -> u.filteradd.p.action;		// what action to take when filter matches
  filter -> acount = acount;					// how many longwords to compare
  filter -> astart = astart;					// what longword to start comparing at

  /* Copy whole longwords from supplied data to internal buffer */
  /* Keep the bytes in network order to make filter_check fast  */

  acount = 0;							// start counting array elements
  while (iopex -> u.filteradd.p.size >= 4) {			// repeat as long as there are whole longwords
    filter -> a[acount].data = *(uLong *)(iopex -> u.filteradd.p.data); // copy data longword
    filter -> a[acount].mask = *(uLong *)(iopex -> u.filteradd.p.mask); // copy mask longword
    iopex -> u.filteradd.p.size -= 4;				// reduce size by a longword
    (uByte *)(iopex -> u.filteradd.p.data) += 4;		// increment pointers by a longword
    (uByte *)(iopex -> u.filteradd.p.mask) += 4;
    acount ++;							// increment array index
  }

  /* Copy any part of left over bytes, padded to a longword */

  if (iopex -> u.filteradd.p.size > 0) {			// see if any left over bytes
    filter -> a[acount].data = 0;				// if so, pad them with zeroes
    filter -> a[acount].mask = 0;
    memcpy (&(filter -> a[acount].data), iopex -> u.filteradd.p.data, iopex -> u.filteradd.p.size); // this should work
    memcpy (&(filter -> a[acount].mask), iopex -> u.filteradd.p.mask, iopex -> u.filteradd.p.size); // ... endian-independent
    acount ++;							// we padded to a whole longword, so count it
  }

  /* Filter_check reqvires a non-zero array element count */

  if (acount == 0) {						// check for zero
    filter -> a[0].data = 0;					// fill in an entry of zeroes
    filter -> a[0].mask = 0;
    acount = 1;							// now there is one element
  }
  filter -> acount = acount;					// save final longword count

  /* Link it into filter list */

#if 000
  oz_knl_printk ("oz_dev_ip filteradd: %d[%d]\n", iopex -> u.filteradd.p.listid, iopex -> u.filteradd.p.index);
  oz_knl_dumpmem (sizeof *filter + (acount - 1) * sizeof (filter -> a[0]), filter);
#endif

  *lfilter = filter;
  return (OZ_SUCCESS);
}

	/*************************/
	/*  Remove filter entry  */
	/*************************/

static uLong be_filterrem (Iopex *iopex)

{
  Filter *filter, **lfilter;
            
  lfilter = NULL;
  if (iopex -> u.filterrem.p.listid == OZ_IO_IP_FILTER_INPUT)   lfilter = &input_filters;
  if (iopex -> u.filterrem.p.listid == OZ_IO_IP_FILTER_FORWARD) lfilter = &forward_filters;
  if (iopex -> u.filterrem.p.listid == OZ_IO_IP_FILTER_OUTPUT)  lfilter = &output_filters;
  if (lfilter == NULL) return (OZ_BADPARAM);

  while (-- (iopex -> u.filterrem.p.index) >= 0) {
    filter = *lfilter;
    if (filter == NULL) return (OZ_SUBSCRIPT);
    lfilter = &(filter -> next);
  }
  filter = *lfilter;
  if (filter == NULL) return (OZ_SUBSCRIPT);
  *lfilter = filter -> next;
  OZ_KNL_PGPFREE (filter);
  return (OZ_SUCCESS);
}

	/***********************/
	/*  List filter entry  */
	/***********************/

static uLong be_filterlist (Iopex *iopex)

{
  Filter *filter, **lfilter;
  int acount;
  uLong ldata, lmask, sts;
            
  lfilter = NULL;
  if (iopex -> u.filterlist.p.listid == OZ_IO_IP_FILTER_INPUT)   lfilter = &input_filters;
  if (iopex -> u.filterlist.p.listid == OZ_IO_IP_FILTER_FORWARD) lfilter = &forward_filters;
  if (iopex -> u.filterlist.p.listid == OZ_IO_IP_FILTER_OUTPUT)  lfilter = &output_filters;
  if (lfilter == NULL) return (OZ_BADPARAM);

  for (filter = *lfilter; filter != NULL; filter = filter -> next) {
    if (-- (iopex -> u.filterlist.p.index) < 0) goto foundit;
  }
  return (OZ_SUBSCRIPT);

foundit:

  /* Return the action code if requested */

  if (iopex -> u.filterlist.p.action_r != NULL) {
    sts = oz_knl_section_uput (iopex -> procmode, sizeof *(iopex -> u.filterlist.p.action_r), &(filter -> action), iopex -> u.filterlist.p.action_r);
    if (sts != OZ_SUCCESS) return (sts);
  }

  /* If buffer can't even hold the leading zeroes, zero the buffers and return overflow status */

  if (iopex -> u.filterlist.p.size < filter -> astart * 4) {
    memset (iopex -> u.filterlist.p.data, 0, iopex -> u.filterlist.p.size);
    memset (iopex -> u.filterlist.p.mask, 0, iopex -> u.filterlist.p.size);
    return (OZ_BUFFEROVF);
  }

  /* Ok, put in leading zeroes */

  memset (iopex -> u.filterlist.p.data, 0, filter -> astart * 4);
  memset (iopex -> u.filterlist.p.mask, 0, filter -> astart * 4);
  iopex -> u.filterlist.p.size -= filter -> astart * 4;
  ((uLong *)(iopex -> u.filterlist.p.data)) += filter -> astart;
  ((uLong *)(iopex -> u.filterlist.p.mask)) += filter -> astart;

  /* Return any whole longwords the caller can take and that we have */

  acount = 0;
  while ((iopex -> u.filterlist.p.size >= 4) && (acount < filter -> acount)) {
    *(uLong *)(iopex -> u.filterlist.p.data) = filter -> a[acount].data;
    *(uLong *)(iopex -> u.filterlist.p.mask) = filter -> a[acount].mask;
    iopex -> u.filterlist.p.size -= 4;
    ((uLong *)(iopex -> u.filterlist.p.data)) ++;
    ((uLong *)(iopex -> u.filterlist.p.mask)) ++;
    acount ++;
  }

  /* If we gave all we got, fill caller's buffers with zeroes */

  if (acount == filter -> acount) {
    memset (iopex -> u.filterlist.p.data, 0, iopex -> u.filterlist.p.size);
    memset (iopex -> u.filterlist.p.mask, 0, iopex -> u.filterlist.p.size);
  }

  /* Otherwise, try to return the last few bytes to caller then zero fill */

  else {
    memcpy (iopex -> u.filterlist.p.data, &(filter -> a[acount].data), iopex -> u.filterlist.p.size);
    memcpy (iopex -> u.filterlist.p.mask, &(filter -> a[acount].mask), iopex -> u.filterlist.p.size);
    if (++ acount < filter -> acount) return (OZ_BUFFEROVF);
    ldata = filter -> a[acount].data;
    lmask = filter -> a[acount].mask;
    memset (&ldata, 0, iopex -> u.filterlist.p.size);
    memset (&lmask, 0, iopex -> u.filterlist.p.size);
    if ((ldata | lmask) != 0) return (OZ_BUFFEROVF);
  }
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Raw IP packet I/O routines						*/
/*									*/
/************************************************************************/

static uLong be_ipbind (Iopex *iopex)

{
  Chnex *chnex;

  chnex = iopex -> chnex;
  if (chnex -> ipbind_iopqt != NULL) return (OZ_CHANALRBOUND);

  /* Set up the chnex struct to reference specific ip packet conditions */

  CPYIPAD (chnex -> ipbind_lclipaddr, iopex -> u.ipbind.lclipaddr);	/* get local ip address */
  CPYIPAD (chnex -> ipbind_remipaddr, iopex -> u.ipbind.remipaddr);	/* get remote ip address */
  chnex -> ipbind_proto    = iopex -> u.ipbind.p.proto;			/* get ip protocol */
  chnex -> ipbind_passiton = iopex -> u.ipbind.p.passiton;		/* see whether or not to pass it on for normal processing */
  chnex -> ipbind_next     = ipbinds;					/* enable it */
  chnex -> ipbind_iopqh    = NULL;
  chnex -> ipbind_iopqt    = &(chnex -> ipbind_iopqh);
  chnex -> ip_threadid     = THREADID (iopex);
  ipbinds = chnex;
  return (OZ_SUCCESS);
}

/*************************************/
/*  Start transmitting an ip packet  */
/*************************************/

static uLong be_iptransmit (Iopex *iopex)

{
  Buf *buf;
  Hw *hw;

  /* Make sure it's not going to Vermont (you can't get there from here) */

  hw = find_send_hw (iopex -> u.iptransmit.p.ippkt -> dstipaddr, NULL, NULL, 1);
  if (hw == NULL) return (OZ_NOROUTETODEST);

  /* Make sure the message fits in a single IP packet */

  if (iopex -> u.iptransmit.p.pktsize > hw -> mtu) return (OZ_BUFFEROVF);

  /* Ok, queue send buffer malloc request */

  iopex -> chnex -> ip_transcount ++;
  iopex -> chnex -> ip_threadid = THREADID (iopex);
  buf = mallocsndubuf (hw, iptransmit_mem, iopex, &(iopex -> msubp));
  if (buf != NULL) iptransmit_mem (buf, iopex);
  return (OZ_STARTED);
}

/* There is a buffer available for the request, start sending the packet */

static void iptransmit_mem (Buf *buf, void *iopexv)

{
  Iopex *iopex;
  uLong sts;

  iopex = iopexv;
  sts   = OZ_NOROUTETODEST;
  if (buf != NULL) {
    ACCESSBUFFS (iopex);
    memcpy (IPPKT (buf -> hw, buf -> cxb), iopex -> u.iptransmit.p.ippkt, iopex -> u.iptransmit.p.pktsize);
    sts = sendip (buf, iptransmit_done, iopex);
  }
  if (sts != OZ_STARTED) oz_knl_iodone (iopex -> ioop, sts, NULL, NULL, NULL);
}

static void iptransmit_done (void *iopexv, uLong status)

{
  oz_knl_iodone (((Iopex *)iopexv) -> ioop, status, NULL, NULL, NULL);
}

/**********************************/
/*  Start receiving an ip packet  */
/**********************************/

static uLong be_ipreceive (Iopex *iopex)

{
  Chnex *chnex;

  chnex = iopex -> chnex;
  if (chnex -> ipbind_iopqt == NULL) return (OZ_CHANOTBOUND);	/* it must have been bound with OZ_IO_IP_IPBIND */
  *(chnex -> ipbind_iopqt) = iopex;				/* put request on end of queue */
  chnex -> ipbind_iopqt = &(iopex -> next);
  chnex -> ip_recvcount ++;
  chnex -> ip_threadid = THREADID (iopex);
  iopex -> next = NULL;
  return (OZ_STARTED);
}

/**************************/
/*  Get ip info, part 1  */
/**************************/

static uLong be_ipgetinfo1 (Iopex *iopex)

{
  Chnex *chnex;
  Iopex *riopex;

  chnex = iopex -> chnex;

  /* If ipbind_iopqt is NULL, it is not bound, so return zeroes */

  if (chnex -> ipbind_iopqt == NULL) {
    if (iopex -> u.ipgetinfo1.p.lclipaddr != NULL) CLRIPAD (iopex -> u.ipgetinfo1.p.lclipaddr);
    if (iopex -> u.ipgetinfo1.p.remipaddr != NULL) CLRIPAD (iopex -> u.ipgetinfo1.p.remipaddr);
    iopex -> u.ipgetinfo1.p.recvpend = -1;
  }

  /* It is bound, return what it is bound to */

  else {
    if (iopex -> u.ipgetinfo1.p.lclipaddr != NULL) CPYIPAD (iopex -> u.ipgetinfo1.p.lclipaddr, chnex -> ipbind_lclipaddr);
    if (iopex -> u.ipgetinfo1.p.remipaddr != NULL) CPYIPAD (iopex -> u.ipgetinfo1.p.remipaddr, chnex -> ipbind_remipaddr);
    iopex -> u.ipgetinfo1.p.recvpend = 0;
    for (riopex = chnex -> ipbind_iopqh; riopex != NULL; riopex = riopex -> next) {
      iopex -> u.ipgetinfo1.p.recvpend ++;
    }
  }

  iopex -> u.ipgetinfo1.p.proto      = chnex -> ipbind_proto;
  iopex -> u.ipgetinfo1.p.passiton   = chnex -> ipbind_passiton;
  iopex -> u.ipgetinfo1.p.recvcount  = chnex -> ip_recvcount;
  iopex -> u.ipgetinfo1.p.transcount = chnex -> ip_transcount;
  iopex -> u.ipgetinfo1.p.threadid   = chnex -> ip_threadid;

  movc4 (sizeof iopex -> u.ipgetinfo1.p, &(iopex -> u.ipgetinfo1.p), iopex -> u.ipgetinfo1.as, iopex -> u.ipgetinfo1.ap);

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  UDP I/O routines							*/
/*									*/
/************************************************************************/

static uLong be_udpbind (Iopex *iopex)

{
  Chnex *chnex;
  int ephem;

  chnex = iopex -> chnex;

  /* If udpbind_iopqt is not NULL, it is already bound to a udp socket */

  if (chnex -> udpbind_iopqt != NULL) return (OZ_CHANALRBOUND);

  /* Not bound already, set channel up on the udpbinds list */

  chnex -> udpbind_ephem = 0;
  CPYIPAD (chnex -> udpbind_lclipaddr, iopex -> u.udpbind.lclipaddr);
  CPYPORT (chnex -> udpbind_lclportno, iopex -> u.udpbind.lclportno);
  CPYIPAD (chnex -> udpbind_remipaddr, iopex -> u.udpbind.remipaddr);
  CPYPORT (chnex -> udpbind_remportno, iopex -> u.udpbind.remportno);
  if (ZERPORT (chnex -> udpbind_lclportno)) {
    for (ephem = udp_ephemnext; udp_ephemsocks[ephem-EPHEMMIN] == 0;) {
      ephem ++;
      if (ephem == EPHEMMAX) ephem = EPHEMMIN;
      if (ephem == udp_ephemnext) return (OZ_NOMOREPHEMPORTS);
    }
    udp_ephemnext = ephem + 1;
    if (udp_ephemnext == EPHEMMAX) udp_ephemnext = EPHEMMIN;
    udp_ephemsocks[ephem-EPHEMMIN] = 0;
    H2NW (ephem, chnex -> udpbind_lclportno);
    chnex -> udpbind_ephem = ephem;
  }

  chnex -> udpbind_iopqh = NULL;
  chnex -> udpbind_iopqt = &(chnex -> udpbind_iopqh);
  chnex -> udpbind_next  = udpbinds;
  chnex -> udp_threadid  = THREADID (iopex);
  udpbinds = chnex;

  return (OZ_SUCCESS);
}

/*************************************/
/*  Start transmitting udp datagram  */
/*************************************/

static uLong be_udptransmit (Iopex *iopex)

{
  Buf *buf;
  Hw *hw;
  Ippkt *ippkt;

  /* Make sure it's not going to Vermont (you can't get there from here) */

  hw = find_send_hw (iopex -> u.udptransmit.p.dstipaddr, iopex -> u.udptransmit.myipaddr, NULL, 1);
  if (hw == NULL) return (OZ_NOROUTETODEST);

  /* Make up an ip packet identification */

  while (++ ident == 0) {}
  iopex -> u.udptransmit.ident = ident;

  /* Increment channel's udp transmit count */

  iopex -> chnex -> udp_transcount ++;
  iopex -> chnex -> udp_threadid = THREADID (iopex);

  /* Process 'no fragmentation required' separately here */

  if (ippkt -> dat.udp.raw - (uByte *)ippkt + iopex -> u.udptransmit.p.rawsize <= hw -> mtu) {
    buf = mallocsndubuf (hw, udptransmit_mem, iopex, &(iopex -> msubp));
    if (buf != NULL) udptransmit_mem (buf, iopex);
    return (OZ_STARTED);
  }

  /* Fragmentation required */

  return (OZ_BUFFEROVF);
}

/* There is a buffer available, start transmitting datagram */

static void udptransmit_mem (Buf *buf, void *iopexv)

{
  Iopex *iopex;
  Ippkt *ippkt;
  uLong sts;
  uWord cksm, len;

  iopex = iopexv;

  sts = OZ_NOROUTETODEST;
  if (buf != NULL) {
    ippkt = IPPKT (buf -> hw, buf -> cxb);					/* point to ip packet */
    ippkt -> hdrlenver  = 0x45;							/* header length and version */
    ippkt -> typeofserv = 0;							/* type of service */
    len = ippkt -> dat.udp.raw + iopex -> u.udptransmit.p.rawsize - (uByte *)ippkt;
    H2NW (len, ippkt -> totalen);						/* total ip packet length (includes ip header, udp header and data) */
    H2NW (iopex -> u.udptransmit.ident, ippkt -> ident);			/* identification (used for re-assembly) */
    H2NW (0, ippkt -> flags);							/* no flags required */
    ippkt -> ttl   = 255;							/* time-to-live */
    ippkt -> proto = PROTO_IP_UDP;						/* protocol */
    CPYIPAD (ippkt -> srcipaddr, iopex -> u.udptransmit.myipaddr);		/* source ip address = local machine */
    CPYPORT (ippkt -> dat.udp.sport, iopex -> chnex -> udpbind_lclportno);	/* source port = port that we're bound to (if any) */
    CPYIPAD (ippkt -> dstipaddr, iopex -> u.udptransmit.p.dstipaddr);		/* destination ip address = as given */
    CPYPORT (ippkt -> dat.udp.dport, iopex -> u.udptransmit.p.dstportno);	/* destination udp port = as given */
    len = ippkt -> dat.udp.raw + iopex -> u.udptransmit.p.rawsize - (uByte *)&(ippkt -> dat.udp);
    H2NW (len, ippkt -> dat.udp.length);					/* udp length (includes udp header and length of data) */
    ACCESSBUFFS (iopex);	/* attach to requestor's process to access buffer */
    memcpy (ippkt -> dat.udp.raw, iopex -> u.udptransmit.p.rawbuff, iopex -> u.udptransmit.p.rawsize); /* copy in the data */
    H2NW (0, ippkt -> dat.udp.cksm);						/* set up new checksum */
    cksm = oz_dev_ip_udpcksm (ippkt);
    H2NW (cksm, ippkt -> dat.udp.cksm);
    sts = sendip (buf, udptransmit_done, iopex);				/* start sending it, post request when done */
  }
  if (sts != OZ_STARTED) oz_knl_iodone (iopex -> ioop, sts, NULL, NULL, NULL);
}

static void udptransmit_done (void *iopexv, uLong status)

{
  oz_knl_iodone (((Iopex *)iopexv) -> ioop, status, NULL, NULL, NULL);
}

/***********************************/
/*  Start receiving an udp packet  */
/***********************************/

static uLong be_udpreceive (Iopex *iopex)

{
  Chnex *chnex;

  chnex = iopex -> chnex;

  /* Make sure it is bound to a socket or we'll never get anything */

  if (chnex -> udpbind_iopqt == NULL) return (OZ_CHANOTBOUND);

  /* Put iopex on end of channel's read request queue */

  *(chnex -> udpbind_iopqt) = iopex;
  chnex -> udpbind_iopqt = &(iopex -> next);
  iopex -> next = NULL;

  chnex -> udp_recvcount ++;
  chnex -> udp_threadid = THREADID (iopex);

  return (OZ_STARTED);
}

/**************************/
/*  Get udp info, part 1  */
/**************************/

static uLong be_udpgetinfo1 (Iopex *iopex)

{
  Chnex *chnex;
  Iopex *riopex;

  chnex = iopex -> chnex;

  /* If udpbind_iopqt is NULL, it is not bound to an udp socket, so return zeroes */

  if (chnex -> udpbind_iopqt == NULL) {
    if (iopex -> u.udpgetinfo1.p.lclipaddr != NULL) CLRIPAD (iopex -> u.udpgetinfo1.p.lclipaddr);
    if (iopex -> u.udpgetinfo1.p.lclportno != NULL) CLRPORT (iopex -> u.udpgetinfo1.p.lclportno);
    if (iopex -> u.udpgetinfo1.p.remipaddr != NULL) CLRIPAD (iopex -> u.udpgetinfo1.p.remipaddr);
    if (iopex -> u.udpgetinfo1.p.remportno != NULL) CLRPORT (iopex -> u.udpgetinfo1.p.remportno);
    iopex -> u.udpgetinfo1.p.recvpend = -1;
  }

  /* It is bound, return what it is bound to */

  else {
    if (iopex -> u.udpgetinfo1.p.lclipaddr != NULL) CPYIPAD (iopex -> u.udpgetinfo1.p.lclipaddr, chnex -> udpbind_lclipaddr);
    if (iopex -> u.udpgetinfo1.p.lclportno != NULL) CPYPORT (iopex -> u.udpgetinfo1.p.lclportno, chnex -> udpbind_lclportno);
    if (iopex -> u.udpgetinfo1.p.remipaddr != NULL) CPYIPAD (iopex -> u.udpgetinfo1.p.remipaddr, chnex -> udpbind_remipaddr);
    if (iopex -> u.udpgetinfo1.p.remportno != NULL) CPYPORT (iopex -> u.udpgetinfo1.p.remportno, chnex -> udpbind_remportno);
    iopex -> u.udpgetinfo1.p.recvpend = 0;
    for (riopex = chnex -> udpbind_iopqh; riopex != NULL; riopex = riopex -> next) {
      iopex -> u.udpgetinfo1.p.recvpend ++;
    }
  }

  iopex -> u.udpgetinfo1.p.recvcount  = chnex -> udp_recvcount;
  iopex -> u.udpgetinfo1.p.transcount = chnex -> udp_transcount;
  iopex -> u.udpgetinfo1.p.threadid   = chnex -> udp_threadid;

  movc4 (sizeof iopex -> u.udpgetinfo1.p, &(iopex -> u.udpgetinfo1.p), iopex -> u.udpgetinfo1.as, iopex -> u.udpgetinfo1.ap);

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  TCP connection oriented I/O routines				*/
/*									*/
/************************************************************************/

/************************************************************************/
/*  Start connecting to a server					*/
/************************************************************************/

static uLong be_tcpconnect (Iopex *iopex)

{
  Buf *buf;
  Chnex *chnex;
  Hw *hw;
  Tcpcon *tcpcon;
  Sock lsockno, rsockno;
  uByte myipaddr[IPADDRSIZE];
  uLong seq, sts;
  uWord maxsendsize;

  chnex = iopex -> chnex;

  /* Can't already have a connection on this channel */

  if (chnex -> tcpcon != NULL) return (OZ_CHANALRBOUND);

  /* Find out if we have a way to get to the server and get my ip address associated with it */

  hw = find_send_hw (iopex -> u.tcpconnect.p.dstipaddr, myipaddr, NULL, 1);
  if (hw == NULL) return (OZ_NOROUTETODEST);

  /* Allocate an unused ephemeral socket number if caller did not supply one */

  lsockno = 0;
  if (iopex -> u.tcpconnect.p.srcportno != NULL) lsockno = N2HW (iopex -> u.tcpconnect.p.srcportno);
  rsockno = N2HW (iopex -> u.tcpconnect.p.dstportno);

  if (lsockno == 0) {
    lsockno = alloctcpephem (&seq);
    if (lsockno == 0) return (OZ_NOMOREPHEMPORTS);
  }

  /* An explicit socket number was given, make sure it is not in use */

  else {
    for (tcpcon = tcpcons; tcpcon != NULL; tcpcon = tcpcon -> next) {
      if ((tcpcon -> lsockno == lsockno) && (tcpcon -> rsockno == rsockno) 
       && CEQIPAD (tcpcon -> ripaddr, iopex -> u.tcpconnect.p.dstipaddr)) {
        return (OZ_SOCKNOINUSE);
      }
    }
    seq = oz_hw_tod_getnow ();
  }

  /* Fill in new connection block */

  sts = calloctcpcon (iopex, iopex -> u.tcpconnect.p.windowsize, iopex -> u.tcpconnect.p.windowbuff, &tcpcon);
  if (sts != OZ_SUCCESS) return (sts);

  tcpcon -> window99s              = iopex -> u.tcpconnect.p.window99s;
  tcpcon -> next                   = tcpcons;		/* next in tcpcons list */
  tcpcon -> rcvchnex               = chnex;		/* save pointer to receiving channel */
  tcpcon -> seq_nextusersendata    = seq;		/* this is sequence of first byte we will send out */
  tcpcon -> seq_lastrcvdack        = seq;		/* pretend the remote has acked all we set up to this point */
  tcpcon -> nextxmtseq             = seq;		/* next thing to transmit */
  tcpcon -> seq_lastrcvdwsize      = seq + MINWINSIZE;	/* pretend the remote has sent us a minimal wsize */
							/* ... so we will at least be able to send the SYN */
  tcpcon -> lastxmtreq             = &(tcpcon -> nextackreq);
  CPYIPAD (tcpcon -> lipaddr, myipaddr);		/* set up client's ip address based on what interface we are using */
  CPYIPAD (tcpcon -> ripaddr, iopex -> u.tcpconnect.p.dstipaddr); /* save server host's ip address */
  tcpcon -> lsockno                = lsockno;		/* save local (ephemeral) port number */
  tcpcon -> rsockno                = rsockno;		/* save remote server's port number */
  tcpcon -> maxsendsize            = TCP_MAXSENDSIZE (hw); /* maximum size to send in a datagram */
  tcpcon -> congestionwindow       = TCP_MAXSENDSIZE (hw);
  tcpcon -> slowstarthresh         = 65535;
  tcpcon -> retransmitinterval     = TPS;		/* initial retransmit interval = one second */
  tcpcon -> smoothedroundtrip      = TPS * 8;		/* corresponding smoothed round trip time (ticks * 8) */
  tcpcon -> timeout                = ticks + iopex -> u.tcpconnect.p.timeout / MSPT;
  if (iopex -> u.tcpconnect.p.timeout == 0) tcpcon -> timeout = -1;
  tcpcon -> tcprcv_iopqt           = &(tcpcon -> tcprcv_iopqh); /* initialize receive request queue */
  tcpcons = tcpcon;					/* link it on list */
  ntcpcons ++;
  chnex -> tcpcon = tcpcon;				/* remeber connection is in progress on the channel */

  if (debug &  DEBUG_ERRORS) {
    PRINTF "new outbound ");
    printtcpcon (tcpcon);
    PRINTF "\n");
  }

  validtcpxmtq (tcpcon);

  chnex -> tcp_threadid = THREADID (iopex);

  /* Start sending connection request to server.  If/when server acknowledges it, */
  /* request will be complete.  If it times out, the connection will be aborted.  */

  if (debug & DEBUG_IOPOST) PRINTF "oz_dev_ip*: starting tcpconnect iopex %p, tcpcon %p\n", iopex, tcpcon);

  memset (&(iopex -> u.tcptransmit), 0, sizeof iopex -> u.tcptransmit);	/* make up a transmit packet */
  iopex -> u.tcptransmit.p.rawsize = 1;					/* SYN messages consume 1 sequence number */
  iopex -> u.tcptransmit.syn       = TCP_FLAGS_SYN | (1 << TCP_FLAGS_HLNSHF); /* it is a SYN message */
  queuetcptransmit (iopex, tcpcon);					/* start sending it */
  return (OZ_STARTED);
}

/************************************************************************/
/*  Close the connection						*/
/************************************************************************/

static uLong be_tcpclose (Iopex *iopex)

{
  uLong sts;
  Tcpcon *tcpcon;

  tcpcon = iopex -> chnex -> tcpcon;					/* point to associated connection block */
  if (tcpcon == NULL) return (OZ_CHANOTBOUND);

  /* If abort style, just shut the connection down right now, wipe it all out */

  if (iopex -> u.tcpclose.p.abort) {
    if (tcpcon -> reset == 0) tcpcon -> reset = OZ_ABORTED;
    tcpterminate (tcpcon);
    return (OZ_SUCCESS);
  }

  /* If normal style, queue a transmit buffer that has rawsize=1,rawbuff=NULL.  The transmit  */
  /* routine (starttcpsend) will translate this into a FIN packet to be transmitted normally. */

  memset (&(iopex -> u.tcptransmit), 0, sizeof iopex -> u.tcptransmit);	/* otherwise, make up a transmit packet */
  iopex -> u.tcptransmit.p.rawsize = 1;					/* FIN messages consume 1 sequence number */
  iopex -> u.tcptransmit.syn       = TCP_FLAGS_FIN | TCP_FLAGS_ACK;	/* it is a FIN message */

  tcpcon -> tcpclosed = 1;						/* remember we closed the connection */
  queuetcptransmit (iopex, tcpcon);					/* send it, transmit routine will make a FIN packet */
  return (OZ_STARTED);
}

/************************************************************************/
/*  Start listening for an inbound connection				*/
/************************************************************************/

static uLong be_tcplisten (Iopex *iopex)

{
  Chnex *chnex;
  Tcpcon *tcpcon;
  uLong seq, sts;

  /* We can't already be either trying to connect to a server or listening for an inbound connection on this channel */

  chnex = iopex -> chnex;
  if (chnex -> tcpcon != NULL) return (OZ_CHANALRBOUND);

  /* Ok, start listening for an inbound connection */

  sts = calloctcpcon (iopex, iopex -> u.tcplisten.p.windowsize, iopex -> u.tcplisten.p.windowbuff, &tcpcon);
  if (sts != OZ_SUCCESS) return (sts);
  tcpcon -> lsockno = N2HW (iopex -> u.tcplisten.p.lclportno);		/* save local port number that clients will connect to (required) */
  if (tcpcon -> lsockno == 0) {
    tcpcon -> lsockno = alloctcpephem (&seq);				/*   none given, get an ephemeral socket number */
    if (tcpcon -> lsockno == 0) return (OZ_NOMOREPHEMPORTS);
  }
  else seq = oz_hw_tod_getnow ();
  if (iopex -> u.tcplisten.p.lclipaddr != NULL) {
    CPYIPAD (tcpcon -> lipaddr, iopex -> u.tcplisten.p.lclipaddr);	/* this is the ip address that they have to be connecting to (in case this computer */
  }									/* has more than one ip address and the caller only wants stuff for one of them) */
  if (iopex -> u.tcplisten.p.remportno != NULL) {
    tcpcon -> rsockno = N2HW (iopex -> u.tcplisten.p.remportno);	/* save remote port number (in case caller only wants connections from a specific remote port number) */
  }
  if (iopex -> u.tcplisten.p.remipaddr != NULL) {
    CPYIPAD (tcpcon -> ripaddr, iopex -> u.tcplisten.p.remipaddr);	/* save remote ip address (in case caller only wants connections from a specific remote ip address) */
  }

  tcpcon -> window99s           = iopex -> u.tcplisten.p.window99s;
  tcpcon -> lisiopex            = iopex;				/* we use this iopex to send the SYN/ACK message */
  tcpcon -> tcprcv_iopqt        = &(tcpcon -> tcprcv_iopqh);		/* initialize receive request queue */
  tcpcon -> lastxmtreq          = &(tcpcon -> nextackreq);		/* initialize transmit request queue */
  tcpcon -> rcvchnex            = chnex;				/* i/o channel that is doing the connect/listen */
  tcpcon -> timeout             = -1;					/* listens do not time out */

  tcpcon -> seq_nextusersendata = seq;					/* this is sequence of first byte we will send out */
  tcpcon -> seq_lastrcvdack     = seq;					/* pretend the remote has acked all we set up to this point */
  tcpcon -> nextxmtseq          = seq;					/* next thing to transmit */
  tcpcon -> seq_lastrcvdwsize   = seq;					/* wait for the remote to give us a window size when it connects */

  iopex  -> u.tcplisten.tcpcon  = tcpcon;
  iopex  -> next                = NULL;					/* link it to master list so when an inbound connection */
  iopex  -> u.tcplisten.prev    = tcplisqt;				/* ... packet arrives, we will see this listen request */
  chnex  -> tcpcon              = tcpcon;				/* remember we're doing a listen on this channel */
  *tcplisqt = iopex;
  tcplisqt  = &(iopex -> next);

  chnex -> tcp_threadid = THREADID (iopex);

  return (OZ_STARTED);
}

/************************************************************************/
/*  Start transmitting data to the remote end				*/
/*									*/
/*  This implementation does not post the request's completion until 	*/
/*  it has received the corresponding ack from the remote end.  This 	*/
/*  eliminates a memcpy as these routines do not have to keep a copy 	*/
/*  of the data for retransmission.  So to get good thruput for large 	*/
/*  amounts of data, the application should double-buffer transmits.	*/
/*									*/
/*  Note that you can queue transmits before the connection or listen 	*/
/*  request has completed, and the transmits will start as soon as the 	*/
/*  request completes.							*/
/************************************************************************/

static uLong be_tcptransmit (Iopex *iopex)

{
  Buf *buf;
  Tcpcon *tcpcon;

  /* Make sure we're connected or connecting to something */

  tcpcon = iopex -> chnex -> tcpcon;
  if (tcpcon == NULL) return (OZ_CHANOTBOUND);

  /* Make sure connection is not closed or aborted */

  if (tcpcon -> tcpclosed) return (OZ_LINKCLOSED);
  if (tcpcon -> reset != 0) return (tcpcon -> reset);

  /* Queue it for transmission */

  iopex -> chnex -> tcp_transcount ++;
  iopex -> chnex -> tcp_threadid = THREADID (iopex);
  iopex -> u.tcptransmit.syn = 0;
  queuetcptransmit (iopex, tcpcon);

  /* It will complete when all the data has been acked */

  return (OZ_STARTED);
}

/************************************************************************/
/*  The user is starting a tcp receive request.  We put the request on 	*/
/*  the end of the queue then check to see if any data is currently 	*/
/*  available.								*/
/*									*/
/*  Note that you can queue receives before the connection or listen 	*/
/*  request has completed.						*/
/************************************************************************/

static uLong be_tcpreceive (Iopex *iopex)

{
  Chnex *chnex;
  Tcpcon *tcpcon;

  chnex  = iopex -> chnex;
  tcpcon = chnex -> tcpcon;					/* make sure there is an active connection */
  if (tcpcon == NULL) return (OZ_CHANOTBOUND);
  if (tcpcon -> reset != 0) return (tcpcon -> reset);		/* and it hasn't been reset */
  *(tcpcon -> tcprcv_iopqt) = iopex;				/* ok, put receive request on the queue */
  tcpcon -> tcprcv_iopqt = &(iopex -> next);
  iopex -> next = NULL;
  if (debug & DEBUG_TCPRECV) PRINTF "be_tcpreceive: iopex %p\n", iopex);
  chnex -> tcp_recvcount ++;
  chnex -> tcp_threadid = THREADID (iopex);
  if (iopex -> u.tcpreceive.p.rawbuff == NULL) {		/* if 'no memcpy' style, ... */
    if (iopex -> u.tcpreceive.p.rawsize > tcpcon -> windowsize) iopex -> u.tcpreceive.p.rawsize = tcpcon -> windowsize;
    tcpcon -> rcvwindowrem += iopex -> u.tcpreceive.p.rawsize;	/* purge previously returned data from the window buffer */
    if (tcpcon -> rcvwindowrem > tcpcon -> rcvwindownxt) tcpcon -> rcvwindowrem = tcpcon -> rcvwindownxt;
    if (tcpcon -> rcvwindowrem >= tcpcon -> windowsize) {
      tcpcon -> rcvwindowrem -= tcpcon -> windowsize;
      tcpcon -> rcvwindownxt -= tcpcon -> windowsize;
      tcpcon -> rcvwindowins -= tcpcon -> windowsize;
    }
  }
  tcpreceive_test (tcpcon);					/* see if there is any data for it */
  tcpreplyack (tcpcon, 0);					/* maybe window has opened up, tell remote end to send more */
  return (OZ_STARTED);						/* it will (or has) completed via oz_knl_iodone */
}

/* This routine is called when a tcp buffer has been received (or the connection was closed or aborted). */
/* We scan the connection status and receive queue to see if any receive I/O requests are now complete.  */

static void tcpreceive_test (Tcpcon *tcpcon)

{
  Buf *buf;
  Chnex *chnex;
  Iopex *iopex;
  uLong len, nxt, sts;
  OZ_Iochan *iochan;

  chnex = tcpcon -> rcvchnex;				/* get chnex that has receive I/O requests waiting for data */
  if (chnex == NULL) return;				/* if none, the connection is being closed down */
  iochan = chnex -> iochan;				/* point to the corresponding I/O channel */
  OZ_KNL_CHKOBJTYPE (iochan, OZ_OBJTYPE_IOCHAN);	/* make sure it is still an I/O channel */

  if (debug & DEBUG_TCPRECV) PRINTF "tcpreceive_test: tcprcv_iopqh %p\n", tcpcon -> tcprcv_iopqh);

  /* Increment I/O chan reference count in case one of our calls to iodone ends up deassigning the channel, the iochan (and chnex) won't be freed off */

  oz_knl_iochan_increfc (iochan, 1);

  /* Repeat as long as there are receive I/O requests */

  while ((iopex = tcpcon -> tcprcv_iopqh) != NULL) {

    /* See how many bytes of contiguous valid data we have to process.  Valid data */
    /* start at rcvwindownxt and end just before rcvwindowins but may be wrapped.  */
    /* The data from rcvwindowrem..rcvwindownxt-1 have already been processed.     */

    nxt = tcpcon -> rcvwindownxt;
    len = tcpcon -> rcvwindowins - nxt;

    /* Check for data or finished or reset - any of which will 'complete' a receive request */

    if (debug & DEBUG_TCPRECV) {
      PRINTF "tcpreceive_test: len %u, rcvdfin %d, reset %u\n", len, tcpcon -> rcvdfin, tcpcon -> reset);
    }
    if ((len == 0) && !(tcpcon -> rcvdfin) && (tcpcon -> reset == 0)) break;

    /* Got both a request and an 'event', dequeue the I/O request */

    tcpcon -> tcprcv_iopqh = iopex -> next;
    if (tcpcon -> tcprcv_iopqh == NULL) tcpcon -> tcprcv_iopqt = &(tcpcon -> tcprcv_iopqh);

    /* If connection has been reset, return error status */

    if (tcpcon -> reset != 0) {
      if (debug & DEBUG_TCPRECV) PRINTF "tcpreceive_test: iodone %p sts %u\n", iopex, tcpcon -> reset);
      oz_knl_iodone (iopex -> ioop, tcpcon -> reset, NULL, NULL, NULL);
      continue;
    }

    /* If no data, it is finished, return end-of-file status                   */
    /* If there is data, we don't want to process the fin until it is all gone */

    if (len == 0) {
      if (debug & DEBUG_TCPRECV) PRINTF "tcpreceive_test: iodone %p eof\n", iopex);
      oz_knl_iodone (iopex -> ioop, OZ_ENDOFFILE, NULL, NULL, NULL);
      continue;
    }

    /* If caller wants data copied, don't copy more than caller can handle */
    /* If caller doesn't want data copied, then don't wrap off the end     */

    if (nxt >= tcpcon -> windowsize) nxt -= tcpcon -> windowsize;

    if (iopex -> u.tcpreceive.p.rawbuff != NULL) {
      if (len > iopex -> u.tcpreceive.p.rawsize) len = iopex -> u.tcpreceive.p.rawsize;
    } else {
      if (len > tcpcon -> windowsize - nxt) len = tcpcon -> windowsize - nxt;
    }

    /* Return length of data to caller */

    ACCESSBUFFS (iopex);
    sts = oz_knl_section_uput (iopex -> procmode, sizeof *(iopex -> u.tcpreceive.p.rawrlen), &len, iopex -> u.tcpreceive.p.rawrlen);

    /* Copy data from window buffer to user's buffer, if any */

    if ((iopex -> u.tcpreceive.p.rawbuff != NULL) && (sts == OZ_SUCCESS)) {
      if (len + nxt <= tcpcon -> windowsize) {
        sts = oz_knl_section_uput (iopex -> procmode, len, tcpcon -> windowbuff + nxt, iopex -> u.tcpreceive.p.rawbuff);
      } else {
        sts = oz_knl_section_uput (iopex -> procmode, tcpcon -> windowsize - nxt, 
                                   tcpcon -> windowbuff + nxt, iopex -> u.tcpreceive.p.rawbuff);
        if (sts == OZ_SUCCESS) {
          sts = oz_knl_section_uput (iopex -> procmode, len + nxt - tcpcon -> windowsize, tcpcon -> windowbuff, 
                                     iopex -> u.tcpreceive.p.rawbuff + tcpcon -> windowsize - nxt);
        }
      }
    }

    /* Advance buffer pointer beyond what was copied */

    tcpcon -> rcvwindownxt += len;

    /* If we actually did the memcpy, release those bytes for immediate re-use */

    if ((iopex -> u.tcpreceive.p.rawbuff != NULL) && (sts == OZ_SUCCESS)) {
      tcpcon -> rcvwindowrem = tcpcon -> rcvwindownxt;
      if (tcpcon -> rcvwindowrem >= tcpcon -> windowsize) {
        tcpcon -> rcvwindowrem -= tcpcon -> windowsize;
        tcpcon -> rcvwindownxt -= tcpcon -> windowsize;
        tcpcon -> rcvwindowins -= tcpcon -> windowsize;
      }
    }

    /* Post its completion */

    if (debug & DEBUG_TCPRECV) PRINTF "tcpreceive_test: iodone %p sts %u len %u nxt %u\n", iopex, sts, len, nxt);
    oz_knl_iodone (iopex -> ioop, sts, NULL, NULL, NULL);

    /* Repeat if there are more requests in the queue */
  }

  oz_knl_iochan_increfc (chnex -> iochan, -1);
}

/****************************************************************/
/* Get tcp info, part 1						*/
/****************************************************************/

static uLong be_tcpgetinfo1 (Iopex *iopex)

{
  Chnex *chnex;
  Iopex *xiopex;
  Tcpcon *tcpcon;

  chnex = iopex -> chnex;

  /* If tcpcon is not NULL, get the connection info from it */

  if ((tcpcon = chnex -> tcpcon) != NULL) {
    if (iopex -> u.tcpgetinfo1.p.lclipaddr != NULL) CPYIPAD (iopex -> u.tcpgetinfo1.p.lclipaddr, tcpcon -> lipaddr);
    if (iopex -> u.tcpgetinfo1.p.lclportno != NULL) H2NW (tcpcon -> lsockno, iopex -> u.tcpgetinfo1.p.lclportno);
    if (iopex -> u.tcpgetinfo1.p.remipaddr != NULL) CPYIPAD (iopex -> u.tcpgetinfo1.p.remipaddr, tcpcon -> ripaddr);
    if (iopex -> u.tcpgetinfo1.p.remportno != NULL) H2NW (tcpcon -> rsockno, iopex -> u.tcpgetinfo1.p.remportno);
    iopex -> u.tcpgetinfo1.p.rcvwindowsize  = tcpcon -> windowsize;
    iopex -> u.tcpgetinfo1.p.rcvwindowbuff  = tcpcon -> windowbuff;
    iopex -> u.tcpgetinfo1.p.rcvwindowpid   = 0;
    if (tcpcon -> windowprocess != NULL) iopex -> u.tcpgetinfo1.p.rcvwindowpid = oz_knl_process_getid (tcpcon -> windowprocess);
    iopex -> u.tcpgetinfo1.p.rcvwindowrem   = tcpcon -> rcvwindowrem;
    iopex -> u.tcpgetinfo1.p.rcvwindownxt   = tcpcon -> rcvwindownxt;
    iopex -> u.tcpgetinfo1.p.rcvwindowins   = tcpcon -> rcvwindowins;
    iopex -> u.tcpgetinfo1.p.maxsendsize    = tcpcon -> maxsendsize;
    iopex -> u.tcpgetinfo1.p.lastwsizesent  = tcpcon -> lastwsizesent;

    iopex -> u.tcpgetinfo1.p.seq_lastacksent   = tcpcon -> seq_lastacksent;
    iopex -> u.tcpgetinfo1.p.seq_receivedok    = tcpcon -> seq_receivedok;
    iopex -> u.tcpgetinfo1.p.seq_lastrcvdack   = tcpcon -> seq_lastrcvdack;
    iopex -> u.tcpgetinfo1.p.seq_nexttransmit  = tcpcon -> nextxmtseq;
    iopex -> u.tcpgetinfo1.p.seq_lastrcvdwsize = tcpcon -> seq_lastrcvdwsize;
    iopex -> u.tcpgetinfo1.p.seq_nextuserdata  = tcpcon -> seq_nextusersendata;

    for (xiopex = tcpcon -> nextackreq; xiopex != NULL; xiopex = xiopex -> next) {
      iopex -> u.tcpgetinfo1.p.transpend ++;
    }
    for (xiopex = tcpcon -> tcprcv_iopqh; xiopex != NULL; xiopex = xiopex -> next) {
      iopex -> u.tcpgetinfo1.p.recvpend ++;
    }

    if (tcpcon -> sentsyn)    iopex -> u.tcpgetinfo1.p.state |= OZ_IO_IP_TCPSTATE_SENTSYN;
    if (tcpcon -> rcvdsyn)    iopex -> u.tcpgetinfo1.p.state |= OZ_IO_IP_TCPSTATE_RCVDSYN;
    if (tcpcon -> sentfin)    iopex -> u.tcpgetinfo1.p.state |= OZ_IO_IP_TCPSTATE_SENTFIN;
    if (tcpcon -> rcvdfin)    iopex -> u.tcpgetinfo1.p.state |= OZ_IO_IP_TCPSTATE_RCVDFIN;
    if (tcpcon -> tcpclosed)  iopex -> u.tcpgetinfo1.p.state |= OZ_IO_IP_TCPSTATE_CLOSED;
    if (tcpcon -> reset != 0) iopex -> u.tcpgetinfo1.p.state |= OZ_IO_IP_TCPSTATE_RESET;
    if (tcpcon -> sentrst)    iopex -> u.tcpgetinfo1.p.state |= OZ_IO_IP_TCPSTATE_SENTRST;

    iopex -> u.tcpgetinfo1.p.seq_lastacksent   = tcpcon -> seq_lastacksent;
    iopex -> u.tcpgetinfo1.p.seq_receivedok    = tcpcon -> seq_receivedok;
    iopex -> u.tcpgetinfo1.p.seq_lastrcvdack   = tcpcon -> seq_lastrcvdack;
    iopex -> u.tcpgetinfo1.p.seq_nexttransmit  = tcpcon -> nextxmtseq;
    iopex -> u.tcpgetinfo1.p.seq_lastrcvdwsize = tcpcon -> seq_lastrcvdwsize;
    iopex -> u.tcpgetinfo1.p.seq_nextuserdata  = tcpcon -> seq_nextusersendata;
  }

  /* Otherwise, return zeroes */

  else {
    if (iopex -> u.tcpgetinfo1.p.lclipaddr != NULL) CLRIPAD (iopex -> u.tcpgetinfo1.p.lclipaddr);
    if (iopex -> u.tcpgetinfo1.p.lclportno != NULL) CLRPORT (iopex -> u.tcpgetinfo1.p.lclportno);
    if (iopex -> u.tcpgetinfo1.p.remipaddr != NULL) CLRIPAD (iopex -> u.tcpgetinfo1.p.remipaddr);
    if (iopex -> u.tcpgetinfo1.p.remportno != NULL) CLRPORT (iopex -> u.tcpgetinfo1.p.remportno);
    iopex -> u.tcpgetinfo1.p.recvpend = -1;
  }

  iopex -> u.tcpgetinfo1.p.recvcount  = chnex -> tcp_recvcount;
  iopex -> u.tcpgetinfo1.p.transcount = chnex -> tcp_transcount;
  iopex -> u.tcpgetinfo1.p.threadid   = chnex -> tcp_threadid;

  movc4 (sizeof iopex -> u.tcpgetinfo1.p, &(iopex -> u.tcpgetinfo1.p), iopex -> u.tcpgetinfo1.as, iopex -> u.tcpgetinfo1.ap);

  return (OZ_SUCCESS);
}

/*****************************/
/*  Set up a new tcp window  */
/*****************************/

static uLong be_tcpwindow (Iopex *iopex)

{
  Chnex *chnex;
  OZ_Process *windowprocess;
  OZ_Seclock *windowlock;
  Tcpcon *tcpcon;
  uByte *windowbuff;
  uLong seq, sts, windowsize;

  /* We have to have a connection block */

  chnex  = iopex -> chnex;
  tcpcon = chnex -> tcpcon;
  if (tcpcon == NULL) return (OZ_CHANOTBOUND);

  /* Allocate or lock the new window */

  windowsize = iopex -> u.tcpwindow.p.windowsize;				/* see what new window size they want */
  windowbuff = iopex -> u.tcpwindow.p.windowbuff;				/* see where the new window buffer is */

  if (windowbuff == NULL) {							/* see if user gave us a buffer to use */
    windowlock = NULL;								/* if not, nothing to lock in memory */
    if (windowsize == 0) windowsize = DEFWINSIZE;				/* maybe we need to use the default window size */
    else if (windowsize < MINWINSIZE) windowsize = MINWINSIZE;			/* but always at least use the minimum */
    if (windowsize < tcpcon -> rcvwindowins - tcpcon -> rcvwindowrem) return (OZ_BUFFEROVF); /* not big enuf to hold what we have in there now */
    windowbuff = OZ_KNL_PGPMALLOQ (sizeof *tcpcon + windowsize);		/* allocate it */
    if (windowbuff == NULL) return (OZ_EXQUOTAPGP);				/* ran out of quota, probably should have given us a windowbuff */
    windowprocess = NULL;							/* remember it is from pool, not in any process' address space */
  } else {
    if (windowsize < MINWINSIZE) return (OZ_BADPARAM);				/* it must have some useful size */
    if (windowsize < tcpcon -> rcvwindowins - tcpcon -> rcvwindowrem) return (OZ_BUFFEROVF); /* not big enuf to hold what we have in there now */
    windowprocess = oz_knl_ioop_getprocess (iopex -> ioop);			/* i/o process is how we access the window */
    if ((tcpcon -> windowprocess != NULL) && (tcpcon -> windowprocess != windowprocess)) return (OZ_CROSSPROCBUFF); /* has to be same process as old one */
    sts = oz_knl_section_iolockw (iopex -> procmode, windowsize, windowbuff, &windowlock, NULL, NULL, NULL); /* lock it in memory */
    if (sts != OZ_SUCCESS) return (sts);					/* its address is bad or something like that */
    oz_knl_process_increfc (windowprocess, 1);
  }

  /* Copy old window data to new window */

  if (tcpcon -> rcvwindowins <= tcpcon -> windowsize) {
    memcpy (windowbuff, tcpcon -> windowbuff + tcpcon -> rcvwindowrem, tcpcon -> rcvwindowins - tcpcon -> rcvwindowrem);
  } else {
    memcpy (windowbuff, tcpcon -> windowbuff + tcpcon -> rcvwindowrem, tcpcon -> windowsize - tcpcon -> rcvwindowrem);
    memcpy (windowbuff + tcpcon -> windowsize - tcpcon -> rcvwindowrem, tcpcon -> windowbuff, tcpcon -> rcvwindowins - tcpcon -> windowsize);
  }

  /* Free off old window */

  if (tcpcon -> windowprocess != NULL) {					/* see if it was in some process' address space */
    oz_knl_section_iounlk (tcpcon -> windowlock);				/* ok, unlock the memory */
    oz_knl_process_increfc (tcpcon -> windowprocess, -1);			/* we don't need the process pointer anymore */
  } else if (tcpcon -> windowbuff != (((uByte *)tcpcon) + sizeof *tcpcon)) {	/* see if separately allocated pool */
    OZ_KNL_PGPFREE (tcpcon -> windowbuff);					/* if so, free it off */
  }

  /* Set the new window in the connection block */

  tcpcon -> rcvwindowins -= tcpcon -> rcvwindowrem;				/* all data is now at the beginning of windowbuff */
  tcpcon -> rcvwindownxt -= tcpcon -> rcvwindowrem;
  tcpcon -> rcvwindowrem  = 0;

  tcpcon -> windowsize    = windowsize;						/* save the base window size */
  tcpcon -> windowbuff    = windowbuff;						/* save the base address of the window buffer */
  tcpcon -> windowlock    = windowlock;						/* save how to unlock it */
  tcpcon -> windowprocess = windowprocess;					/* this is the process that owns the buffer */
  tcpcon -> window99s     = iopex -> u.tcpwindow.p.window99s;
  return (OZ_SUCCESS);								/* successful */
}

/*********************************/
/* Allocate TCP ephemeral socket */
/*********************************/

static Sock alloctcpephem (uLong *seq_r)

{
  Sock lsockno;

  for (lsockno = tcp_ephemnext; tcp_ephemsocks[lsockno-EPHEMMIN] == 0;) {	/* scan for non-zero tcp_ephemsocks[i] element */
    lsockno ++;
    if (lsockno == EPHEMMAX) lsockno = EPHEMMIN;
    if (lsockno == tcp_ephemnext) return (0);
  }
  tcp_ephemnext = lsockno + 1;							/* save what to start with next time */
  if (tcp_ephemnext == EPHEMMAX) tcp_ephemnext = EPHEMMIN;
  *seq_r = tcp_ephemsocks[lsockno-EPHEMMIN];					/* get the starting sequence */
  tcp_ephemsocks[lsockno-EPHEMMIN] = 0;						/* tell next guy this one is in use */
  return (lsockno);
}

/************************************************************************/
/*									*/
/*  Abort stuff on a channel						*/
/*									*/
/*    Input:								*/
/*									*/
/*	abort = I/O requests to be aborted				*/
/*									*/
/************************************************************************/

static void abortproc (Abort *abort)

{
  Chnex *chnex, **lchnex, *xchnex;
  Iopex *iopex, **liopex;
  Tcpcon *tcpcon;
  uLong seq;

  chnex = abort -> chnex;

  /* Abort IP receives */

  if (chnex -> ipbind_iopqt != NULL) {
    for (liopex = &(chnex -> ipbind_iopqh); (iopex = *liopex) != NULL;) {
      if (abort -> deassign || oz_knl_ioabortok (iopex -> ioop, abort -> iochan, abort -> procmode, abort -> ioop)) {
        *liopex = iopex -> next;
        oz_knl_iodone (iopex -> ioop, OZ_ABORTED, NULL, NULL, NULL);
      } else {
        liopex = &(iopex -> next);
      }
    }
    chnex -> ipbind_iopqt = liopex;
  }

  /* Abort UDP receives */

  if (chnex -> udpbind_iopqt != NULL) {
    for (liopex = &(chnex -> udpbind_iopqh); (iopex = *liopex) != NULL;) {
      if (abort -> deassign || oz_knl_ioabortok (iopex -> ioop, abort -> iochan, abort -> procmode, abort -> ioop)) {
        *liopex = iopex -> next;
        oz_knl_iodone (iopex -> ioop, OZ_ABORTED, NULL, NULL, NULL);
      } else {
        liopex = &(iopex -> next);
      }
    }
    chnex -> udpbind_iopqt = liopex;
  }

  /* If abort tcp listening, abort the whole connection */

  tcpcon = chnex -> tcpcon;
  if (tcpcon != NULL) {
    iopex = tcpcon -> lisiopex;
    if ((iopex != NULL) && oz_knl_ioabortok (iopex -> ioop, abort -> iochan, abort -> procmode, abort -> ioop)) {
      tcpterminate (tcpcon);
      tcpcon = NULL;
    }
  }

  /* Abort any TCP receives in progress */

  if (tcpcon != NULL) {
    for (liopex = &(tcpcon -> tcprcv_iopqh); (iopex = *liopex) != NULL;) {
      if (abort -> deassign || oz_knl_ioabortok (iopex -> ioop, abort -> iochan, abort -> procmode, abort -> ioop)) {
        *liopex = iopex -> next;
        if (debug & DEBUG_TCPRECV) PRINTF "abortproc: iopex %p abort\n", iopex);
        oz_knl_iodone (iopex -> ioop, OZ_ABORTED, NULL, NULL, NULL);
      } else {
        liopex = &(iopex -> next);
      }
    }
    tcpcon -> tcprcv_iopqt = liopex;
  }

  /* Abort any TCP transmits in progress.  This may leave holes in stuff that hasn't been acked yet, so we   */
  /* reset the connection if asked for a retransmit.  The connection is probably about to be aborted anyway. */

  if (tcpcon != NULL) {
    validtcpxmtq (tcpcon);
    for (liopex = &(tcpcon -> nextackreq); (iopex = *liopex) != NULL;) {
      if (abort -> deassign || oz_knl_ioabortok (iopex -> ioop, abort -> iochan, abort -> procmode, abort -> ioop)) {
        *liopex = iopex -> next;
        tcpcon -> numxmtreqs --;
        oz_knl_iodone (iopex -> ioop, OZ_ABORTED, NULL, NULL, NULL);
      } else {
        liopex = &(iopex -> next);
      }
    }
    tcpcon -> lastxmtreq = liopex;
    validtcpxmtq (tcpcon);
  }

  /* If deassigning, wipe everything out of the channel */

  if (abort -> deassign) {

    /* Undo any OZ_IO_IP_IPBIND */

    if (chnex -> ipbind_iopqt != NULL) {
      for (lchnex = &ipbinds; (xchnex = *lchnex) != NULL; lchnex = &(xchnex -> ipbind_next)) {
        if (xchnex == chnex) {
          *lchnex = xchnex -> ipbind_next;
          chnex -> ipbind_iopqt = NULL;
          break;
        }
      }
    }

    /* Undo any OZ_IO_IP_UDPBIND */

    if (chnex -> udpbind_iopqt != NULL) {
      if (chnex -> udpbind_ephem != 0) {
        udp_ephemsocks[chnex->udpbind_ephem-EPHEMMIN] = 1;
        chnex -> udpbind_ephem = 0;
      }
      for (lchnex = &udpbinds; (xchnex = *lchnex) != NULL; lchnex = &(xchnex -> udpbind_next)) {
        if (xchnex == chnex) {
          *lchnex = xchnex -> udpbind_next;
          chnex -> udpbind_iopqt = NULL;
          while ((iopex = xchnex -> udpbind_iopqh) != NULL) {
            chnex -> udpbind_iopqh = iopex -> next;
            oz_knl_iodone (iopex -> ioop, OZ_ABORTED, NULL, NULL, NULL);
          }
          break;
        }
      }
    }

    /* Abort any TCP listening / connecting / receiving and abort any open connection */

    if (tcpcon != NULL) tcpterminate (tcpcon);
  }

  /* Free off abort block.  Decrement ref count (maybe ip_deassign gets called back). */

  oz_knl_iochan_increfc (abort -> iochan, -1);
  OZ_KNL_NPPFREE (abort);
}

/************************************************************************/
/*									*/
/*  This routine is called once a clock tick				*/
/*									*/
/************************************************************************/

static void timerproc (void)

{
  Arpc *arpc, **larpc;
  Buf *buf, **lbuf, **lxbuf, *xbuf;
  Hw *hw;
  int sentsomething;
  Iopex *iopex;
  Ippkt *ippkt;
  Long lockprev;
  uLong cwsize, howlong, seqbeg, when;
  Tcpcon *tcpcon;

  /* Get rid of all partial fragments that have expired */

  for (lbuf = &frags; (buf = *lbuf) != NULL;) {
    if (buf -> timeout > ticks) lbuf = &(buf -> next);
    else {
      ippkt = IPPKT (buf -> hw, buf -> cxb);
      if (debug & DEBUG_ERRORS) {
        PRINTF "expired fragment from ");
        printipaddr (ippkt -> srcipaddr);
        PRINTF "\n");
      }
      *lbuf = buf -> next;
      do {
        if (debug & DEBUG_ERRORS) PRINTF "  totalen 0x%x, flags 0x%x\n", N2HW (ippkt -> totalen), N2HW (ippkt -> flags));
        xbuf = buf -> frag;
        sendicmpbounce (buf, PROTO_IP_ICMP_TIMEEXC, 1, 0);
        buf = xbuf;
      } while (buf != NULL);
    }
  }

  /* Delete any transmit buffers that have waited too long for an arp */

  for (hw = hws; hw != NULL; hw = hw -> next) {
    for (lbuf = &(hw -> arpwaits); (buf = *lbuf) != NULL;) {
      if (buf -> timeout > ticks) lbuf = &(buf -> next);
      else {
        if (debug & DEBUG_ERRORS) {
          PRINTF "arp request didn't complete for ");
          printipaddr (IPPKT (buf -> hw, buf -> cxb) -> dstipaddr);
          PRINTF "\n");
        }
        *lbuf = buf -> next;
        buf -> iosts = OZ_ARPTIMEOUT;
        sendcomplete (buf);
      }
    }
  }

  /* Process any per-tcp connection items */

scan_tcpcons:
  for (tcpcon = tcpcons; tcpcon != NULL; tcpcon = tcpcon -> next) {

    /* Abort any timed-out outbound connection attempts */

    if ((tcpcon -> timeout < ticks) && (tcpcon -> reset == 0)) {
      if (debug & DEBUG_ERRORS) {
        PRINTF "tcp outbound connection ");
        printtcpcon (tcpcon);
        PRINTF " timed out\n");
      }
      if (tcpcon -> reset == 0) tcpcon -> reset = OZ_CONNECTFAIL;
    }

    /* Resend any tcp packets that have taken too long to be acked */

    sentsomething = 0;
    if (tcpcon -> reset == 0) {

      /* If there are any buffers on 'nextackreq' that have reached their timeout, resend them */

      validtcpxmtq (tcpcon);
      when  = ticks - tcpcon -> retransmitinterval;				/* had to be transmitted before this */
      iopex = tcpcon -> nextackreq;						/* check the oldest buffer */
      if ((iopex != NULL) && (iopex -> u.tcptransmit.lasttimesent <= when)) {
        tcpcon -> nextxmtseq = tcpcon -> seq_lastrcvdack;			/* if it timed out, resend all that aren't acked */
        validtcpxmtq (tcpcon);
        cwsize = tcpcon -> congestionwindow;					/* adjust slow start threshold */
        if (cwsize < tcpcon -> maxsendsize) cwsize = tcpcon -> maxsendsize;
        tcpcon -> slowstarthresh      = cwsize / 2;
        tcpcon -> congestionwindow    = tcpcon -> maxsendsize;			/* reset congestion window so we only retransmit one packet */
        tcpcon -> retransmitinterval += tcpcon -> retransmitinterval / 8 + 1;	/* inc timeout */
        if (debug & DEBUG_TCPFLOW) {
          PRINTF "tcp ");
          printtcpcon (tcpcon);
          PRINTF " rti incd to %u\n  slowstarthresh %u, congestionwindow %u\n", 
                  tcpcon -> retransmitinterval, tcpcon -> slowstarthresh, tcpcon -> congestionwindow);
        }
        sentsomething = starttcpsend (NULL, tcpcon);				/* start sending */
      }
    }

    /* Do the persist timer - */
    /* If there are buffers waiting-for-wsize, transmit an ack if  */
    /* we haven't seen a non-zero wsize from remote end in a while */

    if ((tcpcon -> reset == 0) && !sentsomething && ((Long)(tcpcon -> nextxmtseq - tcpcon -> seq_nextusersendata) < 0)) {
      howlong = ticks - tcpcon -> lasttimenzwsizercvd;
      if ((howlong == TPS + (TPS / 2)) 			/* first 1.5 sec */
       || (howlong == TPS *  3) 			/* or first  3 sec */
       || (howlong == TPS *  6) 			/* or first  6 sec */
       || (howlong == TPS * 12) 			/* or first 12 sec */
       || (howlong == TPS * 24) 			/* or first 24 sec */
       || (howlong == TPS * 48) 			/* or first 48 sec */
       || ((howlong >= TPS * 60) 			/* every 60 seconds */
        && (howlong % (TPS * 60) == 0))) {
        if (debug & DEBUG_ERRORS) {
          PRINTF "tcp persist %u ", howlong);
          printtcpcon (tcpcon);
          PRINTF "\n");
          PRINTF "  congestionwindow %x  seq_lastrcvdwsize %x\n", tcpcon -> congestionwindow, tcpcon -> seq_lastrcvdwsize);
          PRINTF "  seq_lastrcvdack %x\n", tcpcon -> seq_lastrcvdack);
        }
        buf = mallocsndkbuf (find_send_hw (tcpcon -> ripaddr, NULL, NULL, 1));
        if (buf != NULL) {
          sendtcp (buf, tcpcon, 0, TCP_FLAGS_ACK | TCP_FLAGX_KA, tcpcon -> seq_receivedok);
          sentsomething = 1;
        }
      }
    }

    /* Do the keepalive timer - */
    /* If we haven't heard anything in a while, send an ack  */
    /* that will force an ack to be sent back.  This way we  */
    /* can tell if the remote system is no longer reachable. */

    if ((tcpcon -> reset == 0) && !sentsomething) {
      howlong = ticks - tcpcon -> lasttimemsgrcvd;
      if ((howlong == TPS *  600) 		/* at 10 mins */
       || (howlong == TPS *  720) 		/* at 12 mins */
       || (howlong == TPS *  840) 		/* at 14 mins */
       || (howlong == TPS *  960) 		/* at 16 mins */
       || (howlong == TPS * 1080)) {		/* at 18 mins */
        if (debug & DEBUG_ERRORS) {
          PRINTF "tcp keepalive %u ", howlong);
          printtcpcon (tcpcon);
          PRINTF "\n");
        }
        buf = mallocsndkbuf (find_send_hw (tcpcon -> ripaddr, NULL, NULL, 1));
        if (buf != NULL) {
          sendtcp (buf, tcpcon, 0, TCP_FLAGS_ACK | TCP_FLAGX_KA, tcpcon -> seq_receivedok);
          sentsomething = 1;
        }
      }
      if (howlong == TPS * 1200) {		/* abort after 20 mins */
        if (debug & DEBUG_ERRORS) {
          PRINTF "tcp keepalive %u abort ", howlong);
          printtcpcon (tcpcon);
          PRINTF "\n");
        }
        if (tcpcon -> reset == 0) tcpcon -> reset = OZ_LINKDROPPED;
      }
    }

    /* Maybe we received some data that can be acked now */

    tcpreplyack (tcpcon, 0);

    /* Just check to see if it is now dead - if so, delete it and re-scan */

    if (checktcpdead (tcpcon)) goto scan_tcpcons;
  }

  /* Clean out arp cache */

  for (hw = hws; hw != NULL; hw = hw -> next) {					/* scan all ethernet hardware devices */
    validatearpcs (hw, __LINE__);
    for (larpc = &(hw -> arpcs); (arpc = *larpc) != NULL;) {			/* scan the arp cache */
      if (!OZ_HW_READABLE (1, arpc, OZ_PROCMODE_KNL)) {
        oz_knl_printk ("oz_dev_ip %d*: arpc %p, larpc %p\n", __LINE__, arpc, larpc);
        oz_knl_dumpmem (sizeof *hw, hw);
      }
      if (arpc -> timeout > ticks) larpc = &(arpc -> next);			/* on to next entry if it hasn't timed out yet */
      else {
        if (debug & DEBUG_ARPCACHE) {
          PRINTF "delete old arpc  ");
          printenaddr (hw, arpc -> enaddr);
          PRINTF " <-> ");
          printipaddr (arpc -> ipaddr);
          PRINTF "\n");
        }
        *larpc = arpc -> next;							/* entry has timed out, remove and free it */
        freearpc (arpc);
        hw -> arpcount --;
      }
    }
    validatearpcs (hw, __LINE__);
  }

  /* Resend arp requests for those that didn't respond */

  if (ticks % ARP_RETRY == 0) {
    for (hw = hws; hw != NULL; hw = hw -> next) {				/* scan each ethernet device */
      for (buf = hw -> arpwaits; buf != NULL; buf = buf -> next) {		/* loop through all that are waiting for arps */
        for (xbuf = buf -> next; xbuf != NULL; xbuf = xbuf -> next) {		/* only do unique ip addresses */
          if (CEQIPAD (buf -> sendipaddr, xbuf -> sendipaddr)) break;
        }
        if (xbuf == NULL) sendarpreq (buf -> hw, IPPKT (hw, buf -> cxb) -> srcipaddr, buf -> sendipaddr);
      }
    }
  }
}

/************************************************************************/
/*									*/
/*  Start reading a packet from ethernet				*/
/*									*/
/*    Input:								*/
/*									*/
/*	buf = pointer to buf to read into				*/
/*									*/
/************************************************************************/

static void startread (Buf *buf)

{
  Hw *hw;
  uLong sts;
  OZ_IO_ether_receive ether_receive;
  OZ_Iochan *iochan;

  /* Determine if ARP or IP channel */

  hw = buf -> hw;
  iochan = NULL;
  if (buf -> type == BUF_TYPE_RCV_ARP) iochan = hw -> iochan_arp;
  if (buf -> type == BUF_TYPE_RCV_IP)  iochan = hw -> iochan_ip;
  if (iochan == NULL) oz_crash ("oz_dev_ip startread: bad buffer type %d", buf -> type);

  /* Start reading - ethernet driver will fill in buf -> cxb with its internal buffer */

  memset (&ether_receive, 0, sizeof ether_receive);
  ether_receive.rcvfre   = buf -> rcvdrv;		/* free off old buffer, if any */
  ether_receive.rcvdrv_r = &(buf -> rcvdrv);		/* this is where it can put its pointer */
  ether_receive.rcveth_r = &(buf -> cxb);		/* this is where it must put cxb pointer */
  ether_receive.dlen     = &(buf -> cxb_dlen);		/* this is where it must return received data length */
  buf -> rcvdrv = NULL;					/* it's supposedly freeing the old buf, so forget about it */
  buf -> cxb    = NULL;
  hw -> receivesinprog ++;				/* one more read to wait for if terminating */
  sts = oz_knl_iostart2 (1, iochan, OZ_PROCMODE_KNL, readast, buf, NULL, NULL, NULL, NULL, 
                         OZ_IO_ETHER_RECEIVE, sizeof ether_receive, &ether_receive);
  if (sts != OZ_STARTED) readast (buf, sts);
}

/* This ast routine is called when a buffer has been received */
/* It queues it to the kernel thread for processing           */

static void readast (void *bufv, uLong status)

{
  Buf *buf;
  uLong vl;

  buf = bufv;

  buf -> next  = NULL;
  buf -> iosts = status;
  vl = oz_hw_smplock_wait (&smplock_vl);
  *rcvqt = buf;
  rcvqt  = &(buf -> next);
  oz_hw_smplock_clr (&smplock_vl, vl);

  oz_knl_event_set (ipevent, 1);
}

/************************************************************************/
/*									*/
/*  ARP read complete routine						*/
/*									*/
/************************************************************************/

static void arpreadproc (Buf *buf)

{
  uWord proto;

  proto = proc_ether (buf);				/* process ethernet header */
  if (proto == PROTO_ARP) buf = proc_arp (buf);		/* if ARP protocol, go process it */
  else if (debug & DEBUG_ERRORS) PRINTF "  non-arp packet (%4.4x) received on arp channel\n", proto);
  if (buf != NULL) freebuf (buf);			/* anyway, start a replacement read */
}

/************************************************************************/
/*									*/
/*  IP read complete ast routine					*/
/*									*/
/************************************************************************/

#define EXP_TCP_FL_MSK (TCP_FLAGS_FIN | TCP_FLAGS_SYN | TCP_FLAGS_RST | TCP_FLAGS_ACK | TCP_FLAGS_URG | TCP_FLAGS_HLNMSK)
#define EXP_TCP_FL_VAL (TCP_FLAGS_ACK | ((IPPKT (cxb) -> dat.tcp.raw - IPPKT (cxb) -> dat.raw) << (TCP_FLAGS_HLNSHF - 2)))

static void ipreadproc (Buf *buf)

{
  Chnex *chnex;
  Cxb *cxb, *xcxb;
  Iopex *iopex;
  Ippkt *ippkt;
  uLong size, sts, totalen;
  Sock dport, sport;
  Tcpcon *tcpcon;
  uWord proto, wsize;

  proto = proc_ether (buf);
  cxb   = buf -> cxb;
  ippkt = IPPKT (buf -> hw, cxb);

  /* Dump out tcp packet if enabled */

  tcpsum ('r', ippkt);

  /* Better be IP protocol */

  if (proto != PROTO_IP) {
    if (debug & DEBUG_ERRORS) PRINTF "  non-ip packet (%4.4x) received on ip channel\n", proto);
    goto startnewread;
  }

  /* See if it matches an IPBIND channel - copy it out if so */

  for (chnex = ipbinds; chnex != NULL; chnex = chnex -> ipbind_next) {

    /* See if this channel's ipbind info matches the incoming packet and it has a pending ipreceive request */

    iopex = chnex -> ipbind_iopqh;
    if ((chnex -> ipbind_proto != 0) && (chnex -> ipbind_proto != ippkt -> proto)) continue;
    if (!ZERIPAD (chnex -> ipbind_lclipaddr) && !CEQIPAD (chnex -> ipbind_lclipaddr, ippkt -> dstipaddr)) continue;
    if (!ZERIPAD (chnex -> ipbind_remipaddr) && !CEQIPAD (chnex -> ipbind_remipaddr, ippkt -> srcipaddr)) continue;
    if (iopex == NULL) continue;

    /* Ok, unlink the ipreceive request */

    chnex -> ipbind_iopqh = iopex -> next;
    if (chnex -> ipbind_iopqh == NULL) chnex -> ipbind_iopqt = &(chnex -> ipbind_iopqh);

    /* Copy ip datagram to caller's buffer */

    ACCESSBUFFS (iopex);					/* attach to caller's address space */
    size = N2HW (ippkt -> totalen);				/* size of packet received, including ip header */
    sts  = OZ_SUCCESS;
    if (size > iopex -> u.ipreceive.p.pktsize) {
      size = iopex -> u.ipreceive.p.pktsize;			/* if too big for user's buffer, chop it off */
      sts  = OZ_BUFFEROVF;
    }
    memcpy (iopex -> u.ipreceive.p.ippkt, ippkt, size);		/* copy to user's buffer */

    /* Post request complete */

    oz_knl_iodone (iopex -> ioop, sts, NULL, NULL, NULL);

    /* If we don't pass the message on for normal processing, recycle the buffer */

    if (!(chnex -> ipbind_passiton)) goto startnewread;
  }

  /* Check for TCP/IP read data express processing - */
  /*       this gets incoming TCP/IP read data going */
  /*     as soon as possible under common conditions */

#if 00
  if (IPPKT (cxb) -> hdrlenver != 0x45) goto no_tcp_exp;			/* it's correct IP version and length */
  if (!ismyipaddr (IPPKT (cxb) -> dstipaddr, buf -> hw)) goto no_tcp_exp;	/* it's headed for my ip address */
  if (N2HW (IPPKT (cxb) -> flags) & (IP_FLAGS_MF | IP_FLAGS_OFFS)) goto no_tcp_exp; /* it isn't fragmented */
  if (IPPKT (cxb) -> proto != PROTO_IP_TCP) goto no_tcp_exp;			/* it's a TCP packet */
  if ((N2HW (IPPKT (cxb) -> dat.tcp.flags) & EXP_TCP_FL_MSK) 			/* no weird flags or options */
                                         != EXP_TCP_FL_VAL) goto no_tcp_exp;
  sport = N2HW (IPPKT (cxb) -> dat.tcp.sport);					/* ok, get port number pairs */
  dport = N2HW (IPPKT (cxb) -> dat.tcp.dport);
  for (tcpcon = tcpcons; tcpcon != NULL; tcpcon = tcpcon -> next) {		/* search active connection list */
    if (tcpcon -> lsockno != dport) continue;
    if (tcpcon -> rsockno != sport) continue;
    if (!CEQIPAD (tcpcon -> lipaddr, IPPKT (cxb) -> dstipaddr)) continue;
    if (CEQIPAD (tcpcon -> ripaddr, IPPKT (cxb) -> srcipaddr)) break;
  }
  if (tcpcon == NULL) goto no_tcp_exp;						/* must be an active connection */
  if (!(tcpcon -> rcvdsyn)) goto no_tcp_exp;					/* must have already received TCP_FLAGS_SYN */
  if (tcpcon -> rcvdfin) goto no_tcp_exp;					/* must not have received TCP_FLAGS_FIN */
  if (tcpcon -> reset != 0) goto no_tcp_exp;					/* must not have been reset */
  if (tcpcon -> dupacksinarow != 0) goto no_tcp_exp;				/* must not be doing fancy flow control stuff */
  if (tcpcon -> waitingforwsizeqh != NULL) goto no_tcp_exp;			/* must not have anything waiting for windowsize */
  if (tcpcon -> rcvdoutofordr != NULL) goto no_tcp_exp;				/* must not be any packets received out of order */
  if (N2HL (IPPKT (cxb) -> dat.tcp.ack) != tcpcon -> seq_lastrcvdack) goto no_tcp_exp; /* this packet must not ack anything I sent earlier */
  if (tcpbegseq (cxb) != tcpcon -> seq_receivedok) goto no_tcp_exp;		/* this packet must be the next one in order received */
  if (oz_dev_ip_ipcksm (IPPKT (cxb)) != 0xffff) goto no_tcp_exp;		/* it's ip checksum must be good */
  if (oz_dev_ip_tcpcksm (IPPKT (cxb)) != 0xffff) goto no_tcp_exp;		/* it's tcp checksum must be good */
  tcpcon -> lasttimemsgrcvd = ticks;						/* ... remember when we last received something */
  wsize = N2HW (IPPKT (cxb) -> dat.tcp.wsize);					/* ... remember the windowsize */
  if (wsize != 0) tcpcon -> lasttimenzwsizercvd = ticks;
  tcpcon -> lastrcvdwsize = wsize;
  tcpcon -> seq_receivedok = tcpendseq (cxb);					/* ... set up end sequence that's been received ok */
  buf -> tcprawlen = tcprawlen (cxb);						/* ... save usable data length */
  if (buf -> tcprawlen != 0) {							/* ... check for lone ack */
    buf -> tcprawpnt = IPPKT (buf -> cxb) -> dat.tcp.raw;			/* ... save usable data pointer */
    tcpcon -> windowsize -= buf -> tcprawlen;					/* ... decrement available window size */
    buf -> next = NULL;								/* ... add buffer to end of blocked queue */
    *(tcpcon -> rcvblockedqt) = buf;
    tcpcon -> rcvblockedqt = &(buf -> next);
    if (debug & DEBUG_TCPRECV) PRINTF "ipreadproc: %p => rcvblockedq\n", buf);
    tcpcon -> expressreceive ++;						/* ... increment express statistics counter */
    tcpreceive_test (tcpcon);							/* ... start processing it */
    tcpreplyack (tcpcon, 0);
    checktcpdead (tcpcon);							/* now maybe free off tcpcon */
    buf = NULL;									/* ... go start a replacement read */
  }
  goto startnewread;
no_tcp_exp:
#endif

  /* Otherwise, process the hard way */

  buf = proc_ip (buf);

  /* Start a replacement read */

startnewread:
  if (buf != NULL) freebuf (buf);
}

/************************************************************************/
/*									*/
/*  Process ethernet header						*/
/*									*/
/************************************************************************/

static Eproto proc_ether (Buf *buf)

{
  Cxb *cxb;
  Eproto eproto;
  Hw *hw;

  cxb = buf -> cxb;
  hw  = buf -> hw;

  /* Print out ethernet header */

  eproto = geteproto (buf);

  if (debug & DEBUG_ETHERHDR) {
    PRINTF "rcvd daddr ");
    printenaddr (hw, cxb + hw -> ether_getinfo1.dstaddrof);
    PRINTF " saddr ");
    printenaddr (hw, cxb + hw -> ether_getinfo1.srcaddrof);
    PRINTF " eproto %4.4x len %u\n", eproto, buf -> cxb_dlen);
  }

  return (eproto);
}

/************************************************************************/
/*									*/
/*  Process ARP packet							*/
/*									*/
/************************************************************************/

static Buf *proc_arp (Buf *buf)

{
  Arppkt *arppkt, *xarppkt;
  Buf *xbuf;
  Cxb *cxb;
  Hw *hw;
  uByte *senaddr, *sipaddr, *tenaddr, *tipaddr;
  uWord hardtype, op, prottype;

  cxb = buf -> cxb;
  hw  = buf -> hw;

  arppkt   = ARPPKT (hw, cxb);
  hardtype = N2HW (arppkt -> hardtype);
  prottype = N2HW (arppkt -> prottype);
  op       = N2HW (arppkt -> op);

  if (debug & DEBUG_ARPPKT) PRINTF "arp  hardtype %u  prottype %u  hardsize %u  protsize %u  op %u\n", 
					hardtype, prottype, arppkt -> hardsize, arppkt -> protsize, op);
  /* If it's bizarre, ignore it */

  if ((hardtype != hw -> ether_getinfo1.arphwtype) 
   || (prottype != PROTO_IP) 
   || (arppkt -> hardsize != hw -> ether_getinfo1.addrsize) 
   || (arppkt -> protsize != IPADDRSIZE)) return (buf);

  /* Add the sender to the arp cache */

  senaddr = arppkt -> var;
  sipaddr = arppkt -> var + hw -> ether_getinfo1.addrsize;
  tenaddr = arppkt -> var + hw -> ether_getinfo1.addrsize + IPADDRSIZE;
  tipaddr = arppkt -> var + 2 * hw -> ether_getinfo1.addrsize + IPADDRSIZE;

  addtoarpc (hw, senaddr, sipaddr, ticks + ARPCACHE_LIFE);

  /* If the ARP packet is a request for our ethernet address, send reply */

  if ((op == ARP_OP_REQ) && ismyipaddr (tipaddr)) {
    xbuf    = mallocsndkbuf (hw);						// allocate a send buffer
    xarppkt = ARPPKT (hw, xbuf -> cxb);						// point to where arp packet goes
    seteproto (PROTO_ARP, xbuf);						// set the ethernet protocol number

    H2NW (hardtype, xarppkt -> hardtype);					// set up hardware type
    H2NW (PROTO_IP, xarppkt -> prottype);					// set up protocol type
    xarppkt -> hardsize = hw -> ether_getinfo1.addrsize;			// set up hardware address size
    xarppkt -> protsize = IPADDRSIZE;						// set up protocol address size
    H2NW (ARP_OP_RPL, xarppkt -> op);						// set opcode = REPLY

    CPYENAD (xarppkt -> var, hw -> myenaddr, hw);				// sender enaddr = my ethernet address
    CPYIPAD (xarppkt -> var + hw -> ether_getinfo1.addrsize, tipaddr);		// sender ipaddr = requested ip address
    CPYENAD (xarppkt -> var + hw -> ether_getinfo1.addrsize + IPADDRSIZE, senaddr, hw); // target enaddr = requestor's ethernet address
    CPYIPAD (xarppkt -> var + 2 * hw -> ether_getinfo1.addrsize + IPADDRSIZE, 	// target ipaddr = requestor's ip address
             sipaddr);
    sendpkt (xbuf, 								// start transmitting it
             xarppkt -> var + 2 * (hw -> ether_getinfo1.addrsize + IPADDRSIZE) - (uByte *)xarppkt, 
             senaddr);
  }

  return (buf);
}

/************************************************************************/
/*									*/
/*  Process IP packet							*/
/*									*/
/************************************************************************/

#define FROF(__ippkt) ((N2HW ((__ippkt) -> flags) & IP_FLAGS_OFFS) << IP_FLAGS_SHFT)

static Buf *proc_ip (Buf *buf)

{
  Buf *bufx, *frag, *hfrag, **lfrag;
  Cxb *cxb, *cxbx;
  Hw *hw;
  Ippkt *hfrag_ippkt, *ippkt, *ippktx;
  uLong hdrlen, ver;
  uWord cksm, flags, fragbeg, fragend, fraglen, fragoffs, ident, proto, totalen;

  /* Verify IP header */

  cxb    = buf -> cxb;
  hw     = buf -> hw;
  ippkt  = IPPKT (hw, cxb);
  ver    = ippkt -> hdrlenver >> 4;
  hdrlen = ippkt -> hdrlenver & 15;
  if (ver != 4) {
    if (debug & (DEBUG_IPHDR | DEBUG_ERRORS)) {
      PRINTF "ip header from ");
      printipaddr (ippkt -> srcipaddr);
      PRINTF "version %u, not 4\n", ver);
    }
    return (buf);
  }
  totalen = N2HW (ippkt -> totalen);

  if (debug & DEBUG_IPHDR) PRINTF "ip hdr version %u, hdrlen %u, typeofserv 0x%x, totalen %u\n", ver, hdrlen, ippkt -> typeofserv, totalen);

  ident    = N2HW (ippkt -> ident);
  flags    = N2HW (ippkt -> flags);
  fragoffs = FROF (ippkt);
  cksm     = N2HW (ippkt -> hdrcksm);
  proto    = ippkt -> proto;

  if (debug & DEBUG_IPHDR) {
    PRINTF "  srcipaddr ");
    printipaddr (ippkt -> srcipaddr);
    PRINTF "  dstipaddr ");
    printipaddr (ippkt -> dstipaddr);
    PRINTF "  ident %u, flags 0x%x\n", ident, flags);
    PRINTF "  fragoffs %u  ttl %u  proto %u  hdrcksm 0x%4.4x\n", fragoffs, ippkt -> ttl, ippkt -> proto, cksm);
  }

  cksm = oz_dev_ip_ipcksm (ippkt);
  if (cksm != 0xFFFF) {
    if (debug & (DEBUG_IPHDR | DEBUG_ERRORS)) {
      PRINTF "ip header checksum error from ");
      printipaddr (ippkt -> srcipaddr);
      PRINTF " result 0x%4.4x\n", cksm);
    }
    return (buf);
  }

  /* We only process header length 5 */

  if (hdrlen != 5) {
    if (debug & (DEBUG_IPHDR | DEBUG_ERRORS)) {
      PRINTF "ip header from ");
      printipaddr (ippkt -> srcipaddr);
      PRINTF " length %u\n", hdrlen);
    }
    return (buf);
  }

  /* If totalen too large, reject it (means data would be hanging off end) */

  if (totalen > hw -> mtu) {
    if (debug & (DEBUG_IPHDR | DEBUG_ERRORS)) {
      PRINTF "ip packet from ");
      printipaddr (ippkt -> srcipaddr);
      PRINTF " destination ");
      printipaddr (ippkt -> dstipaddr);
      PRINTF " too long, totalen %u, mtu %u\n", totalen, hw -> mtu);
    }
    return (buf);
  }

  /* If part of a fragmented buffer, deal with it */

  if (flags & (IP_FLAGS_MF | IP_FLAGS_OFFS)) {

    /* Try to find any earlier fragments with same ident/proto/src/dstipaddr */

    for (lfrag = &frags; (hfrag = *lfrag) != NULL; lfrag = &(hfrag -> next)) {
      ippktx = IPPKT (hfrag -> hw, hfrag -> cxb);
      if (N2HW (ippktx -> ident) != ident) continue;
      if (ippktx -> proto != proto) continue;
      if (!CEQIPAD (ippkt -> dstipaddr, ippktx -> dstipaddr)) continue;
      if (CEQIPAD (ippkt -> srcipaddr, ippktx -> srcipaddr)) break;
    }

    /* If we can't, just put it on end of 'frags' list */

    if (hfrag == NULL) {
      bufx = mallocsndkbuf (hw);					/* allocate a temp buffer */
									/* - don't let precious receive buffers get hung up */
      *lfrag = bufx;							/* link onto end of 'frags' list */
      bufx -> next    = NULL;
      bufx -> frag    = NULL;
      bufx -> timeout = ticks + IP_FRAG_LIFE;				/* let it time out at this time */
      memcpy (IPPKT (hw, bufx -> cxb), ippkt, totalen);			/* copy the data */
      return (buf);							/* read back into original receive buffer */
    }

    /* We found it, unlink from 'frags' list for now (in case new buffer is at      */
    /* offset zero and it gets inserted before this one - it would be the new head) */

    *lfrag = hfrag -> next;

    /* Insert the new buffer on its own frag list by order of ascending frag offset */

    buf -> next = NULL;
    for (lfrag = &hfrag; (frag = *lfrag) != NULL; lfrag = &(frag -> frag)) {
      ippktx = IPPKT (frag -> hw, frag -> cxb);
      if (fragoffs <= FROF (ippktx)) break;
    }
    buf -> frag = frag;
    *lfrag = buf;

    /* See if we have all the fragments */

    fragoffs = 0;							/* have all up to this */
    for (frag = hfrag; frag != NULL; frag = frag -> frag) {		/* scan through fragments for the datagram */
      ippktx  = IPPKT (frag -> hw, frag -> cxb);			/* point to the fragment's ip packet */
      fragbeg = FROF (ippktx);						/* get frag beginning */
      fragend = fragbeg + N2HW (ippktx -> totalen);			/* get fragment end */
      if (!(N2HW (ippktx -> flags) & IP_FLAGS_MF)) break;		/* break if last one */
      if (fragbeg > fragoffs) break;					/* break if data missing */
      if (fragend > fragoffs) fragoffs = fragend;			/* ok so far, get new end */
    }

    /* If we don't, link it all to 'frags' list and wait for more frags */

    if (N2HW (ippktx -> flags) & IP_FLAGS_MF) {
      hfrag -> next = frags;
      frags = hfrag;
      return (NULL);
    }

    /* Copy it all into one buffer (extended ip receive) */

    buf   = hfrag;									/* point to top frag buffer */
    hw    = buf -> hw;
    cxb   = buf -> cxb;
    ippkt = IPPKT (hw, cxb);
    hfrag = hfrag -> frag;								/* get next fragment for the ip datagram */
    if (fragend > DATASIZE) {								/* see if too big for single receive buffer */
      bufx = OZ_KNL_PGPMALLOC (sizeof *bufx);						/* if so, allocate a new one */
      cxbx = OZ_KNL_PGPMALLOC ((uByte *)ippkt + fragend - (uByte *)cxb);		/* ... with enough room for all */
      *bufx = *buf;									/* just copy all this junk as is */
      bufx -> type = BUF_TYPE_RCV_IPX;							/* ... but change type to 'extended ip receive' */
      bufx -> cxb  = cxbx;								/* ... and keep new cxb pointer */
      bufx -> hw   = hw;								/* use same hardware parameters */
      memcpy (IPPKT (hw, cxbx), ippkt, N2HW (ippkt -> totalen));			/* copy first part of fragment */
      OZ_KNL_PGPVALID (cxbx);
      freebuf (buf);									/* free off the old receive buffer */
      buf   = bufx;									/* pretend we had this one all along */
      cxb   = cxbx;
      ippkt = IPPKT (hw, cxb);
    }
    H2NW (fragend, ippkt -> totalen);							/* save new total ip datagram length */

    while ((frag = hfrag) != NULL) {							/* repeat for each fragment of ip datagram */
      cxbx     = frag -> cxb;
      ippktx   = IPPKT (frag -> hw, cxbx);
      fraglen  = N2HW (ippktx -> totalen);						/* get length of fragment packet (includes ip header) */
      fragoffs = FROF (ippktx);								/* get offset with data area of ip packet */
      if (fraglen > ippkt -> dat.raw - (uByte *)ippkt) {				/* this length includes the ip header */
        fraglen -= ippkt -> dat.raw - (uByte *)ippkt;					/* ... so subtract length of ip header */
        memcpy (ippkt -> dat.raw + fragoffs, ippktx -> dat.raw, fraglen);		/* copy into top data buffer */
      }
      if (!(N2HW (ippktx -> flags) & IP_FLAGS_MF)) break;				/* stop if this is the last one */
      hfrag = hfrag -> frag;								/* if not, free it off */
      freebuf (frag);									/* ... then process next one */
    }

    while ((frag = hfrag) != NULL) {							/* free off any left-overs */
      hfrag = hfrag -> frag;
      freebuf (frag);
    }
  }

  /* Make sure it's intended for me.  If not, try to forward it. */

  if (!ismyipaddr (ippkt -> dstipaddr)) {
    if (debug & DEBUG_FORWARD) {
      PRINTF "forwarding ");
      printipaddr (ippkt -> srcipaddr);
      PRINTF " to ");
      printipaddr (ippkt -> dstipaddr);
      PRINTF " ttl %u", ippkt -> ttl);
    }
    if (memcmp (cxb + hw -> ether_getinfo1.dstaddrof, arpenaddr, hw -> ether_getinfo1.addrsize) == 0) {
      if (debug & DEBUG_FORWARD) PRINTF " - broadcast enaddr\n");		/* don't forward broadcast messages */
      return (buf);
    }
    if (isnwbcipad (ippkt -> dstipaddr, hw)) {
      if (debug & DEBUG_FORWARD) PRINTF " - network/broadcast\n");
      return (buf);
    }
    if (ippkt -> ttl < 2) {							/* bounce if ttl is 0 or 1 */
      if (debug & DEBUG_FORWARD) PRINTF " - ttl expired\n");
      sendicmpbounce (buf, PROTO_IP_ICMP_TIMEEXC, 0, 0);
      return (NULL);
    }
    hw = find_send_hw (ippkt -> dstipaddr, NULL, buf -> sendipaddr, 1);		/* find out where to send it */
    if (hw == NULL) {
      if (debug & DEBUG_FORWARD) PRINTF " - no route\n");
      sendicmpbounce (buf, PROTO_IP_ICMP_DESTUNR, 0, 0);			/* if can't forward it, bounce it */
      return (NULL);
    }
    if (!filter_check (&forward_filters, ippkt)) {				/* lastly, check the forwarding filters */
      if (debug & DEBUG_FORWARD) PRINTF " - filtered out\n");
      sendicmpbounce (buf, PROTO_IP_ICMP_DESTUNR, 10, 0);
      return (NULL);
    }
    if (debug & DEBUG_FORWARD) {
      PRINTF "\n - via %s ip ", hw -> devname);
      printipaddr (buf -> sendipaddr);
      PRINTF "\n");
    }
    ippkt -> ttl --;								/* decrement time-to-live */
    for (fragoffs = 0; fragoffs < totalen; fragoffs += fraglen) {		/* need to copy and maybe fragment */
      frag   = mallocsndkbuf (hw);						/* allocate a buffer for a fragment */
      ippktx = IPPKT (hw, frag -> cxb);						/* point to where to put the ip packet therein */
      CPYIPAD (frag -> sendipaddr, buf -> sendipaddr);				/* this is the ip it goes to (next hop) */
      fraglen = hw -> mtu - IPHDRSIZE;						/* this is the most this hw can do at once */
      memcpy (ippktx, ippkt, IPHDRSIZE);					/* copy the original ip header as is */
      cksm = flags + (fragoffs >> IP_FLAGS_SHFT);				/* set up new flags */
      if (fraglen >= totalen - fragoffs) fraglen = totalen - fragoffs;		/* if it all fits, chop length off */
      else if (flags & IP_FLAGS_DF) goto dontfragit;
      else cksm |= IP_FLAGS_MF;							/* doesn't fit, say more frags follow */
      H2NW (cksm, ippktx -> flags);						/* set up new flags */
      H2NW (fraglen + IPHDRSIZE, ippktx -> totalen);				/* set up new length */
      H2NW (0, ippktx -> hdrcksm);						/* calc new ip header checksum */
      cksm = oz_dev_ip_ipcksm (ippktx);
      H2NW (cksm, ippktx -> hdrcksm);
      memcpy (ippktx -> dat.raw, ippkt -> dat.raw + fragoffs, fraglen); 	/* copy data */
      sendippkt (frag, NULL, NULL);						/* start sending it */
    }
    return (buf);

dontfragit:
    sendicmpbounce (buf, PROTO_IP_ICMP_DESTUNR, 4, hw -> mtu);			/* fragmenting prohibited, bounce it */
    return (NULL);
  }

  /* Check input filters */

  if (!filter_check (&input_filters, ippkt)) {
    sendicmpbounce (buf, PROTO_IP_ICMP_DESTUNR, 10, 0);
    return (NULL);
  }

  /* Process protocol - bounce those we don't care about */

  switch (ippkt -> proto) {
    case PROTO_IP_ICMP: {
      buf = proc_icmp (buf);
      break;
    }
    case PROTO_IP_UDP: {
      buf = proc_udp (buf);
      break;
    }
    case PROTO_IP_TCP: {
      buf = proc_tcp (buf);
      break;
    }
    default: {
      sendicmpbounce (buf, PROTO_IP_ICMP_DESTUNR, 2, 0);
      buf = NULL;
    }
  }

  return (buf);
}

/************************************************************************/
/*									*/
/*  Process ICMP datagram						*/
/*									*/
/************************************************************************/

static Buf *proc_icmp (Buf *buf)

{
  Buf *xbuf;
  Cxb *cxb;
  Ippkt *ippkt, *xippkt;
  uByte icmptype, icmpcode;
  uWord cksm, totalen;

  cxb   = buf -> cxb;
  ippkt = IPPKT (buf -> hw, cxb);

  totalen  = N2HW (ippkt -> totalen);
  icmptype = ippkt -> dat.icmp.type;
  icmpcode = ippkt -> dat.icmp.code;
  cksm     = N2HW (ippkt -> dat.icmp.cksm);
  if (debug & DEBUG_ICMPHDR) PRINTF "  icmp type %u  code %u  totalen %u  cksm 0x%4.4x", icmptype, icmpcode, totalen, cksm);

  cksm = oz_dev_ip_icmpcksm (ippkt);
  if (cksm != 0xFFFF) {
    if (debug & DEBUG_ICMPHDR) PRINTF "  result 0x%4.4x\n", cksm);
    return (buf);
  }
  if (debug & DEBUG_ICMPHDR) PRINTF "\n");

  switch (icmptype) {

    /* Ignore PING replies - they should be handled by user code */

    case PROTO_IP_ICMP_PINGRPL: {
      break;
    }

    /* Process ICMP PING requests */

    case PROTO_IP_ICMP_PINGREQ: {
      if (totalen > buf -> hw -> mtu) {
        if (debug & DEBUG_ERRORS) {
          PRINTF "icmp ping req totalen %u mtu %u from ", totalen, buf -> hw -> mtu);
          printipaddr (ippkt -> srcipaddr);
          PRINTF "\n");
        }
      } else {
        xbuf   = mallocsndkbuf (buf -> hw);					// mallocate a send buffer
        xippkt = IPPKT (xbuf -> hw, xbuf -> cxb);				// point to ip packet therein
        memcpy (xippkt, ippkt, totalen);					// copy the ip packet as is
        seteproto (PROTO_IP, xbuf);						// set protocol in ethernet header to IP
        xippkt -> dat.icmp.type = PROTO_IP_ICMP_PINGRPL;			// change icmp type to REPLY (instead of REQUEST)
        CPYIPAD (xippkt -> srcipaddr, ippkt -> dstipaddr);			// swap source and destination ip addresses
        CPYIPAD (xippkt -> dstipaddr, ippkt -> srcipaddr);
        H2NW (0, xippkt -> dat.icmp.cksm);					// compute new icmp checksum
        cksm = oz_dev_ip_icmpcksm (xippkt);
        H2NW (cksm, xippkt -> dat.icmp.cksm);
        if (!filter_check (&output_filters, xippkt)) freebuf (xbuf);		// discard if we're not allowed to output it
        else sendpkt (xbuf, N2HW (xippkt -> totalen), cxb + buf -> hw -> ether_getinfo1.srcaddrof); // otherwise, start transmitting reply
      }
      break;
    }

    /* Assume everything else is an bounce */

    default: {
      if (debug & DEBUG_BOUNCE) printbounce (ippkt);
      if (icmptype == PROTO_IP_ICMP_DESTUNR) proc_destunr (&(ippkt -> dat.icmp));
      break;
    }
  }

  return (buf);
}

/* Print out an incoming icmp bounce message */

static void printbounce (Ippkt *ip)

{
  Ippkt *eip;

  eip = (Ippkt *)(ip -> dat.icmp.raw + 4);

  PRINTF "rcvd icmp bounce type %u code %u extra %u\n  from ", ip -> dat.icmp.type, ip -> dat.icmp.code, N2HL (ip -> dat.icmp.raw + 0));
  printipaddr (ip -> srcipaddr);
  PRINTF " to ");
  printipaddr (ip -> dstipaddr);
  PRINTF "\n  for %u byte proto %u packet\n  from ", N2HW (eip -> totalen), eip -> proto);
  printipaddr (eip -> srcipaddr);
  if (eip -> proto == PROTO_IP_TCP) PRINTF ",%u", N2HW (eip -> dat.tcp.sport));
  if (eip -> proto == PROTO_IP_UDP) PRINTF ",%u", N2HW (eip -> dat.udp.sport));
  PRINTF " to ");
  printipaddr (eip -> dstipaddr);
  if (eip -> proto == PROTO_IP_TCP) PRINTF ",%u", N2HW (eip -> dat.tcp.dport));
  if (eip -> proto == PROTO_IP_UDP) PRINTF ",%u", N2HW (eip -> dat.udp.dport));
  PRINTF "\n");
}

/* Apply the icmp error to anything we can */

static void proc_destunr (Icmppkt *icmppkt)

{
  Ippkt *ippkt;
  Tcpcon *tcpcon;
  uLong newmtu, sts;

  ippkt = (Ippkt *)(icmppkt -> raw);

  /* Reset all tcp max send sizes that have had fragmentation refused */

  if ((icmppkt -> code == 4) && (ippkt -> proto == PROTO_IP_TCP)) {
    for (tcpcon = tcpcons; tcpcon != NULL; tcpcon = tcpcon -> next) {
      if (!CEQIPAD (tcpcon -> ripaddr, ippkt -> dstipaddr)) continue;
      if (!CEQIPAD (tcpcon -> lipaddr, ippkt -> srcipaddr)) continue;
      if (tcpcon -> rsockno != N2HW (ippkt -> dat.tcp.dport)) continue;
      if (tcpcon -> lsockno != N2HW (ippkt -> dat.tcp.sport)) continue;
      newmtu  = N2HL (icmppkt -> raw + 0);				/* get the new mtu it wants to set */
      if (newmtu < MINMTU) newmtu = MINMTU;				/* must be at least this much */
      newmtu -= TCPHDRSIZE;						/* subtract off size of ip & tcp header */
      if (newmtu < tcpcon -> maxsendsize) {				/* can't be more than what we already have */
        tcpcon -> maxsendsize = newmtu;					/* ok, set up the new max send size */
        tcpcon -> congestionwindow = newmtu;				/* reset the congestion window to send just one packet */
        tcpcon -> nextxmtseq = tcpcon -> seq_lastrcvdack;		/* re-transmit everything that hasn't been acked */
        starttcpsend (NULL, tcpcon);					/* start retransmitting */
      }
      break;
    }
    return;
  }

  /* If outbound connection refused, abort the attempt */

  if (ippkt -> proto == PROTO_IP_TCP) {
    sts = OZ_PENDING;
    switch (icmppkt -> code) {
      case  0:				/* network unreachable */
      case  1:				/* host unreachable */
      case  5:				/* source route failed */
      case  6:				/* destination network unknown */
      case  7:				/* destination host unknown */
      case  8:				/* source host isolated (obsolete) */
      case  9:				/* dest network adminstrattively prohibited */
      case 10:				/* dest host adminstrattively prohibited */
      case 11:				/* network unreachable for TOS */
      case 12: sts = OZ_NOROUTETODEST;	/* host unreachable for TOS */
               break;
      case  2:				/* protocol unreachable */
      case  3: sts = OZ_CONNECTREFUSED;	/* port unreachable */
               break;
    }
    if (sts == OZ_PENDING) return;
    for (tcpcon = tcpcons; tcpcon != NULL; tcpcon = tcpcon -> next) {
      if (!CEQIPAD (tcpcon -> ripaddr, ippkt -> dstipaddr)) continue;
      if (!CEQIPAD (tcpcon -> lipaddr, ippkt -> srcipaddr)) continue;
      if (tcpcon -> rsockno != N2HW (ippkt -> dat.tcp.dport)) continue;
      if (tcpcon -> lsockno != N2HW (ippkt -> dat.tcp.sport)) continue;
      if (tcpcon -> reset == 0) tcpcon -> reset = sts;
      tcpterminate (tcpcon);
      break;
    }
    return;
  }
}

/************************************************************************/
/*									*/
/*  Process UDP datagram						*/
/*									*/
/************************************************************************/

static Buf *proc_udp (Buf *buf)

{
  Chnex *chnex;
  int used;
  Iopex *iopex;
  Ippkt *ippkt;
  Sock dport, sport;
  uLong rawsize, sts;
  uWord cksm, length;

  ippkt  = IPPKT (buf -> hw, buf -> cxb);

  sport  = N2HW (ippkt -> dat.udp.sport);
  dport  = N2HW (ippkt -> dat.udp.dport);
  length = N2HW (ippkt -> dat.udp.length);
  cksm   = N2HW (ippkt -> dat.udp.cksm);

  if (debug & DEBUG_UDPHDR) PRINTF "udp sport %u  dport %u  length %u  cksm 0x%4.4x\n", sport, dport, length, cksm);

  cksm = oz_dev_ip_udpcksm (ippkt);
  if (cksm != 0xffff) {
    if (debug & (DEBUG_UDPHDR | DEBUG_ERRORS)) {
      PRINTF "udp packet from ");
      printipaddr (ippkt -> srcipaddr);
      PRINTF " checksum error - result 0x%4.4x\n", cksm);
    }
    return (buf);
  }

  /* Pass it on to any matching bound udp ports */

  used = 0;
  for (chnex = udpbinds; chnex != NULL; chnex = chnex -> udpbind_next) {
    if (!CEQPORT (chnex -> udpbind_lclportno, ippkt -> dat.udp.dport)) continue;
    if (!ZERIPAD (chnex -> udpbind_lclipaddr) && !CEQIPAD (chnex -> udpbind_lclipaddr, ippkt -> dstipaddr)) continue;
    iopex = chnex -> udpbind_iopqh;
    if (iopex == NULL) continue;
    chnex -> udpbind_iopqh = iopex -> next;
    if (chnex -> udpbind_iopqh == NULL) chnex -> udpbind_iopqt = &(chnex -> udpbind_iopqh);

    /* Copy data from internal buffer to user's buffer */

    ACCESSBUFFS (iopex);
    rawsize = length + (((uByte *)&(ippkt -> dat.udp)) - ippkt -> dat.udp.raw);
    sts     = OZ_SUCCESS;
    if (rawsize > iopex -> u.udpreceive.p.rawsize) {
       oz_knl_printk ("oz_dev_ip udpreceive: messagesize %u, buffersize %u\n", rawsize, iopex -> u.udpreceive.p.rawsize);
       rawsize = iopex -> u.udpreceive.p.rawsize;
       sts     = OZ_BUFFEROVF;
    }
    memcpy (iopex -> u.udpreceive.p.rawbuff, ippkt -> dat.udp.raw, rawsize);
    if (iopex -> u.udpreceive.p.rawrlen   != NULL) *(iopex -> u.udpreceive.p.rawrlen) = rawsize;
    if (iopex -> u.udpreceive.p.srcipaddr != NULL) CPYIPAD (iopex -> u.udpreceive.p.srcipaddr, ippkt -> srcipaddr);
    if (iopex -> u.udpreceive.p.srcportno != NULL) CPYPORT (iopex -> u.udpreceive.p.srcportno, ippkt -> dat.udp.sport);

    /* Post the I/O request */

    oz_knl_iodone (iopex -> ioop, sts, NULL, NULL, NULL);
    used = 1;
  }

  /* Read back into same buffer again */

  if (used) return (buf);

  /* No one used it, bounce it */

  sendicmpbounce (buf, PROTO_IP_ICMP_DESTUNR, 3, 0);
  return (NULL);
}

/************************************************************************/
/*									*/
/*  Process TCP datagram						*/
/*									*/
/************************************************************************/

static Buf *proc_tcp (Buf *buf)

{
  Buf **lbuf, *xbuf;
  Chnex *chnex;
  Cxb *cxb;
  uByte *rawpnt;
  Hw *hw;
  Long rtt, rtterr, seqdiff;
  int duplicateack, i, neednewrti;
  Iopex *iopex, *tcplis;
  Ippkt *ippkt, *xippkt;
  Sock dport, sport;
  Tcpcon *tcpcon;
  uLong ack, ins, len, seq, seqend, siz, sts;
  uWord cksm, flags, ignore, maxsendsize, rawlen, totalen, urgent, wsize;

  cxb    = buf -> cxb;
  hw     = buf -> hw;
  ippkt  = IPPKT (hw, cxb);

  sport  = N2HW (ippkt -> dat.tcp.sport);
  dport  = N2HW (ippkt -> dat.tcp.dport);
  seq    = tcpbegseq (ippkt);
  ack    = N2HL (ippkt -> dat.tcp.ack);
  flags  = N2HW (ippkt -> dat.tcp.flags);
  wsize  = N2HW (ippkt -> dat.tcp.wsize);
  cksm   = N2HW (ippkt -> dat.tcp.cksm);
  urgent = N2HW (ippkt -> dat.tcp.urgent);
  rawlen = tcprawlen (ippkt);
  seqend = tcpendseq (ippkt);

  if (debug & DEBUG_TCPHDR) {
    PRINTF "tcp ");
    printipaddr (ippkt -> srcipaddr);
    PRINTF ",%u ", sport);
    printipaddr (ippkt -> dstipaddr);
    PRINTF ",%u\n", dport);
    PRINTF "  seq %u..%u  ack %u\n", seq, seqend, ack);
    PRINTF "  flags ");
    printtcpflags (flags);
    PRINTF "  rawlen %u  wsize %u\n", rawlen, wsize);
  }
  cksm = oz_dev_ip_tcpcksm (ippkt);
  if (cksm != 0xffff) {
    if (debug & (DEBUG_TCPHDR | DEBUG_ERRORS)) {
      PRINTF "tcp packet from ");
      printipaddr (ippkt -> srcipaddr);
      PRINTF " checksum error - result 0x%4.4x\n", cksm);
      printippkt ("        ", ippkt);
      totalen = N2HW (ippkt -> totalen);
      oz_knl_dumpmem (totalen, ippkt);
    }
    return (buf);
  }

  /* See if a connection block already exists for it */

  for (tcpcon = tcpcons; tcpcon != NULL; tcpcon = tcpcon -> next) {
    if (tcpcon -> lsockno != dport) continue;
    if (tcpcon -> rsockno != sport) continue;
    if (!CEQIPAD (tcpcon -> lipaddr, ippkt -> dstipaddr)) continue;
    if (CEQIPAD (tcpcon -> ripaddr, ippkt -> srcipaddr)) break;
  }

  if (debug & DEBUG_TCPHDR) PRINTF " - tcpcon %p\n", tcpcon);

  /* Process inbound connection requests */

  if ((tcpcon == NULL) && ((flags & (TCP_FLAGS_SYN | TCP_FLAGS_ACK)) == TCP_FLAGS_SYN)) { /* see if flags indicate and we don't already have one */
    for (tcplis = tcplisqh; tcplis != NULL; tcplis = tcplis -> next) {			/* ok, search the listening queue */
      if  (tcplis -> u.tcplisten.tcpcon -> lsockno != dport) continue;
      if ((tcplis -> u.tcplisten.tcpcon -> rsockno != 0) && (tcplis -> u.tcplisten.tcpcon -> rsockno != sport)) continue;
      if (!ZERIPAD (tcplis -> u.tcplisten.tcpcon -> lipaddr) && !CEQIPAD (tcplis -> u.tcplisten.tcpcon -> lipaddr, ippkt -> dstipaddr)) continue;
      if (!ZERIPAD (tcplis -> u.tcplisten.tcpcon -> ripaddr) && !CEQIPAD (tcplis -> u.tcplisten.tcpcon -> ripaddr, ippkt -> srcipaddr)) continue;
      break;
    }
    if (tcplis == NULL) {								/* bounce packet if there is no listener */
      sendicmpbounce (buf, PROTO_IP_ICMP_DESTUNR, 3, 0);
      return (NULL);
    }

    /* Unlink listen request from tcplisq */

    *(tcplis -> u.tcplisten.prev) = tcplis -> next;
    if (tcplis -> next != NULL) tcplis -> next -> u.tcplisten.prev = tcplis -> u.tcplisten.prev;
    if (tcplisqt == &(tcplis -> next)) tcplisqt = tcplis -> u.tcplisten.prev;

    /* Finish filling in stuff in the connection struct and link it to list of active connections */

    tcpcon = tcplis -> u.tcplisten.tcpcon;						/* get preallocated connection block */
    tcpcon -> lisiopex            = NULL;						/* we're going to post the lisiopex here eventually */
											/* ... so don't let the tcpterminate routine see it */
    tcpcon -> next                = tcpcons;						/* next in tcpcons list */
    tcpcon -> lsockno             = dport;						/* save local port number */
    tcpcon -> rsockno             = sport;						/* save remote port number */
    tcpcon -> maxsendsize         = TCP_MAXSENDSIZE (hw);				/* maximum size to send in a datagram */
    tcpcon -> congestionwindow    = TCP_MAXSENDSIZE (hw);
    tcpcon -> slowstarthresh      = 65535;
    tcpcon -> retransmitinterval  = TPS;						/* initial retransmit interval = one second */
    tcpcon -> smoothedroundtrip   = TPS * 8;						/* corresponding smoothed round trip time (ticks * 8) */
    tcpcon -> timeout             = -1;
    tcpcon -> seq_lastacksent     = seq;						/* pretend we acked all before the incoming SYN message */
    tcpcon -> seq_lastacksent2    = seq;
    tcpcon -> seq_receivedok      = seq + 1;						/* this processes the incoming SYN flag */
											/* ... and causes our outgoing ack seq to include the incoming SYN flag */
    tcpcon -> seq_lastrcvdwsize  += MINWINSIZE;						/* pretend client has sent us a minimal wsize */
											/* ... so we will at least be able to send the SYN */
    CPYIPAD (tcpcon -> lipaddr, ippkt -> dstipaddr);					/* save local host's ip address */
    CPYIPAD (tcpcon -> ripaddr, ippkt -> srcipaddr);					/* save remote host's ip address */
    tcpcons = tcpcon;									/* link it on list */
    ntcpcons ++;

    if (debug & DEBUG_ERRORS) {
      PRINTF "new inbound ");
      printtcpcon (tcpcon);
      PRINTF "\n");
    }
    validtcpxmtq (tcpcon);

    /* Return actual ip addresses and socket numbers to requestor */

    ACCESSBUFFS (tcplis);
    if (tcplis -> u.tcplisten.p.lclportno != NULL) H2NW (tcpcon -> lsockno, tcplis -> u.tcplisten.p.lclportno);
    if (tcplis -> u.tcplisten.p.remportno != NULL) H2NW (tcpcon -> rsockno, tcplis -> u.tcplisten.p.remportno);
    if (tcplis -> u.tcplisten.p.lclipaddr != NULL) CPYIPAD (tcplis -> u.tcplisten.p.lclipaddr, tcpcon -> lipaddr);
    if (tcplis -> u.tcplisten.p.remipaddr != NULL) CPYIPAD (tcplis -> u.tcplisten.p.remipaddr, tcpcon -> ripaddr);

    /* Convert the listen request to a transmit to send the SYN/ACK message back to the client. */
    /* When the client acknowledges it, it will post the listen request's completion.           */

    memset (&(tcplis -> u.tcptransmit), 0, sizeof tcplis -> u.tcptransmit);		/* convert it to a transmit request */
    tcplis -> u.tcptransmit.p.rawsize = 1;						/* a SYN consumes one seq number */
    tcplis -> u.tcptransmit.syn       = TCP_FLAGS_SYN | TCP_FLAGS_ACK | (1 << TCP_FLAGS_HLNSHF); /* flags that go with it */
    queuetcptransmit (tcplis, tcpcon);							/* put on end of transmit queue */
  }

  /* If there is no connect block, send a reset message */

  if (tcpcon == NULL) {
    if (debug & DEBUG_ERRORS) {
      PRINTF "tcp datagram from ");
      printipaddr (ippkt -> srcipaddr);
      PRINTF ",%u to ", sport);
      printipaddr (ippkt -> dstipaddr);
      PRINTF ",%u\n  flags ", dport);
      printtcpflags (flags);
      PRINTF " has no connection block\n", dport);
    }
    if (!(flags & TCP_FLAGS_RST)) {
      flags   = TCP_FLAGS_RST | TCP_FLAGS_ACK;
      flags  += (ippkt -> dat.tcp.raw - ippkt -> dat.raw) << (TCP_FLAGS_HLNSHF - 2);
      totalen = ippkt -> dat.tcp.raw - (uByte *)ippkt;

      xbuf = mallocsndkbuf (find_send_hw (ippkt -> srcipaddr, NULL, NULL, 1));
      if (xbuf != NULL) {
        xippkt = IPPKT (xbuf -> hw, xbuf -> cxb);

        memcpy (xippkt, ippkt, totalen);

        H2NW (totalen, xippkt -> totalen);			/* save total length in ip header */
        H2NW (0, xippkt -> flags);				/* no ip flags */
        xippkt -> ttl = 255;					/* set up time-to-live */

        CPYIPAD (xippkt -> srcipaddr, xippkt -> dstipaddr);	/* swap ip addresses */
        CPYIPAD (xippkt -> dstipaddr, xippkt -> srcipaddr);
        H2NW (sport, xippkt -> dat.tcp.dport);			/* swap tcp port numbers */
        H2NW (dport, xippkt -> dat.tcp.sport);

        H2NL (ack, xippkt -> dat.tcp.seq);			/* give it what it expects */
        H2NL (seqend, xippkt -> dat.tcp.ack);			/* say we received it ok */
        H2NW (flags, xippkt -> dat.tcp.flags);			/* save the tcp flags */
        H2NW (0, xippkt -> dat.tcp.wsize);			/* not receiving anything */
        H2NW (0, xippkt -> dat.tcp.urgent);			/* clear urgent pointer */

        H2NW (0, xippkt -> dat.tcp.cksm);			/* set up tcp checksum */
        cksm = oz_dev_ip_tcpcksm (xippkt);
        H2NW (cksm, xippkt -> dat.tcp.cksm);

        sendip (xbuf, NULL, NULL);				/* send it & forget about it */
      }
    }
    return (buf);						/* read in something new */
  }

  /* It belongs to a connection, remember when we last heard from remote end */

  tcpcon -> lasttimemsgrcvd = ticks;

  /* Save the window size it just sent us */

  if (flags & TCP_FLAGS_ACK) {
    sts = wsize + ack;					/* see what's the highest sequence it wants us to send */
    seqdiff = sts - tcpcon -> seq_lastrcvdwsize;	/* see if that's better than what we've had before */
    if (seqdiff > 0) {					/* if not, this is probably an old duplicate packet */
      tcpcon -> seq_lastrcvdwsize   = sts;		/* if it is better, remember it */
      tcpcon -> lasttimenzwsizercvd = ticks;		/* remember when we made progress */
    }
  }

  /* See if it is a duplicate ack */

  duplicateack = 0;
  if (flags & TCP_FLAGS_ACK) {
    seqdiff = ack - tcpcon -> seq_lastrcvdack;
    if ((seqdiff < 0)  || ((seqdiff == 0) && (seqend == seq))) {
      seqdiff = ack - tcpcon -> seq_nextusersendata;	/* see if it is acking all we ever sent (to avoid a dup ack race) */
							/* if so, ignore it as there is nothing to resend */
      if (seqdiff < 0) {				/* if not, then if we get 3 in a row, resend any unacked data */
        duplicateack = 1;

        if (++ (tcpcon -> dupacksinarow) >= 3) {
          tcpcon -> dupacksinarow = 0;

          /* If we have at least 3 dup acks in a row, it means the other end is losing packets and the network is   */
          /* congested.  So decrement the congestionwindow by one segment (ie, send half as many packets during the */
          /* same amount of time).  Then set the slowstart threshold to half the congestionwindow (this will cause  */
          /* the exponential increase to stop half way to the congestion point, then increase linearly from there). */
          /* Finally, re-send the unacked data.                                                                     */

          tcpcon -> congestionwindow -= tcpcon -> maxsendsize;
          if (tcpcon -> congestionwindow < tcpcon -> maxsendsize) tcpcon -> congestionwindow = tcpcon -> maxsendsize;

          tcpcon -> slowstarthresh = tcpcon -> congestionwindow / 2;
          if (debug & DEBUG_TCPFLOW) PRINTF "slowstarthresh %u, congestionwindow %u\n", tcpcon -> slowstarthresh, tcpcon -> congestionwindow);

          validtcpxmtq (tcpcon);
          tcpcon -> nextxmtseq = tcpcon -> seq_lastrcvdack;
          validtcpxmtq (tcpcon);

          /* The three dup acks may be Linux complaining about an ack we sent that only acked a partial packet because */
          /* the window overflowed.  Linux will hang the connection if we keep acking that partial packet.  So here we */
          /* send an ack that acknowledges only up to what it just sent us as part of the duplicate ack packet.        */

          xbuf = mallocsndkbuf (find_send_hw (tcpcon -> ripaddr, NULL, NULL, 1));
          sendtcp (xbuf, tcpcon, 0, TCP_FLAGS_ACK, seqend);
        }
      }

    } else {

      /* It has acked 'seqdiff' new bytes of data, reset dup ack counter and reset lastrcvdack number */

      validtcpxmtq (tcpcon);

      tcpcon -> dupacksinarow = 0;						/* not a dup ack, it is acking new stuff */
      seqdiff = ack - tcpcon -> seq_nextusersendata;				/* see if it acking more than has ever been sent */
      if (seqdiff > 0) ack = tcpcon -> seq_nextusersendata;			/* if so, limit it to what we have sent */
      tcpcon -> seq_lastrcvdack = ack;						/* ok, ack makes sense now, save it */
      seqdiff = ack - tcpcon -> nextxmtseq;					/* see if it is acking stuff we are about to re-transmit */
      if (seqdiff > 0) tcpcon -> nextxmtseq = ack;				/* if so, don't bother retransmitting any of it */

      /* Ack as many whole buffers as the 'ack' sequence number will allow */

      neednewrti = 0;
      while ((iopex = tcpcon -> nextackreq) != NULL) {				/* see what is there to ack */
        seqdiff = ack - iopex -> u.tcptransmit.startseq - iopex -> u.tcptransmit.p.rawsize;
        if (seqdiff < 0) break;							/* stop if whole thing not acked yet */

        /* Remove it from queue */

        tcpcon -> nextackreq = iopex -> next;
        tcpcon -> numxmtreqs --;

        /* If it did not need to be retransmitted, we can measure and use its round trip time */

        if (iopex -> u.tcptransmit.retransmits == 0) {				/* see if it has been retransmitted */
          if (iopex -> u.tcptransmit.sendstarted != 0) {
            rtt = (ticks - iopex -> u.tcptransmit.sendstarted) * 8;		/* if not, calculate round trip time */
            rtterr = rtt - tcpcon -> smoothedroundtrip;
            tcpcon -> smoothedroundtrip = (tcpcon -> smoothedroundtrip * 7 + rtterr + 4) / 8;
            if (rtterr < 0) rtterr = -rtterr;
            tcpcon -> roundtripdeviation = (tcpcon -> roundtripdeviation * 3 + rtterr + 2) / 4;
            if (debug & DEBUG_TCPFLOW) PRINTF "rtt %d, rtterr %d, smoothed %u, deviation %u\n", rtt, rtterr, tcpcon -> smoothedroundtrip, tcpcon -> roundtripdeviation);
            neednewrti = 1;
          } else {
            PRINTF "oz_dev_ip*: tcptransmit.sendstarted is 0, flags %X\n", iopex -> u.tcptransmit.syn); // this should be a crash?
          }
        }

        /* Post the acked transmit request, as it has successfully completed */
        /* It may also be a converted connect, listen or close request       */

        if (debug & DEBUG_IOPOST) PRINTF "oz_dev_ip*: posting tcp iopex %p\n", iopex);
        oz_knl_iodone (iopex -> ioop, OZ_SUCCESS, NULL, NULL, NULL);

        /* Now increment congestion window.  If below slowstarthresh, we want  */
        /* to double the transmission rate (in terms of bytes per second).  If */
        /* above slowstarthresh, we want to linearly increase the transmission */
        /* rate (in terms of bytes per second) by one maxsendsize.             */

        if (tcpcon -> congestionwindow < tcpcon -> slowstarthresh) {

          /* Adding one maxsendsize to congestionwindow will allow one more    */
          /* datagram to be transmitted at a time.  Since we do this for each  */
          /* datagram acked, it doubles the rate in terms of bytes per second, */
          /* because we allow one more to be sent per unit time and therefore  */
          /* do one more of these increments in the next such unit time.       */

          tcpcon -> congestionwindow += tcpcon -> maxsendsize;

        } else {

          /* This adds 1/(cw/mss) to cw/mss.  This has the effect of allowing */
          /* only one more packet per unit time, as it requires cw/mss        */
          /* packets to be acked in order to increment cw by one mss.  The    */
          /* mss/8 factor is a fudge to make sure it does get incremented.    */

          tcpcon -> congestionwindow += tcpcon -> maxsendsize * tcpcon -> maxsendsize / tcpcon -> congestionwindow + tcpcon -> maxsendsize / 8;
        }
        if (debug & DEBUG_TCPFLOW) PRINTF "congestionwindow %u\n", tcpcon -> congestionwindow);
      }

      /* If queue is empty, reset the end pointer */

      if (tcpcon -> nextackreq == NULL) tcpcon -> lastxmtreq = &(tcpcon -> nextackreq);
      validtcpxmtq (tcpcon);

      /* Maybe we need a new retransmit interval calculation */

      if (neednewrti) {
        tcpcon -> retransmitinterval = (tcpcon -> smoothedroundtrip + 4 * tcpcon -> roundtripdeviation + 7) / 8;
        if (tcpcon -> retransmitinterval < 2) tcpcon -> retransmitinterval = 2;
        if (debug & DEBUG_TCPFLOW) PRINTF "retransmitinterval %u\n", tcpcon -> retransmitinterval);
      }
    }
  }

  /* Set up initial pointers to data in buf */

  buf -> tcprawlen = tcprawlen (ippkt);			/* this includes any option stuff */
  buf -> tcprawpnt = ippkt -> dat.tcp.raw;		/* this points to any option stuff present */
  buf -> tcprawseq = seq;				/* this includes any SYN present */
  buf -> tcphasfin = ((flags & TCP_FLAGS_FIN) != 0);	/* also remember if it has a FIN flag */

  /* If we received a reset, mark the connection as being reset */

  if (flags & TCP_FLAGS_RST) {
    if (debug & DEBUG_ERRORS) {
      PRINTF "tcp reset received from ");
      printipaddr (ippkt -> srcipaddr);
      PRINTF ",%u\n", sport);
    }
    if (tcpcon -> reset == 0) tcpcon -> reset = OZ_LINKABORTED;
    goto finish;
  }

  /* If the SYN flag is set, remember we got it and set last good sequence number = beginning of message just received */

  /* Only do this once per connection as this might be a duplicate packet        */
  /* and we don't want to reset the sequence numbers if that happens             */
  /* Also, disable the outbound connection timeout as the connection is complete */

  if (flags & TCP_FLAGS_SYN) {
    if (!(tcpcon -> rcvdsyn)) {
      tcpcon -> seq_receivedok   = seq + 1;			/* SYN consumes one seq number, so first data starts at seq + 1 */
      tcpcon -> seq_lastacksent  = seq;				/* we have yet to ack the SYN itself, though */
      tcpcon -> seq_lastacksent2 = seq;
      if (flags & TCP_FLAGS_ACK) tcpcon -> lastwsizesent = 1;	/* pretend we last sent a wsize 1 (for just the SYN) */
								/* this will force us to send an ACK for the SYN right away */
      tcpcon -> rcvdsyn =  1;					/* seq_receivedok is now valid */
      tcpcon -> timeout = -1;					/* remote end has responded to outbound connect, don't time out the connection */
    }
    buf -> tcprawseq ++;					/* we consider the SYN to be at the beginning of the data, so skip a seq for it */
  }

  /* If we haven't received SYN yet, ignore the packet - we don't know if it is part of this connection or left over trash */

  if (!(tcpcon -> rcvdsyn)) {
    if (debug & DEBUG_ERRORS) {
      PRINTF "tcp sync not yet received from ");
      printipaddr (ippkt -> srcipaddr);
      PRINTF ",%u\n", sport);
    }
    goto finish;
  }

  /* Process header options - although theoretically we should do this in order, since the only option we care about */
  /* is maxsendsize, do it here.  We don't want to have to keep the cxb around while we wait for the missing data.   */

  ignore  = (flags & TCP_FLAGS_HLNMSK) >> (TCP_FLAGS_HLNSHF - 2);	/* get header length in bytes */
  ignore -= ippkt -> dat.tcp.raw - ippkt -> dat.raw;			/* subtract base header size */
  if (ignore > buf -> tcprawlen) ignore = buf -> tcprawlen;		/* make sure it's no longer than all data */

  for (i = 0; i < ignore;) {
    switch (buf -> tcprawpnt[i]) {
      case TCP_OPT_END: goto opt_end;
      case TCP_OPT_NOP: { i ++; break; }
      case TCP_OPT_MSS: {
        if (buf -> tcprawpnt[i+1] == 4) {
          tcpcon -> maxsendsize = N2HW (buf -> tcprawpnt + i + 2);
          if (tcpcon -> maxsendsize > TCP_MAXSENDSIZE (hw)) tcpcon -> maxsendsize = TCP_MAXSENDSIZE (hw);
          tcpcon -> congestionwindow = tcpcon -> maxsendsize;
        }
      }
      default: {
        if (buf -> tcprawpnt[i+1] == 0) goto opt_end;
        i += buf -> tcprawpnt[i+1];
      }
    }
  }
opt_end:
  buf -> tcprawlen -= ignore;					/* remove options from data */
  buf -> tcprawpnt += ignore;

  /* If it has seqend < last good stuff, it is probably a keepalive packet. */
  /* In this case, send an ack.  If we didn't make a special case out of    */
  /* this, we would just toss it away as it would look like duplicate data. */

  if (!(flags & TCP_FLAGS_SYN)) {				/* don't bother for incoming SYN's tho */
								/* ... they have seq_receivedok=seq+1  */
    seqdiff = seqend - tcpcon -> seq_receivedok;
    if ((seqdiff < 0) || ((seqdiff == 0) && (((Long) (seq - tcpcon -> seq_receivedok)) < 0))) {
      xbuf = mallocsndkbuf (find_send_hw (tcpcon -> ripaddr, NULL, NULL, 1)); /* allocate send buffer */
      sendtcp (xbuf, tcpcon, 0, TCP_FLAGS_ACK, tcpcon -> seq_receivedok); /* send the ack */
    }
  }

  /* buf -> tcprawlen = length of just the data within cxb  */
  /* buf -> tcprawpnt = address of just the data within cxb */
  /* buf -> tcprawseq = seq of byte pointed to by tcprawpnt */
  /* buf -> tcphasfin = it has a FIN flag                   */

  /* If it is a repeat of data previously received, remove that from the front of the message */

  seqdiff = tcpcon -> seq_receivedok - buf -> tcprawseq;
  if (seqdiff > 0) {
    if (buf -> tcprawlen <= seqdiff) goto finish;
    buf -> tcprawlen -= seqdiff;
    buf -> tcprawpnt += seqdiff;
    buf -> tcprawseq += seqdiff;
  }

  /* Copy its data to the window buffer.  Rcvwindowins points to where in the buffer */
  /* we can insert data, rcvwindowrem points to where the user has to remove stuff.  */
  /* It is quite possible that we may not be able to save all (or any) of this data, */
  /* in which case it will be lost until the sender re-sends it.  This should not    */
  /* happen if the sender obeys our window sizes, though.                            */

  len = buf -> tcprawlen;					/* see how much data is in the packet */
  if (len == 0) {
    if (buf -> tcphasfin) goto queueit;				/* no data, queue if it has fin flag */
    goto finish;						/* if neither, go free it off */
  }
  seq = buf -> tcprawseq;					/* get seq of byte pointed to by tcprawpnt */
  siz = tcpwindowsize (tcpcon, tcpcon -> seq_receivedok);	/* this is how many bytes are available at ins, including wrap */
  if (siz == 0) goto finish;					/* can't copy any if window buffer is full */
  seqdiff = seq - tcpcon -> seq_receivedok;			/* this is how many bytes after ins we have to skip in the window buffer */
								/* - seq = seq of first byte of tcprawpnt */
								/* - seq_receivedok = seq of byte pointed to by rcvwindowins */
  if (seqdiff >= siz) goto finish;				/* if there's no room left in window buffer after that, trash buffer */
  siz -= seqdiff;						/* there is that much less room left in window */
  ins  = tcpcon -> rcvwindowins;				/* this is where seq_receivedok byte goes */
  ins += seqdiff;						/* this is where the tcprawseq byte goes */
  if (ins >= tcpcon -> windowsize) ins -= tcpcon -> windowsize; /* it may be wrapped */
  if (ins >= tcpcon -> windowsize) oz_crash ("oz_dev_ip proc_tcp: ins %u, tcpcon -> windowsize %u", ins, tcpcon -> windowsize);
  if (len > siz) {
    len = siz;							/* limit the length to copy to the available space in the buffer */
    buf -> tcprawlen = siz;					/* (we lose the rest - sender overflowed us, it will have to resend it) */
  }
  siz = tcpcon -> windowsize - ins;				/* this is size left from insertion point until wrap point */
  if (tcpcon -> windowprocess != NULL) oz_knl_process_setcur (tcpcon -> windowprocess); /* access user-supplied window buffer */
  if (len > siz) {						/* see if copy will wrap */
    checkwin99s (tcpcon -> windowbuff + ins, siz, tcpcon);
    checkwin99s (tcpcon -> windowbuff, len - siz, tcpcon);
    memcpy (tcpcon -> windowbuff + ins, buf -> tcprawpnt, siz);
    memcpy (tcpcon -> windowbuff, buf -> tcprawpnt + siz, len - siz);
  } else {
    checkwin99s (tcpcon -> windowbuff + ins, len, tcpcon);
    memcpy (tcpcon -> windowbuff + ins, buf -> tcprawpnt, len);
  }
  if (tcpcon -> windowprocess != NULL) oz_knl_process_setcur (oz_s_systemproc); /* unmap for safety's sake */

  /* Insert buf in rcvdoutofordr queue in order of tcprawseq, merging with anything it overlaps (or abutts) */

queueit:
  seqend = seq + buf -> tcprawlen + buf -> tcphasfin;		/* get seq at end of what's left of packet */
  for (lbuf = &(tcpcon -> rcvdoutofordr); (xbuf = *lbuf) != NULL;) {
    seqdiff = xbuf -> tcprawseq - seq;				/* compare xbuf's start sequence to buf's */
    if (seqdiff > 0) break;					/* if xbuf's > buf's, stop scan */
    seqdiff += xbuf -> tcprawlen;				/* xbuf's <= buf's, see if there is a gap */
    if (seqdiff < 0) lbuf = &(xbuf -> next);			/* gap, on to next */
    else if (seqdiff < seqend - seq) {				/* no gap, see which goes on farther */
      buf -> tcprawlen += xbuf -> tcprawlen - seqdiff;		/* buf goes farther, merge xbuf into buf */
      buf -> tcprawseq  = seq = xbuf -> tcprawseq;
      *lbuf = xbuf -> next;					/* remove xbuf from the list */
      freebuf (xbuf);						/* free it off */
    } else {
      goto finish;						/* buf completely redundant, ignore it */
    }
  }
  if ((xbuf != NULL) && (seqdiff <= buf -> tcprawlen)) {	/* see if this one can be merged with successor */
    xbuf -> tcprawlen += seqdiff;				/* if so, increase size of successor */
    xbuf -> tcprawseq -= seqdiff;				/* ... and back up its sequence accordingly */
    if (xbuf -> tcprawseq + xbuf -> tcprawlen + xbuf -> tcphasfin < seqend) { /* ... maybe extend it to cover buf's end */
      xbuf -> tcphasfin |= buf -> tcphasfin;
      xbuf -> tcprawlen  = seqend - xbuf -> tcprawseq - xbuf -> tcprawlen - xbuf -> tcphasfin;
    }
    freebuf (buf);						/* ... and we don't need this one anymore */
    buf = NULL;
  } else {
    buf -> next = xbuf;						/* there is a gap, link this one on list */
    *lbuf = buf;
  }

  /* Now eliminate anything in rcvdoutofordr that is contiguous with the known valid data in the window buffer */
  /* The sequence numbers in there should be in the range seq_receivedok..seq_receivedok+windowsize-1          */

  while ((xbuf = tcpcon -> rcvdoutofordr) != NULL) {		/* point to top buffer on queue */
    if (xbuf -> tcprawseq != tcpcon -> seq_receivedok) break;	/* see if contiguous */
    tcpcon -> rcvwindowins   += xbuf -> tcprawlen;		/* ok, increment pointer */
    if (tcpcon -> rcvwindowins > tcpcon -> rcvwindowrem + tcpcon -> windowsize) oz_crash ("oz_dev_ip: rcvwindowins %u, rcvwindowrem %u, windowsize %u", tcpcon -> rcvwindowins, tcpcon -> rcvwindowrem, tcpcon -> windowsize);
    tcpcon -> seq_receivedok += xbuf -> tcprawlen + xbuf -> tcphasfin; /* ... and increment sequence for next packet / what needs to be acked */
    tcpcon -> rcvdoutofordr   = xbuf -> next;			/* unlink buffer from queue */
    tcpcon -> rcvdfin        |= xbuf -> tcphasfin;		/* remember we hit the fin flag */
    freebuf (xbuf);						/* free it off */
    if (xbuf == buf) buf = NULL;				/* remember if we freed off newly received buffer */
  }

  /* Now if 'buf' (what was just received) is still on rcvdoutofordr,    */
  /* free off just the cxb part so a new message can be received into it */
  /* We have already copied all its data to the window buffer.           */

  if (buf != NULL) {
    freecxb (buf);						/* free just the cxb part, a receive will start on it with a new buf */
    buf -> tcprawpnt = NULL;					/* this pointer isn't any good anymore */
    buf = NULL;							/* don't start a receive on this buf */
  }

  /* Finish up by restarting any stalled processing */

finish:
  tcpreceive_test (tcpcon);					/* process data in window buffer to complete I/O requests */
  starttcpsend (NULL, tcpcon);					/* maybe transmit window has opened up to allow sending */
  tcpreplyack (tcpcon, 1);					/* maybe we need to send an ack */
  checktcpdead (tcpcon);					/* see if we finished off the connection */
  return (buf);							/* if buf isn't NULL, it will be re-received into */
}

static void checkwin99s (uByte *buf, uLong siz, Tcpcon *tcpcon)

{
  if (buf < tcpcon -> windowbuff) oz_crash ("oz_dev_ip checkwin99s: buf %p, windowbuff %p", buf, tcpcon -> windowbuff);
  if (siz > tcpcon -> windowsize) oz_crash ("oz_dev_ip checkwin99s: siz %x, windowsize %x", siz, tcpcon -> windowsize);
  if (buf + siz > tcpcon -> windowbuff + tcpcon -> windowsize) oz_crash ("oz_dev_ip checkwin99s: buf %p+%x, windowbuff %p+%x", buf, siz, tcpcon -> windowbuff, tcpcon -> windowsize);
}

/* Determine the begin sequence number for a TCP datagram */

static uLong tcpbegseq (Ippkt *ippkt)

{
  return (N2HL (ippkt -> dat.tcp.seq));
}

/* Determine the end sequence number for a TCP datagram.  Add 1 if it is a SYN or FIN. */

static uLong tcpendseq (Ippkt *ippkt)

{
  uLong seqend;
  uWord flags, totalen;

  totalen = N2HW (ippkt -> totalen);
  flags   = N2HW (ippkt -> dat.tcp.flags);

  seqend = (uByte *)ippkt + totalen - ippkt -> dat.raw - ((flags & TCP_FLAGS_HLNMSK) >> (TCP_FLAGS_HLNSHF - 2)) + tcpbegseq (ippkt);

  if (flags & (TCP_FLAGS_SYN | TCP_FLAGS_FIN)) seqend ++;

  return (seqend);
}

/* Determine the length of the data following the header of a TCP datagram */
/* This includes any option bytes that follow the base header              */

static uWord tcprawlen (Ippkt *ippkt)

{
  uWord rawlen, totalen;

  totalen = N2HW (ippkt -> totalen);
  rawlen  = (uByte *)ippkt + totalen - ippkt -> dat.tcp.raw;

  return (rawlen);
}

/* Start sending an ack if one is needed */

static void tcpreplyack (Tcpcon *tcpcon, int outofordr)

{
  Buf *buf;
  Long howmuchisreallyleftinwindowbuffer;
  Long howmuchmorewehavethanremotethinkswehave;
  Long howmuchremotethinksisleftinwindowbuffer;

  /* Don't send if we have been reset  */

  if (tcpcon -> reset != 0) return;

  /* - send if we have something to ack and it has been a while or we have something to send */
  /* - send if we have rcvdoutofordr                                                         */
  /* - send if we last sent a small wsize but we now have some more space                    */

  // there is something to ack
  //   and it's been a while
  //     or there is something to send (probably waiting for window to open up)

  if ((tcpcon -> seq_lastacksent != tcpcon -> seq_receivedok) 
    && ((tcpcon -> lasttimemsgrcvd + ACKTIME <= ticks) 							
     || ((Long)(tcpcon -> nextxmtseq - tcpcon -> seq_nextusersendata) < 0))) goto sendit;		

  // there is stuff received out of order

  if ((tcpcon -> rcvdoutofordr != NULL) && outofordr) goto sendit;

  // ack every third buffer received so sender won't think it has too much stuff outstanding

  if (tcpcon -> seq_receivedok - tcpcon -> seq_lastacksent > tcpcon -> maxsendsize * 3) goto sendit;

  // window opens up - but don't do three of these in a row or sender will think we are naking it

  howmuchremotethinksisleftinwindowbuffer = tcpcon -> lastwsizesent + tcpcon -> seq_lastacksent - tcpcon -> seq_receivedok;
  if ((tcpcon -> seq_lastacksent2 != tcpcon -> seq_lastacksent) || (howmuchremotethinksisleftinwindowbuffer < tcpcon -> maxsendsize)) {
    howmuchisreallyleftinwindowbuffer       = tcpwindowsize (tcpcon, tcpcon -> seq_receivedok);
    howmuchmorewehavethanremotethinkswehave = howmuchisreallyleftinwindowbuffer - howmuchremotethinksisleftinwindowbuffer;
    if (howmuchmorewehavethanremotethinkswehave > tcpcon -> windowsize / 3) goto sendit;
  }

  return;

sendit:
  buf = mallocsndkbuf (find_send_hw (tcpcon -> ripaddr, NULL, NULL, 1));
  sendtcp (buf, tcpcon, 0, TCP_FLAGS_ACK, tcpcon -> seq_receivedok);
}

/* See if tcp connection is dead and free off context block if so */

static int checktcpdead (Tcpcon *tcpcon)

{
  /* If the connection is reset or finished, wipe it out - this will abort any left-over receives, etc */

  if (tcpcon -> reset != 0) goto itsdead;		// if either end has reset it, the link is dead

  if (!(tcpcon -> sentfin)) goto itsalive;		// if this end hasn't sent a FIN, the link is alive
  if (!(tcpcon -> rcvdfin)) goto itsalive;		// if other end hasn't sent a FIN, the link is alive
  if (tcpcon -> nextackreq != NULL) goto itsalive;	// if it hasn't ackd all sent, link is alive

itsdead:
  tcpterminate (tcpcon);
  return (1);

itsalive:
  return (0);
}

/************************************************************************/
/*									*/
/*  Queue a request for tcp transmit					*/
/*									*/
/*    Input:								*/
/*									*/
/*	iopex  = request to be transmitted				*/
/*	       -> u.tcptransmit.p.rawsize = size of data		*/
/*	       -> u.tcptransmit.p.rawbuff = address of data		*/
/*	       -> u.tcptransmit.syn = 0 : normal data			*/
/*	                           else : flags to send			*/
/*	tcpcon = connection to transmit it on				*/
/*									*/
/*    Output:								*/
/*									*/
/*	request queued for transmission					*/
/*	it will be posted when the remote end acknowledges it		*/
/*									*/
/*    Note:								*/
/*									*/
/*	if rawbuff == NULL (and rawsize == 1), it is a 'send fin/syn' 	*/
/*	request and results in a FIN or SYN packet being sent, not data	*/
/*									*/
/************************************************************************/

static void queuetcptransmit (Iopex *iopex, Tcpcon *tcpcon)

{
  validtcpxmtq (tcpcon);
  iopex -> u.tcptransmit.retransmits  = 0;				/* hasn't been retransmitted yet */
  iopex -> u.tcptransmit.sendstarted  = 0;				/* haven't started sending it yet */
  iopex -> u.tcptransmit.lasttimesent = 0;
  iopex -> u.tcptransmit.startseq     = tcpcon -> seq_nextusersendata;	/* assign the outgoing sequence numbers */
  iopex -> next = NULL;							/* put on end of transmit queue */
  *(tcpcon -> lastxmtreq) = iopex;
  tcpcon -> lastxmtreq    = &(iopex -> next);
  tcpcon -> seq_nextusersendata  += iopex -> u.tcptransmit.p.rawsize;	/* increment sequence number for next one */
  tcpcon -> numxmtreqs ++;
  validtcpxmtq (tcpcon);
  starttcpsend (NULL, tcpcon);						/* maybe we can start sending it now */
}

/************************************************************************/
/*									*/
/*  This routine starts transmitting the data for sequence nextxmtseq	*/
/*									*/
/*    Input:								*/
/*									*/
/*	buf = a mallocsndubuf that we can use (or NULL to get our own)	*/
/*	tcpcon -> nextxmtseq = sequence to start sending at		*/
/*	tcpcon -> nextackreq -> u.tcptransmit.p.rawsize   = total size of data in this request
/*	tcpcon -> nextackreq -> u.tcptransmit.p.rawbuff   = start of data in this request
/*	tcpcon -> nextackreq -> u.tcptransmit.startseq    = sequence at beginning of buffer
/*	tcpcon -> nextackreq -> u.tcptransmit.retransmits = number of retransmits done in this buffer
/*	tcpcon -> nextackreq -> u.tcptransmit.sendstarted = first time a send was done from this buffer
/*									*/
/*    Output:								*/
/*									*/
/*	contents transmitted and iopex will be completed via iodone 	*/
/*	when the data has been acked by the remote end			*/
/*									*/
/************************************************************************/

static int starttcpsend (Buf *buf, Tcpcon *tcpcon)

{
  Hw *hw;
  int sentsomething;
  Iopex *iopex;
  Ippkt *ippkt;
  Long howmuchcansend, howmuchtoskip;
  uLong nsize, seqbeg, seqend, size;

  static int nestlevel = 0;

  if (++ nestlevel > ntcpcons) oz_crash ("oz_dev_ip starttcpsend: nestlevel %d, ntcpcons %d\n	buf %p, tcpcon %d, msubp %p", nestlevel, ntcpcons, buf, tcpcon, &(tcpcon -> msubp));

  validtcpxmtq (tcpcon);

  size   = tcpcon -> seq_lastrcvdwsize - tcpcon -> seq_lastrcvdack;	/* see thru what seq we are allowed to transmit */
  if (size > tcpcon -> congestionwindow) size = tcpcon -> congestionwindow; /* ... subject to congestion window, too */
  seqend = tcpcon -> seq_lastrcvdack + size;

  hw = find_send_hw (tcpcon -> ripaddr, NULL, NULL, 1);			/* figure out how to get there */
  if (hw == NULL) {
    if (tcpcon -> reset == 0) tcpcon -> reset = OZ_NOROUTETODEST;	/* can't, abort connection */
    if (buf != NULL) freebuf (buf);					/* free off the given buffer (if any) */
    -- nestlevel;
    return (0);								/* didn't send anything */
  }

  /* Find first sendable request, ie, skip over all requests that end before nextxmtseq */

  for (iopex = tcpcon -> nextackreq; iopex != NULL; iopex = iopex -> next) {
    howmuchcansend = iopex -> u.tcptransmit.startseq + iopex -> u.tcptransmit.p.rawsize - tcpcon -> nextxmtseq;
    if (howmuchcansend > 0) break;
  }

  /* Transmit starting at nextxmtseq up through as much as we have to seq_lastrcvdwsize */

  sentsomething = 0;							/* haven't sent anything yet */
  while (iopex != NULL) {
    howmuchtoskip = tcpcon -> nextxmtseq - iopex -> u.tcptransmit.startseq; /* see how much to skip at beginning of request */
    if (howmuchtoskip < 0) goto hitanhole;				/* abort if we hit an hole left by abortproc */
    size = iopex -> u.tcptransmit.p.rawsize;				/* get size of data in the request */
    if (howmuchtoskip >= size) {					/* if we skip it all, try next request in list */
      iopex = iopex -> next;
      continue;
    }
    howmuchcansend = seqend - tcpcon -> nextxmtseq;			/* see how much of it we can send */
    if (howmuchcansend <= 0) break;					/* done for now if windows won't allow any */
    if (howmuchcansend > tcpcon -> maxsendsize) howmuchcansend = tcpcon -> maxsendsize; /* can't do more than fits in a buffer */
    size -= howmuchtoskip;						/* see how much is left to be sent in this request */
    if (size > howmuchcansend) size = howmuchcansend;			/* don't send more than windows allow */
    if (iopex -> u.tcptransmit.p.rawbuff != NULL) {			/* check to see if it is a real data request */
      if ((buf == NULL) || (buf -> hw != hw)) {
        buf = mallocsndubuf (hw, starttcpsendagain, tcpcon, &(tcpcon -> msubp)); /* allocate a buffer for it */
        if (buf == NULL) break;						/* done for now if there aren't any available */
      }
      ippkt = IPPKT (hw, buf -> cxb);					/* point to where ip packet goes therein */
      ACCESSBUFFS (iopex);						/* switch to requestor's process to access data */
      memcpy (ippkt -> dat.tcp.raw, iopex -> u.tcptransmit.p.rawbuff + howmuchtoskip, size); /* copy data */
      while ((size < howmuchcansend) && (iopex -> next != NULL)) {	/* see if there is more data in the chain to send and we have room */
        if (iopex -> u.tcptransmit.sendstarted == 0) iopex -> u.tcptransmit.sendstarted = ticks;
        iopex = iopex -> next;						/* if so, point to it */
        if (iopex -> u.tcptransmit.startseq != tcpcon -> nextxmtseq + size) goto hitanhole; /* check for hole left by abortproc */
        if (iopex -> u.tcptransmit.p.rawbuff == NULL) break;		/* stop if it is a 'send fin/syn' request (they go all by themselves) */
        nsize = iopex -> u.tcptransmit.p.rawsize;			/* send data request, see how much is there */
        if (nsize > howmuchcansend - size) nsize = howmuchcansend - size; /* get as much of it as we can do */
        memcpy (ippkt -> dat.tcp.raw + size, iopex -> u.tcptransmit.p.rawbuff, nsize);
        size += nsize;							/* increase size being transmitted by that much */
      }
      if (iopex -> u.tcptransmit.sendstarted == 0) iopex -> u.tcptransmit.sendstarted = ticks;
      iopex -> u.tcptransmit.lasttimesent = ticks;			/* remember when we sent it so we know when to retry */
      sendtcp (buf, tcpcon, size, TCP_FLAGS_ACK | TCP_FLAGS_PSH, tcpcon -> seq_receivedok); /* start transmitting it */
    } else {
#if 00
      if ((iopex -> u.tcptransmit.syn & TCP_FLAGS_FIN) && (tcpcon -> nextackreq != iopex)) break; /* be sure other end has all data before sending fin */
#endif
      if (buf == NULL) buf = mallocsndkbuf (find_send_hw (tcpcon -> ripaddr, NULL, NULL, 1)); /* allocate a temp buffer */
      ippkt = IPPKT (hw, buf -> cxb);					/* point to where ip packet goes therein */
      if (iopex -> u.tcptransmit.syn & TCP_FLAGS_SYN) {
        ippkt -> dat.tcp.raw[0] = TCP_OPT_MSS;				/* syn request, make up the parameter info */
        ippkt -> dat.tcp.raw[1] = 4;
        H2NW (tcpcon -> maxsendsize, ippkt -> dat.tcp.raw + 2);
      }
      if (iopex -> u.tcptransmit.sendstarted == 0) iopex -> u.tcptransmit.sendstarted = ticks;
      iopex -> u.tcptransmit.lasttimesent = ticks;			/* remember when we sent it so we know when to retry */
      sendtcp (buf, tcpcon, 0, iopex -> u.tcptransmit.syn, tcpcon -> seq_receivedok); /* start transmitting it */
    }
    buf = NULL;								/* if we loop to send more, we'll need a new buffer */
    sentsomething = 1;							/* remember we sent something */
    tcpcon -> nextxmtseq += size;					/* advance transmit pointer by amount transmitted */
    validtcpxmtq (tcpcon);
  }
  if (buf != NULL) freebuf (buf);					/* if they gave us a buf we didn't use, free it */

  validtcpxmtq (tcpcon);

  -- nestlevel;
  return (sentsomething);

  /* If we hit an hole left by abortproc, reset the connection */

hitanhole:
  if (tcpcon -> reset == 0) tcpcon -> reset = OZ_ABORTED;
  if (buf != NULL) freebuf (buf);
  -- nestlevel;
  return (sentsomething);
}

static void starttcpsendagain (Buf *buf, void *tcpconv)

{
  starttcpsend (buf, tcpconv);
}

/************************************************************************/
/*									*/
/*  Validate the tcp transmit queue.  Call this routine before and 	*/
/*  after making changes to nextackreq, nextxmtseq, lastxmtreq, 	*/
/*  seq_lastrcvdack and seq_nextusersendata.				*/
/*									*/
/*    Input:								*/
/*									*/
/*	tcpcon -> seq_lastrcvdack: last received ack sequence number	*/
/*	        seq_lastrcvdwsize: seq number that remote end is allowing us to send up to
/*	      seq_nextusersendata: seq number to assign to next user data queued
/*	               nextxmtseq: sequence of next byte in nextackreq list that needs to be transmitted
/*	               nextackreq: next request that has data that needs to be acked
/*	               lastxmtreq: last request in nextackreq chain	*/
/*									*/
/************************************************************************/

static void validtcpxmtq (Tcpcon *tcpcon)

{
  int i;
  Iopex *iopex, **liopex;
  Long seqdiff;
  uLong numxmtreqs, startseq;

  if (debug & DEBUG_TCPXMTQ) {
    PRINTF "validtcpxmtq: tcpcon %p, lastrcvdack %u, nextusersendata %u\n", 
             tcpcon, tcpcon -> seq_lastrcvdack, tcpcon -> seq_nextusersendata);
    PRINTF "    nextackreq %p, nextxmtseq %u, lastxmtreq %p\n", 
             tcpcon -> nextackreq, tcpcon -> nextxmtseq, tcpcon -> lastxmtreq);
    PRINTF "    numxmtreqs %u\n", tcpcon -> numxmtreqs);
  }

  /* We should never transmit data that has already been acked by the remote end */

  seqdiff = tcpcon -> seq_lastrcvdack - tcpcon -> nextxmtseq;
  if (seqdiff > 0) {
    oz_crash ("oz_dev_ip validtcpxmtq: lastrcvdack %u, nextxmtseq %u", tcpcon -> seq_lastrcvdack, tcpcon -> nextxmtseq);
  }

  /* We should never try to transmit past the end of data that has been queued for transmit */

  seqdiff = tcpcon -> nextxmtseq - tcpcon -> seq_nextusersendata;
  if (seqdiff > 0) {
    oz_crash ("oz_dev_ip validtcpxmtq: nextxmtseq %u, nextusersendata %u", tcpcon -> nextxmtseq, tcpcon -> seq_nextusersendata);
  }

  /* Make sure all buffers queued for transmit make sense */

  numxmtreqs  = 0;
  startseq    = tcpcon -> seq_lastrcvdack;
  for (liopex = &(tcpcon -> nextackreq); (iopex = *liopex) != NULL; liopex = &(iopex -> next)) {
    numxmtreqs ++;

    /* The next buffer's sequence must be at or after the current sequence number */
    /* There might be a gap froma cancelled request                               */

    if (iopex != tcpcon -> nextackreq) {
      seqdiff = startseq - iopex -> u.tcptransmit.startseq;
      if (debug & DEBUG_TCPXMTQ) {
        if (seqdiff < 0) PRINTF "  (hole: %d)\n", - seqdiff);
        PRINTF "  %u: %p, startseq %u, rawsize %u, syn %x\n", numxmtreqs, iopex, iopex -> u.tcptransmit.startseq, iopex -> u.tcptransmit.p.rawsize, iopex -> u.tcptransmit.syn);
      }
      if (seqdiff > 0) {
        oz_knl_printk ("oz_dev_ip validtcpxmtq: tcpcon %p -> seq_nextusersendata %u\n", tcpcon, tcpcon -> seq_nextusersendata);
        oz_knl_printk ("oz_dev_ip validtcpxmtq: iopex %p -> u.tcptransmit:\n", iopex);
        oz_knl_dumpmem (sizeof iopex -> u.tcptransmit, &(iopex -> u.tcptransmit));
        oz_crash ("oz_dev_ip validtcpxmtq: req startseq is %u, last one endseq %u", iopex -> u.tcptransmit.startseq, startseq);
      }
    }

    /* The only time rawbuff is allowed to be null is with size 1 (for SYN, FIN packets) */

    if ((iopex -> u.tcptransmit.p.rawbuff == NULL) && (iopex -> u.tcptransmit.p.rawsize != 1)) {
      oz_crash ("oz_dev_ip validtcpxmtq: rawbuff null, rawsize %u", iopex -> u.tcptransmit.p.rawsize);
    }

    /* The next packet should start with sequence after this packet's last byte */

    startseq = iopex -> u.tcptransmit.startseq + iopex -> u.tcptransmit.p.rawsize;
  }
  if (debug & DEBUG_TCPXMTQ) {
    PRINTF "    oz_dev_ip_init+%x", ((OZ_Pointer)(oz_hw_getrtnadr (1))) - (OZ_Pointer)oz_dev_ip_init);
#if 000
    for (i = 1; i <= 4; i ++) {
      PRINTF "  %x", ((OZ_Pointer)(oz_hw_getrtnadr (i))) - (OZ_Pointer)oz_dev_ip_init);
    }
#endif
    PRINTF "\n");
  }

  /* The end-of-list pointer must be correct */

  if (tcpcon -> lastxmtreq != liopex) oz_crash ("oz_dev_ip validtcpxmtq: lastxmtreq %p, should be %p", tcpcon -> lastxmtreq, liopex);

  /* The seq of the next data to be queued must be gt seq of last data alreay in queue */
  /* There might be a gap from a cancelled request                                     */

  seqdiff = startseq - tcpcon -> seq_nextusersendata;
  if (seqdiff > 0) {
    oz_crash ("oz_dev_ip validtcpxmtq: seq_nextusersendata %u, last ended at %u", tcpcon -> seq_nextusersendata, startseq);
  }

  /* The number of requests queued must be exactly correct */

  if (tcpcon -> numxmtreqs != numxmtreqs) {
    oz_crash ("oz_dev_ip validtcpxmtq: numxmtreqs %u but %u on queue", tcpcon -> numxmtreqs, numxmtreqs);
  }
}

/************************************************************************/
/*									*/
/*  Send TCP datagram, free buffer when send completes			*/
/*									*/
/*    Input:								*/
/*									*/
/*	buf -> tcp.raw = raw data to be sent				*/
/*	tcpcon         = pointer to connect node			*/
/*	rawlen         = length of data in buf -> tcp.raw, 		*/
/*	                 not including any option longwords		*/
/*	flags          = tcp flags + option longword count		*/
/*	               + possibly TCP_FLAGX_KA (keepalive)		*/
/*	                 (if TCP_FLAGX_KA, rawlen must be 0)		*/
/*	seq_receivedok = ack number to send in packet, 			*/
/*	                 usually tcpcon -> seq_receivedok		*/
/*									*/
/*    Output:								*/
/*									*/
/*	if no device to send it on, 					*/
/*	  buffer gets freed off						*/
/*	  connection gets reset						*/
/*	otherwise, 							*/
/*	  buffer gets sent						*/
/*	  buffer gets freed off						*/
/*									*/
/************************************************************************/

static void sendtcp (Buf *buf, Tcpcon *tcpcon, uWord rawlen, uLong flags, uLong seq_receivedok)

{
  Cxb *cxb;
  Iopex *iopex;
  Ippkt *ippkt;
  Long seqdiff;
  uLong seq, seq_cws, seq_lws, sts, wsize;
  uWord totalen;

  cxb   = buf -> cxb;
  ippkt = IPPKT (buf -> hw, cxb);

  validtcpxmtq (tcpcon);
  seq = tcpcon -> nextxmtseq;						/* get sequence of next stuff to transmit */
  if (flags & TCP_FLAGX_KA) seq --;					/* - decrement if keepalive */

  /* Fill in IP and TCP headers */

  while (++ ident == 0) {}						/* give it a unique ident number */

  flags  += (ippkt -> dat.tcp.raw - ippkt -> dat.raw) 			/* add base header size to flags word */
         << (TCP_FLAGS_HLNSHF - 2);

  totalen = ippkt -> dat.raw - (uByte *)ippkt 				/* total message length = ip header size */
          + ((flags & TCP_FLAGS_HLNMSK) >> (TCP_FLAGS_HLNSHF - 2))      /* + tcp header size (incl options) */
          + rawlen;				                        /* + raw data length */

  if (flags & TCP_FLAGS_SYN) tcpcon -> sentsyn = 1;			/* remember that we've sent a SYN */
  if (flags & TCP_FLAGS_FIN) tcpcon -> sentfin = 1;			/* remember that we've sent a FIN */
  if (flags & TCP_FLAGS_RST) {
    if (tcpcon -> sentrst) {
      freebuf (buf);							/* only send one reset per connection */
      return;
    }
    tcpcon -> sentrst = 1;						/* remember that we've sent a RST */
  }

  ippkt -> hdrlenver  = 0x45;						/* set up ip header length and version */
  ippkt -> typeofserv = 0;						/* type of service = 0 */
  H2NW (totalen, ippkt -> totalen);					/* save total length in ip header */
  H2NW (ident, ippkt -> ident);						/* set up message ident in ip header */
  H2NW (0, ippkt -> flags);						/* no ip flags */
  ippkt -> ttl = 255;							/* set up time-to-live */
  ippkt -> proto = PROTO_IP_TCP;					/* set up ip protocol = tcp */
  CPYIPAD (ippkt -> srcipaddr, tcpcon -> lipaddr);			/* set up source ip address */
  CPYIPAD (ippkt -> dstipaddr, tcpcon -> ripaddr);			/* set up destination ip address */
  H2NW (tcpcon -> lsockno, ippkt -> dat.tcp.sport);			/* set up source port number */
  H2NW (tcpcon -> rsockno, ippkt -> dat.tcp.dport);			/* set up destination port number */
  H2NL (seq, ippkt -> dat.tcp.seq);					/* set up seq number for first byte */
  H2NW (flags, ippkt -> dat.tcp.flags);					/* save the flags */
  H2NW (0, ippkt -> dat.tcp.urgent);					/* clear urgent pointer (we don't use it) */

  /* Set up the window size */

  seq_lws = tcpcon -> seq_lastacksent + tcpcon -> lastwsizesent;	/* this is the seq we told it it could send last time */
  seq_cws = tcpwindowsize (tcpcon, seq_receivedok) + seq_receivedok;	/* this is the seq we can handle now */
  seqdiff = seq_cws - seq_lws;						/* this is how many more we can do this time */
  if ((seqdiff < 0) 							/* always send any decreases */
   || (seqdiff >= tcpcon -> maxsendsize) 				/* only send respectable increases */
   || (seqdiff >= tcpcon -> windowsize / 2) 				/* ... to avoid silly window syndrome */
   || (seq_cws - seq_receivedok == tcpcon -> windowsize)) {		/* (catch-all when window completely opens up) */
    seq_lws = seq_cws;
  }
  wsize = seq_lws - seq_receivedok;
  if (wsize > 65535) wsize = 65535;					/* this is the largest we can possibly send */
  H2NW (wsize, ippkt -> dat.tcp.wsize);
  tcpcon -> lastwsizesent  = wsize;					/* remember what we last sent */

  /* Set up the ack sequence number = last received good sequential data */

  H2NL (seq_receivedok, ippkt -> dat.tcp.ack);				/* set up seq of next contiguous byte to be received */
  tcpcon -> seq_lastacksent2 = tcpcon -> seq_lastacksent;
  tcpcon -> seq_lastacksent  = seq_receivedok;				/* remember we tried to send it */
  tcpcon -> lasttimemsgsent  = ticks;					/* remember when it was sent */

  /* Start sending it */

  if (tcpcon -> reset != 0) sendip (buf, NULL, NULL);			/* if connection is reset, no completion routine -   */
									/* we're probably just sending a reset packet and we */
									/* don't want to get in an infinite loop aborting    */
  else {
    tcpcon -> sendpend ++;						/* normal, remeber we got a send in progress */
    sts = sendip (buf, sendtcpdone, tcpcon);				/* start sending the packet */
    if (sts != OZ_STARTED) sendtcpdone (tcpcon, sts);			/* it completed already, call completion routine */
  }

  validtcpxmtq (tcpcon);
}

/* If unable to send (like no arp response), abort the connection */

static void sendtcpdone (void *tcpconv, uLong status)

{
  Tcpcon *tcpcon;

  tcpcon = tcpconv;
  if ((status != OZ_SUCCESS) && (tcpcon -> reset == 0)) {	/* check for error status */
    if (debug & DEBUG_ERRORS) {
      PRINTF "sendtcp error %u for ", status);
      printtcpcon (tcpcon);
      PRINTF "\n");
    }
    if (tcpcon -> reset == 0) tcpcon -> reset = status; 	/* transmit failed, abort connection with the failure status */
  }
  validtcpxmtq (tcpcon);

  tcpcon -> sendpend --;					/* one less transmit in progress */
  if (tcpcon -> sendpend == 0) checktcpdead (tcpcon);		/* if idle, maybe we can close the connection now */
}

/* Calculate current windowsize for a connection, ie, how many bytes can we possibly receive beginning with */
/* seq_receivedok.  This is the actual number of bytes that will fit into the window buffer given what has  */
/* been received and what the user has processed already.  This routine may return a number > 65535.        */

static uLong tcpwindowsize (Tcpcon *tcpcon, uLong seq_receivedok)

{
  uLong wsize;

  if (tcpcon -> rcvwindowins < tcpcon -> rcvwindowrem) oz_crash ("oz_dev_ip: rcvwindowins %u, rcvwindowrem %u", tcpcon -> rcvwindowins, tcpcon -> rcvwindowrem);
  wsize = tcpcon -> rcvwindowins - tcpcon -> rcvwindowrem;	/* this is how much room is occupied by good contig data */
  if (wsize > tcpcon -> windowsize) oz_crash ("oz_dev_ip: wsize %u, windowsize %u", wsize, tcpcon -> windowsize);
  wsize  = tcpcon -> windowsize - wsize;			/* this is how much room is available */
  wsize += seq_receivedok - tcpcon -> seq_receivedok;		/* sometimes caller wants window size from a moment ago */
  return (wsize);
}

/************************************************************************/
/*									*/
/*  Bounce an incoming message by sending an icmp error message back	*/
/*									*/
/*    Input:								*/
/*									*/
/*	buf      = pointer to message being bounced			*/
/*	icmptype = icmp error message type				*/
/*	icmpcode = icmp error message code				*/
/*	extra    = extra longword (bytes 4..7)				*/
/*									*/
/*    Output:								*/
/*									*/
/*	icmp message sent (eventually)					*/
/*	buf freed off							*/
/*									*/
/************************************************************************/

static void sendicmpbounce (Buf *buf, uByte icmptype, uByte icmpcode, uLong extra)

{
  Buf *icmpbuf;
  const char *why;
  Cxb *cxb, *icmpcxb;
  Hw *hw;
  Ippkt *icmpip, *ip;
  uLong maxcopylen, sincelastbounce, totalen;
  uWord cksm;

  cxb = buf -> cxb;
  hw  = buf -> hw;
  ip  = IPPKT (hw, cxb);

  if (debug & DEBUG_BOUNCE) {
    PRINTF "send icmp bounce type %u code %u for %u byte proto %u packet\n  from ", icmptype, icmpcode, N2HW (ip -> totalen), ip -> proto);
    printipaddr (ip -> srcipaddr);
    if (ip -> proto == PROTO_IP_TCP) PRINTF ",%u", N2HW (ip -> dat.tcp.sport));
    if (ip -> proto == PROTO_IP_UDP) PRINTF ",%u", N2HW (ip -> dat.udp.sport));
    PRINTF "  to  ");
    printipaddr (ip -> dstipaddr);
    if (ip -> proto == PROTO_IP_TCP) PRINTF ",%u", N2HW (ip -> dat.tcp.dport));
    if (ip -> proto == PROTO_IP_UDP) PRINTF ",%u", N2HW (ip -> dat.udp.dport));
    PRINTF "\n");
  }

  /* For several cases, just throw the packet away to avoid looping */

  why = "non PINGREQ ICMP";					/* ICMP packet that is not a PINGREQ */
  if ((ip -> proto == PROTO_IP_ICMP) && (ip -> dat.icmp.type != PROTO_IP_ICMP_PINGREQ)) goto throw_away;

  why = "not first fragment";					/* not the first of a fragment */
  if (N2HW (ip -> flags) & IP_FLAGS_OFFS) goto throw_away;

  why = "bounce rate exceeded";					/* cant send too many at once to avoid flooding out */
  sincelastbounce  = ticks - hw -> lastbounce;
  if (sincelastbounce < 2) goto throw_away;
  if (sincelastbounce > 65536) sincelastbounce = 65536;
  hw -> lastbounce = ticks - sincelastbounce / 2 + 1;

  /* Ok, send back a ICMP error packet of some sort */

  totalen = N2HW (ip -> totalen);				/* get length of original ip datagram */
  maxcopylen = ip -> dat.raw + ICMP_BOUNCE_TOTALEN - ip -> dat.icmp.raw - 4;
  if (totalen > maxcopylen) totalen = maxcopylen;		/* make sure we don't copy more than what would be ICMP_BOUNCE_TOTALEN */

  icmpbuf = mallocsndkbuf (find_send_hw (ip -> srcipaddr, NULL, NULL, 1)); /* allocate buffer to send bounce message in */
  if (icmpbuf != NULL) {
    icmpcxb = icmpbuf -> cxb;
    icmpip  = IPPKT (icmpbuf -> hw, icmpcxb);

    icmpip -> dat.icmp.type = icmptype;				/* set up icmp message type */
    icmpip -> dat.icmp.code = icmpcode;				/* ... and icmp error code */
    H2NL (extra, icmpip -> dat.icmp.raw + 0);			/* set up extra longword */
    memcpy (icmpip -> dat.icmp.raw + 4, ip, totalen);		/* copy the data starting at ip header */

    totalen += icmpip -> dat.icmp.raw + 4 - (uByte *)icmpip;	/* get the total length I am sending (max ICMP_BOUNCE_TOTALEN) */
    while (++ ident == 0) {}					/* get an ident for bounce message */

    icmpip -> hdrlenver  = 0x40 + (IPHDRSIZE / 4);		/* set up ip version and header length */
    icmpip -> typeofserv = 0;					/* type of service = 0 */
    H2NW (totalen, icmpip -> totalen);				/* total length of icmp bounce message */
    H2NW (ident, icmpip -> ident);				/* set up its ident */
    H2NW (0, ip -> flags);					/* clear any frag or other flags */
    icmpip -> ttl = ICMP_BOUNCE_TTL;				/* set time-to-live */
    icmpip -> proto = PROTO_IP_ICMP;				/* it is an ICMP message */
    CPYIPAD (icmpip -> dstipaddr, ip -> srcipaddr);		/* going back to original sender */

    H2NW (0, ip -> dat.icmp.cksm);				/* calculate icmp checksum */
    cksm = oz_dev_ip_icmpcksm (ip);
    H2NW (cksm, ip -> dat.icmp.cksm);

    sendip (icmpbuf, NULL, NULL);				/* send icmp packet */
  }
  freebuf (buf);						/* free off buffer that caused the bounce */
  return;

  /* Not supposed to send ICMP error under these conditions, just toss packet */

throw_away:
  if (debug & DEBUG_BOUNCE) PRINTF " - %s\n", why);
  freebuf (buf);
}

/************************************************************************/
/*									*/
/*  Start sending an ip datagram that originates on this computer	*/
/*									*/
/*    Input:								*/
/*									*/
/*	buf -> cxb -> PKT.ip = ip message to send			*/
/*	buf -> hw = hw interface to send it on				*/
/*	ip header checksum not yet set up				*/
/*	srcipaddr not yet set up					*/
/*	donentry, donparam = completion routine, if any			*/
/*									*/
/*    Output:								*/
/*									*/
/*	sendip = OZ_STARTED : donentry will be called on completion	*/
/*	               else : error status				*/
/*									*/
/************************************************************************/

static uLong sendip (Buf *buf, void (*donentry) (void *donparam, uLong status), void *donparam)

{
  uByte proto;
  Cxb *cxb;
  Hw *hw;
  Ippkt *ippkt;
  uWord cksm, tcphdrlen, totalen;

  /* Make some basic validity checks */

  cxb     = buf -> cxb;
  ippkt   = IPPKT (buf -> hw, cxb);
  totalen = N2HW (ippkt -> totalen);
  proto   = ippkt -> proto;

  if (ippkt -> hdrlenver != 0x45) goto badpkt;
  if (totalen < ippkt -> dat.raw - (uByte *)ippkt) goto badpkt;
  switch (proto) {
    case PROTO_IP_ICMP: {
      if (totalen < ippkt -> dat.icmp.raw - (uByte *)ippkt) goto badpkt;
      break;
    }
    case PROTO_IP_UDP: {
      if (totalen < ippkt -> dat.udp.raw - (uByte *)ippkt) goto badpkt;
      break;
    }
    case PROTO_IP_TCP: {
      tcphdrlen = (N2HW (ippkt -> dat.tcp.flags) & TCP_FLAGS_HLNMSK) >> (TCP_FLAGS_HLNSHF - 2);
      if (tcphdrlen < ippkt -> dat.tcp.raw - ippkt -> dat.raw) goto badpkt;
      if (totalen < ippkt -> dat.raw + tcphdrlen - (uByte *)ippkt) goto badpkt;
      break;
    }
    default: goto badpkt;
  }

  /* Check output filters */

  if (!filter_check (&output_filters, ippkt)) {
    if (debug & DEBUG_ERRORS) {
      PRINTF "output filtered ");
      printipaddr (buf -> sendipaddr);
      if (!CEQIPAD (buf -> sendipaddr, ippkt -> dstipaddr)) {
        PRINTF " (");
        printipaddr (ippkt -> dstipaddr);
        PRINTF ")");
      }
      PRINTF "\n");
    }
    freebuf (buf);
    return (OZ_NOROUTETODEST);
  }

  /* Find out which interface to send it on and set the source ip address based on interface */

  hw = find_send_hw (ippkt -> dstipaddr, ippkt -> srcipaddr, buf -> sendipaddr, 1);

  /* If it can't be sent, return failure status */

  if (hw == NULL) {
    if (debug & DEBUG_ERRORS) {
      PRINTF "no interface for ");
      printipaddr (buf -> sendipaddr);
      if (!CEQIPAD (buf -> sendipaddr, ippkt -> dstipaddr)) {
        PRINTF " (");
        printipaddr (ippkt -> dstipaddr);
        PRINTF ")");
      }
      PRINTF "\n");
    }
    freebuf (buf);
    return (OZ_NOROUTETODEST);
  }

  /* Make sure totalen doesn't exceed mtu */

  if (totalen > hw -> mtu) goto badpkt;

  /* Compute checksums */

  H2NW (0, ippkt -> hdrcksm);			/* set up ip header checksum */
  cksm = oz_dev_ip_ipcksm (ippkt);
  H2NW (cksm, ippkt -> hdrcksm);
  if (proto == PROTO_IP_UDP) {			/* set up udp checksum */
    H2NW (0, ippkt -> dat.udp.cksm);
    cksm = oz_dev_ip_udpcksm (ippkt);
    H2NW (cksm, ippkt -> dat.udp.cksm);
  }
  if (proto == PROTO_IP_TCP) {			/* set up tcp checksum */
    H2NW (0, ippkt -> dat.tcp.cksm);
    cksm = oz_dev_ip_tcpcksm (ippkt);
    H2NW (cksm, ippkt -> dat.tcp.cksm);
  }

  /* Double check to make sure hw is the same as when buffer is allocated so it will be the same layout      */
  /* Also, for SNDU bufs, it must match so we aren't taking buffs from one transmitter and giving to another */

  if (buf -> hw != hw) oz_crash ("oz_dev_ip: trying to change hw of a %d buffer", buf -> type);

  /* Set the ethernet protocol now that we know where it goes */

  seteproto (PROTO_IP, buf);

  /* Maybe print out tcp header stuff */

  if ((debug & DEBUG_TCPHDR) && (ippkt -> proto == PROTO_IP_TCP)) {
    PRINTF "                                  tcp ");
    printipaddr (ippkt -> srcipaddr);
    PRINTF ",%u ", N2HW (ippkt -> dat.tcp.sport));
    printipaddr (ippkt -> dstipaddr);
    PRINTF ",%u\n", N2HW (ippkt -> dat.tcp.dport));
    PRINTF "                                    seq %u..%u  ack %u\n", tcpbegseq (ippkt), tcpendseq (ippkt), N2HL (ippkt -> dat.tcp.ack));
    PRINTF "                                    flags ");
    printtcpflags (N2HW (ippkt -> dat.tcp.flags));
    PRINTF "  rawlen %u  wsize %u\n", tcprawlen (ippkt), N2HW (ippkt -> dat.tcp.wsize));
    PRINTF "                                    buf %p\n", buf);
  }

  sendippkt (buf, donentry, donparam);
  return (OZ_STARTED);

  /* Packet fails basic validity checks */

badpkt:
  oz_knl_printk ("oz_dev_ip: sendip packet %p/%p/%p fails basic validity checks\n", buf, cxb, ippkt);
  oz_knl_dumpmem (48, ippkt);
  freebuf (buf);
  return (OZ_BUGCHECK);
}

/************************************************************************/
/*									*/
/*  Start sending an ip packet						*/
/*									*/
/*    Input:								*/
/*									*/
/*	buf -> hw = hardware to send it on				*/
/*	buf -> sendipaddr = ip address to send it to, assumed to be 	*/
/*	                    arp-able from the hw			*/
/*	all ip data all set up, incl checksums, etc			*/
/*	totalen assumed to be <= hw -> mtu				*/
/*									*/
/*    Output:								*/
/*									*/
/*	donentry called upon completion (or failure)			*/
/*									*/
/************************************************************************/

static void sendippkt (Buf *buf, void (*donentry) (void *donparam, uLong status), void *donparam)

{
  Arpc *arpc;
  Buf *arpbuf;
  Cxb *cxb;
  Hw *hw;
  Ippkt *ippkt;
  uWord totalen;

  hw    = buf -> hw;
  cxb   = buf -> cxb;
  ippkt = IPPKT (hw, cxb);
  seteproto (PROTO_IP, buf);
  buf -> donentry = donentry;
  buf -> donparam = donparam;
  totalen = N2HW (ippkt -> totalen);
  if (totalen > hw -> mtu) oz_crash ("oz_dev_ip sendippkt: totalen %u, %s mtu %u", totalen, hw -> devname, hw -> mtu);

  /* Get ethernet address from arp cache - note that the arpc cache is assumed to contain an entry for the interface itself */

  for (arpc = hw -> arpcs; arpc != NULL; arpc = arpc -> next) {	/* scan the arp cache for the device */
    if (CEQIPAD (arpc -> ipaddr, buf -> sendipaddr)) {		/* see if this ip is found */
      sendpkt (buf, totalen, arpc -> enaddr);			/* it is, send the ip packet to it */
      return;
    }
  }

  /* Not found, send an arp request packet */

  if ((debug & DEBUG_TCPHDR) && (ippkt -> proto == PROTO_IP_TCP)) {
    PRINTF "                                    arping for ");
    printipaddr (buf -> sendipaddr);
    PRINTF "\n");
  }

  buf -> next     = hw -> arpwaits;				/* link the buffer to 'waiting for arp' list */
  buf -> timeout  = ticks + ARP_REQ_LIFE;
  buf -> donentry = donentry;
  buf -> donparam = donparam;
  hw  -> arpwaits = buf;

  for (arpbuf = buf -> next; arpbuf != NULL; arpbuf = arpbuf -> next) { /* don't arp for same ip more than once at a time */
    if (CEQIPAD (arpbuf -> sendipaddr, buf -> sendipaddr)) return;
  }

  sendarpreq (hw, ippkt -> srcipaddr, buf -> sendipaddr);
}

/* Send arp request - hwipaddr=my ip address, tipaddr=target ip address */

static void sendarpreq (Hw *hw, const uByte hwipaddr[IPADDRSIZE], const uByte tipaddr[IPADDRSIZE])

{
  Arppkt *arppkt;
  Buf *arpbuf;
  Cxb *arpcxb;
  uByte *p;

  arpbuf = mallocsndkbuf (hw);					/* allocate an internal send buffer */
  arpcxb = arpbuf -> cxb;
  arppkt = ARPPKT (hw, arpcxb);

  seteproto (PROTO_ARP, arpbuf);

  H2NW (hw -> ether_getinfo1.arphwtype, arppkt -> hardtype);	/* hardware type = ethernet */
  H2NW (PROTO_IP, arppkt -> prottype);				/* protocol type = IP */
  arppkt -> hardsize = hw -> ether_getinfo1.addrsize;		/* hardware address size = 6 */
  arppkt -> protsize = IPADDRSIZE;				/* protocol address size = 4 */
  H2NW (ARP_OP_REQ, arppkt -> op);				/* this is a request, not a reply */

  CPYENAD (arppkt -> var, hw -> myenaddr, hw);								// sender's ethernet address
  CPYIPAD (arppkt -> var + hw -> ether_getinfo1.addrsize, hwipaddr);					// sender's ip address
  memclr (arppkt -> var + hw -> ether_getinfo1.addrsize + IPADDRSIZE, hw -> ether_getinfo1.addrsize);	// target's ethernet address
  CPYIPAD (arppkt -> var + 2 * hw -> ether_getinfo1.addrsize + IPADDRSIZE, tipaddr);			// target's ip address

  sendpkt (arpbuf, arppkt -> var + 2 * (hw -> ether_getinfo1.addrsize + IPADDRSIZE) - (uByte *)arppkt, arpenaddr); // send it
}

/************************************************************************/
/*									*/
/*  Find hardware interface						*/
/*									*/
/************************************************************************/

static Hw *find_hw_by_name (const char *devname)

{
  Hw *hw;

  for (hw = hws; hw != NULL; hw = hw -> next) {
    if (strcmp (hw -> devname, devname) == 0) break;
  }
  return (hw);
}

/************************************************************************/
/*									*/
/*  Find hardware interface to send on					*/
/*									*/
/*    Input:								*/
/*									*/
/*	ipaddr = ip address that packet is to be sent to		*/
/*									*/
/*    Output:								*/
/*									*/
/*	find_send_hw = NULL : there is no route to the destination	*/
/*	               else : hw of the interface to send on		*/
/*	*myipaddr = ip address of interface to send on			*/
/*	*gwipad   = gateway's ip address (same as ipaddr if directly connected
/*									*/
/************************************************************************/

static Hw *find_send_hw (const uByte *ipaddr, uByte *myipaddr, uByte *gwipaddr, int inclroutes)

{
  Hw *besthw, *hw;
  Hwipam *hwipam;
  Route *route;
  uByte bestmask[IPADDRSIZE];

  CLRIPAD (bestmask);
  besthw = NULL;
  route  = NULL;

  /* Find hardware that matches that has the narrowest mask */

  if (gwipaddr != NULL) CPYIPAD (gwipaddr, ipaddr);					/* if directly connected, gateway ip address = target ip address */
  for (hw = hws; hw != NULL; hw = hw -> next) {						/* scan all interfaces */
    for (hwipam = hw -> hwipams; hwipam != NULL; hwipam = hwipam -> next) {		/* scan all network definitions for it */
      if (!CEQIPADM (ipaddr, hwipam -> nwipaddr, hwipam -> nwipmask)) continue;		/* skip if network ip address/mask dont match */
      if (!CGTIPAD (hwipam -> nwipmask, bestmask) && (besthw != NULL)) continue;	/* skip if its not better than the last one we found */
      besthw = hw;									/* its the best so far, save it */
      if (myipaddr != NULL) CPYIPAD (myipaddr, hwipam -> hwipaddr);
      CPYIPAD (bestmask, hwipam -> nwipmask);
    }
  }

  /* Now see if we can find a router that has a narrower mask */

  if (inclroutes) {
    for (route = routes; route != NULL; route = route -> next) {			/* scan all routers */
      if (route -> hw == NULL) continue;						/* skip if we cant access the router */
      if (!CEQIPADM (ipaddr, route -> nwipaddr, route -> nwipmask)) continue;		/* skip if netword ip address/mask dont match */
      if (!CGTIPAD (route -> nwipmask, bestmask) && (besthw != NULL)) continue;		/* skip if its not better than the last one we found */
      besthw = route -> hw;								/* its the best so far, save it */
      if (myipaddr != NULL) CPYIPAD (myipaddr, route -> hwipaddr);
      if (gwipaddr != NULL) CPYIPAD (gwipaddr, route -> gwipaddr);
      CPYIPAD (bestmask, route -> nwipmask);
    }
  }

  /* Return pointer to interface (or NULL if nothing found at all) */

  for (hw = hws; hw != besthw; hw = hw -> next) {
    if (hw == NULL) oz_crash ("oz_dev_ip find_send_hw: couldn't find %p on hws list", besthw);
  }

  return (besthw);
}

/************************************************************************/
/*									*/
/*  Find the hardware interface given the exact ip address of the 	*/
/*  interface								*/
/*									*/
/************************************************************************/

static int ismyipaddr (const uByte ipaddr[IPADDRSIZE])

{
  Hw *hw;
  Hwipam *hwipam;

  for (hw = hws; hw != NULL; hw = hw -> next) {
    for (hwipam = hw -> hwipams; hwipam != NULL; hwipam = hwipam -> next) {
      if (CEQIPAD (ipaddr, hwipam -> hwipaddr)) return (1);
    }
  }
  return (0);
}

/************************************************************************/
/*									*/
/*  See if given ip address is the network or broadcast ip address	*/
/*									*/
/************************************************************************/

static int isnwbcipad (const uByte ipaddr[IPADDRSIZE], Hw *hw)

{
  Hwipam *hwipam;
  Ipad ipaddrl, nwipaddrl, nwipmaskl;

  ipaddrl = *(Ipad *)ipaddr;
  if (ipaddrl == 0) return (1);
  if (ipaddrl == (Ipad)(-1)) return (1);

  for (hwipam = hw -> hwipams; hwipam != NULL; hwipam = hwipam -> next) {
    nwipaddrl = *(Ipad *)(hwipam -> nwipaddr);
    nwipmaskl = *(Ipad *)(hwipam -> nwipmask);
    if (ipaddrl == (nwipaddrl &   nwipmaskl)) return (1);
    if (ipaddrl == (nwipaddrl | ~ nwipmaskl)) return (1);
  }
  return (0);
}

/************************************************************************/
/*									*/
/*  Start sending a packet						*/
/*									*/
/*    Input:								*/
/*									*/
/*	buf -> hw  = device						*/
/*	buf -> cxb = message data					*/
/*	buf -> donentry = routine to be called when complete		*/
/*	totalen = size of message to send, not including any ethernet 	*/
/*	          header data, but starting with arp or ip header	*/
/*	enaddr = who to send it to					*/
/*									*/
/*    Output:								*/
/*									*/
/*	message eventually sent and buffer freed off			*/
/*	donentry routine called						*/
/*									*/
/************************************************************************/

static void sendpkt (Buf *buf, uWord totalen, const uByte enaddr[OZ_IO_ETHER_MAXADDR])

{
  Cxb *cxb;
  Eproto eproto;
  Hw *hw;
  OZ_IO_ether_transmit ether_transmit;
  OZ_Iochan *iochan;
  uLong sts, vl;

  cxb = buf -> cxb;
  hw  = buf -> hw;
  buf -> cxb_dlen = totalen;			/* length of ethernet data (doesn't include the dstaddr, srcaddr, eproto fields) */

  /* If being sent to self, just put it on receive queue */

  if (CEQENAD (enaddr, hw -> myenaddr, hw)) {
    if (debug & DEBUG_ETHERHDR) {
      PRINTF "send daddr ");
      printenaddr (hw, enaddr);
      PRINTF " to self ");
      PRINTF " eproto %4.4X len %u\n", eproto, totalen);
    }
    if (buf -> donentry != NULL) {
      (*(buf -> donentry)) (buf -> donparam, OZ_SUCCESS);
      buf -> donentry = NULL;
    }
    buf -> next  = NULL;
    buf -> iosts = OZ_SUCCESS;
    vl = oz_hw_smplock_wait (&smplock_vl);
    *rcvqt = buf;
    rcvqt  = &(buf -> next);
    oz_hw_smplock_clr (&smplock_vl, vl);
    return;
  }

  /* Get I/O channel given the ethernet protocol number */

  eproto = geteproto (buf);						/* get ethernet protocol number */
  iochan = NULL;							/* assume it is invalid */
  if (eproto == PROTO_ARP) iochan = hw -> iochan_arp;			/* maybe it is ARP protocol */
  if (eproto == PROTO_IP)  iochan = hw -> iochan_ip;			/* maybe it is IP protocol */
  if (iochan == NULL) oz_crash ("oz_dev_ip sendpkt: bad ethernet protocol 0x%x", eproto);

  /* Fill in ethernet header */

  CPYENAD (cxb + hw -> ether_getinfo1.dstaddrof, enaddr, hw);		/* ethernet address it is going to */
  CPYENAD (cxb + hw -> ether_getinfo1.srcaddrof, hw -> myenaddr, hw);	/* address it is coming from */

  /* If we are pretending there is a failure rate on the channel, just pretend we sent it successfully */
  /* We just leave buf->xmtdrv and buf->cxb as they are, so we end up just re-using this same buffer   */

  if (debugfailrate ()) {
    hw -> transmitsinprog ++;
    writeast (buf, OZ_SUCCESS);
    return;
  }

  /* Start transmitting */

  memset (&ether_transmit, 0, sizeof ether_transmit);
  ether_transmit.dlen = totalen;		/* length of data */
  if (buf -> xmtdrv == NULL) {			/* see if our buf or ethernet driver's */
    ether_transmit.size = hw -> ether_getinfo1.dataoffset + totalen;
    ether_transmit.buff = cxb;			/* ours, just point to it */
  } else {
    ether_transmit.xmtdrv   = buf -> xmtdrv;	/* ethernet driver's, set up pointer to it */
    ether_transmit.xmtdrv_r = &(buf -> xmtdrv);	/* tell it to give us a replacement here */
    ether_transmit.xmteth_r = &(buf -> cxb);
    buf -> xmtdrv = NULL;			/* make sure we get a replacement */
    buf -> cxb    = NULL;
  }

  if (debug & DEBUG_ETHERHDR) {
    PRINTF "send daddr ");
    printenaddr (hw, cxb + hw -> ether_getinfo1.dstaddrof);
    PRINTF " saddr ");
    printenaddr (hw, cxb + hw -> ether_getinfo1.srcaddrof);
    PRINTF " eproto %4.4x len %u\n", eproto, totalen);
  }

  hw -> transmitsinprog ++;			/* one more transmit to wait for when we terminate */
  sts = oz_knl_iostart2 (1, iochan, OZ_PROCMODE_KNL, writeast, buf, NULL, NULL, NULL, NULL, OZ_IO_ETHER_TRANSMIT, sizeof ether_transmit, &ether_transmit);
  if (sts != OZ_STARTED) writeast (buf, sts);
}

/* This ast routine is called when the arp or ip packet has been sent */
/* It queues the buf to the kthentry routine for processing           */

static void writeast (void *bufv, uLong status)

{
  Buf *buf;
  uLong vl;

  buf = bufv;

  if ((status != OZ_SUCCESS) && !(buf -> hw -> terminated)) {
    oz_knl_printk ("oz_dev_ip writeast: error %u writing to %s\n", status, buf -> hw -> devname);
  }

  buf -> next  = NULL;
  buf -> iosts = status;
  vl = oz_hw_smplock_wait (&smplock_vl);
  *xmtqt = buf;
  xmtqt  = &(buf -> next);
  oz_hw_smplock_clr (&smplock_vl, vl);
  oz_knl_event_set (ipevent, 1);
}

/* Perform processing to be done when a transmit completes -       */
/* Also called for an ip transmit when the corresponding arp fails */

static void sendcomplete (Buf *buf)

{
  void (*donentry) (void *donparam, uLong status);

  /* If there is an associated donentry routine, call it */

  donentry = buf -> donentry;
  if (donentry != NULL) {
    buf -> donentry = NULL;
    (*donentry) (buf -> donparam, buf -> iosts);
  }

  /* Anyway, free off the buffer */

  freebuf (buf);
}

/************************************************************************/
/*									*/
/*  Get / Set ethernet protocol number in an ethernet buffer		*/
/*									*/
/************************************************************************/

static Eproto geteproto (Buf *buf)

{
  if (sizeof (Eproto) != 2) oz_crash ("oz_dev_ip geteproto: sizeof (Eproto) is %d, not 2", sizeof (Eproto));
  return (N2HW (buf -> cxb + buf -> hw -> ether_getinfo1.protooffs));
}

static void seteproto (Eproto eproto, Buf *buf)

{
  if (sizeof (Eproto) != 2) oz_crash ("oz_dev_ip seteproto: sizeof (Eproto) is %d, not 2", sizeof (Eproto));
  H2NW (eproto, buf -> cxb + buf -> hw -> ether_getinfo1.protooffs);
}

/************************************************************************/
/*									*/
/*  This routine is called when an arp relationship is detected		*/
/*									*/
/*    Input:								*/
/*									*/
/*	hw       = hardware device pointer				*/
/*	enaddr   = ethernet address					*/
/*	ipaddr   = ip address						*/
/*	arpwaits = queue of buf's waiting for arp			*/
/*	timeout  = abs time (in ticks) that it will time-out		*/
/*									*/
/*    Output:								*/
/*									*/
/*	arpc     = updated with new entry				*/
/*	arpwaits = possibly some entries removed and restarted		*/
/*									*/
/************************************************************************/

static void addtoarpc (Hw *hw, const uByte enaddr[OZ_IO_ETHER_MAXADDR], const uByte ipaddr[IPADDRSIZE], uLong timeout)

{
  Arpc *arpc;
  Buf *arpw, **larpw;
  uLong sts;
  void (*donentry) (void *donparam, uLong status);
  void *donparam;

  validatearpcs (hw, __LINE__);
  for (arpc = hw -> arpcs; arpc != NULL; arpc = arpc -> next) if (CEQIPAD (arpc -> ipaddr, ipaddr)) break;
  if (arpc == NULL) {
    arpc = mallocarpc ();
    CPYIPAD (arpc -> ipaddr, ipaddr);
    arpc -> next = hw -> arpcs;
    hw -> arpcs = arpc;
    if (debug & DEBUG_ARPCACHE) {
      PRINTF "new arpc %s  ", hw -> devname);
      printenaddr (hw, enaddr);
      PRINTF " <-> ");
      printipaddr (ipaddr);
      PRINTF "\n");
    }
    hw -> arpcount ++;
    validatearpcs (hw, __LINE__);
  }
  CPYENAD (arpc -> enaddr, enaddr, hw);
  arpc -> timeout = timeout;

  /* Re-start any transmits waiting for arp of that ip address */

  for (larpw = &(hw -> arpwaits); (arpw = *larpw) != NULL;) {
    if (CEQIPAD (arpw -> sendipaddr, ipaddr)) {
      *larpw = arpw -> next;
      arpw -> hw = hw;
      donentry = arpw -> donentry;
      donparam = arpw -> donparam;
      arpw -> donentry = NULL;
      arpw -> donparam = NULL;
      sendippkt (arpw, donentry, donparam);
    } else {
      larpw = &(arpw -> next);
    }
  }
}

/************************************************************************/
/*									*/
/*  See if a particular packet is allowed by a set of filters		*/
/*									*/
/*    Input:								*/
/*									*/
/*	filters = the filter listhead to check				*/
/*	ippkt   = ip packet to check					*/
/*									*/
/*    Output:								*/
/*									*/
/*	filter_check = 0 : denied					*/
/*	               1 : accepted					*/
/*									*/
/*    Note:								*/
/*									*/
/*	This routine is set up to accept the packet by default.  This 	*/
/*	is so the loader can use it without any problems.  To make 	*/
/*	deny the default, just define a filter at the bottom of the 	*/
/*	list to deny all before any daemons are started up.		*/
/*									*/
/************************************************************************/

static int filter_check (Filter **filters, Ippkt *ippkt)

{
  Filter *filter;
  Filtera *fa;
  int i, index;
  uLong *cd;

  index = 0;
  for (filter = *filters; filter != NULL; filter = filter -> next) {		// loop through list of filters
    cd  = (uLong *)ippkt;							// point to ip packet to be tested
    fa  = filter -> a;								// point to data/mask array
    i   = filter -> acount;							// get count of array elements
    cd += filter -> astart;							// skip this many longwords at the beginning
    while (((*cd ^ fa -> data) & fa -> mask) == 0) {				// check a longword of the packet
      if (-- i <= 0) goto matches;
      cd ++;									// increment on to next longword of packet
      fa ++;									// increment on to next filter array element
    }
    index ++;
  }
  return (filters != &forward_filters);						// by default, accept the packet

matches:
  if (filter -> action & OZ_IO_IP_FILTER_ACTION_TRACE) {
    i = ' ';
    if (filters == &forward_filters) i = 'f';
    if (filters == &input_filters)   i = 'i';
    if (filters == &output_filters)  i = 'o';
    switch (ippkt -> proto) {
      case PROTO_IP_ICMP: {
        PRINTF "%c[%d]: ICMP ", (char)i, index);
        printipaddr (ippkt -> srcipaddr);
        PRINTF " > ");
        printipaddr (ippkt -> dstipaddr);
        PRINTF " %u.%u %u\n", ippkt -> dat.icmp.type, ippkt -> dat.icmp.code, N2HW (ippkt -> totalen));
        break;
      }
      case PROTO_IP_TCP: {
        char flagstr[16];
        uLong begseq, endseq;
        uWord flags;

        PRINTF "%c[%d]: TCP ", (char)i, index);
        printipaddr (ippkt -> srcipaddr);
        PRINTF ".%u > ", N2HW (ippkt -> dat.tcp.sport));
        printipaddr (ippkt -> dstipaddr);
        PRINTF ".%u %u ", N2HW (ippkt -> dat.tcp.dport), N2HW (ippkt -> totalen));

        i = 0;
        flags = N2HW (ippkt -> dat.tcp.flags);
        if (flags & TCP_FLAGS_FIN) flagstr[i++] = 'F';
        if (flags & TCP_FLAGS_SYN) flagstr[i++] = 'S';
        if (flags & TCP_FLAGS_RST) flagstr[i++] = 'R';
        if (flags & TCP_FLAGS_ACK) {
          flagstr[i++] = 'A';
          flagstr[i++] = ':';
          oz_hw_ztoa (N2HL (ippkt -> dat.tcp.ack), 9, flagstr + i);
          i += strlen (flagstr + i);
        }
        if (i == 0) flagstr[i++] = '-';
        flagstr[i] = 0;
        begseq = tcpbegseq (ippkt);
        endseq = tcpendseq (ippkt);
        PRINTF " %s %x-%x(%u)\n", flagstr, begseq, endseq, endseq - begseq);
        break;
      }
      case PROTO_IP_UDP: {
        PRINTF "%c[%d]: UDP ", (char)i, index);
        printipaddr (ippkt -> srcipaddr);
        PRINTF ".%u > ", N2HW (ippkt -> dat.udp.sport));
        printipaddr (ippkt -> dstipaddr);
        PRINTF ".%u %u %u\n", N2HW (ippkt -> dat.udp.dport), N2HW (ippkt -> totalen), N2HW (ippkt -> dat.udp.length));
        break;
      }
      default: {
        PRINTF "%c[%d]: %u ", (char)i, index, ippkt -> proto);
        printipaddr (ippkt -> srcipaddr);
        PRINTF " > ");
        printipaddr (ippkt -> dstipaddr);
        PRINTF " %u\n", N2HW (ippkt -> totalen));
        break;
      }
    }
  }
  return (filter -> action & OZ_IO_IP_FILTER_ACTION_ACCEPT);
}

/************************************************************************/
/*									*/
/*  Print out a summary of a tcp packet					*/
/*									*/
/************************************************************************/

static void tcpsum (char rt, Ippkt *ippkt)

{
  Byte c, datstr[17], flgstr[21];
  uByte *datpnt;
  int i;
  uLong ack, totalen;
  uWord flags, datlen;

  if ((debug & DEBUG_TCPSUM) && (ippkt -> proto == PROTO_IP_TCP)) {
    flags = N2HW (ippkt -> dat.tcp.flags);
    memset (flgstr, ' ', 20);
    if (flags & TCP_FLAGS_FIN) memcpy (flgstr + 0, "FIN", 3);
    if (flags & TCP_FLAGS_SYN) memcpy (flgstr + 4, "SYN", 3);
    if (flags & TCP_FLAGS_RST) memcpy (flgstr + 8, "RST", 3);
    if (flags & TCP_FLAGS_ACK) {
      ack = N2HL (ippkt -> dat.tcp.ack);
      memcpy (flgstr + 12, "ACK-", 4);
      flgstr[16] = hextab[(ack>>12)&15];
      flgstr[17] = hextab[(ack>> 8)&15];
      flgstr[18] = hextab[(ack>> 4)&15];
      flgstr[19] = hextab[(ack    )&15];
    }
    flgstr[20] = 0;
    totalen = N2HW (ippkt -> totalen);
    datpnt  = ippkt -> dat.raw;
    datpnt += (flags & TCP_FLAGS_HLNMSK) >> (TCP_FLAGS_HLNSHF - 2);
    datlen  = (uByte *)ippkt + totalen - datpnt;
    for (i = 0; (i < datlen) && (i < 16); i ++) {
      c = datpnt[i] & 127;
      if ((c == 127) || (c < ' ')) c = '.';
      datstr[i] = c;
    }
    datstr[i] = 0;
    PRINTF "tcp %c .%u,%u -> .%u,%u  %s  %4.4x-%4.4x ws%4.4x [%u] %s\n", 
                                     rt, ippkt -> srcipaddr[IPADDRSIZE-1], 
                                            N2HW (ippkt -> dat.tcp.sport), 
                                         ippkt -> dstipaddr[IPADDRSIZE-1], 
                                            N2HW (ippkt -> dat.tcp.dport), 
                             flgstr, tcpbegseq (ippkt), tcpendseq (ippkt), 
                            N2HW (ippkt -> dat.tcp.wsize), datlen, datstr);
    if (flags & (TCP_FLAGS_SYN | TCP_FLAGS_FIN | TCP_FLAGS_RST)) {
      for (i = 0; i < totalen; i += 20) {
        PRINTF "  %8.8x %8.8x %8.8x %8.8x %8.8x   %4.4x\n", 
                                             *(uLong *) ((uByte *)ippkt + i + 16), 
             *(uLong *) ((uByte *)ippkt + i + 12), *(uLong *) ((uByte *)ippkt + i +  8), 
             *(uLong *) ((uByte *)ippkt + i +  4), *(uLong *) ((uByte *)ippkt + i +  0), i);
      }
    }
  }
}

/************************************************************************/
/*									*/
/*  This routine dumps out the state of everything			*/
/*									*/
/************************************************************************/

#if 00
static void dumpbufs (int indent, Buf *bufs);
static void dumpbuf (int indent, Buf *bufs);

void oz_dev_ip_dumpstate ()

{
  Arpc *arpc;
  Byte timstr[32], *tltype;
  Buf *buf;
  Hw *hw;
  int i, j, k;
  Long seqdiff;
  Tcpcon *tcpcon;
  Tcplis *tcplis;
  uWord flags;

  PRINTF "\n\nBegin State Dump\n\n");

  PRINTF "debug         0x%x\n", debug);
  PRINTF "ticks         %u\n",   ticks);
  PRINTF "ident         %u\n",   ident);
  PRINTF "udp_ephemnext %d\n\n", udp_ephemnext);
  PRINTF "tcp_ephemnext %d\n\n", tcp_ephemnext);

  PRINTF "\narpcs:\n");
  for (hw = hws; hw != NULL; hw = hw -> next) {
    PRINTF "  %s:\n", hw -> devname);
    for (arpc = hw -> arpcs; arpc != NULL; arpc = arpc -> next) {
      PRINTF "    timeout %u  enaddr ", arpc -> timeout);
      printenaddr (arpc -> enaddr);
      PRINTF "  ipaddr ");
      printipaddr (arpc -> ipaddr);
      PRINTF "\n");
    }
  }

  PRINTF "\narpwaits:\n");
  dumpbufs (0, arpwaits);

  PRINTF "\nfrags:\n");
  dumpbufs (0, frags);

  PRINTF "\ntcplisqh:\n");
  for (tcplis = tcplisqh; tcplis != NULL; tcplis = tcplis -> next) {
    PRINTF "%6d\n", tcplis -> dport);
  }

  PRINTF "\ntcpcons:\n");
  for (tcpcon = tcpcons; tcpcon != NULL; tcpcon = tcpcon -> next) {
    PRINTF "  tcpcon %x\n", tcpcon);
    PRINTF "         localaddress "); printipaddr (tcpcon -> lipaddr);
    PRINTF ",%u\n", tcpcon -> lsockno);
    PRINTF "       remote address "); printipaddr (tcpcon -> ripaddr);
    PRINTF ",%u\n", tcpcon -> rsockno);
    PRINTF "      lasttimemsgrcvd %u\n", tcpcon -> lasttimemsgrcvd);
    PRINTF "  lasttimenzwsizercvd %u\n", tcpcon -> lasttimenzwsizercvd);
    PRINTF "           windowsize %d\n", tcpwindowsize (tcpcon));
    PRINTF "        lastwsizesent %u\n", tcpcon -> lastwsizesent);
    PRINTF "          maxsendsize %u\n", tcpcon -> maxsendsize);
    PRINTF "     congestionwindow %u\n", tcpcon -> congestionwindow);
    PRINTF "       slowstarthresh %u\n", tcpcon -> slowstarthresh);
    PRINTF "             rcvchnex 0x%x\n", tcpcon -> rcvchnex);
    PRINTF "      seq_lastrcvdack 0x%x\n", tcpcon -> seq_lastrcvdack);
    PRINTF "  seq_nextusersendata 0x%x\n", tcpcon -> seq_nextusersendata);
    PRINTF "      seq_lastacksent 0x%x\n", tcpcon -> seq_lastacksent);
    PRINTF "       seq_receivedok 0x%x\n", tcpcon -> seq_receivedok);
    PRINTF "    seq_lastrcvdwsize 0x%x\n", tcpcon -> seq_lastrcvdwsize);
    seqdiff = tcpcon -> seq_receivedok - tcpcon -> seq_lastacksent;
    if (seqdiff != 0) PRINTF "     (have yet to ack %d)\n", seqdiff);
    PRINTF "    smoothedroundtrip %u\n", tcpcon -> smoothedroundtrip);
    PRINTF "   roundtripdeviation %u\n", tcpcon -> roundtripdeviation);
    PRINTF "   retransmitinterval %u\n", tcpcon -> retransmitinterval);
    PRINTF "              rcvdsyn %d\n", tcpcon -> rcvdsyn);
    PRINTF "              sentfin %d\n", tcpcon -> sentfin);
    PRINTF "              rcvdfin %d\n", tcpcon -> rcvdfin);
    PRINTF "                reset %u\n", tcpcon -> reset);
    PRINTF "       expressreceive %u\n", tcpcon -> expressreceive);
    PRINTF "        normalreceive %u\n", tcpcon -> normalreceive);

    PRINTF "  sentbutnotacked:\n");
    dumpbufs (4, tcpcon -> sentbutnotacked);

    PRINTF "  waitingforwsize:\n");
    dumpbufs (4, tcpcon -> waitingforwsizeqh);

    PRINTF "  rcvblockedqh:\n");
    dumpbufs (4, tcpcon -> rcvblockedqh);

    PRINTF "  rcvdoutofordr:\n");
    dumpbufs (4, tcpcon -> rcvdoutofordr);
  }

  PRINTF "\nEnd State Dump\n");
}

static void dumpbufs (int indent, Buf *bufs)

{
  Buf *buf;

  for (buf = bufs; buf != NULL; buf = buf -> next) dumpbuf (indent, buf);
}

static void dumpbuf (int indent, Buf *buf)

{
  Arppkt *arppkt;
  Buf *frag;
  char indents[256];
  Cxb *cxb;
  Hw *hw;
  uWord cksm;

  cxb = buf -> cxb;
  hw  = buf -> hw;

  memset (indents, ' ', indent);
  indents[indent] = 0;

  PRINTF "%sbuf %x:\n", indents, buf);

  PRINTF   "%s          timeout %u\n",     indents, buf -> timeout);
  PRINTF   "%s   tcpretransmits %u\n",     indents, buf -> tcpretransmits);
  PRINTF   "%s        tcprawlen %u\n",     indents, buf -> tcprawlen);
  PRINTF   "%s        tcprawpnt 0x%x\n",   indents, buf -> tcprawpnt);
  PRINTF   "%s               hw %s\n\n",   indents, hw -> devname);
  PRINTF   "%s        dstaddr ", indents);        printenaddr (hw, cxb + hw -> ether_getinfo1.dstaddrof);
  PRINTF "\n%s           .saddr ", indents);        printenaddr (hw, cxb + hw -> ether_getinfo1.srcaddrof);
  PRINTF "\n%s           .proto %u\n",     indents, geteproto (buf));
  PRINTF "\n");

  if (proto == PROTO_ARP) {
    arppkt = ARPPKT (hw, cxb);
    PRINTF   "%s     arp.hardtype %u\n",   indents, N2HW (arppkt -> hardtype));
    PRINTF   "%s        .prottype 0x%x\n", indents, N2HW (arppkt -> prottype));
    PRINTF   "%s        .hardsize %u\n",   indents, arppkt -> hardsize);
    PRINTF   "%s        .protsize %u\n",   indents, arppkt -> protsize);
    PRINTF   "%s              .op %u\n",   indents, N2HW (arppkt -> op));
    PRINTF   "%s         .senaddr ", indents);      printenaddr (arppkt -> senaddr);
    PRINTF "\n%s         .sipaddr ", indents);      printipaddr (arppkt -> sipaddr);
    PRINTF "\n%s         .tenaddr ", indents);      printenaddr (arppkt -> tenaddr);
    PRINTF "\n%s         .tipaddr ", indents);      printipaddr (arppkt -> tipaddr);
    PRINTF "\n\n");
  }

  else if (proto == PROTO_IP) {
    printippkt (indents, IPPKT (hw, cxb));
  }
}
#endif

/************************************************************************/
/*									*/
/*  Print out an IP packet						*/
/*									*/
/*    Input:								*/
/*									*/
/*	indents = a string of spaces to put at beg of each line		*/
/*	ip      = points to ip packet					*/
/*									*/
/************************************************************************/

static void printippkt (char *indents, Ippkt *ip)

{
  uWord cksm, proto;

  PRINTF   "%s     ip.hdrlenver 0x%x\n", indents, ip -> hdrlenver);
  PRINTF   "%s      .typeofserv 0x%x\n", indents, ip -> typeofserv);
  PRINTF   "%s         .totalen %u\n",   indents, N2HW (ip -> totalen));
  PRINTF   "%s           .ident %u\n",   indents, N2HW (ip -> ident));
  PRINTF   "%s           .flags 0x%x\n", indents, N2HW (ip -> flags));
  PRINTF   "%s             .ttl %u\n",   indents, ip -> ttl);
  proto = ip -> proto;
  PRINTF   "%s           .proto %u\n",   indents, proto);
  PRINTF   "%s         .hdrcksm 0x%x\n", indents, N2HW (ip -> hdrcksm));
  PRINTF   "%s       .srcipaddr ", indents);      printipaddr (ip -> srcipaddr);
  PRINTF "\n%s       .dstipaddr ", indents);      printipaddr (ip -> dstipaddr);
  PRINTF "\n");
  cksm = oz_dev_ip_ipcksm (ip);
  if (cksm != 0xFFFF) PRINTF "%s -- bad checksum (0x%x)\n", indents, cksm);
  PRINTF "\n");

  if (proto == PROTO_IP_ICMP) {
    PRINTF   "%s        icmp.type %u\n",   indents, ip -> dat.icmp.type);
    PRINTF   "%s            .code %u\n",   indents, ip -> dat.icmp.code);
    PRINTF   "%s            .cksm 0x%x\n", indents, N2HW (ip -> dat.icmp.cksm));
    cksm = oz_dev_ip_icmpcksm (ip);
    if (cksm != 0xFFFF) PRINTF "%s -- bad checksum (0x%x)\n", indents, cksm);
    PRINTF "\n");
  }

  else if (proto == PROTO_IP_UDP) {
    PRINTF   "%s        udp.sport %u\n",   indents, N2HW (ip -> dat.udp.sport));
    PRINTF   "%s           .dport %u\n",   indents, N2HW (ip -> dat.udp.dport));
    PRINTF   "%s          .length %u\n",   indents, N2HW (ip -> dat.udp.length));
    PRINTF   "%s            .cksm 0x%x\n", indents, N2HW (ip -> dat.udp.cksm));
    cksm = oz_dev_ip_udpcksm (ip);
    if (cksm != 0xFFFF) PRINTF "%s -- bad checksum (0x%x)\n", indents, cksm);
    PRINTF "\n");
  }

  else if (proto == PROTO_IP_TCP) {
    PRINTF   "%s        tcp.sport %u\n",   indents, N2HW (ip -> dat.tcp.sport));
    PRINTF   "%s           .dport %u\n",   indents, N2HW (ip -> dat.tcp.dport));
    PRINTF   "%s             .seq 0x%x\n", indents, N2HL (ip -> dat.tcp.seq));
    PRINTF   "%s             .ack 0x%x\n", indents, N2HL (ip -> dat.tcp.ack));
    PRINTF   "%s           .flags 0x%x\n", indents, N2HW (ip -> dat.tcp.flags));
    PRINTF   "%s           .wsize %u\n",   indents, N2HW (ip -> dat.tcp.wsize));
    PRINTF   "%s            .cksm 0x%x\n", indents, N2HW (ip -> dat.tcp.cksm));
    PRINTF   "%s          .urgent %u\n",   indents, N2HW (ip -> dat.tcp.urgent));
    cksm = oz_dev_ip_tcpcksm (ip);
    if (cksm != 0xFFFF) PRINTF "%s -- bad checksum (0x%x)\n", indents, cksm);
    PRINTF "\n");
  }
}

/************************************************************************/
/*									*/
/*  Print tcp connection ident						*/
/*									8/
/************************************************************************/

static void printtcpcon (Tcpcon *tcpcon)

{
  printipaddr (tcpcon -> lipaddr);
  PRINTF ",%u ", tcpcon -> lsockno);
  printipaddr (tcpcon -> ripaddr);
  PRINTF ",%u", tcpcon -> rsockno);
  if (tcpcon -> timeout == (uLong)(-1)) PRINTF ", no timeout");
  else PRINTF ", timeout %u, now %u", tcpcon -> timeout, ticks);
}

/************************************************************************/
/*									*/
/*  Print TCP flags word						*/
/*									*/
/************************************************************************/

static void printtcpflags (uWord flags)

{
  PRINTF "<");
  if (flags & TCP_FLAGS_FIN) PRINTF "F");
  if (flags & TCP_FLAGS_SYN) PRINTF "S");
  if (flags & TCP_FLAGS_RST) PRINTF "R");
  if (flags & TCP_FLAGS_PSH) PRINTF "P");
  if (flags & TCP_FLAGS_ACK) PRINTF "A");
  PRINTF ">");
}

/************************************************************************/
/*									*/
/*  Print an ethernet address						*/
/*									*/
/************************************************************************/

static void printenaddr (Hw *hw, const uByte enaddr[OZ_IO_ETHER_MAXADDR])

{
  int i;

  PRINTF "%2.2x", enaddr[0]);
  for (i = 1; i < hw -> ether_getinfo1.addrsize; i ++) PRINTF "-%2.2x", enaddr[i]);
}

/************************************************************************/
/*									*/
/*  Print an ip address							*/
/*									*/
/************************************************************************/

static void printipaddr (const uByte ipaddr[IPADDRSIZE])

{
  int i;

  PRINTF "%u", ipaddr[0]);
  for (i = 1; i < IPADDRSIZE; i ++) PRINTF ".%u", ipaddr[i]);
}

/************************************************************************/
/*									*/
/*  Internal malloc routines						*/
/*									*/
/************************************************************************/

/* Allocate an arp cache buffer */

static Arpc *mallocarpc (void)

{
  Arpc *arpc;

  arpc = OZ_KNL_PGPMALLOC (sizeof *arpc);
  return (arpc);
}

/* Free arp cache buffer */

static void freearpc (Arpc *arpc)

{
  OZ_KNL_PGPFREE (arpc);
}

/* Allocate a TCP connection block and clear it to zeroes.  Also set up the receive window buffer. */

static uLong calloctcpcon (Iopex *iopex, uLong windowsize, uByte *windowbuff, Tcpcon **tcpcon_r)

{
  OZ_Process *windowprocess;
  OZ_Seclock *windowlock;
  Tcpcon *tcpcon;
  uLong sts;

  windowprocess = NULL;
  if (windowbuff == NULL) {					/* see if user gave us a buffer to use */
    windowlock = NULL;						/* if not, nothing to lock in memory */
    if (windowsize == 0) windowsize = DEFWINSIZE;		/* maybe we need to use the default window size */
    else if (windowsize < MINWINSIZE) windowsize = MINWINSIZE;	/* but always at least use the minimum */
    tcpcon = OZ_KNL_PGPMALLOQ (sizeof *tcpcon + windowsize);	/* allocate it along with the connection block */
    if (tcpcon == NULL) return (OZ_EXQUOTAPGP);			/* ran out of quota, probably should have given us a windowbuff */
    windowbuff = ((uByte *)tcpcon) + sizeof *tcpcon;		/* made it, windowbuff follows the tcpcon struct */
  } else {
    if (windowsize < MINWINSIZE) return (OZ_BADPARAM);		/* it must have some useful size */
    sts = oz_knl_section_iolockw (iopex -> procmode, windowsize, windowbuff, &windowlock, NULL, NULL, NULL); /* lock it in memory */
    if (sts != OZ_SUCCESS) return (sts);			/* its address is bad or something like that */
    tcpcon = OZ_KNL_PGPMALLOQ (sizeof *tcpcon);			/* allocate an tcpcon struct */
    if (tcpcon == NULL) {
      oz_knl_section_iounlk (windowlock);			/* ran out of quota, unlock window buffer */
      return (OZ_EXQUOTAPGP);					/* return error status */
    }
    windowprocess = oz_knl_ioop_getprocess (iopex -> ioop);	/* i/o process is how we access the window */
    oz_knl_process_increfc (windowprocess, 1);
  }
  memclr (tcpcon, sizeof *tcpcon);				/* either way, clear the tcpcon struct */
  tcpcon -> windowsize    = windowsize;				/* save the base window size */
  tcpcon -> windowbuff    = windowbuff;				/* save the base address of the window buffer */
  tcpcon -> windowlock    = windowlock;				/* save how to unlock it */
  tcpcon -> windowprocess = windowprocess;			/* this is the process that owns the buffer */
  *tcpcon_r = tcpcon;						/* return pointer to tcpcon block */
  return (OZ_SUCCESS);						/* successful */
}

/* Wipe out and abort everything associated with the connection block and free it off */

static void tcpterminate (Tcpcon *tcpcon)

{
  Buf *buf;
  Chnex *chnex;
  Iopex *iopex;
  Sock ephem;
  Tcpcon **ltcpcon, *xtcpcon;
  uLong seq;

  if (debug & DEBUG_TCPTERM) PRINTF "tcpterm: tcpcon %p -> reset %u\n", tcpcon, tcpcon -> reset);

  tcpreceive_test (tcpcon);					/* try to complete any pending receives as normally as possible */
  if (tcpcon -> reset == 0) tcpcon -> reset = OZ_ABORTED;	/* remember we closed the connection */

  /* Send a reset packet so other end will know connection is closed */

  if (debug & DEBUG_TCPTERM) PRINTF "tcpterm: sentrst %u\n", tcpcon -> sentrst);
  if (!(tcpcon -> sentrst)) {
    buf = mallocsndkbuf (find_send_hw (tcpcon -> ripaddr, NULL, NULL, 1));
    if (buf != NULL) sendtcp (buf, tcpcon, 0, TCP_FLAGS_RST, tcpcon -> seq_receivedok);
  }

  /* Disassociate connection with I/O channel */

  chnex = tcpcon -> rcvchnex;
  if (debug & DEBUG_TCPTERM) PRINTF "tcpterm: chnex %p\n", chnex);
  if (chnex != NULL) {
    tcpcon -> rcvchnex = NULL;
    chnex  -> tcpcon   = NULL;
  }

  /* Clear out the transmit queue */

  validtcpxmtq (tcpcon);					/* make sure it's valid before we wipe it */
  if (debug & DEBUG_TCPTERM) PRINTF "tcpterm: nextactreq %p\n", tcpcon -> nextackreq);
  while ((iopex = tcpcon -> nextackreq) != NULL) {		/* loop until all buffers are gone */
    tcpcon -> nextackreq = iopex -> next;
    oz_knl_iodone (iopex -> ioop, tcpcon -> reset, NULL, NULL, NULL);
  }
  tcpcon -> lastxmtreq = &(tcpcon -> nextackreq);		/* queue is empty */
  tcpcon -> numxmtreqs = 0;
  tcpcon -> seq_lastrcvdack = tcpcon -> seq_nextusersendata;
  tcpcon -> nextxmtseq      = tcpcon -> seq_nextusersendata;

  /* Free off any received packets */

  if (debug & DEBUG_TCPTERM) PRINTF "tcpterm: rcvdoutofordr %p\n", tcpcon -> rcvdoutofordr);
  while ((buf = tcpcon -> rcvdoutofordr) != NULL) {
    tcpcon -> rcvdoutofordr = buf -> next;
    freebuf (buf);
  }

  /* Abort any listen in progress */

  iopex = tcpcon -> lisiopex;
  if (debug & DEBUG_TCPTERM) PRINTF "tcpterm: lisiopex %p\n", iopex);
  if (iopex != NULL) {
    *(iopex -> u.tcplisten.prev) = iopex -> next;			/* remove from tcplisqh/t list */
    if (iopex -> next != NULL) iopex -> next -> u.tcplisten.prev = iopex -> u.tcplisten.prev;
    if (tcplisqt == &(iopex -> next)) tcplisqt = iopex -> u.tcplisten.prev;
    tcpcon -> lisiopex = NULL;
    oz_knl_iodone (iopex -> ioop, OZ_ABORTED, NULL, NULL, NULL);	/* abort the I/O request */
  }

  /* Terminate any pending receives */

  if (debug & DEBUG_TCPTERM) PRINTF "tcpterm: tcprcv_iopqh %p\n", tcpcon -> tcprcv_iopqh);
  while ((iopex = tcpcon -> tcprcv_iopqh) != NULL) {			/* get all the I/O requests in there */
    tcpcon -> tcprcv_iopqh = iopex -> next;				/* unlink it */
    oz_knl_iodone (iopex -> ioop, tcpcon -> reset, NULL, NULL, NULL); /* abort each one */
  }
  tcpcon -> tcprcv_iopqt = &(tcpcon -> tcprcv_iopqh);			/* queue is now empty */

  /* Maybe free off ephemeral socket */

  ephem = tcpcon -> lsockno;
  if (debug & DEBUG_TCPTERM) PRINTF "tcpterm: ephem %u\n", ephem);
  if ((ephem >= EPHEMMIN) && (ephem < EPHEMMAX)) {
    seq = tcpcon -> seq_nextusersendata;
    do ++ seq;
    while (seq == 0);
    tcp_ephemsocks[ephem-EPHEMMIN] = seq;
  }
  tcpcon -> lsockno = 0;

  /* Unlock the user supplied window buffer and the process that owns it */

  if (debug & DEBUG_TCPTERM) PRINTF "tcpterm: windowlock %p, windowprocess %p\n", tcpcon -> windowlock, tcpcon -> windowprocess);
  if (tcpcon -> windowlock != NULL) {			/* see if user provided the buffer */
    oz_knl_section_iounlk (tcpcon -> windowlock);	/* if so, unlock it */
    tcpcon -> windowsize = 0;				/* forget all about it */
    tcpcon -> windowbuff = NULL;
    tcpcon -> windowlock = NULL;
  }
  if (tcpcon -> windowprocess != NULL) {
    oz_knl_process_increfc (tcpcon -> windowprocess, -1);
    tcpcon -> windowprocess = NULL;
  }
  if ((tcpcon -> windowbuff != NULL) & (tcpcon -> windowbuff != ((uByte *)tcpcon) + sizeof *tcpcon)) {
    OZ_KNL_PGPFREE (tcpcon -> windowbuff);
    tcpcon -> windowbuff = NULL;
  }

  /* Unlink and free off the struct.  But if some sends are still going on, come back when they finish. */

  if (debug & DEBUG_TCPTERM) PRINTF "tcpterm: sendpend %u\n", tcpcon -> sendpend);
  for (ltcpcon = &tcpcons; (xtcpcon = *ltcpcon) != NULL; ltcpcon = &(xtcpcon -> next)) {
    if (xtcpcon == tcpcon) {
      *ltcpcon = xtcpcon -> next;
      ntcpcons --;
      break;
    }
  }
  if (tcpcon -> sendpend == 0) OZ_KNL_PGPFREE (tcpcon);
}

/* Allocate a user send buffer - there are a fixed number of these per hardware interface */
/* Returns NULL if there are none available now then calls entry routine when one is free */
/* Otherwise, returns pointer to allocated buffer                                         */
/* Note that the cxb part is really one of the ethernet driver's internal buffers         */

static Buf *mallocsndubuf (Hw *hw, void (*entry) (Buf *buf, void *param), void *param, Msubp *msubp)

{
  Buf *buf;
  Msubp *msubp_next, **msubp_prev;

  valsndubufs (hw, __LINE__);
  buf = hw -> freesndubufs;				/* check for a free user send buffer */
  if (buf != NULL) {
    hw -> freesndubufs = buf -> next;			/* got one, just unlink it as is */
    hw -> freesndubufc --;
    valsndubufs (hw, __LINE__);
    msubp_prev = msubp -> prev;
    if (msubp_prev != NULL) {				/* remove it from the wait queue */
      if (*msubp_prev != msubp) oz_crash ("oz_dev_ip %d: *(msubp %p -> prev %p) = %p", msubp, msubp_prev, *msubp_prev);
      msubp_next  = msubp -> next;
      *msubp_prev = msubp_next;
      if (msubp_next == NULL) hw -> waitsndubuft = msubp_prev;
      else msubp_next -> prev = msubp_prev;
      msubp -> prev = NULL;
      msubp -> next = msubp;
      hw -> waitsndubufc --;
      valsndubufs (hw, __LINE__);
    }
    buf -> donentry = NULL;				/* no associated donentry, yet */
  } else if (msubp -> prev == NULL) {			/* nothing there, put it on the wait queue */
    *(hw -> waitsndubuft) = msubp;
    msubp -> prev  = hw -> waitsndubuft;
    msubp -> next  = NULL;
    msubp -> entry = entry;
    msubp -> param = param;
    hw -> waitsndubuft = &(msubp -> next);
    hw -> waitsndubufc ++;
    valsndubufs (hw, __LINE__);
  }
  return (buf);						/* return pointer, NULL or otherwise */
}

/* Allocate an internal send buffer - there is an unlimited supply of these.  They are just malloc'd from general pool as they */
/* are just used for short control-type messages, so the memcpy is probably faster than doing an ETHER_TRANSMITALLOC call.     */

static Buf *mallocsndkbuf (Hw *hw)

{
  Buf *buf;

  buf = NULL;
  if (hw != NULL) {
    buf = OZ_KNL_PGPMALLOC (sizeof *buf + hw -> ether_getinfo1.buffsize); /* malloc buf followed by the cxb */
    memset (buf, 0, sizeof *buf);				/* clear out the 'buf' portion */
								/* specifically we need rcvdrv, xmtdrv and donentry cleared */
    buf -> type = BUF_TYPE_SNDK;				/* set the type so it will get freed properly */
    buf -> cxb  = (Cxb *)(buf + 1);				/* point to the ethernet message portion */
    buf -> hw   = hw;						/* this is the hardware it will get sent on */
								/* ... so stuff will know at what offset the arp/ip packet begins */
  }
  return (buf);							/* return pointer to buf */
}

/* Free off just the cxb of a buffer */

static void freecxb (Buf *oldbuf)

{
  Buf *newbuf;

  if (oldbuf -> donentry != NULL) oz_crash ("oz_dev_ip freecxb: buf has non-null donentry %p", oldbuf -> donentry);

  switch (oldbuf -> type) {

    /* For these types, copy the buf part and free it normally     */
    /* These have cxb's which are internal ethernet driver buffers */

    case BUF_TYPE_RCV_ARP:
    case BUF_TYPE_RCV_IP:
    case BUF_TYPE_SNDU: {
      newbuf  = OZ_KNL_PGPMALLOC (sizeof *newbuf);
      *newbuf = *oldbuf;
      freebuf (newbuf);
      break;
    }

    /* If freeing an extended IP receive buffer, just free the cxb part */
    /* It is separate from the buf but we malloc'd it, it is not an ethernet driver buffer */

    case BUF_TYPE_RCV_IPX: {
      OZ_KNL_PGPFREE (oldbuf -> cxb);
      break;
    }

    /* Do nothing for SNDK's, the cxb is actually part of the buf */

    case BUF_TYPE_SNDK: {
      break;
    }

    /* Bad buffer type, crash */

    default: {
      oz_crash ("oz_dev_ip freecxb: bad buffer type %d", oldbuf -> type);
    }
  }

  /* Anyhoo, clear stuff in buf that is no longer valid */

  oldbuf -> cxb    = NULL;
  oldbuf -> xmtdrv = NULL;
  oldbuf -> rcvdrv = NULL;
}

/* Free a buffer of any type */

static void freebuf (Buf *buf)

{
  Hw *hw;
  Msubp *msubp;

  if (buf -> donentry != NULL) oz_crash ("oz_dev_ip freebuf: buf has non-null donentry %p", buf -> donentry);

  /* If cxb was already freed off via freecxb, just free the buf back to general pool */

  if (buf -> cxb == NULL) {
    OZ_KNL_PGPFREE (buf);
    return;
  }

  /* Otherwise, maybe recycle the cxb (it might be an internal ethernet driver buffer) */

  switch (buf -> type) {

    /* If freeing a normal receive buffer, just start another read going on it   */
    /* Note that the cxb portion is really the ethernet driver's internal buffer */

    case BUF_TYPE_RCV_ARP:
    case BUF_TYPE_RCV_IP: {
      startread (buf);
      break;
    }

    /* If freeing an extended IP receive buffer or an internal send buffer, free them off to general pool */

    case BUF_TYPE_RCV_IPX: {
      OZ_KNL_PGPFREE (buf -> cxb);
    }
    case BUF_TYPE_SNDK: {
      OZ_KNL_PGPFREE (buf);
      break;
    }

    /* If freeing off an user send buffer, put it on hardware's list of free buffers  */
    /* Note that the cxb part is internal to the ethernet driver, so retain for reuse */

    case BUF_TYPE_SNDU: {
      hw = buf -> hw;				/* point to hw interface */
      valsndubufs (hw, __LINE__);
      buf -> next = hw -> freesndubufs;		/* buffer on free user send buffer list */
      hw -> freesndubufs = buf;
      hw -> freesndubufc ++;
      valsndubufs (hw, __LINE__);
      if (hw -> waitsndubufh != NULL) {		/* if something waiting for buffer, wake main loop to get it. */
        scanmsubps = 1;				/* theoretically, we could call msubp->entry here, but things */
        oz_knl_event_set (ipevent, 1);		/* can nest deeply under heavy load and it puques the stack.  */
      }
      break;
    }

    /* Bad buffer type, crash */

    default: {
      oz_crash ("oz_dev_ip freebuf: bad buffer type %d", buf -> type);
    }
  }
}

/* Validate buffers on freesndubufs and waitsndubufh lists */

static void valsndubufs (Hw *hw, int line)

{
  Buf *buf;
  int count;
  Msubp **lmsubp, *msubp;

  buf = hw -> freesndubufs;
  for (count = 0; count < hw -> freesndubufc; count ++) {
    if (buf == NULL) oz_crash ("oz_dev_ip valsndubufs %d: count %d, freesndubufc %d", line, count, hw -> freesndubufc);
    if (buf -> type != BUF_TYPE_SNDU) oz_crash ("oz_dev_ip valsndubufs %d: type %d at %d", line, buf -> type, line);
    buf = buf -> next;
  }
  if (buf != NULL) oz_crash ("oz_dev_ip valsndubufs %d: count %d, buf %p", line, count, buf);

  msubp = *(lmsubp = &(hw -> waitsndubufh));
  for (count = 0; count < hw -> waitsndubufc; count ++) {
    if (msubp == NULL) oz_crash ("oz_dev_ip valsndubufs %d: count %d, waitsndubufc %d", line, count, hw -> waitsndubufc);
    if (msubp -> prev != lmsubp) oz_crash ("oz_dev_ip valsndubufs %d: msubp -> prev %p, lmsubp %p", line, msubp -> prev, lmsubp);
    msubp = *(lmsubp = &(msubp -> next));
  }
  if (msubp != NULL) oz_crash ("oz_dev_ip valsndubufs %d: count %d, msubp %p", line, count, msubp);
  if (hw -> waitsndubuft != lmsubp) oz_crash ("oz_dev_ip valsndubufs %d: lmsubp %p, waitsndubuft %p", line, lmsubp, hw -> waitsndubuft);
}

/************************************************************************/
/*									*/
/*  Simulate a transmit/receive failure rate of 0-15%			*/
/*									*/
/*    Input:								*/
/*									*/
/*	debug & DEBUG_FAILRATE = 0..15					*/
/*									*/
/*    Output:								*/
/*									*/
/*	debugfailrate = 0 : don't fail this packet			*/
/*	                1 : fail this packet				*/
/*									*/
/************************************************************************/

static int debugfailrate (void)

{
  uLong rand, rate;

  rate = debug & DEBUG_FAILRATE;	// see what rate is currently enabled
  if (rate == 0) return (0);		// if none, just return a zero always
  rand = oz_hw_tod_getnow ();		// otherwise, get a somewhat random number
  return ((rand % 100) < rate);		// return whether or not we should fail the packet
}

/************************************************************************/
/*									*/
/*  IP checksum routines						*/
/*									*/
/*  These routines are global so they can be called by anybody (even 	*/
/*  from user mode)							*/
/*									*/
/************************************************************************/

/* Generate an ip header checksum */

uWord oz_dev_ip_ipcksm (const Ippkt *ip)

{
  return (oz_dev_ip_gencksm ((ip -> hdrlenver & 15) * 2, ip, 0xffff));
}

/* Generate an icmp message checksum */

uWord oz_dev_ip_icmpcksm (Ippkt *ip)

{
  uLong icmplen;

  icmplen  = N2HW (ip -> totalen);
  icmplen -= ip -> dat.raw - ((uByte *)ip);
  if (icmplen & 1) ip -> dat.raw[icmplen++] = 0;

  return (oz_dev_ip_gencksm (icmplen / 2, ip -> dat.raw, 0xffff));
}

/* Generate an udp message checksum */

uWord oz_dev_ip_udpcksm (Ippkt *ip)

{
  uLong cksm, udplen;
  struct { uByte srcipaddr[IPADDRSIZE];
           uByte dstipaddr[IPADDRSIZE];
           uByte zero;
           uByte proto;
           uByte udplen[2];
         } pseudo;

  CPYIPAD (pseudo.srcipaddr, ip -> srcipaddr);
  CPYIPAD (pseudo.dstipaddr, ip -> dstipaddr);
  pseudo.zero  = 0;
  pseudo.proto = PROTO_IP_UDP;
  pseudo.udplen[0] = ip -> dat.udp.length[0];
  pseudo.udplen[1] = ip -> dat.udp.length[1];

  udplen = N2HW (pseudo.udplen);
  if (udplen & 1) ip -> dat.raw[udplen++] = 0;

  cksm = oz_dev_ip_gencksm (sizeof pseudo / 2, &pseudo, 0xffff);
  cksm = oz_dev_ip_gencksm (udplen / 2, ip -> dat.raw, cksm);

  return (cksm);
}

/* Generate a tcp message checksum */

uWord oz_dev_ip_tcpcksm (Ippkt *ip)

{
  uLong cksm, tcplen;
  struct { uByte srcipaddr[IPADDRSIZE];
           uByte dstipaddr[IPADDRSIZE];
           uByte zero;
           uByte proto;
           uByte tcplen[2];
         } pseudo;

  tcplen = ((uByte *)ip) + N2HW (ip -> totalen) - ip -> dat.raw;

  CPYIPAD (pseudo.srcipaddr, ip -> srcipaddr);
  CPYIPAD (pseudo.dstipaddr, ip -> dstipaddr);
  pseudo.zero  = 0;
  pseudo.proto = PROTO_IP_TCP;
  H2NW (tcplen, pseudo.tcplen);

  if (tcplen & 1) ip -> dat.raw[tcplen++] = 0;

  cksm = oz_dev_ip_gencksm (sizeof pseudo / 2, &pseudo, 0xffff);
  cksm = oz_dev_ip_gencksm (tcplen / 2, ip -> dat.raw, cksm);

  return (cksm);
}

static void validatearpcs (Hw *hw, int line)

{
  Arpc *arpc;
  int n;

  n = 0;
  for (arpc = hw -> arpcs; arpc != NULL; arpc = arpc -> next) n ++;
  if (n != hw -> arpcount) oz_crash ("oz_dev_ip validatearpcs %d: hw %p -> arpcount %d, n %d", line, hw, hw -> arpcount, n);
}
