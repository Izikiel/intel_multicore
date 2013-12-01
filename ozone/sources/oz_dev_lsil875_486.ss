;;+++2002-01-14
;;    Copyright (C) 2001,2002  Mike Rieker, Beverly, MA USA
;;
;;    This program is free software; you can redistribute it and/or modify
;;    it under the terms of the GNU General Public License as published by
;;    the Free Software Foundation; version 2 of the License.
;;
;;    This program is distributed in the hope that it will be useful,
;;    but WITHOUT ANY WARRANTY; without even the implied warranty of
;;    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;;    GNU General Public License for more details.
;;
;;    You should have received a copy of the GNU General Public License
;;    along with this program; if not, write to the Free Software
;;    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
;;---2002-01-14

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;  Copyright 2000,2001,2002  OZONE ENTERPRISES, BEVERLY, MA			;
;										;
;  SCRIPTS for 53C875 chip							;
;										;
;  Under normal conditions, this script is always running.  It looks for new 	;
;  requests to process and old targets re-connecting.  Although there is code 	;
;  to handle the case of a requestor connecting, the host CPU has the control 	;
;  registers set up to disable a response from the chip.			;
;										;
;  In the absense of errors, to get things going, the host CPU queues requests 	;
;  to the scsi_id_table[scsi_id].queue_head and sets the SIGP bit in the ISTAT 	;
;  reg.  This chip then signals completion of the request by doing an INTFLY	;
;  instruction, then this chip processes any other requests it finds in a 	;
;  queue.									;
;										;
;  If an error occurrs, this chip halts and the host CPU gets an interrupt.  	;
;  The host CPU will attempt to recover the condition and restart the chip.  	;
;  The host CPU may decide the best recovery is to software reset this chip 	;
;  and perform a SCSI reset, at which point it will re-load this code and re-	;
;  start this chip.								;
;										;
;  Interrupts occur during these normal conditions and operation continues	;
;    1) A target wants to disconnect in the middle of a transfer (like a disk 	;
;       head seeking to a new track in a long transfer).  The host CPU adjusts 	;
;       the CHMOV/MOVE instructions and restarts this chip.			;
;    2) An I/O completes.  This chip does an INTFLY and continues processing 	;
;       another request without host CPU intervention.				;
;    3) A select times out.  The host CPU removes all requests for this 	;
;       scsi_id and restarts this chip.						;
;  Interrupts that occur for any other condition are considered fatal and 	;
;  cause a reset of the chip and the scsi bus.					;
;										;
;  Command tag queuing not yet supported					;
;										;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
	ARCH	875				; chip is an 53C875
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
;  List of entrypoints that the host can start us at
;
	ENTRY	scsi_id_table			; address of the scsi-id queue head table
	ENTRY	disconnecting			; jump here if unexpected disconnect interrupt
	ENTRY	select_timedout			; jump here after 'SELECT' timeout interrupt
	ENTRY	queue_fixed			; jump here after fixing the queue (INT 0x96)
	ENTRY	startup				; initialization
	ENTRY	transfer_data_done		; the list of rp_datamovs JUMP's here when done
	ENTRY	transfer_data_mismatch		; re-enter here if phase mismatch during data transfer
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
;  Transfer requested negotiation options
;
	ABSOLUTE REQ_ACK_OFFSET   = 16	; can send up to 16 unacknowleged data at a time
	ABSOLUTE XFER_PERIOD_FACT = 12	; 12=50ns (20MHz)
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
;  Scsi id table entry definitions
;
;  There is one entry per possible scsi id.  These are stored in the internal RAM.  
;  The table must be aligned on a 256-byte boundary.  Each element can have up to 
;  16 bytes.
;
;	   se_queue_head[32]: pointer to first request in queue (must be longword aligned)
;	                      <00:01> : 00 - queue is empty
;	                                01 - new request queued
;	                                10 - request is in progress
;	           se_select: the next 4 bytes compose this and are used for the SELECT instruction
;	     se0_sequence[8]: sequence number of request being processed
;	        se1_sxfer[8]: negotiated value (default startup value 0x00)
;	      se2_scsi_id[8]: corresponding scsi_id for SELECT instruction
;	       se3_scntl3[8]: negotiated value (default startup value 0x55)
;	    se_saved_dsp[32]: save pointers' rp_datamov_pa value
;	                      <00> = 0 : no data has been transferred since save
;	                      <00> = 1 : data has been transferred since save
;	    se_saved_dbc[32]: save pointers' contents of 0(rp_datamov_pa)
;
	ABSOLUTE se_queue_head = 0
	ABSOLUTE se_select     = 4
	ABSOLUTE   se0_sequence = se_select + 0
	ABSOLUTE   se1_sxfer    = se_select + 1
	ABSOLUTE   se2_scsi_id  = se_select + 2
	ABSOLUTE   se3_scntl3   = se_select + 3
	ABSOLUTE se_saved_dsp  =  8
	ABSOLUTE se_saved_dbc  = 12
