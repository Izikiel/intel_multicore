##+++2004-01-03
##    Copyright (C) 2001,2002,2003,2004  Mike Rieker, Beverly, MA USA
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
##---2004-01-03

##########################################################################
##									##
##  Division routines - the Alpha does not have a divide instruction	##
##                      so we do division in these routines		##
##									##
##  It does 3.31 multipies per call, on average				##
##									##
##  This routine runs about 23% faster on average than DEC's routine, 	##
##  taking an average of 154 cycles vs DEC's 198 (at least on a 600au)	##
##									##
##  GCC/GAS calls them thusly:						##
##									##
##    Input:								##
##									##
##	R23 = return address						##
##	R24 = dividend							##
##	R25 = divisor							##
##	R27 = entrypoint address					##
##									##
##    Output:								##
##									##
##	R27 = result (quotient or remainder)				##
##	R28 = scratch							##
##	all other registers, including R23,R24,R25, are preserved	##
##									##
##########################################################################

	.set	noat
	.set	nomacro

	.globl	__divl
	.globl	__divlu
	.globl	__divq
	.globl	__divqu
	.globl	__reml
	.globl	__remlu
	.globl	__remq
	.globl	__remqu
	.type	__divl,@function
	.type	__divlu,@function
	.type	__divq,@function
	.type	__divqu,@function
	.type	__reml,@function
	.type	__remlu,@function
	.type	__remq,@function
	.type	__remqu,@function

	PAL_bugcheck = 129
	PAL_gentrap  = 170
	GEN_INTDIV   = -2

	.text

# Table is set up with the top bit missing, ie, add 0x10000000000000000 to each entry

# For powers of 2, just use shifting to divide

# For cases where there are .le. NMULTBITS of precision in the divisor:
#  Entry [0x000] is for dividing by 0x201, 
#   entry 0x0FF is for dividing by 0x300 
#   entry 0x1FF is for dividing by 0x400, etc.
#  For example, to divide by 3:
#   1) normalize divisor 3 in range 0x201..0x400 to get 0x300
#   2) take entry 0x0FF=5555.5555.5555.5555 (3FF.FFFF.FFFF.FFFF.FFFF/300-1.0000.0000.0000.0000)
#   3) umulh dividend by 1.5555.5555.5555.5556 to get quotient
#   4) shift resultant quotient right 1 place
#   5) since original divisor 3's top bit is bit #1, shift right by 1 addional place
#      if original divisor was 6, then we'd shift right by 2 additional places instead

# For cases where there are .gt. NMULTBITS of precision in the divisor:
#  Entry [0x000] is for dividing by 0x200,
#   entry 0x0FF is for dividing by 0x2FF
#   entry 0x1FF is for dividing by 0x3FF, etc.
#  For example, to divide by 0x3001:
#   1) normalize divisor 0x3001 in range 0x200..0x3FF to get 0x300
#   2) take entry 0x200=54E3.B419.4CE6.5DE0 (3FF.FFFF.FFFF.FFFF.FFFF/301-1.0000.0000.0000.0000)
#   3) put the 1 back on the top and shift the entry right 1 place = AA71.DA0C.A673.2EF0
#   4) guesstimate = dividend umulh AA71.DA0C.A673.2EF0
#      we know we can't overestimate the true quotient because we really 
#      used the entry for 0x301, which we know is .gt. the true divisor
#      thus guesstimate will always be on the low side
#   5) shift guesstimate right by 13 (because original divisor 0x3001's top bit is #13)
#   6) add guesstimate to accumulated quotient
#   7) subtract guesstimate*divisor from dividend, repeat back to 4 if still .ge. divisor

	NMULTBITS = 10

	.p2align 6	# should keep it out of I-cache lines
