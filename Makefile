all:	lbard

lbard:	main.c Makefile
	cc -g -Wall -o lbard main.c -lcurl

