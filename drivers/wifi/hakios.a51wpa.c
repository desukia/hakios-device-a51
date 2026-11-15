/*
 * devices/a51/drivers/wifi/hakios.a51wpa.c: WPA2-Personal for hakios.a51wifi.c, the station's half of the 4-way
 * handshake (IEEE 802.11-2016 12.7.6) with CCMP: SHA-1, HMAC-SHA-1, PBKDF2 (the PMK from the passphrase), the PRF
 * (the PTK), the EAPOL-Key MIC (key descriptor version 2, HMAC-SHA-1-128) and AES key unwrap (RFC 3394, the GTK).
 *
 * Nothing in here touches the hardware or the kernel: it builds alone on the PC too (tools: a test against Python's
 * hashlib/hmac and an access point simulated the same way), which is how it was checked before the phone used it.
 */
#include <stdint.h>
#include <stddef.h>
#ifdef KERNEL
#include <sys/systm.h>
#else
#include <string.h>
#endif

/* (the declarations here, not in a header: only conf's files reach the kernel tree) */
typedef struct {
	/* set by the caller before the handshake */
	uint8_t pmk[32];
	uint8_t aa[6];                  /* the access point (authenticator) */
	uint8_t spa[6];                 /* this station (supplicant) */
	uint8_t snonce[32];             /* random */
	uint8_t rsn[64];                /* our RSN element, exactly as in the association request */
	size_t rsn_length;
	/* what the handshake found */
	int state;                      /* 0, 2 (message 2 sent), 4 (message 4 sent) */
	uint8_t anonce[32];
	uint8_t replay[8];
	int have_replay;
	uint8_t ptk[48];                /* KCK 0..15, KEK 16..31, TK 32..47 */
	uint8_t gtk[32];
	size_t gtk_length;
	int gtk_id;
	uint8_t gtk_rsc[8];
	const char *failure;
} a51wpa_t;

#define A51WPA_IGNORED  0               /* not for us, or a repeat: no reply */
#define A51WPA_M2       2               /* reply: message 2 */
#define A51WPA_M4       4               /* reply: message 4; the PTK's TK and the GTK are ready */
#define A51WPA_FAILED   -1              /* w->failure says why */

void a51wpa_pmk(const char *passphrase, size_t passphrase_length, const uint8_t *ssid, size_t ssid_length, uint8_t pmk[32]);
void a51wpa_ptk(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6], const uint8_t anonce[32],
    const uint8_t snonce[32], uint8_t ptk[48]);
void a51wpa_hmac_sha1(const uint8_t *key, size_t key_length, const uint8_t *const *piece, const size_t *piece_length,
    int pieces, uint8_t out[20]);
int a51wpa_unwrap(const uint8_t kek[16], const uint8_t *in, size_t length, uint8_t *out);

/* An EAPOL frame (from its version byte) from the access point: the reply to send (out, at most 256 bytes). */
int a51wpa_rx(a51wpa_t *w, const uint8_t *frame, size_t length, uint8_t *out, size_t *out_length);


/* ---- SHA-1 (FIPS 180-4) ---- */

typedef struct {
	uint32_t h[5];
	uint64_t length;
	uint8_t block[64];
	uint32_t used;
} sha1_t;

static uint32_t
rol(uint32_t x, int n)
{
	return (x << n) | (x >> (32 - n));
}

static void
sha1_block(sha1_t *s, const uint8_t *p)
{
	uint32_t w[80], a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];

	for (int i = 0; i < 16; i++) {
		w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
	}
	for (int i = 16; i < 80; i++) {
		w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	}
	for (int i = 0; i < 80; i++) {
		uint32_t f, k;
		if (i < 20) {
			f = (b & c) | (~b & d);
			k = 0x5a827999;
		} else if (i < 40) {
			f = b ^ c ^ d;
			k = 0x6ed9eba1;
		} else if (i < 60) {
			f = (b & c) | (b & d) | (c & d);
			k = 0x8f1bbcdc;
		} else {
			f = b ^ c ^ d;
			k = 0xca62c1d6;
		}
		uint32_t t = rol(a, 5) + f + e + k + w[i];
		e = d;
		d = c;
		c = rol(b, 30);
		b = a;
		a = t;
	}
	s->h[0] += a;
	s->h[1] += b;
	s->h[2] += c;
	s->h[3] += d;
	s->h[4] += e;
}

