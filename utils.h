#pragma once

#include <ranges>
#include <string_view>
#include <utility>
#include <vector>
struct algo {
  static constexpr std::vector<std::string_view> split(std::string_view s, std::string_view div = " ") {
    std::vector<std::string_view> vs;
    int pos = 0;
    do
    {
        auto d = s.find(div, pos);
        if (d == std::string::npos) {
            if (pos != s.size()) {
                vs.push_back(s.substr(pos));
            }
            break;
        }
        vs.push_back(s.substr(pos, d - pos));
        pos = d + 1;
    } while (true);
    return vs;
  }

  static constexpr std::pair<std::string_view, std::string_view> split_one(std::string_view s, std::string_view div = " ") {
    auto pos = s.find(div);
    if (pos == std::string_view::npos) {
      return std::make_pair(s, "");
    } else {
      return std::make_pair(s.substr(0, pos), s.substr(pos+1));
    }
  }
};