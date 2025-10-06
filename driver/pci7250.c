// ----------------------------------------------------------------------
// driver karty PCI-7250 dla FreeBSD
// o karcie:
//      https://www.adlinktech.com/products/Data_Acquisition/DigitalI_O/PCI-7250_7251
//      https://www.adlinktech.com/Products/Download.ashx?type=MDownload&isManual=yes&file=21%5cPCI-7250_manual_en.pdf
// by Marek Kowalczyk / kowalczm666, 2025
//
#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/kthread.h>
#include <sys/conf.h>
#include <sys/bus.h>
#include <machine/bus.h>
#include <machine/resource.h>
#include <sys/rman.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>
#include <pci_if.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/ioccom.h>

#define PCI7250_PORT_OUT	0
#define PCI7250_PORT_IN		1

// kontekst softwarowy
struct softc {
	device_t			device;
	struct resource		*res[2];
	struct cdev		*uchodev;			// byte output device
	struct cdev		*uchidev;			// byte input device
	struct cdev		*bitodev[8];		// osobne bity-kanały out
	struct cdev		*bitidev[8];		// osobne bity-kanały out
};

static unsigned char h2b(char h) {
	switch (h) {
		case '0' ... '9':
			return h-'0';
		case 'a' ... 'f':
			return 0xA + (h-'a');
		case 'A' ... 'F':
			return 0xA + (h-'A');
	}
	return 0xFF;
}

// podstawowe operacje na devie

// otwarcie
static int pci7250_open(struct cdev *dev, int oflags, int devtype, struct thread *td) {
    struct softc *sc;
    sc = dev->si_drv1;
	if ( dev == sc->uchodev ) {
		device_printf( sc->device, "dout byte opened\n");
	}
	else if ( dev == sc->uchidev ) {
		device_printf( sc->device, "din byte opened\n");
	}
	else {
		for (int b=0; b<8; b++) {
			if ( dev == sc->bitodev[b] ) {
				device_printf( sc->device, "dout bit %d opened\n", b);
				break;
			}
			if ( dev == sc->bitidev[b] ) {
				device_printf( sc->device, "din bit %d opened\n", b);
				break;
			}
		}
	}
    return 0;
}

// zamkniecie
static int pci7250_close(struct cdev *dev, int fflag, int devtype, struct thread *td) {
    struct softc *sc;
    sc = dev->si_drv1;
    device_printf( sc->device, "closed\n");
    return 0;
}


// odczyt diwajsa, nn hex dla całego bajta lub 0,1 dla kanału
static int pci7250_read(struct cdev *dev, struct uio *uio, int ioflag) {
	int error = 0;
	const char *hexDigits = "0123456789abcdef";
	char outbuff[3] = {0,0,0};
    struct softc *sc;

	// zakonczenie odczytu
	if ( uio->uio_offset >= sizeof(outbuff) ) {
	   	return 0;
	}

    sc = dev->si_drv1;
	// wykmin jakie urzadzenie czytamy - czy feedback czy din i czy bitowe czy całe
	if ( dev == sc->uchodev ) {
		device_printf( sc->device, "dout byte read, feedback\n");
		unsigned char byte = bus_read_1(sc->res[1], PCI7250_PORT_OUT ); // feedback reg.
		outbuff[0] = hexDigits[ (byte >> 4) & 0x0F ];
		outbuff[1] = hexDigits[ byte & 0x0F ];
	}
	else if ( dev == sc->uchidev ) {
		device_printf( sc->device, "din byte read\n");
		unsigned char byte = bus_read_1(sc->res[1], PCI7250_PORT_IN ); // wejscie
		outbuff[0] = hexDigits[ (byte >> 4) & 0x0F ];
		outbuff[1] = hexDigits[ byte & 0x0F ];
	}
	else {
		for (int b=0; b<8; b++) {
			if ( dev == sc->bitodev[b] ) {
				device_printf( sc->device, "dout bit %d read, feedback\n", b);
				unsigned char byte = bus_read_1(sc->res[1], PCI7250_PORT_OUT ); // feedback reg.
				outbuff[0] = (byte & (1<<b)) != 0 ? '1' : '0';
				break;
			}
			if ( dev == sc->bitidev[b] ) {
				device_printf( sc->device, "din bit %d read\n", b);
				unsigned char byte = bus_read_1(sc->res[1], PCI7250_PORT_IN ); // feedback reg.
				outbuff[0] = (byte & (1<<b)) != 0 ? '1' : '0';
				break;
			}
		}
	}
	error = uiomove( outbuff + uio->uio_offset, MIN(uio->uio_resid, sizeof(outbuff) - uio->uio_offset),uio);
	device_printf( sc->device, "read [%s], err %d\n", outbuff,error);
 	return error;
}

