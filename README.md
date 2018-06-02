# GLQuake3D

This source-port based on WinQuake/GLQuake and have this changes:
1. Incrased the engine limits
2. Improved glowing on entities
3. Added FOG (Experimental)
4. Incrased the audio samplerate from 11.025 Hz to 44.100 Hz
5. Added new icon and loading bar (still in developing)

Advantages:
1. This port can run on NT5.x+ (XP,2003)

TODO:
1. Add the current level checking for chaning the fog (also check id1 folder or no)
2. Improve shadows code
3. Test it on ReactOS
4. Add the windows userstyles support
5. Fix the weapon interoplating

Current projects problems:
1. Current working only "GL Release" & "GL Debug" (WinQuake's Debug & Release currently not working.)
2. Not abiliable Linux makefiles
3. Render is still glitching sometimes (to reproduce this bug, use the this command: gl_flashblend 1)
4. Fog incorrectly rendering the glowing and flash-blending

Credits:
ID Software: Quake, WinQuake, GLQuake
DartPower: Code rewrite
D'Sparil: Common engine improvements ideas and game icon, also for testing