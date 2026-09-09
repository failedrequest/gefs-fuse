#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <avl.h>
#include <bio.h>

#include "dat.h"
#include "fns.h"

typedef struct Cmd	Cmd;
typedef struct Sizes	Sizes;

struct Cmd {
	char	*name;
	char	*sub;
	int	minarg;
	int	maxarg;
	int	epoch;
	void	(*fn)(int, char**, int);
};

struct Sizes {
	vlong	datasz;
	vlong	metasz;
	vlong	delqsz;
	vlong	clobsz;
};


static double
hscaled(vlong sz, char **unit)
{
	static char *units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB", nil};
	double hsz;
	int u;

	hsz = sz;
	for(u = 0; u < nelem(units)-1 && hsz >= 500 ; u++)
		hsz /= 1024;
	*unit = units[u];
	return hsz;
}


static void
setdbg(int fd, char **ap, int na)
{
	debug = (na == 1) ? atoi(ap[0]) : !debug;
	fprint(fd, "debug → %d\n", debug);
}

static void
sendsync(int fd, int halt)
{
	Amsg *a;

	a = mallocz(sizeof(Amsg), 1);
	if(a == nil){
		fprint(fd, "alloc sync msg: %r\n");
		free(a);
		return;
	}
	a->op = AOsync;
	a->halt = halt;
	a->fd = fd;
	chsend(fs->admchan, a);		
}

static void
syncfs(int fd, char **, int)
{
	sendsync(fd, 0);
	fprint(fd, "synced\n");
}

static void
haltfs(int fd, char **, int)
{
	sendsync(fd, 1);
	fprint(fd, "gefs: ending...\n");
}

static void
listsnap(int fd)
{
	char pfx[Snapsz];
	Scan s;
	uint flg;
	int sz;

	pfx[0] = Klabel;
	sz = 1;
	btnewscan(&s, pfx, sz);
	btenter(&fs->snap, &s);
	while(1){
		if(!btnext(&s, &s.kv))
			break;
		flg = UNPACK32(s.kv.v+1+8);
		fprint(fd, "snap %.*s", s.kv.nk-1, s.kv.k+1);
		if(flg != 0)
			fprint(fd, " [");
		if(flg & Lmut)
			fprint(fd, " mutable");
		if(flg & Lauto)
			fprint(fd, " auto");
		if(flg & Ltsnap)
			fprint(fd, " tsnap");
		if(flg != 0)
			fprint(fd, " ]");
		fprint(fd, "\n");
	}
	btexit(&s);
}

static void
snapfs(int fd, char **ap, int na)
{
	Amsg *a;
	int i;

	if((a = mallocz(sizeof(Amsg), 1)) == nil){
		fprint(fd, "alloc sync msg: %r\n");
		return;
	}
	a->op = AOsnap;
	a->fd = fd;
	a->flag = Ltsnap;
	while(ap[0][0] == '-'){
		for(i = 1; ap[0][i]; i++){
			switch(ap[0][i]){
			case 'S':	a->flag &= ~Ltsnap;	break;
			case 'm':	a->flag |= Lmut;	break;
			case 'd':	a->delete++;		break;
			case 'l':
				listsnap(fd);
				free(a);
				return;
			default:
				fprint(fd, "usage: snap -[Smdl] [old [new]]\n");
				free(a);
				return;
			}
		}
		na--;
		ap++;
	}
	if(a->delete && na != 1 || !a->delete && na != 2){
		fprint(fd, "usage: snap -[md] old [new]\n");
		free(a);
		return;
	}
	if(na >= 1)
		strecpy(a->old, a->old+sizeof(a->old), ap[0]);
	if(na >= 2)
		strecpy(a->new, a->new+sizeof(a->new), ap[1]);
	sendsync(fd, 0);
	chsend(fs->admchan, a);
}

static void
fsckfs(int fd, char**, int)
{
	if(checkfs(fd))
		fprint(fd, "ok\n");
	else
		fprint(fd, "broken\n");
}

