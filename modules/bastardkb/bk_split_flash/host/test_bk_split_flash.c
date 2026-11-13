// Copyright 2026 bastardkb
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Native host harness for the pure, hardware-independent safety-core logic in
// bk_split_flash.h (CRC-32, image length and plausibility checks). Compiled and
// run on the host (`make test`), no RP2040 or board required.
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifndef SPLIT_FLASH_MAX_IMAGE_SIZE
#    define SPLIT_FLASH_MAX_IMAGE_SIZE (256u * 1024)
#endif

#include "../bk_split_flash.h"

static int failures = 0;

#define CHECK(cond)                                                                                    \
    do {                                                                                               \
        if (!(cond)) {                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                    \
            failures++;                                                                                \
        }                                                                                              \
    } while (0)

static void write32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void test_crc32(void) {
    const uint8_t *known = (const uint8_t *)"123456789";
    CHECK(bk_split_flash_crc32(known, 9) == 0xCBF43926u);
    CHECK(bk_split_flash_crc32(NULL, 0) == 0x00000000u);
    uint8_t buf[64];
    memset(buf, 0xFF, sizeof(buf));
    CHECK(bk_split_flash_crc32(buf, sizeof(buf)) == 0x0F6187BAu);
}

static void test_len_in_range(void) {
    CHECK(bk_split_flash_len_in_range(BK_SPLIT_FLASH_MIN_IMAGE_SIZE));
    CHECK(bk_split_flash_len_in_range(SPLIT_FLASH_MAX_IMAGE_SIZE));
    CHECK(bk_split_flash_len_in_range((BK_SPLIT_FLASH_MIN_IMAGE_SIZE + SPLIT_FLASH_MAX_IMAGE_SIZE) / 2));
    CHECK(!bk_split_flash_len_in_range(0));
    CHECK(!bk_split_flash_len_in_range(BK_SPLIT_FLASH_MIN_IMAGE_SIZE - 1));
    CHECK(!bk_split_flash_len_in_range(SPLIT_FLASH_MAX_IMAGE_SIZE + 1));
    CHECK(!bk_split_flash_len_in_range(0xFFFFFFFFu));
}

// Real boot2 blob, verbatim from platforms/chibios/vendors/RP/stage2_bootloaders.c
// (default W25Q080). Hard positive test for the bootrom checksum algorithm.
static const uint8_t boot2_w25q080[256] = {
    0x00, 0xb5, 0x32, 0x4b, 0x21, 0x20, 0x58, 0x60, 0x98, 0x68, 0x02, 0x21,
    0x88, 0x43, 0x98, 0x60, 0xd8, 0x60, 0x18, 0x61, 0x58, 0x61, 0x2e, 0x4b,
    0x00, 0x21, 0x99, 0x60, 0x04, 0x21, 0x59, 0x61, 0x01, 0x21, 0xf0, 0x22,
    0x99, 0x50, 0x2b, 0x49, 0x19, 0x60, 0x01, 0x21, 0x99, 0x60, 0x35, 0x20,
    0x00, 0xf0, 0x44, 0xf8, 0x02, 0x22, 0x90, 0x42, 0x14, 0xd0, 0x06, 0x21,
    0x19, 0x66, 0x00, 0xf0, 0x34, 0xf8, 0x19, 0x6e, 0x01, 0x21, 0x19, 0x66,
    0x00, 0x20, 0x18, 0x66, 0x1a, 0x66, 0x00, 0xf0, 0x2c, 0xf8, 0x19, 0x6e,
    0x19, 0x6e, 0x19, 0x6e, 0x05, 0x20, 0x00, 0xf0, 0x2f, 0xf8, 0x01, 0x21,
    0x08, 0x42, 0xf9, 0xd1, 0x00, 0x21, 0x99, 0x60, 0x1b, 0x49, 0x19, 0x60,
    0x00, 0x21, 0x59, 0x60, 0x1a, 0x49, 0x1b, 0x48, 0x01, 0x60, 0x01, 0x21,
    0x99, 0x60, 0xeb, 0x21, 0x19, 0x66, 0xa0, 0x21, 0x19, 0x66, 0x00, 0xf0,
    0x12, 0xf8, 0x00, 0x21, 0x99, 0x60, 0x16, 0x49, 0x14, 0x48, 0x01, 0x60,
    0x01, 0x21, 0x99, 0x60, 0x01, 0xbc, 0x00, 0x28, 0x00, 0xd0, 0x00, 0x47,
    0x12, 0x48, 0x13, 0x49, 0x08, 0x60, 0x03, 0xc8, 0x80, 0xf3, 0x08, 0x88,
    0x08, 0x47, 0x03, 0xb5, 0x99, 0x6a, 0x04, 0x20, 0x01, 0x42, 0xfb, 0xd0,
    0x01, 0x20, 0x01, 0x42, 0xf8, 0xd1, 0x03, 0xbd, 0x02, 0xb5, 0x18, 0x66,
    0x18, 0x66, 0xff, 0xf7, 0xf2, 0xff, 0x18, 0x6e, 0x18, 0x6e, 0x02, 0xbd,
    0x00, 0x00, 0x02, 0x40, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x07, 0x00,
    0x00, 0x03, 0x5f, 0x00, 0x21, 0x22, 0x00, 0x00, 0xf4, 0x00, 0x00, 0x18,
    0x22, 0x20, 0x00, 0xa0, 0x00, 0x01, 0x00, 0x10, 0x08, 0xed, 0x00, 0xe0,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x07, 0x0b, 0x8f, 0xd5};

