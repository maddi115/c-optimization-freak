// fuzz_client.c - random "server messages" into the browser code (client.c) and our unpacker
// (pack.h). In a browser this runs as WebAssembly on data from the network, so it must never
// read or write outside its buffers, whatever arrives.
#include <stdlib.h>
#include <string.h>
#include "../client.c"

static void apply(void *s, int kind, int a, int b, int c) { (void)s; (void)kind; (void)a; (void)b; (void)c; }
static void step(void *s) { ((uint8_t *)s)[0]++; }

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    static FkClient c;
    static uint8_t state[5000], rows[4096], scratch[32 * FK_CHUNK], packed[4096 + 4096 / 8 + 80], tmp[4096 + 4096 / 8 + 80];
    static uint32_t sums[FKC_RING * 5];
    if (!c.scratch) {
        c.state = state; c.state_bytes = (int)sizeof state; c.apply = apply; c.step = step;
        c.sums = sums; c.nchunks = 5; c.rows = rows; c.nrows = 4096; c.row_size = 1;
        c.scratch = scratch; c.scratch_cap = (int)sizeof scratch;
    }
    fkc_reset(&c);
    if (n && d[0] & 1) { c.synced = 1; c.tick = 41; }   // sometimes pretend we are in step already
    // the input is several messages: [u16 length][bytes] ...
    size_t i = 1;
    while (i + 2 <= n) {
        size_t len = d[i] | d[i + 1] << 8;
        i += 2;
        if (len > n - i) len = n - i;
        fkc_receive(&c, d + i, (int)len);
        c.out_len = 0;
        i += len;
    }
    // and the unpacker on its own: random input must fail cleanly or produce exactly cap bytes
    pk_unpack(rows, 4096, d, (int)n);
    // round trip must be exact
    if (n <= 4096) {
        int z = pk_pack(packed, tmp, d, (int)n);
        if (pk_unpack(tmp, (int)n, packed, z) != (int)n || memcmp(tmp, d, n)) __builtin_trap();
    }
    return 0;
}
