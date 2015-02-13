all:	lbard

lbard:	main.c Makefile
	gcc -g -Wall -o lbard main.c