static void
refreshusers(int fd, char **, int)
{
	Mount *mnt;

	if((mnt = getmount("adm")) == nil){
		fprint(fd, "load users: missing 'adm'\n");
		return;
	}
	if(waserror()){
		fprint(fd, "load users: %s\n", errmsg());
		clunkmount(mnt);
		return;
	}
	loadusers(fd, mnt->root);
	fprint(fd, "refreshed users\n");
	clunkmount(mnt);
}

static void
showbstate(int fd, char**, int)
{
	char *p, fbuf[8];
	Blk *b;

	for(b = blkbuf; b != blkbuf+fs->cmax; b++){
		p = fbuf;
		if(b->flag & Bdirty)	*p++ = 'd';
		if(b->flag & Bfinal)	*p++ = 'f';
		if(b->flag & Bfreed)	*p++ = 'F';
		if(b->flag & Bcached)	*p++ = 'c';
		if(b->flag & Bqueued)	*p++ = 'q';
		if(b->flag & Blimbo)	*p++ = 'L';
		*p = 0;
		fprint(fd, "blk %#p type %d flag %s bp %B ref %ld alloc %#p queued %#p, hold %#p drop %#p cached %#p\n",
			b, b->type, fbuf, b->bp, b->ref, b->alloced, b->queued, b->lasthold, b->lastdrop, b->cached);
	}
}

static void
showusers(int fd, char**, int)
{
	User *u, *v;
	int i, j;
	char *sep;

	rlock(&fs->userlk);
	for(i = 0; i < fs->nusers; i++){
		u = &fs->users[i];
		fprint(fd, "%d:%s:", u->id, u->name);
		if((v = uid2user(u->lead)) == nil)
			fprint(fd, "???:");
		else
			fprint(fd, "%s:", v->name);
		sep = "";
		for(j = 0; j < u->nmemb; j++){
			if((v = uid2user(u->memb[j])) == nil)
				fprint(fd, "%s???", sep);
			else
				fprint(fd, "%s%s", sep, v->name);
			sep = ",";
		}
		fprint(fd, "\n");
	}
	runlock(&fs->userlk);
}

static void
countlog(int fd, Dlist *dl)
{
	Bptr bp, nb;
	Blk *b;
	int n;

	n = 0;
	for(bp = dl->hd; bp.addr != -1; bp = nb){
		if(waserror()){
			fprint(fd, "error loading %B\n", bp);
			return;
		}
		b = getblk(bp, 0);
		nb = b->logp;
		dropblk(b);
		poperror();
		n += b->logsz/8;
		n++;
	}
	fprint(fd, "\tDl(%lld, %lld): %d blocks\n", dl->gen, dl->bgen, n);
}


static void
prleak(int fd, uvlong *marks)
{
	vlong a0, a1, ba, bi, leaksz;
	Arena *a;
	Arange *r;

	if(marks == nil)
		return;
	leaksz = 0;
	for(a = &fs->arenas[0]; a < &fs->arenas[fs->narena]; a++){
		r = (Arange*)avlmin(a->free);
		a0 = a->h0->bp.addr + 2*Blksz;
		a1 = a->h0->bp.addr + a->size - 2*Blksz;
		for(ba = a0; ba < a1; ba += Blksz){
			if(r != nil && ba == r->off){
				for(; ba < r->off+r->len; ba += Blksz){
					bi = ba/Blksz;
					if(marks[bi/64] & 1ULL<<(bi%64))
						fprint(fd, "uaf %#llx\n", ba);
				}
				r = (Arange*)avlnext(r);
			}
			if(ba >= a1)
				break;
			bi = ba/Blksz;
			if((marks[bi/64] & 1ULL<<(bi%64)) == 0){
				leaksz += Blksz;
				fprint(fd, "leak %#llx\n", ba);
			}
		}
	}
	fprint(fd, "total bytes leaked: %lld (%f MiB)\n", leaksz, (double)leaksz/MiB);
}

