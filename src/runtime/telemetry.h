// OGS-TS telemetry (Open Game Standards Telemetry Standard): what the player is doing, for motion rigs, haptics
// and dashboards, sent by UDP multicast (239.255.17.17, TTL 1, so it also leaves the PC for a rig on the LAN).
// Read from the game's RAM, no hooks, and serialised by the standard's own dependency-free C++ writer
// (third_party/ogs_telemetry).
//
// Off by default; save/settings.ini:
//   telemetry = 1
//   telemetry_port = 40123
//
// What is sent: the craft (Yavin's X-wing, Hoth's snowspeeder, Endor's speeder bike, the lightsaber duels), the
// shield and the score, and a body.impact event each time the shield is hit. In the flights the craft's position
// and attitude (the game's camera: what the player sees move) go too, with the forces worked out from them.
// Outside a game (attract, menus, mission select) the stream goes on with session.paused set, so a rig settles
// (CONVENTIONS C-11).
//
// The motion is only sent while the craft is flown, not in the cinematics (the opening one of each stage, and the
// scenes between the phases): the pose stays where it was and no forces are sent. The lightsaber duels send no
// motion either: their object has not been looked at.
#pragma once

#include <cstddef>
#include <cstdint>

// Game thread, every vblank (runtime.cpp, after stats_frame)
void telemetry_frame(bool on, int port);
