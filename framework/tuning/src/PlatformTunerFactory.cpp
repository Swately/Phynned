// framework/tuning/src/PlatformTunerFactory.cpp
// ITuningProvider::make() factory — selects the platform-appropriate concrete
// tuner at runtime. Compiled into both Linux and Windows builds; the body
// dispatches via #ifdef to the matching concrete class.
//
#include <phyriad/tuning/TuningProvider.hpp>
#include <phyriad/tuning/LinuxTuner.hpp>
#include <phyriad/tuning/WindowsTuner.hpp>
#include <memory>

namespace phyriad::tuning {

std::unique_ptr<ITuningProvider> ITuningProvider::make() noexcept {
#ifdef _WIN32
    return std::make_unique<WindowsTuner>();
#else
    return std::make_unique<LinuxTuner>();
#endif
}

} // namespace phyriad::tuning
// Made with my soul - Swately <3