multipliers:
 .quad  0xFF007FC01FF007FC,0xFE01FE01FE01FE01,0xFD04794A10E6A606,0xFC07F01FC07F01FC
 .quad  0xFB0C610D5E938F1A,0xFA11CAA01FA11CAA,0xF9182B6813BAF1B2,0xF81F81F81F81F81F
 .quad  0xF727CCE5F530A519,0xF6310ACA0DBB574B,0xF53B3A3FA204E514,0xF44659E4A427157F
 .quad  0xF3526859B8CEC01F,0xF25F644230AB50CA,0xF16D4C4401F16D4C,0xF07C1F07C1F07C1F
 .quad  0xEF8BDB389EBACC38,0xEE9C7F8458E01EE9,0xEDAE0A9B3D3A55D0,0xECC07B301ECC07B3
 .quad  0xEBD3CFF850B0C01E,0xEAE807ABA01EAE80,0xE9FD21044E798A49,0xE9131ABF0B7672A0
 .quad  0xE829F39AEF5090EC,0xE741AA59750E466C,0xE65A3DBE74D6ADD5,0xE573AC901E573AC9
 .quad  0xE48DF596F33941C6,0xE3A9179DC1A733F4,0xE2C511719EE15AFD,0xE1E1E1E1E1E1E1E1
 .quad  0xE0FF87C01E0FF87C,0xE01E01E01E01E01E,0xDF3D4F17DE4DB070,0xDE5D6E3F8868A470
 .quad  0xDD7E5E316D94C01D,0xDCA01DCA01DCA01D,0xDBC2ABE7D71D453A,0xDAE6076B981DAE60
 .quad  0xDA0A2F3803B4145E,0xD92F2231E7F89B43,0xD854DF401D854DF4,0xD77B654B82C33917
 .quad  0xD6A2B33EF7447B2C,0xD5CAC807572B201D,0xD4F3A293769C9F5E,0xD41D41D41D41D41D
 .quad  0xD347A4BC01D347A4,0xD272CA3FC5B1A6B8,0xD19EB155F08A3B1C,0xD0CB58F6EC07432D
 .quad  0xCFF8C01CFF8C01CF,0xCF26E5C44BFC61B2,0xCE55C8EAC7900739,0xCD85689039B0AD12
 .quad  0xCCB5C3B636E3A7D1,0xCBE6D9601CBE6D96,0xCB18A8930DE5FF1A,0xCA4B3055EE19101C
 .quad  0xC97E6FB15E44CD83,0xC8B265AFB8A4201C,0xC7E7115D0CE94B3D,0xC71C71C71C71C71C
 .quad  0xC65285FD56843703,0xC5894D10D4985C1F,0xC4C0C61456A8E5E9,0xC3F8F01C3F8F01C3
 .quad  0xC331CA3E91678BAD,0xC26B5392EA01C26B,0xC1A58B327F5761EB,0xC0E070381C0E0703
 .quad  0xC01C01C01C01C01C,0xBF583EE868D8AEBE,0xBE9526D0769F9E4F,0xBDD2B899406F74AE
 .quad  0xBD10F365451B61CA,0xBC4FD65883E7B3A2,0xBB8F609879493469,0xBACF914C1BACF914
 .quad  0xBA10679BD84886B0,0xB951E2B18FF23570,0xB89401B89401B894,0xB7D6C3DDA338B2AF
 .quad  0xB71A284EE6B33E2D,0xB65E2E3BEEE05231,0xB5A2D4D5B081EC57,0xB4E81B4E81B4E81B
 .quad  0xB42E00DA17006D0B,0xB37484AD806CDD21,0xB2BBA5FF26A22D00,0xB2036406C80D901B
 .quad  0xB14BBDFD760E6303,0xB094B31D922A3E85,0xAFDE42A2CB481E5D,0xAF286BCA1AF286BC
 .quad  0xAE732DD1C2A093F7,0xADBE87F94905E01A,0xAD0A798177692A51,0xAC5701AC5701AC57
 .quad  0xABA41FBD2E5B0A70,0xAAF1D2F87EBFCAA1,0xAA401AA401AA401A,0xA98EF606A63BD81A
 .quad  0xA8DE64688EBAB5BB,0xA82E65130E158A5B,0xA77EF750A56D989B,0xA6D01A6D01A6D01A
 .quad  0xA621CDB4F8FDF055,0xA574107688A4A156,0xA4C6E200D2637100,0xA41A41A41A41A41A
 .quad  0xA36E2EB1C432CA57,0xA2C2A87C51CA04E8,0xA217AE575FF2EF42,0xA16D3F97A4B01A16
 .quad  0xA0C35B92ECDF088C,0xA01A01A01A01A01A,0x9F713117200CFB89,0x9EC8E951033D91D2
 .quad  0x9E2129A7D5F0A1C4,0x9D79F176B682D395,0x9CD34019CD34019C,0x9C2D14EE4A1019C2
 .quad  0x9B876F5262DD093E,0x9AE24EA5510DA483,0x9A3DB2474FB97D65,0x9999999999999999
 .quad  0x98F603FE6709FC01,0x9852F0D8EC0FF33D,0x97B05F8D5665203F,0x970E4F80CB8727C0
 .quad  0x966CC01966CC0196,0x95CBB0BE377AD92A,0x952B20D73EE97259,0x948B0FCD6E9E0652
 .quad  0x93EB7D0AA6758C07,0x934C67F9B2CE6019,0x92ADD0064AB74019,0x920FB49D0E228D59
 .quad  0x9172152B841DCB77,0x90D4F120190D4F12,0x903847EA1CEC1132,0x8F9C18F9C18F9C18
 .quad  0x8F0063C018F0063C,0x8E6527AF1373F070,0x8DCA64397E407C4F,0x8D3018D3018D3018
 .quad  0x8C9644F01EFBBD62,0x8BFCE8062FF3A018,0x8B64018B64018B64,0x8ACB90F6BF3A9A37
 .quad  0x8A3395C018A3395C,0x899C0F601899C0F6,0x8904FD503744B39F,0x886E5F0ABB04994B
 .quad  0x87D8340AB6E96C4B,0x87427BCC092B8EE6,0x86AD35CB59A84018,0x8618618618618618
 .quad  0x8583FE7A7C018583,0x84F00C2780613C03,0x845C8A0CE512956D,0x83C977AB2BEDD28E
 .quad  0x8336D48397A238B8,0x82A4A0182A4A0182,0x8212D9EBA4018212,0x8181818181818181
 .quad  0x80F0965DFABCB5F1,0x8060180601806018,0x7FD005FF4017FD00,0x7F405FD017F405FD
 .quad  0x7EB124FFA053B6C0,0x7E225515A4F1D1B9,0x7D93EF9AA4B45AEC,0x7D05F417D05F417D
 .quad  0x7C7862170949F065,0x7BEB3922E017BEB3,0x7B5E78C6937337F1,0x7AD2208E0ECC3545
 .quad  0x7A463005E918C017,0x79BAA6BB6398B6F6,0x792F843C689C2DAC,0x78A4C8178A4C8178
 .quad  0x781A71DC01781A71,0x77908119AC60D341,0x7706F5610D8D005D,0x767DCE434A9B1017
 .quad  0x75F50B522B17BCCD,0x756CAC201756CAC2,0x74E4B040174E4B04,0x745D1745D1745D17
 .quad  0x73D5E0C5899F68F1,0x734F0C541FE8CB0F,0x72C899870F91EC72,0x724287F46DEBC05C
 .quad  0x71BCD732E940A1C2,0x713786D9C7C08A74,0x70B29680E66F9E10,0x702E05C0B81702E0
 .quad  0x6FA9D432443802DF,0x6F26016F26016F26,0x6EA28D118B474016,0x6E1F76B4337C6CB1
 .quad  0x6D9CBDF26EAEF380,0x6D1A62681C860FB0,0x6C9863B1AB4294D4,0x6C16C16C16C16C16
 .quad  0x6B957B34E7802D72,0x6B1490AA31A3CFC7,0x6A94016A94016A94,0x6A13CD153729043E
 .quad  0x6993F349CC7267CF,0x691473A88D0BFD2D,0x68954DD2390B9ECF,0x6816816816816816
 .quad  0x67980E0BF08C7765,0x6719F36016719F36,0x669C31075AB40166,0x661EC6A5122F9016
 .quad  0x65A1B3DD13356F69,0x6524F853B4AA339E,0x64A893ADCD25F6F1,0x642C8590B21642C8
 .quad  0x63B0CDA236E1C7BA,0x63356B88AC0DE016,0x62BA5EEADE65D882,0x623FA7701623FA77
 .quad  0x61C544C0161C544C,0x614B36831AE93AA6,0x60D17C61DA197F23,0x6058160581605816
 .quad  0x5FDF0317B5C6F558,0x5F66434292DFBE1C,0x5EEDD630A9FB33BF,0x5E75BB8D015E75BB
 .quad  0x5DFDF303137B62C6,0x5D867C3ECE2A5349,0x5D0F56EC91E56954,0x5C9882B931057262
 .quad  0x5C21FF51EF005708,0x5BABCC647FA9150C,0x5B35E99F06714015,0x5AC056B015AC056B
 .quad  0x5A4B1346ADD2AF2C,0x59D61F123CCAA376,0x596179C29D2CDBE9,0x58ED2308158ED230
 .quad  0x58791A9357CCDE06,0x5805601580560158,0x5791F34015791F34,0x571ED3C506B39A22
 .quad  0x56AC0156AC0156AC,0x56397BA7C52E1EBF,0x55C7426B792862CB,0x5555555555555555
 .quad  0x54E3B4194CE65DE0,0x54725E6BB82FE015,0x5401540154015401,0x5390948F40FEAC6F
 .quad  0x53201FCB02FB0847,0x52AFF56A8054ABFD,0x5240152401524015,0x51D07EAE2F8151D0
 .quad  0x516131C01516131C,0x50F22E111C4C56DE,0x508373590EC9C6D1,0x5015015015015015
 .quad  0x4FA6D7AEB597C3B0,0x4F38F62DD4C9A844,0x4ECB5C86B3D23A32,0x4E5E0A72F0539782
 .quad  0x4DF0FFAC83C014DF,0x4D843BEDC2C4B8FF,0x4D17BEF15CB4DBE4,0x4CAB88725AF6E74F
 .quad  0x4C3F982C207235DC,0x4BD3EDDA68FE0E42,0x4B68893948D1B826,0x4AFD6A052BF5A814
 .quad  0x4A928FFAD5B5C014,0x4A27FAD76014A27F,0x49BDAA583B40149B,0x49539E3B2D066EA2
 .quad  0x48E9D63E504D16CE,0x4880522014880522,0x4817119F3D324D89,0x47AE147AE147AE14
 .quad  0x47455A726ABF1F00,0x46DCE34596066250,0x4674AEB4717E90BC,0x460CBC7F5CF9A1C0
 .quad  0x45A50C670938EC99,0x453D9E2C776CA014,0x44D67190F8B42EF2,0x446F86562D9FAEE4
 .quad  0x4408DC3E05B227DF,0x43A2730ABEE4D1DB,0x433C4A7EE52B3ED0,0x42D6625D51F86EF9
 .quad  0x4270BA692BC4CD4D,0x420B5265E595123D,0x41A62A173E820AAE,0x4141414141414141
 .quad  0x40DC97A843AE87FD,0x40782D10E6566064,0x4014014014014014,0x3FB013FB013FB013
 .quad  0x3F4C65072BF744E9,0x3EE8F42A5AF06DA0,0x3E85C12A9D6517F3,0x3E22CBCE4A9027C4
 .quad  0x3DC013DC013DC013,0x3D5D991AA75C5BBD,0x3CFB5B51698EB428,0x3C995A47BABE7440
 .quad  0x3C3795C553AFB5E2,0x3BD60D923295482C,0x3B74C1769AA5BCD7,0x3B13B13B13B13B13
 .quad  0x3AB2DCA869B81620,0x3A524387AC82260F,0x39F1E5A22F36E108,0x3991C2C187F63371
 .quad  0x3931DAAF8F721568,0x38D22D366088DBF3,0x3872BA2057E04459,0x3813813813813813
 .quad  0x37B48248727447D6,0x3755BD1C945EDC1F,0x36F7317FD92119D0,0x3698DF3DE0747953
 .quad  0x363AC622898B0ED8,0x35DCE5F9F2AF821E,0x357F3E9078E5B470,0x3521CFB2B78C1352
 .quad  0x34C4992D87FD9676,0x34679ACE0134679A,0x340AD461776D32D6,0x33AE45B57BCB1E0C
 .quad  0x3351EE97DBFC660A,0x32F5CED6A1DFA013,0x3299E64013299E64,0x323E34A2B10BF66E
 .quad  0x31E2B9CD37DC276E,0x3187758E9EBB6013,0x312C67B6173EE1E6,0x30D190130D190130
 .quad  0x3076EE7525C2C013,0x301C82AC40260390,0x2FC24C887448614C,0x2F684BDA12F684BD
 .quad  0x2F0E8071A5702A9E,0x2EB4EA1FED14B15E,0x2E5B88B5E3103D6A,0x2E025C04B8097012
 .quad  0x2DA963DDD3CFAFDF,0x2D50A012D50A012D,0x2CF8107590E66DEC,0x2C9FB4D812C9FB4D
 .quad  0x2C478D0C9C012C47,0x2BEF98E5A3710FD1,0x2B97D835D548D9AC,0x2B404AD012B404AD
 .quad  0x2AE8F087718CFD5F,0x2A91C92F3C1053F9,0x2A3AD49AF090747E,0x29E4129E4129E412
 .quad  0x298D830D13780253,0x293725BB804A4DC9,0x28E0FA7DD35A2A54,0x288B01288B01288B
 .quad  0x2835399057EFCD16,0x27DFA38A1CE4D6F8,0x278A3EEAEE6503C0,0x27350B88127350B8
 .quad  0x26E009370049B802,0x268B37CD601268B3,0x263697210AA178F5,0x25E22708092F1138
 .quad  0x258DE75895120F7A,0x2539D7E9177B21CA,0x24E5F890293056F4,0x2492492492492492
 .quad  0x243EC97D49EAE176,0x23EB79717605B399,0x239858D86B11F09F,0x23456789ABCDF012
 .quad  0x22F2A55CE8FC4E6B,0x22A0122A0122A012,0x224DADC90048936B,0x21FB78121FB78121
 .quad  0x21A970DDC5BA69CB,0x21579804855E6012,0x2105ED5F1E335E8D,0x20B470C67C0D8875
 .quad  0x20632213B6C6D458,0x2012012012012012,0x1FC10DC4FCE8AD1A,0x1F7047DC11F7047D
 .quad  0x1F1FAF3F16B6419C,0x1ECF43C7FB84C2F0,0x1E7F0550DB594011,0x1E2EF3B3FB874431
 .quad  0x1DDF0ECBCB840C48,0x1D8F5672E4ABC83A,0x1D3FCA840A073E1E,0x1CF06ADA2811CF06
 .quad  0x1CA13750547FDC6B,0x1C522FC1CE058D9A,0x1C035409FC1DF459,0x1BB4A4046ED29011
 .quad  0x1B661F8CDE832EC5,0x1B17C67F2BAE2B20,0x1AC998B75EB906E7,0x1A7B9611A7B9611A
 .quad  0x1A2DBE6A5E3E4718,0x19E0119E0119E011,0x19928F89362B721D,0x19453808CA29C046
 .quad  0x18F80AF9B06DC0E4,0x18AB083902BDAB94,0x185E2FA401185E2F,0x1811811811811811
 .quad  0x17C4FC72BFCB8B10,0x1778A191BD684180,0x172C7052E131589A,0x16E0689427378EB4
 .quad  0x16948A33B08FA497,0x1648D50FC3201164,0x15FD4906C96F086A,0x15B1E5F75270D045
 .quad  0x1566ABC011566ABC,0x151B9A3FDD5C8CB8,0x14D0B155B19AE5C7,0x1485F0E0ACD3B68C
 .quad  0x143B58C01143B58C,0x13F0E8D3447241C0,0x13A6A0F9CF01E263,0x135C81135C81135C
 .quad  0x131288FFBB3B5DC0,0x12C8B89EDC0ABBD7,0x127F0FD0D229481B,0x12358E75D30336A0
 .quad  0x11EC346E36091857,0x11A3019A748267AE,0x1159F5DB29605DF6,0x1111111111111111
 .quad  0x10C8531D0952D8D7,0x107FBBE01107FBBE,0x10374B3B480AA228,0x0FEF010FEF010FEF
 .quad  0x0FA6DD3F6732238C,0x0F5EDFAB325A1A80,0x0F170834F27F9A57,0x0ECF56BE69C8FDE2
 .quad  0x0E87CB297A51E61C,0x0E40655826010E40,0x0DF9252C8E5E629A,0x0DB20A88F469598C
 .quad  0x0D6B154FB86F8E56,0x0D24456359E39D2C,0x0CDD9AA677344010,0x0C9714FBCDA3AC10
 .quad  0x0C50B446391F2E60,0x0C0A7868B41708E6,0x0BC4614657568DBA,0x0B7E6EC259DC7935
 .quad  0x0B38A0C010B38A0C,0x0AF2F722EECB5712,0x0AAD71CE84D1622E,0x0A6810A6810A6810
 .quad  0x0A22D38EAF2BEC3F,0x09DDBA6AF8360109,0x0998C51F624D4AF5,0x0953F39010953F39
 .quad  0x090F45A1430A9CDC,0x08CABB37565E2010,0x08865436C3CF6F56,0x0842108421084210
 .quad  0x07FDF0041FF7C010,0x07B9F29B8EAE19C1,0x0776182F57385881,0x073260A47F7C66CF
 .quad  0x06EECBE029154FDB,0x06AB59C7912FB61F,0x06680A40106680A4,0x0624DD2F1A9FBE76
 .quad  0x05E1D27A3EE9C010,0x059EEA0727586632,0x055C23BB98E2A5E6,0x05197F7D73404146
 .quad  0x04D6FD32B0C7B499,0x04949CC1664C5789,0x04525E0FC2FCB1F4,0x0410410410410410
 .quad  0x03CE4584B19A0185,0x038C6B78247FBF1C,0x034AB2C50040D2AC,0x03091B51F5E1A4EE
 .quad  0x02C7A505CFFBF4E1,0x02864FC7729E8C5E,0x02451B7DDB2D2594,0x0204081020408102
 .quad  0x01C315657186ABAC,0x0182436517A3752F,0x014191F67411155A,0x0101010101010101
 .quad  0x00C0906C513CEDB2,0x0080402010080402,0x0040100401004010,0x0000000000000000

  ## For a given byte of data, tell us which is the highest bit set