;
;  Initialization value for longword at offset 4
;
	ABSOLUTE INIT_SCNTL3 = 0x55
	ABSOLUTE INIT_SXFER  = 0x00
	ABSOLUTE INIT_SELECT = (INIT_SXFER << 8) | (INIT_SCNTL3 << 24)
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
;  Request packet definitions.  There is one per outstanding I/O request.  These 
;  are stored in the host CPU's general memory.
;
;	      rp_this_va[32]: host's virtual address of this packet
;	      rp_next_pa[32]: pointer to next request in queue 
;	                      (0 if end of queue, else address with low bit set)
;	   rp_datamov_pa[32]: pointer to data transfer CHMOV's
;	        rp0_flags[8]: flag bits:
;	                      <0> : 0=have already done identify message
;	                            1=have yet to do identify message (this is the initial state)
;	                      <1> : 0=don't do width negotiation
;	                            1=do width negotiation
;	                      <2> : 0=don't do synchronous negotiation
;	                            1=do sync negotiation
;	                      <3> : 0=disconnect not allowed
;	                            1=disconnect allowed
;	                      <4> : 0=haven't received status yet
;	                            1=status byte has been received
;	                      <5> : 0=request still pending
;	                            1=request completed
;	                      <6> : 0=don't allow target to disconnect
;	                            1=allow target to disconnect
;	        rp1_abort[8]: 0=host wants request executed as is
;	                   else=host wants request aborted asap (set RP_FLAG_DONE and unhook abort complete)
;	       rp2_seqsts[8]: input: this request's sequence number
;	                     output: the final scsi status byte (RP_FLAG_GOTSTATUS will be set)
;	       rp3_cmdlen[8]: number of bytes in command (1..??)
;	      rp_command[??]: command bytes
;
	ABSOLUTE rp_this_va    =  0
	ABSOLUTE rp_next_pa    =  4
	ABSOLUTE rp_datamov_pa =  8
	ABSOLUTE rp0_flags     = 12
		ABSOLUTE RP_FLAG_NEEDTOIDENT = 0x01
		ABSOLUTE RP_FLAG_NEGWIDTH    = 0x02
		ABSOLUTE RP_FLAG_NEGSYNCH    = 0x04
		ABSOLUTE RP_FLAG_GOTSTATUS   = 0x08
		ABSOLUTE RP_FLAG_DONE        = 0x10
		ABSOLUTE RP_FLAG_ABORTED     = 0x20
		ABSOLUTE RP_FLAG_DISCONNECT  = 0x40
	ABSOLUTE rp1_abort     = 13
		ABSOLUTE RP_ABORT_BUFFEROVF  = 1	; this is the only one of these codes set by the SCRIPTS
	ABSOLUTE rp2_seqsts    = 14
	ABSOLUTE rp3_cmdlen    = 15
	ABSOLUTE rp_command    = 16
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
;  Scratch registers
;
	; scsi_id_entry   = SCRATCHG		; scsi_id_table entry
	; request_packetv = SCRATCHH		; host virt address of request being processed
						; (only valid when SCRATCHJ1 = 1)
						; (... or for INT 0x69)
	; request_packet  = SCRATCHI		; base address of request being processed
	; state           = SCRATCHJ0		; current state (for diag purposes only)
	; doing_data_xfer = SCRATCHJ1		; used to inform host CPU when we expect that an exception might occur
	; scsi_index      = SCRATCHJ2		; scsi index being processed
	; next_scsi_index = SCRATCHJ3		; next scsi index to be processed
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
;  State values kept in SCRATCHJ0
;
	ABSOLUTE STATE_IDLE                 =  0
	ABSOLUTE STATE_GETTING_MESSAGE      =  1
	ABSOLUTE STATE_GETTING_STATUS       =  2
	ABSOLUTE STATE_MESSAGE_OUT          =  3
	ABSOLUTE STATE_CHECKING_TARGET      =  4
	ABSOLUTE STATE_SELECTED             =  5
	ABSOLUTE STATE_SELECTING            =  6
	ABSOLUTE STATE_SENDING_COMMAND      =  7
	ABSOLUTE STATE_TRANSFERRING_DATA    =  8
	ABSOLUTE STATE_WAITING_FOR_RESELECT =  9
	ABSOLUTE STATE_ABORTING             = 10
	ABSOLUTE STATE_REQ_COMPLETE         = 11
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
;  Flags that go in SCRATCHJ1.  The host cpu driver uses these values when it 
;  gets an interrupt to see if it should do anything special to process it.
;
	ABSOLUTE SCRATCHJ1_CHMOVS = 1
	ABSOLUTE SCRATCHJ1_SELECT = 2
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
;  Internal memory
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
;  SCSI id table - this is assumed by the host CPU to be at the very beginning 
;  of the internal memory.  It is assumed by this SCRIPT processor to be on a 
;  256-byte boundary.
;
scsi_id_table:	CHMOV	0, INIT_SELECT | ( 0 << 16), WHEN DATA_OUT
		CHMOV	0,                        0, WHEN DATA_OUT
		CHMOV	0, INIT_SELECT | ( 1 << 16), WHEN DATA_OUT
		CHMOV	0,                        0, WHEN DATA_OUT
		CHMOV	0, INIT_SELECT | ( 2 << 16), WHEN DATA_OUT
		CHMOV	0,                        0, WHEN DATA_OUT
		CHMOV	0, INIT_SELECT | ( 3 << 16), WHEN DATA_OUT
		CHMOV	0,                        0, WHEN DATA_OUT
		CHMOV	0, INIT_SELECT | ( 4 << 16), WHEN DATA_OUT
		CHMOV	0,                        0, WHEN DATA_OUT
		CHMOV	0, INIT_SELECT | ( 5 << 16), WHEN DATA_OUT
		CHMOV	0,                        0, WHEN DATA_OUT
		CHMOV	0, INIT_SELECT | ( 6 << 16), WHEN DATA_OUT
		CHMOV	0,                        0, WHEN DATA_OUT
		CHMOV	0, INIT_SELECT | ( 7 << 16), WHEN DATA_OUT
		CHMOV	0,                        0, WHEN DATA_OUT
		CHMOV	0, INIT_SELECT | ( 8 << 16), WHEN DATA_OUT
		CHMOV	0,                        0, WHEN DATA_OUT
		CHMOV	0, INIT_SELECT | ( 9 << 16), WHEN DATA_OUT
		CHMOV	0,                        0, WHEN DATA_OUT
		CHMOV	0, INIT_SELECT | (10 << 16), WHEN DATA_OUT
		CHMOV	0,                        0, WHEN DATA_OUT
		CHMOV	0, INIT_SELECT | (11 << 16), WHEN DATA_OUT
		CHMOV	0,                        0, WHEN DATA_OUT
		CHMOV	0, INIT_SELECT | (12 << 16), WHEN DATA_OUT
		CHMOV	0,                        0, WHEN DATA_OUT
		CHMOV	0, INIT_SELECT | (13 << 16), WHEN DATA_OUT
		CHMOV	0,                        0, WHEN DATA_OUT
		CHMOV	0, INIT_SELECT | (14 << 16), WHEN DATA_OUT
		CHMOV	0,                        0, WHEN DATA_OUT
		CHMOV	0, INIT_SELECT | (15 << 16), WHEN DATA_OUT
		CHMOV	0,                        0, WHEN DATA_OUT
;
;  Misc variables
;
scsi_id_entry:	CHMOV	0, scsi_id_table, WHEN DATA_OUT	; points to current scsi_id's entry in scsi_id_table
							; (copy of SCRATCHG so it can be easily LOADed into the DSA)
;
request_packet:	CHMOV	0, 0, WHEN DATA_OUT		; points to current request packet being worked on
							; low 2 bits are always clear in this version
							; (copy of SCRATCHI so it can be easily LOADed into the DSA)
;
msg_buf:	CHMOV	0, 0, WHEN DATA_OUT		; 8-byte temp message-in or -out buffer
;
;  Predefined messages
;
ident_abort_task_msg:	CHMOV	0x06C0, 0, WHEN DATA_OUT ; IDENTIFY, ABORT TASK (0xC0, 0x06)
abort_task:		CHMOV	0x06, 0, WHEN DATA_OUT	; ABORT TASK (0x06)
message_reject:		CHMOV	0x07, 0, WHEN DATA_OUT	; MESSAGE REJECT (0x07)
noop_message:		CHMOV	0x08, 0, WHEN DATA_OUT	; NO-OP (0x08)

					; the 'message_ident_*' messages have their first byte modified depending on the 
					; setting of rp0_flags<6>.  1=allow target to disconnect, 0=don't allow it.
message_ident_width:	CHMOV	0x0201C0, 1, WHEN STATUS
					; IDENTIFY AND ALLOW DISCONNECT (0xC0), 
					; NEGOTIATE WIDE DATA TRANSFER (0x01,0x02,0x03,1)

