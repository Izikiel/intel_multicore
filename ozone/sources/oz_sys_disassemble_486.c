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

/* Print i386 instructions for GDB, the GNU debugger.
   Copyright (C) 1988, 89, 91, 93, 94, 95, 96, 97, 1998
   Free Software Foundation, Inc.

This file is part of GDB.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  */

/*
 * 80386 instruction printer by Pace Willisson (pace@prep.ai.mit.edu)
 * July 1988
 *  modified by John Hassey (hassey@dg-rtp.dg.com)
 *
 * Adapted for OZONE, Mike Rieker, Beverly, MA, October 2001
 */

#include "ozone.h"
#include "oz_sys_disassemble.h"
#include "oz_sys_xprintf.h"

#define MAXLEN 20	/* max bytes per instruction */

typedef const char *ccharp;

typedef struct {
	int prefixes;
	char obuf[100];
	char *obufp;
	char scratchbuf[100];
	unsigned char *start_codep;
	unsigned char *codep;
	int mod;
	int rm;
	int reg;
	char op1out[100], op2out[100], op3out[100];
	int op_address[3], op_ad, op_index[3];
	int start_pc;
	int outlen;
	char *outbuf;
} Pb;

/*
 * The main tables describing the instructions is essentially a copy
 * of the "Opcode Map" chapter (Appendix A) of the Intel 80386
 * Programmers Manual.  Usually, there is a capital letter, followed
 * by a small letter.  The capital letter tell the addressing mode,
 * and the small letter tells about the operand size.  Refer to 
 * the Intel manual for details.
 */

#define FETCH_DATA(info,addr) (1)
#define PARAMS(p) p

#define PREFIX_REPZ 1
#define PREFIX_REPNZ 2
#define PREFIX_LOCK 4
#define PREFIX_CS 8
#define PREFIX_SS 0x10
#define PREFIX_DS 0x20
#define PREFIX_ES 0x40
#define PREFIX_FS 0x80
#define PREFIX_GS 0x100
#define PREFIX_DATA 0x200
#define PREFIX_ADR 0x400
#define PREFIX_FWAIT 0x800

#define Eb OP_E, b_mode
#define indirEb OP_indirE, b_mode
#define Gb OP_G, b_mode
#define Ev OP_E, v_mode
#define indirEv OP_indirE, v_mode
#define Ew OP_E, w_mode
#define Ma OP_E, v_mode
#define M OP_E, 0
#define Mp OP_E, 0		/* ? */
#define Gv OP_G, v_mode
#define Gw OP_G, w_mode
#define Rw OP_rm, w_mode
#define Rd OP_rm, d_mode
#define Ib OP_I, b_mode
#define sIb OP_sI, b_mode	/* sign extened byte */
#define Iv OP_I, v_mode
#define Iw OP_I, w_mode
#define Jb OP_J, b_mode
#define Jv OP_J, v_mode
#if 0
#define ONE OP_ONE, 0
#endif
#define Cd OP_C, d_mode
#define Dd OP_D, d_mode
#define Td OP_T, d_mode

#define eAX OP_REG, eAX_reg
#define eBX OP_REG, eBX_reg
#define eCX OP_REG, eCX_reg
#define eDX OP_REG, eDX_reg
#define eSP OP_REG, eSP_reg
#define eBP OP_REG, eBP_reg
#define eSI OP_REG, eSI_reg
#define eDI OP_REG, eDI_reg
#define AL OP_REG, al_reg
#define CL OP_REG, cl_reg
#define DL OP_REG, dl_reg
#define BL OP_REG, bl_reg
#define AH OP_REG, ah_reg
#define CH OP_REG, ch_reg
#define DH OP_REG, dh_reg
#define BH OP_REG, bh_reg
#define AX OP_REG, ax_reg
#define DX OP_REG, dx_reg
#define indirDX OP_REG, indir_dx_reg

#define Sw OP_SEG, w_mode
#define Ap OP_DIR, lptr
#define Av OP_DIR, v_mode
#define Ob OP_OFF, b_mode
#define Ov OP_OFF, v_mode
#define Xb OP_DSSI, b_mode
#define Xv OP_DSSI, v_mode
#define Yb OP_ESDI, b_mode
#define Yv OP_ESDI, v_mode

#define es OP_REG, es_reg
#define ss OP_REG, ss_reg
#define cs OP_REG, cs_reg
#define ds OP_REG, ds_reg
#define fs OP_REG, fs_reg
#define gs OP_REG, gs_reg

#define MX OP_MMX, 0
#define EM OP_EM, v_mode
#define MS OP_MS, b_mode

static void oappend (Pb *pb, const char *s);

static int OP_E (Pb *pb, int bytecode, int aflag, int dflag);
static int OP_G (Pb *pb, int bytecode, int aflag, int dflag);
static int OP_I (Pb *pb, int bytecode, int aflag, int dflag);
static int OP_indirE (Pb *pb, int bytecode, int aflag, int dflag);
static int OP_sI (Pb *pb, int bytecode, int aflag, int dflag);
static int OP_REG (Pb *pb, int bytecode, int aflag, int dflag);
static int OP_J (Pb *pb, int bytecode, int aflag, int dflag);
static int OP_DIR (Pb *pb, int bytecode, int aflag, int dflag);
static int OP_OFF (Pb *pb, int bytecode, int aflag, int dflag);
static int OP_ESDI (Pb *pb, int bytecode, int aflag, int dflag);
static int OP_DSSI (Pb *pb, int bytecode, int aflag, int dflag);
static int OP_SEG (Pb *pb, int bytecode, int aflag, int dflag);
static int OP_C (Pb *pb, int bytecode, int aflag, int dflag);
static int OP_D (Pb *pb, int bytecode, int aflag, int dflag);
static int OP_T (Pb *pb, int bytecode, int aflag, int dflag);
static int OP_rm (Pb *pb, int bytecode, int aflag, int dflag);
static int OP_ST (Pb *pb, int bytecode, int aflag, int dflag);
static int OP_STi  (Pb *pb, int bytecode, int aflag, int dflag);
#if 0
static int OP_ONE (Pb *pb, int bytecode, int aflag, int dflag);
#endif
static int OP_MMX (Pb *pb, int bytecode, int aflag, int dflag);
static int OP_EM (Pb *pb, int bytecode, int aflag, int dflag);
static int OP_MS (Pb *pb, int bytecode, int aflag, int dflag);

static void append_prefix (Pb *pb);
static void set_op (Pb *pb, int op);
static void putop (Pb *pb, const char *template, int aflag, int dflag);
static void dofloat (Pb *pb, int aflag, int dflag);
static int get16 (Pb *pb);
static int get32 (Pb *pb);
static void ckprefix (Pb *pb);
static void outputaddr (Pb *pb, void *addr);
static void outputstr (Pb *pb, char *s);

#define b_mode 1
#define v_mode 2
#define w_mode 3
#define d_mode 4

#define es_reg 100
#define cs_reg 101
#define ss_reg 102
#define ds_reg 103
#define fs_reg 104
#define gs_reg 105
#define eAX_reg 107
#define eCX_reg 108
#define eDX_reg 109
#define eBX_reg 110
#define eSP_reg 111
#define eBP_reg 112
#define eSI_reg 113
#define eDI_reg 114

#define lptr 115

#define al_reg 116
#define cl_reg 117
#define dl_reg 118
#define bl_reg 119
#define ah_reg 120
#define ch_reg 121
#define dh_reg 122
#define bh_reg 123

#define ax_reg 124
#define cx_reg 125
#define dx_reg 126
#define bx_reg 127
#define sp_reg 128
#define bp_reg 129
#define si_reg 130
#define di_reg 131

#define indir_dx_reg 150

#define GRP1b NULL, NULL, 0
#define GRP1S NULL, NULL, 1
#define GRP1Ss NULL, NULL, 2
#define GRP2b NULL, NULL, 3
#define GRP2S NULL, NULL, 4
#define GRP2b_one NULL, NULL, 5
#define GRP2S_one NULL, NULL, 6
#define GRP2b_cl NULL, NULL, 7
#define GRP2S_cl NULL, NULL, 8
#define GRP3b NULL, NULL, 9
#define GRP3S NULL, NULL, 10
#define GRP4  NULL, NULL, 11
#define GRP5  NULL, NULL, 12
#define GRP6  NULL, NULL, 13
#define GRP7 NULL, NULL, 14
#define GRP8 NULL, NULL, 15
#define GRP9 NULL, NULL, 16
#define GRP10 NULL, NULL, 17
#define GRP11 NULL, NULL, 18
#define GRP12 NULL, NULL, 19