hibitset:
  .byte 8,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4
  .byte 5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5
  .byte	6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6
  .byte	6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6
  .byte	7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7
  .byte	7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7
  .byte	7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7
  .byte	7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7

##########################################################################
##									##
##  Signed wrappers							##
##									##
##  For division, the result is negative iff the input operands are of 	##
##    different sign							##
##									##
##  For remainder, the result is negative iff the dividend is negative	##
##									##
##  In all cases, the magnitude of quotients and remainders is always 	##
##    the same as the unsigned values					##
##									##
##########################################################################

	.p2align 4
__divq:
	subq	$sp,32,$sp
	lda	$27,__divqu-__divq($27)
	xor	$24,$25,$28	# see if output should be negative
	stq	$23, 0($sp)	# save return address
	stq	$28, 8($sp)	# save 'output is negative' flag
	stq	$24,16($sp)	# preserve original operands
	stq	$25,24($sp)
	subq	$31,$24,$23	# maybe dividend is negative
	subq	$31,$25,$28	# maybe divisor is negative
	cmovlt	$24,$23,$24	# make sure dividend is positive
	cmovlt	$25,$28,$25	# make sure divisor is positive
	bsr	$23,__divqu	# perform unsigned division
	subq	$31,$27,$25	# get negative output
	ldq	$23, 0($sp)	# restore return address
	ldq	$28, 8($sp)	# retrieve 'output is negative' flag
	ldq	$24,16($sp)	# restore orignal operands only to have 
	cmovlt	$28,$25,$27	# (maybe make output negative)
	ldq	$25,24($sp)	# ... them discarded 99.99% of the time
	addq	$sp,32,$sp
	ret	$31,($23)

	.p2align 4
