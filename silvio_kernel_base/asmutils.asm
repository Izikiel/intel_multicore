;global getEAX
;global getEBX
;global getECX
;global getEDX
;global getESI
;global getEDI
;global getEBP
;global getESP
;global getEIP
;global getCS
;global getDS
;global getES
;global getFS
;global getGS
;global getSS
;global getEFLAGS
;global getCR0
;global getCR2
;global getCR3
;global getCR4
;global jmpToTask
;global sleepClock
;global haltCpu
;
;sleepClock:	HLT
;			ret			
;haltCpu:
;		cli
;		hlt
;		ret
;
;getEAX:
;	;en EAX se retorna el valor de EAX, que es lo que queriamos
;	ret
;
;getEBX:	
;	mov EAX, EBX
;	ret
;
;getECX:	
;	mov EAX, ECX
;	ret
;
;getEDX:	
;	mov EAX, EDX
;	ret
;
;getESI:	
;	mov EAX, ESI
;	ret
;
;getEDI:	
;	mov EAX, EDI
;	ret
;
;getEBP:	
;	mov EAX, EBP
;	ret
;
;getESP:	
;	mov EAX, ESP
;	ret
;
;getEIP:	
;	MOV EAX, [ESP];leo la direccion de retorno y la copio en EAX!
;	ret
;
;getCS:	
;	mov EAX, CS
;	ret
;
;getDS:	
;	mov EAX, DS
;	ret
;
;getES:	
;	mov EAX, ES
;	ret
;
;getFS:	
;	mov EAX, FS
;	ret
;
;getGS:	
;	mov EAX, GS
;	ret
;
;getSS:	
;	mov EAX, SS
;	ret
;
;getEFLAGS:	
;	PUSHFQ;levanto EFLAGS a la pila
;	POP RAX; paso EFLAGS a eax	
;	ret
;
;getCR0:
;	mov EAX, CR0
;	ret
;
;getCR2:
;	mov EAX, CR2
;	ret
;
;getCR3:
;	mov EAX, CR3
;	ret
;
;getCR4:
;	mov EAX, CR4
;	ret