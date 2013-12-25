#ifndef __console_H__
#define __console_H__
#include <types.h>
#include <vargs.h>

/* Definicion de limites */
/* -------------------------------------------------------------------------- */
#define VIDEO_FILS 25
#define VIDEO_COLS 80

/* Definicion de colores */
/* -------------------------------------------------------------------------- */
#define C_FG_BLACK              (0x0)
#define C_FG_BLUE               (0x1)
#define C_FG_GREEN              (0x2)
#define C_FG_CYAN               (0x3)
#define C_FG_RED                (0x4)
#define C_FG_MAGENTA            (0x5)
#define C_FG_BROWN              (0x6)
#define C_FG_LIGHT_GREY         (0x7)
#define C_FG_DARK_GREY          (0x8)
#define C_FG_LIGHT_BLUE         (0x9)
#define C_FG_LIGHT_GREEN        (0xA)
#define C_FG_LIGHT_CYAN         (0xB)
#define C_FG_LIGHT_RED          (0xC)
#define C_FG_LIGHT_MAGENTA      (0xD)
#define C_FG_LIGHT_BROWN        (0xE)
#define C_FG_WHITE              (0xF)

#define C_BG_BLACK              (0x0 << 4)
#define C_BG_BLUE               (0x1 << 4)
#define C_BG_GREEN              (0x2 << 4)
#define C_BG_CYAN               (0x3 << 4)
#define C_BG_RED                (0x4 << 4)
#define C_BG_MAGENTA            (0x5 << 4)
#define C_BG_BROWN              (0x6 << 4)
#define C_BG_LIGHT_GREY         (0x7 << 4)

#define C_BLINK                 (0x8 << 4)

#define modoEscrituraTexto whiteOnBlack
#define modoEscrituraFillWhite (C_BG_LIGHT_GREY | C_FG_BLACK)

#define redOnBlack (C_BG_BLACK | C_FG_RED)
#define greenOnBlack (C_BG_BLACK | C_FG_GREEN)
#define whiteOnRed (C_BG_RED | C_FG_WHITE)
#define whiteOnBlack (C_BG_BLACK | C_FG_WHITE)
#define whiteOnBlue (C_BG_BLUE | C_FG_WHITE)
#define whiteOnGreen (C_BG_GREEN | C_FG_WHITE)
#define blackOnWhite (C_BG_LIGHT_GREY | C_FG_BLACK)
#define blackOnOrange (C_BG_BROWN | C_FG_BLACK)
#define blackOnGreen (C_BG_GREEN | C_FG_BLACK)
#define blackOnCyan (C_BG_CYAN | C_FG_BLACK)
#define blackOnBlue (C_BG_BLUE | C_FG_BLACK)

//-------------- Start Command Processing functions -------------------

void console_processEnterKey();
void console_processTabKey();
void console_processBackSpaceKey();
void console_processKey(const char keyPressed);

//-------------- End Command Processing functions -------------------


void console_setXCursor(uint32_t number);
void console_setYCursor(uint32_t number);

void console_set_panic_format();

void console_moveUp();
void console_moveBack(bool fromSystem);

void console_pos_putc(char caracter, uint8_t format, uint8_t posX, uint8_t posY);
void console_puts(const char* string, uint8_t format);
void console_pos_print(const char* cadena, uint8_t format, uint8_t posX, uint8_t posY);
void console_pos_printInt(uint32_t number, uint8_t format, uint8_t posX, uint8_t posY);
void console_println(const char* cadena, uint8_t format);
void console_putc(char caracter, uint8_t format);
void console_printlnNumber(uint32_t number, uint8_t format);
void console_print_next_cursor();
void console_clear();
void console_hide_text_cursor();
void console_update_text_cursor();
void console_get_last_line(char* buffer);//Nota, el buffer devuelto es de tamanio VIDEO_COLS
void console_initialize_console();
void console_reset_console();

//Imprime el mensaje, con formato estilo printf de C. 
//PRE: La cantidad de parametros pasados DEBE ser correcta
void console_printf(const char* msg, ...);
// Tomado de juampi OS
void console_vprintf(const char* msg, va_list l);

#endif  /* !__console_H__ */