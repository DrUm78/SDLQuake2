/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
#include "client.h"

/*

key up events are sent even if in console mode

*/


#define		MAXCMDLINE	256
char	key_lines[32][MAXCMDLINE];
int		key_linepos;
int		shift_down=false;
int	anykeydown;

int		edit_line=0;
int		history_line=0;

int		key_waiting;
char *		keybindings[K_LAST];
qboolean	consolekeys[K_LAST];	// if true, can't be rebound while in console
qboolean	menubound[K_LAST];	// if true, can't be rebound while in menu
int		keyshift[K_LAST];	// key to map to if shift held down in console
int		key_repeats[K_LAST];	// if > 1, it is autorepeating
qboolean	keydown[K_LAST];

typedef struct
{
	char	*name;
	int		keynum;
} keyname_t;

keyname_t keynames[] =
{
	{"TAB", K_TAB},
	{"ENTER", K_ENTER},
	{"ESCAPE", K_ESCAPE},
	{"SPACE", K_SPACE},
	{"BACKSPACE", K_BACKSPACE},
	{"UPARROW", K_UPARROW},
	{"DOWNARROW", K_DOWNARROW},
	{"LEFTARROW", K_LEFTARROW},
	{"RIGHTARROW", K_RIGHTARROW},

	{"ALT", K_ALT},
	{"CTRL", K_CTRL},
	{"SHIFT", K_SHIFT},

	{"F1", K_F1},
	{"F2", K_F2},
	{"F3", K_F3},
	{"F4", K_F4},
	{"F5", K_F5},
	{"F6", K_F6},
	{"F7", K_F7},
	{"F8", K_F8},
	{"F9", K_F9},
	{"F10", K_F10},
	{"F11", K_F11},
	{"F12", K_F12},

	{"INS", K_INS},
	{"DEL", K_DEL},
	{"PGDN", K_PGDN},
	{"PGUP", K_PGUP},
	{"HOME", K_HOME},
	{"END", K_END},

	{"MOUSE1", K_MOUSE1},
	{"MOUSE2", K_MOUSE2},
	{"MOUSE3", K_MOUSE3},
	{"MOUSE4", K_MOUSE4},
	{"MOUSE5", K_MOUSE5},

	{"JOY1", K_JOY1},
	{"JOY2", K_JOY2},
	{"JOY3", K_JOY3},
	{"JOY4", K_JOY4},

	{"AUX1", K_AUX1},
	{"AUX2", K_AUX2},
	{"AUX3", K_AUX3},
	{"AUX4", K_AUX4},
	{"AUX5", K_AUX5},
	{"AUX6", K_AUX6},
	{"AUX7", K_AUX7},
	{"AUX8", K_AUX8},
	{"AUX9", K_AUX9},
	{"AUX10", K_AUX10},
	{"AUX11", K_AUX11},
	{"AUX12", K_AUX12},
	{"AUX13", K_AUX13},
	{"AUX14", K_AUX14},
	{"AUX15", K_AUX15},
	{"AUX16", K_AUX16},
	{"AUX17", K_AUX17},
	{"AUX18", K_AUX18},
	{"AUX19", K_AUX19},
	{"AUX20", K_AUX20},
	{"AUX21", K_AUX21},
	{"AUX22", K_AUX22},
	{"AUX23", K_AUX23},
	{"AUX24", K_AUX24},
	{"AUX25", K_AUX25},
	{"AUX26", K_AUX26},
	{"AUX27", K_AUX27},
	{"AUX28", K_AUX28},
	{"AUX29", K_AUX29},
	{"AUX30", K_AUX30},
	{"AUX31", K_AUX31},
	{"AUX32", K_AUX32},

	{"KP_HOME",			K_KP_HOME },
	{"KP_UPARROW",		K_KP_UPARROW },
	{"KP_PGUP",			K_KP_PGUP },
	{"KP_LEFTARROW",	K_KP_LEFTARROW },
	{"KP_5",			K_KP_5 },
	{"KP_RIGHTARROW",	K_KP_RIGHTARROW },
	{"KP_END",			K_KP_END },
	{"KP_DOWNARROW",	K_KP_DOWNARROW },
	{"KP_PGDN",			K_KP_PGDN },
	{"KP_ENTER",		K_KP_ENTER },
	{"KP_INS",			K_KP_INS },
	{"KP_DEL",			K_KP_DEL },
	{"KP_SLASH",		K_KP_SLASH },
	{"KP_MINUS",		K_KP_MINUS },
	{"KP_PLUS",			K_KP_PLUS },

	{"MWHEELUP", K_MWHEELUP },
	{"MWHEELDOWN", K_MWHEELDOWN },

	{"PAUSE", K_PAUSE},

	{"SEMICOLON", ';'},	// because a raw semicolon seperates commands

	{NULL,0}
};