static void
sha1_init(sha1_t *s)
{
	s->h[0] = 0x67452301;
	s->h[1] = 0xefcdab89;
	s->h[2] = 0x98badcfe;
	s->h[3] = 0x10325476;
	s->h[4] = 0xc3d2e1f0;
	s->length = 0;
	s->used = 0;
}

static void
sha1_update(sha1_t *s, const uint8_t *p, size_t n)
{
	s->length += n;
	while (n > 0) {
		uint32_t take = 64 - s->used;
		if (take > n) {
			take = (uint32_t)n;
		}
		memcpy(s->block + s->used, p, take);
		s->used += take;
		p += take;
		n -= take;
		if (s->used == 64) {
			sha1_block(s, s->block);
			s->used = 0;
		}
	}
}

static void
sha1_final(sha1_t *s, uint8_t out[20])
{
	uint64_t bits = s->length * 8;
	uint8_t pad = 0x80, zero = 0, len[8];

	sha1_update(s, &pad, 1);
	while (s->used != 56) {
		sha1_update(s, &zero, 1);
	}
	for (int i = 0; i < 8; i++) {
		len[i] = (uint8_t)(bits >> (56 - 8 * i));
	}
	sha1_update(s, len, 8);
	for (int i = 0; i < 5; i++) {
		out[4 * i] = (uint8_t)(s->h[i] >> 24);
		out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
		out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
		out[4 * i + 3] = (uint8_t)s->h[i];
	}
}

/* ---- HMAC-SHA-1 over a list of pieces ---- */

void
a51wpa_hmac_sha1(const uint8_t *key, size_t key_length, const uint8_t *const *piece, const size_t *piece_length,
    int pieces, uint8_t out[20])
{
	uint8_t k[64], pad[64], inner[20];
	sha1_t s;

	memset(k, 0, sizeof(k));
	if (key_length > 64) {
		sha1_init(&s);
		sha1_update(&s, key, key_length);
		sha1_final(&s, k);
	} else {
		memcpy(k, key, key_length);
	}
	for (int i = 0; i < 64; i++) {
		pad[i] = k[i] ^ 0x36;
	}
	sha1_init(&s);
	sha1_update(&s, pad, 64);
	for (int i = 0; i < pieces; i++) {
		sha1_update(&s, piece[i], piece_length[i]);
	}
	sha1_final(&s, inner);
	for (int i = 0; i < 64; i++) {
		pad[i] = k[i] ^ 0x5c;
	}
	sha1_init(&s);
	sha1_update(&s, pad, 64);
	sha1_update(&s, inner, 20);
	sha1_final(&s, out);
}

static void
hmac1(const uint8_t *key, size_t key_length, const uint8_t *data, size_t length, uint8_t out[20])
{
	a51wpa_hmac_sha1(key, key_length, &data, &length, 1, out);
}

/* ---- PBKDF2-HMAC-SHA-1, 4096 rounds: the PMK of a passphrase and an SSID ---- */

void
a51wpa_pmk(const char *passphrase, size_t passphrase_length, const uint8_t *ssid, size_t ssid_length, uint8_t pmk[32])
{
	for (uint32_t block = 1; block <= 2; block++) {
		uint8_t salt[36], u[20], t[20];
		size_t n = ssid_length > 32 ? 32 : ssid_length;

		memcpy(salt, ssid, n);
		salt[n] = (uint8_t)(block >> 24);
		salt[n + 1] = (uint8_t)(block >> 16);
		salt[n + 2] = (uint8_t)(block >> 8);
		salt[n + 3] = (uint8_t)block;
		hmac1((const uint8_t *)passphrase, passphrase_length, salt, n + 4, u);
		memcpy(t, u, 20);
		for (int i = 1; i < 4096; i++) {
			hmac1((const uint8_t *)passphrase, passphrase_length, u, 20, u);
			for (int j = 0; j < 20; j++) {
				t[j] ^= u[j];
			}
		}
		memcpy(pmk + 20 * (block - 1), t, block == 1 ? 20 : 12);
	}
}

/* ---- the PRF (12.7.1.2): PRF-384 for a CCMP PTK (KCK 16, KEK 16, TK 16) ---- */

static void
prf(const uint8_t *key, size_t key_length, const char *label, const uint8_t *data, size_t data_length, uint8_t *out,
    size_t out_length)
{
	uint8_t zero = 0, counter, digest[20];
	size_t label_length = strlen(label);

	for (counter = 0; out_length > 0; counter++) {
		const uint8_t *piece[4] = { (const uint8_t *)label, &zero, data, &counter };
		size_t length[4] = { label_length, 1, data_length, 1 };
		a51wpa_hmac_sha1(key, key_length, piece, length, 4, digest);
		size_t n = out_length < 20 ? out_length : 20;
		memcpy(out, digest, n);
		out += n;
		out_length -= n;
	}
}

