all:disk fs client clean

disk:disk.o
	gcc -o disk disk.o
disk.o:disk.c
	gcc -c disk.c -o disk.o

fs:fs.o
	gcc -o fs fs.o
fs.o:fs.c
	gcc -c fs.c -o fs.o

client:client.o
	gcc -o client client.o
client.o:client.c
	gcc -c client.c -o client.o

clean:
	rm *.o
