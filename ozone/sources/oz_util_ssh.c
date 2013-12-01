//+++2002-08-17
//    Copyright (C) 2001,2002  Mike Rieker, Beverly, MA USA
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
//---2002-08-17

#define RECEIVEBUFFERSIZE 35000
#define TRANSMITBUFFERSIZE 35000
#define MAXPAYLOADSIZE 32768

typedef struct Sshctx {
	void *xmtctx;
	void *rcvctx;
	uLong (*transmit) (void *xmtctx, uLong size, const uByte *buff);
	uLong (*receive)  (void *rcvctx, uLong size, uByte *buff);
	void *cmpctx;
	void *expctx;
	uLong (*compress) (void *cmpctx, uLong explen, uByte *expbuf, uByte *cmpbuf);
	uLong (*expand)   (void *expctx, uLong explen, uByte *cmpbuf, uByte *expbuf);
	uLong xmt_macsize;
	uLong rcv_macsize;
	void *xmt_macctx;
	void *rcv_macctx;
	void (*xmt_genmac) (void *xmt_macctx, uLong datlen, uByte *datbuf, uByte *macbuf);
	void (*rcv_genmac) (void *rcv_macctx, uLong datlen, uByte *datbuf, uByte *macbuf);
	uLong xmt_cipherbs;
	uLong rcv_cipherbs;
	void *xmt_cipherctx;
	void *rcv_cipherctx;
	void (*encrypt) (void *xmt_cipherctx, uLong length, uByte *plaintext, uByte *ciphertext);
	void (*decrypt) (void *rcv_cipherctx, uLong length, uByte *ciphertext, uByte *plaintext);

	void (*random_fill) (uLong size, uByte *buff);

	uByte rcvbuf[RECEIVEBUFFERSIZE];
	uByte xmtbuf[TRANSMITBUFFERSIZE*2+4];
};

/************************************************************************/
/*									*/
/*  Null compression and encryption routines				*/
/*									*/
/************************************************************************/

static uLong compress_none (void *cmpctx, uLong explen, uByte *expbuf, uByte *cmpbuf)

{
  memcpy (cmpbuf, expbuf, explen);
  return (explen);
}

static uLong expand_none   (void *expctx, uLong cmplen, uByte *cmpbuf, uByte *expbuf)

{
  memcpy (expbuf, cmpbuf, cmplen);
  return (cmplen);
}

static void genmac_none (void *macctx, uLong datlen, uByte *datbuf, uByte *macbuf)

{ }

static void encrypt_none (void *cipherctx, uLong length, uByte *plaintext, uByte *ciphertext)

{
  memcpy (ciphertext, plaintext, length);
}

static void decrypt_none (void *cipherctx, uLong length, uByte *ciphertext, uByte *plaintext)

{
  memcpy (plaintext, ciphertext, length);
}


/************************************************************************/
/*									*/
/*  This routine is called to initialize a connection			*/
/*									*/
/*    Input:								*/
/*									*/
/*	sshctx -> xmtstart = routine to start transmitting		*/
/*	       -> xmtctx   = context to pass to xmtstart		*/
/*	       -> rcvstart = routine to start receiving			*/
/*	       -> rcvctx   = context to pass to rcvstart		*/
/*									*/
/************************************************************************/

uLong ssh_initialize (Sshctx *sshctx)

