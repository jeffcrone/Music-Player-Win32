/*
 * resource.h - IDs shared between app.rc and the C code.
 */
#ifndef MP_RESOURCE_H
#define MP_RESOURCE_H

#define IDI_APP             1
#define IDR_MAINMENU        100
#define IDR_ACCEL           101

/* Menu commands (also used by the matching buttons). */
#define IDM_OPEN_FILES      1001
#define IDM_ADD_FILES       1002
#define IDM_OPEN_PLAYLIST   1003
#define IDM_SAVE_PLAYLIST   1004
#define IDM_REMOVE_SELECTED 1005
#define IDM_CLEAR_PLAYLIST  1006
#define IDM_EXIT            1007
#define IDM_MOVE_UP         1008
#define IDM_MOVE_DOWN       1009
#define IDM_PLAY_PAUSE      1101
#define IDM_STOP            1102
#define IDM_PREVIOUS        1103
#define IDM_NEXT            1104
#define IDM_REPEAT          1105
#define IDM_VOLUME_UP       1106
#define IDM_VOLUME_DOWN     1107
#define IDM_MUTE            1108
/* The nine speeds, in order (main.c's `speeds` table matches). */
#define IDM_SPEED_25        1120
#define IDM_SPEED_50        1121
#define IDM_SPEED_75        1122
#define IDM_SPEED_100       1123
#define IDM_SPEED_125       1124
#define IDM_SPEED_150       1125
#define IDM_SPEED_175       1126
#define IDM_SPEED_200       1127
#define IDM_SPEED_300       1128
#define IDM_ABOUT           1201

/* Child control IDs. */
#define IDC_TITLE           2001
#define IDC_ARTIST          2002
#define IDC_SEEK            2003
#define IDC_TIME            2004
#define IDC_LIST            2005
#define IDC_STATUS          2006
#define IDC_BTN_PREV        2101
#define IDC_BTN_PLAY        2102
#define IDC_BTN_STOP        2103
#define IDC_BTN_NEXT        2104
#define IDC_BTN_ADD         2105
#define IDC_BTN_PLAYLIST    2106
#define IDC_VOLUME_LABEL    2007
#define IDC_VOLUME          2008
#define IDC_SPEED_LABEL     2009
#define IDC_SPEED           2010

#endif