static int
less(const uint8_t *a, const uint8_t *b, size_t n)
{
	return memcmp(a, b, n) < 0;
}

void
a51wpa_ptk(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6], const uint8_t anonce[32],
    const uint8_t snonce[32], uint8_t ptk[48])
{
	uint8_t data[76];

	memcpy(data, less(aa, spa, 6) ? aa : spa, 6);
	memcpy(data + 6, less(aa, spa, 6) ? spa : aa, 6);
	memcpy(data + 12, less(anonce, snonce, 32) ? anonce : snonce, 32);
	memcpy(data + 44, less(anonce, snonce, 32) ? snonce : anonce, 32);
	prf(pmk, 32, "Pairwise key expansion", data, sizeof(data), ptk, 48);
}

/* ---- AES-128 (FIPS 197), its S-boxes worked out at first use; only what key unwrap needs: decrypt ---- */

static uint8_t sbox[256], inv_sbox[256];

static uint8_t
xtime(uint8_t x)
{
	return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
}

static uint8_t
gmul(uint8_t a, uint8_t b)
{
	uint8_t r = 0;

	while (b) {
		if (b & 1) {
			r ^= a;
		}
		a = xtime(a);
		b >>= 1;
	}
	return r;
}

static void
aes_tables(void)
{
	if (sbox[0] == 0x63) {
		return;
	}
	for (int i = 0; i < 256; i++) {
		/* the multiplicative inverse in GF(2^8) (0 for 0), then the affine map */
		uint8_t inv = 0;
		for (int j = 1; j < 256 && i != 0; j++) {
			if (gmul((uint8_t)i, (uint8_t)j) == 1) {
				inv = (uint8_t)j;
				break;
			}
		}
		uint8_t x = inv, y = inv;
		for (int k = 0; k < 4; k++) {
			y = (uint8_t)((y << 1) | (y >> 7));
			x ^= y;
		}
		sbox[i] = x ^ 0x63;
	}
	for (int i = 0; i < 256; i++) {
		inv_sbox[sbox[i]] = (uint8_t)i;
	}
}

static void
aes_expand(const uint8_t key[16], uint8_t rk[176])
{
	uint8_t rcon = 1;

	memcpy(rk, key, 16);
	for (int i = 16; i < 176; i += 4) {
		uint8_t t[4] = { rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1] };
		if (i % 16 == 0) {
			uint8_t u = t[0];
			t[0] = sbox[t[1]] ^ rcon;
			t[1] = sbox[t[2]];
			t[2] = sbox[t[3]];
			t[3] = sbox[u];
			rcon = xtime(rcon);
		}
		for (int j = 0; j < 4; j++) {
			rk[i + j] = rk[i - 16 + j] ^ t[j];
		}
	}
}