{
  int i;
  uLong sts;

  sshctx -> cmpctx        = NULL;
  sshctx -> expctx        = NULL;
  sshctx -> compress      = compress_none;
  sshctx -> expand        = expand_none;
  sshctx -> xmt_macsize   = 0;
  sshctx -> rcv_macsize   = 0;
  sshctx -> xmt_genmac    = genmac_none;
  sshctx -> rcv_genmac    = genmac_none;
  sshctx -> xmt_cipherbs  = CIPHERBS_NONE;
  sshctx -> rcv_cipherbs  = CIPHERBS_NONE;
  sshctx -> xmt_cipherctx = NULL;
  sshctx -> rcv_cipherctx = NULL;
  sshctx -> encrypt       = encrypt_none;
  sshctx -> decrypt       = decrypt_none;

  strcpy (sshctx -> xmtbuf, "SSH-2.0-ozone_v1.0\r\n");
  sts = (*(sshctx -> xmtstart)) (sshctx -> xmtctx, strlen (sshctx -> xmtbuf), sshctx -> xmtbuf, sshctx);
  if (sts == SSH_STARTED) sts = SSH_SUCCESS;
  else if (sts == SSH_SUCCESS) ssh_transmitted (sshctx, SSH_SUCCESS, strlen (sshctx -> xmtbuf), sshctx -> xmtbuf);

  if (sts == SSH_SUCCESS) {
    sshctx -> rcvstate = RCVSTATE_INITIALIZE;
    sshctx -> rcvlen1  = 0;
    sshctx -> rcvbuf1  = NULL;
    sshctx -> rcvlen2  = 0;
    sshctx -> rcvbuf2  = NULL;
    (*(sshctx -> rcvstart)) (sshctx -> rcvctx, sshctx);
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  A transmission started by xmtstart has completed			*/
/*									*/
/************************************************************************/

void ssh_transmitted (Sshctx *sshctx, uLong status, uLong xmtlen, uByte *xmtbuf)

{
  ????
}

/************************************************************************/
/*									*/
/*  This routine is called back by rcvstart when the data is received	*/
/*									*/
/*    Input:								*/
/*									*/
/*	sshctx = connection context block pointer			*/
/*	status = I/O status of the receive operation			*/
/*	rcvlen = number of bytes received				*/
/*	rcvbuf = where the data was read into				*/
/*									*/
/************************************************************************/

uLong ssh_received (Sshctx *sshctx, uLong status, uLong rcvlen, uByte *rcvbuf)

{
  uByte paybuf[MAXPACKETSIZE];
  uLong paylen, sts;

  if (status != SSH_SUCCESS) {
    ??
  }

  /* Add what we got onto end of existing data */

  if (sshctx -> rcvlen2 != 0) {
    if (sshctx -> rcvbuf2 + sshctx -> rcvlen2 != rcvbuf) abort ();
    sshctx -> rcvlen2 += rcvlen;
  } else if (sschctx -> rcvlen1 == 0) {
    sshctx -> rcvlen1  = rcvlen;
    sshctx -> rcvbuf1  = rcvbuf;
  } else if (sshctx -> rcvbuf1 + sshctx -> rcvlen1 == rcvbuf) {
    sshctx -> rcvlen1 += rcvlen;
  } else {
    sshctx -> rcvlen2  = rcvlen;
    sshctx -> rcvbuf2  = rcvbuf;
  }

  /* We don't want anything removed yet */

  removed = 0;

  while (1) {
    switch (sshctx -> rcvstate) {

      /* Keep reading data until we have a string that begins with SSH- and ends with <LF> */

      case RCVSTATE_INITIALIZE: {

        /* Find an <LF> and get lengths of part1 and part2 that contain the <LF> */

        if (sshctx -> rcvlen2 != 0) {
          p = memchr (sshctx -> rcvbuf2, '\n', sshctx -> rcvlen2);
          if (p == NULL) return (removed);
          len1 = sshctx -> rcvlen1;
          len2 = p - sshctx -> rcvbuf2 + 1;
        } else {
          p = memchr (sshctx -> rcvbuf1, '\n', sshctx -> rcvlen1);
          if (p == NULL) return (removed);
          len1 = p - sshctx -> rcvbuf1 + 1;
          len2 = 0;
        }

        /* Let main program print out the text if it wants to */

        (*(sshctx -> identmsg)) (sshctx, len1, sshctx -> rcvbuf1, len2, sshctx -> rcvbuf2);

        /* See if the line begins with SSH- */

        foundit = 0;
        if (len1 >= 4) foundit = (memcmp (sshctx -> rcvbuf1, "SSH-", 4) == 0);
        else if (len1 + len2 >= 4) foundit = (memcmp (sshctx -> rcvbuf1, "SSH-", len1) == 0) 
                                          && (memcmp (sshctx -> rcvbuf2, "SSH-" + len1, 4 - len1) == 0);

        /* If so, it is the last of the ascii text lines */

        if (foundit) {

          /* We only accept the connection if it begins with SSH-2.0- */

          foundit = 0;
          if (len1 >= 8) foundit = (memcmp (sshctx -> rcvbuf1, "SSH-2.0-", 8) == 0);
          else if (len1 + len2 >= 8) foundit = (memcmp (sshctx -> rcvbuf1, "SSH-2.0-", len1) == 0) 
                                            && (memcmp (sshctx -> rcvbuf2, "SSH-2.0-" + len1, 8 - len1) == 0);
          if (!foundit) {
            ?? bad SSH protocol ??
          }

          ?? start sending key negotiation ??

          /* Next thing we receive should be a standard binary message header */

          sshctx -> rcvstate = RCVSTATE_HEADER;
        }

        /* In any case, we processed len1+len2 bytes of the input data */

        break;
      }

      /* We are receiving a standard binary message header */

      case RCVSTATE_HEADER: {

        /* Return if there aren't enough bytes yet */

        if (sshctx -> rcvlen1 + sshctx -> rcvlen2 < sshctx -> rcv_cipherbs) return (removed);

        /* Decipher header and get the total packet size (including header and mac) */

        if (sshctx -> rcvlen1 >= sshctx -> rcv_cipherbs) {
          sshctx -> rcvtotal = ssh_decodehdr (sshctx, sshctx -> rcvbuf1);
        } else {
          memcpy (paybuf, sshctx -> rcvbuf1, sshctx -> rcvlen1);
          memcpy (paybuf + sshctx -> rcvlen1, sshctx -> rcvbuf2, sshctx -> rcv_cipherbs - sshctx -> rcvlen1);
          sshctx -> rcvtotal = ssh_decodehdr (sshctx, paybuf);
          memcpy (sshctx -> rcvbuf1, paybuf, sshctx -> rcvlen1);
          memcpy (sshctx -> rcvbuf2, paybuf + sshctx -> rcvlen1, sshctx -> rcv_cipherbs - sshctx -> rcvlen1);
        }

        /* Next we look for the rest of the packet */

        sshctx -> rcvstate = RCVSTATE_BODY;

        /* Fall through to RCVSTATE_BODY, without removing the header from the buffer (although it is decrypted) */
      }

      /* Receiving the rest of the packet */

      case RCVSTATE_BODY: {

        /* If we don't have it all yet, come back when we have more */

        if (sshctx -> rcvlen1 + sshctx -> rcvlen2 < sshctx -> rcvtotal) return (removed);

        /* Get lengths of part1 and part2 up to the required total length */
        /* This includes the decrypted header                             */

        len1 = sshctx -> rcvtotal;
        len2 = 0;
        if (sshctx -> rcvlen1 < sshctx -> rcvtotal) {
          len1 = sshctx -> rcvlen1;
          len2 = sshctx -> rcvtotal - sshctx -> rcvlen1;
        }

        /* Finish decrypting message, validate it and expand payload into paybuf */

        paylen = ssh_decodepkt (sshctx, len1, sshctx -> rcvbuf1, len2, sshctx -> rcvbuf2, paybuf);
        if (paylen == 0) ?? bad mac ??

        ?? process paylen, paybuf ??

        /* Next thing we receive should be another header */

        sshctx -> rcvstate = RCVSTATE_HEADER;

        /* Remove the processed message from the receive buffer */

        break;
      }
      default: abort ();
    }

    /* Remove len1+len2 bytes from the receive buffer */

    sshctx -> rcvlen1 -= len1;
    sshctx -> rcvbuf1 += len1;
    sshctx -> rcvlen2 -= len2;
    sshctx -> rcvbuf2 += len2;
    if (sshctx -> rcvlen1 == 0) {
      sshctx -> rcvlen1 = sshctx -> rcvlen2;
      sshctx -> rcvbuf1 = sshctx -> rcvbuf2;
      sshctx -> rcvlen2 = 0;
      sshctx -> rcvbuf2 = NULL;
    }
    removed += len1 + len2;

    /* Repeat with new state (maybe there's enough in buffers to do next state) */
  }
}

uLong ssh_transmit (Sshctx *sshctx, uLong size, uByte *buff)

{
  uByte pktbuf[TRANSMITBUFFERSIZE];
  uLong offs, pktlen, sent, sts;

  for (offs = 0; offs < size; offs += sent) {
    sent = size - offs;
    if (sent > MAXPAYLOADSIZE) sent = MAXPAYLOADSIZE;
    pktlen = ssh_encodepkt (sshctx, sent, buff, pktbuf);
    sts = (*(sshctx -> transmit)) (sshctx -> xmtctx, sent, pktbuf);
    if (sts != SSH_SUCCESS) return (sts);
  }

  return (SSH_SUCCESS);
}

uLong ssh_receive (Sshctx *sshctx, uLong size, uByte *buff)

{
}

/************************************************************************/
/*									*/
/*  Encode a packet for transmission					*/
/*									*/
/*    Input:								*/
/*									*/
/*	sshctx = connection context					*/
/*	payloadlen = plaintext data length				*/
/*	payloadbuf = plaintext data buffer				*/
/*									*/
/*    Output:								*/
/*									*/
/*	ssh_encode = total length to transmit				*/
/*	*encodedbuf = filled in with data to transmit			*/
/*									*/
/************************************************************************/

static uLong ssh_encodepkt (Sshctx *sshctx, uLong payloadlen, uByte *payloadbuf, uByte *encodedbuf)

{
  uLong cipherblocksize, compressedlen, packetlen, paddinglen;

  /* Compress payloadbuf -> encodedbuf+5 */

  compressedlen = (*(sshctx -> compress)) (sshctx -> cmpctx, payloadlen, payloadbuf, encodedbuf + 5);

  /* Calculate padding length = at least 4, but to make length a multiple of cipher block size */

  cipherblocksize = sshctx -> xmt_cipherbs;
  if (cipherblocksize < 8) cipherblocksize = 8;
  paddinglen = cipherblocksize - (compressedlen % cipherblocksize);
  if (paddinglen < 4) paddinglen += cipherblocksize;
  encodedbuf[4] = paddinglen;

  /* Fill in padding bytes */

  (*(sshctx -> random_fill)) (paddinglen, encodedbuf + 5 + compressedlen);

  /* Fill in packet length */

  packetlen = 5 + compressedlen + paddinglen;
  encodedbuf[0] = packetlen >> 24;
  encodedbuf[1] = packetlen >> 16;
  encodedbuf[2] = packetlen >>  8;
  encodedbuf[3] = packetlen;

  /* Fill in message authentication code */

  (*(sshctx -> xmt_genmac)) (sshctx -> xmt_macctx, packetlen + 4, encodedbuf, encodedbuf + packetlen);

  /* Encrypt message */

  (*(sshctx -> encrypt)) (sshctx -> encctx, packetlen + 4, encodedbuf, encodedbuf);

  /* Return total length */

  return (packetlen + 4 + sshctx -> xmt_macsize);
}

/************************************************************************/
/*									*/
/*  Decode a received packet						*/
/*									*/
/*    Input:								*/
/*									*/
/*	sshctx = connection context					*/
/*	payloadlen = plaintext data length				*/
/*	payloadbuf = plaintext data buffer				*/
/*									*/
/*    Output:								*/
/*									*/
/*	ssh_encode = total length to transmit				*/
/*	*encodedbuf = filled in with data to transmit			*/
/*									*/
/************************************************************************/

	/* returns number of bytes yet to receive */

static uLong ssh_decodehdr (Sshctx *sshctx, uByte *encheader)

{
  uLong packetlen;

  (*(sshctx -> decrypt)) (sshctx -> decctx, sshctx -> rcv_cipherbs, encheader, encheader);
  packetlen = (dechdr[0] << 24) | (dechdr[1] << 16) | (dechdr[2] << 8) | dechdr[3];
  return (packetlen + 4 + sshctx -> rcv_macsize - sshctx -> rcv_cipherbs);
}

	/* returns number of data bytes stored in payloadbuf */
	/*         or zero if the mac failed                 */

static uLong ssh_decodepkt (Sshctx *sshctx, uByte *encpacket, uByte *payloadbuf)

{
  uByte *macbuf;
  uLong packetlen, payloadlen;

  /* Decrypt the packet (header has already been decrypted) */

  packetlen = (encpacket[0] << 24) | (encpacket[1] << 16) | (encpacket[2] << 8) | encpacket[3];
  (*(sshctx -> decrypt)) (sshctx -> decctx, packetlen + 4 - sshctx -> rcv_cipherbs, encpacket + sshctx -> rcv_cipherbs, encpacket + sshctx -> rcv_cipherbs);

  /* Verify the message authentication */

  if (sshctx -> macsize != 0) {
    macbuf = alloca (sshctx -> macsize);
    (*(sshctx -> rcv_genmac)) (sshctx -> rcv_macctx, packetlen + 4, encpacket, macbuf);
    if (memcmp (macbuf, encpacket + packetlen + 4, sshctx -> macsize) != 0) return (0);
  }

  /* Expand the compressed data */

  payloadlen = (*(sshctx -> expand)) (sshctx -> expctx, packetlen - 1 - encpacket[4], encpacket + 5, payloadbuf);

  return (payloadlen);
}