#define FLOATCODE 50
#define FLOAT NULL, NULL, FLOATCODE

typedef int (*op_rtn) PARAMS ((Pb *pb, int bytemode, int aflag, int dflag));

struct dis386 {
  const char *name;
  op_rtn op1;
  int bytemode1;
  op_rtn op2;
  int bytemode2;
  op_rtn op3;
  int bytemode3;
};

static const struct dis386 dis386[] = {
  /* 00 */
  { "addb",	Eb, Gb },
  { "addS",	Ev, Gv },
  { "addb",	Gb, Eb },
  { "addS",	Gv, Ev },
  { "addb",	AL, Ib },
  { "addS",	eAX, Iv },
  { "pushS",	es },
  { "popS",	es },
  /* 08 */
  { "orb",	Eb, Gb },
  { "orS",	Ev, Gv },
  { "orb",	Gb, Eb },
  { "orS",	Gv, Ev },
  { "orb",	AL, Ib },
  { "orS",	eAX, Iv },
  { "pushS",	cs },
  { "(bad)" },	/* 0x0f extended opcode escape */
  /* 10 */
  { "adcb",	Eb, Gb },
  { "adcS",	Ev, Gv },
  { "adcb",	Gb, Eb },
  { "adcS",	Gv, Ev },
  { "adcb",	AL, Ib },
  { "adcS",	eAX, Iv },
  { "pushS",	ss },
  { "popS",	ss },
  /* 18 */
  { "sbbb",	Eb, Gb },
  { "sbbS",	Ev, Gv },
  { "sbbb",	Gb, Eb },
  { "sbbS",	Gv, Ev },
  { "sbbb",	AL, Ib },
  { "sbbS",	eAX, Iv },
  { "pushS",	ds },
  { "popS",	ds },
  /* 20 */
  { "andb",	Eb, Gb },
  { "andS",	Ev, Gv },
  { "andb",	Gb, Eb },
  { "andS",	Gv, Ev },
  { "andb",	AL, Ib },
  { "andS",	eAX, Iv },
  { "(bad)" },			/* SEG ES prefix */
  { "daa" },
  /* 28 */
  { "subb",	Eb, Gb },
  { "subS",	Ev, Gv },
  { "subb",	Gb, Eb },
  { "subS",	Gv, Ev },
  { "subb",	AL, Ib },
  { "subS",	eAX, Iv },
  { "(bad)" },			/* SEG CS prefix */
  { "das" },
  /* 30 */
  { "xorb",	Eb, Gb },
  { "xorS",	Ev, Gv },
  { "xorb",	Gb, Eb },
  { "xorS",	Gv, Ev },
  { "xorb",	AL, Ib },
  { "xorS",	eAX, Iv },
  { "(bad)" },			/* SEG SS prefix */
  { "aaa" },
  /* 38 */
  { "cmpb",	Eb, Gb },
  { "cmpS",	Ev, Gv },
  { "cmpb",	Gb, Eb },
  { "cmpS",	Gv, Ev },
  { "cmpb",	AL, Ib },
  { "cmpS",	eAX, Iv },
  { "(bad)" },			/* SEG DS prefix */
  { "aas" },
  /* 40 */
  { "incS",	eAX },
  { "incS",	eCX },
  { "incS",	eDX },
  { "incS",	eBX },
  { "incS",	eSP },
  { "incS",	eBP },
  { "incS",	eSI },
  { "incS",	eDI },
  /* 48 */
  { "decS",	eAX },
  { "decS",	eCX },
  { "decS",	eDX },
  { "decS",	eBX },
  { "decS",	eSP },
  { "decS",	eBP },
  { "decS",	eSI },
  { "decS",	eDI },
  /* 50 */
  { "pushS",	eAX },
  { "pushS",	eCX },
  { "pushS",	eDX },
  { "pushS",	eBX },
  { "pushS",	eSP },
  { "pushS",	eBP },
  { "pushS",	eSI },
  { "pushS",	eDI },
  /* 58 */
  { "popS",	eAX },
  { "popS",	eCX },
  { "popS",	eDX },
  { "popS",	eBX },
  { "popS",	eSP },
  { "popS",	eBP },
  { "popS",	eSI },
  { "popS",	eDI },
  /* 60 */
  { "pusha" },
  { "popa" },
  { "boundS",	Gv, Ma },
  { "arpl",	Ew, Gw },
  { "(bad)" },			/* seg fs */
  { "(bad)" },			/* seg gs */
  { "(bad)" },			/* op size prefix */
  { "(bad)" },			/* adr size prefix */
  /* 68 */
  { "pushS",	Iv },		/* 386 book wrong */
  { "imulS",	Gv, Ev, Iv },
  { "pushS",	sIb },		/* push of byte really pushes 2 or 4 bytes */
  { "imulS",	Gv, Ev, Ib },
  { "insb",	Yb, indirDX },
  { "insS",	Yv, indirDX },
  { "outsb",	indirDX, Xb },
  { "outsS",	indirDX, Xv },
  /* 70 */
  { "jo",	Jb },
  { "jno",	Jb },
  { "jb",	Jb },
  { "jae",	Jb },
  { "je",	Jb },
  { "jne",	Jb },
  { "jbe",	Jb },
  { "ja",	Jb },
  /* 78 */
  { "js",	Jb },
  { "jns",	Jb },
  { "jp",	Jb },
  { "jnp",	Jb },
  { "jl",	Jb },
  { "jnl",	Jb },
  { "jle",	Jb },
  { "jg",	Jb },
  /* 80 */
  { GRP1b },
  { GRP1S },
  { "(bad)" },
  { GRP1Ss },
  { "testb",	Eb, Gb },
  { "testS",	Ev, Gv },
  { "xchgb",	Eb, Gb },
  { "xchgS",	Ev, Gv },
  /* 88 */
  { "movb",	Eb, Gb },
  { "movS",	Ev, Gv },
  { "movb",	Gb, Eb },
  { "movS",	Gv, Ev },
  { "movS",	Ev, Sw },
  { "leaS",	Gv, M },
  { "movS",	Sw, Ev },
  { "popS",	Ev },
  /* 90 */
  { "nop" },
  { "xchgS",	eCX, eAX },
  { "xchgS",	eDX, eAX },
  { "xchgS",	eBX, eAX },
  { "xchgS",	eSP, eAX },
  { "xchgS",	eBP, eAX },
  { "xchgS",	eSI, eAX },
  { "xchgS",	eDI, eAX },
  /* 98 */
  { "cWtS" },
  { "cStd" },
  { "lcall",	Ap },
  { "(bad)" },		/* fwait */
  { "pushf" },
  { "popf" },
  { "sahf" },
  { "lahf" },
  /* a0 */
  { "movb",	AL, Ob },
  { "movS",	eAX, Ov },
  { "movb",	Ob, AL },
  { "movS",	Ov, eAX },
  { "movsb",	Yb, Xb },
  { "movsS",	Yv, Xv },
  { "cmpsb",	Yb, Xb },
  { "cmpsS",	Yv, Xv },
  /* a8 */
  { "testb",	AL, Ib },
  { "testS",	eAX, Iv },
  { "stosb",	Yb, AL },
  { "stosS",	Yv, eAX },
  { "lodsb",	AL, Xb },
  { "lodsS",	eAX, Xv },
  { "scasb",	AL, Yb },
  { "scasS",	eAX, Yv },
  /* b0 */
  { "movb",	AL, Ib },
  { "movb",	CL, Ib },
  { "movb",	DL, Ib },
  { "movb",	BL, Ib },
  { "movb",	AH, Ib },
  { "movb",	CH, Ib },
  { "movb",	DH, Ib },
  { "movb",	BH, Ib },
  /* b8 */
  { "movS",	eAX, Iv },
  { "movS",	eCX, Iv },
  { "movS",	eDX, Iv },
  { "movS",	eBX, Iv },
  { "movS",	eSP, Iv },
  { "movS",	eBP, Iv },
  { "movS",	eSI, Iv },
  { "movS",	eDI, Iv },
  /* c0 */
  { GRP2b },
  { GRP2S },
  { "ret",	Iw },
  { "ret" },
  { "lesS",	Gv, Mp },
  { "ldsS",	Gv, Mp },
  { "movb",	Eb, Ib },
  { "movS",	Ev, Iv },
  /* c8 */
  { "enter",	Iw, Ib },
  { "leave" },
  { "lret",	Iw },
  { "lret" },
  { "int3" },
  { "int",	Ib },
  { "into" },
  { "iret" },
  /* d0 */
  { GRP2b_one },
  { GRP2S_one },
  { GRP2b_cl },
  { GRP2S_cl },
  { "aam",	Ib },
  { "aad",	Ib },
  { "(bad)" },
  { "xlat" },
  /* d8 */
  { FLOAT },
  { FLOAT },
  { FLOAT },
  { FLOAT },
  { FLOAT },
  { FLOAT },
  { FLOAT },
  { FLOAT },
  /* e0 */
  { "loopne",	Jb },
  { "loope",	Jb },
  { "loop",	Jb },
  { "jCcxz",	Jb },
  { "inb",	AL, Ib },
  { "inS",	eAX, Ib },
  { "outb",	Ib, AL },
  { "outS",	Ib, eAX },
  /* e8 */
  { "call",	Av },
  { "jmp",	Jv },
  { "ljmp",	Ap },
  { "jmp",	Jb },
  { "inb",	AL, indirDX },
  { "inS",	eAX, indirDX },
  { "outb",	indirDX, AL },
  { "outS",	indirDX, eAX },
  /* f0 */
  { "(bad)" },			/* lock prefix */
  { "(bad)" },
  { "(bad)" },			/* repne */
  { "(bad)" },			/* repz */
  { "hlt" },
  { "cmc" },
  { GRP3b },
  { GRP3S },
  /* f8 */
  { "clc" },
  { "stc" },
  { "cli" },
  { "sti" },
  { "cld" },
  { "std" },
  { GRP4 },
  { GRP5 },
};

