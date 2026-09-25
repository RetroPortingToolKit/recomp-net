#include "recomp_net/hash_confirm.h"

#include <stdio.h>

static int failures;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL: %s\n", msg);                                         \
            failures++;                                                        \
        } else {                                                               \
            printf("ok:   %s\n", msg);                                         \
        }                                                                      \
    } while (0)

int main(void)
{
    RNetHashConfirm hc;
    rnet_hc_reset(&hc);

    CHECK(!rnet_hc_confirm_through(&hc, 0), "empty not confirmed");
    CHECK(rnet_hc_resolved_through(&hc) == 0u, "resolved starts 0");

    rnet_hc_note_local(&hc, 0, 0x1111u);
    rnet_hc_note_local(&hc, 1, 0x2222u);
    rnet_hc_note_local(&hc, 2, 0x3333u);
    CHECK(!rnet_hc_confirm_through(&hc, 0), "local-only not enough");

    rnet_hc_note_peer(&hc, 0, 0x1111u);
    CHECK(rnet_hc_confirm_through(&hc, 0), "tick 0 matched");
    CHECK(rnet_hc_resolved_through(&hc) == 0u, "resolved at 0");
    CHECK(!rnet_hc_confirm_through(&hc, 1), "tick 1 not yet");

    rnet_hc_note_peer(&hc, 1, 0x2222u);
    CHECK(rnet_hc_confirm_through(&hc, 1), "tick 1 matched");
    CHECK(rnet_hc_resolved_through(&hc) == 1u, "resolved at 1");

    rnet_hc_note_peer(&hc, 2, 0xDEADu);
    CHECK(rnet_hc_resolved_through(&hc) == 1u, "mismatch holds watermark");
    {
        uint32_t mt = 0, mld = 0, mpd = 0;
        CHECK(rnet_hc_peek_mismatch(&hc, &mt, &mld, &mpd), "peek mismatch");
        CHECK(mt == 2u && mld == 0x3333u && mpd == 0xDEADu, "peek mismatch vals");
    }

    rnet_hc_prime_after(&hc, 819u);
    rnet_hc_note_local(&hc, 820, 0xAAAAu);
    rnet_hc_note_peer(&hc, 820, 0xAAAAu);
    CHECK(rnet_hc_resolved_through(&hc) == 820u, "prime then match");

    rnet_hc_reset(&hc);
    rnet_hc_note_local(&hc, 0, 0x1u);
    rnet_hc_note_peer(&hc, 0, 0x1u);
    rnet_hc_note_local(&hc, 1, 0xAAAAu);
    rnet_hc_note_peer(&hc, 1, 0xBBBBu);
    rnet_hc_note_local(&hc, 1u + RNET_HC_RING, 0xCCCCu);
    rnet_hc_note_peer(&hc, 1u + RNET_HC_RING, 0xCCCCu);
    rnet_hc_note_local(&hc, 50u + RNET_HC_RING, 0xDDDDu);
    rnet_hc_note_peer(&hc, 50u + RNET_HC_RING, 0xDDDDu);
    CHECK(rnet_hc_heal_stale_gap(&hc), "heal advances over stale gap");
    CHECK(rnet_hc_resolved_through(&hc) == 50u + RNET_HC_RING, "heal tip");

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
