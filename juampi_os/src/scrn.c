#include <scrn.h>
#include <utils.h>
#include <vargs.h>
#include <irq.h>
//Posicion de inicio de la memoria de video
static ushort* video_mem = (ushort*) 0xb8000;   //La memoria de video empieza en la direccion 0xb8000
//Variables de alineacion
static uchar cursor_r = 0, cursor_c = 0, align_c = 0;
static ushort format;

uint in_video_mem(uint address)
{
	return address >= 0xb8000 &&
		address <= 0xb8000 + 
			VIDEO_WIDTH*VIDEO_HEIGHT*sizeof(ushort);	
}

static int scrn_valid_pos(uchar cursor_r, uchar cursor_c)
{
	return  cursor_r <= VIDEO_HEIGHT &&
	        cursor_c <= VIDEO_WIDTH;
}

uchar scrn_getrow()
{
	return cursor_r;
}

uchar scrn_getcol()
{
	return cursor_c;
}

void scrn_set_blink_indicator(uchar row, uchar col)
{
	ushort tmp = ((uint)row)*VIDEO_WIDTH + col;
	outb(0x3D4, 14);
	outb(0x3D5, tmp >> 8);
	outb(0x3D4, 15);
	outb(0x3D5, tmp);
}

void scrn_cls()
{
	cursor_r = cursor_c = align_c = 0;
	for(int i = 0; i < VIDEO_WIDTH*VIDEO_HEIGHT; i++)
		video_mem[i] = 0x20 | (GREEN << 8);
	scrn_set_blink_indicator(0,0);
}

void scrn_setcursor(uchar row, uchar col)
{
	cursor_r = row;
	cursor_c = col;	
}

void scrn_setmode(color font, color backgr)
{
	format = ((backgr << 4) | font) << 8;
}

ushort scrn_getmode()
{
	return format;
}

void scrn_move_back()
{
	if(cursor_c > 0) cursor_c--;
	video_mem[cursor_r*VIDEO_WIDTH + cursor_c] = 0x20 | format;
	scrn_set_blink_indicator(cursor_r,cursor_c);
}

void scrn_shift_up()
{
	ushort backup_buffer[VIDEO_HEIGHT][VIDEO_WIDTH];
	memcpy(backup_buffer,video_mem,sizeof(backup_buffer));
	memcpy(video_mem,backup_buffer[1],
		VIDEO_WIDTH*(VIDEO_HEIGHT-1)*sizeof(ushort));	
	memset(video_mem + (VIDEO_HEIGHT-1)*VIDEO_WIDTH,
			0, VIDEO_WIDTH*sizeof(ushort));
}

void scrn_putc(char c, ushort fmt)
{
	if(cursor_c >= VIDEO_WIDTH) {
		scrn_setcursor(cursor_r+1,cursor_c - VIDEO_WIDTH);	
	}
	if(cursor_r >= VIDEO_HEIGHT) {
		scrn_shift_up();
		scrn_setcursor(VIDEO_HEIGHT-1,cursor_c);
	}
	switch(c) {
	case '\n': //nueva linea con retorno de carro (irse a la izquierda)
		scrn_setcursor(cursor_r+1,0);
		break;
	case '\r':	
		for(int i = 0; i < VIDEO_WIDTH; i++)
			video_mem[cursor_r*VIDEO_WIDTH+i] = 0;
		scrn_setcursor(cursor_r,0);
		break;
	case '\t': //tab
		scrn_setcursor(cursor_r,cursor_c + TAB_WIDTH);
		break;
	default: //caracter (se supone imprimible).
		if(cursor_c < VIDEO_WIDTH && cursor_r < VIDEO_HEIGHT) {
			video_mem[cursor_r*VIDEO_WIDTH+cursor_c] = (fmt | c);
			scrn_setcursor(cursor_r,cursor_c+1);
		}
		break;
	}
	
	scrn_set_blink_indicator(cursor_r,cursor_c);
}

void scrn_print(char* msg)
{
	uint i;
	for(i = 0; msg[i]; i++) {
		scrn_putc(msg[i],format);
	}
}

void scrn_vprintf(char* msg, varg_list l)
{
	uint i;
	char buffer[32];
	align_c = cursor_c;
	for(i = 0; msg[i]; i++) {
		switch(msg[i]) {
		case '%':
			i++;
			switch(msg[i]) {
			case '%':
				scrn_putc(msg[i], format);
				break;
			case 'u':
				num_to_str(varg_yield(l,uint), 16, buffer);
				scrn_print(buffer);
				break;
			case 'd':
				num_to_str(varg_yield(l,uint), 10, buffer);
				scrn_print(buffer);
				break;
			case 's':
				scrn_print(varg_yield(l,char*));
				break;
			case 'c':
				scrn_putc(varg_yield(l,char),format);
				break;
			case 'b':
				scrn_print(varg_yield(l,uint) ? "true" : "false");
				break;
			}
			break;
		default:
			scrn_putc(msg[i],format);
			break;
		}
	}
}

void scrn_printf(char* msg, ...)
{
	varg_list l;
	varg_set(l,msg);
	scrn_vprintf(msg,l);
	varg_end(l);
}

//Imprime en la direccion indicada: Devuelve 0 si esta todo
//bien o -1 en caso de error
int scrn_pos_print(uchar row, uchar col, char* msg)
{
	uint eflags = irq_cli();
	int ret = 0;
	if(scrn_valid_pos(row,col)) {
		uchar   prev_row = scrn_getrow(),
		        prev_col = scrn_getcol();
		scrn_setcursor(row,col);
		scrn_print(msg);
		scrn_setcursor(prev_row,prev_col);
	} else {
		ret = -1;
	}
	irq_sti(eflags);
	return ret;
}

int scrn_pos_printf(uchar row, uchar col, char* msg, ...)
{
	uint eflags = irq_cli();
	int ret = 0;
	if(scrn_valid_pos(row,col)) {
		uchar   prev_row = scrn_getrow(),
		        prev_col = scrn_getcol();
		scrn_setcursor(row,col);
		varg_list l;
		varg_set(l,msg);
		scrn_vprintf(msg,l);
		varg_end(l);
		scrn_setcursor(prev_row,prev_col);
	} else {
		ret = -1;
	}
	irq_sti(eflags);
	return ret;
}
