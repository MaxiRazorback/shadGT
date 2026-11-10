#pragma once

namespace Native {
// project_hooks: GT Sport's script-read hook went in, so in_race reflects the game's project.
// FIX-GT7-140: without it (GT7 and other titles) in_race never turns true and the menu wait
// would hold every draw for its new pipeline; shadgt7's rule applies instead: keep going (mode 2)
// never waits, the other modes wait outside races.
constexpr bool MenuCompileMustWait(unsigned mode, bool in_race, bool capture_hooks,
                                   bool capture_idle, bool project_hooks = true) {
    if (capture_hooks && !capture_idle) return true;
    if (!project_hooks) return mode != 2 && !in_race;
    return !in_race && (mode != 2 || !capture_hooks);
}
} // namespace Native
