#pragma once
#include "hexa_udon/core/models.hpp"
#include "hexa_udon/protocol/result.hpp"
#include "hexa_udon/simulator/action.hpp"
#include <string>
#include <vector>
namespace hexa_udon::protocol {
[[nodiscard]] Result<core::MatchConfig> decode_match_setting(const std::string& text);
[[nodiscard]] Result<core::DailyState> decode_match_state(const std::string& text,const core::MatchConfig& config);
[[nodiscard]] Result<std::int32_t> decode_revision(const std::string& text);
[[nodiscard]] Result<std::string> encode_agent_kinds(const std::vector<core::AgentKind>& kinds);
[[nodiscard]] Result<std::string> encode_actions(const simulator::DayActionPlan& plan);
}