message_ident_synch:	CHMOV	0x0301C0, XFER_PERIOD_FACT+(REQ_ACK_OFFSET<<8), WHEN DATA_IN
					; IDENTIFY AND ALLOW DISCONNECT (0xC0), 
					; NEGOTIATE SYNCHRONOUS TRANSFER (0x01,0x03,0x01,XFER_PERIOD_FACT,REQ_ACK_OFFSET)
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
;  This is the initial start entrypoint
;
startup:
	MOVE 0 TO SCRATCHJ1				; we are not doing anything special
							; - so if host gets an interrupt (other than INTFLY), 
							;   it should consider it a fatal error
	MOVE 0 TO SCRATCHJ3				; start looking at device 0
	LOAD SCRATCHG0, 4, scsi_id_entry+4		; set scsi_id_table entry pointer
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
;  See if there is any new request to start (or any old requests that are to be aborted)
;
;  New requests will have the low bit set in scsi_id_table[scsi_id].queue_head
;  When the queue is empty, scsi_id_table[scsi_id].queue_head will be zero
;  When the request is in progress, scsi_id_table[scsi_id].queue_head will be non-zero with two low bits = 10
;
select_timedout:
queue_fixed:
mainloop:
	MOVE STATE_IDLE TO SCRATCHJ0			; we aren't doing anything
	MOVE ISTAT & 0xDF TO ISTAT			; about to scan loop, so clear SIGP flag bit
							; if host queues something while we're scanning, 
							; ... it will set this bit again and we won't 
							; ... hang in the WAIT RESELECT instruction
	MOVE 0xF0 TO SCRATCHA0				; max of 16 devices to scan
new_req_scanloop:
	MOVE SCRATCHJ3 TO SFBR				; get which device to scan for
	MOVE SFBR TO SCRATCHJ2
	MOVE SCRATCHJ3 +  1 TO SCRATCHJ3		; set up id of next device to check
	MOVE SCRATCHJ3 & 15 TO SCRATCHJ3
	CALL REL (get_request_packet)			; point to first request packet queued to device
	JUMP REL (check_next_scsi_id), IF 0		; on to next device if nothing queued
	JUMP REL (got_new_req), IF 1			; break out if we got something new
	LOAD DSA0, 4, request_packet			; see if there is an old request ...
	LOAD SCRATCHA1, 1, DSAREL (rp1_abort)		; ... that host CPU wants us to abort
	MOVE SCRATCHA1 TO SFBR
	JUMP REL (abort_request_inprog), IF NOT 0
check_next_scsi_id:
	MOVE SCRATCHA0 + 1 TO SCRATCHA0			; nothing to do there, try next device
	JUMP REL (new_req_scanloop), IF NOT CARRY
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
;  Check for some target trying to reselect
;  Presumably, it is ready for data or status transfer
;
check_reselect:
	MOVE 0 TO SCRATCHJ1				; (no longer doing the SELECT instruction)
	WAIT RESELECT REL (check_select)		; wait for reselection
							; - this will jump to 'check_select' if either ISTAT<SIGP> is set
							;   (meaning there are new requests to process) or some dumbell is 
							;   trying to select me
;
;  Something is trying to re-select, resume processing the target
;
	MOVE SSID TO SFBR				; see who is calling us
	JUMP REL (bad_reselect_scsi_id), IF 0 AND MASK 0x7F ; bad if the 'valid' bit is not set
	MOVE SFBR & 0x0F TO SCRATCHJ2			; save the scsi id in the scsi_index scratch register

	CALL REL (get_request_packet)			; set request_packet = first queue entry for the scsi_index
	JUMP REL (reselected), IF 2			; jump if an old request is in progress on the target
							; if we don't have an old request, we have no 
							; idea why this target is trying to re-connect!
bad_reselect_scsi_id:
	CALL REL (set_atn)				; reselect from unknown source, tell target we have something to tell it
	MOVE 0x00 TO SCNTL2				; we expect a disconnect
	MOVE 1, abort_task, WHEN MSG_OUT		; tell it to abort what it is trying to do
	WAIT DISCONNECT					; wait for target to disconnect from scsi bus
	JUMP REL (mainloop)				; go find something else to do
;
;  Target just reselected
;
;    Registers:
;
;	SCRATCHG  (scsi_id_entry)  = points to the scsi_id_table entry for the device
;	SCRATCHI  (request_packet) = points to the old request packet (low bits are clear)
;	SCRATCHJ2 (scsi_index)     = scsi-id of device that reselected
;	DSA = scsi_id_entry
;
reselected:
	CALL REL (restore_pointers)
	LOAD DSA0, 4, scsi_id_entry
;
;  Set up previously negotiated width and speed then see what target wants
;
set_connect_params:
	LOAD SCNTL3, 1, DSAREL (se3_scntl3)
	LOAD SXFER, 1, DSAREL (se1_sxfer)
	JUMP REL (check_target_state)
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
;  Not being reselected, maybe some idiot is trying to select me
;  Supposedly this should never happen as the host CPU has disabled selection as a target
;
check_select:
	WAIT SELECT REL (mainloop)			; see if someone is trying to select me
							; - this will jump to 'mainloop' if either ISTAT<SIGP> is set
							;   (meaning there are new requests to process) or some target 
							;   is trying to reselect me
	MOVE 1, abort_task, WITH MSG_OUT
	DISCONNECT
	CLEAR TARGET
	JUMP REL (mainloop)
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
;  The host CPU has asked that a request in progress is to be aborted
;  So we tell the target to abort the task and post the request as completed
;
abort_request_inprog:
	LOAD DSA0, 4, scsi_id_entry
	SELECT ATN FROM se_select, REL (check_reselect)	; select the target and set previously negotiated width and speed
							; - a jump to check_reselect is made if either someone is trying to 
							;   reselect me or if the host CPU sets the SIGP bit
							; - if this select times out, the host CPU will abort all requests 
							;   queued to the device anyway before restarting me
	MOVE 0x00 TO SCNTL2				; we expect a disconnect
	MOVE 2, ident_abort_task_msg, WHEN MSG_OUT	; ... after we tell it to F0A9+4
	JUMP REL (abort_request_markit)
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
;  New request found, try to select the device.  If another target tries to 
;  reselect us in the mean time, forget about this for now and go process it.
;
;    Registers:
;
;	SCRATCHG  (scsi_id_entry)  = DSA = points to the scsi_id_table entry for the device
;	SCRATCHI  (request_packet) = points to the new request packet (low bits are clear)
;	SCRATCHJ2 (scsi_index)     = scsi-id of device that has a new request
;
got_new_req:
	LOAD SCRATCHA0, 1, DSAREL (se0_sequence)	; get what its sequence should be
	MOVE SCRATCHA0 TO SFBR				; ... into SFBR
	LOAD DSA0, 4, request_packet			; point to the request packet
	LOAD SCRATCHA2, 1, DSAREL (rp2_seqsts)		; get what its sequence actually is
	MOVE SCRATCHA2 XOR SFBR TO SCRATCHA2		; hopefully they match
	MOVE SCRATCHA2 TO SFBR
	INT 0x96, IF NOT 0				; halt if not
							; - the host CPU will fix the queue then restart us at queue_fixed
	LOAD SCRATCHA1, 1, DSAREL (rp1_abort)		; see if host CPU wants this request aborted
	MOVE SCRATCHA1 TO SFBR
	JUMP REL (req_complete_ab), IF NOT 0		; if so, go post it as completed
	LOAD DSA0, 4, scsi_id_entry
	MOVE STATE_SELECTING TO SCRATCHJ0		; set the state to 'SELECTING'
	MOVE SCRATCHJ1_SELECT TO SCRATCHJ1		; tell host CPU that this is THE select so it knows to do recovery
	SELECT ATN FROM se_select, REL (check_reselect)	; select the target and set previously negotiated width and speed
							; a jump is made to 'check_reselect' if someone is trying to (re)select
							; - note that SIGP is probably still clear so the WAIT instructions should 
							;   do their job.  If SIGP was set by the host in the mean time, this idiot 
							;   computer skips the WAIT instructions and thus we should end up right 
							;   back here very quickly only to do it all over again (but hopefully with 
							;   SIGP cleared)
							; if this times out, host CPU will jump to 'select_timedout' 
							; - the host aborts all requests from the queue first, however
							; - it also clears SCRATCHJ1 for us
							; - the jump goes back to clear SIGP and re-scan for more requests to start
	MOVE 0 TO SCRATCHJ1				; (no longer doing the SELECT instruction)
