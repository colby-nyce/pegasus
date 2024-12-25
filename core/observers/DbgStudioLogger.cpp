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

        // {
        //   <key>: {
        //     "hart": 0,
        //     "x0": "0x00000000",
        //     ...
        //     "x31": "0x00000000",
        //     "mstatus": "0x00000000",
        //     ...
        //   }
        // }

        std::ostringstream json;
        json << "{";
        json << "\"" << key << "\": {";
        json << "\"hart\": \"" << state_->getHartId() << "\",";

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
            json << "\"" << to_lower(reg->getName()) << "\": \"" << HEX16(reg->dmiRead<uint64_t>()) << "\"";
            if (++idx < reg_count) {
                json << ",";
            }
        }

        for (auto reg : csr_regs) {
            json << "\"" << to_lower(reg->getName()) << "\": \"" << HEX16(reg->dmiRead<uint64_t>()) << "\"";
            if (++idx < reg_count) {
                json << ",";
            }
        }

        json << "}}";
        *dbg_studio_json_fout_ << json.str() << "\n";
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

        // Spike handler code; Atlas handler code; spike #defines;

        // {
        //   "hart": 0,
        //   "pc": "0x00000000",
        //   "opc": "0xefefefef",
        //   "disasm": "add x7,8",
        //   "rs1": "x7",
        //   "rd":  "x7",
        //   "imm": 8,
        //   "rd_prev": "0x00000000",
        //   "rd_now": "0x00000008",
        //   "symbols": "0xdeabeef",
        //   "priv": "M"
        // }

        std::ostringstream json;
        json << "{";
        json << "\"hart\": \"" << state->getHartId() << "\",";
        json << "\"pc\": \"" << HEX16(pc_) << "\",";
        json << "\"opc\": \"" << HEX8(opcode_) << "\",";
        const auto & symbols = state->getAtlasSystem()->getSymbols();
        if (symbols.find(pc_) != symbols.end()) {
            json << "\"symbols\": \"" << symbols.at(pc_) << "\",";
        }
        if (src_regs_.size() > 0) {
            json << "\"rs1\": \"" << src_regs_[0].reg_id.reg_name << "\",";
            json << "\"rs1_val\": \"" << HEX16(convertFromByteVector<uint64_t>(src_regs_[0].reg_value)) << "\",";
        }
        if (src_regs_.size() > 1) {
            json << "\"rs2\": \"" << src_regs_[1].reg_id.reg_name << "\",";
            json << "\"rs2_val\": \"" << HEX16(convertFromByteVector<uint64_t>(src_regs_[1].reg_value)) << "\",";
        }
        if (dst_regs_.size() > 0) {
            json << "\"rd\": \"" << dst_regs_[0].reg_id.reg_name << "\",";
            json << "\"rd_prev\": \"" << HEX16(convertFromByteVector<uint64_t>(dst_regs_[0].reg_prev_value)) << "\",";
            json << "\"rd_now\": \"" << HEX16(convertFromByteVector<uint64_t>(dst_regs_[0].reg_value)) << "\",";
        }
        if (inst->hasImmediate()) {
            json << "\"imm\": \"" << inst->getImmediate() << "\",";
        }

        // We have to replace the \t with spaces in the disasm string
        // or the JSON will be invalid
        std::string dasm;
        for (auto c : inst->dasmString()) {
            if (c == '\t') {
                dasm += "    ";
            } else {
                dasm += c;
            }
        }

        json << "\"disasm\": \"" << dasm << "\",";
        json << "\"priv\": " << (uint64_t)state->getPrivMode();
        json << "}";

        *dbg_studio_json_fout_ << json.str() << "\n";

        return nullptr;
    }
} // namespace atlas
