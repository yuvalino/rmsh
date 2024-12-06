.PHONY: first rmsh clean

first: rmsh

clean:
	@rm rmsh

rmsh:
	gcc -g -rdynamic -I. main.c -o rmsh
