##+++2003-11-18
##    Copyright (C) 2001,2002,2003  Mike Rieker, Beverly, MA USA
##
##    This program is free software; you can redistribute it and/or modify
##    it under the terms of the GNU General Public License as published by
##    the Free Software Foundation; version 2 of the License.
##
##    This program is distributed in the hope that it will be useful,
##    but WITHOUT ANY WARRANTY; without even the implied warranty of
##    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
##    GNU General Public License for more details.
##
##    You should have received a copy of the GNU General Public License
##    along with this program; if not, write to the Free Software
##    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
##---2003-11-18

##########################################################################
##									##
##  Time-of-day routines						##
##									##
##########################################################################

	.include "oz_params_486.s"

	TIMER_APORT = 0x70
	TIMER_DPORT = 0x71

	TIMER_SECONDS    = 0x00
	TIMER_MINUTES    = 0x02
	TIMER_HOURS      = 0x04
	TIMER_DAYOFWEEK  = 0x06
	TIMER_DAYOFMONTH = 0x07
	TIMER_MONTH      = 0x08
	TIMER_YEAR       = 0x09
	TIMER_STATUS_A   = 0x0A
	TIMER_STATUS_B   = 0x0B
	TIMER_STATUS_C   = 0x0C
	TIMER_STATUS_D   = 0x0D
	TIMER_CENTURY    = 0x32

	.data
	.align	8
	.globl	oz_hw486_rdtscpersec
	.type	oz_hw486_rdtscpersec,@object
basetime:		.long	0,0		# the boottime (actually, the time when RDTSC was zero)
oz_hw486_rdtscpersec:	.long	0		# number of rdtsc ticks per second
todlock:		.long	0		#    0: unlocked
						#   -1: locked for write
						# else: locked for read

	.text

	# These macros must be called with hw interrupt delivery 
	# ... inhibitedas they do not allow for nested calls

.macro	TODLOCKREAD
todlockread_loop1_\@:
	movl	todlock,%eax
todlockread_loop2_\@:
	movl	%eax,%ecx
	incl	%ecx
	je	todlockread_loop1_\@
	lock
	cmpxchgl %ecx,todlock
	jne	todlockread_loop2_\@
	.endm

.macro	TODUNLKREAD
	lock
	decl	todlock
	.endm

.macro	TODLOCKWRITE
todlockwrite_loop1_\@:
	movl	todlock,%eax
todlockwrite_loop2_\@:
	movl	%eax,%ecx
	subl	$1,%ecx
	jnc	todlockwrite_loop1_\@
	lock
	cmpxchgl %ecx,todlock
	jne	todlockwrite_loop2_\@
	.endm

.macro	TODUNLKWRITE
	lock
	incl	todlock
	.endm

##########################################################################
##									##
##  Time-of-day initialization routine					##
##									##
##########################################################################

tod_init_msg0:	.string	"oz_hw486_tod_init: reading cmos clock...\n"
tod_init_msg1: 	.ascii	"oz_hw486_tod_init: cpu freq %u Hz\n"
		.ascii	"oz_hw486_tod_init: boot time %##t UTC\n"
		.string	"oz_hw486_tod_init: base time %##t UTC\n"
tod_init_toerr:	.string	"oz_hw486_tod_init: timed out waiting for beginning of second\n"

	.align	4
	.globl	oz_hw486_tod_init
	.type	oz_hw486_tod_init,@function
oz_hw486_tod_init:
	pushl	$tod_init_msg0
	call	oz_knl_printk
	addl	$4,%esp

## Wait for timer update cycle to end

	movl	oz_hw486_ldr_clock_rate_ptr,%eax # see if paramter block states CPU frequency
	movl	(%eax),%ecx
	jecxz	tod_init_measure
	movl	%ecx,oz_hw486_rdtscpersec	# if so, save it where we can access it
	call	tod_init_begsec			# wait for beginning of RTC second
	movl	%eax,basetime+0			# save the current rdtsc timestamp
	movl	%edx,basetime+4
	jmp	tod_init_gotrate
tod_init_measure:
	call	tod_init_begsec
	movl	%eax,%ebx			# save low order timestamp in %ebx
	call	tod_init_begsec
						# now we have almost a whole second to read the clock

	movl	%eax,basetime+0			# save rdtsc at beginning of second we calculate time for
	movl	%edx,basetime+4

	subl	%ebx,%eax			# calculate number of rdtsc ticks per second
	movl	%eax,oz_hw486_rdtscpersec
