# Controls (Meta Quest Touch Plus)

The game still sees a Wii Remote with a Nunchuk attached. The Touch Plus
controllers are mapped onto them like this. The mapping lives in
`updateInput()` in `platform/src/xr/xr_app.cpp`.

| Touch Plus | Wii | In Super Mario Galaxy |
|---|---|---|
| Left thumbstick | Nunchuk stick | Move |
| **A** | A | Jump, talk, confirm. Hold to skip a cutscene or a dialogue |
| **B** | Shake the Remote | Spin |
| **Y** | Shake the Remote | Spin |
| Flick the right controller | Shake the Remote | Spin |
| Flick the left controller | Shake the Nunchuk | Spin, keeping the pointer steady |
| Right trigger | B | Shoot star bits, back/cancel in menus |
| Right controller aim | Pointer | Collect star bits, grab Pull Stars, point at menus |
| Left trigger | Z | Crouch, ground pound, long / backflip jumps (with A) |
| Left grip | C | Put the camera behind Mario |
| Menu (left) | + | Pause menu (one press) |
| X | − | Pause menu (one press) |
| Right stick left / right | D-pad left / right | In the diorama: turn it smoothly round Mario, faster the further you push (a dead zone of 15%, then up to 120° a second, with the edges of the view darkened while it turns); with *Smooth turning* off, a step (45°) at a time behind a short blink. On the giant screen: turn the game camera around Mario in steps, where the level allows it. Pushing the stick right turns the view to the right; with *Invert camera* off (see the VR settings) it moves the camera to the right instead, as the Wii's D-pad did |
| Right stick up / down | D-pad up / down, with *Smooth turning* off and in a menu's lists | With *Smooth turning* on, in the diorama: tilt the view round Mario, up to see him from higher (a dead zone of 15%, then up to 60° a second; the same curve as the turn; the tilt is kept within -60° and +60°, never past straight down) |
| Right stick click | None | In the diorama: level the tilt (back to the usual height over half a second); with *Planet turns* Steps or Off, also level the world to Mario's up behind a blink |

With *Smooth turning* off, the right stick up is the D-pad up (first-person
look) and the stick down is the D-pad down, as before. On the Steam Frame
controllers the left D-pad's up is the Wii D-pad up too (first-person look)
in either case. The stick click used to be first person as well; it is the
tilt reset now (it does nothing outside the diorama or on a menu), so on the
Touch Plus controllers, which have no D-pad, first person needs *Smooth
turning* off.

**Steam Frame: diorama height and size.** In the diorama, with *Smooth
turning* on or off, hold the left grip and use the left D-pad: up and down
raise and lower the diorama (*Height*, 0.4 m a second), right and left make
the world bigger and smaller (*World size*, up to 1.8 times a second, 0.5 to
3). The values are saved to the settings file a moment after you stop. While
the left grip is held, the D-pad reaches nothing else (no Wii D-pad, no
first person, no turn), and the grip is not C there (the left bumper still
is); outside the diorama the left grip is C as before. The Touch Plus
controllers have no D-pad, so they have none of this.

**Left-handed** (Controls tab of the VR settings, or `left_handed = 1`): the
left controller is the Remote (laser, pointer, settings panel clicks, the
tick when it touches a target, shoot trigger, spin bumper, tilt for the Star
Ball and the Ray, flick to spin, stick click to level the tilt, and the
stick that turns and tilts the diorama), and the right controller is the
Nunchuk (its stick moves Mario, its trigger crouches, its grip or bumper is
C). The face buttons, Menu, View and the D-pad stay where they are. The
Frame's D-pad is on the left controller only, so the left grip stays the
modifier for the diorama's height and size, and it is free for that because
C is on the right grip then.

Rumble is played on both controllers' haptics.

## Tilt

Rolling the Star Ball and surfing on the Ray steer with the Wii Remote's
tilt. There, the right controller's orientation takes its place:

- **Star Ball:** hold the right controller level (it stands for the Wii
  Remote held straight up, as the sign asks). Tip it down to roll forward,
  raise it to roll back, and roll it left or right to steer.
- **Ray:** hold the right controller level, pointing ahead (the penguin's
  "point your Wii Remote at the screen"), and twist it left or right to
  turn.

Elsewhere, the game sees a Wii Remote held level and still, so aiming the
laser never counts as a shake.

## Pointing