__divl:
	subq	$sp,32,$sp
	lda	$27,__divqu-__divl($27)
	stq	$24,16($sp)	# preserve original operands
	stq	$25,24($sp)
	addl	$24,  0,$24	# make sure operands are properly sign extended
	addl	$25,  0,$25
	stq	$23, 0($sp)	# save return address
	xor	$24,$25,$28	# see if output should be negative
	subl	$31,$24,$23	# maybe dividend is negative
	stl	$28, 8($sp)	# save 'output is negative' flag
	subl	$31,$25,$28	# maybe divisor is negative
	cmovlt	$24,$23,$24	# make sure dividend is positive
	cmovlt	$25,$28,$25	# make sure divisor is positive
	bsr	$23,__divqu	# perform unsigned division
				# - use quad divide, we know top half of R24 & R25 are zero
	subl	$31,$27,$25	# get negative output
	ldq	$23, 0($sp)	# restore return address
	ldl	$28, 8($sp)	# retrieve 'output is negative' flag
	ldq	$24,16($sp)	# restore orignal operands only to have 
	cmovlt	$28,$25,$27	# (maybe make output negative)
	ldq	$25,24($sp)	# ... them discarded 99.99% of the time
	addq	$sp,32,$sp
	ret	$31,($23)

	.p2align 4
