#ifndef JOHN_RAR5_COMMON_H
#define JOHN_RAR5_COMMON_H

#define SIZE_SALT50 16
#define SIZE_PSWCHECK 8
#define SIZE_PSWCHECK_CSUM 4
#define SIZE_INITV 16

#define FORMAT_TAG  		"$rar5$"
#define TAG_LENGTH  		6

#define BINARY_SIZE		SIZE_PSWCHECK

#define SHA256_DIGEST_SIZE	32
#define MaxSalt			64

#include "formats.h"
#include "memdbg.h"

#define  Min(x,y) (((x)<(y)) ? (x):(y))

static struct fmt_tests tests[] = {
	{"$rar5$16$37526a0922b4adcc32f8fed5d51bb6c8$15$8955617d9b801def51d734095bb8ecdb$8$9f0b23c98ebb3653", "password"},
	/* "-p mode" test vectors */
	{"$rar5$16$92373e6493111cf1f2443dcd82122af9$15$011a3192b2f637d43deba9d0a08b7fa0$8$6862fcec47944d14", "openwall"},
	{"$rar5$16$92373e6493111cf1f2443dcd82122af9$15$a3af5246dd171431ac823cc79567e77e$8$16015b087c86964b", "password"},
	/* from CMIYC 2014 contest */
	{"$rar5$16$ed9bd88cc649bd06bfd7dc418fcf5fbd$15$21771e718815d6f23073ea294540ce94$8$92c584bec0ad2979", "rar"}, // 1798815729.rar
	{"$rar5$16$ed9bd88cc649bd06bfd7dc418fcf5fbd$15$21771e718815d6f23073ea294540ce94$8$5c4361e549c999e1", "win"}, // 844013895.rar
	{NULL}
};

static ARCH_WORD_32 (*crypt_out)[BINARY_SIZE / sizeof(ARCH_WORD_32)];

static struct custom_salt {
	//int version;
	//int hp;
	int saltlen;
	//int ivlen;
	unsigned int iterations;
	unsigned char salt[32];
	//unsigned char iv[32];
} *cur_salt;

static int valid(char *ciphertext, struct fmt_main *self)
{
	char *ctcopy, *keeptr, *p;
	int len;

	if (strncmp(ciphertext, FORMAT_TAG, TAG_LENGTH) != 0)
		return 0;

	ctcopy = strdup(ciphertext);
	keeptr = ctcopy;
	ctcopy += TAG_LENGTH;
	if ((p = strtokm(ctcopy, "$")) == NULL) // salt length
		goto err;
	len = atoi(p);
	if(len > 32)
		goto err;
	if ((p = strtokm(NULL, "$")) == NULL) // salt
		goto err;
	if (strlen(p) != len * 2)
		goto err;
	if ((p = strtokm(NULL, "$")) == NULL) // iterations (in log2)
		goto err;
	if(atoi(p) > 24) // CRYPT5_KDF_LG2_COUNT_MAX
		goto err;
	if ((p = strtokm(NULL, "$")) == NULL) // AES IV
		goto err;
	if (strlen(p) != SIZE_INITV * 2)
		goto err;
	if ((p = strtokm(NULL, "$")) == NULL) // pswcheck len (redundant)
		goto err;
	len = atoi(p);
	if(len != BINARY_SIZE)
		goto err;
	if ((p = strtokm(NULL, "$")) == NULL) // pswcheck
		goto err;
	if(strlen(p) != BINARY_SIZE * 2)
		goto err;

	MEM_FREE(keeptr);
	return 1;

err:
	MEM_FREE(keeptr);
	return 0;
}

static void *get_salt(char *ciphertext)
{
	char *ctcopy = strdup(ciphertext);
	char *keeptr = ctcopy;
	char *p;
	int i;
	static struct custom_salt cs;

	memset(&cs, 0, sizeof(cs));
	ctcopy += TAG_LENGTH;
	p = strtokm(ctcopy, "$");
	cs.saltlen = atoi(p);
	p = strtokm(NULL, "$");
	for (i = 0; i < cs.saltlen; i++)
		cs.salt[i] = atoi16[ARCH_INDEX(p[i * 2])] * 16
			+ atoi16[ARCH_INDEX(p[i * 2 + 1])];
	p = strtokm(NULL, "$");
	cs.iterations = 1 << atoi(p);
	p = strtokm(NULL, "$");
/* We currently do not use the IV */
#if 0
	for (i = 0; i < SIZE_INITV; i++)
		cs.iv[i] = atoi16[ARCH_INDEX(p[i * 2])] * 16
			+ atoi16[ARCH_INDEX(p[i * 2 + 1])];
#endif
	MEM_FREE(keeptr);

#ifdef RARDEBUG
	fprintf(stderr, "get_salt len %d iter %d\n", cs.saltlen, cs.iterations);
	dump_stuff(cs.salt, SIZE_SALT50);
#endif
	return (void *)&cs;
}

static void *get_binary(char *ciphertext)
{
	static union {
		unsigned char c[BINARY_SIZE];
		ARCH_WORD dummy;
		ARCH_WORD_32 swap[1];
	} buf;
	unsigned char *out = buf.c;
	char *p;
	int i;

	p = strrchr(ciphertext, '$') + 1;
	for (i = 0; i < BINARY_SIZE; i++) {
		out[i] =
		    (atoi16[ARCH_INDEX(*p)] << 4) |
		    atoi16[ARCH_INDEX(p[1])];
		p += 2;
	}
#if (ARCH_LITTLE_ENDIAN==0)
	for (i = 0; i < sizeof(buf.c)/4; ++i) {
		buf.swap[i] = JOHNSWAP(buf.swap[i]);
	}
#endif
	return out;
}

static int cmp_all(void *binary, int count)
{
	int index = 0;
	for (; index < count; index++)
		if (!memcmp(binary, crypt_out[index], BINARY_SIZE))
			return 1;
	return 0;
}

static int cmp_one(void *binary, int index)
{
	return !memcmp(binary, crypt_out[index], BINARY_SIZE);
}

static int cmp_exact(char *source, int index)
{
	return 1;
}

static int get_hash_0(int index)
{
#ifdef RARDEBUG
	dump_stuff_msg("get_hash", crypt_out[index], BINARY_SIZE);
#endif
	return crypt_out[index][0] & 0xf;
}
static int get_hash_1(int index) { return crypt_out[index][0] & 0xff; }
static int get_hash_2(int index) { return crypt_out[index][0] & 0xfff; }
static int get_hash_3(int index) { return crypt_out[index][0] & 0xffff; }
static int get_hash_4(int index) { return crypt_out[index][0] & 0xfffff; }
static int get_hash_5(int index) { return crypt_out[index][0] & 0xffffff; }
static int get_hash_6(int index) { return crypt_out[index][0] & 0x7ffffff; }

#if FMT_MAIN_VERSION > 11
static unsigned int iteration_count(void *salt)
{
	struct custom_salt *my_salt;

	my_salt = salt;
	return (unsigned int)my_salt->iterations;
}
#endif

#endif /* JOHN_RAR5_COMMON_H */
