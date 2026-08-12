// Positional audio for MX Bikes, via Mumble's Link plugin.
//
// Mumble does the voice; we only tell it where we are. The Link interface is a named
// shared-memory block that Mumble polls: we write our own avatar position and facing each
// frame, and Mumble's server exchanges those between clients and does the attenuation and
// panning itself.
//
// The consequence worth understanding: **we report only ourselves.** We never have to
// match a voice stream to a rider on track, which is the hard half of proximity chat and
// the part that needs an identity join we don't reliably have. Everyone else's position
// arrives at Mumble from their own copy of this code.
#pragma once

namespace mumblelink {

// Create (or attach to) the shared block. Safe to call more than once; safe to call when
// Mumble isn't installed, since we create the mapping rather than requiring it to exist.
bool Init();

// Push our current pose. Call once per frame; cheap enough to leave on.
//
// `yawDeg` is the SDK's yaw — degrees from north, the same value the radar consumes.
// Position is in metres, in the game's own axes, which are already Mumble's (X right,
// Y up, Z forward, left-handed) so nothing is converted.
void Update(float x, float y, float z, float yawDeg);

// Stop publishing: zeroes the position, which is how the Link interface says "no
// positional data" without tearing the block down. Called when we leave a session, so
// riders sitting in menus don't stay pinned to wherever they last were on track.
void Clear();

// The grouping key, in two halves. Mumble strips positional data between two users whose
// context differs, so this is what scopes proximity to one server and track — riders
// elsewhere are heard flat rather than from a position that means nothing to them.
//
// Split because the two arrive from different callbacks and change independently: the
// server is known once, when the event opens; the track changes under us on a rotation.
// Setting them together would mean a track change wiping the server name.
void SetServer(const char* serverName);
void SetTrack(const char* trackName);

// Who we are, for Mumble's own bookkeeping. Our GUID when EventInit has given us one.
void SetIdentity(const char* riderName);
bool HasIdentity();

// Whether a context has been established yet. Publishing before it is, is worse than
// publishing nothing: an empty context is a context, and every rider carrying it would be
// grouped together regardless of which server they are actually on.
bool HasContext();

// Forget the server and track. Called when the event closes, so a stale context can't
// group us with whoever is in the next lobby.
void ClearContext();

// Runtime toggle, persisted alongside the radar's settings.
void SetEnabled(bool on);
bool Enabled();

void Shutdown();

} // namespace mumblelink