// mozliwości
// len=8 - 0/1
// len=2 - 0..9/a..f/A..F
// len=1 - 0,1
static int pci7250_write(struct cdev *dev, struct uio *uio, int ioflag) {
    struct softc *sc;
	unsigned char byte;
    sc = dev->si_drv1;
	void *kbuf;
	off_t of;
	size_t len;
	int ret;
	len = uio->uio_resid;
	of = (uintmax_t)uio->uio_offset;
	device_printf( sc->device, "read %ld bytes at 0x%lx\n", len, of );

	kbuf = malloc(len+1, M_DEVBUF, M_WAITOK); // worek na wypociny z user space
	memset( kbuf, 0x00, len+1 );
	//len1 = uio->uio_resid;	// ile
	//of = uio->uio_offset;	// od kiedy
	ret = uiomove(kbuf, len, uio);	// pobranie z userspace
	if (ret == 0) {
		((char*)kbuf)[ len ] = 0x00;
		device_printf( sc->device, "got data [%s]\n", (char*)kbuf);
		// hex?
		// 2 znaczki 00\n - ff\n
		// 1 znaczki 1,0\n
		if (len == 2) {
			char hic = ((char*)kbuf)[0];
			unsigned char hib = h2b(hic);
			if ( hib == 0xFF ) {
				device_printf( sc->device, "err, bad hex\n");
				ret = 1;
				goto finally;
			}
			char loc = ((char*)kbuf)[1];
			unsigned char lob = h2b(loc);
			if ( lob == 0xFF ) {
				device_printf( sc->device, "err, bad hex\n");
				ret = 1;
				goto finally;
			}
			//
			byte = hib << 4 | lob;
			bus_write_1(sc->res[1], PCI7250_PORT_OUT, byte );
			device_printf( sc->device, "set data [%02X]\n", byte);
		}
		if (len == 1) {
			char hic = ((char*)kbuf)[0];
			if (hic != '0' && hic != '1' ) {
				device_printf( sc->device, "err, bad bit\n");
				ret = 1;
				goto finally;
			}
			// wydlub nr bitu z devica
			int bit = 0;
			for (int b=0; b<8; b++) {
				if ( dev == sc->bitodev[b] ) {
					bit = b;
					break;
				}
			}
			// odczytaj stary
			byte = bus_read_1(sc->res[1], PCI7250_PORT_OUT ); // feedback reg.
			if (hic == '1') {
				// set bit
				byte |= 1 << bit;
			}
			else {
				// clear bit
				byte &= ~(1 << bit);
			}
			bus_write_1(sc->res[1], PCI7250_PORT_OUT, byte );
			device_printf( sc->device, "set data [%02X]\n", byte);
		}
		if (len == 8) {
			byte = 0x00;
			for (int b = 0; b < 8; b++ ) {
				char c = ((char*)kbuf)[b];
				if ( c == '1' || c == '0' ) {
					byte |= (c-'0') << (7-b);
				}
				else {
					device_printf( sc->device, "err, bad bin\n");
					ret = 1;
					goto finally;
				}
			}
			bus_write_1(sc->res[1], PCI7250_PORT_OUT, (unsigned char)byte );
			device_printf( sc->device, "set data [%02X]\n", (unsigned char)byte);
		}
	}
finally:
	free(kbuf, M_DEVBUF);	// worek precz
	return ret; // 0 - ok
}


static struct cdevsw pci7250_cdevsw = {
	.d_version 	= D_VERSION,
//	.d_flags 	= D_NEEDGIANT,		//!!!? wtf
	.d_open 	= pci7250_open,
	.d_close 	= pci7250_close,
	.d_read 	= pci7250_read,
	.d_write 	= pci7250_write,
	.d_name 	= "pci7250",
};

static struct resource_spec pci7250_res_spec[] = {
	{ SYS_RES_IOPORT,	PCIR_BAR(1),	RF_ACTIVE}, // xuj wie co ten rejestr robi, expander?, nie grzebać
	{ SYS_RES_IOPORT,	PCIR_BAR(2),	RF_ACTIVE}, // faktyczne I/O karty: PCI7250_PORT_OUT etc/itd
	{ -1, 0, 0 }
};