static const struct dis386 dis386_twobyte[] = {
  /* 00 */
  { GRP6 },
  { GRP7 },
  { "larS", Gv, Ew },
  { "lslS", Gv, Ew },  
  { "(bad)" },
  { "(bad)" },
  { "clts" },
  { "(bad)" },  
  /* 08 */
  { "invd" },
  { "wbinvd" },
  { "(bad)" },  { "ud2a" },  
  { "(bad)" },  { "(bad)" },  { "(bad)" },  { "(bad)" },  
  /* 10 */
  { "(bad)" },  { "(bad)" },  { "(bad)" },  { "(bad)" },  
  { "(bad)" },  { "(bad)" },  { "(bad)" },  { "(bad)" },  
  /* 18 */
  { "(bad)" },  { "(bad)" },  { "(bad)" },  { "(bad)" },  
  { "(bad)" },  { "(bad)" },  { "(bad)" },  { "(bad)" },  
  /* 20 */
  /* these are all backward in appendix A of the intel book */
  { "movl", Rd, Cd },
  { "movl", Rd, Dd },
  { "movl", Cd, Rd },
  { "movl", Dd, Rd },  
  { "movl", Rd, Td },
  { "(bad)" },
  { "movl", Td, Rd },
  { "(bad)" },  
  /* 28 */
  { "(bad)" },  { "(bad)" },  { "(bad)" },  { "(bad)" },  
  { "(bad)" },  { "(bad)" },  { "(bad)" },  { "(bad)" },  
  /* 30 */
  { "wrmsr" },     { "rdtsc" },    { "rdmsr" },  { "rdpmc" },  
  { "sysenter" },  { "sysexit" },  { "(bad)" },  { "(bad)" },  
  /* 38 */
  { "(bad)" },  { "(bad)" },  { "(bad)" },  { "(bad)" },  
  { "(bad)" },  { "(bad)" },  { "(bad)" },  { "(bad)" },  
  /* 40 */
  { "cmovo", Gv,Ev }, { "cmovno", Gv,Ev }, { "cmovb", Gv,Ev }, { "cmovae", Gv,Ev },
  { "cmove", Gv,Ev }, { "cmovne", Gv,Ev }, { "cmovbe", Gv,Ev }, { "cmova", Gv,Ev },
  /* 48 */
  { "cmovs", Gv,Ev }, { "cmovns", Gv,Ev }, { "cmovp", Gv,Ev }, { "cmovnp", Gv,Ev },
  { "cmovl", Gv,Ev }, { "cmovge", Gv,Ev }, { "cmovle", Gv,Ev }, { "cmovg", Gv,Ev },  
  /* 50 */
  { "(bad)" },  { "(bad)" },  { "(bad)" },  { "(bad)" },  
  { "(bad)" },  { "(bad)" },  { "(bad)" },  { "(bad)" },  
  /* 58 */
  { "(bad)" },  { "(bad)" },  { "(bad)" },  { "(bad)" },  
  { "(bad)" },  { "(bad)" },  { "(bad)" },  { "(bad)" },  
  /* 60 */
  { "punpcklbw", MX, EM },
  { "punpcklwd", MX, EM },
  { "punpckldq", MX, EM },
  { "packsswb", MX, EM },
  { "pcmpgtb", MX, EM },
  { "pcmpgtw", MX, EM },
  { "pcmpgtd", MX, EM },
  { "packuswb", MX, EM },
  /* 68 */
  { "punpckhbw", MX, EM },
  { "punpckhwd", MX, EM },
  { "punpckhdq", MX, EM },
  { "packssdw", MX, EM },
  { "(bad)" },  { "(bad)" },
  { "movd", MX, Ev },
  { "movq", MX, EM },
  /* 70 */
  { "(bad)" },
  { GRP10 },
  { GRP11 },
  { GRP12 },
  { "pcmpeqb", MX, EM },
  { "pcmpeqw", MX, EM },
  { "pcmpeqd", MX, EM },
  { "emms" },
  /* 78 */
  { "(bad)" },  { "(bad)" },  { "(bad)" },  { "(bad)" },  
  { "(bad)" },  { "(bad)" },
  { "movd", Ev, MX },
  { "movq", EM, MX },
  /* 80 */
  { "jo", Jv },
  { "jno", Jv },
  { "jb", Jv },
  { "jae", Jv },  
  { "je", Jv },
  { "jne", Jv },
  { "jbe", Jv },
  { "ja", Jv },  
  /* 88 */
  { "js", Jv },
  { "jns", Jv },
  { "jp", Jv },
  { "jnp", Jv },  
  { "jl", Jv },
  { "jge", Jv },
  { "jle", Jv },
  { "jg", Jv },  
  /* 90 */
  { "seto", Eb },
  { "setno", Eb },
  { "setb", Eb },
  { "setae", Eb },
  { "sete", Eb },
  { "setne", Eb },
  { "setbe", Eb },
  { "seta", Eb },
  /* 98 */
  { "sets", Eb },
  { "setns", Eb },
  { "setp", Eb },
  { "setnp", Eb },
  { "setl", Eb },
  { "setge", Eb },
  { "setle", Eb },
  { "setg", Eb },  
  /* a0 */
  { "pushS", fs },
  { "popS", fs },
  { "cpuid" },
  { "btS", Ev, Gv },  
  { "shldS", Ev, Gv, Ib },
  { "shldS", Ev, Gv, CL },
  { "(bad)" },
  { "(bad)" },  
  /* a8 */
  { "pushS", gs },
  { "popS", gs },
  { "rsm" },
  { "btsS", Ev, Gv },  
  { "shrdS", Ev, Gv, Ib },
  { "shrdS", Ev, Gv, CL },
  { "(bad)" },
  { "imulS", Gv, Ev },  
  /* b0 */
  { "cmpxchgb", Eb, Gb },
  { "cmpxchgS", Ev, Gv },
  { "lssS", Gv, Mp },	/* 386 lists only Mp */
  { "btrS", Ev, Gv },  
  { "lfsS", Gv, Mp },	/* 386 lists only Mp */
  { "lgsS", Gv, Mp },	/* 386 lists only Mp */
  { "movzbS", Gv, Eb },
  { "movzwS", Gv, Ew },  
  /* b8 */
  { "ud2b" },
  { "(bad)" },
  { GRP8 },
  { "btcS", Ev, Gv },  
  { "bsfS", Gv, Ev },
  { "bsrS", Gv, Ev },
  { "movsbS", Gv, Eb },
  { "movswS", Gv, Ew },  
  /* c0 */
  { "xaddb", Eb, Gb },
  { "xaddS", Ev, Gv },
  { "(bad)" },
  { "(bad)" },  
  { "(bad)" },
  { "(bad)" },
  { "(bad)" },
  { GRP9 },  
  /* c8 */
  { "bswap", eAX },
  { "bswap", eCX },
  { "bswap", eDX },
  { "bswap", eBX },
  { "bswap", eSP },
  { "bswap", eBP },
  { "bswap", eSI },
  { "bswap", eDI },
  /* d0 */
  { "(bad)" },
  { "psrlw", MX, EM },
  { "psrld", MX, EM },
  { "psrlq", MX, EM },
  { "(bad)" },
  { "pmullw", MX, EM },
  { "(bad)" },  { "(bad)" },  
  /* d8 */
  { "psubusb", MX, EM },
  { "psubusw", MX, EM },
  { "(bad)" },
  { "pand", MX, EM },
  { "paddusb", MX, EM },
  { "paddusw", MX, EM },
  { "(bad)" },
  { "pandn", MX, EM },
  /* e0 */
  { "(bad)" },
  { "psraw", MX, EM },
  { "psrad", MX, EM },
  { "(bad)" },
  { "(bad)" },
  { "pmulhw", MX, EM },
  { "(bad)" },  { "(bad)" },  
  /* e8 */
  { "psubsb", MX, EM },
  { "psubsw", MX, EM },
  { "(bad)" },
  { "por", MX, EM },
  { "paddsb", MX, EM },
  { "paddsw", MX, EM },
  { "(bad)" },
  { "pxor", MX, EM },
  /* f0 */
  { "(bad)" },
  { "psllw", MX, EM },
  { "pslld", MX, EM },
  { "psllq", MX, EM },
  { "(bad)" },
  { "pmaddwd", MX, EM },
  { "(bad)" },  { "(bad)" },  
  /* f8 */
  { "psubb", MX, EM },
  { "psubw", MX, EM },
  { "psubd", MX, EM },
  { "(bad)" },  
  { "paddb", MX, EM },
  { "paddw", MX, EM },
  { "paddd", MX, EM },
  { "(bad)" }
};