static void
marktree(Tree *t, Blk *b, Sizes *ts, uvlong *marks)
{
	int i, fill;
	vlong bn;
	Bptr bp;
	Blk *c;
	Msg m;

	bn = b->bp.addr/Blksz;
	if(marks != nil)
		marks[bn/64] |= 1ULL<<(bn%64);
	ts->metasz += Blksz;
	switch(b->type){
	case Tleaf:
		for(i = 0; i < b->nval; i++){
			getval(b, i, &m);
			if(m.k[0] != Kdat)
				continue;
			bp = unpackbp(m.v, m.nv);
			bn = bp.addr/Blksz;
			if(marks != nil)
				marks[bn/64] |= 1ULL<<(bn%64);
			if(bp.gen <= t->pred)
				continue;
			ts->datasz += Blksz;
		}
		break;
	case Tpivot:
		for(i = 0; i < b->nval; i++){
			getval(b, i, &m);
			bp = getptr(&m, &fill);
			if(bp.gen <= t->pred)
				continue;
			c = getblk(bp, 0);
			marktree(t, c, ts, marks);
		}
		for(i = 0; i < b->nbuf; i++){
			getmsg(b, i, &m);
			if(m.k[0] != Kdat)
				continue;
			switch(m.op){
			case Odelete:	ts->delqsz += Blksz;	break;
			case Oclobber:	ts->clobsz += Blksz;	break;
			case Oclearb:	ts->clobsz += Blksz;	break;
			case Oinsert:
				bp = unpackbp(m.v, m.nv);
				bn = bp.addr/Blksz;
				if(marks != nil)
					marks[bn/64] |= 1ULL<<(bn%64);
				if(bp.gen > t->pred)
					ts->datasz += Blksz;
				break;
			}
		}
		break;
	}
}

static int
marklog(int arena, Bptr hd, uvlong *marks)
{
	Bptr bp, nb;
	vlong bn;
	Blk *b;

	bp = (Bptr){-1, -1, -1};
	for(bp = hd; bp.addr != -1; bp = nb){
tracex("marklog", bp, arena, -1);
		b = getblk(bp, 0);
		bn = b->bp.addr/Blksz;
		marks[bn/64] |= 1ULL<<(bn%64);
		nb = b->logp;
		dropblk(b);
	}
	return 1;
}

static int
markdlist(Bptr hd, uvlong *marks)
{
	Bptr bp, nb;
	vlong bn;
	char *p;
	Blk *b;

	bp = (Bptr){-1, -1, -1};
	for(bp = hd; bp.addr != -1; bp = nb){
		b = getblk(bp, 0);
		bn = b->bp.addr/Blksz;
		marks[bn/64] |= 1ULL<<(bn%64);
		for(p = b->data; p != b->data+b->logsz; p += 8){
			bn = UNPACK64(p);
			bn /= Blksz;
			marks[bn/64] |= 1ULL<<(bn%64);
		}
		nb = b->logp;
		dropblk(b);
	}
	return 1;
}

static int
markdlists(uvlong *marks)
{
	char pfx[1];
	Dlist dl;
	Scan s;

	markdlist(fs->snapdl.hd, marks);
	pfx[0] = Kdlist;
	btnewscan(&s, pfx, 1);
	btenter(&fs->snap, &s);
	while(1){
		if(!btnext(&s, &s.kv))
			break;
		kv2dlist(&s.kv, &dl);
		markdlist(dl.hd, marks);
	}
	btexit(&s);
	return 0;
}