// operacje na urządzeniu szyny PCI - probe, attach, detach, etc

#define PCI7250_VENDOR_ID		0x144a
#define PCI7250_DEVICE_ID		0x7250
#define PCI7250_DESCRIPTION		"PCI-7250 8xDIN 8xDOUT (ADLINK)"

// dopasowanie moduły do karty
static int pci7250_probe(device_t self) {
	if ( pci_get_vendor(self) == PCI7250_VENDOR_ID && pci_get_device(self) == PCI7250_DEVICE_ID) {
		device_set_desc(self, PCI7250_DESCRIPTION);
		device_printf(self, "pci7250_probe hit ok\n");
		return BUS_PROBE_DEFAULT;
	}
	return ENXIO;
}

// przypisanie instancji dev do karty
static int pci7250_attach(device_t self) {
	struct timespec ts;
	getnanotime( &ts );
	device_printf(self, "pci7250_attach %ld\n", ts.tv_sec );

	struct softc *sc;
	int error;

	sc = device_get_softc(self);
	bzero(sc, sizeof *sc);
	sc->device = self;

	error = bus_alloc_resources(self, pci7250_res_spec, sc->res);
	if (error) {
		device_printf(self, "bus_alloc_resources failed\n");
		return error;
	}
	// dalsze fizyczne operacje na porcie to wpisy pod adres sc->res[1] (!!!)

	// znakowe na całe bajty 		/dev/doutN
	sc->uchodev = make_dev(
		&pci7250_cdevsw,
		device_get_unit(self),
	    UID_ROOT,
		GID_WHEEL,
		0666,
		"dout%d",
		device_get_unit(self)
	);
	sc->uchodev->si_drv1 = sc;

	// znakowe na całe bajty 		/dev/dinN
	sc->uchidev = make_dev(
		&pci7250_cdevsw,
		device_get_unit(self),
	    UID_ROOT,
		GID_WHEEL,
		0666,
		"din%d",
		device_get_unit(self)
	);
	sc->uchidev->si_drv1 = sc;

	// bitowe
	for (int b=0; b < 8; b++) {
		// urzadznienie /dev/dout0.4
		sc->bitodev[ b ] = make_dev(
			&pci7250_cdevsw,
			device_get_unit(self),
			UID_ROOT,
			GID_WHEEL,
			0666,
			"dout%d.%d",
			device_get_unit(self),
			b
		);
		sc->bitodev[ b ]->si_drv1 = sc;

		// urzadzenie /dev/din1.0
		sc->bitidev[ b ] = make_dev(
			&pci7250_cdevsw,
			device_get_unit(self),
			UID_ROOT,
			GID_WHEEL,
			0666,
			"din%d.%d",
			device_get_unit(self),
			b
		);
		sc->bitidev[ b ]->si_drv1 = sc;

	}
	// zerujemy przekaźniki na starcie
	bus_write_1(sc->res[1], PCI7250_PORT_OUT , 0x00);
	// meldujemy stan wejść optoizolowanych na moment załadowania drivera
	unsigned char in = bus_read_1(sc->res[1], PCI7250_PORT_IN);
	device_printf(self, "init out = 00, in = %02x\n", in );
	return 0;
}

// posprzątenie urządzeń i odlączenie karty
static int pci7250_detach(device_t self) {
    struct softc *sc = device_get_softc(self);
    destroy_dev(sc->uchidev);
	destroy_dev(sc->uchodev);
	for (int b=0; b< 8; b++ ) {
		destroy_dev(sc->bitidev[b]);
		destroy_dev(sc->bitodev[b]);
	}
	device_printf(self, "pci7250_detach\n");
    return 0;
}

static device_method_t pci7250_methods[] = {
	DEVMETHOD(device_probe,		pci7250_probe),
	DEVMETHOD(device_attach,	pci7250_attach),
	DEVMETHOD(device_detach,	pci7250_detach),
	DEVMETHOD(device_suspend,	bus_generic_suspend),
	DEVMETHOD(device_resume,	bus_generic_resume),
	DEVMETHOD(device_shutdown,	bus_generic_shutdown),
	DEVMETHOD_END
};

static driver_t pci7250_driver = {
	"pci7250",
	pci7250_methods,
	sizeof(struct softc)
};

DRIVER_MODULE(pci7250, pci, pci7250_driver, 0, 0);
//MODULE_PNP_INFO("U16:vendor;U16:device;D:#", pci, adlink, pci7250_id, nitems(pci7250_id)); // a po xuj to?