;
;  We have selected the target, mark the queue entry as being 'in progress' by clearing se_queue_head <00> and setting <01>
;  Also, clear the low bit in queue_head entry so we won't think we need to select again
;
	MOVE STATE_SELECTED TO SCRATCHJ0		; set the state to 'SELECTED'
	MOVE SCRATCHI0 | 0x02 TO SFBR			; bit <00> is already clear, so just set bit <01>
	STORE NOFLUSH SFBR, 1, DSAREL (se_queue_head)	; store the request_packet pointer with appropriate low bits
;
;  Save the initial data pointer
;
	CALL REL (save_data_pointer)
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
;  Now see what target wants to do - it tells us by what phase it is in
;
;    Registers:
;
;	SCRATCHJ2 (scsi_index)     = the scsi-id of the device we are processing
;	SCRATCHG  (scsi_id_entry)  = points to entry in the scsi_id_table that we are processing
;	SCRATCHI  (request_packet) = points to the request packet for the device
;
check_target_state:
	MOVE STATE_CHECKING_TARGET TO SCRATCHJ0		; set state to 'checking target'
	JUMP REL (transfer_message_out), WHEN MSG_OUT	; now see what the target wants to do
							; ?? - it seems that we get the 'select timeout' interrupt
							; ??   here instead of at the SELECT instruction above!
							; ??   so the host CPU is going to have to deal with that!
							; ?? - also got an 'unexpected disconnect' here so we have 
							; ??   the host CPU jump to 'disconnecting' label below
	JUMP REL (transfer_command),     IF CMD
	JUMP REL (transfer_data_in),     IF DATA_IN
	JUMP REL (transfer_data_out),    IF DATA_OUT
	JUMP REL (transfer_status),      IF STATUS
	JUMP REL (transfer_message_in),  IF MSG_IN
	INT 0xA1					; don't know what to do - target has gone nuts
;
;  Target is ready to accept a message from us
;  We generally have three messages to send it (in this order):
;    1) Identify (always)
;    2) Negotiate width (optional)
;    3) Negotiate speed (optional)
;
transfer_message_out:
	MOVE STATE_MESSAGE_OUT TO SCRATCHJ0		; set up new state
	LOAD DSA0, 4, request_packet			; see what messages we have yet to send
	LOAD SCRATCHA0, 1, DSAREL (rp0_flags)
	MOVE SCRATCHA0 TO SFBR
	JUMP REL (send_message_ident),       IF                    RP_FLAG_NEEDTOIDENT AND MASK 0xFF - (RP_FLAG_NEGSYNCH | RP_FLAG_NEGWIDTH | RP_FLAG_NEEDTOIDENT)
	JUMP REL (send_message_width),       IF RP_FLAG_NEGWIDTH                       AND MASK 0xFF - (                   RP_FLAG_NEGWIDTH | RP_FLAG_NEEDTOIDENT)
	JUMP REL (send_message_ident_width), IF RP_FLAG_NEGWIDTH | RP_FLAG_NEEDTOIDENT AND MASK 0xFF - (                   RP_FLAG_NEGWIDTH | RP_FLAG_NEEDTOIDENT)
	JUMP REL (send_message_synch),       IF RP_FLAG_NEGSYNCH                       AND MASK 0xFF - (RP_FLAG_NEGSYNCH | RP_FLAG_NEGWIDTH | RP_FLAG_NEEDTOIDENT)
	JUMP REL (send_message_ident_synch), IF RP_FLAG_NEGSYNCH | RP_FLAG_NEEDTOIDENT AND MASK 0xFF - (RP_FLAG_NEGSYNCH | RP_FLAG_NEGWIDTH | RP_FLAG_NEEDTOIDENT)

	MOVE 1, noop_message, WHEN MSG_OUT		; we have nothing to send, so send a noop (this also clears the ATN bit)
	JUMP REL (check_target_state)

send_message_ident:
	MOVE SFBR & 0xFF - RP_FLAG_NEEDTOIDENT TO SFBR	; clear flag bit
	STORE NOFLUSH SFBR, 1, DSAREL (rp0_flags)
	MOVE SFBR & 0x40 TO SFBR			; get 'allow disconnects' flag bit
	MOVE SFBR | 0x80 TO SFBR			; make the 'identify' message code
	STORE NOFLUSH SFBR, 1, message_ident_width	; store in message to be sent
	MOVE 1, message_ident_width, WHEN MSG_OUT	; send ident message (this also clears the ATN bit)
	JUMP REL (check_target_state)

send_message_ident_width:
	MOVE SFBR & 0xFF - (RP_FLAG_NEEDTOIDENT | RP_FLAG_NEGWIDTH) TO SFBR ; clear flag bits
	STORE NOFLUSH SFBR, 1, DSAREL (rp0_flags)
	MOVE SFBR & 0x40 TO SFBR			; get 'allow disconnects' flag bit
	MOVE SFBR | 0x80 TO SFBR			; make the 'identify' message code
	STORE NOFLUSH SFBR, 1, message_ident_width	; store in message to be sent
	MOVE 5, message_ident_width, WHEN MSG_OUT	; send ident and width messages (this also clears the ATN bit)
	JUMP REL (proc_width_reply)

send_message_width:
	MOVE SFBR & 0xFF - RP_FLAG_NEGWIDTH TO SFBR	; clear flag bit
	STORE NOFLUSH SFBR, 1, DSAREL (rp0_flags)
	MOVE 4, message_ident_width+1, WHEN MSG_OUT	; send width message (this also clears the ATN bit)

