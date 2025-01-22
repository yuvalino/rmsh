.PHONY: first rmsh clean

first: rmsh

clean:
	@rm rmsh

rmsh:
	gcc -g -rdynamic -I. main.c -o rmsh

librmsh:
	gcc -g -I. main.c -c -o main.o -DLIBRMSH
	ar rcs librmsh.a main.o