tod_init_gotrate:

## Read current date/time out of non-volitile ram and set boot date/time accordingly

	movl	$TIMER_CENTURY,%eax		# get current century
	outb	$TIMER_APORT
	IODELAY
	inb	$TIMER_DPORT
	IODELAY
	call	bcdtobin
	movl	$100,%edx			# ... times 100
	mull	%edx
	movl	%eax,%ebx			# ... into ebx
	movl	$TIMER_YEAR,%eax		# get current year in century
	outb	$TIMER_APORT
	IODELAY
	inb	$TIMER_DPORT
	IODELAY
	call	bcdtobin
	addl	%eax,%ebx			# add to century*100
	shll	$16,%ebx			# shift into bits <16:23>
	movl	$TIMER_MONTH,%eax		# get current month in year
	outb	$TIMER_APORT
	IODELAY
	inb	$TIMER_DPORT
	IODELAY
	call	bcdtobin
	movb	%al,%bh				# ... into bits <08:15>
	movl	$TIMER_DAYOFMONTH,%eax		# get current day in month
	outb	$TIMER_APORT
	IODELAY
	inb	$TIMER_DPORT
	IODELAY
	call	bcdtobin
	movb	%al,%bl				# ... into bits <00:07>
	pushl	%ebx				# push on stack
	call	oz_sys_daynumber_encode		# encode it into day since epoch
	movl	%eax,(%esp)			# put that on the stack
	movl	$TIMER_HOURS,%eax		# get hour of the day
	outb	$TIMER_APORT
	IODELAY
	inb	$TIMER_DPORT
	IODELAY
	call	bcdtobin
	movl	$3600,%edx
	mull	%edx				# scale it to seconds
	movl	%eax,%ebx
	movl	$TIMER_MINUTES,%eax		# get minute of the hour
	outb	$TIMER_APORT
	IODELAY
	inb	$TIMER_DPORT
	IODELAY
	call	bcdtobin
	movl	$60,%edx
	mull	%edx				# scale it to seconds
	addl	%eax,%ebx			# add to scaled hours
	movl	$TIMER_SECONDS,%eax		# get second of the minute
	outb	$TIMER_APORT
	IODELAY
	inb	$TIMER_DPORT
	IODELAY
	call	bcdtobin
	addl	%eax,%ebx			# add to scaled minutes and hours
	pushl	%ebx				# push second of the day
	pushl	$0				# push fraction of second
	pushl	%esp				# push pointer to 3 longwords
	call	oz_sys_datebin_encode		# encode into quadword
	addl	$16,%esp			# pop pointer and 3 longwords
	movl	%eax,oz_s_boottime+0
	movl	%edx,oz_s_boottime+4

## Add rtc timezone offset from boottime

	call	oz_hw_getrtcoffset		# get tz_offset_rtc into eax
	imull	oz_TIMER_RESOLUTION		# convert from seconds to 100nS
	subl	%eax,oz_s_boottime+0		# convert boottime from RTC timezone to UTC
	sbbl	%edx,oz_s_boottime+4

## Calculate basetime = what time it was when the RDTSC was zero

	movl	basetime+0,%eax			# get what RDTSC was when boottime was calculated
	movl	basetime+4,%edx
	call	timer_scale			# convert it to 100nS ticks in %ecx:%eax

	movl	oz_s_boottime+0,%ebx		# subtract from boottime
	movl	oz_s_boottime+4,%edx

	subl	%eax,%ebx			# ... to get the basetime
	sbbl	%ecx,%edx

	movl	%ebx,basetime+0
	movl	%edx,basetime+4

## Output message

	pushl	basetime+4
	pushl	basetime+0
	pushl	oz_s_boottime+4
	pushl	oz_s_boottime+0
	pushl	oz_hw486_rdtscpersec
	pushl	$tod_init_msg1
	call	oz_knl_printk
	addl	$24,%esp

	call	oz_hw_tod_getnow

	ret

# Read timestamp counter at beginning of a second according to the RTC
# Preserve %ebx, %ecx is scratch, output in %edx:%eax

	.align	4
