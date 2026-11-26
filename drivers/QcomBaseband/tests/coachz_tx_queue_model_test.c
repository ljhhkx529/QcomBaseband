/* The TX ring as coachz_baseband_poll and bb_queue_tx drive it: the model exists to pin the two properties the
   real code needs and that a host test can check without IOKit, that bb_tx_count never goes past BB_TX and that a
   prepend after a refused send never writes over a frame that is already queued. */
#include <stdint.h>
#include <stdio.h>

#define BB_TX 64u
#define BB_POLL_BATCH 16u

typedef struct { uint32_t seq; } frame;
static frame q[BB_TX];
static uint32_t head, count, dropped, enqueued;

static void q_init(void) { head = count = dropped = enqueued = 0; }

/* bb_queue_tx: the >= check refuses once the ring is full instead of wrapping. */
static void q_enq(uint32_t seq)
{
    if (count >= BB_TX) { dropped++; return; }
    q[(head + count) % BB_TX] = (frame){seq};
    count++;
    enqueued++;
}

static uint32_t q_take(frame *batch, uint32_t limit)
{
    uint32_t n = 0;
    while (count && n < limit) {
        batch[n++] = q[head];
        head = (head + 1u) % BB_TX;
        count--;
    }
    return n;
}

/* The poll's refused path: keep what fits, from the front of the refused tail, and drop the rest. */
static void q_prepend(frame *batch, uint32_t n, uint32_t i)
{
    const uint32_t back = n - i;
    const uint32_t room = BB_TX - count;
    const uint32_t keep = back < room ? back : room;
    for (uint32_t k = 0; k < keep; k++) {
        const uint32_t pos = (head + BB_TX - keep + k) % BB_TX;
        q[pos] = batch[i + k];
    }
    head = (head + BB_TX - keep) % BB_TX;
    count += keep;
    for (uint32_t k = keep; k < back; k++) dropped++;
}

static uint32_t q_at(uint32_t ordinal) { return q[(head + ordinal) % BB_TX].seq; }

static int check(int ok, const char *what) { printf("%s %s\n", ok ? "PASS" : "FAIL", what); return ok; }

int main(void)
{
    int ok = 1;

    /* Filling past the ring refuses at 64 and does not wrap it. */
    q_init();
    for (uint32_t i = 0; i < 100u; i++) q_enq(i);
    ok &= check(count == BB_TX && dropped == 36u, "fill 100 keeps 64 and drops 36");
    ok &= check(q_at(0) == 0u && q_at(63) == 63u, "fill keeps the first 64 in order");

    /* A full queue gives a full batch of 16 and leaves 48. */
    q_init();
    for (uint32_t i = 0; i < BB_TX; i++) q_enq(i);
    frame batch[BB_POLL_BATCH] = { 0 };
    uint32_t n = q_take(batch, BB_POLL_BATCH);
    ok &= check(n == 16u && count == 48u, "take 16 from full leaves 48");
    ok &= check(batch[0].seq == 0u && batch[15].seq == 15u, "take keeps FIFO order");

    /* The host refills the queue while the batch is being sent. */
    for (uint32_t i = 64u; i < 80u; i++) q_enq(i);
    ok &= check(count == BB_TX, "concurrent enqueue refills to 64");

    /* No room left: the whole refused tail is dropped, nothing is overwritten, the count stays at 64. */
    q_prepend(batch, n, 0u);
    ok &= check(count == BB_TX && dropped == 16u, "no room drops the tail and keeps the count");
    ok &= check(q_at(48) == 64u && q_at(63) == 79u, "no room keeps the queue contents");

    /* Room for part of the tail: 8 frames fit, 8 are dropped, order is kept. */
    q_init();
    for (uint32_t i = 0; i < BB_TX; i++) q_enq(i);
    n = q_take(batch, BB_POLL_BATCH);
    for (uint32_t i = 64u; i < 72u; i++) q_enq(i);
    ok &= check(count == 56u, "eight enqueued after the batch leaves 56");
    q_prepend(batch, n, 0u);
    ok &= check(count == BB_TX && dropped == 8u, "partial room keeps eight and drops eight");
    ok &= check(q_at(0) == 0u && q_at(7) == 7u, "partial room puts the kept frames in order");
    ok &= check(q_at(56) == 64u, "partial room leaves the host frames behind the kept ones");

    /* A failed send that the queue has room for keeps the tail intact. */
    q_init();
    for (uint32_t i = 0; i < 32u; i++) q_enq(i);
    n = q_take(batch, BB_POLL_BATCH);
    q_prepend(batch, n, 4u);
    ok &= check(count == 28u && dropped == 0u, "room for the tail keeps it whole");
    ok &= check(q_at(0) == 4u && q_at(11) == 15u, "kept tail starts at the refused frame");

    /* The count never leaves [0, BB_TX]. */
    {
        int bounded = 1;
        q_init();
        frame b2[BB_POLL_BATCH] = { 0 };
        for (uint32_t round = 0; round < 500u; round++) {
            for (uint32_t i = 0; i < 5u; i++) q_enq(round * 5u + i);
            const uint32_t k = q_take(b2, BB_POLL_BATCH);
            if (round & 1u) q_prepend(b2, k, k ? 1u : 0u);
            if (count > BB_TX) bounded = 0;
        }
        ok &= check(bounded, "count stays within the ring over 500 rounds");
    }

    printf(ok ? "all tx queue model tests pass\n" : "tx queue model tests failed\n");
    return ok ? 0 : 1;
}
