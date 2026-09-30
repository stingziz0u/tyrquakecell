// ps3_osk.h -- the console with the pad and the XMB keyboard (ps3_osk.c)

#ifndef PS3_OSK_H
#define PS3_OSK_H

#include "qtypes.h"

// Takes the pad's button mask for this frame and returns what the game
// should see: handles SELECT + TRIANGLE (console), the console's own pad
// controls and the XMB keyboard. Called once per IN_Commands().
unsigned PS3_ConsolePad_Filter(unsigned raw);

qboolean PS3_OSK_Active(void);
void PS3_OSK_Shutdown(void);

#endif