proc_width_reply:
	MOVE 1, msg_buf, WHEN MSG_IN			; read the reply, byte-by-byte
	JUMP REL (bad_nego_mess), IF NOT 0x01		; - it must be an extended message
	CLEAR ACK
	MOVE 1, msg_buf, WHEN MSG_IN
	JUMP REL (bad_nego_mess_reject), IF NOT 0x02	; - it must have length 2
	CLEAR ACK
	MOVE 1, msg_buf, WHEN MSG_IN
	JUMP REL (bad_nego_mess_reject), IF NOT 0x03	; - it must be a 'wide data transfer' message
	CLEAR ACK
	MOVE 1, msg_buf, WHEN MSG_IN			; ok, get the resultant width (either 8 or 16 bits)
	CALL REL (save_transfer_width)			; save it in memory
	LOAD DSA0, 4, request_packet			; see if we have to send synchronous negotiation message, too
	LOAD SCRATCHA0, 1, DSAREL (rp0_flags)
	MOVE SCRATCHA0 TO SFBR
	JUMP REL (proc_width_reply_clear_ack), IF 0 AND MASK 0xFF - RP_FLAG_NEGSYNCH
	SET ATN						; need to do synch negotiation, tell target we have another message
proc_width_reply_clear_ack:
	CLEAR ACK					; anyway, clear ack to indicate we got all the incoming message
	JUMP REL (set_connect_params)			; set up new width

send_message_ident_synch:
	MOVE SFBR & 0xFF - (RP_FLAG_NEEDTOIDENT | RP_FLAG_NEGSYNCH) TO SFBR ; clear flag bits
	STORE NOFLUSH SFBR, 1, DSAREL (rp0_flags)
	MOVE SFBR & 0x40 TO SFBR			; get 'allow disconnects' flag bit
	MOVE SFBR | 0x80 TO SFBR			; make the 'identify' message code
	STORE NOFLUSH SFBR, 1, message_ident_width	; store in message to be sent
	MOVE 6, message_ident_synch, WHEN MSG_OUT	; send ident and synch messages (this also clears the ATN bit)
	JUMP REL (proc_synch_reply)

send_message_synch:
	MOVE SFBR & 0xFF - RP_FLAG_NEGSYNCH TO SFBR	; clear flag bit
	STORE NOFLUSH SFBR, 1, DSAREL (rp0_flags)
	MOVE 5, message_ident_synch+1, WHEN MSG_OUT	; send synch message (this also clears the ATN bit)

proc_synch_reply:
	MOVE 1, msg_buf, WHEN MSG_IN			; read the reply, byte-by-byte
	JUMP REL (bad_nego_mess), IF NOT 0x01		; - it must be an extended message
	CLEAR ACK
	MOVE 1, msg_buf, WHEN MSG_IN
	JUMP REL (bad_nego_mess_reject), IF NOT 0x03	; - it must have length 3
	CLEAR ACK
	MOVE 1, msg_buf, WHEN MSG_IN
	JUMP REL (bad_nego_mess_reject), IF NOT 0x01	; - it must be a 'synchronous data transfer' message
	CLEAR ACK
	MOVE 1, msg_buf, WHEN MSG_IN			; ok, get the resultant tranfer period factor
	CALL REL (save_transfer_period)			; save it in memory
	CLEAR ACK
	MOVE 1, msg_buf, WHEN MSG_IN			; get the resultant req/ack offset
	CALL REL (save_req_ack_offset)			; save it in memory
	CLEAR ACK					; clear ack to indicate we got all the incoming message
	JUMP REL (set_connect_params)			; set up new speed
;
;  Target is ready to accept the command from us
;
transfer_command:
	MOVE STATE_SENDING_COMMAND TO SCRATCHJ0		; set state 'sending command'
	LOAD DSA0, 4, request_packet			; point to the request packet
	LOAD SCRATCHA3, 1, DSAREL (rp3_cmdlen)		; get length of command
	MOVE SCRATCHA3 TO SFBR
	STORE NOFLUSH SFBR, 1, command_move+0		; store in the 'command_move' instruction
	MOVE DSA0 + rp_command TO DSA0			; point to the command bytes
	MOVE DSA1 + 0 TO DSA1 WITH CARRY
	MOVE DSA2 + 0 TO DSA2 WITH CARRY
	MOVE DSA3 + 0 TO DSA3 WITH CARRY
	STORE DSA0, 4, command_move+4
command_move:
	MOVE 0, 0, WHEN CMD				; transfer the command bytes to the target
	JUMP REL (check_target_state)			; now see what target wants to do
;
;  Target is ready to transfer data
;
transfer_data_in:
transfer_data_out:
	MOVE STATE_TRANSFERRING_DATA TO SCRATCHJ0	; set state 'transferring data'
	LOAD DSA0, 4, scsi_id_entry			; remember pointer is about to be moved
	LOAD SCRATCHA0, 1, DSAREL (se_saved_dsp)	; ... by setting se_saved_dsp<00>
	MOVE SCRATCHA0 | 0x01 TO SCRATCHA0		; ... so restore_pointers knows it has work to do
	STORE NOFLUSH SCRATCHA0, 1, DSAREL (se_saved_dsp)
	LOAD DSA0, 4, request_packet			; point to the request packet
	LOAD TEMP0, 4, DSAREL (rp_datamov_pa)		; get where the CHMOV's are
	LOAD SCRATCHH0, 4, DSAREL (rp_this_va)		; get virt address of request packet (for host CPU in case of interrupt)
	MOVE SCRATCHJ1_CHMOVS TO SCRATCHJ1		; set the 'CHMOV in progress' flag
	RETURN						; execute the CHMOV's (jumps to address in TEMP)
							; code will jump to 'transfer_data_done' if it gets to the end ok
							; otherwise, if a phase mismatch occurrs, the host cpu will modify 
							;   the 'rp_datamov_pa' pointer accordingly then jump to 
							;   'transfer_data_mismatch'
;
transfer_data_done2:
	STORE NOFLUSH TEMP0, 4, DSAREL (rp_datamov_pa)	; set rp_datamov_pa = transfer_no_data
transfer_data_mismatch:
	MOVE 0 TO SCRATCHJ1				; clear the 'CHMOV in progress' flag
	JUMP REL (check_target_state)			; go see what target wants now
;
transfer_data_done:
	CALL REL (transfer_data_done2)			; set TEMP = transfer_no_data and jump
transfer_no_data:
	CALL REL (set_atn)				; target has/wants more data but there isn't any (room) left
	MOVE 0x00 TO SCNTL2				; - we expect a disconnect
	MOVE 1, abort_task, WHEN MSG_OUT		; - send it a nasty message
	MOVE RP_ABORT_BUFFEROVF TO SCRATCHA1		; set up abort code
	STORE NOFLUSH SCRATCHA1, 1, DSAREL (rp1_abort)
abort_request_markit:
	LOAD SCRATCHA0, 1, DSAREL (rp0_flags)		; tell the host we sent the target an 'abort task' message
	MOVE SCRATCHA0 | RP_FLAG_ABORTED TO SCRATCHA0
	STORE NOFLUSH SCRATCHA0, 1, DSAREL (rp0_flags)
	WAIT DISCONNECT					; target responds by disconnecting
