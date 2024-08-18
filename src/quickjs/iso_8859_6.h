
#ifndef TUTF8E_ISO_8859_6_H
#define TUTF8E_ISO_8859_6_H

#include <tutf8e.h>

static inline int tutf8e_iso_8859_6_string_length(const char *input, size_t *input_length, const char *invalid, size_t *output_length)
{
  return tutf8e_encoder_string_length(tutf8e_encoder_iso_8859_6, input, input_length, invalid, output_length);
}

static inline int tutf8e_iso_8859_6_string_encode(const char *input, char *output, const char *invalid, size_t *output_length)
{
  return tutf8e_encoder_string_encode(tutf8e_encoder_iso_8859_6, input, invalid, output, output_length);
}

static inline int tutf8e_iso_8859_6_buffer_length(const char *i, size_t input_length, const char *invalid, size_t *length)
{
  return tutf8e_encoder_buffer_length(tutf8e_encoder_iso_8859_6, input, input_length, invalid, length);
}

static inline int tutf8e_iso_8859_6_buffer_encode(const char *i, size_t input_length, const char *invalid, char *output, size_t *output_length)
{
  return tutf8e_encoder_buffer_encode(tutf8e_encoder_iso_8859_6, input, input_length, invalid, output, output_length);
}

#endif