tod_init_begsec:
	movl	$10000000,%ecx			# only do this so many times
	call	tod_init_readsec		# read second into %dl
	movb	%dl,%dh				# save it in %dh
tod_init_begsec_loop:
	call	tod_init_readsec		# read second into %dl
	cmpb	%dl,%dh				# see if different
	loopz	tod_init_begsec_loop		# repeat if same
	jz	tod_init_begsec_to		# took too long, barf
	rdtsc					# read timestamp counter at beginning of second
	ret

tod_init_begsec_to2:
	addl	$4,%esp
tod_init_begsec_to:
	pushl	$tod_init_toerr
	call	oz_knl_printk
	addl	$4,%esp
	rdtsc
	ret

tod_init_readsec:
	movb	$TIMER_STATUS_A,%al		# point to the status port
	outb	$TIMER_APORT
	IODELAY
	inb	$TIMER_DPORT			# read the status port
	IODELAY
	testb	$0x80,%al
	loopnz	tod_init_readsec		# loop until <7> is clear
	jnz	tod_init_begsec_to2
	movb	$TIMER_SECONDS,%al		# point to the seconds port
	outb	$TIMER_APORT
	IODELAY
	inb	$TIMER_DPORT			# read the number of seconds
	IODELAY
	movb	%al,%dl				# save in %dl
	movb	$TIMER_STATUS_A,%al		# point to the status port again
	outb	$TIMER_APORT
	IODELAY
	inb	$TIMER_DPORT			# read the status port
	IODELAY
	testb	$0x80,%al
	loopnz	tod_init_readsec		# loop until <7> is clear
	jnz	tod_init_begsec_to2
	ret					# status<7> was clear both before and after, so %dl is valid

# Convert bcd in %al to binary in %al
# %cl is scratch, %ah is cleared

	.align	4
bcdtobin:
	movb	%al,%cl		# save original in cl
	shrb	$4,%al		# get top 4 bits in lower 4 bits of al
	movb	$10,%ah		# multiply them by 10
	mulb	%ah
	andb	$15,%cl		# mask to get original low 4 bits
	addb	%cl,%al		# add original low 4 bits back in
	ret			# done

##########################################################################
##									##
##  Get current date/time quadword					##
##									##
##    Output:								##
##									##
##	%edx:%eax = current date/time quadword				##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_tod_getnow
	.type	oz_hw_tod_getnow,@function
oz_hw_tod_getnow:
	pushl	%ebp			# make call frame
	movl	%esp,%ebp
	pushl	%edi			# save C registers
	pushl	%esi
	pushl	%ebx

	int	$INT_GETNOW		# do most of the work in kernel mode

	popl	%ebx			# restore C registers
	popl	%esi
	popl	%edi
	popl	%ebp			# pop call frame
	ret

## This exception handler runs with interrupts disabled and gets the current date/time in edx:eax
## It is also called from the timer interrupt handler

	.align	4
	.globl	oz_hw_except_getnow
	.type	oz_hw_except_getnow,@function
oz_hw_except_getnow:
	cmpl	$0,oz_hw486_rdtscpersec	# make sure this is set up first
	jz	timer_getnow_null	# ... so we don't get div-by-zero crash

	# Get what time this cpu thinks it is by doing an rdtsc.  
	# The rdtsc is adjusted so that this never returns a 
	# value .lt. last time it was called, even across cpu's.

	call	oz_hw_tod_iotanow

	# %edx:%eax = number of rdtsc ticks since basetime

	call	oz_hw486_aiota2sys
	iret

	# Some wise-ass is calling us before timing params are set up

timer_getnow_null:
	xorl	%eax,%eax
	xorl	%edx,%edx
	iret

# Convert absolute iota to absolute system time
# Hardware interrupt delivery must be inhibited
#   Input: %edx:%eax = rdtsc since basetime
#  Output: %edx:%eax = 100nS absolute time
# Scratch: %ebx,%ecx

	.align	4
	.globl	oz_hw486_aiota2sys
	.type	oz_hw486_aiota2sys,@function
