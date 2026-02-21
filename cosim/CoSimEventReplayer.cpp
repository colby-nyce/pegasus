#include "cosim/CoSimEventReplayer.hpp"
#include "cosim/CoSimEventPipeline.hpp"
#include "cosim/Event.hpp"
#include "sim/PegasusSim.hpp"
#include "core/PegasusCore.hpp"
#include "core/PegasusState.hpp"
#include "sparta/app/SimulationConfiguration.hpp"
#include "simdb/sqlite/DatabaseManager.hpp"
#include "source/include/softfloat.h"

namespace pegasus::cosim
{

    CoSimEventReplayer::CoSimEventReplayer(const std::string & db_file,
                                           const std::string & arch)
    {
        if (arch == "rv32")
        {
            reg_width_ = 8;
        }
        else if (arch == "rv64")
        {
            reg_width_ = 16;
        }
        else
        {
            throw sparta::SpartaException("Invalid arch; must be rv32 or rv64, not ") << arch;
        }

        db_mgr_.reset(new simdb::DatabaseManager(db_file, false /*not a new file*/));
        scheduler_.reset(new sparta::Scheduler);
        pegasus_sim_.reset(new PegasusSim(scheduler_.get()));

        // Figure out the total number of events on disk
        auto num_evts_query = db_mgr_->createQuery("CompressedEvents");
        num_evts_query->select("EndEuid", num_events_on_disk_);
        num_evts_query->orderBy("EndEuid", simdb::QueryOrder::DESC);
        num_evts_query->getResultSet().getNextRecord();

        if (num_events_on_disk_ == 0)
        {
            throw sparta::SpartaException("Cannot run cosim event replayer - no events on disk!");
        }

        // Recreate the final ParameterTree config
        sim_config_.reset(new sparta::app::SimulationConfiguration);
        auto ptree_query = db_mgr_->createQuery("ParameterTree");

        std::string ptree_path, ptree_value;
        ptree_query->select("PTreePath", ptree_path);
        ptree_query->select("ValueString", ptree_value);

        auto ptree_results = ptree_query->getResultSet();
        while (ptree_results.getNextRecord())
        {
            sim_config_->processParameter(ptree_path, ptree_value);
        }

        sim_config_->copyTreeNodeExtensionsFromArchAndConfigPTrees();

        // Configure PegasusSim
        pegasus_sim_->configure(0, nullptr, sim_config_.get());
        pegasus_sim_->buildTree();
        pegasus_sim_->configureTree();
        pegasus_sim_->finalizeTree();
        pegasus_sim_->finalizeFramework();
    }

    const PegasusSim & CoSimEventReplayer::getPegasusSim() const { return *pegasus_sim_; }

    PegasusSim & CoSimEventReplayer::getPegasusSim() { return *pegasus_sim_; }

    bool CoSimEventReplayer::step(CoreId core_id, HartId hart_id)
    {
        if (next_euid_ == num_events_on_disk_)
        {
            return false;
        }

        auto state = pegasus_sim_->getPegasusCore(core_id)->getPegasusState(hart_id);
        (void)state;

        auto event = CoSimEventPipeline::recreateEventFromDisk_(next_euid_++, db_mgr_.get(),
                                                                core_id, hart_id);

        cacheArchDatas_(core_id, hart_id);
        auto& adatas = adatas_[core_id][hart_id];
        sparta_assert(!adatas.empty());

        if (reg_width_ == 8)
        {
            apply_<uint32_t>(*event, state, adatas);
        }
        else
        {
            apply_<uint64_t>(*event, state, adatas);
        }

        return true;
    }

    const Event* CoSimEventReplayer::getLastEvent(CoreId core_id, HartId hart_id) const
    {
        // TODO cnyce
        (void)core_id;
        (void)hart_id;
        return nullptr;
    }

    void CoSimEventReplayer::cacheArchDatas_(CoreId core_id, HartId hart_id)
    {
        auto& adatas = adatas_[core_id][hart_id];
        if (!adatas.empty())
        {
            return;
        }

        std::map<sparta::ArchData*, sparta::TreeNode*> adatas_helper;
        std::function<void(sparta::TreeNode* n)> recurseFindArchDatas;
        recurseFindArchDatas = [&recurseFindArchDatas, &adatas, &adatas_helper](sparta::TreeNode* n)
        {
            assert(n);
            auto assoc_adatas = n->getAssociatedArchDatas();
            for (sparta::ArchData* ad : assoc_adatas) {
                if (ad != nullptr) {
                    auto itr = adatas_helper.find(ad);
                    if (itr != adatas_helper.end()) {
                        throw sparta::SpartaException("Found a second reference to ArchData ")
                            << ad << " in the cosim event replayer . First reference found through "
                            << itr->second->getLocation() << " and second found through " << n->getLocation()
                            << " . An ArchData should be findable throug exactly 1 TreeNode";
                    }
                    adatas.push_back(ad);
                    adatas_helper[ad] = n;
                }
            }
            for (sparta::TreeNode* child : sparta::TreeNodePrivateAttorney::getAllChildren(n)) {
                recurseFindArchDatas(child);
            }
        };

        auto state = pegasus_sim_->getPegasusCore(core_id)->getPegasusState(hart_id);
        auto system = pegasus_sim_->getPegasusCore(core_id)->getSystem();

        recurseFindArchDatas(state->getContainer());
        recurseFindArchDatas(system->getContainer());

        if (adatas.empty())
        {
            throw sparta::SpartaException("No ArchDatas exist!");
        }
    }

