#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <thread.h>

int mainstacksize = 64*1024*1024;
typedef struct Bench Bench;
enum {
	KiB	= 1024ULL,
	MiB	= 1024ULL*KiB,
	GiB	= 1024ULL*MiB,
	Bufsz	= 128*IOUNIT,
};

enum {
	Bps,
	Fps,
};
	
struct Bench {
	char	*name;
	char	*unit;
	vlong	(*fn)(Bench*);
	vlong	reps;
	int	nproc;
	int	id;
	Channel	*rc;

	vlong	i0;
	vlong	i1;
	vlong	i2;
	char	*s0;
	char	*s1;
};

#define	GBIT64(p)	((u32int)(((uchar*)(p))[0]|(((uchar*)(p))[1]<<8)|\
				(((uchar*)(p))[2]<<16)|(((uchar*)(p))[3]<<24)) |\
			((uvlong)(((uchar*)(p))[4]|(((uchar*)(p))[5]<<8)|\
				(((uchar*)(p))[6]<<16)|(((uchar*)(p))[7]<<24)) << 32))
vlong
vrand(vlong n)
{
	uchar buf[8];
	vlong slop, v;

	slop = 0x7fffffffffffffffULL % n;
	do{
		prng(buf, 8);
		v = GBIT64(buf);
	}while(v <= slop);
	return v % n;
}

vlong
wrfile_la(Bench *b)
{
	char buf[Bufsz];
	vlong i;
	int fd;

	if((fd = create(b->s0, OWRITE, 0666)) == -1)
		sysfatal("open: %r");
	for(i = 0; i < b->i0; i += Bufsz)
		if(write(fd, buf, Bufsz) != Bufsz)
			sysfatal("write: %r");
	close(fd);
	return b->i0/MiB;
}

vlong
wrfile_ra(Bench *b)
{
	char buf[Bufsz];
	vlong i, n, j, t, *off;
	int fd;

	n = b->i0/Bufsz;
	if((fd = create(b->s0, OWRITE, 0666)) == -1)
		sysfatal("open: %r");
	if((off = malloc(n*sizeof(vlong))) == nil)
		sysfatal("malloc: %r");
	for(i = 0; i < n; i++)
		off[i] = i*Bufsz;
	for (i = n - 1; i > 0; i--) {
		j = vrand(i+1);
		t = off[i];
		off[i] = off[j];
		off[j] = t;
	}
	for(i = 0; i < n; i++)
		if(pwrite(fd, buf, Bufsz, off[i]) != Bufsz)
			sysfatal("write: %r");
	close(fd);
	free(off);
	return b->i0/MiB;
}

vlong
wrfile_rr(Bench *b)
{
	char buf[Bufsz];
	vlong i, n, j, t, *off;
	int fd;

	n = b->i0/Bufsz;
	if((fd = create(b->s0, OWRITE, 0666)) == -1)
		sysfatal("open: %r");
	if((off = malloc(n*sizeof(vlong))) == nil)
		sysfatal("malloc: %r");
	for(i = 0; i < n; i++)
		off[i] = i*Bufsz;
	for (i = n - 1; i > 0; i--) {
		j = vrand(i+1);
		t = off[i];
		off[i] = off[j];
		off[j] = t;
	}
	for(i = 0; i < n; i++)
		if(pwrite(fd, buf, Bufsz, off[i] + vrand(100)) != Bufsz)
			sysfatal("write: %r");
	close(fd);
	free(off);
	return b->i0/MiB;
}

