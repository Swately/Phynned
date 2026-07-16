// apps/ayama/observer/src/ProcessObserver_win32.cpp
// Platform-specific Toolhelp32 enumeration removed.
// ProcessObserver::refresh() now uses phyriad::proc::enumerate_processes (FR-4),
// which handles both Windows and Linux internally with zero heap allocation.
// File kept (empty) to avoid touching CMakeLists.txt; remove the source
// listing later when convenient.
// Made with my soul - Swately <3
