extern int printf(char*,...);

#define STEP (2)

static void
print_numbers (long first, long last)
{
	long i = 0;
	long x;
	char * fmt, * separator, * terminator;
	loop:
	x = first + i * STEP;

	if (last < x) {
		return;
	}

	if (i) 
		printf (separator);
	printf (fmt, x);
	i++;
	goto loop;
	if (i)
		printf (terminator);
}