req_complete_ab:
	LOAD DSA0, 4, scsi_id_entry			; go mark the request complete and tell host CPU
	JUMP REL (req_complete2)
;
;  Target has status byte ready for us
;
transfer_status:
	MOVE STATE_GETTING_STATUS TO SCRATCHJ0		; set state 'getting status'
	MOVE 1, msg_buf, WHEN STATUS			; read the status byte
	MOVE SFBR TO SCRATCHA2
	LOAD DSA0, 4, request_packet			; store in request packet struct
	STORE NOFLUSH SCRATCHA2, 1, DSAREL (rp2_seqsts)
	LOAD SCRATCHA0, 1, DSAREL (rp0_flags)		; ... and say that we got it
	MOVE SCRATCHA0 | RP_FLAG_GOTSTATUS TO SCRATCHA0
	STORE SCRATCHA0, 1, DSAREL (rp0_flags)
	JUMP REL (check_target_state)
;
;  Target has an unsolicited message for us
;
transfer_message_in:
	MOVE STATE_GETTING_MESSAGE TO SCRATCHJ0		; set state 'getting message'
	MOVE 1, msg_buf, WHEN MSG_IN			; see what the target wants
	JUMP REL (req_complete), IF 0x00		; 00 means the request is now complete
	JUMP REL (got_extended_message), IF 0x01	; check for 'extended' messages
	JUMP REL (save_data_pointer_ack), IF 0x02	; 02 means to save data transfer pointer
	JUMP REL (restore_pointers_ack), IF 0x03	; 03 means to restore transfer pointers
	JUMP REL (disconnecting), IF 0x04		; 04 means it is disconnecting and will reselect us later
	JUMP REL (ignore_message), IF 0x07		; skip over reject's
	JUMP REL (ignore_message), IF 0x08		; skip over nop's
	JUMP REL (ignore_message), IF 0x80 AND MASK 0x7F ; ignore all identify messages from target

reject_message:
	CALL REL (set_atn)				; tell target we are rejecting its message
	MOVE 1, message_reject, WHEN MSG_OUT		; send reject message, clear ATN
	JUMP REL (check_target_state)

save_data_pointer_ack:
	CALL REL (save_data_pointer)			; save current data pointer
ignore_message:
	CLEAR ACK					; acknowledge it
	JUMP REL (check_target_state)			; go see what target wants now

restore_pointers_ack:
	CLEAR ACK					; acknowledge it
	LOAD DSA0, 4, scsi_id_entry			; go restore pointers
	CALL REL (restore_pointers)
	JUMP REL (check_target_state)

got_extended_message:
	CLEAR ACK
	MOVE 2, msg_buf, WHEN MSG_IN			; get extended message length and code
	LOAD SCRATCHA0, 2, msg_buf			; get length in A0, code in A1
	MOVE SCRATCHA1 TO SFBR				; check out the code
	JUMP REL (got_sync_data_xfer_msg), IF 0x01
	JUMP REL (got_wide_data_xfer_msg), IF 0x03
	JUMP REL (reject_message)

got_wide_data_xfer_msg:
	MOVE SCRATCHA0 TO SFBR				; make sure the length is 2
	JUMP REL (bad_nego_mess_reject), IF NOT 0x02
	CLEAR ACK
	MOVE 1, msg_buf, WHEN MSG_IN			; get the requested width factor
	CLEAR ACK					; tell target we got the last byte of message
	MOVE MEMORY NOFLUSH 8, message_ident_width, msg_buf ; copy a template message
	JUMP REL (width_ok), IF 0			; width 0 (8-bits) is ok as is
	MOVE 1 TO SFBR					; else, do width 1 (16-bits)
width_ok:
	STORE NOFLUSH SFBR, 1, msg_buf+4		; save it in outgoing message buffer
	CALL REL (save_transfer_width)			; save negotiated width in scsi_id_table entry
	MOVE 4, msg_buf+1, WHEN MSG_OUT			; send reply message
	JUMP REL (set_connect_params)			; go set up new width

got_sync_data_xfer_msg:
	MOVE SCRATCHA0 to SFBR				; make sure the length is 3
	JUMP REL (bad_nego_mess_reject), IF NOT 0x03
	CLEAR ACK
	MOVE 2, msg_buf, WHEN MSG_IN			; get the transfer period factor and req/ack offset bytes
	CLEAR ACK					; tell target we got the last bytes of message
	LOAD SCRATCHB1, 1, msg_buf+1			; save req/ack offset factor
	MOVE MEMORY NOFLUSH 8, message_ident_synch, msg_buf ; set up a template message
	CALL REL (save_transfer_period)			; process negotiated speed setting
	MOVE SCRATCHA2 TO SFBR				; save it in reply message buffer
	STORE NOFLUSH SFBR, 1, msg_buf+4
	MOVE SCRATCHB1 TO SFBR				; get requested req/ack offset
	CALL REL (save_req_ack_offset)			; process it
	MOVE SCRATCHA2 TO SFBR				; save negotiated value in reply message
	MOVE SFBR TO SCRATCHA1
	STORE NOFLUSH SCRATCHA1, 1, msg_buf+5
	MOVE 5, msg_buf+1, WHEN MSG_OUT			; send reply message
	JUMP REL (set_connect_params)			; go set up new speed
;
;  Something was bad about the negotiation reply or request, send reject then set up async mode
;
bad_nego_mess:
	JUMP REL (bad_nego_mess_async), IF 0x07		; maybe it is rejecting my request
bad_nego_mess_reject:
	CALL REL (set_atn)				; something else bad with message, reject it
	MOVE 1, message_reject, WHEN MSG_OUT
bad_nego_mess_async:
	CLEAR ACK
	MOVE INIT_SCNTL3 TO SCRATCHA3			; set up async transfer mode
	MOVE INIT_SXFER TO SCRATCHA1
	LOAD DSA0, 4, scsi_id_entry
	STORE NOFLUSH SCRATCHA3, 1, DSAREL (se3_scntl3)
	STORE NOFLUSH SCRATCHA1, 1, DSAREL (se1_sxfer)
	JUMP REL (set_connect_params)
;
;  Target is disconnecting, remember the state and go do something else in the mean time
;
disconnecting:
	MOVE 0x00 TO SCNTL2				; we expect a disconnect
	CLEAR ACK					; acknowledge it
	MOVE STATE_WAITING_FOR_RESELECT TO SCRATCHJ0	; remember we're waiting for a reselect from it
	WAIT DISCONNECT					; wait for target to disconnect from scsi bus
	JUMP REL (mainloop)				; go find something else to do
;
;  The request is complete - unlink it from queue and do an INTFLY to notify host CPU that a request just completed
;
req_complete:
	MOVE 0x00 TO SCNTL2				; we expect a disconnect
	CLEAR ACK					; acknowledge it
	WAIT DISCONNECT					; wait for target to disconnect from scsi bus
