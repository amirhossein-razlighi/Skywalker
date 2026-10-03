#pragma once
// Studio catalogue: roles, disciplines, team templates and loop templates.
//
// This is the single source of truth for role titles, missions and defaults. The editor
// reads it through `studio_overview` (include_catalog) instead of keeping its own copy.

#include <string>
#include <string_view>
#include <vector>

#include "skywalker/core/Json.h"

namespace sky::studio {

struct RoleInfo {
    std::string id;
    std::string title;
    std::string discipline;
    std::string symbol;   // SF Symbol name for the editor
    std::string mission;  // the role's built-in mission (part of the system prompt)
    std::string autonomy; // default autonomy: observe | ask | autonomous
};

const std::vector<RoleInfo>& roles();
const RoleInfo* findRole(std::string_view id);
const std::vector<std::string>& disciplines();
std::vector<std::string> roleIds();

/// Feedback categories, severities, task statuses (enums shared by tools and the editor).
const std::vector<std::string>& feedbackCategories();
const std::vector<std::string>& feedbackStatuses();
const std::vector<std::string>& severities();
const std::vector<std::string>& taskStatuses();
const std::vector<std::string>& priorities();

/// Team templates: arrays of agent specs (as accepted by studio_agent_define).
std::vector<std::string> teamTemplateNames();
Json teamTemplate(std::string_view name);
std::string teamTemplateDescription(std::string_view name);

/// Loop templates: loop specs (as accepted by studio_loop_define).
std::vector<std::string> loopTemplateNames();
Json loopTemplate(std::string_view name);
std::string loopTemplateDescription(std::string_view name);

/// {roles, disciplines, team_templates, loop_templates, feedback_categories, ...}
Json catalogJson();

}  // namespace sky::studio
