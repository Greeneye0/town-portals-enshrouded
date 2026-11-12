Town Portals 2.0.2 for Enshrouded
Saving your ass since 2026.

Open a portal home from anywhere. Press T: after a short cast, a portal opens in front
of you and a second one beside your Flame Altar (the one marked as your preferred respawn).
Step through, travel home, and come back through the same portal to where you were.

HOW IT WORKS
- Press T (OpenPortalKey in town-portal.ini) to cast. The cast takes 3 seconds and you
  cannot move during it; press Jump to cancel. The portal forms in front of you, and its
  twin forms beside your preferred Flame Altar.
- Hold T for a second to close your portal (a portal still forming is cancelled). Your
  own portal's prompt shows this too.
- Walk up to either end and press E to travel (or the key you gave Interact in the game's
  controls). While the portal's prompt shows, that key is the portal's only: a grapple
  point, workbench or altar next to it will not react too.
- Each player has one portal. Opening a new one replaces your old one.
- Other players can use your portal (the prompt says it is shared). Everyone in the world
  needs Town Portals; on a dedicated server, the server needs it too (server/).
- The portal shows the other side live, like a window: things move in it, day and night
  change, and you can walk round it to see more. People, animals and enemies near the far
  end show too (on a dedicated server, the server needs Town Portals 2.0 for that). While
  the far side loads, the portal shows its last picture of the place as if through water.
  LiveView in town-portal.ini: Auto (default: live only with the game's Ultra Quality preset
  on an NVIDIA card with 8 GB or more, else the still pictures), Full, Balanced (no water in
  the far view), Fast
  (also no bounced light, darker) or Off (the 1.x still pictures, with depth as you
  move: no live view, no frame rate cost). The live view costs
  frame rate while a portal is on screen; Balanced, Fast or Off give some back.
- While a portal is open, the area round each of its ends stays loaded on the host, so the
  creatures there stay too.
- An open portal hums softly when you stand near it (the game's Master and Ambience volume).
- While you travel, a painted world map shows the trip: your portal, your altar, and a
  path that grows as the destination loads, with a loading percentage. The map is
  lettered in the game's language (your own map: map\map-<language>.jpg, e.g. map-ru.jpg).
- On low graphics settings (Quality Preset Lowest, Low or Medium) portals draw a lighter
  version: a coarser picture and no light on the ground. PortalQuality in the .ini
  overrides it.
- All text in Enshrouded's 15 languages, following the game's language. Your character
  says a line when a portal cannot open (VoiceFeedback=false turns that off).

WHEN PORTALS CLOSE
- Going home does not close your portal: you can always go back the way you came.
- Coming back from your altar closes it behind you. If other players are in the world it
  stays open 15 seconds first, so they can follow you.
- With DisableAutoClose=true (server setting) it stays open until you open a new one.
- Holding T for a second closes it whenever you like.
- Jumping during the cast cancels the portal.
- Portals do not last past leaving the world.

WHERE PORTALS CAN'T OPEN
- Too close to a wall (there is no room).
- In the Shroud or the Hollow Halls, if the host turns that off (AllowInShroud,
  AllowInHollowHalls). Both are allowed by default.

INSTALL
client/ is for players, server/ is for dedicated servers only.
- Playing alone, hosting from your game, or joining one: the client files. Everyone in
  the world needs Town Portals.
- Dedicated server: every player installs the client files, AND the server gets the
  server files.

Client (every player)
1. Close the game.
2. Open the game folder: in Steam, right-click Enshrouded > Manage > Browse local files
   (the folder with enshrouded.exe, usually ...\steamapps\common\Enshrouded).
3. Copy what is INSIDE client/ (dbgcore.dll and the greeneye folder) into it. Don't copy
   the client folder itself.
4. If the greeneye folder already exists, let it merge. If dbgcore.dll already exists,
   skip it: another Greeneye mod already installed the loader.
   Result: Enshrouded\dbgcore.dll and Enshrouded\greeneye\town-portal\town-portal.dll

Dedicated server (server admin only)
1. Stop the server.
2. Open the server's folder (the one with enshrouded_server.exe, usually
   ...\steamapps\common\EnshroudedServer).
3. Copy what is INSIDE server/ (dbgcore.dll and the greeneye folder) into it.
4. Start the server; greeneye\town-portal\town-portal.ini is written there.
   Result: EnshroudedServer\dbgcore.dll and EnshroudedServer\greeneye\town-portal\town-portal.dll
   On a rented server, upload the same files next to enshrouded_server.exe with the
   host's file manager (if it lets you add DLL files).

Check: greeneye\loader.log has a line with "Town Portals 2.0.2" (the server's says
"server companion loaded").
Uninstall: delete greeneye\town-portal (and dbgcore.dll if no other Greeneye mod uses
it), in the game folder and on the server.

SETTINGS: greeneye\town-portal\town-portal.ini (written on first start)
[Portals] (the host's file: the dedicated server's, or the hosting player's)
  AllowInShroud=true, AllowInHollowHalls=true, DisableAutoClose=false
  LiveView=Auto          Auto (live only with the Ultra Quality preset on an NVIDIA card with
                         8 GB or more; else still pictures), Full, Balanced, Fast or Off
                         (each player's own: read from your file)
[Controls] (each player's own)
  OpenPortalKey=T        a letter, F1-F24, Mouse4, Mouse5, ... or a key code like 0x54
  InteractKey=Auto       the key that uses a portal: Auto follows the game's Interact key
  VoiceFeedback=true
  PortalQuality=Auto     Auto, Low or High
  PortalColor=Auto       Auto, or a colour: yellow, blue, red, green, purple, orange, cyan, pink, white, lime, teal, magenta, brown, navy, olive or maroon
Picture sharing: delete greeneye\town-portal\share-snapshots.enabled to turn it off.

KNOWN LIMITATIONS
- The live view costs frame rate while a portal is on screen (on the RTX 5090 laptop it was
  made on, about 74 to 40 frames a second); LiveView=Balanced, Fast or Off gives some back.
- The far side takes a few seconds to load the first time; a portal shows its last picture
  of that place, wavy, until then.
- Things far from the portal's far end show in less detail, and some small effects can be
  missing there.
- On a world you host, if the game still has too few task queues free for the live view,
  portals there show their pictures instead.
- Creatures by an open portal's far end stay while the portal is open, even with nobody there.
- The world map is a painting: the portals' places on it are approximate.
- The loading percentage is an estimate from the trip's stages, not the game's own count.
- Only the keyboard's Interact key is kept from other objects at the prompt, not a
  gamepad's button.
- Large pictures take a moment to reach other players.

TROUBLESHOOTING / BUG REPORTS
Town Portals writes to greeneye\loader.log in the game folder (lines starting with
"Town Portal"), and on a dedicated server to greeneye\loader.log in the server's folder.
loader.log starts fresh each time the game or server starts and rolls over at 10 MB
(the earlier part moves to loader.old.log), so a long session keeps its last 10-20 MB.
Please attach it to a bug report, with a screenshot if it is visual.

THANKS
Thanks to Lomack for being our test guinea pig. We hope Lomack makes a speedy recovery
from the teleport accident!

Cast animation: Quaternius, Universal Animation Library (CC0). Portal sounds made from
"Whoosh Sci-Fi Portal" by virtualzero (Pixabay). Voice lines, banner and map art are AI-generated.

Source: https://github.com/Greeneye0/town-portals-enshrouded (MIT)
