/* 64-bit C example kernel for Pure64 */
/* Written by Ian Seyler (www.returninfinity.com) */


unsigned int print(char *message, unsigned int line);

int main()
{
	print("Welcome to your 64-bit OS written in C (Thanks to Pure64!).", 12);
	while(1)
	{
		// infinite loop of doing nothing
	}
	return (0);
};



/* Kernel functions */

unsigned int print(char *message, unsigned int line)
{
	char *vidmem = (char *) 0xb8000;
	unsigned int i= 0;

	i=(line*80*2);

	while(*message!=0) // 24h
	{
		vidmem[i]= *message;
		*message++;
		i++;
		vidmem[i]= 0x7;
		i++;
	};

	return(1);
};