static const unsigned char onebyte_has_modrm[256] = {
  1,1,1,1,0,0,0,0,1,1,1,1,0,0,0,0,
  1,1,1,1,0,0,0,0,1,1,1,1,0,0,0,0,
  1,1,1,1,0,0,0,0,1,1,1,1,0,0,0,0,
  1,1,1,1,0,0,0,0,1,1,1,1,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,1,1,0,0,0,0,0,1,0,1,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  1,1,0,0,1,1,1,1,0,0,0,0,0,0,0,0,
  1,1,1,1,0,0,0,0,1,1,1,1,1,1,1,1,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,1,1,0,0,0,0,0,0,1,1
};

static const unsigned char twobyte_has_modrm[256] = {
  /* 00 */ 1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0, /* 0f */
  /* 10 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 1f */
  /* 20 */ 1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0, /* 2f */
  /* 30 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 3f */
  /* 40 */ 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, /* 4f */
  /* 50 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 5f */
  /* 60 */ 1,1,1,1,1,1,1,1,1,1,1,1,0,0,1,1, /* 6f */
  /* 70 */ 0,1,1,1,1,1,1,0,0,0,0,0,0,0,1,1, /* 7f */
  /* 80 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 8f */
  /* 90 */ 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, /* 9f */
  /* a0 */ 0,0,0,1,1,1,1,1,0,0,0,1,1,1,1,1, /* af */
  /* b0 */ 1,1,1,1,1,1,1,1,0,0,1,1,1,1,1,1, /* bf */
  /* c0 */ 1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0, /* cf */
  /* d0 */ 0,1,1,1,0,1,0,0,1,1,0,1,1,1,0,1, /* df */
  /* e0 */ 0,1,1,0,0,1,0,0,1,1,0,1,1,1,0,1, /* ef */
  /* f0 */ 0,1,1,1,0,1,0,0,1,1,1,0,1,1,1,0  /* ff */
};

static const ccharp names32[]={
  "%eax","%ecx","%edx","%ebx", "%esp","%ebp","%esi","%edi",
};
static const ccharp names16[] = {
  "%ax","%cx","%dx","%bx","%sp","%bp","%si","%di",
};
static const ccharp names8[] = {
  "%al","%cl","%dl","%bl","%ah","%ch","%dh","%bh",
};
static const ccharp names_seg[] = {
  "%es","%cs","%ss","%ds","%fs","%gs","%?","%?",
};
static const ccharp index16[] = {
  "bx+si","bx+di","bp+si","bp+di","si","di","bp","bx"
};

static const struct dis386 grps[][8] = {
  /* GRP1b */
  {
    { "addb",	Eb, Ib },
    { "orb",	Eb, Ib },
    { "adcb",	Eb, Ib },
    { "sbbb",	Eb, Ib },
    { "andb",	Eb, Ib },
    { "subb",	Eb, Ib },
    { "xorb",	Eb, Ib },
    { "cmpb",	Eb, Ib }
  },
  /* GRP1S */
  {
    { "addS",	Ev, Iv },
    { "orS",	Ev, Iv },
    { "adcS",	Ev, Iv },
    { "sbbS",	Ev, Iv },
    { "andS",	Ev, Iv },
    { "subS",	Ev, Iv },
    { "xorS",	Ev, Iv },
    { "cmpS",	Ev, Iv }
  },
  /* GRP1Ss */
  {
    { "addS",	Ev, sIb },
    { "orS",	Ev, sIb },
    { "adcS",	Ev, sIb },
    { "sbbS",	Ev, sIb },
    { "andS",	Ev, sIb },
    { "subS",	Ev, sIb },
    { "xorS",	Ev, sIb },
    { "cmpS",	Ev, sIb }
  },
  /* GRP2b */
  {
    { "rolb",	Eb, Ib },
    { "rorb",	Eb, Ib },
    { "rclb",	Eb, Ib },
    { "rcrb",	Eb, Ib },
    { "shlb",	Eb, Ib },
    { "shrb",	Eb, Ib },
    { "(bad)" },
    { "sarb",	Eb, Ib },
  },
  /* GRP2S */
  {
    { "rolS",	Ev, Ib },
    { "rorS",	Ev, Ib },
    { "rclS",	Ev, Ib },
    { "rcrS",	Ev, Ib },
    { "shlS",	Ev, Ib },
    { "shrS",	Ev, Ib },
    { "(bad)" },
    { "sarS",	Ev, Ib },
  },
  /* GRP2b_one */
  {
    { "rolb",	Eb },
    { "rorb",	Eb },
    { "rclb",	Eb },
    { "rcrb",	Eb },
    { "shlb",	Eb },
    { "shrb",	Eb },
    { "(bad)" },
    { "sarb",	Eb },
  },
  /* GRP2S_one */
  {
    { "rolS",	Ev },
    { "rorS",	Ev },
    { "rclS",	Ev },
    { "rcrS",	Ev },
    { "shlS",	Ev },
    { "shrS",	Ev },
    { "(bad)" },
    { "sarS",	Ev },
  },
  /* GRP2b_cl */
  {
    { "rolb",	Eb, CL },
    { "rorb",	Eb, CL },
    { "rclb",	Eb, CL },
    { "rcrb",	Eb, CL },
    { "shlb",	Eb, CL },
    { "shrb",	Eb, CL },
    { "(bad)" },
    { "sarb",	Eb, CL },
  },
  /* GRP2S_cl */
  {
    { "rolS",	Ev, CL },
    { "rorS",	Ev, CL },
    { "rclS",	Ev, CL },
    { "rcrS",	Ev, CL },
    { "shlS",	Ev, CL },
    { "shrS",	Ev, CL },
    { "(bad)" },
    { "sarS",	Ev, CL }
  },
  /* GRP3b */
  {
    { "testb",	Eb, Ib },
    { "(bad)",	Eb },
    { "notb",	Eb },
    { "negb",	Eb },
    { "mulb",	AL, Eb },
    { "imulb",	AL, Eb },
    { "divb",	AL, Eb },
    { "idivb",	AL, Eb }
  },
  /* GRP3S */
  {
    { "testS",	Ev, Iv },
    { "(bad)" },
    { "notS",	Ev },
    { "negS",	Ev },
    { "mulS",	eAX, Ev },
    { "imulS",	eAX, Ev },
    { "divS",	eAX, Ev },
    { "idivS",	eAX, Ev },
  },
  /* GRP4 */
  {
    { "incb", Eb },
    { "decb", Eb },
    { "(bad)" },
    { "(bad)" },
    { "(bad)" },
    { "(bad)" },
    { "(bad)" },
    { "(bad)" },
  },
  /* GRP5 */
  {
    { "incS",	Ev },
    { "decS",	Ev },
    { "call",	indirEv },
    { "lcall",	indirEv },
    { "jmp",	indirEv },
    { "ljmp",	indirEv },
    { "pushS",	Ev },
    { "(bad)" },
  },
  /* GRP6 */
  {
    { "sldt",	Ew },
    { "str",	Ew },
    { "lldt",	Ew },
    { "ltr",	Ew },
    { "verr",	Ew },
    { "verw",	Ew },
    { "(bad)" },
    { "(bad)" }
  },
  /* GRP7 */
  {
    { "sgdt", Ew },
    { "sidt", Ew },
    { "lgdt", Ew },
    { "lidt", Ew },
    { "smsw", Ew },
    { "(bad)" },
    { "lmsw", Ew },
    { "invlpg", Ew },
  },
  /* GRP8 */
  {
    { "(bad)" },
    { "(bad)" },
    { "(bad)" },
    { "(bad)" },
    { "btS",	Ev, Ib },
    { "btsS",	Ev, Ib },
    { "btrS",	Ev, Ib },
    { "btcS",	Ev, Ib },
  },
  /* GRP9 */
  {
    { "(bad)" },
    { "cmpxchg8b", Ev },
    { "(bad)" },
    { "(bad)" },
    { "(bad)" },
    { "(bad)" },
    { "(bad)" },
    { "(bad)" },
  },
  /* GRP10 */
  {
    { "(bad)" },
    { "(bad)" },
    { "psrlw", MS, Ib },
    { "(bad)" },
    { "psraw", MS, Ib },
    { "(bad)" },
    { "psllw", MS, Ib },
    { "(bad)" },
  },
  /* GRP11 */
  {
    { "(bad)" },
    { "(bad)" },
    { "psrld", MS, Ib },
    { "(bad)" },
    { "psrad", MS, Ib },
    { "(bad)" },
    { "pslld", MS, Ib },
    { "(bad)" },
  },
  /* GRP12 */
  {
    { "(bad)" },
    { "(bad)" },
    { "psrlq", MS, Ib },
    { "(bad)" },
    { "(bad)" },
    { "(bad)" },
    { "psllq", MS, Ib },
    { "(bad)" },
  }
};


