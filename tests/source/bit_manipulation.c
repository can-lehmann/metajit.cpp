#include <stdint.h>

typedef struct {
  uint8_t in8;
  uint16_t in16;
  uint32_t in32;
  uint64_t in64;
  uint8_t out8;
  uint16_t out16;
  uint32_t out32;
  uint64_t out64;
  uint8_t pop8;
  uint16_t pop16;
  uint32_t pop32;
  uint64_t pop64;
} test_data_t;

void run(test_data_t* data) {
  data->out8 = __builtin_bitreverse8(data->in8);
  data->out16 = __builtin_bitreverse16(data->in16);
  data->out32 = __builtin_bitreverse32(data->in32);
  data->out64 = __builtin_bitreverse64(data->in64);
  data->pop8 = __builtin_popcountg(data->in8);
  data->pop16 = __builtin_popcountg(data->in16);
  data->pop32 = __builtin_popcountg(data->in32);
  data->pop64 = __builtin_popcountg(data->in64);
}