// Compute the boot2 checksum the bootrom expects (see bk_split_flash.h), then
// patch it into boot2[252:256]. Mirrors the header's validity function.
static uint32_t compute_boot2_checksum(const uint8_t *b) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < 252; ++i) {
        crc ^= bk_split_flash_btrev8(b[i]);
        for (uint32_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
        }
    }
    return bk_split_flash_btrev32(crc);
}

static void test_boot2_checksum(void) {
    // The real boot2 blob must validate, and its stored word must match the
    // independently computed value.
    CHECK(bk_split_flash_boot2_checksum_valid(boot2_w25q080));
    uint8_t bad[256];
    memcpy(bad, boot2_w25q080, sizeof(bad));
    bad[10] ^= 0xFF;
    CHECK(!bk_split_flash_boot2_checksum_valid(bad));
    // A fresh fake boot2 (all 0xFF) can be patched to a valid checksum.
    uint8_t fake[256];
    memset(fake, 0xFF, sizeof(fake));
    write32_le(fake + 252, compute_boot2_checksum(fake));
    CHECK(bk_split_flash_boot2_checksum_valid(fake));
    // Stored checksum of the real blob matches the known constant.
    CHECK(bk_split_flash_read32_le(boot2_w25q080 + 252) == 0xd58f0b07u);
}

// Build a fake-but-plausible RP2040 image: 0xFF fill, a believable vector table
// at offset 0x100, and a boot2 block with a valid bootrom checksum.
static void make_valid_image(uint8_t *img, uint32_t len) {
    memset(img, 0xFF, len);
    write32_le(img + 0x100, 0x20040400);  // initial SP into SRAM
    write32_le(img + 0x104, 0x100002c5);  // reset vector, Thumb bit set
    write32_le(img + 252, compute_boot2_checksum(img));
}

static void test_image_plausible(void) {
    uint8_t img[BK_SPLIT_FLASH_MIN_IMAGE_SIZE + 64];
    uint8_t bad[BK_SPLIT_FLASH_MIN_IMAGE_SIZE + 64];
    uint32_t len = BK_SPLIT_FLASH_MIN_IMAGE_SIZE;

    make_valid_image(img, len);
    CHECK(bk_split_flash_image_plausible(img, len));

    // Corrupted boot2 checksum.
    memcpy(bad, img, len);
    bad[10] ^= 0xFF;
    CHECK(!bk_split_flash_image_plausible(bad, len));

    // Initial SP outside SRAM.
    memcpy(bad, img, len);
    write32_le(bad + 0x100, 0x20000000u - 1u);
    CHECK(!bk_split_flash_image_plausible(bad, len));
    memcpy(bad, img, len);
    write32_le(bad + 0x100, 0x20042000u);
    CHECK(!bk_split_flash_image_plausible(bad, len));

    // Reset vector missing the Thumb bit.
    memcpy(bad, img, len);
    write32_le(bad + 0x104, 0x100002c4u);
    CHECK(!bk_split_flash_image_plausible(bad, len));

    // Reset vector outside the bounded XIP image window.
    memcpy(bad, img, len);
    write32_le(bad + 0x104, 0x110002c5u);
    CHECK(!bk_split_flash_image_plausible(bad, len));

    // Length too small / too large.
    CHECK(!bk_split_flash_image_plausible(img, BK_SPLIT_FLASH_MIN_IMAGE_SIZE - 1));
    CHECK(!bk_split_flash_image_plausible(img, SPLIT_FLASH_MAX_IMAGE_SIZE + 1));
}