oz_hw486_aiota2sys:

	# %edx:%eax = number of rdtsc ticks since basetime

	# Block basetime & rdtscpersec modifications

	pushl	%eax
	TODLOCKREAD
	popl	%eax

	# Scale iotas to system

	call	timer_scale

	# %ecx:%eax = 100nS since basetime

	# Calculate current time by adding basetime

	movl	%ecx,%edx
	addl	basetime+0,%eax
	adcl	basetime+4,%edx

	# Allow basetime & rdtscpersec modifications

	pushl	%eax
	TODUNLKREAD
	popl	%eax

	ret

# Convert absolute system to absolute iota time
# Hardware interrupt delivery must be inhibited

	.align	4
	.globl	oz_hw486_asys2iota
	.type	oz_hw486_asys2iota,@function
oz_hw486_asys2iota:

	# %edx:%eax = system time

	# Block basetime & rdtscpersec modifications

	pushl	%eax
	TODLOCKREAD
	popl	%eax

	# Subtract off the base time

	subl	basetime+0,%eax
	sbbl	basetime+4,%edx
	jc	asys2iota_underflow

	# Scale system to iotas

	call	timer_unscale

	# %ecx:%eax = rdtsc value, 0xFFFFFFFF:FFFFFFFF if overflow

	# Allow basetime & rdtscpersec modifications

	pushl	%eax
	TODUNLKREAD
	popl	%eax
	movl	%ecx,%edx

	ret

asys2iota_underflow:
	TODUNLKREAD
	xorl	%eax,%eax
	xorl	%edx,%edx
	ret

##########################################################################
##									##
##  Set the current date/time						##
##									##
##    Input:								##
##									##
##	  8:4(%esp) = new datebin					##
##	16:12(%esp) = old datebin					##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_tod_setnow
	.type	oz_hw_tod_setnow,@function
oz_hw_tod_setnow:
	pushl	%ebp				# make call frame
	movl	%esp,%ebp
	pushl	%edi
	pushl	%esi
	pushl	%ebx
	pushfl					# inhibit hardware interrupt delivery
	cli					# ... for TODLOCKWRITE macro

	TODLOCKWRITE				# block out all readers

	#  8(%ebp) = new time
	# 16(%ebp) = old time

	movl	basetime+0,%eax			# read current basetime into %edx:%eax
	movl	basetime+4,%edx

	subl	16(%ebp),%eax			# calc new basetime = old basetime + newtime - oldtime
	sbbl	20(%ebp),%edx
	addl	 8(%ebp),%eax
	adcl	12(%ebp),%edx

	movl	%eax,basetime+0
	movl	%edx,basetime+4

	TODUNLKWRITE

	popfl					# restore interrupt delivery

## Inhibit softint delivery to avoid switching CPU

	pushl	$0
	call	oz_hw_cpu_setsoftint
	movl	%eax,(%esp)

## Write new date/time to RTC

	movb	$TIMER_STATUS_A,%al		# point to the status port
	outb	$TIMER_APORT
	IODELAY
	movb	$0x60,%al			# stop the RTC from updating
	outb	$TIMER_DPORT
	IODELAY

setnowloop1sthalf:
	call	setnowhalfsec			# see what half second we are in
	testb	$1,%al
	jne	setnowloop1sthalf		# repeat if in 2nd half
