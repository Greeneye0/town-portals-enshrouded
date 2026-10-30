Town Portals 1.0.2 for Enshrouded
Saving your ass since 2026.

Open a portal home from anywhere. Press T: after a short cast, a portal opens in front
of you and a second one beside your Flame Altar (the one marked as your preferred respawn).
Step through, travel home, and come back through the same portal to where you were.

HOW IT WORKS
- Press T (OpenPortalKey in town-portal.ini) to cast. The cast takes 3 seconds and you
  cannot move during it; press Jump to cancel. The portal forms in front of you, and its
  twin forms beside your preferred Flame Altar.
- Walk up to either end and press E to travel. While the portal's prompt shows, E is the
  portal's only: a grapple point, workbench or altar next to it will not react too.
- Each player has one portal. Opening a new one replaces your old one.
- Other players can use your portal (the prompt says it is shared). Everyone in the world
  needs Town Portals; on a dedicated server, the server needs it too (server/).
- The portal shows a picture of where it leads, with depth as you move around it. It is a
  snapshot, not a live view: it is taken when you travel through, and refreshed every trip.
  Before your first trip to a place, the altar's portal shows a landmark of that region,
  by day or by night. Your altar's picture is saved, so it shows again at your next login.
  In multiplayer, players share their pictures, so others see your portal's view too.
- While you travel, a painted world map shows the trip: your portal, your altar, and a
  path that grows as the destination loads, with a loading percentage. The map is in the
  game's language (English shows it as painted).
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

Check: greeneye\loader.log has a line with "Town Portals 1.0.2" (the server's says
"server companion loaded").
Uninstall: delete greeneye\town-portal (and dbgcore.dll if no other Greeneye mod uses
it), in the game folder and on the server.

SETTINGS: greeneye\town-portal\town-portal.ini (written on first start)
[Portals] (the host's file: the dedicated server's, or the hosting player's)
  AllowInShroud=true, AllowInHollowHalls=true, DisableAutoClose=false
[Controls] (each player's own)
  OpenPortalKey=T        a letter, F1-F24, Mouse4, Mouse5, ... or a key code like 0x54
  VoiceFeedback=true
  PortalQuality=Auto     Auto, Low or High
Picture sharing: delete greeneye\town-portal\share-snapshots.enabled to turn it off.

KNOWN LIMITATIONS
- The picture in a portal is a snapshot, not a live image of the other side.
- A portal's picture is only accurate after a trip through it: until then the altar's
  portal shows a landmark of the region, and the field's portal your altar as last seen.
- Slight visual anomalies in the portal's picture: blocky, stair-stepped edges when seen
  from aside or close up, and stretched spots behind objects the snapshot could not see.
- The world map is a painting: the portals' places on it are approximate.
- The loading percentage is an estimate from the trip's stages, not the game's own count.
- Only the keyboard's E is kept from other objects at the prompt, not a gamepad's button.
- Large pictures take a moment to reach other players.

TROUBLESHOOTING / BUG REPORTS
Town Portals writes to greeneye\loader.log in the game folder (lines starting with
"Town Portal"), and on a dedicated server to greeneye\loader.log in the server's folder.
Please attach it to a bug report, with a screenshot if it is visual.

THANKS
Thanks to Lomack for being our test guinea pig. We hope Lomack makes a speedy recovery
from the teleport accident!

Cast animation: Quaternius, Universal Animation Library (CC0). Portal sounds made from
"Whoosh Sci-Fi Portal" by virtualzero (Pixabay). Voice lines, banner and map art are AI-generated.

Source: https://github.com/Greeneye0/town-portals-enshrouded (MIT)