keyname_t keynames_clean[] =
{
	{"L1", K_TAB},
	{"L2", K_PGUP},
	{"L3", K_KP_SLASH},
	{"START", K_ENTER},
	{"SELECT", K_ESCAPE},
	{"R1", K_BACKSPACE},
	{"R2", K_PGDN},
	{"R3", K_KP_DEL},
	{"UP", K_UPARROW},
	{"DOWN", K_DOWNARROW},
	{"LEFT", K_LEFTARROW},
	{"RIGHT", K_RIGHTARROW},

	{"A", K_CTRL},
	{"B", K_ALT},
	{"X", K_SPACE},
	{"Y", K_SHIFT},

	{"POWER", K_HOME},

	{NULL,0}
};


/*
==============================================================================

			VIRTUAL KEYBOARD (gamepad-only console typing)

	Character grid navigable with the D-pad, meant for handheld consoles
	with no physical keyboard. Reuses the existing mapping defined in
	keynames_clean:
		D-pad           -> moves the cursor around the grid
		A (CTRL)        -> types the selected character
		B (ALT)         -> backspace
		X (SPACE)       -> space
		Y (SHIFT)       -> toggles uppercase/lowercase
		L1 (TAB)        -> complete command
		R1 (BACKSPACE)  -> next page (letters <-> symbols)
		L2 (PGUP)       -> console up
		R2 (PGDN)       -> console down
		START (ENTER)   -> closes the virtual keyboard (same as HOME)
		SELECT (ESCAPE) -> submits the typed command, whether the
			virtual keyboard is open or closed (see the SELECT block in
			Key_Event), but does NOT close it -- HOME/START are what
			close it
		HOME            -> opens/closes the virtual keyboard (see Key_Console)

	NOTE: HOME is reused here for the toggle (no L3 on this device).
	While the virtual keyboard toggle is bound to HOME, the normal
	"scroll backlog to top" behavior of HOME is unavailable in console
	mode; K_KP_HOME (numpad Home) is untouched and still scrolls the
	backlog as before.

	Holding ANY of the keys above auto-repeats while the virtual
	keyboard is open, via VKB_Think(), polled once per rendered frame
	from Con_DrawKeyboard() in console.c, using keydown[] and
	cls.realtime -- NOT Key_Event's own key_repeats counter, which only
	increments on repeated down events and this gamepad backend
	apparently never generates those for held buttons (unlike a PC
	keyboard's OS-level autorepeat, which is what that counter was
	originally built for). Repeat speed accelerates the longer a key is
	held (see the VKB_REPEAT_* constants below).

	Rows shorter than KB_COLS end with padding spaces so every row lines
	up in a KB_COLS-wide grid (see kb_layout below); kb_row_len gives the
	real number of usable keys per row, and column navigation wraps
	within that real length rather than the full padded width, so you
	can never land on a blank filler cell without visual feedback.
==============================================================================
*/

#define	KB_ROWS		4
#define	KB_COLS		10
#define	KB_PAGES	2

// how long (ms) a direction must be held before it starts repeating, the
// interval between repeats right after that, and the interval once it
// has been held past VKB_ACCEL_AFTER ms (faster = feels more responsive
// when scrolling all the way across the grid)
#define	VKB_REPEAT_DELAY	350
#define	VKB_REPEAT_SLOW		160
#define	VKB_REPEAT_FAST		80
#define	VKB_ACCEL_AFTER		0

qboolean	kb_active = false;
int			kb_row = 0;
int			kb_col = 0;
int			kb_page = 0;
qboolean	kb_caps = false;

// each row must be exactly KB_COLS characters; a space is padding used
// to keep every row the same width for the grid layout below -- it is
// NOT a selectable key (see kb_row_len)
static const char *kb_layout[KB_PAGES][KB_ROWS] =
{
	// page 0: alphabet + common punctuation
	{
		"1234567890",
		"qwertyuiop",
		"asdfghjkl ",
		"zxcvbnm.: "
	},
	// page 1: symbols (IPs, paths, cheats...)
	{
		"!@#$%^&*()",
		"-_=+[]{}\\|",
		"/:;\"'<>,.?",
		"~`        "
	}
};

