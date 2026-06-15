#pragma once

#include "ui/panorama/panorama_dom.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace openstrike
{
class PanoramaLocalization
{
public:
    void load(const std::filesystem::path& resource_root)
    {
        tokens_.clear();
        load_builtin_defaults();

        std::vector<std::filesystem::path> roots;
        if (!resource_root.empty())
        {
            roots.push_back(resource_root);
            roots.push_back(resource_root.parent_path());
            roots.push_back(resource_root.parent_path().parent_path());
        }

        std::unordered_set<std::string> visited;
        const auto try_file = [&](const std::filesystem::path& file) {
            if (file.empty())
            {
                return;
            }
            const std::filesystem::path normalized = file.lexically_normal();
            const std::string key = normalized.generic_string();
            if (!visited.insert(key).second)
            {
                return;
            }
            std::error_code error;
            if (std::filesystem::is_regular_file(normalized, error))
            {
                load_file(normalized);
            }
        };

        for (const std::filesystem::path& root : roots)
        {
            if (root.empty())
            {
                continue;
            }
            try_file(root / "resource/valve_english.txt");
            try_file(root / "resource/csgo_english.txt");
        }
    }

    [[nodiscard]] std::string localize(std::string_view text) const
    {
        const std::string token = normalize_token(text);
        if (token.empty())
        {
            return {};
        }

        const auto it = tokens_.find(lower_ascii(token));
        if (it != tokens_.end())
        {
            return it->second;
        }
        return has_token_marker(text) ? token : std::string(text);
    }

    void localize_tree(PanoramaNode& node) const
    {
        if (!node.text.empty() && has_token_marker(node.text))
        {
            node.text = localize(node.text);
        }
        for (const auto& child : node.children)
        {
            localize_tree(*child);
        }
    }

private:
    class TokenStream
    {
    public:
        explicit TokenStream(std::string_view text) : text_(text) {}

        [[nodiscard]] std::size_t mark() const noexcept { return pos_; }
        void restore(std::size_t mark) noexcept { pos_ = mark; }

        bool next(std::string& out, bool& quoted)
        {
            skip_space_and_comments();
            if (pos_ >= text_.size())
            {
                return false;
            }

            quoted = false;
            out.clear();
            if (text_[pos_] == '"')
            {
                quoted = true;
                ++pos_;
                while (pos_ < text_.size())
                {
                    const char ch = text_[pos_++];
                    if (ch == '"')
                    {
                        return true;
                    }
                    if (ch == '\\' && pos_ < text_.size())
                    {
                        const char escaped = text_[pos_++];
                        switch (escaped)
                        {
                        case 'n': out.push_back('\n'); break;
                        case 't': out.push_back('\t'); break;
                        case 'r': out.push_back('\r'); break;
                        case '"': out.push_back('"'); break;
                        case '\\': out.push_back('\\'); break;
                        default: out.push_back(escaped); break;
                        }
                    }
                    else
                    {
                        out.push_back(ch);
                    }
                }
                return true;
            }

            if (text_[pos_] == '{' || text_[pos_] == '}')
            {
                out.push_back(text_[pos_++]);
                return true;
            }

            while (pos_ < text_.size())
            {
                const char ch = text_[pos_];
                if (std::isspace(static_cast<unsigned char>(ch)) != 0 || ch == '{' || ch == '}' || ch == '"')
                {
                    break;
                }
                if (ch == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '/')
                {
                    break;
                }
                out.push_back(ch);
                ++pos_;
            }
            return !out.empty();
        }

    private:
        void skip_space_and_comments()
        {
            for (;;)
            {
                while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_])) != 0)
                {
                    ++pos_;
                }
                if (pos_ + 1 >= text_.size() || text_[pos_] != '/' || text_[pos_ + 1] != '/')
                {
                    return;
                }
                pos_ += 2;
                while (pos_ < text_.size() && text_[pos_] != '\n' && text_[pos_] != '\r')
                {
                    ++pos_;
                }
            }
        }

        std::string_view text_;
        std::size_t pos_ = 0;
    };

    static bool has_token_marker(std::string_view text)
    {
        return !text.empty() && text.front() == '#';
    }

    static std::string lower_ascii(std::string_view text)
    {
        std::string out(text);
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return out;
    }

    static std::string normalize_token(std::string_view text)
    {
        if (text.empty())
        {
            return {};
        }
        if (text.front() == '#')
        {
            text.remove_prefix(1);
        }

        std::string out;
        out.reserve(text.size());
        bool escaped = false;
        for (char ch : text)
        {
            if (ch == '@' && !escaped)
            {
                break;
            }
            if (escaped)
            {
                out.push_back(ch);
                escaped = false;
            }
            else if (ch == '\\')
            {
                escaped = true;
            }
            else
            {
                out.push_back(ch);
            }
        }
        if (escaped)
        {
            out.push_back('\\');
        }
        return out;
    }

    static void append_utf8(std::string& out, std::uint32_t cp)
    {
        if (cp <= 0x7F)
        {
            out.push_back(static_cast<char>(cp));
        }
        else if (cp <= 0x7FF)
        {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp <= 0xFFFF)
        {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    static std::string decode_text_file(const std::vector<unsigned char>& bytes)
    {
        if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE)
        {
            std::string out;
            for (std::size_t i = 2; i + 1 < bytes.size(); i += 2)
            {
                std::uint32_t cp = static_cast<std::uint32_t>(bytes[i]) |
                                   (static_cast<std::uint32_t>(bytes[i + 1]) << 8);
                if (cp == 0)
                {
                    break;
                }
                append_utf8(out, cp);
            }
            return out;
        }
        if (bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF)
        {
            std::string out;
            for (std::size_t i = 2; i + 1 < bytes.size(); i += 2)
            {
                std::uint32_t cp = (static_cast<std::uint32_t>(bytes[i]) << 8) |
                                   static_cast<std::uint32_t>(bytes[i + 1]);
                if (cp == 0)
                {
                    break;
                }
                append_utf8(out, cp);
            }
            return out;
        }

        std::size_t start = 0;
        if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
        {
            start = 3;
        }
        return std::string(reinterpret_cast<const char*>(bytes.data() + start), bytes.size() - start);
    }

    bool load_file(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            return false;
        }

        const std::vector<unsigned char> bytes(
            (std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>());
        if (bytes.empty())
        {
            return false;
        }

        parse_tokens(decode_text_file(bytes));
        return true;
    }

    void parse_tokens(std::string_view text)
    {
        TokenStream stream(text);
        std::string token;
        bool quoted = false;
        while (stream.next(token, quoted))
        {
            if (lower_ascii(token) != "tokens")
            {
                continue;
            }

            if (!stream.next(token, quoted) || token != "{")
            {
                continue;
            }

            int depth = 1;
            while (depth > 0 && stream.next(token, quoted))
            {
                if (token == "{")
                {
                    ++depth;
                    continue;
                }
                if (token == "}")
                {
                    --depth;
                    continue;
                }
                if (depth != 1)
                {
                    continue;
                }

                std::string value;
                bool value_quoted = false;
                if (!stream.next(value, value_quoted))
                {
                    break;
                }
                if (value == "{" || value == "}")
                {
                    continue;
                }

                const std::size_t mark = stream.mark();
                std::string condition;
                bool condition_quoted = false;
                if (stream.next(condition, condition_quoted))
                {
                    if (condition_quoted || (condition.rfind("[$", 0) != 0 && condition.rfind("[!$", 0) != 0))
                    {
                        stream.restore(mark);
                    }
                }

                tokens_[lower_ascii(normalize_token(token))] = value;
            }
        }
    }

    void add_builtin(std::string_view key, std::string_view value)
    {
        tokens_[lower_ascii(normalize_token(key))] = std::string(value);
    }

    void load_builtin_defaults()
    {
        add_builtin("SFUI_MainMenu_PlayButton", "PLAY");
        add_builtin("play_setting_offline", "Practice With Bots");
        add_builtin("play_setting_training_course", "Training Course");
        add_builtin("play_setting_workshop", "Workshop Maps");
        add_builtin("play_setting_community", "Community Server Browser");
        add_builtin("SFUI_GameModeCompetitive", "Competitive");
        add_builtin("SFUI_GameModeScrimComp2v2", "Wingman");
        add_builtin("SFUI_GameModeCasual", "Casual");
        add_builtin("SFUI_Deathmatch", "Deathmatch");
        add_builtin("SFUI_GameModeSkirmish", "War Games");
        add_builtin("SFUI_GameModeSurvival", "Danger Zone");
        add_builtin("SFUI_BotDifficulty", "Bot Difficulty");
        add_builtin("presets_title", "Presets");
        add_builtin("mg_quick_favorites", "Favorites");
        add_builtin("mg_quick_new", "New");
        add_builtin("mg_quick_premier", "Premier");
        add_builtin("mg_quick_activeduty", "Active Duty");
        add_builtin("mg_quick_hostage", "Hostage");
        add_builtin("mg_quick_select_all", "Select All");
        add_builtin("mg_quick_clear_all", "Clear All");
        add_builtin("mg_quick_save_favorites", "Save Favorites");
        add_builtin("SFUI_Map_lobby_mapveto", "Premier");
        add_builtin("SFUI_Mapgroup_casualsigma", "Defusal Group Sigma");
        add_builtin("SFUI_Mapgroup_casualdelta", "Defusal Group Delta");
        add_builtin("SFUI_Mapgroup_hostage", "Hostage Group");
        add_builtin("SFUI_Mapgroup_allclassic", "All Classic Maps");
        add_builtin("SFUI_Map_de_ancient", "Ancient");
        add_builtin("SFUI_Map_de_anubis", "Anubis");
        add_builtin("SFUI_Map_de_inferno", "Inferno");
        add_builtin("SFUI_Map_de_mirage", "Mirage");
        add_builtin("SFUI_Map_de_nuke", "Nuke");
        add_builtin("SFUI_Map_de_overpass", "Overpass");
        add_builtin("SFUI_Map_de_vertigo", "Vertigo");
        add_builtin("SFUI_Map_de_dust2", "Dust II");
        add_builtin("SFUI_Map_de_train", "Train");
        add_builtin("SFUI_Map_de_cache", "Cache");
        add_builtin("SFUI_Map_cs_office", "Office");
        add_builtin("SFUI_Map_cs_italy", "Italy");
    }

    std::unordered_map<std::string, std::string> tokens_;
};
}
