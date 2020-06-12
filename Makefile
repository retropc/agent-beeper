CFLAGS=-std=c99 -pedantic -Wall -Werror

.PHONY: clean

agent-beeper: agent-beeper.o connections.o

agent-beeper.o: connections.h

connections.o: connections.h config.h

clean:
	rm -f *.o agent-beeper