// number of real, selectable keys at the start of each row (the rest is
// padding, see kb_layout above) -- keep in sync with it by hand
static const int kb_row_len[KB_PAGES][KB_ROWS] =
{
	{ 10, 10, 9, 9 },
	{ 10, 10, 10, 2 }
};

/*
================
VKB_GetChar
================
*/
char VKB_GetChar (int page, int row, int col)
{
	char c = kb_layout[page][row][col];

	if (kb_caps && c >= 'a' && c <= 'z')
		c -= 32;

	return c;
}

/*
================
Key_ConsoleActive
================
*/
static qboolean Key_ConsoleActive (void)
{
	if (cls.key_dest == key_console)
		return true;

	return cls.key_dest == key_game && (cls.state != ca_active || cl.attractloop);
}

/*
================
VKB_Toggle
================
*/
void VKB_Toggle (void)
{
	kb_active = !kb_active;
	kb_row = 0;
	kb_col = 0;
}

/*
================
VKB_InsertChar
================
*/
static void VKB_InsertChar (char c)
{
	if (key_linepos < MAXCMDLINE - 1)
	{
		key_lines[edit_line][key_linepos] = c;
		key_linepos++;
		key_lines[edit_line][key_linepos] = 0;
	}
}

/*
================
Key_ConsoleSubmit
================
*/
static void Key_ConsoleSubmit (void)
{
	if (key_lines[edit_line][1] == '\\' || key_lines[edit_line][1] == '/')
		Cbuf_AddText (key_lines[edit_line]+2);
	else
		Cbuf_AddText (key_lines[edit_line]+1);

	Cbuf_AddText ("\n");
	Com_Printf ("%s\n", key_lines[edit_line]);
	edit_line = (edit_line + 1) & 31;
	history_line = edit_line;
	key_lines[edit_line][0] = ']';
	key_linepos = 1;
	if (cls.state == ca_disconnected)
		SCR_UpdateScreen ();	// force an update, because the command
								// may take some time
}

/*
================
VKB_ClampCol
================
*/
static void VKB_ClampCol (void)
{
	int len = kb_row_len[kb_page][kb_row];

	if (kb_col >= len)
		kb_col = len - 1;
}

/*
================
VKB_MoveRow / VKB_MoveCol
================
*/
static void VKB_MoveRow (int step)
{
	kb_row = (kb_row + step + KB_ROWS * 4) % KB_ROWS;
	VKB_ClampCol ();
}

static void VKB_MoveCol (int step)
{
	int len = kb_row_len[kb_page][kb_row];

	kb_col = (kb_col + step + len * 4) % len;
}

/*
================
VKB_HandleKey
================
*/
qboolean VKB_HandleKey (int key)
{
	switch (key)
	{
	case K_UPARROW:
	case K_KP_UPARROW:
		VKB_MoveRow (-1);
		return true;

	case K_DOWNARROW:
	case K_KP_DOWNARROW:
		VKB_MoveRow (1);
		return true;

	case K_LEFTARROW:
	case K_KP_LEFTARROW:
		VKB_MoveCol (-1);
		return true;

	case K_RIGHTARROW:
	case K_KP_RIGHTARROW:
		VKB_MoveCol (1);
		return true;

	case K_CTRL:		// A button: type the selected character
		VKB_InsertChar (VKB_GetChar (kb_page, kb_row, kb_col));
		return true;

	case K_ALT:			// B button: backspace
		if (key_linepos > 1)
			key_linepos--;
		return true;

	case K_SPACE:		// X button: space
		VKB_InsertChar (' ');
		return true;

	case K_SHIFT:		// Y button: toggle uppercase/lowercase
		kb_caps = !kb_caps;
		return true;

	case K_TAB:			// L1: complete command
		CompleteCommand ();
		return true;

	case K_BACKSPACE:	// R1: next page
		kb_page = (kb_page + 1) % KB_PAGES;
		VKB_ClampCol ();
		return true;

	case K_PGUP:		// L2: console up
	case K_KP_PGUP:
		con.display -= 2;
		return true;

	case K_PGDN:		// R2: console down
	case K_KP_PGDN:
		con.display += 2;
		if (con.display > con.current)
			con.display = con.current;
		return true;

	case K_ENTER:
	case K_KP_ENTER:
		kb_active = false;
		return true;

	default:
		return false;	// key not handled by the virtual keyboard
	}
}

/*
================
VKB_Think
================
*/
typedef struct
{
	int			key;
	int			key_alt;		// secondary keycode for the same action, 0 if none
	unsigned	press_time;		// cls.realtime when this key was first seen held, 0 if not held
	unsigned	next_time;		// cls.realtime of the next scheduled repeat
} vkb_hold_t;