static void
showsnapsz(int fd)
{
	char pfx[1], name[Keymax+1], *u;
	int i, h, ndone;
	uvlong *marks;
	vlong *done;
	vlong ba, bn, used, total;
	double sz;
	Limbo *l;
	Sizes ts;
	Tree *t;
	Scan s;
	Blk *b;


	done = nil;
	ndone = 0;
	total = 0;
	ba = fs->sb1->bp.addr/Blksz;
	marks = mallocz(sizeof(vlong)*(ba/64 + 1), 1);
	if(marks == nil)
		fprint(2, "not enough memory for leak detection\n");

	/* RACY, may crash */
	for(i = 0; i < 3; i++){
		for(l = fs->limbo[i]; l != nil; l = l->next){
			if(l->op == DFbp){
				bn = ((Bfree*)l)->bp.addr/Blksz;
				marks[bn/64] |= 1ULL<<(bn%64);
			}else if(l->op == DFblk){
				bn = ((Blk*)l)->bp.addr/Blksz;
				marks[bn/64] |= 1ULL<<(bn%64);
			}
		}
	}

	b = getroot(&fs->snap, &h);
	memset(&ts, 0, sizeof(Sizes));
	marktree(&fs->snap, b, &ts, marks);
	dropblk(b);

	pfx[0] = Klabel;
	btnewscan(&s, pfx, 1);
	btenter(&fs->snap, &s);
	while(1){
		if(!btnext(&s, &s.kv))
			break;
		if(waserror()){
			fprint(fd, "moving on: %s\n", errmsg());
			continue;
		}
		memcpy(name, s.kv.k+1, s.kv.nk-1);
		name[s.kv.nk-1] = 0;
		if((t = opensnap(name, nil)) == nil){
			fprint(2, "invalid snap label %s\n", name);
			break;
		}
		fprint(fd, "snap %s [gen %lld..%lld]:\n", name, t->pred+1, t->gen);
		for(i = 0; i < ndone; i++){
			if(done[i] == t->gen){
				fprint(fd, "\tdup\n");
				goto Next;
			}
		}
		done = realloc(done, (ndone+1)*sizeof(vlong));
		done[ndone++] = t->gen;

		b = getroot(t, &h);
		memset(&ts, 0, sizeof(Sizes));
		marktree(t, b, &ts, marks);

		used = ts.datasz + ts.metasz;
		sz = hscaled(used, &u);
		fprint(fd, "\tused %lld (%.2f %s)\n", used, sz, u);
		sz = hscaled(ts.datasz, &u);
		fprint(fd, "\tdata %lld (%.2f %s)\n", ts.datasz, sz, u);
		sz = hscaled(ts.metasz, &u);
		fprint(fd, "\tmeta %lld (%.2f %s)\n", ts.metasz, sz, u);
		sz = hscaled(ts.delqsz, &u);
		fprint(fd, "\tdelq %lld (%.2f %s)\n", ts.delqsz, sz, u);
		sz = hscaled(ts.clobsz, &u);
		fprint(fd, "\tclob %lld (%.2f %s)\n", ts.clobsz, sz, u);
		dropblk(b);
		total += used;
Next:
		closesnap(t);
		poperror();
	}
	btexit(&s);
	if(marks != nil){
		for(i = 0; i < fs->narena; i++)
			marklog(i, fs->arenas[i].loghd, marks);
		markdlists(marks);
	}
	sz = hscaled(total, &u);
	fprint(fd, "total used: %lld (%.2f %s)\n", total, sz, u);
	prleak(fd, marks);
	free(marks);
}

static void
showdf(int fd, char **ap, int na)
{
	vlong size, used, free;
	double hsize, hused, hfree;
	char *us, *uu, *uf;
	double pct;
	Arena *a;
	int i;

	size = 0;
	used = 0;
	for(i = 0; i < fs->narena; i++){
		a = &fs->arenas[i];
		qlock(a);
		size += a->size;
		used += a->used;
		qunlock(a);
		fprint(fd, "arena %d: %llx/%llx (%.2f%%)\n", i, a->used, a->size, 100*(double)a->used/(double)a->size);
	}
	free = size - used;
	hsize = hscaled(size, &us);
	hused = hscaled(used, &uu);
	hfree = hscaled(free, &uf);
	pct = 100.0*(double)used/(double)size;
	fprint(fd, "fill:\t%.2f%%\n", pct);
	fprint(fd, "used:\t%lld (%.2f %s)\n", used, hused, uu);
	fprint(fd, "size:\t%lld (%.2f %s)\n", size, hsize, us);
	fprint(fd, "free:\t%lld (%.2f %s)\n", free, hfree, uf);
	if(na == 1 && strcmp(ap[0], "verbose") == 0)
		showsnapsz(fd);
}

