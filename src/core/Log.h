#pragma once

#include <iostream>
#include <utility>

// Minimal, dependency-free logging. Not intended to be fast or structured -
// just enough to see what the renderer is doing at startup and when
// something goes wrong.
namespace RoyalGL::Log
{
    enum class Level
    {
        Info,
        Warn,
        Error
    };

    template <typename... Args>
    void Print(Level level, Args&&... args)
    {
        std::ostream& os = (level == Level::Error) ? std::cerr : std::cout;
        switch (level)
        {
            case Level::Info:  os << "[RoyalGL] "; break;
            case Level::Warn:  os << "[RoyalGL][WARN] "; break;
            case Level::Error: os << "[RoyalGL][ERROR] "; break;
        }
        (os << ... << std::forward<Args>(args)) << std::endl;
    }
}

#define ROYALGL_LOG_INFO(...)  ::RoyalGL::Log::Print(::RoyalGL::Log::Level::Info, __VA_ARGS__)
#define ROYALGL_LOG_WARN(...)  ::RoyalGL::Log::Print(::RoyalGL::Log::Level::Warn, __VA_ARGS__)
#define ROYALGL_LOG_ERROR(...) ::RoyalGL::Log::Print(::RoyalGL::Log::Level::Error, __VA_ARGS__)