__remq:
	subq	$sp,24,$sp
	lda	$27,__remqu-__remq($27)
	stq	$23, 0($sp)	# save return address
	stq	$24, 8($sp)	# save 'output is negative' flag / preserve dividend
	stq	$25,16($sp)	# preserve original divisor
	subq	$31,$24,$23	# maybe dividend is negative
	subq	$31,$25,$28	# maybe divisor is negative
	cmovlt	$24,$23,$24	# make sure dividend is positive
	cmovlt	$25,$28,$25	# make sure divisor is positive
	bsr	$23,__remqu	# perform unsigned division
	subq	$31,$27,$25	# get negative output
	ldq	$24, 8($sp)	# retrieve 'output is negative' flag / restore dividend
	ldq	$23, 0($sp)	# restore return address
	cmovlt	$24,$25,$27	# maybe make output negative
	ldq	$25,24($sp)	# restore original divisor
	addq	$sp,24,$sp
	ret	$31,($23)

	.p2align 4
__reml:
	subq	$sp,32,$sp
	lda	$27,__remqu-__reml($27)
	stq	$23, 0($sp)	# save return address
	stl	$24, 8($sp)	# save 'output is negative' flag
	stq	$24,16($sp)	# preserve original operands
	stq	$25,24($sp)
	addl	$24,  0,$24	# make sure operands are properly sign extended
	addl	$25,  0,$25
	subl	$31,$24,$23	# maybe dividend is negative
	subl	$31,$25,$28	# maybe divisor is negative
	cmovlt	$24,$23,$24	# make sure dividend is positive
	cmovlt	$25,$28,$25	# make sure divisor is positive
	bsr	$23,__remqu	# perform unsigned division
				# - use quad divide, we know top half of R24 & R25 are zero
	subl	$31,$27,$25	# get negative output
	ldq	$23, 0($sp)	# restore return address
	ldl	$28, 8($sp)	# retrieve 'output is negative' flag
	ldq	$24,16($sp)	# restore original operands
	cmovlt	$28,$25,$27	# maybe make output negative
	ldq	$25,24($sp)
	addq	$sp,32,$sp
	ret	$31,($23)

