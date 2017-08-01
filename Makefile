OBJS = ap6212hciattach.o hciattach_bcm43xx.o

ap6212hciattach: $(OBJS)
	gcc $(OBJS) -o ap6212hciattach

.c.o:
	gcc -O -c -Wall $<

clean:
	rm -f $(OBJS) ap6212hciattach

$(OBJS): hciattach.h tty.h

deb:
	dpkg-buildpackage -b -uc
