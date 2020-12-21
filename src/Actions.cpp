#include "Actions.hpp"
#include <string.h>

constexpr const char* kCommandNames[] = {
    "<unknown>",
    "RunShellCommand",
    "WriteTextFile"
};
constexpr size_t kNumCommandNames = sizeof(kCommandNames) / sizeof(kCommandNames[0]);

namespace ActionType {

    Enum FromString(const char* name)
    {
        for (size_t i = 0; i < kNumCommandNames; ++i)
        {
            if (strcmp(name, kCommandNames[i]) == 0)
            {
                return static_cast<Enum>(i);
            }
        }

        return ActionType::kUnknown;
    }

    const char* ToString(Enum value)
    {
        return kCommandNames[static_cast<size_t>(value)];
    }
}
