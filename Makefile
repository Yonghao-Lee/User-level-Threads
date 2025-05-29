all: libuthreads.a

uthreads.o: uthreads.cpp uthreads.h
	g++ -Wall -g -std=c++11 -c uthreads.cpp -o uthreads.o

libuthreads.a: uthreads.o
	ar rcs libuthreads.a uthreads.o

clean:
	rm -f *.o libuthreads.a