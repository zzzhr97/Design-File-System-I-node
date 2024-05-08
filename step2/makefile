all:fs clean

fs:fs.o
	gcc -o fs fs.o
fs.o:fs.c
	gcc -c fs.c -o fs.o

clean:
	rm *.o
