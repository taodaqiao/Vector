#pragma once

#include <memory>
#include <string>

#include "core/name_table.h"

/**
 * @file config_bridge.h
 * @brief A native-side cache for configuration data, currently only the obfuscation map.
 */

namespace sys::rt {

/**
 * @class SettingsSource
 * @brief A singleton that holds configuration data.
 */
class SettingsSource {
public:
    virtual ~SettingsSource() = default;

    SettingsSource(const SettingsSource &) = delete;
    SettingsSource &operator=(const SettingsSource &) = delete;

    /**
     * @brief Gets the singleton instance of the SettingsSource.
     */
    static SettingsSource *GetInstance() { return instance_.get(); }

    /**
     * @brief Releases ownership of the singleton instance.
     */
    static std::unique_ptr<SettingsSource> ReleaseInstance() { return std::move(instance_); }

    /// Gets a reference to the obfuscation map.
    virtual NameTable &obfuscation_map() = 0;

    /// Sets the obfuscation map.
    virtual void obfuscation_map(NameTable map) = 0;

protected:
    SettingsSource() = default;

    /// The singleton instance, managed alongside the main Context.
    static std::unique_ptr<SettingsSource> instance_;
};

}  // namespace sys::rt
