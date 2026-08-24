// SPIKE: does the ARMv8 SHA-2 extension work here, and does it agree with NIST?
#include <arm_neon.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/auxv.h>
#include <asm/hwcap.h>

static const uint32_t K[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

__attribute__((target("+crypto")))
static void compress_hw(uint32_t state[8], const uint8_t* data, size_t nblocks) {
  uint32x4_t STATE0 = vld1q_u32(&state[0]);
  uint32x4_t STATE1 = vld1q_u32(&state[4]);
  uint32x4_t ABEF_SAVE, CDGH_SAVE, MSG0, MSG1, MSG2, MSG3, TMP0, TMP1, TMP2;

  while (nblocks--) {
    ABEF_SAVE = STATE0;
    CDGH_SAVE = STATE1;
    // SHA-256 loads message words BIG-endian; vrev32q_u8 does the swap in one op.
    MSG0 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data +  0)));
    MSG1 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 16)));
    MSG2 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 32)));
    MSG3 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 48)));
    TMP0 = vaddq_u32(MSG0, vld1q_u32(&K[0]));

    /* rounds 0-3 */
    MSG0 = vsha256su0q_u32(MSG0, MSG1);
    TMP2 = STATE0;
    TMP1 = vaddq_u32(MSG1, vld1q_u32(&K[4]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
    MSG0 = vsha256su1q_u32(MSG0, MSG2, MSG3);
    /* rounds 4-7 */
    MSG1 = vsha256su0q_u32(MSG1, MSG2);
    TMP2 = STATE0;
    TMP0 = vaddq_u32(MSG2, vld1q_u32(&K[8]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
    MSG1 = vsha256su1q_u32(MSG1, MSG3, MSG0);
    /* rounds 8-11 */
    MSG2 = vsha256su0q_u32(MSG2, MSG3);
    TMP2 = STATE0;
    TMP1 = vaddq_u32(MSG3, vld1q_u32(&K[12]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
    MSG2 = vsha256su1q_u32(MSG2, MSG0, MSG1);
    /* rounds 12-15 */
    MSG3 = vsha256su0q_u32(MSG3, MSG0);
    TMP2 = STATE0;
    TMP0 = vaddq_u32(MSG0, vld1q_u32(&K[16]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
    MSG3 = vsha256su1q_u32(MSG3, MSG1, MSG2);
    /* rounds 16-19 */
    MSG0 = vsha256su0q_u32(MSG0, MSG1);
    TMP2 = STATE0;
    TMP1 = vaddq_u32(MSG1, vld1q_u32(&K[20]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
    MSG0 = vsha256su1q_u32(MSG0, MSG2, MSG3);
    /* rounds 20-23 */
    MSG1 = vsha256su0q_u32(MSG1, MSG2);
    TMP2 = STATE0;
    TMP0 = vaddq_u32(MSG2, vld1q_u32(&K[24]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
    MSG1 = vsha256su1q_u32(MSG1, MSG3, MSG0);
    /* rounds 24-27 */
    MSG2 = vsha256su0q_u32(MSG2, MSG3);
    TMP2 = STATE0;
    TMP1 = vaddq_u32(MSG3, vld1q_u32(&K[28]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
    MSG2 = vsha256su1q_u32(MSG2, MSG0, MSG1);
    /* rounds 28-31 */
    MSG3 = vsha256su0q_u32(MSG3, MSG0);
    TMP2 = STATE0;
    TMP0 = vaddq_u32(MSG0, vld1q_u32(&K[32]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
    MSG3 = vsha256su1q_u32(MSG3, MSG1, MSG2);
    /* rounds 32-35 */
    MSG0 = vsha256su0q_u32(MSG0, MSG1);
    TMP2 = STATE0;
    TMP1 = vaddq_u32(MSG1, vld1q_u32(&K[36]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
    MSG0 = vsha256su1q_u32(MSG0, MSG2, MSG3);
    /* rounds 36-39 */
    MSG1 = vsha256su0q_u32(MSG1, MSG2);
    TMP2 = STATE0;
    TMP0 = vaddq_u32(MSG2, vld1q_u32(&K[40]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
    MSG1 = vsha256su1q_u32(MSG1, MSG3, MSG0);
    /* rounds 40-43 */
    MSG2 = vsha256su0q_u32(MSG2, MSG3);
    TMP2 = STATE0;
    TMP1 = vaddq_u32(MSG3, vld1q_u32(&K[44]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
    MSG2 = vsha256su1q_u32(MSG2, MSG0, MSG1);
    /* rounds 44-47 */
    MSG3 = vsha256su0q_u32(MSG3, MSG0);
    TMP2 = STATE0;
    TMP0 = vaddq_u32(MSG0, vld1q_u32(&K[48]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
    MSG3 = vsha256su1q_u32(MSG3, MSG1, MSG2);
    /* rounds 48-51 */
    TMP2 = STATE0;
    TMP1 = vaddq_u32(MSG1, vld1q_u32(&K[52]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
    /* rounds 52-55 */
    TMP2 = STATE0;
    TMP0 = vaddq_u32(MSG2, vld1q_u32(&K[56]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
    /* rounds 56-59 */
    TMP2 = STATE0;
    TMP1 = vaddq_u32(MSG3, vld1q_u32(&K[60]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
    /* rounds 60-63 */
    TMP2 = STATE0;
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);

    STATE0 = vaddq_u32(STATE0, ABEF_SAVE);
    STATE1 = vaddq_u32(STATE1, CDGH_SAVE);
    data += 64;
  }
  vst1q_u32(&state[0], STATE0);
  vst1q_u32(&state[4], STATE1);
}

int main() {
  if (!(getauxval(AT_HWCAP) & HWCAP_SHA2)) { std::printf("NO HWCAP_SHA2\n"); return 2; }
  // "abc" padded into one 64-byte block by hand.
  uint8_t block[64] = {};
  std::memcpy(block, "abc", 3);
  block[3] = 0x80;
  block[62] = 0x00; block[63] = 24;   // length = 24 bits, big-endian
  uint32_t st[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                    0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  compress_hw(st, block, 1);
  char hex[65];
  for (int i = 0; i < 8; i++) std::sprintf(hex + i*8, "%08x", st[i]);
  const char* want = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  std::printf("got  %s\nwant %s\n%s\n", hex, want,
              std::strcmp(hex, want) == 0 ? "MATCH -- hardware SHA-256 works" : "MISMATCH");
  return std::strcmp(hex, want) == 0 ? 0 : 1;
}
