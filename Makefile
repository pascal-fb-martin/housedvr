
OBJS= housedvr_store.o housedvr_feed.o housedvr.o
LIBOJS=

SHARE=/usr/local/share/house

all: housedvr

clean:
	rm -f *.o *.a housedvr

rebuild: clean all

%.o: %.c
	gcc -c -g -O -o $@ $<

housedvr: $(OBJS)
	gcc -g -O -o housedvr $(OBJS) -lhouseportal -lechttp -lssl -lcrypto -lgpiod -lrt

install:
	if [ -e /etc/init.d/housedvr ] ; then systemctl stop housedvr ; fi
	mkdir -p /usr/local/bin
	mkdir -p /var/lib/house
	mkdir -p /etc/house
	rm -f /usr/local/bin/housedvr /etc/init.d/housedvr
	cp housedvr /usr/local/bin
	cp init.debian /etc/init.d/housedvr
	chown root:root /usr/local/bin/housedvr /etc/init.d/housedvr
	chmod 755 /usr/local/bin/housedvr /etc/init.d/housedvr
	mkdir -p $(SHARE)/public/dvr
	chmod 755 $(SHARE) $(SHARE)/public $(SHARE)/public/dvr
	cp public/* $(SHARE)/public/dvr
	chown root:root $(SHARE)/public/dvr/*
	chmod 644 $(SHARE)/public/dvr/*
	touch /etc/default/dvr
	systemctl daemon-reload
	systemctl enable housedvr
	systemctl start housedvr

uninstall:
	systemctl stop housedvr
	systemctl disable housedvr
	rm -f /usr/local/bin/housedvr /etc/init.d/housedvr
	rm -f $(SHARE)/public/dvr
	systemctl daemon-reload

purge: uninstall
	rm -rf /etc/house/dvr.config /etc/default/dvr