vlong
rdfile_la(Bench *b)
{
	char path[128], buf[Bufsz];
	vlong i, rep;
	int fd;

	if(b->id == -1)
		snprint(path, sizeof(path), "%s", b->s0);
	else if(b->i1 != 0)
		snprint(path, sizeof(path), "%s.%lld", b->s0, b->id % b->i1);
	else
		snprint(path, sizeof(path), "%s.%d", b->s0, b->id);
	if((fd = open(path, OREAD)) == -1)
		sysfatal("open: %r");
	for(rep = 0; rep < b->reps; rep++){
		seek(fd, 0, 0);
		for(i = 0; i < b->i0; i += Bufsz)
			if(read(fd, buf, Bufsz) != Bufsz)
				sysfatal("write: %r");
	}
	close(fd);
	return b->reps*(b->i0/MiB);
}
vlong
rdfile_ra(Bench *b)
{
	char path[128], buf[Bufsz];
	vlong i, rep;
	uvlong off;
	int fd;

	if(b->id == -1)
		snprint(path, sizeof(path), "%s", b->s0);
	else if(b->i1 != 0)
		snprint(path, sizeof(path), "%s.%lld", b->s0, b->id % b->i1);
	else
		snprint(path, sizeof(path), "%s.%d", b->s0, b->id);
	if((fd = open(path, OREAD)) == -1)
		sysfatal("open: %r");
	for(rep = 0; rep < b->reps; rep++){
		seek(fd, 0, 0);
		for(i = 0; i < b->i0; i += Bufsz){
			off = vrand(b->i0-Bufsz) & ~((vlong)Bufsz-1);
			if(pread(fd, buf, Bufsz, off) != Bufsz)
				sysfatal("write: %r");
		}
	}
	close(fd);
	return b->reps*(b->i0/MiB);
}

vlong
rwfile_lala(Bench *b)
{
	char buf[64];
	Bench bb;

	bb = *b;
	if(b->id >= b->i1)
		return rdfile_la(&bb);
	else{
		snprint(buf, sizeof(buf), "%s%d.w%d", b->s0, getpid(), b->id);
		bb.s0 = buf;
		return wrfile_la(&bb);
	}
}

vlong
rdfile_rr(Bench *b)
{
	char path[128], buf[Bufsz];
	vlong i, rep;
	uvlong off;
	int fd;

	if(b->id == -1)
		snprint(path, sizeof(path), "%s", b->s0);
	else if(b->i1 != 0)
		snprint(path, sizeof(path), "%s.%lld", b->s0, b->id % b->i1);
	else
		snprint(path, sizeof(path), "%s.%d", b->s0, b->id);
	if((fd = open(path, OREAD)) == -1)
		sysfatal("open: %r");
	for(rep = 0; rep < b->reps; rep++){
		for(i = 0; i < b->i0; i += Bufsz){
			off = vrand(b->i0-Bufsz);
			if(pread(fd, buf, Bufsz, off) != Bufsz)
				sysfatal("read: %r");
		}
	}
	close(fd);
	return b->reps*(b->i0/MiB);
}

vlong
createflat(Bench *b)
{
	char buf[Bufsz];
	int i, fd;

	for(i = 0; i < b->i0; i++){
		snprint(buf, sizeof(buf), "%s%d", b->s0, i);
		if((fd = create(buf, OWRITE, 0666)) == -1)
			sysfatal("create: %r");
		if(b->i1 != 0)
			write(fd, buf, b->i1);
		close(fd);
	}
	return b->i0;
}

int
createlevel(int n, int d)
{
	char buf[Bufsz];
	int i, s, fd;

	s = 0;
	for(i = 0; i < n; i++){
		snprint(buf, sizeof(buf), "%d", i);
		if(d > 0){
			if((fd = create(buf, OWRITE, 0777|DMDIR)) == -1)
				sysfatal("create: %r");
			if(chdir(buf) == -1)
				sysfatal("chdir %s: %r", buf);
			s += createlevel(n, d-1);
			chdir("..");
		}else{
			if((fd = create(buf, OWRITE, 0666)) == -1)
				sysfatal("create: %r");
			s++;
		}
		close(fd);
	}
	return s;
}

vlong
createhier(Bench *b)
{
	return createlevel(b->i0, b->i1);
}

vlong
listfiles(Bench *b)
{
	char buf[Bufsz];
	int i, r, fd;

	for(i = 0; i < b->reps; i++){
		if((fd = open(".", OREAD)) == -1)
			sysfatal("open .: %r");
		while(1){
			if((r = read(fd, buf, sizeof(buf))) == -1)
				sysfatal("read: %r");
			if(r == 0)
				break;
		}
		close(fd);
	}
	return b->reps*b->i0;
}

