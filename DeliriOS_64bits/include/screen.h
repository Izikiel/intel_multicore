#ifndef __screen_H__
#define __screen_H__
#include <types.h>

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
#define whiteOnRed (C_BG_RED | C_FG_WHITE)
#define whiteOnBlack (C_BG_BLACK | C_FG_WHITE)
#define whiteOnBlue (C_BG_BLUE | C_FG_WHITE)
#define blackOnWhite (C_BG_LIGHT_GREY | C_FG_BLACK)
#define blackOnOrange (C_BG_BROWN | C_FG_BLACK)
#define blackOnGreen (C_BG_GREEN | C_FG_BLACK)
#define blackOnCyan (C_BG_CYAN | C_FG_BLACK)
#define blackOnBlue (C_BG_BLUE | C_FG_BLACK)

void putChar(char caracter, uint8_t format, uint8_t posX, uint8_t posY);
void printString(char* cadena, uint8_t format, uint8_t posX, uint8_t posY);
void printInteger(uint32_t number, uint8_t format, uint8_t posX, uint8_t posY);
void printLine(char* cadena, uint8_t format);
void printChar(char caracter, uint8_t format);
void printLineNumber(uint32_t number, uint8_t format);
void clockCursor();
void clrscr();
void hideCursor();
void printProxCursor();
void backspace();
void setInitialPrintingLine(uint32_t number);
void getLastScreenLine(char* buffer);//Nota, el buffer devuelto es de tamanio VIDEO_COLS
void writeConsoleSymbol();

#endif  /* !__screen_H__ */