static vkb_hold_t vkb_holds[] =
{
	{ K_UPARROW,	K_KP_UPARROW,		0, 0 },
	{ K_DOWNARROW,	K_KP_DOWNARROW,		0, 0 },
	{ K_LEFTARROW,	K_KP_LEFTARROW,		0, 0 },
	{ K_RIGHTARROW,	K_KP_RIGHTARROW,	0, 0 },
	{ K_CTRL,		0,					0, 0 },		// A: type selected char
	{ K_ALT,		0,					0, 0 },		// B: backspace
	{ K_SPACE,		0,					0, 0 },		// X: space
	{ K_SHIFT,		0,					0, 0 },		// Y: toggle case
	{ K_TAB,		0,					0, 0 },		// L1: complete command
	{ K_BACKSPACE,	0,					0, 0 },		// R1: next page (letters <-> symbols)
	{ K_PGUP,		K_KP_PGUP,			0, 0 },		// L2: console up
	{ K_PGDN,		K_KP_PGDN,			0, 0 },		// R2: console down
	{ K_ENTER,		K_KP_ENTER,			0, 0 }		// START: close
};

#define	VKB_HOLD_COUNT	((int)(sizeof(vkb_holds) / sizeof(vkb_holds[0])))

void VKB_Think (void)
{
	int			i;
	qboolean	held;
	unsigned	held_for, interval;

	if (!kb_active)
		return;

	for (i = 0 ; i < VKB_HOLD_COUNT ; i++)
	{
		held = keydown[vkb_holds[i].key]
			|| (vkb_holds[i].key_alt && keydown[vkb_holds[i].key_alt]);

		if (!held)
		{
			vkb_holds[i].press_time = 0;
			continue;
		}

		if (vkb_holds[i].press_time == 0)
		{
			vkb_holds[i].press_time = cls.realtime;
			vkb_holds[i].next_time = cls.realtime + VKB_REPEAT_DELAY;
			continue;
		}

		if (cls.realtime < vkb_holds[i].next_time)
			continue;

		held_for = cls.realtime - vkb_holds[i].press_time;
		interval = (held_for > VKB_ACCEL_AFTER) ? VKB_REPEAT_FAST : VKB_REPEAT_SLOW;

		VKB_HandleKey (vkb_holds[i].key);

		// kb_active may have just been turned off (K_ENTER) -- stop
		// touching state for the rest of this pass if so
		if (!kb_active)
			return;

		vkb_holds[i].next_time = cls.realtime + interval;
	}
}


/*
==============================================================================

			LINE TYPING INTO THE CONSOLE

==============================================================================
*/

void CompleteCommand (void)
{
	char	*cmd, *s;

	s = key_lines[edit_line]+1;
	if (*s == '\\' || *s == '/')
		s++;

	cmd = Cmd_CompleteCommand (s);
	if (!cmd)
		cmd = Cvar_CompleteVariable (s);
	if (cmd)
	{
		key_lines[edit_line][1] = '/';
		strcpy (key_lines[edit_line]+2, cmd);
		key_linepos = strlen(cmd)+2;
		key_lines[edit_line][key_linepos] = ' ';
		key_linepos++;
		key_lines[edit_line][key_linepos] = 0;
		return;
	}
}

