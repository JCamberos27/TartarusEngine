#pragma once

// Gives gameplay code (GameShell) the editor's shortcuts, sounds, F-row pill and look prefs. Called once by
// Application::Run before the session starts; the unit tests never call it, so GameShell stays a quiet no-op there.
void WireGameShell();
