#ifndef HAM_ICONS_H
#define HAM_ICONS_H

#include <stdint.h>

// Microphone  8 x 12  (mono, SSD1306 vertical pages, LSB=top)
static const int Microphone_width = 8;
static const int Microphone_height = 12;
static const uint8_t Microphone[] = {0xc0, 0x80, 0xbf, 0xbf, 0xbf, 0xbf,
                                     0x80, 0xc0, 0x01, 0x01, 0x01, 0x0f,
                                     0x0f, 0x01, 0x01, 0x01};

#endif