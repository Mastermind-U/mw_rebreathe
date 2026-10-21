#pragma once

namespace MorrowindModernSettings {

struct LaunchSettings {
    // Zero dimensions/scale select desktop and an automatic readable scale.
    int width = 0;
    int height = 0;
    int screenMode = 2; // 0 fullscreen, 1 window, 2 borderless
    double uiScale = 0.0;
    bool experimental = false; // Reserved for migration; stable build never enables it.
    bool trace = false;
    bool lockRam = true;
};

// Shows a native modal settings dialog. Loads settings.ini beside the project and
// writes it only after "Play". On cancel returns false and leaves input intact.
bool ConfigureLaunch(LaunchSettings& settings);

} // namespace MorrowindModernSettings