vlong
randopen(Bench *b)
{
	char buf[Bufsz];
	int i, fd;

	for(i = 0; i < b->reps; i++){
		snprint(buf, sizeof(buf), "%s%lld", b->s0, vrand(b->i0));
		if((fd = open(buf, OREAD)) == -1)
			sysfatal("open: %r");
		if(b->i0)
			if(read(fd, buf, sizeof(buf)) == -1)
				sysfatal("read: %r");
		close(fd);	
	}
	return b->reps;
}

void
launch(void *p)
{
	Bench *b;
	vlong r;

	b = p;
	r = b->fn(b);
	send(b->rc, &r);
}

vlong
runpar(Bench *b)
{
	vlong r, sum;
	Bench *sub;
	int i;
	
	sum = 0;
	b->rc = chancreate(sizeof(vlong), b->nproc);
	if((sub = calloc(b->nproc, sizeof(Bench))) == nil)
		sysfatal("malloc: %r");
	for(i = 0; i < b->nproc; i++){
		sub[i] = *b;
		sub[i].id = i;
		proccreate(launch, &sub[i], mainstacksize);
	}
	for(i = 0; i < b->nproc; i++){
		recv(b->rc, &r);
		sum += r;
	}
	free(sub);
	return sum;
}

void
runbench(Bench *b, int nb)
{
	char *unit[] = {"ns", "us", "ms", "s"};
	double oc, dt;
	vlong t0, t1;
	int i, j;

	for(i = 0; i < nb; i++){
		if(b[i].reps == 0)
			b[i].reps = 1;
		print("%20s:\t", b[i].name);
		t0 = nsec();
		if(b[i].nproc <= 1){
			b[i].id = -1;
			oc = b[i].fn(&b[i]);
		}else
			oc = runpar(&b[i]);
		t1 = nsec();
		dt = (t1 - t0);
		for(j = 0; j < nelem(unit)-1; j++)
			if(dt/100 < 1)
				break;
			else
				dt /= 1000.0;
		print("%f%s (%f %s/%s)\n", dt, unit[j], (double)oc/dt, b[i].unit, unit[j]);
	}
}

