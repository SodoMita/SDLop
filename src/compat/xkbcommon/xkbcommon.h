/*
  SDLop - vendored minimal <xkbcommon/xkbcommon.h> ABI subset.

  Used ONLY when the system xkbcommon development headers are not
  installed (CMake picks this directory via src/compat then). SDLop
  dlopen-s libxkbcommon at run time (see src/internal/sdlop_xkb_dyn.h),
  so only the ABI-stable type/enum/keysym *values* are ever needed at
  build time; every function is reached through a dlsym'ed pointer.

  The numeric values below are frozen by the X Keyboard Extension and
  keysymdef.h - they cannot change without breaking every existing
  libxkbcommon client.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef SDLop_COMPAT_XKBCOMMON_H_
#define SDLop_COMPAT_XKBCOMMON_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* opaque objects */
struct xkb_context;
struct xkb_keymap;
struct xkb_state;

typedef uint32_t xkb_keycode_t;
typedef uint32_t xkb_keysym_t;
typedef uint32_t xkb_mod_index_t;
typedef uint32_t xkb_mod_mask_t;
typedef uint32_t xkb_layout_index_t;

struct xkb_rule_names {
    const char *rules;
    const char *model;
    const char *layout;
    const char *variant;
    const char *options;
};

enum xkb_context_flags {
    XKB_CONTEXT_NO_FLAGS = 0
};

enum xkb_keymap_compile_flags {
    XKB_KEYMAP_COMPILE_NO_FLAGS = 0
};

enum xkb_keymap_format {
    XKB_KEYMAP_FORMAT_TEXT_V1 = 1
};

enum xkb_key_direction {
    XKB_KEY_UP = 0,
    XKB_KEY_DOWN = 1
};

enum xkb_state_component {
    XKB_STATE_MODS_DEPRESSED = (1 << 0),
    XKB_STATE_MODS_LATCHED = (1 << 1),
    XKB_STATE_MODS_LOCKED = (1 << 2),
    XKB_STATE_MODS_EFFECTIVE = (1 << 3),
    XKB_STATE_LAYOUT_DEPRESSED = (1 << 4),
    XKB_STATE_LAYOUT_LATCHED = (1 << 5),
    XKB_STATE_LAYOUT_LOCKED = (1 << 6),
    XKB_STATE_LAYOUT_EFFECTIVE = (1 << 7),
    XKB_STATE_LEDS = (1 << 8)
};

/* modifier names (keymap text - stable ABI) */
#define XKB_MOD_NAME_SHIFT "Shift"
#define XKB_MOD_NAME_CAPS  "Caps"
#define XKB_MOD_NAME_CTRL  "Control"
#define XKB_MOD_NAME_ALT   "Mod1"
#define XKB_MOD_NAME_NUM   "Mod2"
#define XKB_MOD_NAME_LOGO  "Mod4"

/* keysym values actually referenced by SDLop (frozen by keysymdef.h) */
#define XKB_KEY_NoSymbol 0x000000

#define XKB_KEY_BackSpace 0xff08
#define XKB_KEY_Tab       0xff09
#define XKB_KEY_Return    0xff0d
#define XKB_KEY_Pause     0xff13
#define XKB_KEY_Scroll_Lock 0xff14
#define XKB_KEY_Escape    0xff1b
#define XKB_KEY_Delete    0xffff

#define XKB_KEY_ISO_Left_Tab 0xfe20

#define XKB_KEY_Home      0xff50
#define XKB_KEY_Left      0xff51
#define XKB_KEY_Up        0xff52
#define XKB_KEY_Right     0xff53
#define XKB_KEY_Down      0xff54
#define XKB_KEY_Page_Up   0xff55
#define XKB_KEY_Page_Down 0xff56
#define XKB_KEY_End       0xff57
#define XKB_KEY_Print     0xff61
#define XKB_KEY_Insert    0xff63
#define XKB_KEY_Menu      0xff67
#define XKB_KEY_Num_Lock  0xff7f

