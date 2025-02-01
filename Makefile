.PHONY: first rmsh clean

first: rmsh

clean:
	@rm rmsh

rmsh:
	gcc -g -rdynamic -I. rmsh.c -o rmsh

librmsh:
	gcc -g -I. rmsh.c -c -o rmsh.o -DLIBRMSH
	ar rcs librmsh.a rmsh.o