##########################################################################
##									##
##  Unsigned long wrappers						##
##									##
##########################################################################

	.p2align 4
__divlu:
	subq	$sp,24,$sp
	lda	$27,__divqu-__divlu($27)
	stq	$23, 0($sp)	# save return address
	stq	$24, 8($sp)	# preserve original operands
	stq	$25,16($sp)
	zapnot	$24,15,$24	# convert unsigned long to unsigned quad
	zapnot	$25,15,$25
	bsr	$23,__divqu	# divide
	ldq	$23, 0($sp)	# restore
	ldq	$24, 8($sp)
	ldq	$25,16($sp)
	addq	$sp,24,$sp
	ret	$31,($23)

	.p2align 4
__remlu:
	subq	$sp,24,$sp
	lda	$27,__remqu-__remlu($27)
	stq	$23, 0($sp)	# save return address
	stq	$24, 8($sp)	# preserve original operands
	stq	$25,16($sp)
	zapnot	$24,15,$24	# convert unsigned long to unsigned quad
	zapnot	$25,15,$25
	bsr	$23,__remqu	# remainder
	ldq	$23, 0($sp)	# restore
	ldq	$24, 8($sp)
	ldq	$25,16($sp)
	addq	$sp,24,$sp
	ret	$31,($23)

##########################################################################
##									##
##  Unsigned Quad Divide						##
##									##
##########################################################################

	.p2align 4
__divqu:
	beq	$25,divbyzero
	subq	$sp,64,$sp
	cmpbge	$31,$25,$28		# see which is top non-zero byte of R25
	stq	$16, 0($sp)		# save some scratch regs
	subq	$27,$28,$28
	ldbu	$16,hibitset+255-__divqu($28)
	extbl	$25,$16,$28		# see which is top bit within that byte
	stq	$17, 8($sp)
	addq	$27,$28,$28
	ldbu	$17,hibitset-__divqu($28)
	s8addq	$16,$17,$16		# calc top bit number in R25 that is set
	xor	$16, 63,$28		# invert it
	stq	$18,16($sp)
	sll	$25,$28,$28		# R28<63> is known to now be non-zero

	# R16 = bit number of top '1' bit in R25
	# R28 = R25 shifted left so '1' bit is in <63>

	stq	$19,24($sp)
	sll	$28,NMULTBITS,$19
	stq	$20,32($sp)
	addq	$28,$28,$28		# shift out top '1' bit
	stq	$21,40($sp)
	beq	$28,divqu_powr2		# special case for divide by power-of-two
	mov	  1,$21
	srl	$28,65-NMULTBITS,$28	# get index for multipliers table = top NMULTBITS divisor bits, but without the '1'
	stq	$22,48($sp)
	sll	$21, 63,$21		# R21 = TOPBIT
	stq	$24,56($sp)
	s8addq	$28,$27,$18
	beq	$19,divqu_quick		# quick for easy divisors (just NMULTBITS of precision)
	ldq	$18,multipliers-__divqu($18) # R18 = multiplier
	mov	  0,$27			# (reset quotient)
	srl	$18,  1,$18
	xor	$25,$21,$20		# R20 = divisor - TOPBIT
	or	$18,$21,$18
divqu_loop:

	# R16 = bit number of top '1' bit in R25
	# R18 = multiplier, such that (remainder*multiplier)>>R16 is a good guess at quotient
	# R20 = divisor - TOPBIT
	# R21 = TOPBIT (1<<63)
	# R24 = remainder
	# R25 = divisor
	# R27 = quotient accumulator

	srl	$24,  1,$28		# calc remainder / 2
	cmpult	$28,$25,$28		# see if (remainder / 2) .lt. divisor
	blbs	$28,divqu_done		# if so, no more looping
	umulh	$24,$18,$17		# guesstimate = umulh (remainder, multiplier)
	srl	$17,$16,$17		# shift guesstimate right by scale factor
					# calc product that guesstimate would give us
	xor	$17,$21,$22		#   R22 = guesstimate - TOPBIT
	addq	$17,$25,$28		#   R28 = guesstimate + divisor
	mulq	$22,$20,$19		#   R19 = (guesstimate - TOPBIT) * (divisor - TOPBIT)
	sll	$28, 63,$28		#   R28 = (guesstimate + divisor) * TOPBIT
	nop
	subq	$19,$28,$19		#   R19 = guesstimate * divisor

	# R17 = guesstimate
	# R19 = guesstimate * divisor

	addq	$27,$17,$27		# add guesstimate to quotient
	cmpule	$19,$24,$28		# we should never overestimate
	subq	$24,$19,$24		# and subtract product from remainder
	blbs	$28,divqu_loop
	call_pal PAL_bugcheck		# barf if product .gt. remainder
divqu_done:
	cmpule	$25,$24,$28		# see if divisor .le. remainder
	ldq	$16, 0($sp)		# restore scratch
	addq	$27,$28,$27		# if so, increment quotient one last time
	ldq	$17, 8($sp)
	ldq	$18,16($sp)
	ldq	$19,24($sp)
	ldq	$20,32($sp)
	ldq	$21,40($sp)
	ldq	$22,48($sp)
	ldq	$24,56($sp)
	addq	$sp,64,$sp
	ret	$31,($23)		# return

	.align 4