/*
====================
Key_Console

Interactive line editing and console scrollback
====================
*/
void Key_Console (int key)
{
	if (key == K_HOME)
	{
		VKB_Toggle ();
		return;
	}

	switch ( key )
	{
	case K_KP_SLASH:
		key = '/';
		break;
	case K_KP_MINUS:
		key = '-';
		break;
	case K_KP_PLUS:
		key = '+';
		break;
	case K_KP_HOME:
		key = '7';
		break;
	case K_KP_UPARROW:
		key = '8';
		break;
	case K_KP_PGUP:
		key = '9';
		break;
	case K_KP_LEFTARROW:
		key = '4';
		break;
	case K_KP_5:
		key = '5';
		break;
	case K_KP_RIGHTARROW:
		key = '6';
		break;
	case K_KP_END:
		key = '1';
		break;
	case K_KP_DOWNARROW:
		key = '2';
		break;
	case K_KP_PGDN:
		key = '3';
		break;
	case K_KP_INS:
		key = '0';
		break;
	case K_KP_DEL:
		key = '.';
		break;
	default:
		break;
	}

	if ( ( toupper( key ) == 'V' && keydown[K_CTRL] ) ||
		 ( ( ( key == K_INS ) || ( key == K_KP_INS ) ) && keydown[K_SHIFT] ) )
	{
		char *cbd;
		
		if ( ( cbd = Sys_GetClipboardData() ) != 0 )
		{
			int i;

			strtok( cbd, "\n\r\b" );

			i = strlen( cbd );
			if ( i + key_linepos >= MAXCMDLINE)
				i= MAXCMDLINE - key_linepos;

			if ( i > 0 )
			{
				cbd[i]=0;
				strcat( key_lines[edit_line], cbd );
				key_linepos += i;
			}
			free( cbd );
		}

		return;
	}

	if ( key == 'l' ) 
	{
		if ( keydown[K_CTRL] )
		{
			Cbuf_AddText ("clear\n");
			return;
		}
	}

	if ( key == K_ENTER || key == K_KP_ENTER )
	{	// backslash text are commands, else chat
		Key_ConsoleSubmit ();
		return;
	}

	if (key == K_TAB)
	{	// command completion
		CompleteCommand ();
		return;
	}
	
	if ( ( key == K_BACKSPACE ) || ( key == K_LEFTARROW ) || ( key == K_KP_LEFTARROW ) || ( ( key == 'h' ) && ( keydown[K_CTRL] ) ) )
	{
		if (key_linepos > 1)
			key_linepos--;
		return;
	}

	if ( ( key == K_UPARROW ) || ( key == K_KP_UPARROW ) ||
		 ( ( key == 'p' ) && keydown[K_CTRL] ) )
	{
		do
		{
			history_line = (history_line - 1) & 31;
		} while (history_line != edit_line
				&& !key_lines[history_line][1]);
		if (history_line == edit_line)
			history_line = (edit_line+1)&31;
		strcpy(key_lines[edit_line], key_lines[history_line]);
		key_linepos = strlen(key_lines[edit_line]);
		return;
	}

	if ( ( key == K_DOWNARROW ) || ( key == K_KP_DOWNARROW ) ||
		 ( ( key == 'n' ) && keydown[K_CTRL] ) )
	{
		if (history_line == edit_line) return;
		do
		{
			history_line = (history_line + 1) & 31;
		}
		while (history_line != edit_line
			&& !key_lines[history_line][1]);
		if (history_line == edit_line)
		{
			key_lines[edit_line][0] = ']';
			key_linepos = 1;
		}
		else
		{
			strcpy(key_lines[edit_line], key_lines[history_line]);
			key_linepos = strlen(key_lines[edit_line]);
		}
		return;
	}

	if (key == K_PGUP || key == K_KP_PGUP || key == K_MWHEELUP )
	{
		con.display -= 2;
		return;
	}

	if (key == K_PGDN || key == K_KP_PGDN || key == K_MWHEELDOWN ) 
	{
		con.display += 2;
		if (con.display > con.current)
			con.display = con.current;
		return;
	}

	if (key == K_HOME || key == K_KP_HOME )
	{
		con.display = con.current - con.totallines + 10;
		return;
	}

	if (key == K_END || key == K_KP_END )
	{
		con.display = con.current;
		return;
	}
	
	if (key < 32 || key > 127)
		return;	// non printable
		
	if (key_linepos < MAXCMDLINE-1)
	{
		key_lines[edit_line][key_linepos] = key;
		key_linepos++;
		key_lines[edit_line][key_linepos] = 0;
	}

}

//============================================================================

qboolean	chat_team;
char		chat_buffer[MAXCMDLINE];
int			chat_bufferlen = 0;

void Key_Message (int key)
{

	if ( key == K_ENTER || key == K_KP_ENTER )
	{
		if (chat_team)
			Cbuf_AddText ("say_team \"");
		else
			Cbuf_AddText ("say \"");
		Cbuf_AddText(chat_buffer);
		Cbuf_AddText("\"\n");

		cls.key_dest = key_game;
		chat_bufferlen = 0;
		chat_buffer[0] = 0;
		return;
	}

	if (key == K_ESCAPE)
	{
		cls.key_dest = key_game;
		chat_bufferlen = 0;
		chat_buffer[0] = 0;
		return;
	}

	if (key < 32 || key > 127)
		return;	// non printable

	if (key == K_BACKSPACE)
	{
		if (chat_bufferlen)
		{
			chat_bufferlen--;
			chat_buffer[chat_bufferlen] = 0;
		}
		return;
	}

	if (chat_bufferlen == sizeof(chat_buffer)-1)
		return; // all full

	chat_buffer[chat_bufferlen++] = key;
	chat_buffer[chat_bufferlen] = 0;
}