static const ccharp float_mem[] = {
  /* d8 */
  "fadds",
  "fmuls",
  "fcoms",
  "fcomps",
  "fsubs",
  "fsubrs",
  "fdivs",
  "fdivrs",
  /*  d9 */
  "flds",
  "(bad)",
  "fsts",
  "fstps",
  "fldenv",
  "fldcw",
  "fNstenv",
  "fNstcw",
  /* da */
  "fiaddl",
  "fimull",
  "ficoml",
  "ficompl",
  "fisubl",
  "fisubrl",
  "fidivl",
  "fidivrl",
  /* db */
  "fildl",
  "(bad)",
  "fistl",
  "fistpl",
  "(bad)",
  "fldt",
  "(bad)",
  "fstpt",
  /* dc */
  "faddl",
  "fmull",
  "fcoml",
  "fcompl",
  "fsubl",
  "fsubrl",
  "fdivl",
  "fdivrl",
  /* dd */
  "fldl",
  "(bad)",
  "fstl",
  "fstpl",
  "frstor",
  "(bad)",
  "fNsave",
  "fNstsw",
  /* de */
  "fiadd",
  "fimul",
  "ficom",
  "ficomp",
  "fisub",
  "fisubr",
  "fidiv",
  "fidivr",
  /* df */
  "fild",
  "(bad)",
  "fist",
  "fistp",
  "fbld",
  "fildll",
  "fbstp",
  "fistpll",
};

#define ST OP_ST, 0
#define STi OP_STi, 0

#define FGRPd9_2 NULL, NULL, 0
#define FGRPd9_4 NULL, NULL, 1
#define FGRPd9_5 NULL, NULL, 2
#define FGRPd9_6 NULL, NULL, 3
#define FGRPd9_7 NULL, NULL, 4
#define FGRPda_5 NULL, NULL, 5
#define FGRPdb_4 NULL, NULL, 6
#define FGRPde_3 NULL, NULL, 7
#define FGRPdf_4 NULL, NULL, 8

static const struct dis386 float_reg[][8] = {
  /* d8 */
  {
    { "fadd",	ST, STi },
    { "fmul",	ST, STi },
    { "fcom",	STi },
    { "fcomp",	STi },
    { "fsub",	ST, STi },
    { "fsubr",	ST, STi },
    { "fdiv",	ST, STi },
    { "fdivr",	ST, STi },
  },
  /* d9 */
  {
    { "fld",	STi },
    { "fxch",	STi },
    { FGRPd9_2 },
    { "(bad)" },
    { FGRPd9_4 },
    { FGRPd9_5 },
    { FGRPd9_6 },
    { FGRPd9_7 },
  },
  /* da */
  {
    { "fcmovb",	ST, STi },
    { "fcmove",	ST, STi },
    { "fcmovbe",ST, STi },
    { "fcmovu",	ST, STi },
    { "(bad)" },
    { FGRPda_5 },
    { "(bad)" },
    { "(bad)" },
  },
  /* db */
  {
    { "fcmovnb",ST, STi },
    { "fcmovne",ST, STi },
    { "fcmovnbe",ST, STi },
    { "fcmovnu",ST, STi },
    { FGRPdb_4 },
    { "fucomi",	ST, STi },
    { "fcomi",	ST, STi },
    { "(bad)" },
  },
  /* dc */
  {
    { "fadd",	STi, ST },
    { "fmul",	STi, ST },
    { "(bad)" },
    { "(bad)" },
    { "fsub",	STi, ST },
    { "fsubr",	STi, ST },
    { "fdiv",	STi, ST },
    { "fdivr",	STi, ST },
  },
  /* dd */
  {
    { "ffree",	STi },
    { "(bad)" },
    { "fst",	STi },
    { "fstp",	STi },
    { "fucom",	STi },
    { "fucomp",	STi },
    { "(bad)" },
    { "(bad)" },
  },
  /* de */
  {
    { "faddp",	STi, ST },
    { "fmulp",	STi, ST },
    { "(bad)" },
    { FGRPde_3 },
    { "fsubp",	STi, ST },
    { "fsubrp",	STi, ST },
    { "fdivp",	STi, ST },
    { "fdivrp",	STi, ST },
  },
  /* df */
  {
    { "(bad)" },
    { "(bad)" },
    { "(bad)" },
    { "(bad)" },
    { FGRPdf_4 },
    { "fucomip",ST, STi },
    { "fcomip", ST, STi },
    { "(bad)" },
  },
};


static const ccharp fgrps[][8] = {
  /* d9_2  0 */
  {
    "fnop","(bad)","(bad)","(bad)","(bad)","(bad)","(bad)","(bad)",
  },

  /* d9_4  1 */
  {
    "fchs","fabs","(bad)","(bad)","ftst","fxam","(bad)","(bad)",
  },

  /* d9_5  2 */
  {
    "fld1","fldl2t","fldl2e","fldpi","fldlg2","fldln2","fldz","(bad)",
  },

  /* d9_6  3 */
  {
    "f2xm1","fyl2x","fptan","fpatan","fxtract","fprem1","fdecstp","fincstp",
  },

  /* d9_7  4 */
  {
    "fprem","fyl2xp1","fsqrt","fsincos","frndint","fscale","fsin","fcos",
  },

  /* da_5  5 */
  {
    "(bad)","fucompp","(bad)","(bad)","(bad)","(bad)","(bad)","(bad)",
  },

  /* db_4  6 */
  {
    "feni(287 only)","fdisi(287 only)","fNclex","fNinit",
    "fNsetpm(287 only)","(bad)","(bad)","(bad)",
  },

  /* de_3  7 */
  {
    "(bad)","fcompp","(bad)","(bad)","(bad)","(bad)","(bad)","(bad)",
  },

  /* df_4  8 */
  {
    "fNstsw","(bad)","(bad)","(bad)","(bad)","(bad)","(bad)","(bad)",
  },
};

/************************************************************************/
/*									*/
/*  Disassemble an instruction						*/
/*									*/
/*    Input:								*/
/*									*/
/*	il = max length of bytes pointed to by ob			*/
/*	ib = pointer to instruction buffer that actually holds the bytes*/
/*	pc = address of this instruction, it will be used to print the 	*/
/*	     target address if this is a relative jump or call		*/
/*	ol = output buffer length					*/
/*	ob = output buffer address					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_disassemble = number of bytes for instruction		*/
/*	*ob = filled in (null terminated string)			*/
/*	*into = -1 : steps into but clears Tracing flag			*/
/*	         0 : doesn't step into anything				*/
/*	         1 : steps into, leaves Tracing flag alone		*/
/*									*/
/************************************************************************/

/*
 *   On the 386's of 1988, the maximum length of an instruction is 15 bytes.
 *   (see topic "Redundant prefixes" in the "Differences from 8086"
 *   section of the "Virtual 8086 Mode" chapter.)
 * 'pc' should be the address of this instruction, it will
 *   be used to print the target address if this is a relative jump or call
 */

int oz_sys_disassemble (int il, const uByte *ib, const uByte *pc, int ol, char *ob, int *into)

