GCC=gcc
FLAGS=-O2 -Wall -g
LIBS=-lao -lmpg123
INCL=

OBJ=main.o

main.o : src/main.c
	${GCC} ${FLAGS} -c src/main.c ${INCL}

all : ${OBJ}
	${GCC} ${FLAGS} ${OBJ} -o mp3player.out ${LIBS}

clean :
	rm *.o *.out