static void
aes_decrypt(const uint8_t rk[176], uint8_t b[16])
{
	for (int i = 0; i < 16; i++) {
		b[i] ^= rk[160 + i];
	}
	for (int round = 9; round >= 0; round--) {
		uint8_t t[16];
		/* inverse shift rows (column-major state: byte r + 4c), inverse sub bytes */
		for (int c = 0; c < 4; c++) {
			for (int r = 0; r < 4; r++) {
				t[r + 4 * ((c + r) % 4)] = inv_sbox[b[r + 4 * c]];
			}
		}
		for (int i = 0; i < 16; i++) {
			b[i] = t[i] ^ rk[16 * round + i];
		}
		if (round == 0) {
			break;
		}
		for (int c = 0; c < 4; c++) {
			uint8_t *s = b + 4 * c, a0 = s[0], a1 = s[1], a2 = s[2], a3 = s[3];
			s[0] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
			s[1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
			s[2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
			s[3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
		}
	}
}

/* AES key unwrap (RFC 3394): `length` bytes of wrapped data (a multiple of 8, at least 24) to length - 8. 0: good. */
int
a51wpa_unwrap(const uint8_t kek[16], const uint8_t *in, size_t length, uint8_t *out)
{
	uint8_t rk[176], a[8], b[16];
	size_t n = length / 8 - 1;

	if (length % 8 != 0 || length < 24) {
		return -1;
	}
	aes_tables();
	aes_expand(kek, rk);
	memcpy(a, in, 8);
	memcpy(out, in + 8, length - 8);
	for (int j = 5; j >= 0; j--) {
		for (size_t i = n; i >= 1; i--) {
			uint64_t t = (uint64_t)n * (uint64_t)j + i;
			memcpy(b, a, 8);
			for (int k = 0; k < 8; k++) {
				b[7 - k] ^= (uint8_t)(t >> (8 * k));
			}
			memcpy(b + 8, out + 8 * (i - 1), 8);
			aes_decrypt(rk, b);
			memcpy(a, b, 8);
			memcpy(out + 8 * (i - 1), b + 8, 8);
		}
	}
	for (int k = 0; k < 8; k++) {
		if (a[k] != 0xa6) {
			return -1;
		}
	}
	return 0;
}

/* ---- the handshake ---- */

/*
 * An EAPOL-Key frame, from the EAPOL header on: version 0, type 1 (3: key), body length 2 (big endian), descriptor 4
 * (2: RSN), key information 5, key length 7, replay counter 9, nonce 17, IV 49, RSC 65, reserved 73, MIC 81, key data
 * length 97, key data 99.
 */
#define K_INFO          5
#define K_LENGTH        7
#define K_REPLAY        9
#define K_NONCE         17
#define K_RSC           65
#define K_MIC           81
#define K_DATA_LENGTH   97
#define K_DATA          99

#define INFO_VERSION    0x0007
#define INFO_PAIRWISE   0x0008
#define INFO_INSTALL    0x0040
#define INFO_ACK        0x0080
#define INFO_MIC        0x0100
#define INFO_SECURE     0x0200
#define INFO_ENCRYPTED  0x1000

static uint16_t
be16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

static void
put_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void
mic(const uint8_t kck[16], uint8_t *frame, size_t length)
{
	uint8_t digest[20];

	memset(frame + K_MIC, 0, 16);
	hmac1(kck, 16, frame, length, digest);
	memcpy(frame + K_MIC, digest, 16);
}

/* Our reply: a key frame with this information, replay counter, nonce and key data. Returns its length. */
static size_t
reply(a51wpa_t *w, const uint8_t *request, uint16_t info, const uint8_t *nonce, const uint8_t *data,
    size_t data_length, uint8_t *out)
{
	size_t length = K_DATA + data_length;

	memset(out, 0, length);
	out[0] = request[0];                            /* the authenticator's EAPOL version */
	out[1] = 3;
	put_be16(out + 2, (uint16_t)(length - 4));
	out[4] = 2;
	put_be16(out + K_INFO, info);
	memcpy(out + K_REPLAY, request + K_REPLAY, 8);
	if (nonce != NULL) {
		memcpy(out + K_NONCE, nonce, 32);
	}
	put_be16(out + K_DATA_LENGTH, (uint16_t)data_length);
	memcpy(out + K_DATA, data, data_length);
	mic(w->ptk, out, length);
	return length;
}

int
a51wpa_rx(a51wpa_t *w, const uint8_t *frame, size_t length, uint8_t *out, size_t *out_length)
{
	*out_length = 0;
	if (length < K_DATA || frame[1] != 3 || (frame[4] != 2 && frame[4] != 254)) {
		return A51WPA_IGNORED;
	}
	uint16_t info = be16(frame + K_INFO), data_length = be16(frame + K_DATA_LENGTH);
	if ((size_t)K_DATA + data_length > length || !(info & INFO_PAIRWISE) || !(info & INFO_ACK)) {
		return A51WPA_IGNORED;
	}
	if ((info & INFO_VERSION) != 2) {
		w->failure = "key descriptor version is not 2 (HMAC-SHA-1/AES): not handled";
		return A51WPA_FAILED;
	}
	if (w->have_replay && memcmp(frame + K_REPLAY, w->replay, 8) <= 0) {
		return A51WPA_IGNORED;                  /* an old or repeated counter */
	}

	if (!(info & INFO_MIC)) {
		/* message 1: the ANonce. The PTK from it and our SNonce; message 2 carries our RSN element */
		memcpy(w->anonce, frame + K_NONCE, 32);
		a51wpa_ptk(w->pmk, w->aa, w->spa, w->anonce, w->snonce, w->ptk);
		memcpy(w->replay, frame + K_REPLAY, 8);
		w->have_replay = 1;
		*out_length = reply(w, frame, 2 | INFO_PAIRWISE | INFO_MIC, w->snonce, w->rsn, w->rsn_length, out);
		w->state = 2;
		return A51WPA_M2;
	}

	/* message 3: same ANonce, a good MIC, the GTK in the encrypted key data */
	if (w->state < 2 || memcmp(frame + K_NONCE, w->anonce, 32) != 0) {
		w->failure = "message 3 without message 1, or another ANonce";
		return A51WPA_FAILED;
	}
	uint8_t check[512], digest[16];
	if (length > sizeof(check)) {
		w->failure = "message 3 too long";
		return A51WPA_FAILED;
	}
	memcpy(check, frame, length);
	memcpy(digest, frame + K_MIC, 16);
	mic(w->ptk, check, length);
	if (memcmp(check + K_MIC, digest, 16) != 0) {
		w->failure = "message 3's MIC is wrong: the passphrase or the SSID does not match";
		return A51WPA_FAILED;
	}
	if (!(info & INFO_ENCRYPTED) || data_length < 24) {
		w->failure = "message 3 has no encrypted key data";
		return A51WPA_FAILED;
	}
	uint8_t plain[256];
	if ((size_t)data_length - 8 > sizeof(plain) || a51wpa_unwrap(w->ptk + 16, frame + K_DATA, data_length, plain) != 0) {
		w->failure = "message 3's key data does not unwrap";
		return A51WPA_FAILED;
	}
	/* the elements: the GTK KDE is dd len 00-0f-ac 01, then key id (bits 0-1), a reserved byte, the GTK */
	w->gtk_length = 0;
	for (size_t e = 0; e + 2 <= (size_t)data_length - 8 && e + 2 + plain[e + 1] <= (size_t)data_length - 8;
	    e += 2 + plain[e + 1]) {
		if (plain[e] == 0xdd && plain[e + 1] >= 6 + 16 && plain[e + 2] == 0x00 && plain[e + 3] == 0x0f &&
		    plain[e + 4] == 0xac && plain[e + 5] == 0x01) {
			w->gtk_id = plain[e + 6] & 3;
			w->gtk_length = plain[e + 1] - 6;
			if (w->gtk_length > sizeof(w->gtk)) {
				w->gtk_length = sizeof(w->gtk);
			}
			memcpy(w->gtk, plain + e + 8, w->gtk_length);
		}
		if (plain[e] == 0xdd && plain[e + 1] == 0) {
			break;                          /* padding */
		}
	}
	if (w->gtk_length == 0) {
		w->failure = "message 3 carries no GTK";
		return A51WPA_FAILED;
	}
	memcpy(w->gtk_rsc, frame + K_RSC, 8);
	memcpy(w->replay, frame + K_REPLAY, 8);
	*out_length = reply(w, frame, 2 | INFO_PAIRWISE | INFO_MIC | INFO_SECURE, NULL, NULL, 0, out);
	w->state = 4;
	return A51WPA_M4;
}

/* ---- for hakios.a51wifi.c: one handshake at a time, kept here (it needs no more than these) ---- */

static a51wpa_t the;

void
a51wpa_begin(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6], const uint8_t snonce[32],
    const uint8_t *rsn, size_t rsn_length)
{
	memset(&the, 0, sizeof(the));
	memcpy(the.pmk, pmk, 32);
	memcpy(the.aa, aa, 6);
	memcpy(the.spa, spa, 6);
	memcpy(the.snonce, snonce, 32);
	the.rsn_length = rsn_length > sizeof(the.rsn) ? sizeof(the.rsn) : rsn_length;
	memcpy(the.rsn, rsn, the.rsn_length);
}

int
a51wpa_frame(const uint8_t *frame, size_t length, uint8_t *out, size_t *out_length)
{
	return a51wpa_rx(&the, frame, length, out, out_length);
}

/* After A51WPA_M4: the TK (16), the GTK (its length returned), its key id and RSC. */
size_t
a51wpa_keys(uint8_t tk[16], uint8_t gtk[32], int *gtk_id, uint8_t rsc[8])
{
	memcpy(tk, the.ptk + 32, 16);
	memcpy(gtk, the.gtk, the.gtk_length);
	*gtk_id = the.gtk_id;
	memcpy(rsc, the.gtk_rsc, 8);
	return the.gtk_length;
}

const char *
a51wpa_failure(void)
{
	return the.failure != NULL ? the.failure : "";
}

/* Forget the keys. */
void
a51wpa_forget(void)
{
	memset(&the, 0, sizeof(the));
}