setnowloop2ndhalf:
	call	setnowhalfsec			# see what half second we are in
	testb	$1,%al
	je	setnowloop2ndhalf		# repeat if still in 1st half

						# the 2nd half of current second has just begun

	movb	$0x20,%al			# re-enable RTC updating, next update in 500mS
	outb	$TIMER_DPORT
	IODELAY

	call	oz_hw_getrtcoffset		# get tz_offset_rtc into eax (signed)
	imull	oz_TIMER_RESOLUTION		# convert from seconds to 100nS (signed)
	addl	%esi,%eax			# add in new date/time
	adcl	%edi,%edx

	movl	$60*10000000,%ecx
	divl	%ecx				# %eax = minute number
	pushl	%edx				# %edx = fraction of a minute (0..599999999)
	xorl	%edx,%edx
	movl	$60*24,%ecx
	divl	%ecx				# %eax = daynumber
	pushl	%edx				# %edx = minute within day (0..1439)
	pushl	%eax				# convert daynumber to yyyymmdd
	call	oz_sys_daynumber_decode
	movl	%eax,(%esp)

	# 0(%esp).byte = day in month (1..31)
	# 1(%esp).byte = month in year (1..12)
	# 2(%esp).word = year (2000..9999)
	# 4(%esp).long = minute within day (0..1439)
	# 8(%esp).long = fraction of a minute (0..599999999)

	movb	$TIMER_CENTURY,%al		# set current century
	outb	$TIMER_APORT
	IODELAY
	movw	2(%esp),%ax			# - get year
	movb	$100,%cl
	divb	%cl				# - %al=century, %ah=year-in-century
	movb	%ah,2(%esp)
	call	bintobcd			# - convert binary century to bcd
	outb	$TIMER_DPORT			# - write to RTC's ram
	IODELAY
	movb	$TIMER_YEAR,%al			# set current year in century
	outb	$TIMER_APORT
	IODELAY
	movb	2(%esp),%al			# - get year-in-century
	call	bintobcd			# - convert binary year to bcd
	outb	$TIMER_DPORT			# - write to RTC's ram
	IODELAY
	movb	$TIMER_MONTH,%al		# set current month in year
	outb	$TIMER_APORT
	IODELAY
	movb	1(%esp),%al
	call	bintobcd
	outb	$TIMER_DPORT
	IODELAY
	movb	$TIMER_DAYOFMONTH,%al		# set current day in month
	outb	$TIMER_APORT
	IODELAY
	movb	(%esp),%al
	call	bintobcd
	outb	$TIMER_DPORT
	IODELAY
	movb	$TIMER_HOURS,%al		# set hour of the day
	outb	$TIMER_APORT
	IODELAY
	movw	4(%esp),%ax			# - get minute within day
	movb	$60,%cl
	divb	%cl				# - %al=hour, %ah=minute
	movb	%ah,4(%esp)
	call	bintobcd			# - convert binary hour to bcd
	outb	$TIMER_DPORT			# - write to RTC's ram
	IODELAY
	movb	$TIMER_MINUTES,%al		# set minute of the hour
	outb	$TIMER_APORT
	IODELAY
	movb	4(%esp),%al
	call	bintobcd
	outb	$TIMER_DPORT
	IODELAY
	movb	$TIMER_SECONDS,%al		# set second of the minute
	outb	$TIMER_APORT
	IODELAY
	movl	8(%esp),%eax			# - get 100nS within minute
	xorl	%edx,%edx
	divl	oz_TIMER_RESOLUTION		# - %eax=second, %edx=100nS of second
	call	bintobcd			# - convert binary second to bcd
	outb	$TIMER_DPORT			# - write to RTC's ram
	IODELAY

	addl	$12,%esp			# wipe temp stuff from stack

## Restore softint delivery

	call	oz_hw_cpu_setsoftint
	addl	$4,%esp

## Return

	popl	%ebx				# pop call frame
	popl	%esi
	popl	%edi
	popl	%ebp
	ret

# See what half second we are in
# Output: %eax=even: 1st half, odd: 2nd half, %edi/%esi=current datetime
# Scratch: %ebx,%ecx,%edx

setnowhalfsec:
	call	oz_hw_tod_iotanow		# get current time
	call	oz_hw486_aiota2sys		# ... in 100nS units
	movl	%eax,%esi			# save current date/time
	movl	%edx,%edi
	movl	oz_TIMER_RESOLUTION,%ecx	# get 5,000,000 in %ecx
	shrl	$1,%ecx
	movl	%edx,%eax			# divide high-order current time
	xorl	%edx,%edx
	divl	%ecx				# ... by 5,000,000
	movl	%esi,%eax			# divide low-order current time
	divl	%ecx				# ... by 5,000,000
	ret					# quotient in %eax; even=1st half, odd=2nd half

# Convert binary number in %al (range 00..99) into equivalent bcd (range 0x00..0x99)
# Scratch: ah, cl

bintobcd:
	xorb	%ah,%ah				# divide %al by 10
	movb	$10,%cl
	divb	%cl				# %al=quotient, %ah=remainder
	shlb	$4,%al				# quotient is most significant digit
	orb	%ah,%al				# remainder is least significant digit
	ret

##########################################################################
##									##
##  Get current timer rate						##
##									##
##    Output:								##
##									##
##	%eax = timer rate						##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_tod_getrate
	.type	oz_hw_tod_getrate,@function
oz_hw_tod_getrate:
	movl	oz_hw486_rdtscpersec,%eax
	ret

