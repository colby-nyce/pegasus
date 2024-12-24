#pragma once

#include "core/observers/Observer.hpp"

namespace atlas
{
    class AtlasState;

    class DbgStudioLogger : public Observer
    {
      public:
        using base_type = DbgStudioLogger;

        DbgStudioLogger(AtlasState* state);

        void enable(std::shared_ptr<std::ofstream> dbg_studio_json_fout);

        void dumpAllRegisters(const char* key);

        template <typename T>
        void dumpMetadata(const char* key, const T& value);

      private:
        ActionGroup* preExecute_(AtlasState* state);
        ActionGroup* postExecute_(AtlasState* state);

        AtlasState* state_ = nullptr;
        std::shared_ptr<std::ofstream> dbg_studio_json_fout_;
    };
} // namespace atlas