void
threadmain(int argc, char **argv)
{
	Bench marks[] = {
		/* l => linear, a => aligned, r => random */
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.0"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.1"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.2"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.3"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.4"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.5"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.6"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.7"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.8"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.9"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.10"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.11"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.12"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.13"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.14"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.15"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.16"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.17"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.18"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.19"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.20"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.21"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.22"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.23"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.24"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.25"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.26"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.27"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.28"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.29"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.30"},
		{.name="wrcached_la",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="cached0.31"},

		{.name="rdcached_lala",	.i0=128*MiB,	.reps=2, .unit="MiB", .fn=rdfile_la, .s0="cached0"},
		{.name="rdcached_lara",	.i0=128*MiB,	.reps=2, .unit="MiB", .fn=rdfile_ra, .s0="cached0"},
		{.name="rdcached_larr",	.i0=128*MiB,	.reps=2, .unit="MiB", .fn=rdfile_rr, .s0="cached0"},

		{.name="rdcached_lala_2p",	.i0=128*MiB,	.reps=2, .unit="MiB", .fn=rdfile_la, .s0="cached0", .nproc=2},
		{.name="rdcached_lara_2p",	.i0=128*MiB,	.reps=2, .unit="MiB", .fn=rdfile_ra, .s0="cached0", .nproc=2},
		{.name="rdcached_larr_2p",	.i0=128*MiB,	.reps=2, .unit="MiB", .fn=rdfile_rr, .s0="cached0", .nproc=2},

		{.name="rdcached_lala_4p",	.i0=128*MiB,	.reps=2, .unit="MiB", .fn=rdfile_la, .s0="cached0", .nproc=4},
		{.name="rdcached_lara_4p",	.i0=128*MiB,	.reps=2, .unit="MiB", .fn=rdfile_ra, .s0="cached0", .nproc=4},
		{.name="rdcached_larr_4p",	.i0=128*MiB,	.reps=2, .unit="MiB", .fn=rdfile_rr, .s0="cached0", .nproc=4},

		{.name="rdcached_lala_8p",	.i0=128*MiB,	.reps=2, .unit="MiB", .fn=rdfile_la, .s0="cached0", .nproc=8},
		{.name="rdcached_lara_8p",	.i0=128*MiB,	.reps=2, .unit="MiB", .fn=rdfile_ra, .s0="cached0", .nproc=8},
		{.name="rdcached_larr_8p",	.i0=128*MiB,	.reps=2, .unit="MiB", .fn=rdfile_rr, .s0="cached0", .nproc=8},

		{.name="rdcached_lala_12p",	.i0=128*MiB,	.reps=2, .unit="MiB", .fn=rdfile_la, .s0="cached0", .nproc=12},
		{.name="rdcached_lara_12p",	.i0=128*MiB,	.reps=2, .unit="MiB", .fn=rdfile_ra, .s0="cached0", .nproc=12},
		{.name="rdcached_larr_12p",	.i0=128*MiB,	.reps=2, .unit="MiB", .fn=rdfile_rr, .s0="cached0", .nproc=12},

		{.name="rdcached_lala_16p",	.i0=128*MiB,	.reps=2, .unit="MiB", .fn=rdfile_la, .s0="cached0", .nproc=16},
		{.name="rdcached_lara_16p",	.i0=128*MiB,	.reps=2, .unit="MiB", .fn=rdfile_ra, .s0="cached0", .nproc=16},
		{.name="rdcached_larr_16p",	.i0=128*MiB,	.reps=2, .unit="MiB", .fn=rdfile_rr, .s0="cached0", .nproc=16},

		{.name="rdcached_lala_16p",	.i0=128*MiB,	.reps=2, .unit="MiB", .fn=rdfile_la, .s0="cached0", .nproc=16},
		{.name="rdcached_lara_16p",	.i0=128*MiB,	.reps=2, .unit="MiB", .fn=rdfile_ra, .s0="cached0", .nproc=16},
		{.name="rdcached_larr_16p",	.i0=128*MiB,	.reps=2, .unit="MiB", .fn=rdfile_rr, .s0="cached0", .nproc=16},

		{.name="rdcached_lala_16p_1f",	.i0=128*MiB,	.reps=2, .unit="MiB", .fn=rdfile_la, .s0="cached0", .nproc=16, .i1=1},
		{.name="rdcached_lara_16p_8f",	.i0=128*MiB,	.reps=2, .unit="MiB", .fn=rdfile_ra, .s0="cached0", .nproc=16, .i1=8},
		{.name="rdcached_larr_32p_16f",	.i0=128*MiB,	.reps=2, .unit="MiB", .fn=rdfile_rr, .s0="cached0", .nproc=32, .i1=16},
		{.name="rdcached_larr_32p_32f",	.i0=128*MiB,	.reps=2, .unit="MiB", .fn=rdfile_rr, .s0="cached0", .nproc=32, .i1=32},

		{.name="wrcached_ra",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_ra, .s0="cached1"},
		{.name="rdcached_rala",	.i0=128*MiB,	.reps=10, .unit="MiB", .fn=rdfile_la, .s0="cached1"},
		{.name="rdcached_rara",	.i0=128*MiB,	.reps=10, .unit="MiB", .fn=rdfile_ra, .s0="cached1"},
		{.name="rdcached_rarr",	.i0=128*MiB,	.reps=10, .unit="MiB", .fn=rdfile_rr, .s0="cached1"},

		{.name="wrcached_rr",	.i0=128*MiB,	.reps=1, .unit="MiB", .fn=wrfile_rr, .s0="cached2"},
		{.name="rdcached_rrla",	.i0=128*MiB,	.reps=10, .unit="MiB", .fn=rdfile_la, .s0="cached2"},
		{.name="rdcached_rrra",	.i0=128*MiB,	.reps=10, .unit="MiB", .fn=rdfile_ra, .s0="cached2"},
		{.name="rdcached_rrrr",	.i0=128*MiB,	.reps=10, .unit="MiB", .fn=rdfile_rr, .s0="cached2"},

		{.name="rwcached_la_r0_w2_w",	.i0=64*MiB,	.reps=10, .unit="MiB", .fn=rwfile_lala, .s0="cached0", .nproc=2, .i1=2},
		{.name="rwcached_la_r0_w4_w",	.i0=64*MiB,	.reps=10, .unit="MiB", .fn=rwfile_lala, .s0="cached0", .nproc=4, .i1=4},
		{.name="rwcached_la_r1_w1_w",	.i0=64*MiB,	.reps=10, .unit="MiB", .fn=rwfile_lala, .s0="cached0", .nproc=2, .i1=1},
		{.name="rwcached_la_r3_w1_w",	.i0=64*MiB,	.reps=10, .unit="MiB", .fn=rwfile_lala, .s0="cached0", .nproc=4, .i1=1},
		{.name="rwcached_la_r2_w2_w",	.i0=64*MiB,	.reps=10, .unit="MiB", .fn=rwfile_lala, .s0="cached0", .nproc=4, .i1=2},
		{.name="rwcached_la_r6_w2_w",	.i0=64*MiB,	.reps=10, .unit="MiB", .fn=rwfile_lala, .s0="cached0", .nproc=8, .i1=2},
		{.name="rwcached_la_r4_w4_w",	.i0=64*MiB,	.reps=10, .unit="MiB", .fn=rwfile_lala, .s0="cached0", .nproc=8, .i1=4},

		{.name="rwcached_la_r1_w1_r",	.i0=64*MiB,	.reps=10, .unit="MiB", .fn=rwfile_lala, .s0="cached0", .nproc=2, .i1=1},
		{.name="rwcached_la_r2_w1_r",	.i0=64*MiB,	.reps=10, .unit="MiB", .fn=rwfile_lala, .s0="cached0", .nproc=3, .i1=1},
		{.name="rwcached_la_r3_w1_r",	.i0=64*MiB,	.reps=10, .unit="MiB", .fn=rwfile_lala, .s0="cached0", .nproc=4, .i1=1},
		{.name="rwcached_la_r1_w2_r",	.i0=64*MiB,	.reps=10, .unit="MiB", .fn=rwfile_lala, .s0="cached0", .nproc=4, .i1=2},
		{.name="rwcached_la_r1_w2_r",	.i0=64*MiB,	.reps=10, .unit="MiB", .fn=rwfile_lala, .s0="cached0", .nproc=3, .i1=2},
		{.name="rwcached_la_r2_w2_r",	.i0=64*MiB,	.reps=10, .unit="MiB", .fn=rwfile_lala, .s0="cached0", .nproc=4, .i1=2},
		{.name="rwcached_la_r4_w2_r",	.i0=64*MiB,	.reps=10, .unit="MiB", .fn=rwfile_lala, .s0="cached0", .nproc=4, .i1=2},
		{.name="rwcached_la_r6_w2_r",	.i0=64*MiB,	.reps=10, .unit="MiB", .fn=rwfile_lala, .s0="cached0", .nproc=8, .i1=2},
		{.name="rwcached_la_r4_w4_r",	.i0=64*MiB,	.reps=10, .unit="MiB", .fn=rwfile_lala, .s0="cached0", .nproc=8, .i1=4},

		{.name="createflat",	.i0=100*1000,	.reps=1, 	.unit="files", .fn=createflat, .s0="cz"},
		{.name="write1flat",	.i0=100*1000,	.reps=1, 	.unit="files", .fn=createflat, .i1=1, .s0="c1"},
		{.name="write100flat",	.i0=100*1000,	.reps=1, 	.unit="files", .fn=createflat, .i1=100, .s0="c100"},
		{.name="write1027flat",	.i0=100*1000,	.reps=1, 	.unit="files", .fn=createflat, .i1=1027, .s0="c1027"},
		{.name="listfflat",	.i0=100*1000,	.reps=10,	.unit="files", .fn=listfiles},
		{.name="openfflat",	.i0=100*1000,	.reps=100*1000, .unit="files", .fn=randopen, .s0="cz"},
		{.name="read0flat",	.i0=100*1000,	.reps=100*1000, .unit="files", .fn=randopen, .i1=1, .s0="cz"},
		{.name="read1flat",	.i0=100*1000,	.reps=100*1000, .unit="files", .fn=randopen, .i1=1, .s0="c1"},
		{.name="read100flat",	.i0=100*1000,	.reps=100*1000, .unit="files", .fn=randopen, .i1=1, .s0="c100"},
		{.name="read1027flat",	.i0=100*1000,	.reps=100*1000, .unit="files", .fn=randopen, .i1=1, .s0="c1027"},

//		{.name="rwcached_lara",	.i0=512*MiB,	.reps=10, .unit="MiB", .fn=rwfile_la, .s0="cached0", .i0=1, .i1=3},
//		{.name="rwcached_la",	.i0=512*MiB,	.reps=10, .unit="MiB", .fn=rwfile_la, .s0="cached0", .i0=3, .i1=3},
//		{.name="rwcached_la",	.i0=512*MiB,	.reps=10, .unit="MiB", .fn=rwfile_la, .s0="cached0", .i0=10, .i1=10},


		{.name="wrlarge_la",	.i0=16*GiB,	.reps=1, .unit="MiB", .fn=wrfile_la, .s0="large0"},
		{.name="rdlarge_lala",	.i0=16*GiB,	.reps=1, .unit="MiB", .fn=rdfile_la, .s0="large0"},
		{.name="rdlarge_lara",	.i0=16*GiB,	.reps=1, .unit="MiB", .fn=rdfile_ra, .s0="large0"},
//		{.name="rdlarge_larr",	.i0=16*GiB,	.reps=1, .unit="MiB", .fn=rdfile_rr, .s0="large0"},

		{.name="wrlarge_ra",	.i0=16*GiB,	.reps=1, .unit="MiB", .fn=wrfile_ra, .s0="large1"},
		{.name="rdlarge_lara",	.i0=16*GiB,	.reps=1, .unit="MiB", .fn=rdfile_la, .s0="large1"},
		{.name="rdlarge_rara",	.i0=16*GiB,	.reps=1, .unit="MiB", .fn=rdfile_ra, .s0="large1"},
//		{.name="rdlarge_rarr",	.i0=16*GiB,	.reps=1, .unit="MiB", .fn=rdfile_rr, .s0="large1"},

//		{.name="wrlarge_rr",	.i0=16*GiB,	.reps=1, .unit="MiB", .fn=wrfile_rr, .s0="large2"},
//		{.name="rdlarge_larr",	.i0=16*GiB,	.reps=1, .unit="MiB", .fn=rdfile_la, .s0="large2"},
//		{.name="rdlarge_rarr",	.i0=16*GiB,	.reps=1, .unit="MiB", .fn=rdfile_ra, .s0="large2"},
//		{.name="rdlarge_rrrr",	.i0=16*GiB,	.reps=1, .unit="MiB", .fn=rdfile_rr, .s0="large2"},

//		{.name="createheir",	.i0=3, .i1=10,	.reps=1, .unit="files", .fn=createhier},
//		{.name="openheir",	.i0=3, .i1=10,	.reps=1, .unit="files", .fn=randwalk},
	};

	ARGBEGIN{
	}ARGEND;
	
	if(argc != 1){
		fprint(2, "usage: %s wdir\n", argv0);
		exits("usage");
	}
	if(chdir(argv[0]) == -1)
		sysfatal("chdir: %r");
	runbench(marks, nelem(marks));
	exits(nil);
}
