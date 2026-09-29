// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "http/route_pattern.hpp"

namespace http {

RoutePattern::RoutePattern(std::string_view pattern,
                           std::vector<std::pair<std::string, std::string>> defaultValues)
    : defaultValues_(std::move(defaultValues))
{
    size_t pos = 0;
    if (!pattern.empty() && pattern.front() == '/')
        pos = 1;

    while (pos < pattern.size()) {
        size_t slash = pattern.find('/', pos);
        std::string_view raw = pattern.substr(
            pos, slash == std::string_view::npos ? std::string_view::npos : slash - pos);
        Segment seg;
        if (raw.size() >= 2 && raw.front() == '{' && raw.back() == '}') {
            seg.isParam = true;
            seg.paramName = std::string(raw.substr(1, raw.size() - 2));
        } else {
            seg.literal = std::string(raw);
        }
        segments_.push_back(std::move(seg));
        if (slash == std::string_view::npos)
            break;

        pos = slash + 1;
    }
}

const std::string* RoutePattern::defaultFor(const std::string& paramName) const
{
    for (auto& kv : defaultValues_) {
        if (kv.first == paramName)
            return &kv.second;
    }
    return nullptr;
}

bool RoutePattern::match(std::string_view path,
                         std::vector<std::pair<std::string, std::string>>& outParams) const
{
    size_t pos = 0;
    if (!path.empty() && path.front() == '/')
        pos = 1;

    size_t segIndex = 0;
    while (true) {
        bool pathExhausted = (pos >= path.size());
        bool segExhausted = (segIndex >= segments_.size());
        if (segExhausted)
            return pathExhausted;

        if (pathExhausted) {
            // The request path ran out before the pattern did -- this is
            // only a match if every remaining segment is a parameter with
            // a registered default value (e.g. the trailing {action} in
            // "/api/{controller}/{action}").
            for (size_t i = segIndex; i < segments_.size(); ++i) {
                const Segment& seg = segments_[i];
                const std::string* defaultValue = seg.isParam ? defaultFor(seg.paramName) : nullptr;
                if (!defaultValue)
                    return false;

                outParams.emplace_back(seg.paramName, *defaultValue);
            }
            return true;
        }

        size_t slash = path.find('/', pos);
        std::string_view part = path.substr(
            pos, slash == std::string_view::npos ? std::string_view::npos : slash - pos);
        const Segment& seg = segments_[segIndex];
        if (seg.isParam) {
            outParams.emplace_back(seg.paramName, std::string(part));
        } else if (seg.literal != part) {
            return false;
        }
        ++segIndex;
        pos = (slash == std::string_view::npos) ? path.size() : slash + 1;
    }
}

}  // namespace http
