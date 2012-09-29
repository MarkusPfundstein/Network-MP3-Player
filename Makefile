GCC=gcc
FLAGS=-O2 -Wall
LIBS=-lao -lmpg123
INCL=

OBJ=connection.o main.o

connection.o : src/connection.c
	${GCC} ${FLAGS} -c src/connection.c ${INCL}

main.o : src/main.c
	${GCC} ${FLAGS} -c src/main.c ${INCL}

all : ${OBJ}
	${GCC} ${FLAGS} ${OBJ} -o mp3player.out ${LIBS}

clean :
	rm *.o *.out
