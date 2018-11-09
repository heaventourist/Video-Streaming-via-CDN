all: miProxy nameserver
miProxy: miProxy.o
	g++ miProxy.o -o miProxy

miProxy.o: miProxy.cpp
	g++ -c miProxy.cpp

nameserver: nameserver.o
	g++ nameserver.o -o nameserver

nameserver.o: nameserver.cpp
	g++ -c nameserver.cpp


clean: 
	rm -f *.o
	rm miProxy
	rm nameserver