##########################################################################
##									##
##  Set timer rate							##
##									##
##    Input:								##
##									##
##	4(%esp) = new timer rate					##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_tod_setrate
	.type	oz_hw_tod_setrate,@function
oz_hw_tod_setrate:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%edi
	pushl	%esi
	pushfl
	cli

	TODLOCKWRITE

	# We also need to adjust basetime so the current time remains the same
	# basetime += oldscale(RDTDC) - newscale(RDTSC)

	call	oz_hw_tod_iotanow			# get current RDTSC value
	pushl	%edx
	pushl	%eax
	call	timer_scale				# scale it using the old rate
	movl	%eax,%esi				# save scaled result
	movl	%ecx,%edi
	movl	8(%ebp),%ecx				# scale it using the new rate
	movl	oz_hw486_rdtscpersec,%eax		# don't allow too drastic a change
	shrl	$1,%eax
	cmpl	%eax,%ecx
	jc	setrate_crash1
	shll	$2,%eax
	cmpl	%ecx,%eax
	jc	setrate_crash1
	popl	%eax
	popl	%edx
	movl	%ecx,oz_hw486_rdtscpersec
	call	timer_scale
	subl	%eax,%esi				# this is how much to add to basetime
	sbbl	%ecx,%edi
	addl	%esi,basetime+0				# calc new basetime
	adcl	%edi,basetime+4

	TODUNLKWRITE

	popfl

	movl	oz_hw486_rdtscpersec,%eax		# get the new rate
	movl	oz_hw486_ldr_clock_rate_ptr,%edx	# store in loader params block
	movl	%eax,(%edx)

	popl	%esi
	popl	%edi
	popl	%ebp
	ret

setrate_crash1:
	pushl	%ecx
	pushl	oz_hw486_rdtscpersec
	pushl	$setrate_msg1
	call	oz_crash

setrate_msg1:	.asciz	"oz_hw_tod_setrate: old rate %u, new rate %u"

##########################################################################
##									##
##  Inter-cpu safe rdtsc routine					##
##  It never returns a decreasing value					##
##									##
##    Input:								##
##									##
##	kernel mode, .ge. softint so cpu doesn't switch on us		##
##									##
##    Output:								##
##									##
##	%edx:%eax = rdtsc value, adjusted				##
##									##
##########################################################################

	.data

		.align	8
lastrdtsc:	.long	0,0			# last value returned by any cpu

	.text
	.align	4
	.globl	oz_hw_tod_iotanow
	.type	oz_hw_tod_iotanow,@function
oz_hw_tod_iotanow:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%esi
	pushl	%ebx

	call	oz_hw486_getcpu			# point to this cpu's data struct

	movl	%eax,%ebx			# atomically sample lastrdtsc into %eax:%edx
	movl	%edx,%ecx
	lock
	cmpxchg8b lastrdtsc

rdtsc_sampled:
	movl	%eax,%ebx			# save sample from RDTSC instruction
	movl	%edx,%ecx

	pushfl					# don't let an interrupt flake out RDTSCBIAS fixup
	cli

	rdtsc					# see what this cpu's RDTSC is
	addl	CPU_Q_RDTSCBIAS+0(%esi),%eax	# offset by any previously computed bias
	adcl	CPU_Q_RDTSCBIAS+4(%esi),%edx

	xchgl	%eax,%ebx			# set %edx:%eax = sample
	xchgl	%edx,%ecx			# and %ecx:%ebx = reading

	subl	%eax,%ebx			# compute %ecx:%ebx = reading - sample
	sbbl	%edx,%ecx

	jc	rdtsc_recal			# recalibrate if reading .lt. sample
						# ... we never return a decreasing value

	popfl

	# reading .ge. sample
	# so we can return reading directly
	# ... after setting lastrdtsc

	addl	%eax,%ebx			# restore %ecx:%ebx = reading
	adcl	%edx,%ecx

	lock					# hopefully, no other cpu has mussed lastrdtsc
	cmpxchg8b lastrdtsc
	jne	rdtsc_sampled			# ... or we start all over

	movl	%ebx,%eax			# ok, return reading in %eax:%edx
	movl	%ecx,%edx

	popl	%ebx				# restore scratch and return
	popl	%esi
	popl	%ebp
	ret

	# reading was .lt. sample so this cpu is behind the others
	# so we add the difference to this cpu's bias to catch it up
	# we return the sampled lastrdtsc value (still in %edx:%eax)

