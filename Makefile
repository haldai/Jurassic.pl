all: jurassic

jurassic:
	+$(MAKE) -C c/
	mv c/jurassic.so lib/

clean:
	+$(MAKE) clean -C c/
	rm -rf lib/*.so