#define XKB_KEY_KP_Space     0xff80
#define XKB_KEY_KP_Tab       0xff89
#define XKB_KEY_KP_Enter     0xff8d
#define XKB_KEY_KP_Multiply  0xffaa
#define XKB_KEY_KP_Add       0xffab
#define XKB_KEY_KP_Separator 0xffac
#define XKB_KEY_KP_Subtract  0xffad
#define XKB_KEY_KP_Decimal   0xffae
#define XKB_KEY_KP_Divide    0xffaf
#define XKB_KEY_KP_0         0xffb0
#define XKB_KEY_KP_1         0xffb1
#define XKB_KEY_KP_2         0xffb2
#define XKB_KEY_KP_3         0xffb3
#define XKB_KEY_KP_4         0xffb4
#define XKB_KEY_KP_5         0xffb5
#define XKB_KEY_KP_6         0xffb6
#define XKB_KEY_KP_7         0xffb7
#define XKB_KEY_KP_8         0xffb8
#define XKB_KEY_KP_9         0xffb9
#define XKB_KEY_KP_Equal     0xffbd

#define XKB_KEY_F1  0xffbe
#define XKB_KEY_F24 0xffd5

#define XKB_KEY_Shift_L    0xffe1
#define XKB_KEY_Shift_R    0xffe2
#define XKB_KEY_Control_L  0xffe3
#define XKB_KEY_Control_R  0xffe4
#define XKB_KEY_Caps_Lock  0xffe5
#define XKB_KEY_Alt_L      0xffe9
#define XKB_KEY_Alt_R      0xffea
#define XKB_KEY_Super_L    0xffeb
#define XKB_KEY_Super_R    0xffec

/* Latin-1 letters (0x0041-0x007a; the driver range-checks with A/Z/a/z) */
#define XKB_KEY_A 0x0041
#define XKB_KEY_B 0x0042
#define XKB_KEY_C 0x0043
#define XKB_KEY_D 0x0044
#define XKB_KEY_E 0x0045
#define XKB_KEY_F 0x0046
#define XKB_KEY_G 0x0047
#define XKB_KEY_H 0x0048
#define XKB_KEY_I 0x0049
#define XKB_KEY_J 0x004a
#define XKB_KEY_K 0x004b
#define XKB_KEY_L 0x004c
#define XKB_KEY_M 0x004d
#define XKB_KEY_N 0x004e
#define XKB_KEY_O 0x004f
#define XKB_KEY_P 0x0050
#define XKB_KEY_Q 0x0051
#define XKB_KEY_R 0x0052
#define XKB_KEY_S 0x0053
#define XKB_KEY_T 0x0054
#define XKB_KEY_U 0x0055
#define XKB_KEY_V 0x0056
#define XKB_KEY_W 0x0057
#define XKB_KEY_X 0x0058
#define XKB_KEY_Y 0x0059
#define XKB_KEY_Z 0x005a
#define XKB_KEY_a 0x0061
#define XKB_KEY_b 0x0062
#define XKB_KEY_c 0x0063
#define XKB_KEY_d 0x0064
#define XKB_KEY_e 0x0065
#define XKB_KEY_f 0x0066
#define XKB_KEY_g 0x0067
#define XKB_KEY_h 0x0068
#define XKB_KEY_i 0x0069
#define XKB_KEY_j 0x006a
#define XKB_KEY_k 0x006b
#define XKB_KEY_l 0x006c
#define XKB_KEY_m 0x006d
#define XKB_KEY_n 0x006e
#define XKB_KEY_o 0x006f
#define XKB_KEY_p 0x0070
#define XKB_KEY_q 0x0071
#define XKB_KEY_r 0x0072
#define XKB_KEY_s 0x0073
#define XKB_KEY_t 0x0074
#define XKB_KEY_u 0x0075
#define XKB_KEY_v 0x0076
#define XKB_KEY_w 0x0077
#define XKB_KEY_x 0x0078
#define XKB_KEY_y 0x0079
#define XKB_KEY_z 0x007a

#ifdef __cplusplus
}
#endif

#endif /* SDLop_COMPAT_XKBCOMMON_H_ */