rdtsc_recal:
	subl	%ebx,CPU_Q_RDTSCBIAS+0(%esi)	# increment bias so it will come out right next time
	sbbl	%ecx,CPU_Q_RDTSCBIAS+4(%esi)

	popfl					# allow interrupt routine to call me now

	popl	%ebx				# restore scratch and return
	popl	%esi
	popl	%ebp
	ret

##########################################################################
##									##
##  Convert a delta iota to a delta system time				##
##									##
##    Input:								##
##									##
##	4(%esp) = delta iota time					##
##									##
##    Output:								##
##									##
##	%edx:%eax = delta system time					##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_tod_diota2sys
	.type	oz_hw_tod_diota2sys,@function
oz_hw_tod_diota2sys:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%ebx

	movl	 8(%ebp),%eax
	movl	12(%ebp),%edx
	call	timer_scale
	movl	%ecx,%edx

	popl	%ebx
	popl	%ebp
	ret

##########################################################################
##									##
##  Scale number of rdtsc ticks in %edx:%eax to 100nS in %ecx:%eax	##
##    Scratch is %ebx, %edx						##
##    Preserves %esi, %edi						##
##									##
##########################################################################

	.align	4
timer_scale:
	movl	%edx,%ecx		# save high order ticks in %ecx
	mull	oz_TIMER_RESOLUTION	# multiply low order ticks by timer resolution
	pushl	%edx			# save high order result on stack for now
	movl	%eax,%ebx		# save low order result in %ebx
	movl	%ecx,%eax		# get high order ticks back
	mull	oz_TIMER_RESOLUTION	# multiply high order ticks by timer resolution
	popl	%ecx			# restore intermediate order result
	addl	%ecx,%eax		# add in other intermediate order result
	adcl	$0,%edx			# add any carry to high order result

	## %edx:%eax:%ebx = ticks_since_basetime * timer_resolution_per_second

	divl	oz_hw486_rdtscpersec	# divide high:middle order by ticks_per_second
	movl	%eax,%ecx		# save quotient in %ecx as high order result
	movl	%ebx,%eax		# divide middle:low order by ticks_per_second
	divl	oz_hw486_rdtscpersec	# quotient in %eax is low order result
	ret

##########################################################################
##									##
##  Convert a delta system to a delta iota time				##
##									##
##    Input:								##
##									##
##	4(%esp) = delta system time					##
##									##
##    Output:								##
##									##
##	%edx:%eax = delta iota time					##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_tod_dsys2iota
	.type	oz_hw_tod_dsys2iota,@function
oz_hw_tod_dsys2iota:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%ebx

	movl	 8(%ebp),%eax
	movl	12(%ebp),%edx

	call	timer_unscale

	movl	%ecx,%edx

	popl	%ebx
	popl	%ebp
	ret

	.align	4
timer_unscale:
	movl	%edx,%ecx		# save high order ticks in %ecx
	mull	oz_hw486_rdtscpersec	# multiply low order ticks by timer resolution
	pushl	%edx			# save high order result on stack for now
	movl	%eax,%ebx		# save low order result in %ebx
	movl	%ecx,%eax		# get high order ticks back
	mull	oz_hw486_rdtscpersec	# multiply high order ticks by timer resolution
	popl	%ecx			# restore intermediate order result
	addl	%ecx,%eax		# add in other intermediate order result
	adcl	$0,%edx			# add any carry to high order result

	cmpl	%edx,oz_TIMER_RESOLUTION
	jbe	timer_unscale_overflow

	divl	oz_TIMER_RESOLUTION	# divide high:middle order by ticks_per_second
	movl	%eax,%ecx		# save quotient in %ecx as high order result
	movl	%ebx,%eax		# divide middle:low order by ticks_per_second
	divl	oz_TIMER_RESOLUTION	# quotient in %eax is low order result

	ret

timer_unscale_overflow:
	movl	$-1,%eax
	movl	$-1,%ecx
	ret

