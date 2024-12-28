all: vfd cpu

vfd:
	gcc -O3 ./main.c -o vfd-daemon -I./lib -L./lib -l:libch347.a

cpu:
	gcc -O3 ./cpu_util.c -o cpu-util

