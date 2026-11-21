// Mapping from M5Stack M5Unit-KEYBOARD, MIT, copyright 2025 M5Stack Technology CO LTD.
// Preserved layer mapping from tab5-web-display. See THIRD_PARTY_LICENSES.
#pragma once
#include <stdint.h>
typedef keyboard_key_t KbdKey;
static const KbdKey KBD_BASE[70] = {
  // Esc 1 2 3 4 5 6 7 8 9 0 - + Del
  {0x29, 0}, {0x1E, 0}, {0x1F, 0}, {0x20, 0}, {0x21, 0}, {0x22, 0}, {0x23, 0}, {0x24, 0}, {0x25, 0}, {0x26, 0},
  {0x27, 0}, {0x2D, 0}, {0x2E, 2}, {0x4C, 0},
  // ` ! @ # $ % ^ & * ( ) [ ] backslash
  {0x35, 0}, {0x1E, 2}, {0x1F, 2}, {0x20, 2}, {0x21, 2}, {0x22, 2}, {0x23, 2}, {0x24, 2}, {0x25, 2}, {0x26, 2},
  {0x27, 2}, {0x2F, 0}, {0x30, 0}, {0x31, 0},
  // Tab q w e r t y u i o p ; ' Backspace
  {0x2B, 0}, {0x14, 0}, {0x1A, 0}, {0x08, 0}, {0x15, 0}, {0x17, 0}, {0x1C, 0}, {0x18, 0}, {0x0C, 0}, {0x12, 0},
  {0x13, 0}, {0x33, 0}, {0x34, 0}, {0x2A, 0},
  // Sym Aa a s d f g h j k l Up _ Enter
  {0, 0}, {0, 0}, {0x04, 0}, {0x16, 0}, {0x07, 0}, {0x09, 0}, {0x0A, 0}, {0x0B, 0}, {0x0D, 0}, {0x0E, 0},
  {0x0F, 0}, {0x52, 0}, {0x2D, 2}, {0x28, 0},
  // Ctrl Alt z x c v b n m . Left Down Right Space
  {0, 0}, {0, 0}, {0x1D, 0}, {0x1B, 0}, {0x06, 0}, {0x19, 0}, {0x05, 0}, {0x11, 0}, {0x10, 0}, {0x37, 0},
  {0x50, 0}, {0x51, 0}, {0x4F, 0}, {0x2C, 0},
};
// The Sym layer differs only here: ` ~  ! ?  * /  ( <  ) >  [ {  ] }  \ |  ; :  ' "  _ =  . ,
// and Left Home, Right End, Up Page Up, Down Page Down, Backspace Delete (keys the keyboard lacks)
typedef struct { uint8_t idx; KbdKey key; } KbdSym;
static const KbdSym KBD_SYM[] = {
  {14, {0x35, 2}}, {15, {0x38, 2}}, {22, {0x38, 0}}, {23, {0x36, 2}}, {24, {0x37, 2}}, {25, {0x2F, 2}},
  {26, {0x30, 2}}, {27, {0x31, 2}}, {39, {0x33, 2}}, {40, {0x34, 2}}, {54, {0x2E, 0}}, {65, {0x36, 0}},
  {66, {0x4A, 0}}, {68, {0x4D, 0}}, {53, {0x4B, 0}}, {67, {0x4E, 0}}, {41, {0x4C, 0}},
};
static const uint8_t KIDX_SYM = 42, KIDX_AA = 43, KIDX_CTRL = 56, KIDX_ALT = 57;