divqu_powr2:
	srl	$24,$16,$27		# shift dividend over to get quotient
	ldq	$16, 0($sp)		# restore scratch
	ldq	$17, 8($sp)
	ldq	$19,24($sp)
	addq	$sp,64,$sp
	ret	$31,($23)		# return

	## If the multiplier consists of NMULTBITS of precision, the multiplier from the table 
	## is an 'exact' value

	## R16 = shift factor for quotient
	## R18 = index in multipliers table
	## R21 = TOPBIT (1<<63)
	## R24 = original dividend
	## R25 = original divisor

	## For example, divide by 3:
	##  R16 = 1 (top bit of R25 that is a 1)
	##  R18 = we want the entry 0x5555.5555.5555.5555
	##  R25 = 3
	## The 0x5555.5555.5555.5555 actually represents 0x1.5555.5555.5555.5555
	## We want to multiply by 0x1.5555.5555.5555.5556 and right-shift by 2

	.align 4
divqu_quick:
	ldq	$18,multipliers-8-__divqu($18) # R18 = multiplier
	addq	$18,  1,$18		# convert 0x5555.5555.5555.5555 to 0x5555.5555.5555.5556
	umulh	$18,$24,$27		# calc quotient=multiplier*dividend, high 64 bits

	ldq	$17, 8($sp)		# restore scratch during multiply
	ldq	$18,16($sp)
	ldq	$19,24($sp)
	ldq	$21,40($sp)

	addq	$27,$24,$27		# this gives the 0x1. on the front
	cmpult	$27,$24,$28		# this is the carry bit from the addition
	srl	$27,  1,$27		# shift the 65 bits right one bit
	sll	$28, 63,$28
	addq	$27,$28,$27
	srl	$27,$16,$27		# scale quotient

	ldq	$16, 0($sp)		# finish restoring scratch
	addq	$sp,64,$sp
	ret	$31,($23)		# return

##########################################################################
##									##
##  Unsigned Quad Remainder						##
##									##
##  This is a direct copy of the __divqu routine, except the output is 	##
##  taken from R24 instead of R27					##
##									##
##########################################################################

	.p2align 4
__remqu:
	beq	$25,divbyzero
	subq	$sp,64,$sp
	cmpbge	$31,$25,$28		# see which is top non-zero byte of R25
	stq	$16, 0($sp)		# save some scratch regs
	subq	$27,$28,$28
	ldbu	$16,hibitset+255-__remqu($28)
	extbl	$25,$16,$28		# see which is top bit within that byte
	stq	$17, 8($sp)
	addq	$27,$28,$28
	ldbu	$17,hibitset-__remqu($28)
	s8addq	$16,$17,$16		# calc top bit number in R25 that is set
	xor	$16, 63,$28		# invert it
	stq	$18,16($sp)
	sll	$25,$28,$28		# R28<63> is known to now be non-zero

	# R16 = bit number of top '1' bit in R25
	# R28 = R25 shifted left so '1' bit is in <63>

	stq	$19,24($sp)
	sll	$28,NMULTBITS,$19
	stq	$20,32($sp)
	addq	$28,$28,$28		# shift out top '1' bit
	stq	$21,40($sp)
	beq	$28,remqu_powr2		# special case for divide by power-of-two
	mov	  1,$21
	srl	$28,65-NMULTBITS,$28	# get index for multipliers table = top NMULTBITS divisor bits, but without the '1'
	stq	$22,48($sp)
	sll	$21, 63,$21		# R21 = TOPBIT
	stq	$24,56($sp)
	s8addq	$28,$27,$18
	beq	$19,remqu_quick		# quick for easy divisors (just NMULTBITS of precision)
	ldq	$18,multipliers-__remqu($18) # R18 = multiplier
	mov	  0,$27			# (reset quotient)
	srl	$18,  1,$18
	xor	$25,$21,$20		# R20 = divisor - TOPBIT
	or	$18,$21,$18
remqu_loop:

	# R16 = bit number of top '1' bit in R25
	# R18 = multiplier, such that (remainder*multiplier)>>R16 is a good guess at quotient
	# R20 = divisor - TOPBIT
	# R21 = TOPBIT (1<<63)
	# R24 = remainder
	# R25 = divisor
	# R27 = quotient accumulator

	srl	$24,  1,$28		# calc remainder / 2
	cmpult	$28,$25,$28		# see if (remainder / 2) .lt. divisor
	blbs	$28,remqu_done		# if so, no more looping
	umulh	$24,$18,$17		# guesstimate = umulh (remainder, multiplier)
	srl	$17,$16,$17		# shift guesstimate right by scale factor
					# calc product that guesstimate would give us
	xor	$17,$21,$22		#   R22 = guesstimate - TOPBIT
	addq	$17,$25,$28		#   R28 = guesstimate + divisor
	mulq	$22,$20,$19		#   R19 = (guesstimate - TOPBIT) * (divisor - TOPBIT)
	sll	$28, 63,$28		#   R28 = (guesstimate + divisor) * TOPBIT
	nop
	subq	$19,$28,$19		#   R19 = guesstimate * divisor

	# R17 = guesstimate
	# R19 = guesstimate * divisor

	addq	$27,$17,$27		# add guesstimate to quotient
	cmpule	$19,$24,$28		# we should never overestimate
	subq	$24,$19,$24		# and subtract product from remainder
	blbs	$28,remqu_loop
	call_pal PAL_bugcheck		# barf if product .gt. remainder
remqu_done:
	cmpule	$25,$24,$28		# see if divisor .le. remainder
	ldq	$16, 0($sp)		# restore scratch
	subq	$24,$25,$27		# assume so and set result = remainder - divisor
	ldq	$17, 8($sp)
	cmoveq	$28,$24,$27		# but if not, set result = remainder
	ldq	$18,16($sp)
	ldq	$19,24($sp)
	ldq	$20,32($sp)
	ldq	$21,40($sp)
	ldq	$22,48($sp)
	ldq	$24,56($sp)
	addq	$sp,64,$sp
	ret	$31,($23)		# return

	.align 4
