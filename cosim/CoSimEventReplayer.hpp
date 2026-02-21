#pragma once

#include "include/PegasusTypes.hpp"
#include "sparta/utils/ValidValue.hpp"
#include <map>
#include <vector>

namespace sparta
{
    class ArchData;
    class Scheduler;

    namespace app
    {
        class SimulationConfiguration;
    }
} // namespace sparta

namespace pegasus
{
    class PegasusSim;
    class PegasusState;
} // namespace pegasus

namespace simdb
{
    class DatabaseManager;
} // namespace simdb

namespace pegasus::cosim
{

    class CoSimEventPipeline;
    class Event;

    class CoSimEventReplayer
    {
      public:
        CoSimEventReplayer(const std::string & db_file, const std::string & arch);

        const PegasusSim & getPegasusSim() const;

        PegasusSim & getPegasusSim();

        bool step(CoreId core_id, HartId hart_id);

        const Event* getLastEvent(CoreId core_id, HartId hart_id) const;

      private:
        void cacheArchDatas_(CoreId core_id, HartId hart_id);

        template <typename XLEN>
        static void apply_(const Event & reload_evt, PegasusState* state, std::vector<sparta::ArchData*>&);

        std::shared_ptr<simdb::DatabaseManager> db_mgr_;
        std::shared_ptr<sparta::Scheduler> scheduler_;
        std::shared_ptr<pegasus::PegasusSim> pegasus_sim_;
        std::shared_ptr<sparta::app::SimulationConfiguration> sim_config_;
        std::map<CoreId, std::map<HartId, std::vector<sparta::ArchData*>>> adatas_;
        uint64_t next_euid_ = 1;
        uint64_t num_events_on_disk_ = 0;
        size_t reg_width_ = 0;
    };

} // namespace pegasus::cosim