    template <typename XLEN>
    void CoSimEventReplayer::apply_(const Event & reload_evt, PegasusState* state, std::vector<sparta::ArchData*> & adatas)
    {
        static_assert(std::is_same_v<XLEN, uint32_t> || std::is_same_v<XLEN, uint64_t>);
        sparta_assert(!adatas.empty());

        [[maybe_unused]] auto core_id = reload_evt.getCoreId();
        [[maybe_unused]] auto hart_id = reload_evt.getHartId();

        // pc
        state->setPc(reload_evt.getNextPc());

        // priv mode
        state->setPrivMode(reload_evt.getNextPrivilegeMode(), state->getVirtualMode());

        // reservation
        if (reload_evt.getEndReservation().isValid())
        {
            state->getCore()->getReservation(hart_id) = reload_evt.getEndReservation();
        }
        else
        {
            state->getCore()->getReservation(hart_id).clearValid();
        }

        // softfloat
        softfloat_roundingMode = reload_evt.end_softfloat_flags_.softfloat_roundingMode;
        softfloat_detectTininess = reload_evt.end_softfloat_flags_.softfloat_detectTininess;
        softfloat_exceptionFlags = reload_evt.end_softfloat_flags_.softfloat_exceptionFlags;
        extF80_roundingPrecision = reload_evt.end_softfloat_flags_.extF80_roundingPrecision;

        // sim state
        auto sim_state = state->getSimState();
        sim_state->reset();
        sim_state->current_opcode = reload_evt.getOpcode();
        sim_state->current_uid = reload_evt.getSimStateCurrentUID();
        sim_state->sim_stopped = reload_evt.isLastEvent();
        sim_state->inst_count = reload_evt.getSimStateCurrentUID();
        sim_state->test_passed = sim_state->workload_exit_code == 0;
        if (!sim_state->sim_stopped)
        {
            sim_state->workload_exit_code = 0;
        }

        // mmu mode / translation mode
        bool change_mmu_mode = false;
        change_mmu_mode |= reload_evt.curr_priv_ != reload_evt.next_priv_;
        change_mmu_mode |= reload_evt.curr_ldst_priv_ != reload_evt.next_ldst_priv_;
        if (!change_mmu_mode && reload_evt.inst_csr_ != std::numeric_limits<uint32_t>::max())
        {
            change_mmu_mode |= reload_evt.inst_csr_ == MSTATUS || reload_evt.inst_csr_ == SSTATUS
                               || reload_evt.inst_csr_ == VSSTATUS
                               || reload_evt.inst_csr_ == HSTATUS;
            change_mmu_mode |= reload_evt.inst_csr_ == SATP || reload_evt.inst_csr_ == VSATP
                               || reload_evt.inst_csr_ == HGATP;
        }

        if (change_mmu_mode)
        {
            state->updateTranslationMode<XLEN>(
                translate_types::TranslationStage::SUPERVISOR);
            state->updateTranslationMode<XLEN>(
                translate_types::TranslationStage::VIRTUAL_SUPERVISOR);
            state->updateTranslationMode<XLEN>(
                translate_types::TranslationStage::GUEST);
        }

        // current exception
        state->setCurrentException(reload_evt.getExceptionCode());

        // enabled extensions
        std::vector<std::string> exts_to_enable;
        std::vector<std::string> exts_to_disable;
        const auto & ext_changes = reload_evt.extension_changes_;
        for (auto rit = ext_changes.rbegin(); rit != ext_changes.rend(); ++rit)
        {
            const auto & ext_info = *rit;
            const auto & ext_names = ext_info.extensions;
            const auto enabled = ext_info.enabled;

            if (enabled)
            {
                exts_to_disable.insert(exts_to_disable.end(), ext_names.begin(),
                                       ext_names.end());
            }
            else
            {
                exts_to_enable.insert(exts_to_enable.end(), ext_names.begin(), ext_names.end());
            }
        }

        exts_to_enable.erase(std::unique(exts_to_enable.begin(), exts_to_enable.end()),
                             exts_to_enable.end());
        exts_to_disable.erase(std::unique(exts_to_disable.begin(), exts_to_disable.end()),
                              exts_to_disable.end());

        if (!exts_to_enable.empty() || !exts_to_disable.empty())
        {
            auto & ext_mgr = state->getCore()->getExtensionManager();
            ext_mgr.changeExtensions(exts_to_enable, exts_to_disable);
            state->getCore()->changeMavisContext();
        }

        // memory / registers
        // TODO cnyce
    }

} // namespace pegasus::cosim
