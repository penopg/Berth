// Кодирование кодпоинта в UTF-8.
#ifndef BERTH_UTF8_H
#define BERTH_UTF8_H

#include <stdint.h>

// Кодирует один кодпоинт в буфер. Возвращает число байт (1–4).
// Всё, что больше U+10FFFF, заменяется на U+FFFD.
int utf8_encode(uint32_t cp, char out[4]);

#endif // BERTH_UTF8_H
