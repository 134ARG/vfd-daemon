all:
	gcc -O3 ./main.c -o vfd-daemon -I./lib -L./lib -l:libch347.a
