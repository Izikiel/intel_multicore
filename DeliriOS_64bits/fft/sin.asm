global sin

local: dq 0x0

sin:
	finit
	movq [local], xmm0
	fld qword [local]
	fsin
	fst qword [local]
	movq xmm0, [local]