##########################################################################
##									##
##  Software timing loop delay						##
##									##
##    Input:								##
##									##
##	 4(%esp) = delay (in microseconds)				##
##	 8(%esp) = callback entrypoint (or 0 if none)			##
##	12(%esp) = callback parameter					##
##									##
##    Output:								##
##									##
##	%eax = 0 : delay completed					##
##	    else : return value from callback routine			##
##									##
##########################################################################

	.align	4
	.global	oz_hw_stl_microwait
	.type	oz_hw_stl_microwait,@function
oz_hw_stl_microwait:
	pushl	%ebp		# make call frame
	movl	%esp,%ebp
	movl	8(%ebp),%eax	# get number of microseconds to wait
	movl	$1000000,%ecx
	mull	oz_hw486_rdtscpersec # multiply by timer resolution in ticks per second
				# %edx = high order product; %eax = low order product
	pushl	%eax		# save lower order result on stack
	movl	%edx,%eax
	xorl	%edx,%edx
				# %edx = zero; %eax = high order product; (%esp) = low order product
	divl	%ecx
				# %eax = high order quotient; %edx = high order remainder; (%esp) = low order product
	xchgl	%eax,(%esp)	# save high order quotient, get low order product
				# %edx = high order remainder; %eax = low order product; (%esp) = high order quotient
	divl	%ecx		# divide low order in eax (and high order remainder in edx) by 1000000
				# (%esp) = high order quotient; %eax = low order quotient
	movl	%eax,%ecx	# save low order quotient
	rdtsc			# get current timestamp
	addl	%eax,%ecx	# compute when to stop
	adcl	%edx,(%esp)
	pushl	%ecx
	movl	12(%ebp),%eax	# see if they specified a callback
	testl	%eax,%eax
	jne	stl_delay_loop
	movl	$stl_delay_dummy,12(%ebp)
stl_delay_loop:
	movl	12(%ebp),%eax	# get callback entrypoint
	pushl	16(%ebp)	# push the parameter
	call	*%eax		# call it
	addl	$4,%esp		# pop parameter
	testl	%eax,%eax	# see if abort the loop
	jne	stl_delay_done	# abort if non-zero
stl_delay_next:
	rdtsc			# read current timestamp
	subl	(%esp),%eax	# subtract the stop time
	sbbl	4(%esp),%edx
	jc	stl_delay_loop	# repeat if stop time > current time
	xorl	%eax,%eax	# reached stop time, return zero
stl_delay_done:
	leave
	ret

	.align	4
stl_delay_dummy:
	xorl	%eax,%eax
	ret

##########################################################################
##									##
##  Software timing loop delay in nanoseconds				##
##									##
##    Input:								##
##									##
##	 4(%esp) = delay (in nanoseconds)				##
##									##
##########################################################################

	.align	4
	.global	oz_hw_stl_nanowait
	.type	oz_hw_stl_nanowait,@function
oz_hw_stl_nanowait:
	rdtsc				# get current timestamp
					# - this is where we start timing from
	pushl	%ebp			# make call frame
	movl	%esp,%ebp
	pushl	%ebx			# save C register
	pushl	%edx			# save start time
	pushl	%eax
	movl	8(%ebp),%eax		# get number of nanoseconds to wait
	movl	$1000000000,%ecx
	mull	oz_hw486_rdtscpersec	# multiply by timer resolution in ticks per second
					# %edx = high order product; %eax = low order product
	movl	%eax,%ebx		# save lower order result in %ebx
	movl	%edx,%eax
	xorl	%edx,%edx
					# %edx = zero; %eax = high order product; %ebx = low order product
	divl	%ecx
					# %eax = high order quotient; %edx = high order remainder; %ebx = low order product
	xchgl	%eax,%ebx		# save high order quotient, get low order product
					# %edx = high order remainder; %eax = low order product; %ebx = high order quotient
	divl	%ecx			# divide low order in eax (and high order remainder in edx) by 1000000000
					# %ebx = high order quotient; %eax = low order quotient
	movl	%eax,%ecx		# save low order quotient
	popl	%eax			# get start time
	popl	%edx
	addl	%eax,%ecx		# compute when to stop
	adcl	%edx,%ebx
stl_nanowait_loop:
	rdtsc				# read current timestamp
	subl	%ecx,%eax		# subtract the stop time
	sbbl	%ebx,%edx
	jc	stl_nanowait_loop	# repeat if stop time > current time
	popl	%ebx			# restore C register
	popl	%ebp
	ret
