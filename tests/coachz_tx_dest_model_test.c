/* The TX poll with per-destination credit: a destination that runs out of credit blocks only itself, the other
   destinations of the same batch keep sending, and each destination keeps its own FIFO order. The model pins the
   rules the driver needs and that a host test can check without IOKit. */
#include <stdint.h>
#include <stdio.h>

#define BB_TX 64u
#define BB_BATCH 16u
#define DEST_MAX 8u

typedef struct { uint32_t seq, node, port; } frame;
static frame q[BB_TX];
static uint32_t head, count, dropped, sent_total, passes;

static void q_init(void) { head = count = dropped = sent_total = passes = 0; }

static void q_enq(uint32_t seq, uint32_t node, uint32_t port)
{
    if (count >= BB_TX) { dropped++; return; }
    q[(head + count) % BB_TX] = (frame){ seq, node, port };
    count++;
}

static uint32_t q_take(frame *batch)
{
    uint32_t n = 0;
    while (count && n < BB_BATCH) { batch[n++] = q[head]; head = (head + 1u) % BB_TX; count--; }
    return n;
}

static void q_prepend(frame *kept, uint32_t kept_n)
{
    const uint32_t room = BB_TX - count;
    const uint32_t keep = kept_n < room ? kept_n : room;
    for (uint32_t k = 0; k < keep; k++) q[(head + BB_TX - keep + k) % BB_TX] = kept[k];
    head = (head + BB_TX - keep) % BB_TX;
    count += keep;
    for (uint32_t k = keep; k < kept_n; k++) dropped++;
}

/* One poll pass: credit[dest] is how many frames the destination still accepts. Returns how many frames were
   attempted, so the caller can check that a destination is tried at most once per pass. */
static uint32_t q_pass(uint32_t *credit, uint32_t dests, uint32_t *attempts)
{
    frame batch[BB_BATCH], kept[BB_BATCH];
    uint32_t blocked_node[DEST_MAX], blocked_port[DEST_MAX], blocked_n = 0, kept_n = 0, tried = 0;
    const uint32_t n = q_take(batch);
    passes++;
    for (uint32_t i = 0; i < n; i++) {
        int skip = 0;
        for (uint32_t k = 0; k < blocked_n; k++)
            if (blocked_node[k] == batch[i].node && blocked_port[k] == batch[i].port) skip = 1;
        if (skip) { kept[kept_n++] = batch[i]; continue; }
        const uint32_t d = (batch[i].node * 8u + batch[i].port) % dests;
        if (credit[d]) { credit[d]--; sent_total++; tried++; }
        else {
            if (blocked_n < DEST_MAX) { blocked_node[blocked_n] = batch[i].node; blocked_port[blocked_n] = batch[i].port; blocked_n++; }
            tried++;
            kept[kept_n++] = batch[i];
        }
    }
    if (kept_n) q_prepend(kept, kept_n);
    *attempts = tried;
    return kept_n;
}

static int check(int ok, const char *what) { printf("%s %s\n", ok ? "PASS" : "FAIL", what); return ok; }

int main(void)
{
    int ok = 1;
    uint32_t credit[DEST_MAX];

    /* Destination A (0:77) has no credit, destination B (0:41) has plenty: B still sends. */
    q_init();
    for (uint32_t i = 0; i < 4u; i++) q_enq(i, 0u, 77u);
    for (uint32_t i = 10u; i < 14u; i++) q_enq(i, 0u, 41u);
    for (uint32_t i = 0; i < DEST_MAX; i++) credit[i] = 100u;
    credit[(0u * 8u + 77u) % DEST_MAX] = 0u;
    uint32_t tries = 0;
    q_pass(credit, DEST_MAX, &tries);
    ok &= check(sent_total == 4u && count == 4u, "B sends while A has no credit");
    ok &= check(tries == 5u, "A is tried once, then skipped");

    /* A's own frames keep their order: they are all still at the front, in sequence. */
    ok &= check(q[(head + 0u) % BB_TX].seq == 0u && q[(head + 3u) % BB_TX].seq == 3u, "A keeps its FIFO order");

    /* A gets credit back: the next pass sends A's frames in order. */
    credit[(0u * 8u + 77u) % DEST_MAX] = 10u;
    q_pass(credit, DEST_MAX, &tries);
    ok &= check(count == 0u && sent_total == 8u, "A drains after its credit returns");

    /* Mixed destinations with repeated refusals never let the count pass the ring. */
    {
        int bounded = 1;
        uint32_t seq = 0;
        q_init();
        for (uint32_t round = 0; round < 400u; round++) {
            for (uint32_t i = 0; i < 6u; i++) { const uint32_t s = seq++; q_enq(s, 0u, (s % 3u) ? 41u : 77u); }
            for (uint32_t i = 0; i < DEST_MAX; i++) credit[i] = (round & 1u) ? 1u : 0u;
            q_pass(credit, DEST_MAX, &tries);
            if (count > BB_TX) bounded = 0;
            if (tries > BB_BATCH) bounded = 0;
        }
        ok &= check(bounded, "count stays within the ring and attempts within the batch");
    }

    /* No busy wait: with no credit anywhere, one pass attempts each destination once and returns. */
    {
        q_init();
        q_enq(1u, 0u, 77u); q_enq(2u, 0u, 41u); q_enq(3u, 0u, 55u);
        for (uint32_t i = 0; i < DEST_MAX; i++) credit[i] = 0u;
        const uint32_t before = passes;
        const uint32_t kept = q_pass(credit, DEST_MAX, &tries);
        ok &= check(kept == 3u && count == 3u && passes == before + 1u, "no credit still ends the pass");
        ok &= check(tries == 3u, "each destination is tried once per pass");
    }

    printf(ok ? "all tx destination tests pass\n" : "tx destination tests failed\n");
    return ok ? 0 : 1;
}
