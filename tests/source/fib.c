#include <stdint.h>

typedef struct {
  int32_t n;
  int32_t res;
} test_data_t;

void run(test_data_t* data) {
  int32_t a = 0;
  int32_t b = 1;
  for (int32_t i = 0; i < data->n; i++) {
    int32_t next = a + b;
    a = b;
    b = next;
  }
  data->res = a;
}
