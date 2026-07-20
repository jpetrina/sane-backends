#include "../../../backend/epsonds.h"
#include "../../../backend/epsonds-jpeg.h"

// fuzz_target.cc
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  epsonds_scanner s;
  ring_buffer ringBuffer;
  unsigned char *tmp = (unsigned char *)malloc(size);
  memcpy(tmp, data, size);
  eds_decode_jpeg(&s, tmp, size, &ringBuffer, 1, 1);
  free(tmp);
  return 0;  // Values other than 0 and -1 are reserved for future use.
}

