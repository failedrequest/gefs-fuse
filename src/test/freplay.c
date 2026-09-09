#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

File*	ctlfile;
File*	datfile;
char*	mountpt	= "/mnt/replay";
char*	srvname = "replay";
char*	logfile;
char*	replayfile;
char*	membuf;
vlong	membufsz;
vlong	replaycount	= -1;
int	logfd		= -1;
int	replayfd	= -1;
vlong	nwrites;

void
log1(int fd, void *buf, vlong off, vlong sz)
{
	char *p, hdr[12];

	p = hdr;
	PBIT64(p, off);	p += 8;
	PBIT32(p, sz);
	if(write(fd, hdr, sizeof(hdr)) == -1)
		sysfatal("write header: %r");
	if(write(fd, buf, sz) == -1)
		sysfatal("write data: %r\n");
}

int
replay1(int fd)
{
	uchar *p, hdr[12];
	vlong o;
	int n, r;

	r = readn(fd, hdr, 12);
	if(r == 0)
		return 0;
	if(r != 12)
		sysfatal("failed to read operation header: %r");

	p = hdr;
	o = GBIT64(p);	p += 8;
	n = GBIT32(p);
	if(o + n > membufsz)
		sysfatal("operation exceeds buffer size");
	if(readn(fd, membuf + o, n) != n)
		sysfatal("read op: %r");
	nwrites++;
	return 1;
}

void
readmembuf(Req *r, void *s, vlong n)
{
	r->ofcall.count = r->ifcall.count;
	if(r->ifcall.offset >= n){
		r->ofcall.count = 0;
		return;
	}
	if(r->ifcall.offset+r->ofcall.count > n)
		r->ofcall.count = n - r->ifcall.offset;
	memmove(r->ofcall.data, (char*)s+r->ifcall.offset, r->ofcall.count);
}

void
fsread(Req *r)
{
	char buf[128];

	if(r->fid->file == datfile){
		readmembuf(r, membuf, membufsz);
		respond(r, nil);
	}else if(r->fid->file == ctlfile){
		snprint(buf, sizeof(buf), "writes %lld\n", nwrites);
		readstr(r, buf);
		respond(r, nil);
	}else
		abort();
}

void
fswrite(Req *r)
{
	if(r->fid->file == datfile){
		if(logfile == nil){
			respond(r, "read-only replay file: no log defined");
			return;
		}
		if(r->ifcall.offset + r->ifcall.count > membufsz){
			respond(r, "operation exceeds file size");
			return;
		}
		log1(logfd, r->ifcall.data, r->ifcall.offset, r->ifcall.count);
		memcpy(membuf + r->ifcall.offset, r->ifcall.data, r->ifcall.count);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		nwrites++;
	}else if(r->fid->file == ctlfile){
		if(strncmp(r->ifcall.data, "exit", 4) == 0){
			print("exiting...\n");
			r->ofcall.count = r->ifcall.count;
			respond(r, nil);
			exits(nil);
		}else if(strncmp(r->ifcall.data, "step", 4) == 0){
			r->ofcall.count = r->ifcall.count;
			if(replayfd == -1)
				respond(r, "no active replay");
			else if(!replay1(replayfd))
				respond(r, "no replay left");
			else
				respond(r, nil);
		}else
			respond(r, "unknown ctl message");
	}else
		abort();
}

void
usage(void)
{
	fprint(2, "usage: %s [-l log] [-r replay] [-c count] file\n", argv0);
	exits("usage");
}	

static Srv fs = {
	.read = fsread,
	.write = fswrite,
};
void
main(int argc, char *argv[])
{
	int fd;
	vlong n, off;
	char *uid;
	Dir *d;
	int i;
	
	ARGBEGIN{
	case 'd':
		chatty9p++;
		break;
	case 'l':
		logfile = EARGF(usage());
		break;
	case 'r':
		replayfile = EARGF(usage());
		break;
	case 'c':
		replaycount = atoi(EARGF(usage()));
		break;
	case 'm':
		mountpt = EARGF(usage());
		break;
	case 's':
		srvname = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND;
	
	if(argc != 1)
		usage();
	
	if((fd = open(argv[0], OREAD)) == -1)
		sysfatal("open %s: %r", argv[0]);
	if((d = dirfstat(fd)) == nil)
		sysfatal("failed to stat file: %r");
	if((membuf = sbrk(d->length)) == (void*)-1)
		sysfatal("failed to allocate buffer: %r");
	d->length -= (d->length % IOUNIT);
	memset(membuf, 0, d->length);
	for(off = 0; off < d->length; off += n)
		if((n = read(fd, membuf+off, IOUNIT)) <= 0)
			sysfatal("read %s@%lld: short read: %r", argv[0], off);
	membufsz = d->length;
	free(d);
	if(replayfile != nil){
		if((replayfd = open(replayfile, OREAD)) == -1)
			sysfatal("failed to open replay file: %r");
		for(i = 0; i < replaycount || replaycount == -1; i++)
			if(replay1(replayfd) == 0)
				break;
		print("replayed %d ops\n", i);
	}

	if(logfile != nil){
		if((logfd = create(logfile, OWRITE, 0666)) == -1)
			sysfatal("failed to open log file: %r");
	}
	uid = getuser();
	fs.tree = alloctree(uid, uid, DMDIR|0555, nil);
	ctlfile = createfile(fs.tree->root, "ctl", uid, 0666, nil);
	datfile = createfile(fs.tree->root, "data", uid, 0666, nil);
	datfile->length = membufsz;
	postmountsrv(&fs, srvname, mountpt, MREPL);
	exits(nil);
}