void
showfid(int fd, char**, int)
{
	int i;
	Fid *f;
	Conn *c;

	for(c = fs->conns; c != nil; c = c->next){
		fprint(fd, "-- conn %p: fids --\n", c);
		for(i = 0; i < Nfidtab; i++){
			lock(&c->fidtablk[i]);
			for(f = c->fidtab[i]; f != nil; f = f->next){
				rlock(f->dent);
				fprint(fd, "\tfid[%d] from %#zx: %d [refs=%ld, k=%K, qid=%Q m=%d, dmode:%d duid: %d, dgid: %d]\n",
					i, getmalloctag(f), f->fid, f->dent->ref, &f->dent->Key, f->dent->qid,
					f->mode, f->dmode, f->duid, f->dgid);
				runlock(f->dent);
			}
			unlock(&c->fidtablk[i]);
		}
	}
}

void
showtree(int fd, char **ap, int na)
{
	char *name;
	Tree *t;
	Blk *b;
	int h;

	name = "main";
	memset(&t, 0, sizeof(t));
	if(na == 1)
		name = ap[0];
	if(strcmp(name, "snap") == 0)
		t = &fs->snap;
	else if((t = opensnap(name, nil)) == nil){
		fprint(fd, "open %s: %r\n", name);
		return;
	}
	b = getroot(t, &h);
	fprint(fd, "=== [%s] %B @%d\n", name, t->bp, t->ht);
	showblk(fd, b, "contents", 1);
	dropblk(b);
	if(t != &fs->snap)
		closesnap(t);
}

static void
permflip(int fd, char **ap, int)
{
	if(strcmp(ap[0], "on") == 0)
		permissive = 1;
	else if(strcmp(ap[0], "off") == 0)
		permissive = 0;
	else
		fprint(2, "unknown permissive %s\n", ap[0]);
	fprint(fd, "permissive: %d → %d\n", !permissive, permissive);
}

static void
savetrace(int fd, char **ap, int na)
{
	Biobuf *bfd;
	Trace *t;
	int i;

	if(na == 0)
		bfd = Bfdopen(dup(fd, -1), OWRITE);
	else
		bfd = Bopen(ap[0], OWRITE);
	if(bfd == nil){
		fprint(fd, "error opening output");
		return;
	}
	for(i = 0; i < fs->ntrace; i++){
		t = &fs->trace[(fs->traceidx + i) % fs->ntrace];
		if(t->msg[0] == 0)
			continue;
		Bprint(bfd, "[%d@%d] %s", t->tid, t->qgen, t->msg);
		if(t->bp.addr != -1)
			Bprint(bfd, " %B", t->bp);
		if(t->v0 != -1)
			Bprint(bfd, " %llx", t->v0);
		if(t->v1 != -1)
			Bprint(bfd, " %llx", t->v1);
		Bprint(bfd, "\n");
	}
	Bterm(bfd);
	fprint(fd, "saved\n");
}

static void
showfree(int fd, char **, int)
{
	Arange *r;
	Arena *a;
	int i;

	for(i = 0; i < fs->narena; i++){
		a = &fs->arenas[i];
		qlock(a);
		fprint(fd, "arena %d %llx+%llx{\n", i, a->h0->bp.addr, a->size);
		for(r = (Arange*)avlmin(a->free); r != nil; r = (Arange*)avlnext(r))
			fprint(fd, "\t%llx..%llx (%llx)\n", r->off, r->off+r->len, r->len);
		fprint(fd, "}\n");
		qunlock(a);
	}
}

static void
unreserve(int fd, char **ap, int)
{
	if(strcmp(ap[0], "on") == 0)
		usereserve = 0;
	else if(strcmp(ap[0], "off") == 0)
		usereserve = 1;
	else
		fprint(2, "unknown reserve %s\n", ap[0]);
	fprint(fd, "reserve: %d → %d\n", !permissive, permissive);
}

static void
showbptr(int fd, char **ap, int na)
{
	Bptr bp;
	int i;

	for(i = 0; i < na; i++){
		bp.addr = strtoll(ap[i], nil, 0);
		bp.hash = -1;
		bp.gen = -1;
		showbp(fd, bp, 0);
	}
}