{
  uByte the_buffer[MAXLEN], need_modrm, opcode;
  char *first, *second, *third;
  int aflag, dflag, i;
  int enter_instruction;
  int needcomma;
  Pb pb;
  const struct dis386 *dp;

  aflag = 1;						/* assume 32-bit operands to start */
  dflag = 1;

  movc4 (il, ib, sizeof the_buffer, the_buffer);	/* copy instruction opcodes to internal buffer */

  pb.outlen = ol;					/* set up output buffer descriptor */
  pb.outbuf = ob;
  *pb.outbuf = 0;

  pb.obuf[0] = 0;
  pb.op1out[0] = 0;
  pb.op2out[0] = 0;
  pb.op3out[0] = 0;

  pb.op_index[0] = pb.op_index[1] = pb.op_index[2] = -1;

  pb.start_pc = (int)pc;
  pb.start_codep = the_buffer;
  pb.codep = the_buffer;

  pb.obufp = pb.obuf;
  
  ckprefix (&pb);

  FETCH_DATA (info, pb.codep + 1);
  opcode = *(pb.codep);
  enter_instruction = (opcode == 0xc8);

  if (into != NULL) {
    *into = (opcode == 0xe8) || (opcode == 0x9a) || ((opcode == 0xff) && ((pb.codep[1] & 0x30) == 0x10));
  }
  
  if (pb.prefixes & PREFIX_REPZ)  oappend (&pb, "repz ");
  if (pb.prefixes & PREFIX_REPNZ) oappend (&pb, "repnz ");
  if (pb.prefixes & PREFIX_LOCK)  oappend (&pb, "lock ");
  
  if ((pb.prefixes & PREFIX_FWAIT) && ((*(pb.codep) < 0xd8) || (*(pb.codep) > 0xdf)))
    {
      /* fwait not followed by floating point instruction */
      outputstr (&pb, "fwait");
      return (1);
    }
  
  if (pb.prefixes & PREFIX_DATA) dflag ^= 1;
  
  if (pb.prefixes & PREFIX_ADR)
    {
      aflag ^= 1;
      if (aflag) oappend (&pb, "addr32 ");
      else oappend (&pb, "addr16 ");
    }
  
  if (*(pb.codep) == 0x0f)
    {
      FETCH_DATA (info, pb.codep + 2);
      dp = &dis386_twobyte[*++(pb.codep)];
      need_modrm = twobyte_has_modrm[*(pb.codep)];
    }
  else
    {
      dp = &dis386[*(pb.codep)];
      need_modrm = onebyte_has_modrm[*(pb.codep)];
    }
  pb.codep++;

  if (need_modrm)
    {
      FETCH_DATA (info, pb.codep + 1);
      pb.mod = (*(pb.codep) >> 6) & 3;
      pb.reg = (*(pb.codep) >> 3) & 7;
      pb.rm = *(pb.codep) & 7;
    }

  if (dp->name == NULL && dp->bytemode1 == FLOATCODE)
    {
      dofloat (&pb, aflag, dflag);
    }
  else
    {
      if (dp->name == NULL) dp = &grps[dp->bytemode1][pb.reg];
      
      putop (&pb, dp->name, aflag, dflag);
      
      pb.obufp = pb.op1out;
      pb.op_ad = 2;
      if (dp->op1)
	(*dp->op1)(&pb, dp->bytemode1, aflag, dflag);
      
      pb.obufp = pb.op2out;
      pb.op_ad = 1;
      if (dp->op2)
	(*dp->op2)(&pb, dp->bytemode2, aflag, dflag);
      
      pb.obufp = pb.op3out;
      pb.op_ad = 0;
      if (dp->op3)
	(*dp->op3)(&pb, dp->bytemode3, aflag, dflag);
    }
  
  pb.obufp = pb.obuf + strlen (pb.obuf);
  for (i = strlen (pb.obuf); i < 6; i++) oappend (&pb, " ");
  oappend (&pb, " ");

  outputstr (&pb, pb.obuf);
  
  /* enter instruction is printed with operands in the
   * same order as the intel book; everything else
   * is printed in reverse order 
   */
  if (enter_instruction)
    {
      first = pb.op1out;
      second = pb.op2out;
      third = pb.op3out;
      pb.op_ad = pb.op_index[0];
      pb.op_index[0] = pb.op_index[2];
      pb.op_index[2] = pb.op_ad;
    }
  else
    {
      first = pb.op3out;
      second = pb.op2out;
      third = pb.op1out;
    }
  needcomma = 0;
  if (*first)
    {
      if (pb.op_index[0] != -1) outputaddr (&pb, (void *)(pb.op_address[pb.op_index[0]]));
      else outputstr (&pb, first);
      needcomma = 1;
    }
  if (*second)
    {
      if (needcomma) outputstr (&pb, ",");
      if (pb.op_index[1] != -1) outputaddr (&pb, (void *)(pb.op_address[pb.op_index[1]]));
      else outputstr (&pb, second);
      needcomma = 1;
    }
  if (*third)
    {
      if (needcomma) outputstr (&pb, ",");
      if (pb.op_index[2] != -1) outputaddr (&pb, (void *)(pb.op_address[pb.op_index[2]]));
      else outputstr (&pb, third);
    }
  return (pb.codep - the_buffer);
}

/************************************************************************/
/*									*/
/*  Process prefix opcodes off the front of the instruction		*/
/*									*/
/*    Input:								*/
/*									*/
/*	codep = points to opcode buffer					*/
/*									*/
/*    Output:								*/
/*									*/
/*	prefixes = mask of prefixes found				*/
/*	codep = incremented past the prefixes				*/
/*									*/
/************************************************************************/

static void ckprefix (Pb *pb)

{
  pb -> prefixes = 0;				/* assume no prefixes */
  while (1)					/* repeat forever */
    {
      FETCH_DATA (the_info, pb -> codep + 1);	/* make sure we have an opcode */
      switch (*(pb -> codep))
	{
	case 0xf3:
	  pb -> prefixes |= PREFIX_REPZ;
	  break;
	case 0xf2:
	  pb -> prefixes |= PREFIX_REPNZ;
	  break;
	case 0xf0:
	  pb -> prefixes |= PREFIX_LOCK;
	  break;
	case 0x2e:
	  pb -> prefixes |= PREFIX_CS;
	  break;
	case 0x36:
	  pb -> prefixes |= PREFIX_SS;
	  break;
	case 0x3e:
	  pb -> prefixes |= PREFIX_DS;
	  break;
	case 0x26:
	  pb -> prefixes |= PREFIX_ES;
	  break;
	case 0x64:
	  pb -> prefixes |= PREFIX_FS;
	  break;
	case 0x65:
	  pb -> prefixes |= PREFIX_GS;
	  break;
	case 0x66:
	  pb -> prefixes |= PREFIX_DATA;
	  break;
	case 0x67:
	  pb -> prefixes |= PREFIX_ADR;
	  break;
	case 0x9b:
	  pb -> prefixes |= PREFIX_FWAIT;
	  break;
	default:				/* unknown, it's not a prefix so return with codep pointing to opcode */
	  return;
	}
      pb -> codep++;				/* it was a known prefix, increment and look for more prefixes */
    }
}

static void dofloat (Pb *pb, int aflag, int dflag)

{
  const struct dis386 *dp;
  unsigned char floatop;
  
  floatop = pb -> codep[-1];
  
  if (pb -> mod != 3)
    {
      putop (pb, float_mem[(floatop - 0xd8) * 8 + pb -> reg], aflag, dflag);
      pb -> obufp = pb -> op1out;
      OP_E (pb, v_mode, aflag, dflag);
      return;
    }
  pb -> codep++;
  
  dp = &float_reg[floatop - 0xd8][pb->reg];
  if (dp->name == NULL)
    {
      putop (pb, fgrps[dp->bytemode1][pb->rm], aflag, dflag);
      /* instruction fnstsw is only one with strange arg */
      if (floatop == 0xdf
	  && FETCH_DATA (the_info, pb -> codep + 1)
	  && *(pb -> codep) == 0xe0)
	strcpy (pb -> op1out, "%eax");
    }
  else
    {
      putop (pb, dp->name, aflag, dflag);
      pb -> obufp = pb -> op1out;
      if (dp->op1)
	(*dp->op1)(pb, dp->bytemode1, aflag, dflag);
      pb -> obufp = pb -> op2out;
      if (dp->op2)
	(*dp->op2)(pb, dp->bytemode2, aflag, dflag);
    }
}

static int OP_ST (Pb *pb, int ignore, int aflag, int dflag)

{
  oappend (pb, "%st");
  return (0);
}

