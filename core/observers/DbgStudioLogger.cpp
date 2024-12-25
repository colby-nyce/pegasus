#include "core/observers/DbgStudioLogger.hpp"
#include "core/AtlasState.hpp"
#include "core/AtlasInst.hpp"
#include "include/AtlasUtils.hpp"
#include "system/AtlasSystem.hpp"
#include "sparta/utils/LogUtils.hpp"

namespace atlas
{
    DbgStudioLogger::DbgStudioLogger(AtlasState* state)
        : state_(state)
    {
        pre_execute_action_ =
            atlas::Action::createAction<&DbgStudioLogger::preExecute_>(this, "pre execute");

        post_execute_action_ =
            atlas::Action::createAction<&DbgStudioLogger::postExecute_>(this, "post execute");
    }

    void DbgStudioLogger::enable(std::shared_ptr<std::ofstream> dbg_studio_json_fout)
    {
        dbg_studio_json_fout_ = dbg_studio_json_fout;
        enabled_ = dbg_studio_json_fout_ != nullptr && dbg_studio_json_fout_->is_open();

        if (!enabled_) {
            std::cerr << "DbgStudioLogger: Failed to open JSON file for logging" << std::endl;
            dbg_studio_json_fout_.reset();
        }
    }

    void DbgStudioLogger::dumpAllRegisters(const char* key)
    {
        if (!enabled_) {
            return;
        }

        auto &fout = *dbg_studio_json_fout_;
        fout << "REGISTER_DUMP." << key << "::hart:" << state_->getHartId() << ",pc:0x"
             << std::hex << state_->getPc();

        auto int_regs = state_->getIntRegisters();
        auto csr_regs = state_->getCsrRegisters();
        auto reg_count = int_regs.size() + csr_regs.size();
        size_t idx = 0;

        auto to_lower = [](const std::string & s) {
            std::string lower = s;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            return lower;
        };

        for (auto reg : int_regs) {
            fout << "," << to_lower(reg->getName()) << ":0x" << std::hex << reg->dmiRead<uint64_t>();
            if (++idx < reg_count) {
                fout << ",";
            }
        }

        for (auto reg : csr_regs) {
            fout << "," << to_lower(reg->getName()) << ":0x" << std::hex << reg->dmiRead<uint64_t>();
            if (++idx < reg_count) {
                fout << ",";
            }
        }

        fout << "\n";
    }

    void DbgStudioLogger::simulationEnding(const std::string& msg)
    {
        if (!enabled_) {
            return;
        }

        auto &fout = *dbg_studio_json_fout_;
        fout << "SIM_END::hart:" << state_->getHartId() << ",msg:" << msg << "\n";

        dbg_studio_json_fout_.reset();
        enabled_ = false;
    }

    ActionGroup* DbgStudioLogger::preExecute_(AtlasState* state)
    {
        reset_();

        pc_ = state->getPc();
        AtlasInstPtr inst = state->getCurrentInst();
        opcode_ = inst->getOpcode();

        // Get value of source registers
        if (inst->hasRs1())
        {
            const auto rs1 = inst->getRs1();
            const std::vector<uint8_t> value = convertToByteVector(rs1->dmiRead<uint64_t>());
            src_regs_.emplace_back(getRegId(rs1), value);
        }

        if (inst->hasRs2())
        {
            const auto rs2 = inst->getRs2();
            const std::vector<uint8_t> value = convertToByteVector(rs2->dmiRead<uint64_t>());
            src_regs_.emplace_back(getRegId(rs2), value);
        }

        // Get initial value of destination registers
        if (inst->hasRd())
        {
            const auto rd = inst->getRd();
            const std::vector<uint8_t> value = convertToByteVector(rd->dmiRead<uint64_t>());
            dst_regs_.emplace_back(getRegId(rd), value);
        }

        return nullptr;
    }

    ActionGroup* DbgStudioLogger::postExecute_(AtlasState* state)
    {
        // Get final value of destination registers
        AtlasInstPtr inst = state->getCurrentInst();
        sparta_assert(inst != nullptr, "DbgStudio is not valid for logging!");

        if (inst->hasRd())
        {
            sparta_assert(dst_regs_.size() == 1);
            const auto & rd = inst->getRd();
            const std::vector<uint8_t> value = convertToByteVector(rd->dmiRead<uint64_t>());
            dst_regs_[0].setValue(value);
        }

        auto& fout = *dbg_studio_json_fout_;
        fout << std::dec;
        fout << "INST_DUMP::";
        fout << "hart:" << state->getHartId() << ",";
        fout << "pc:0x" << std::hex << pc_ << ",";
        fout << "opc:0x" << std::hex << opcode_ << ",";

        auto replace_commas = [](const std::string& s, char replace) {
            std::string r = s;
            for (auto& c : r) {
                if (c == ',') {
                    c = replace;
                }
            }
            return r;
        };

        fout << "disasm:" << replace_commas(inst->dasmString(), '!') << ",";
        fout << "priv:" << (uint64_t)state->getPrivMode() << ",";

        const auto & symbols = state->getAtlasSystem()->getSymbols();
        if (symbols.find(pc_) != symbols.end()) {
            fout << "symbols:" << symbols.at(pc_) << ",";
        }
        if (src_regs_.size() > 0) {
            fout << "rs1:" << src_regs_[0].reg_id.reg_name << ",";
            fout << "rs1_val:0x" << std::hex << convertFromByteVector<uint64_t>(src_regs_[0].reg_value) << ",";
        }
        if (src_regs_.size() > 1) {
            fout << "rs2:" << src_regs_[1].reg_id.reg_name << ",";
            fout << "rs2_val:0x" << std::hex << convertFromByteVector<uint64_t>(src_regs_[1].reg_value) << ",";
        }
        if (dst_regs_.size() > 0) {
            fout << "rd:" << dst_regs_[0].reg_id.reg_name << ",";
            fout << "rd_prev:0x" << std::hex << convertFromByteVector<uint64_t>(dst_regs_[0].reg_prev_value) << ",";
            fout << "rd_now:0x" << std::hex << convertFromByteVector<uint64_t>(dst_regs_[0].reg_value) << ",";
        }
        if (inst->hasImmediate()) {
            fout << "imm:" << std::dec << inst->getImmediate() << ",";
        }

        fout << std::dec << "\n";
        return nullptr;
    }
} // namespace atlas
