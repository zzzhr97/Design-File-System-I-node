all:disk clean

disk:disk.o
	gcc -o disk disk.o
disk.o:disk.c
	gcc -c disk.c -o disk.o

clean:
	rm *.o