static int OP_STi (Pb *pb, int ignore, int aflag, int dflag)

{
  oz_sys_sprintf (sizeof pb -> scratchbuf, pb -> scratchbuf, "%%st(%d)", pb -> rm);
  oappend (pb, pb -> scratchbuf);
  return (0);
}

/* capital letters in template are macros */

static void putop (Pb *pb, const char *template, int aflag, int dflag)

{
  const char *p;
  
  for (p = template; *p; p++)
    {
      switch (*p)
	{
	default:
	  *(pb -> obufp)++ = *p;
	  break;
	case 'C':		/* For jcxz/jecxz */
	  if (aflag)
	    *(pb -> obufp)++ = 'e';
	  break;
	case 'N':
	  if ((pb -> prefixes & PREFIX_FWAIT) == 0)
	    *(pb -> obufp)++ = 'n';
	  break;
	case 'S':
	  /* operand size flag */
	  if (dflag)
	    *(pb -> obufp)++ = 'l';
	  else
	    *(pb -> obufp)++ = 'w';
	  break;
	case 'W':
	  /* operand size flag for cwtl, cbtw */
	  if (dflag)
	    *(pb -> obufp)++ = 'w';
	  else
	    *(pb -> obufp)++ = 'b';
	  break;
	}
    }
  *(pb -> obufp) = 0;
}

static void oappend (Pb *pb, const char *s)

{
  strcpy (pb -> obufp, s);
  pb -> obufp += strlen (s);
  *(pb -> obufp) = 0;
}

static void append_prefix (Pb *pb)

{
  if (pb -> prefixes & PREFIX_CS) oappend (pb, "%cs:");
  if (pb -> prefixes & PREFIX_DS) oappend (pb, "%ds:");
  if (pb -> prefixes & PREFIX_SS) oappend (pb, "%ss:");
  if (pb -> prefixes & PREFIX_ES) oappend (pb, "%es:");
  if (pb -> prefixes & PREFIX_FS) oappend (pb, "%fs:");
  if (pb -> prefixes & PREFIX_GS) oappend (pb, "%gs:");
}

static int OP_indirE (Pb *pb, int bytemode, int aflag, int dflag)

{
  oappend (pb, "*");
  return OP_E (pb, bytemode, aflag, dflag);
}

static int OP_E (Pb *pb, int bytemode, int aflag, int dflag)

{
  int disp;

  /* skip mod/rm byte */
  pb -> codep++;

  if (pb -> mod == 3)
    {
      switch (bytemode)
	{
	case b_mode:
	  oappend (pb, names8[pb->rm]);
	  break;
	case w_mode:
	  oappend (pb, names16[pb->rm]);
	  break;
	case v_mode:
	  if (dflag)
	    oappend (pb, names32[pb->rm]);
	  else
	    oappend (pb, names16[pb->rm]);
	  break;
	default:
	  oappend (pb, "<bad dis table>");
	  break;
	}
      return 0;
    }

  disp = 0;
  append_prefix (pb);

  if (aflag) /* 32 bit address mode */
    {
      int havesib;
      int havebase;
      int base;
      int index = 0;
      int scale = 0;

      havesib = 0;
      havebase = 1;
      base = pb -> rm;

      if (base == 4)
	{
	  havesib = 1;
	  FETCH_DATA (the_info, pb -> codep + 1);
	  scale = (*(pb -> codep) >> 6) & 3;
	  index = (*(pb -> codep) >> 3) & 7;
	  base = *(pb -> codep) & 7;
	  pb -> codep++;
	}

      switch (pb -> mod)
	{
	case 0:
	  if (base == 5)
	    {
	      havebase = 0;
	      disp = get32 (pb);
	    }
	  break;
	case 1:
	  FETCH_DATA (the_info, pb -> codep + 1);
	  disp = *(pb -> codep)++;
	  if ((disp & 0x80) != 0)
	    disp -= 0x100;
	  break;
	case 2:
	  disp = get32 (pb);
	  break;
	}

      if (pb -> mod != 0 || base == 5)
	{
	  oz_sys_sprintf (sizeof pb -> scratchbuf, pb -> scratchbuf, "0x%x", disp);
	  oappend (pb, pb -> scratchbuf);
	}

      if (havebase || (havesib && (index != 4 || scale != 0)))
	{
	  oappend (pb, "(");
	  if (havebase)
	    oappend (pb, names32[base]);
	  if (havesib)
	    {
	      if (index != 4)
		{
		  oz_sys_sprintf (sizeof pb -> scratchbuf, pb -> scratchbuf, ",%s", names32[index]);
		  oappend (pb, pb -> scratchbuf);
		}
	      oz_sys_sprintf (sizeof pb -> scratchbuf, pb -> scratchbuf, ",%d", 1 << scale);
	      oappend (pb, pb -> scratchbuf);
	    }
	  oappend (pb, ")");
	}
    }
  else
    { /* 16 bit address mode */
      switch (pb -> mod)
	{
	case 0:
	  if (pb -> rm == 6)
	    {
	      disp = get16 (pb);
	      if ((disp & 0x8000) != 0)
		disp -= 0x10000;
	    }
	  break;
	case 1:
	  FETCH_DATA (the_info, pb -> codep + 1);
	  disp = *(pb -> codep)++;
	  if ((disp & 0x80) != 0)
	    disp -= 0x100;
	  break;
	case 2:
	  disp = get16 (pb);
	  if ((disp & 0x8000) != 0)
	    disp -= 0x10000;
	  break;
	}

      if (pb -> mod != 0 || pb -> rm == 6)
	{
	  oz_sys_sprintf (sizeof pb -> scratchbuf, pb -> scratchbuf, "0x%x", disp);
	  oappend (pb, pb -> scratchbuf);
	}

      if (pb -> mod != 0 || pb -> rm != 6)
	{
	  oappend (pb, "(");
	  oappend (pb, index16[pb -> rm]);
	  oappend (pb, ")");
	}
    }
  return 0;
}

static int OP_G (Pb *pb, int bytemode, int aflag, int dflag)

{
  switch (bytemode) 
    {
    case b_mode:
      oappend (pb, names8[pb->reg]);
      break;
    case w_mode:
      oappend (pb, names16[pb->reg]);
      break;
    case d_mode:
      oappend (pb, names32[pb->reg]);
      break;
    case v_mode:
      if (dflag)
	oappend (pb, names32[pb->reg]);
      else
	oappend (pb, names16[pb->reg]);
      break;
    default:
      oappend (pb, "<internal disassembler error>");
      break;
    }
  return (0);
}

static int get32 (Pb *pb)

{
  int x = 0;

  FETCH_DATA (the_info, pb -> codep + 4);
  x = *(pb -> codep)++ & 0xff;
  x |= (*(pb -> codep)++ & 0xff) << 8;
  x |= (*(pb -> codep)++ & 0xff) << 16;
  x |= (*(pb -> codep)++ & 0xff) << 24;
  return (x);
}

static int get16 (Pb *pb)

{
  int x = 0;

  FETCH_DATA (the_info, pb -> codep + 2);
  x = *(pb -> codep)++ & 0xff;
  x |= (*(pb -> codep)++ & 0xff) << 8;
  return (x);
}

static void set_op (Pb *pb, int op)

{
  pb -> op_index[pb->op_ad] = pb -> op_ad;
  pb -> op_address[pb->op_ad] = op;
}

static int OP_REG (Pb *pb, int code, int aflag, int dflag)

{
  const char *s;
  
  switch (code) 
    {
    case indir_dx_reg: s = "(%dx)"; break;
	case ax_reg: case cx_reg: case dx_reg: case bx_reg:
	case sp_reg: case bp_reg: case si_reg: case di_reg:
		s = names16[code - ax_reg];
		break;
	case es_reg: case ss_reg: case cs_reg:
	case ds_reg: case fs_reg: case gs_reg:
		s = names_seg[code - es_reg];
		break;
	case al_reg: case ah_reg: case cl_reg: case ch_reg:
	case dl_reg: case dh_reg: case bl_reg: case bh_reg:
		s = names8[code - al_reg];
		break;
	case eAX_reg: case eCX_reg: case eDX_reg: case eBX_reg:
	case eSP_reg: case eBP_reg: case eSI_reg: case eDI_reg:
      if (dflag)
	s = names32[code - eAX_reg];
      else
	s = names16[code - eAX_reg];
      break;
    default:
      s = "<internal disassembler error>";
      break;
    }
  oappend (pb, s);
  return (0);
}

static int OP_I (Pb *pb, int bytemode, int aflag, int dflag)