static void
help(int fd, char**, int)
{
	char *msg =
		"help -- show this help\n"
		"check -- check for consistency\n"
		"df -- show disk usage\n"
		"halt -- stop all writers, sync, and go read-only\n"
		"permit [on|off] -- switch to/from permissive mode\n"
		"reserve [on|off] -- enable block reserves\n"
		"snap -[Smdl] [old [new]] -- manage snapshots\n"
		"sync -- flush all pending writes to disk\n"
		"users -- reload user table from adm snapshot\n"
		"save trace [name] -- save a trace of recent activity\n"
		"show -- debug dumps\n"
		"	tree [name]\n"
		"	fid\n"
		"	users\n";
	fprint(fd, "%s", msg);
}

Cmd cmdtab[] = {
	/* admin */
	{.name="check",		.sub=nil,	.minarg=0, .maxarg=0, .fn=fsckfs, .epoch=1},
	{.name="df",		.sub=nil, 	.minarg=0, .maxarg=1, .fn=showdf, .epoch=1},
	{.name="halt",		.sub=nil,	.minarg=0, .maxarg=0, .fn=haltfs},
	{.name="help",		.sub=nil,	.minarg=0, .maxarg=0, .fn=help},
	{.name="permit",	.sub=nil,	.minarg=1, .maxarg=1, .fn=permflip},
	{.name="snap",		.sub=nil,	.minarg=1, .maxarg=3, .fn=snapfs},
	{.name="sync",		.sub=nil,	.minarg=0, .maxarg=0, .fn=syncfs},
	{.name="reserve",	.sub=nil,	.minarg=0, .maxarg=1, .fn=unreserve},
	{.name="users",		.sub=nil,	.minarg=0, .maxarg=1, .fn=refreshusers},

	/* debugging */
	{.name="show",		.sub="bp", 	.minarg=1, .maxarg=1, .fn=showbptr},
	{.name="show",		.sub="fid",	.minarg=0, .maxarg=0, .fn=showfid},
	{.name="show",		.sub="tree",	.minarg=0, .maxarg=1, .fn=showtree, .epoch=1},
	{.name="show",		.sub="users",	.minarg=0, .maxarg=0, .fn=showusers},
	{.name="show",		.sub="bstate",	.minarg=0, .maxarg=0, .fn=showbstate, .epoch=1},
	{.name="show",		.sub="free",	.minarg=0, .maxarg=0, .fn=showfree},
	{.name="debug",		.sub=nil,	.minarg=0, .maxarg=1, .fn=setdbg},
	{.name="save",		.sub="trace",	.minarg=0, .maxarg=1, .fn=savetrace},
	{.name=nil, .sub=nil},
};

void
runcons(int tid, void *pfd)
{
	char buf[256], *f[4], **ap;
	int i, n, nf, na, fd;
	Cmd *c;

	fd = (uintptr)pfd;
	while(1){
		fprint(fd, "gefs# ");
		if((n = read(fd, buf, sizeof(buf)-1)) == -1)
			break;
		buf[n] = 0;
		nf = tokenize(buf, f, nelem(f));
		if(nf == 0 || strlen(f[0]) == 0)
			continue;
		for(c = cmdtab; c->name != nil; c++){
			ap = f;
			na = nf;
			if(strcmp(c->name, *ap) != 0)
				continue;
			ap++;
			na--;
			if(c->sub != nil){
				if(na == 0 || strcmp(c->sub, *ap) != 0)
					continue;
				ap++;
				na--;
			}
			if(na < c->minarg || na > c->maxarg)
				continue;
			if(c->epoch)
				epochstart(tid);
			if(!waserror()){
				c->fn(fd, ap, na);
				poperror();
			}else
				fprint(fd, "%s: %s\n", f[0], errmsg());
			if(c->epoch)
				epochend(tid);
			break;
		}
		if(c->name == nil){
			fprint(fd, "unknown command '%s", f[0]);
			for(i = 1; i < nf; i++)
				fprint(fd, " %s", f[i]);
			fprint(fd, "'\n");
		}
	}
}