req_complete2:
	MOVE STATE_REQ_COMPLETE TO SCRATCHJ0		; posting a request's completion
	LOAD DSA0, 4, request_packet
	MOVE ISTAT | 0x10 TO ISTAT			; set the SEM bit to let host know were modifying queue and RP_FLAG_DONE
	LOAD SCRATCHA0, 4, DSAREL (rp_next_pa)		; find the next item in the list (it is either zero or has the low bit set)
	LOAD DSA0, 4, scsi_id_entry
	STORE NOFLUSH SCRATCHA0, 4, DSAREL (se_queue_head) ; unlink request from queue
							; the host cpu's 'start_request' routine assumes se_queue_head 
							; ... gets cleared before the RP_FLAG_DONE bit gets set
	LOAD SCRATCHA0, 1, DSAREL (se0_sequence)	; increment sequence for next request
	MOVE SCRATCHA0 + 1 TO SCRATCHA0
	STORE NOFLUSH SCRATCHA0, 1, DSAREL (se0_sequence)
	LOAD DSA0, 4, request_packet
	LOAD SCRATCHA0, 1, DSAREL (rp0_flags)		; flag the request as 'done'
	MOVE SCRATCHA0 | RP_FLAG_DONE TO SCRATCHA0
	STORE SCRATCHA0, 1, DSAREL (rp0_flags)
	MOVE ISTAT & 0xEF TO ISTAT			; clear SEM bit to let host know were done with mods
	INTFLY						; tell the host computer that a request completed
	JUMP REL (mainloop)				; go find something to do
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
;  Get pointer to first request packet queued to a scsi device
;
;    Input:
;
;	SCRATCHG = some entry of the scsi_id_table (not necessarily the one we want)
;	SCRATCHJ2 (scsi_index) = scsi id to get packet for
;
;    Output:
;
;	SCRATCHI (request_packet) = address of queued packet (with low bits cleared)
;	SCRATCHG (scsi_id_entry)  = DSA = points to scsi_id_table entry
;	SBFR = 0 : nothing queued, device is idle
;	       1 : new request queued, needs to be started
;	       2 : old request in progress, waiting for reselect
;
get_request_packet:
;
;  We point SCRATCHG and scsi_id_entry at the entry in the scsi_id_table
;  corresponding to the device in question.  It is here that we assume 
;  the scsi_id_table is on a 256-byte boundary so we don't have to worry 
;  about adding and carry bits.
;
	MOVE SCRATCHJ2 TO SFBR			; multiply scsi_index by 16 to get offset in scsi_id_table
	MOVE SFBR + 0 TO SCRATCHG0		; (the '+ 0' hopefully clears carry bit for subsequent SHL's)
	MOVE SCRATCHG0 SHL SCRATCHG0
	MOVE SCRATCHG0 SHL SCRATCHG0
	MOVE SCRATCHG0 SHL SCRATCHG0
	MOVE SCRATCHG0 SHL SCRATCHG0
	STORE NOFLUSH SCRATCHG0, 4, scsi_id_entry ; save it for easy loading into DSA
;
;  We point SCRATCHI and request_packet at the top packet on the queue for 
;  the device in question.  SCRATCHI and request_packet have the low 2 bits 
;  cleared.  The low 2 bits are returned in SFBR for easy testing on return.
;
	LOAD DSA0, 4, scsi_id_entry		; point the DSA at the scsi_id_table entry for the device
	LOAD SCRATCHI0, 4, DSAREL (se_queue_head) ; load the scsi_id_table[scsi_index].queue_head entry into 'request_packet'
	MOVE SCRATCHI0 & 0x03 TO SFBR		; save low 2 bits
	MOVE SCRATCHI0 & 0xFC TO SCRATCHI0	; clear them in the pointer register
	STORE NOFLUSH SCRATCHI0, 4, request_packet ; save it for easy loading into DSA later
;
	RETURN
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
;  Save data pointers
;
save_data_pointer:
	LOAD DSA0, 4, request_packet			; get current pointer
	LOAD SCRATCHA0, 4, DSAREL (rp_datamov_pa)	; get address of chmov
	LOAD DSA0, 4, DSAREL (rp_datamov_pa)		; point to the chmov
	LOAD SCRATCHB0, 4, DSAREL (0)			; get move bytecount
	LOAD DSA0, 4, scsi_id_entry			; point to scsi_id_table entry
	STORE NOFLUSH SCRATCHA0, 4, DSAREL (se_saved_dsp) ; save datapointer
	STORE NOFLUSH SCRATCHB0, 4, DSAREL (se_saved_dbc)
	RETURN
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
;  Restore pointers to their last saved state
;
restore_pointers:
	LOAD SCRATCHA0, 1, DSAREL (se_saved_dsp)	; see if restore required
	MOVE SCRATCHA0 SHR SCRATCHA0
	RETURN, IF NOT CARRY				; (only if <00> is set)
	LOAD DSA0, 4, request_packet			; point to request packet
	LOAD SCRATCHH0, 4, DSAREL (rp_this_va)		; get its va for host CPU
	INT 0x69					; have host CPU fix it
							; - it restores the whole rp_datachmovs list
							; - it restores rp_datamov_pa from se_saved_dsp
							; - it modifies that instr w/ se_saved_dnad
	RETURN
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
;  Set ATN and wait for target to enter MSG_OUT phase
;
;    Output:
;
;	target now in MSG_OUT phase
;
;    Scratch:
;
;	SFBR, msg_buf[0]
;
set_atn:
	SET ATN						; tell target we have something to say
set_atn_clrack:
	CLEAR ACK					; in case last thing was a MSG_IN
set_atn_loop:
	RETURN, WHEN MSG_OUT				; if target is in MSG_OUT, return to caller
	JUMP REL (set_atn_data_in), WHEN DATA_IN	; if target is in DATA_IN, read (& ignore) a byte
	JUMP REL (set_atn_data_out), WHEN DATA_OUT	; if target is in DATA_OUT, send it a null byte
	JUMP REL (set_atn_msg_in), WHEN MSG_IN		; if target is in MSG_IN, read (& ignore) a byte
	JUMP REL (set_atn_command), WHEN CMD		; if target is in CMD, send it a null byte
	JUMP REL (set_atn_status), WHEN STATUS		; if target is in STATUS, read (& ignore) a byte
	INT 0xA2					; don't know what state target is in, barf
set_atn_data_in:
	MOVE 1, msg_buf, WHEN DATA_IN
	JUMP REL (set_atn_loop)
set_atn_data_out:
	MOVE 0 TO SFBR
	STORE SFBR, 1, msg_buf
	MOVE 1, msg_buf, WHEN DATA_OUT
	JUMP REL (set_atn_loop)
set_atn_msg_in:
	MOVE 1, msg_buf, WHEN MSG_IN
	JUMP REL (set_atn_clrack)
set_atn_command:
	MOVE 0 TO SFBR
	STORE SFBR, 1, msg_buf
	MOVE 1, msg_buf, WHEN COMMAND
	JUMP REL (set_atn_loop)
set_atn_status:
	MOVE 1, msg_buf, WHEN STATUS
	JUMP REL (set_atn_loop)
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
;  Speed and width routines
;
;  These routines take the results of negotiation messages and update the 
;  se3_scntl3 and se1_sxfer locations accordingly
;
;    scntl3 consists of the following bits:
;
;	<7> = 0 : normal de-glitching
;	      1 : ultra synchronous de-glitching
;	          this gets set only for 20MHz synchronous mode
;	<4:6> = SCF divider (sets synchronous receive rate = 80MHz/4/SCF)
;	        000 = /3
;	        001 = /1
;	        010 = /1.5
;	        011 = /2
;	        100 = /3
;	        101 = /4
;	<3> = 0 : 8-bit transfers
;	      1 : 16-bit transfers
;	<0:2> = CCF divider (sets asynchronous clock = 80MHz/CCF) (must not exceed 25MHz)
;	        101 = /4
;
;    sxfer consists of these bits:
;
;	<5:7> = TP (synchronous clock = 80MHz/SCF/TP)
;	        000 = /4
;	        001 = /5
;	        010 = /6
;	        011 = /7
;	        100 = /8
;	        101 = /9
;	        110 = /10
;	        111 = /11
;	<0:4> = req/ack offset
;	        0 = async
;
;  These routines assume an 80MHz clock (or 40MHz with doubler enabled)
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
;  Save transfer width
;
;    Input:
;
;	SFBR = as defined in scsi standard for the negotiation value
;	       0:  8-bit transfers
;	       1: 16-bit transfers
;
;    Output:
;
;	se3_scntl3 = modified
;	DSA = scsi_id_entry
;
;    Scratch:
;
;	SCRATCHA3
;
save_transfer_width:
	LOAD DSA0, 4, scsi_id_entry
	LOAD SCRATCHA3, 1, DSAREL (se3_scntl3)		; get what was in there before
	MOVE SCRATCHA3 & 0xF7 TO SCRATCHA3		; clear the 'wide' bit
	JUMP REL (save_transfer_width_ok), IF 0
	MOVE SCRATCHA3 | 0x08 TO SCRATCHA3		; ok, set it then
save_transfer_width_ok:
	STORE NOFLUSH SCRATCHA3, 1, DSAREL (se3_scntl3)
	RETURN
;
;  This is the rate that synchronous transfers will happen at
;
;    Input:
;
;	SFBR = as defined in scsi standard for the negotiation value
;	       ... 10=25ns period, 11=30.3ns, 12=50ns, 13=52nS, 14=56nS, 15=60nS, ...
;
;    Output:
;
;	se1_sxfer  = modified accordingly
;	se3_scntl3 = modified accordingly
;	SCRATCHA2  = resultant negotiation value
;	DSA = scsi_id_entry
;
;    Scratch:
;
;	SFBR, SCRATCHA1, SCRATCHA3
;
;    Note:
;
;	requested	we do	giving SCF
;	<= 50ns  (12)	20MHz	 /1 (001)
;	<=100ns  (25)	10MHz	 /2 (011)
;	else.....	 5MHz	 /4 (101)
;
save_transfer_period:
	LOAD DSA0, 4, scsi_id_entry
	LOAD SCRATCHA3, 1, DSAREL (se3_scntl3)		; get what's in se3_scntl3
	MOVE SCRATCHA3 & 0x8F TO SCRATCHA3		; clear out the SCF bits
	MOVE SCRATCHA3 | 0x50 TO SCRATCHA3		; set SCF = 101 (/4) for 5MHz rate
	MOVE 50 TO SCRATCHA2				; set up negotiation value for 5MHz
	MOVE SFBR + (0xFF - 25) TO SFBR			; sets carry iff sfbr > 25
	JUMP REL (save_transfer_period_ok), IF CARRY
	MOVE SCRATCHA3 - 0x20 TO SCRATCHA3		; set SCF = 011 (/2) for 10MHz rate
	MOVE 25 TO SCRATCHA2				; set up negotiation value for 10MHz
	MOVE SFBR + 13 TO SFBR				; sets carry iff original sfbr > 12
	JUMP REL (save_transfer_period_ok), IF CARRY
							; original SFBR <= 12 ...
	MOVE 12 TO SCRATCHA2				; set up negotiation value for 20MHz
	MOVE SCRATCHA3 - 0x20 TO SCRATCHA3		; set SCF = 101 (/4) for 5MHz rate
save_transfer_period_ok:
	STORE NOFLUSH SCRATCHA3, 1, DSAREL (se3_scntl3)
	JUMP REL (check_ultra_enable)
;
;  This is the maximum number of req's that can be sent out without having 
;  received the corresponding ack's for a synchronous transfer.
;
;  Zero means use asynchronous transfer (the default case)
;
;    Input:
;
;	SFBR = as defined in scsi standard for the negotiation value
;	       range that this chip can handle: 0..16
;
;    Output:
;
;	se1_sxfer  = modified accordingly
;	se3_scntl3 = modified accordingly
;	SCRATCHA2  = resultant negotiated value
;	DSA = points to scsi_id_table entry
;
;    Scratch:
;
;	SFBR, SCRATCHA1, SCRATCHA3
;
save_req_ack_offset:
	LOAD DSA0, 4, scsi_id_entry
	MOVE SFBR TO SCRATCHA2				; assume sfbr value is ok as is
	MOVE SFBR + (0xFF - 16) TO SFBR			; sets carry iff sfbr > 16
	JUMP REL (save_req_ack_offset_ok), IF NOT CARRY
	MOVE 16 TO SCRATCHA2				; if too big, just use 16
save_req_ack_offset_ok:
	MOVE SCRATCHA2 TO SFBR
	LOAD SCRATCHA1, 1, DSAREL (se1_sxfer)		; get what is there
	MOVE SCRATCHA1 & 0xE0 TO SCRATCHA1		; save the existing rate info
	MOVE SCRATCHA1 | SFBR TO SCRATCHA1		; put in the new offset info
	STORE NOFLUSH SCRATCHA1, 1, DSAREL (se1_sxfer)
	LOAD SCRATCHA3, 1, DSAREL (se3_scntl3)
;
;  Set the ULTRA enable bit in SCNTL3 iff SXFER indicates max speed synchronous
;
;    Input:
;
;	SCRATCHA1 = se1_sxfer contents
;	SCRATCHA3 = se3_scntl3 contents
;	DSA = scsi_id_entry
;
check_ultra_enable:
	MOVE SCRATCHA3 & 0x7F TO SCRATCHA3		; clear ultra enable
	MOVE SCRATCHA1 TO SFBR
	JUMP REL (check_ultra_enable_ok), IF 0x00 AND MASK 0xE0 ; don't bother setting it if async mode
	MOVE SCRATCHA3 TO SFBR
	JUMP REL (check_ultra_enable_ok), IF NOT 0x10 AND MASK 0x8F ; don't bother setting if not max speed
	MOVE SCRATCHA3 | 0x80 TO SCRATCHA3		; synchronous and max speed, set ultra enable bit
check_ultra_enable_ok:
	STORE SCRATCHA3, 1, DSAREL (se3_scntl3)
	RETURN
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
