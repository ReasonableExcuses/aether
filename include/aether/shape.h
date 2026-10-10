#ifndef AETHER_SHAPE_H
#define AETHER_SHAPE_H

#include "aether/common.h"
#include "aether/value.h"
#include <unordered_map>
#include <string>
#include <string_view>

namespace aether {

class Shape;

enum class PropertyAttributes : uint8_t {
    None         = 0,
    Writable     = 1 << 0,
    Enumerable   = 1 << 1,
    Configurable = 1 << 2,
    Default      = Writable | Enumerable | Configurable
};

inline constexpr PropertyAttributes operator|(PropertyAttributes a, PropertyAttributes b) {
    return static_cast<PropertyAttributes>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline constexpr bool operator&(PropertyAttributes a, PropertyAttributes b) {
    return (static_cast<uint8_t>(a) & static_cast<uint8_t>(b)) != 0;
}

/**
 * @brief Property descriptor defining layout position and attributes within JSObject.
 */
struct PropertyDescriptor {
    uint32_t slot_index;
    PropertyAttributes attributes;
    bool is_in_object;
};

/**
 * @brief Shape (Hidden Class / Map) representing immutable object topology.
 */
class Shape {
public:
    Shape(uint32_t id, Shape* parent, std::string_view transition_key, TaggedValue prototype)
        : id_(id),
          parent_(parent),
          transition_key_(transition_key),
          prototype_(prototype),
          num_properties_(parent ? parent->num_properties_ + 1 : 0) {
        if (parent) {
            // Inherit property map from parent
            property_map_ = parent->property_map_;
            // Allocate next slot index
            PropertyDescriptor desc{
                .slot_index = num_properties_ - 1,
                .attributes = PropertyAttributes::Default,
                .is_in_object = (num_properties_ - 1) < kDefaultInObjectProperties
            };
            property_map_[std::string(transition_key)] = desc;
        }
    }

    uint32_t Id() const noexcept { return id_; }
    Shape* Parent() const noexcept { return parent_; }
    uint32_t PropertyCount() const noexcept { return num_properties_; }
    TaggedValue Prototype() const noexcept { return prototype_; }
    const std::string& TransitionKey() const noexcept { return transition_key_; }

    const PropertyDescriptor* LookupProperty(std::string_view key) const {
        auto it = property_map_.find(std::string(key));
        if (it != property_map_.end()) {
            return &it->second;
        }
        return nullptr;
    }

    Shape* FindOrCreateTransition(std::string_view key, uint32_t next_id, TaggedValue proto) {
        std::string k(key);
        auto it = transitions_.find(k);
        if (it != transitions_.end()) {
            return it->second.get();
        }
        auto child = std::make_unique<Shape>(next_id, this, key, proto);
        Shape* raw_ptr = child.get();
        transitions_[k] = std::move(child);
        return raw_ptr;
    }

    static constexpr uint32_t kDefaultInObjectProperties = 4;

private:
    uint32_t id_;
    Shape* parent_{nullptr};
    std::string transition_key_{};
    TaggedValue prototype_{TaggedValue::Null()};
    uint32_t num_properties_{0};

    // Immutable property lookup map: Key -> Descriptor
    std::unordered_map<std::string, PropertyDescriptor> property_map_;

    // Outgoing transition tree: PropertyName -> NextShape
    std::unordered_map<std::string, std::unique_ptr<Shape>> transitions_;
};

} // namespace aether

#endif // AETHER_SHAPE_H