//============================================================================


/*
===================
Key_StringToKeynum

Returns a key number to be used to index keybindings[] by looking at
the given string.  Single ascii characters return themselves, while
the K_* names are matched up.
===================
*/
int Key_StringToKeynum (char *str)
{
	keyname_t	*kn;
	
	if (!str || !str[0])
		return -1;
	if (!str[1])
		return str[0];

	for (kn=keynames ; kn->name ; kn++)
	{
		if (!Q_strcasecmp(str,kn->name))
			return kn->keynum;
	}
	return -1;
}

/*
===================
Key_KeynumToString

Returns a string (either a single ascii char, or a K_* name) for the
given keynum.
FIXME: handle quote special (general escape sequence?)
===================
*/
char *Key_KeynumToString (int keynum)
{
	keyname_t	*kn;	
	static	char	tinystr[2];
	
	if (keynum == -1)
		return "<KEY NOT FOUND>";
	if (keynum > 32 && keynum < 127)
	{	// printable ascii
	tinystr[0] = keynum;
	tinystr[1] = 0;
	return tinystr;
	}
	
	for (kn=keynames ; kn->name ; kn++)
		if (keynum == kn->keynum)
			return kn->name;
		
		return "<UNKNOWN KEYNUM>";
}

char *Key_KeynumToString_clean (int keynum)
{
	keyname_t	*kn;	
	static	char	tinystr[2];
	
	if (keynum == -1)
		return "?";
	if (keynum > 32 && keynum < 127)
	{	// printable ascii
	tinystr[0] = keynum;
	tinystr[1] = 0;
	return "?";
	}
	
	for (kn=keynames_clean ; kn->name ; kn++)
		if (keynum == kn->keynum)
			return kn->name;
		
		return "?";
}


/*
===================
Key_SetBinding
===================
*/
void Key_SetBinding (int keynum, char *binding)
{
	char	*new;
	int		l;
			
	if (keynum == -1)
		return;

// free old bindings
	if (keybindings[keynum])
	{
		Z_Free (keybindings[keynum]);
		keybindings[keynum] = NULL;
	}
			
// allocate memory for new binding
	l = strlen (binding);	
	new = Z_Malloc (l+1);
	strcpy (new, binding);
	new[l] = 0;
	keybindings[keynum] = new;	
}

/*
===================
Key_Unbind_f
===================
*/
void Key_Unbind_f (void)
{
	int		b;

	if (Cmd_Argc() != 2)
	{
		Com_Printf ("unbind <key> : remove commands from a key\n");
		return;
	}
	
	b = Key_StringToKeynum (Cmd_Argv(1));
	if (b==-1)
	{
		Com_Printf ("\"%s\" isn't a valid key\n", Cmd_Argv(1));
		return;
	}

	Key_SetBinding (b, "");
}

void Key_Unbindall_f (void)
{
	int		i;
	
	for (i=0 ; i<K_LAST ; i++)
		if (keybindings[i])
			Key_SetBinding (i, "");
}


/*
===================
Key_Bind_f
===================
*/
void Key_Bind_f (void)
{
	int			i, c, b;
	char		cmd[1024];
	
	c = Cmd_Argc();

	if (c < 2)
	{
		Com_Printf ("bind <key> [command] : attach a command to a key\n");
		return;
	}
	b = Key_StringToKeynum (Cmd_Argv(1));
	if (b==-1)
	{
		Com_Printf ("\"%s\" isn't a valid key\n", Cmd_Argv(1));
		return;
	}

	if (c == 2)
	{
		if (keybindings[b])
			Com_Printf ("\"%s\" = \"%s\"\n", Cmd_Argv(1), keybindings[b] );
		else
			Com_Printf ("\"%s\" is not bound\n", Cmd_Argv(1) );
		return;
	}
	
// copy the rest of the command line
	cmd[0] = 0;		// start out with a null string
	for (i=2 ; i< c ; i++)
	{
		strcat (cmd, Cmd_Argv(i));
		if (i != (c-1))
			strcat (cmd, " ");
	}

	Key_SetBinding (b, cmd);
}

/*
============
Key_WriteBindings

Writes lines containing "bind key value"
============
*/
void Key_WriteBindings (FILE *f)
{
	int		i;

	for (i=0 ; i<K_LAST ; i++)
		if (keybindings[i] && keybindings[i][0])
			fprintf (f, "bind %s \"%s\"\n", Key_KeynumToString(i), keybindings[i]);
}