{
  int op;
  
  switch (bytemode) 
    {
    case b_mode:
      FETCH_DATA (the_info, pb -> codep + 1);
      op = *(pb -> codep)++ & 0xff;
      break;
    case v_mode:
      if (dflag)
	op = get32 (pb);
      else
	op = get16 (pb);
      break;
    case w_mode:
      op = get16 (pb);
      break;
    default:
      oappend (pb, "<internal disassembler error>");
      return (0);
    }
  oz_sys_sprintf (sizeof pb -> scratchbuf, pb -> scratchbuf, "$0x%x", op);
  oappend (pb, pb -> scratchbuf);
  return (0);
}

static int OP_sI (Pb *pb, int bytemode, int aflag, int dflag)

{
  int op;
  
  switch (bytemode) 
    {
    case b_mode:
      FETCH_DATA (the_info, pb -> codep + 1);
      op = *(pb -> codep)++;
      if ((op & 0x80) != 0)
	op -= 0x100;
      break;
    case v_mode:
      if (dflag)
	op = get32 (pb);
      else
	{
	  op = get16 (pb);
	  if ((op & 0x8000) != 0)
	    op -= 0x10000;
	}
      break;
    case w_mode:
      op = get16 (pb);
      if ((op & 0x8000) != 0)
	op -= 0x10000;
      break;
    default:
      oappend (pb, "<internal disassembler error>");
      return (0);
    }
  oz_sys_sprintf (sizeof pb -> scratchbuf, pb -> scratchbuf, "$0x%x", op);
  oappend (pb, pb -> scratchbuf);
  return (0);
}

static int OP_J (Pb *pb, int bytemode, int aflag, int dflag)

{
  int disp;
  int mask = -1;
  
  switch (bytemode) 
    {
    case b_mode:
      FETCH_DATA (the_info, pb -> codep + 1);
      disp = *(pb -> codep)++;
      if ((disp & 0x80) != 0)
	disp -= 0x100;
      break;
    case v_mode:
      if (dflag)
	disp = get32 (pb);
      else
	{
	  disp = get16 (pb);
	  if ((disp & 0x8000) != 0)
	    disp -= 0x10000;
	  /* for some reason, a data16 prefix on a jump instruction
	     means that the pc is masked to 16 bits after the
	     displacement is added!  */
	  mask = 0xffff;
	}
      break;
    default:
      oappend (pb, "<internal disassembler error>");
      return (0);
    }
  disp = (pb -> start_pc + pb -> codep - pb -> start_codep + disp) & mask;
  set_op (pb, disp);
  oz_sys_sprintf (sizeof pb -> scratchbuf, pb -> scratchbuf, "0x%x", disp);
  oappend (pb, pb -> scratchbuf);
  return (0);
}

static int OP_SEG (Pb *pb, int dummy, int aflag, int dflag)

{
  static const ccharp sreg[] = {
    "%es","%cs","%ss","%ds","%fs","%gs","%?","%?",
  };

  oappend (pb, sreg[pb->reg]);
  return (0);
}

static int OP_DIR (Pb *pb, int size, int aflag, int dflag)

{
  int seg, offset;
  
  switch (size) 
    {
    case lptr:
      if (aflag) 
	{
	  offset = get32 (pb);
	  seg = get16 (pb);
	} 
      else 
	{
	  offset = get16 (pb);
	  seg = get16 (pb);
	}
      oz_sys_sprintf (sizeof pb -> scratchbuf, pb -> scratchbuf, "0x%x,0x%x", seg, offset);
      oappend (pb, pb -> scratchbuf);
      break;
    case v_mode:
      if (aflag)
	offset = get32 (pb);
      else
	{
	  offset = get16 (pb);
	  if ((offset & 0x8000) != 0)
	    offset -= 0x10000;
	}
      
      offset = pb -> start_pc + pb -> codep - pb -> start_codep + offset;
      set_op (pb, offset);
      oz_sys_sprintf (sizeof pb -> scratchbuf, pb -> scratchbuf, "0x%x", offset);
      oappend (pb, pb -> scratchbuf);
      break;
    default:
      oappend (pb, "<internal disassembler error>");
      break;
    }
  return (0);
}

static int OP_OFF (Pb *pb, int bytemode, int aflag, int dflag)

{
  int off;

  append_prefix (pb);

  if (aflag)
    off = get32 (pb);
  else
    off = get16 (pb);
  
  oz_sys_sprintf (sizeof pb -> scratchbuf, pb -> scratchbuf, "0x%x", off);
  oappend (pb, pb -> scratchbuf);
  return (0);
}

static int OP_ESDI (Pb *pb, int dummy, int aflag, int dflag)

{
  oappend (pb, "%es:(");
  oappend (pb, aflag ? "%edi" : "%di");
  oappend (pb, ")");
  return (0);
}

static int OP_DSSI (Pb *pb, int dummy, int aflag, int dflag)

{
  if ((pb -> prefixes
       & (PREFIX_CS
	  | PREFIX_DS
	  | PREFIX_SS
	  | PREFIX_ES
	  | PREFIX_FS
	  | PREFIX_GS)) == 0)
    pb -> prefixes |= PREFIX_DS;
  append_prefix (pb);
  oappend (pb, "(");
  oappend (pb, aflag ? "%esi" : "%si");
  oappend (pb, ")");
  return (0);
}

#if 0
/* Not used.  */

static int OP_ONE (Pb *pb, int dummy, int aflag, int dflag)

{
  oappend (pb, "1");
  return (0);
}

#endif

static int OP_C (Pb *pb, int dummy, int aflag, int dflag)

{
  pb -> codep++; /* skip mod/rm */
  oz_sys_sprintf (sizeof pb -> scratchbuf, pb -> scratchbuf, "%%cr%d", pb -> reg);
  oappend (pb, pb -> scratchbuf);
  return (0);
}

static int OP_D (Pb *pb, int dummy, int aflag, int dflag)

{
  pb -> codep++; /* skip mod/rm */
  oz_sys_sprintf (sizeof pb -> scratchbuf, pb -> scratchbuf, "%%db%d", pb -> reg);
  oappend (pb, pb -> scratchbuf);
  return (0);
}

static int OP_T (Pb *pb, int dummy, int aflag, int dflag)

{
  pb -> codep++; /* skip mod/rm */
  oz_sys_sprintf (sizeof pb -> scratchbuf, pb -> scratchbuf, "%%tr%d", pb -> reg);
  oappend (pb, pb -> scratchbuf);
  return (0);
}

static int OP_rm (Pb *pb, int bytemode, int aflag, int dflag)

{
  switch (bytemode) 
    {
    case d_mode:
      oappend (pb, names32[pb->rm]);
      break;
    case w_mode:
      oappend (pb, names16[pb->rm]);
      break;
    }
  return (0);
}

static int OP_MMX (Pb *pb, int bytemode, int aflag, int dflag)

{
  oz_sys_sprintf (sizeof pb -> scratchbuf, pb -> scratchbuf, "%%mm%d", pb -> reg);
  oappend (pb, pb -> scratchbuf);
  return 0;
}

static int OP_EM (Pb *pb, int bytemode, int aflag, int dflag)

{
  if (pb -> mod != 3) return OP_E (pb, bytemode, aflag, dflag);

  pb -> codep++;
  oz_sys_sprintf (sizeof pb -> scratchbuf, pb -> scratchbuf, "%%mm%d", pb -> rm);
  oappend (pb, pb -> scratchbuf);
  return 0;
}

static int OP_MS (Pb *pb, int bytemode, int aflag, int dflag)

{
  ++(pb -> codep);
  oz_sys_sprintf (sizeof pb -> scratchbuf, pb -> scratchbuf, "%%mm%d", pb -> rm);
  oappend (pb, pb -> scratchbuf);
  return 0;
}

/************************************************************************/
/*									*/
/*  Output an address string						*/
/*									*/
/************************************************************************/

static void outputaddr (Pb *pb, void *addr)

{
  char s[32];

  oz_sys_sprintf (sizeof s, s, "%p", addr);	/* format the string */
  outputstr (pb, s);				/* output it */
}

/************************************************************************/
/*									*/
/*  Output a null terminated string					*/
/*									*/
/************************************************************************/

static void outputstr (Pb *pb, char *s)

{
  int l;

  l = strlen (s);				/* get length of string */
  if (l >= pb -> outlen) l = pb -> outlen - 1;	/* if too long, truncate */
  memcpy (pb -> outbuf, s, l);			/* copy the string */
  pb -> outlen -= l;				/* decrement remaining output buffer length */
  pb -> outbuf += l;				/* increment remaining output buffer pointer */
  *(pb -> outbuf) = 0;				/* null terminate output string */
}