- **In gameplay (diorama):** a laser and reticle show where the right
  controller aims; the game's own cursor is hidden. Star bits, Pull Stars
  and enemies respond to the laser itself, near or far. The reticle swells
  and the controller ticks when the laser touches one. The laser runs on to
  the first surface in its way, or far past Mario into open space. Star
  bits you shoot leave from the controller and fly along the laser to its
  end.
- **On the giant screen:** the game's own star cursor shows where the
  controller points, as with a Wii Remote and a TV; no laser is drawn to
  it. The laser only shows while you point at the VR settings panel.
- **In menus:** the pause menu and yes/no prompts appear on the HUD panel in
  front of you, and the title and file select appear on the virtual screen.
  Point at them directly, the same way you point a Wii Remote at a TV.

## View

- The screen, the diorama and the HUD panel are placed straight ahead of
  where you are and face when the game starts, and again when you put the
  headset back on. To recentre at any time, hold the Meta button on the
  right controller.
- By default the world is at 1/500 scale, with Mario about 1.5 m in front
  of you and 0.8 m below eye level.
- When scenery hides Mario from you, the part between you and him fades
  away until he is in view again.
- Choosing a galaxy in the observatory domes, and the first-person view,
  happen on the virtual screen.

## Skipping cutscenes and dialogues

Hold **A** during a cutscene or a dialogue to skip it. After a moment a ring appears at
the lower right of your view and fills while you keep holding. When it is
full, the cutscene is skipped. If you let go early, the ring drains away and
the cutscene carries on. A short press still advances text as usual.

- Movies, the camera tour when you arrive in a galaxy, and Mario's flight
  into it stop at once, the way the game skips them itself.
- Other cutscenes run to their end at high speed while the view is dark and
  the ring spins. Text boxes are advanced for you. If a cutscene asks a
  yes/no question, skipping stops there so you can answer it.
- A dialogue (talking to a Luma, a Toad and so on, most of which are no
  cutscene) runs to its end the same way, all its pages, not just the one
  on screen; it too stops at a yes/no question.

To skip again, let go of A first. The A you held never reaches the game as
a jump. Holding A from before a cutscene started doesn't count.
`skip_hold` in the comfort settings below sets how long to hold.

## Playing as Luigi