remqu_powr2:
	ldq	$16, 0($sp)		# restore scratch
	subq	$25,  1,$28		# make a bitmask for result
	ldq	$17, 8($sp)
	and	$24,$28,$27		# mask to get remainder
	ldq	$19,24($sp)
	addq	$sp,64,$sp
	ret	$31,($23)		# return

	## If the multiplier consists of NMULTBITS of precision, the multiplier from the table 
	## is an 'exact' value

	## R16 = shift factor for quotient
	## R18 = index in multipliers table
	## R21 = TOPBIT (1<<63)
	## R24 = original dividend
	## R25 = original divisor

	## For example, divide by 3:
	##  R16 = 1 (top bit of R25 that is a 1)
	##  R18 = we want the entry 0x5555.5555.5555.5555
	##  R25 = 3
	## The 0x5555.5555.5555.5555 actually represents 0x1.5555.5555.5555.5555
	## We want to multiply by 0x1.5555.5555.5555.5556 and right-shift by 2

	.align 4
remqu_quick:
	ldq	$18,multipliers-8-__remqu($18) # R18 = multiplier
	addq	$18,  1,$18		# convert 0x5555.5555.5555.5555 to 0x5555.5555.5555.5556
	umulh	$18,$24,$27		# calc quotient=multiplier*dividend, high 64 bits

	ldq	$17, 8($sp)		# restore scratch during multiply
	ldq	$18,16($sp)
	ldq	$19,24($sp)

	addq	$27,$24,$27		# this gives the 0x1. on the front
	cmpult	$27,$24,$28		# this is the carry bit from the addition
	srl	$27,  1,$27		# shift the 65 bits right one bit
	sll	$28, 63,$28
	addq	$27,$28,$27
	srl	$27,$16,$27		# scale quotient

	## remainder = dividend - quotient*divisor
	##  R21 = TOPBIT (1<<63)
	##  R24 = dividend
	##  R25 = divisor
	##  R27 = quotient

	subq	$25,$21,$28		# calc divisor-TOPBIT
	subq	$27,$21,$21		# calc quotient-TOPBIT
	mulq	$21,$28,$27		# calc (divisor-TOPBIT)*(quotient-TOPBIT)
	ldq	$16, 0($sp)		# finish restoring scratch
	addq	$21,$28,$28		# calc divisor+quotient
	ldq	$21,40($sp)
	sll	$28, 63,$28		# calc (divisor+quotient)*TOPBIT
	addq	$sp,64,$sp
	subq	$27,$28,$28		# calc divisor*quotient
	subq	$24,$28,$27		# calc remainder=dividend-divisor*quotient
	ret	$31,($23)		# return

##########################################################################
##									##
##  Divide by zero trap							##
##									##
##########################################################################

divbyzero:
	lda	$16,GEN_INTDIV		# load GENTRAP code for division by zero
	mov	  0,$27			# return a zero result
	call_pal PAL_gentrap		# puque
	ret	$31,($23)		# return (in case someone tries to continue)

##	/* Divide/Remainder table generator */
##	
##	#include <stdio.h>
##	
##	typedef int Long;
##	typedef long long Quad;
##	typedef unsigned int uLong;
##	typedef unsigned long long uQuad;
##	
##	#define NMULTBITS 10
##	
##	#define TOPBIT 0x8000000000000000ULL
##	#define ARRAYSIZE ((1 << NMULTBITS) / 2)
##	
##	static uQuad calcmult (int n);
##	
##	int main ()
##	
##	{
##	  int n;
##	  uQuad m;
##	
##	  for (n = 0; n < ARRAYSIZE; n ++) {
##	    m = calcmult (n);
##	    if ((n % 4) == 0) printf (" .quad  ");
##	    printf ("0x%16.16llX", m);
##	    if ((n % 4) == 3) printf ("\n");
##	                 else printf (",");
##	  }
##	  return (0);
##	}
##	
##	/* Calculate (TOPBIT << NMULTBITS) / (n + 1 + ARRAYSIZE) */
##	
##	/* Assume NMULTBITS = 8: */
##	
##	/*  n    what it's for  table value          */
##	/* -1    div by  0x80  2.0000.0000.0000.0000 */
##	/* 0x3F  div by  0xC0  1.5555.5555.5555.5555 */
##	/* 0xFF  div by 0x100  1.0000.0000.0000.0000 */
##	
##	/* calcmult(n) = ((1.0000.0000.0000.0000 << NMULTBITS) - 1) / (n + 1 + ARRAYSIZE)) */
##	
##	static uQuad calcmult (int n)
##	
##	{
##	  int i;
##	  uLong divisor;
##	  uQuad dividend, quotient;
##	
##	  if (n == ARRAYSIZE-1) return (0);	/* returns FFFFFFFFFFFFFFFF otherwise */
##	
##	  divisor  = n + 1 + ARRAYSIZE;		/* convert n=0x3F to d=0xC0, etc */
##	
##	  dividend = (1 << NMULTBITS) - 1;
##	  quotient = 0;
##	
##	  for (i = 2; -- i >= 0;) {
##	    dividend <<= 32;
##	    dividend  |= 0xFFFFFFFFULL;
##	    quotient <<= 32;
##	    quotient  += dividend / divisor;
##	    dividend  %= divisor;
##	  }
##	
##	  return (quotient);
##	}