// The probe/reply must fit the configured RPC buffer sizes (config.h).
static void test_packet_sizes(void) {
    CHECK(sizeof(bk_split_flash_probe_t) <= 246);
    CHECK(sizeof(bk_split_flash_echo_t) <= 16);
    CHECK(sizeof(bk_split_flash_probe_t) >= 3);
}

// Reply-usable filter (current protocol): magic + seq echoed and a plausible
// image length. A reply that does not match (mid-boot secondary, or a peer on
// different firmware) is not read further and is retried.
static void test_reply_usable(void) {
    CHECK(bk_split_flash_reply_usable(true, true, BK_SPLIT_FLASH_MIN_IMAGE_SIZE));
    // Length outside the plausible image range.
    CHECK(!bk_split_flash_reply_usable(true, true, 0));
    CHECK(!bk_split_flash_reply_usable(true, true, BK_SPLIT_FLASH_MIN_IMAGE_SIZE - 1));
    CHECK(!bk_split_flash_reply_usable(true, true, SPLIT_FLASH_MAX_IMAGE_SIZE + 1));
    CHECK(!bk_split_flash_reply_usable(true, true, 0xFFFFFFFFu));
    // Round-trip fields must match.
    CHECK(!bk_split_flash_reply_usable(false, true, BK_SPLIT_FLASH_MIN_IMAGE_SIZE));
    CHECK(!bk_split_flash_reply_usable(true, false, BK_SPLIT_FLASH_MIN_IMAGE_SIZE));
}

// Plan section 5 skip policy decision matrix.
static void test_handshake_decide(void) {
    const uint32_t my_len = 93280;
    const uint32_t my_crc = 0x48dd14d1u;
    // Same version + identical fingerprint -> skip.
    CHECK(bk_split_flash_handshake_decide(2, my_len, my_crc, my_len, my_crc) == BK_SPLIT_FLASH_DECISION_SKIP_IDENTICAL);
    // Same version + longer peer image -> transfer.
    CHECK(bk_split_flash_handshake_decide(2, my_len + 1, my_crc, my_len, my_crc) == BK_SPLIT_FLASH_DECISION_BEGIN_TRANSFER);
    // Same version + same length + different CRC -> transfer.
    CHECK(bk_split_flash_handshake_decide(2, my_len, my_crc + 1, my_len, my_crc) == BK_SPLIT_FLASH_DECISION_BEGIN_TRANSFER);
    // Version mismatch wins over an identical fingerprint.
    CHECK(bk_split_flash_handshake_decide(1, my_len, my_crc, my_len, my_crc) == BK_SPLIT_FLASH_DECISION_SKIP_VERSION);
    CHECK(bk_split_flash_handshake_decide(3, my_len, my_crc, my_len, my_crc) == BK_SPLIT_FLASH_DECISION_SKIP_VERSION);
}

// End-to-end fingerprint behaviour over real image buffers: a copied image is
// identical (skip), any byte difference in a same-length image changes the CRC
// (transfer), and a different length with identical content is still different.
static void test_handshake_fingerprint(void) {
    uint8_t own[BK_SPLIT_FLASH_MIN_IMAGE_SIZE + 64];
    uint8_t peer[BK_SPLIT_FLASH_MIN_IMAGE_SIZE + 64];
    uint32_t len = BK_SPLIT_FLASH_MIN_IMAGE_SIZE + 32; // distinct from the other tests

    make_valid_image(own, len);
    memcpy(peer, own, len); // identical secondary
    CHECK(bk_split_flash_handshake_decide(2, len, bk_split_flash_crc32(peer, len), len, bk_split_flash_crc32(own, len)) == BK_SPLIT_FLASH_DECISION_SKIP_IDENTICAL);

    peer[12] ^= 0x01; // one different byte -> different CRC
    CHECK(bk_split_flash_handshake_decide(2, len, bk_split_flash_crc32(peer, len), len, bk_split_flash_crc32(own, len)) == BK_SPLIT_FLASH_DECISION_BEGIN_TRANSFER);

    // Same content but a shorter peer image -> different fingerprint.
    uint32_t shorter = BK_SPLIT_FLASH_MIN_IMAGE_SIZE;
    CHECK(bk_split_flash_handshake_decide(2, shorter, bk_split_flash_crc32(peer, shorter), len, bk_split_flash_crc32(own, len)) == BK_SPLIT_FLASH_DECISION_BEGIN_TRANSFER);
}

int main(void) {
    test_crc32();
    test_len_in_range();
    test_boot2_checksum();
    test_image_plausible();
    test_packet_sizes();
    test_reply_usable();
    test_handshake_decide();
    test_handshake_fingerprint();
    if (failures) {
        printf("%d test(s) FAILED\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}