/*
============
Key_Bindlist_f

============
*/
void Key_Bindlist_f (void)
{
	int		i;

	for (i=0 ; i<K_LAST ; i++)
		if (keybindings[i] && keybindings[i][0])
			Com_Printf ("%s \"%s\"\n", Key_KeynumToString(i), keybindings[i]);
}


/*
===================
Key_Init
===================
*/
void Key_Init (void)
{
	int		i;

	for (i=0 ; i<32 ; i++)
	{
		key_lines[i][0] = ']';
		key_lines[i][1] = 0;
	}
	key_linepos = 1;
	
//
// init ascii characters in console mode
//
	for (i=32 ; i<128 ; i++)
		consolekeys[i] = true;
	consolekeys[K_ENTER] = true;
	consolekeys[K_KP_ENTER] = true;
	consolekeys[K_TAB] = true;
	consolekeys[K_LEFTARROW] = true;
	consolekeys[K_KP_LEFTARROW] = true;
	consolekeys[K_RIGHTARROW] = true;
	consolekeys[K_KP_RIGHTARROW] = true;
	consolekeys[K_UPARROW] = true;
	consolekeys[K_KP_UPARROW] = true;
	consolekeys[K_DOWNARROW] = true;
	consolekeys[K_KP_DOWNARROW] = true;
	consolekeys[K_BACKSPACE] = true;
	consolekeys[K_HOME] = true;
	consolekeys[K_KP_HOME] = true;
	consolekeys[K_END] = true;
	consolekeys[K_KP_END] = true;
	consolekeys[K_PGUP] = true;
	consolekeys[K_KP_PGUP] = true;
	consolekeys[K_PGDN] = true;
	consolekeys[K_KP_PGDN] = true;
	consolekeys[K_SHIFT] = true;
	consolekeys[K_INS] = true;
	consolekeys[K_KP_INS] = true;
	consolekeys[K_KP_DEL] = true;
	consolekeys[K_KP_SLASH] = true;
	consolekeys[K_KP_PLUS] = true;
	consolekeys[K_KP_MINUS] = true;
	consolekeys[K_KP_5] = true;

	consolekeys[K_MWHEELUP] = true;
	consolekeys[K_MWHEELDOWN] = true;

	consolekeys['`'] = false;
	consolekeys['~'] = false;

	for (i=0 ; i<K_LAST ; i++)
		keyshift[i] = i;
	for (i='a' ; i<='z' ; i++)
		keyshift[i] = i - 'a' + 'A';
	keyshift['1'] = '!';
	keyshift['2'] = '@';
	keyshift['3'] = '#';
	keyshift['4'] = '$';
	keyshift['5'] = '%';
	keyshift['6'] = '^';
	keyshift['7'] = '&';
	keyshift['8'] = '*';
	keyshift['9'] = '(';
	keyshift['0'] = ')';
	keyshift['-'] = '_';
	keyshift['='] = '+';
	keyshift[','] = '<';
	keyshift['.'] = '>';
	keyshift['/'] = '?';
	keyshift[';'] = ':';
	keyshift['\''] = '"';
	keyshift['['] = '{';
	keyshift[']'] = '}';
	keyshift['`'] = '~';
	keyshift['\\'] = '|';

	menubound[K_ENTER] = true;
	for (i=0 ; i<12 ; i++)
		menubound[K_F1+i] = true;

//
// register our functions
//
	Cmd_AddCommand ("bind",Key_Bind_f);
	Cmd_AddCommand ("unbind",Key_Unbind_f);
	Cmd_AddCommand ("unbindall",Key_Unbindall_f);
	Cmd_AddCommand ("bindlist",Key_Bindlist_f);
}

