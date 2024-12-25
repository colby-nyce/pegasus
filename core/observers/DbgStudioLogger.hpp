#pragma once

#include "core/observers/Observer.hpp"
#include <fstream>
#include <sstream>

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
        void dumpMetadata(const char* key, const T& value)
        {
            if (!enabled_) {
                return;
            }

            // {
            //   "metadata": {
            //     <key>: <value>
            //   }
            // }

            std::ostringstream json;
            json << "{";
            json << "\"metadata\": {";
            json << "\"" << key << "\": \"" << value << "\"";
            json << "}}";

            *dbg_studio_json_fout_ << json.str() << "\n";
        }

        void simulationEnding(const std::string& msg)
        {
            if (!enabled_) {
                return;
            }

            // {
            //     "SIM_END": <msg>
            // }

            std::ostringstream oss;
            oss << "{";
            oss << "\"SIM_END\": \"" << msg << "\"";
            oss << "}";

            auto json = oss.str();
            *dbg_studio_json_fout_ << json << "\n";

            dbg_studio_json_fout_.reset();
            enabled_ = false;
        }

      private:
        ActionGroup* preExecute_(AtlasState* state);
        ActionGroup* postExecute_(AtlasState* state);

        AtlasState* state_ = nullptr;
        std::shared_ptr<std::ofstream> dbg_studio_json_fout_;
    };
} // namespace atlas
