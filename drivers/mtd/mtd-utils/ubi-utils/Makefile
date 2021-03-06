#
# Makefile for ubi-utils
#

OPTFLAGS := -O2 -g -Wall
KERNELHDR := ../include
DESTDIR := /usr/local
SBINDIR=/usr/sbin
MANDIR=/usr/share/man
INCLUDEDIR=/usr/include

CROSS=mipsel-linux-
CC := $(CROSS)gcc
CFLAGS := -I./inc -I./src -I$(KERNELHDR) $(OPTFLAGS) -Werror \
	-Wwrite-strings -W -std=gnu99 -DPACKAGE_VERSION=\"1.0\"

PERLPROGS = mkpfi ubicrc32.pl

NTARGETS = ubiattach ubicrc32 ubidetach ubimkvol ubinfo ubinize \
	ubirmvol ubiupdatevol
TARGETS = pfiflash pddcustomize ubimirror bin2nand nand2bin ubigen \
	mkbootenv unubi pfi2bin $(NTARGETS)

vpath   %.c ./src

%: %.o
	$(CC) $(LDFLAGS) -g -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -g -c -o $@ $< -g -Wp,-MD,.$(shell basename $<).dep

all: $(TARGETS)
	mkdir -p ubi-utils-dir
	cp $(TARGETS) ubi-utils-dir
	make -C new-utils

IGNORE=${wildcard .*.c.dep}
-include ${IGNORE}

$(NTARGETS):
	make -C new-utils $@
	mv new-utils/$@ $@

clean:
	rm -rf *.o $(TARGETS) .*.c.dep
	rm -fr ubi-utils-dir	
	make -C new-utils clean

pddcustomize: pddcustomize.o error.o libubimirror.o bootenv.o hashmap.o \
		libubi.o crc32.o
	$(CC) $(LDFLAGS) -o $@ $^

pfiflash: pfiflash.o libpfiflash.o list.o reader.o error.o libubimirror.o \
		bootenv.o hashmap.o pfi.o libubi.o crc32.o
	$(CC) $(LDFLAGS) -o $@ $^

ubimirror: ubimirror.o error.o libubimirror.o bootenv.o hashmap.o \
		libubi.o crc32.o
	$(CC) $(LDFLAGS) -o $@ $^

nand2bin: nand2bin.o nandecc.o nandcorr.o
	$(CC) $(LDFLAGS) -o $@ $^

bin2nand: bin2nand.o error.o nandecc.o
	$(CC) $(LDFLAGS) -o $@ $^

ubigen: ubigen.o libubigen.o crc32.o
	$(CC) $(LDFLAGS) -o $@ $^

mkbootenv: mkbootenv.o bootenv.o hashmap.o error.o crc32.o
	$(CC) $(LDFLAGS) -o $@ $^

unubi: unubi.o crc32.o unubi_analyze.o eb_chain.o
	$(CC) $(LDFLAGS) -o $@ $^

pfi2bin: pfi2bin.o peb.o error.o list.o crc32.o libubigen.o bootenv.o \
		hashmap.o reader.o pfi.o
	$(CC) $(LDFLAGS) -o $@ $^

install: ${TARGETS}
	mkdir -p ${DESTDIR}/${SBINDIR}
	install -m0755 ${TARGETS} ${DESTDIR}/${SBINDIR}/
	(cd perl && install ${PERLPROGS} ${DESTDIR}/${SBINDIR}/)

uninstall:
	for file in ${TARGETS} ${PERLPROGS}; do \
		$(RM) ${DESTDIR}/${SBINDIR}/$$file; \
	done
