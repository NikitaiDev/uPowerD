.PHONY: all daemon module clean

all: daemon module

daemon:
	$(MAKE) -C daemon

module:
	$(MAKE) -C module

clean:
	$(MAKE) -C daemon clean
	$(MAKE) -C module clean
