LIBS=-lpthread
OBJS=http_conn.o
CFLAGS=-Wall
server:${OBJS} main.o
	g++ -o $@ ${OBJS} main.o ${LIBS}
client:stress_test.o
	g++ -o $@ stress_test.o ${LIBS}
clean:
	rm -f server client ${OBJS} main.o stress_test.o
