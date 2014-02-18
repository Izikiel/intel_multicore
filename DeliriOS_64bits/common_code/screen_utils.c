#include "screen_utils.h"

#ifndef zero
#define zero 0x30
#endif

#ifndef line_length
#define line_length 80
#endif

#ifndef color
#define color 0x07
#endif

void clear_screen(){
	volatile char* screen = (volatile char*) VIDEO_MEMORY;
	for (int i = 0; i < line_length*25*2; ++i)
		screen[i] = 0;
	return;
}

void print_number_u64(uint64_t number, uint8_t line){
	volatile char* screen = (volatile char*) VIDEO_MEMORY;

	char number_string[line_length] = {0};
	int index = line_length-1;

	for (; index >= 0 && number > 0; index--){
		number_string[index] = number%10 + zero;
		number /= 10;
	}

	for (int i = 0; index < line_length; i++, index++){
		screen[line_length * line * 2 + i] = number_string[index];
		i++;
		screen[line_length * line * 2 + i] = color;

	}

	return;
}

void print_string(const char* string, uint8_t line){
	volatile char* screen = (volatile char*) VIDEO_MEMORY;

	for (int i = 0, j = 0; i < line_length && string[j] > 0; i++, j++){
		screen[line_length * line * 2 + i] = string[j];
		i++;
		screen[line_length * line * 2 + i] = color;
	}
}