Every save file can be played with Mario or Luigi, a new one included:
you do not need to finish the game with Mario first. On a file's start
screen (the one with **Play This File**, which also follows creating a
file), point at the Mario/Luigi button under the file's details and press
A. The file's bar turns green for Luigi. Mario and Luigi keep separate
progress in the same file; the button switches between them. The file's
icon is your own choice either way (Luigi's is among the icons too).

## Pausing

A single press of X or the Menu button opens the pause menu (on the Wii, +
or - had to be held for a fifth of a second, and not while A or B was held
or during a spin). Where the game does not allow pausing for a moment
(while Mario is being hit, during a screen transition) the menu opens as
soon as it does, up to 1.5 s after the press; during cutscenes it does not
open, as on the Wii.

Opening the system menu or taking the headset off pauses the game. When you
come back during play, the game's pause menu is open.

## VR settings

While the pause menu is open (press the Menu button or X), a VR settings
panel appears to the right of its buttons, slightly turned towards you. Aim
the right controller at it and press A or pull the trigger. It holds every
setting of `petari_vr.ini` (see below) on six tabs: click a tab's name at
the top to open it. **−** and **+** change a value by a step (hold to
repeat), a switch turns with a click anywhere on its row, and **Defaults**
puts the settings of the open tab back to their defaults. A setting that
has no effect as things are (the 3D depth while the 3D is off) is dimmed.

The panel has six tabs, in two rows. Rows that do nothing as things are
(the 3D depth while *Stereoscopic 3D* is off, the speeds while *Smooth
turning* is off, *Screen distance* while the giant screen is off) are dimmed
but still work. On the Steam Frame the settings only the Quest has are
left out (*Passthrough*, *Smooth motion*, *Super resolution*, *Refresh rate*,
*High clocks*; the refresh rate is fixed at 120 Hz); their lines in
`petari_vr.ini` still work.

**Diorama**

- **Distance**: how far in front of you Mario stands, 0.6 m to 4 m,
  1.5 m by default (slider, or − and + for 10 cm).
- **Height**: how far below your eyes he stands, 0.8 m by default.
- **World size**: the size of the world, 1.0 by default (1.5 is half as big
  again).
- **Follow smoothing**: how gently the world follows Mario (the seconds it
  takes to close half the gap; more is gentler), 0.12 s by default.
- **See through scenery** (on by default): scenery that hides Mario from
  you fades away between you and him.
- **Turn with the camera** (off by default): the world turns with the game
  camera as it swings round Mario, as early versions did; off, the world
  keeps its facing and the right stick turns it (see *Smooth turning*).
  With this on the stick turns the game camera, as on the giant screen,
  and *Smooth turning* has no effect.

**Screen**

- **Giant screen** (on by default) plays the game on a big 16:9 virtual
  screen from the game's own camera, as on a TV; off, the game is a
  diorama in front of you.
- **Screen distance**: how far away the screen is, 2.5 m to 10 m in steps
  of 0.5 m, 4.5 m by default. Drag the slider or use − and +; the mark
  under the slider is the default. The screen is 5.33 m wide, so it spans
  61 degrees at 4.5 m and 30 degrees (a TV seen from the couch) at 10 m.
- **Stereoscopic 3D** (off by default) shows the giant screen's picture in
  3D: the game is drawn once for each eye, from its own camera moved a
  little to either side. The sky and distant scenery lie far behind the
  screen, Mario stands a little in front of it and the ground before him
  comes out towards you. The HUD, the text and the menus float in front of
  the scene; the pointer's star cursor lies on whatever it points at in the
  scene (and with the menus while you point at those). The switch does
  nothing while the giant screen is off (the diorama is 3D anyway).
- **3D depth** (1.75 by default, 0.25 to 3) is how deep that 3D is. Around
  0.5 the whole scene stays behind the screen, like a window, and the
  effect is slight: a screen 4.5 m away leaves little room for depth behind
  it. More brings Mario and the world out of the screen towards you, which
  is where the depth shows; too much is tiring to look at. The paused scene
  changes as you press, so you can pick by eye. Moving the screen nearer
  deepens it too.
- **3D far depth** (0.95 by default, 0.5 to 1) is how far behind the screen
  the farthest things are drawn: at 1 they look infinitely far away.
- **3D resolution** (0.8 by default, 0.5 to 1) is the size of each eye's
  picture relative to the single one. The GPU draws two pictures of the
  game each frame instead of one, so each is a little smaller; 1 is
  sharpest, but frames come late in busy scenes.

**Cutscenes** (dimmed with the giant screen: a cutscene is on the flat screen there anyway)

- **Diorama in flights** (on by default): launch stars, pipes and other
  transits that carry Mario, and the flight into a galaxy, stay in the
  diorama instead of going back to the flat screen. The world follows the
  game camera as the screen would show it: its up, the point it watches,
  its facing and how high it looks from, smoothed (the pivot follows within
  about 0.12 s, the facing and tilt with the *Turn smoothing* time), with the
  edges of the view darkened while it swings, and the scenery between you
  and Mario fading as usual. When it ends the pivot goes back to Mario and
  the up settles to his without a blink (*Planet turns* smooth; with Steps
  or Off, one step behind a blink if he landed on another planet), but the
  view's facing and tilt stay as the flight left them: the diorama carries
  on from there instead of returning to the old position (a tilt from below
  Mario rises to just above his horizon over a couple of seconds). Off, the
  transits and the flight in are shown on the flat screen as before. Dimmed
  while *Giant screen* is on. The Cutscenes tab does the same for cutscenes.

Every cutscene the game plays itself, except the videos, can stay in the
diorama (a switch on, the default) or go to the flat screen (off), by kind.
In the diorama the world follows the cutscene's own camera, its angles and
framing, as in a transit (see *Diorama in flights*), with the edges
darkened while it swings; when the scene ends the diorama carries on from
the view it ended with and the pivot goes back to Mario. The videos (the
opening, the ending's film), the file select, the observatory domes' galaxy
choice and the first-person view are always on the flat screen. Holding A
skips a cutscene in either.

- **Power Stars** (`cutscene_stars`): a Power Star or the Grand Star
  appearing, returning to the Observatory or coming back.
- **Bosses and enemies** (`cutscene_bosses`): introductions, hits and
  defeats of bosses and of Kamella's ambushes, Bowser's scenes, Dino
  Piranha, Gobblegut, the Tomb Spider and so on.
- **Comet Observatory** (`cutscene_observatory`): Rosalina's explanations,
  the Toad Brigade captain, Luigi's disappearance, the domes opening, the
  launch to a galaxy and the epilogue.
- **Storybook** (`cutscene_storybook`): Rosalina's picture book and her
  reading of it (its pages are pointed at on the HUD panel).
- **Lumas and friends** (`cutscene_friends`): meeting a Luma, chasing the
  rabbits, penguins and other characters' scenes and races.
- **Mario's changes** (`cutscene_mario`): transformations and the super form.
- **Other scenes** (`cutscene_other`): everything else the game plays,
  such as doors opening, cages breaking and things appearing in the level.
  *Other scenes* also covers the game camera leaving Mario to show something
  far off (a door a switch opened; `cutscene_camera`, no cutscene proper):
  off, that goes to the flat screen for as long as it lasts.
- **Hold A to skip**: how long to hold A to skip a cutscene or a dialogue,
  1 s by default; at 0 ("Off") nothing is skipped.

The kind comes from the cutscene's name in the game's own list, by words in
it (`GameScene.cpp`), so a scene the words miss counts as *Other scenes*.

**Controls**

- **Smooth turning** (on by default): the right stick turns the diorama
  smoothly round Mario, in proportion to how far you push it (nothing
  within the first 15%, about 30 degrees a second at half, 120 degrees a
  second at the stop), with no blink. The edges of the view darken while it
  turns (*Comfort vignette*). The stick's up and down tilt the view round Mario
  (see the controls table). The left controller's D-pad left and right turn
  it at a steady 60 degrees a second. Off, each push turns it 45 degrees
  behind a short blink, and up and down are the D-pad's. It does not change
  the giant screen.
- **Turn speed** (120° a second by default, 30 to 240) and **Tilt speed**
  (60°, 15 to 120): the top speed of the stick's turn and tilt with
  *Smooth turning*, at full deflection.
- **Invert camera** (on by default): pushing the right stick to the right
  turns the view to the right, in the diorama and on the giant screen (the
  camera itself goes round Mario to the left). Off, the stick moves the
  camera the way you push it, as the Wii's D-pad did. It only swaps the
  camera's turns: the D-pad's left and right still turn pages the usual
  way.
- **Left-handed** (off by default): see above.

**Comfort**

- **Planet turns** (Smooth by default): what the world does when Mario's
  up changes, as on a round planet. *Smooth*: it turns with him. *Steps*:
  it holds still while he runs round, and once his up is 35 degrees away it
  levels to it in one step behind a short blink (the same blink as a snap
  turn), also when gravity jumps by that much. *Off*: it never turns by
  itself; level it with a click of the right stick, and it levels itself
  (behind a blink) when Mario lands on another planet after a transit. In
  Steps and Off, Mario can be seen tilted away from upright until it
  levels.
- **Turn smoothing**: how gently the world turns with his gravity (Smooth),
  and the game camera's facing in a transit: 0.35 s by default (dimmed
  unless *Planet turns* is Smooth).
- **Comfort vignette** (on by default): the edges of the view darken a
  little at once whenever the view moves smoothly, and more the faster it
  moves: the right stick's turn and tilt, the left grip and D-pad's height
  and size, the world following Mario's gravity, and the game camera in a
  transit. Off, only turns faster than about 20 degrees a second darken the
  edges, up to full at about 85 degrees a second, as before.
- **Vignette at slow movement** and **Vignette at fast movement** (60 % and
  100 % by default): how strong the vignette is at the slowest smooth
  movement and at full speed (a turn of 1 radian, about 57 degrees, a
  second); in between it follows the speed. The vignette is a clear circle
  round the middle of your view with the edges darkening outside it, by the
  angle from where you look, so it does not depend on the headset's field of
  view: at 0 % the circle is 55 degrees across its radius, at 100 % it
  closes in to 34 degrees (Mario, 25 to 30 degrees below your line of sight,
  stays clear) and the edges go to black about 30 degrees further out; the
  percentage is how dark they get. They apply to the stick's turn and tilt,
  the height and size adjust, the world turning with Mario's gravity and the
  game camera in a transit or cutscene. With *Comfort vignette* off only the
  fast value is used, for turns faster than about 20 degrees a second; 0 on
  both turns every vignette off. The old `vignette` line of the file is read
  as the fast value.

**Display**

- **Resolution** (0.80 by default): the size the eyes are rendered at,
  1.00 being Meta's standard eye size; "Now" shows what it is. Higher stays
  sharper, but when the GPU cannot keep up frames come late and the view
  stutters. It is one value for the session: the lowest and the highest
  resolution of the ini file (`min_resolution` and `resolution`) are both
  set to it. On the giant screen the screen's picture is drawn at its full
  size (2048x1152) while the GPU keeps up, and smaller when it does not
  (the stereoscopic 3D takes GPU time), down to three quarters of this
  setting.
- **Sharpening** (off by default) sharpens the world with AMD's
  contrast adaptive sharpening as the app puts it into the eye images (and
  scales it up there when Super Resolution is off). **Sharpening strength** goes
  from 0 (least) to 1 (most), 0.5 by default.
- **Language**: the language of the game's texts, among those your disc
  has: English, French, German, Spanish and Italian on the European disc,
  English, French and Spanish on the American one. English by default. The
  game reads it when it starts, so a change shows the next time you start
  the app (the panel says so until then).

On the Quest the Screen tab also has **Passthrough** (your room around the
giant screen), and the Display tab **Smooth motion** (SpaceWarp),
**Super resolution**, **Refresh rate**, **High clocks** and the lowest and
highest resolution as two rows.

The paused scene behind the menu changes as you change a setting, so you
can judge it before you carry on. When the menu closes, the settings you
changed are written to `petari_vr.ini` (each as its own line; the rest of
the file stays as it is, and settings you never touched keep following the
defaults).

## Comfort settings

Every setting of the VR settings panel is a line of `petari_vr.ini`, next to
the game data in `/sdcard/Android/data/com.galaxy.quest/files/`. The panel
writes it for you; you can also create or edit it yourself, with any of
these lines. The file is read when the app starts.

```
# Size of the world (1 = default, 1.5 = half as big again)
diorama_scale = 1
# Where Mario stands: metres below your eyes and in front of you (the
# distance can also be set on the VR settings panel in the pause menu)
diorama_height = 0.8
diorama_distance = 1.5
# How quickly the world follows Mario and turns with his gravity (seconds
# to close half the gap; larger is gentler). On a small planet it turns a
# little quicker (at most 115 degrees a second) while the view
# would otherwise sink below Mario's horizon; a sudden change of gravity (a
# gravity switch, a room's wall becoming the floor) happens at once behind a
# short blink instead.
# (seconds to close half the gap; larger is gentler)
follow_smoothing = 0.12
turn_smoothing = 0.35
# Darkening at the edges while the view moves (0 = off, 1 = full)
# How dark the edges get (0 to 1) at the slowest smooth movement and at full
# speed (1 radian a second) with comfort_vignette on; with it off, only the
# fast value, for turns above about 20 degrees a second.  An old `vignette`
# line is read as vignette_fast.
vignette_slow = 0.6
vignette_fast = 1
# Fade out scenery that hides Mario (0 = off)
cutaway = 1
# Seconds to hold A to skip a cutscene (0 = never skip)
skip_hold = 1
# 1: pushing the right stick right turns the view to the right (the camera
# goes round Mario to the left); 0: the stick moves the camera the way it is
# pushed, as the Wii's D-pad did
invert_camera = 1
# The game's language, among those the disc has: english, french, german,
# spanish or italian on the European disc, english, french or spanish on
# the American one (the Japanese and Korean discs have their own only).
# Read when the game starts.
language = english
# Largest render resolution, relative to the headset's recommended eye
# size (1680x1760 on Quest 3). The resolution rises towards this while the
# GPU has time to spare and drops (down to min_resolution) as soon as
# frames are missed. Lower it to save battery.
resolution = 1.6
# The lowest render resolution (0.5 to 1.25; also on the VR settings panel).
# The giant screen's picture goes down to three quarters of it (0.6 of its
# full 2048x1152) while the GPU cannot keep up.
min_resolution = 0.8
# Display refresh rate in Hz. At 120 the game's 60 frames a second fit
# the display exactly; other rates make moving things judder.
refresh_rate = 120
# Application SpaceWarp (at 120 Hz): the headset synthesizes the refresh
# between two game frames from motion vectors the app gives it, so the
# world moving past (as the view follows Mario) looks sharp instead of
# doubled. 0 shows each game frame twice instead.
space_warp = 1
# Meta Quest Super Resolution: the headset scales the world from its render
# size to the display with an edge-aware filter and sharpening. 0 scales it
# in the app (bilinear).
super_resolution = 1
# AMD FidelityFX CAS (contrast adaptive sharpening) on the world, and its
# strength from 0 (least) to 1 (most).
sharpening = 0
sharpening_strength = 0.5
# Play on a giant 16:9 screen from the game's own camera (1, the default)
# or in the diorama (0), and how far away that screen is, in metres (it is
# 5.33 m wide: 61 degrees across at 4.5 m, 30 degrees at 10 m).
giant_screen = 1
screen_distance = 4.5
# 1 shows your room around the giant screen (the headset's passthrough)
# instead of the dark; 0 is the default
passthrough = 0
# The giant screen in stereoscopic 3D (1; 0, the default, shows both eyes
# the same picture). stereo_depth is how deep it looks (0.25 to 3, "3D
# depth" on the VR settings panel): how far in front of the farthest things
# Mario is drawn, in distances between your eyes; about 0.5 keeps everything
# behind the screen, more brings the world out of it. stereo_far is how far
# apart the farthest things are drawn, in distances between your eyes (0.5
# to 1): at 1 they look infinitely far away, and it cannot be more.
# stereo_resolution is the size of each of the two pictures relative to the
# single one (0.5 to 1): 1 is sharpest but takes the GPU a quarter longer
# than 0.8, and frames come late in busy scenes.
stereo_screen = 0
stereo_depth = 1.75
stereo_far = 0.95
stereo_resolution = 0.8
# The folder of the game's files, as chosen on the setup screen (the app's
# own files/game when there is no such line).
# game_path = /storage/emulated/0/Download/cooked
# 1 turns the diorama with the game camera as it swings round Mario (as
# early versions did); 0 keeps the world's facing, and the right stick
# turns it (see smooth_turn).
turn_with_camera = 0
# smooth: the world turns with Mario's up round a planet; steps: it holds
# still and levels in one step behind a blink once his up is 35 degrees
# away; off: it never turns by itself (the right stick's click levels it,
# and a transit that lands on another planet does). Also 0, 1, 2.
gravity_turns = smooth
# 1: launch stars, pipes and the flight into a galaxy stay in the diorama,
# following the game camera; 0: they are shown on the flat screen
diorama_transitions = 1
# 1: the edges darken a little at once whenever the view moves smoothly
# (more the faster); 0: only for turns faster than about 20 degrees a second
comfort_vignette = 1
# Which cutscenes stay in the diorama (1; 0 shows them on the flat screen):
# Power Stars appearing and returning, bosses and enemies, the Comet
# Observatory story, Rosalina's storybook, Lumas and other characters,
# Mario's changes, other scenes in the level, the camera showing something
# far from Mario.  The videos are always flat.
cutscene_stars = 1
cutscene_bosses = 1
cutscene_observatory = 1
cutscene_storybook = 1
cutscene_friends = 1
cutscene_mario = 1
cutscene_other = 1
cutscene_camera = 1
# 1: the right stick turns the diorama smoothly, in proportion to its
# deflection (up to 120 degrees a second, no blink); 0: in 45 degree steps
# behind a short blink.
smooth_turn = 1
# Top speed of the stick's turn and tilt with smooth_turn, degrees a second
turn_speed = 120
tilt_speed = 60
# 1: the left controller points and the hands' sticks, triggers, grips and
# bumpers swap; 0: the right one points
left_handed = 0
# Ask the headset for high CPU and GPU clock levels (Meta's SustainedHigh,
# the CPU at 1.92 GHz on Quest 3). 0 leaves the clocks to the system, which
# runs the CPU slower to save battery, and more frames come late.
high_clocks = 1
```

To copy it over: `adb push petari_vr.ini /sdcard/Android/data/com.galaxy.quest/files/`.

## Checking performance

After a session, `tools/app_log.sh` shows the app's log. Every 10 s of
gameplay it contains a line like

```
vr: render scale 1.10-1.25 (now 1.20, 3 changes); eye GPU 5.2 ms avg 6.4 max of 8.3; 2 refreshes missed
```

with the range of render resolutions used, and a line
`vr: the frame loop missed N refreshes (M with its own work late)` whenever
refreshes were missed. Only the misses the frame loop was not late for (the
GPU work overran the refresh) lower the resolution: when the loop's own CPU
work runs late, a lower resolution would not help.

With SpaceWarp on, a line `vr: SpaceWarp: N frames with motion vectors in
the last 10 s, M not to extrapolate` follows every 10 s (about 600 frames,
the game's 60 a second; the frames not to extrapolate are cuts, such as the
fade between the diorama and the virtual screen), and
`adb logcat -s VrApi` shows `FPS=60/120` and `ASW=120, Type=App` (the frames a second shown, the synthesized ones included).
