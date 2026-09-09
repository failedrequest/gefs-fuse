#include <u.h>
#include <libc.h>

#include "../atomic.h"

void
test_aget(void)
{
	long lv = 42;
	vlong vv = 42;
	void *pv = &lv;

	assert(agetl(&lv) == 42);
	assert(agetv(&vv) == 42);
	assert(agetp(&pv) == &lv);
}

void
test_aset(void)
{
	long lv = 42;
	vlong vv = 42;
	void *pv = &lv;

	assert(asetl(&lv, 43) == 42);
	assert(asetv(&vv, 43) == 42);
	assert(asetp(&pv, &vv) == &lv);

	assert(lv == 43);
	assert(vv == 43);
	assert(pv == &vv);
}

void
test_ainc(void)
{
	long lv = 42;
	vlong vv = 42;

	assert(aincl(&lv, 1) == 43);
	assert(aincv(&vv, 1) == 43);
	assert(lv == 43);
	assert(vv == 43);
}

void
test_acas(void)
{
	long lv = 42;
	vlong vv = 42;
	void *pv = &lv;

	assert(!acasl(&lv, 1, 1));
	assert(!acasv(&vv, 1, 1));
	assert(!acasp(&pv, &vv, &pv));
	assert(acasl(&lv, 42, 1));
	assert(acasv(&vv, 42, 1));
	assert(acasp(&pv, &lv, &vv));

	assert(lv == 1);
	assert(vv == 1);
	assert(pv == &vv);
}

void
test_coherence(void)
{
	/* at least not an illegal instruction */
	coherence();
}

void
main(int, char**)
{
	test_aget();
	test_aset();
	test_ainc();
	test_acas();
	test_coherence();
	exits(nil);
}