/*
===================
Key_Event

Called by the system between frames for both key up and key down events
Should NOT be called during an interrupt!
===================
*/
void Key_Event (int key, qboolean down, unsigned time)
{
	char	*kb;
	char	cmd[1024];

	// hack for modal presses
	if (key_waiting == -1)
	{
		if (down)
			key_waiting = key;
		return;
	}

	// update auto-repeat status
	if (down)
	{
		key_repeats[key]++;
		if (key != K_BACKSPACE 
		    && key != K_PAUSE 
		    && key != K_PGUP 
		    && key != K_KP_PGUP 
		    && key != K_PGDN
		    && key != K_KP_PGDN
		    && key_repeats[key] > 1)
		  return;	// ignore most autorepeats
			
		if (key >= 200 && !keybindings[key] && cls.key_dest != key_console)
			Com_Printf ("%s is unbound, hit F4 to set.\n", Key_KeynumToString (key) );
	}
	else
	{
		key_repeats[key] = 0;
	}

	if (key == K_SHIFT)
		shift_down = down;

	// console key is hardcoded, so the user can never unbind it
	if ((key == '`' || key == '~') && !cls.disable_screen)
	{
		if (!down)
			return;
		Con_ToggleConsole_f ();
		return;
	}

	if (down && Key_ConsoleActive () && kb_active)
	{
		keydown[key] = true;

		if (VKB_HandleKey (key))
			return;
	}

	if (down && Key_ConsoleActive () && key == K_ESCAPE)
	{
		Key_ConsoleSubmit ();
		return;
	}

	// any key during the attract mode will bring up the menu
	/*if (cl.attractloop && cls.key_dest != key_menu &&
		!(key >= K_F1 && key <= K_F12))
		key = K_ENTER;*/

	// menu key is hardcoded, so the user can never unbind it
	if (key == K_ENTER && !cls.disable_screen)
	{
		keydown[key] = down;

		if (!down)
			return;

		if (cl.frame.playerstate.stats[STAT_LAYOUTS] && cls.key_dest == key_game)
		{	// put away help computer / inventory
			Cbuf_AddText ("cmd putaway\n");
			return;
		}
		switch (cls.key_dest)
		{
		case key_message:
			Key_Message (key);
			break;
		case key_menu:
			M_Keydown (key);
			break;
		case key_game:
		case key_console:
			M_Menu_Main_f ();
			break;
		default:
			Com_Error (ERR_FATAL, "Bad cls.key_dest");
		}
		return;
	}

	// track if any key is down for BUTTON_ANY
	keydown[key] = down;
	if (down)
	{
		if (key_repeats[key] == 1)
			anykeydown++;
	}
	else
	{
		anykeydown--;
		if (anykeydown < 0)
			anykeydown = 0;
	}

//
// key up events only generate commands if the game key binding is
// a button command (leading + sign).  These will occur even in console mode,
// to keep the character from continuing an action started before a console
// switch.  Button commands include the kenum as a parameter, so multiple
// downs can be matched with ups
//
	if (!down)
	{
		kb = keybindings[key];
		if (kb && kb[0] == '+')
		{
			Com_sprintf (cmd, sizeof(cmd), "-%s %i %i\n", kb+1, key, time);
			Cbuf_AddText (cmd);
		}
		if (keyshift[key] != key)
		{
			kb = keybindings[keyshift[key]];
			if (kb && kb[0] == '+')
			{
				Com_sprintf (cmd, sizeof(cmd), "-%s %i %i\n", kb+1, key, time);
				Cbuf_AddText (cmd);
			}
		}
		return;
	}

//
// if not a consolekey, send to the interpreter no matter what mode is
//
	if ( (cls.key_dest == key_menu && menubound[key])
	|| (cls.key_dest == key_console && !consolekeys[key])
	|| (cls.key_dest == key_game && ( cls.state == ca_active || !consolekeys[key] ) ) )
	{
		kb = keybindings[key];
		if (kb)
		{
			if (kb[0] == '+')
			{	// button commands add keynum and time as a parm
				Com_sprintf (cmd, sizeof(cmd), "%s %i %i\n", kb, key, time);
				Cbuf_AddText (cmd);
			}
			else
			{
				Cbuf_AddText (kb);
				Cbuf_AddText ("\n");
			}
		}
		return;
	}

	if (!down)
		return;		// other systems only care about key down events

	if (shift_down)
		key = keyshift[key];

	switch (cls.key_dest)
	{
	case key_message:
		Key_Message (key);
		break;
	case key_menu:
		M_Keydown (key);
		break;

	case key_game:
	case key_console:
		Key_Console (key);
		break;
	default:
		Com_Error (ERR_FATAL, "Bad cls.key_dest");
	}
}

/*
===================
Key_ClearStates
===================
*/
void Key_ClearStates (void)
{
	int		i;

	anykeydown = false;

	for (i=0 ; i<K_LAST ; i++)
	{
		if ( keydown[i] || key_repeats[i] )
			Key_Event( i, false, 0 );
		keydown[i] = 0;
		key_repeats[i] = 0;
	}
}


/*
===================
Key_GetKey
===================
*/
int Key_GetKey (void)
{
	key_waiting = -1;

	while (key_waiting == -1)
		Sys_SendKeyEvents ();

	return key_waiting;
}

